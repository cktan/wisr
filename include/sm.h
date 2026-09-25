/*
 * sm.h -- wisr's C API: the one entry into the stack, for
 * PostgreSQL to call (DESIGN.md sections 1 and 5).
 *
 * Every call is the sm1, sm2 or sm3 call of the same name with
 * sm_ in front, given a C signature: sm_commit is sm1::commit.
 * The top level adds no notion of its own.
 *
 * Conventions that hold throughout:
 *
 *   - A function returning int returns 0 on success and -1 on
 *     error, as the level below does; the code and message are
 *     then read with sm_errnum() and sm_errmsg(). Where the level
 *     below returns 1 for "no row", so does the call here.
 *   - A call that yields a handle hands it back through its last
 *     parameter, set only on success.
 *   - Names are NUL-terminated strings; data is a pointer and a
 *     length. Both are read during the call and never retained.
 *   - Bytes handed out point into engine memory, are not
 *     NUL-terminated, must not be freed, and stay valid until the
 *     next call on the same session.
 *   - A list is handed out as one malloc'd block, the strings it
 *     points to included, released with one sm_free().
 *   - No C++ exception crosses this boundary. Every call catches
 *     everything: std::bad_alloc becomes SM_ERR_NOMEM, anything
 *     else SM_ERR_EXCEPTION, and when the call has a session, the
 *     session's transaction is aborted before -1 is returned, as
 *     it is after SM_ERR_NOMEM from below -- unless it is
 *     prepared: that is the coordinator's to resolve, and is
 *     left in doubt.
 *
 * Threading is sm1's: an environment may be used from any number
 * of threads, a session and its cursors by one thread at a time.
 */
#ifndef SM_H
#define SM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_error.h"
#include "sm_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An attached home directory: sm1::env_t. */
typedef struct sm_env_t sm_env_t;

/* One database session: sm1::session_t. */
typedef struct sm_session_t sm_session_t;

/* ---- errors: sm1's thread-local error state ---- */

/* The last error raised on the calling thread. */
sm_errnum_t sm_errnum(void);

/* Its message; never NULL, valid until the thread's next error. */
const char* sm_errmsg(void);

/* Its code's name without the SM_ prefix, e.g. "ERR_UPDATED". */
const char* sm_errname(void);

/* 1 if the last error is retryable, else 0. */
int sm_retryable(void);

/* Clear the thread's error back to SM_OK and "no error". */
void sm_reseterr(void);

/* ---- the log hook ---- */

typedef enum sm_loglevel_t {
    SM_LOG_ERROR = 0,   /* something failed and nobody was told */
    SM_LOG_WARN  = 1,   /* something needs an operator's attention */
    SM_LOG_INFO  = 2    /* a notable event, such as recovery */
} sm_loglevel_t;

/*
 * A log hook: level, a NUL-terminated message valid only during
 * the call, and the arg given to sm_set_log().
 *
 * It may run on sm1's own threads, the garbage collector's among
 * them, and inside engine calls: it must return normally, never
 * longjmp, and so must not raise a PostgreSQL ERROR.
 */
typedef void (*sm_log_fn)(void* arg, sm_loglevel_t level, const char* msg);

/* Install fn as the process-wide log hook; NULL removes it. */
int sm_set_log(sm_log_fn fn, void* arg);

/* ---- home directories and databases ---- */

/* Make homedir, which must exist, a wisr home directory. */
int sm_init(const char* homedir);

/* Attach the home directory homedir. */
int sm_attach(const char* homedir, sm_env_t** out);

/* Detach env; force != 0 closes its open sessions first. */
int sm_detach(sm_env_t* env, int force);

/* Create database dbname; opt NULL means the defaults. */
int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt);

/* Drop database dbname. */
int sm_rmdb(sm_env_t* env, const char* dbname);

/* List the databases: *names holds *n strings; sm_free(*names). */
int sm_lsdb(sm_env_t* env, char*** names, size_t* n);

/* Open a session on dbname; opt NULL means the defaults. */
int sm_opendb(sm_env_t* env, const char* dbname, const sm_options_t* opt,
              sm_session_t** out);

/* Close a session; NULL is a no-op. */
int sm_closedb(sm_session_t* session);

/* ---- transactions ---- */

typedef enum sm_xctlevel_t {
    SM_XCT_READ_COMMITTED  = 0,
    SM_XCT_REPEATABLE_READ = 1,
    SM_XCT_SERIALIZABLE    = 2
} sm_xctlevel_t;

/* Begin a transaction at level. */
int sm_begin(sm_session_t* session, sm_xctlevel_t level);

/* Commit the active transaction. */
int sm_commit(sm_session_t* session);

/*
 * Abort the active transaction. A transaction the engine has
 * already aborted, after a retryable error or SM_ERR_NOMEM, is
 * not an error: this returns 0 (DESIGN.md section 5).
 */
int sm_abort(sm_session_t* session);

/* Begin a statement: its writes can be undone on their own. */
int sm_stmt_begin(sm_session_t* session);

/* Set a savepoint; returns its depth (>= 1), or -1. */
int sm_savepoint(sm_session_t* session);

/* Undo every write since the savepoint at depth, which survives. */
int sm_rollback(sm_session_t* session, int depth);

/* Forget the savepoint at depth and those inside it; keep writes. */
int sm_forget(sm_session_t* session, int depth);

/* ---- two-phase commit ---- */

/* Prepare the active transaction under the global id gid. */
int sm_prepare(sm_session_t* session, const char* gid);

/* One unresolved prepared transaction. */
typedef struct sm_prepared_t {
    const char* gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
} sm_prepared_t;

/* List prepared transactions: *out holds *n; sm_free(*out). */
int sm_list_prepared(sm_session_t* session, sm_prepared_t** out,
                     size_t* n);

/* Commit the prepared transaction gid. */
int sm_commit_prepared(sm_session_t* session, const char* gid);

/* Abort the prepared transaction gid. */
int sm_abort_prepared(sm_session_t* session, const char* gid);

/* Release a list handed out by this API; NULL is a no-op. */
void sm_free(void* p);

/*
 * ---- scaffolding: until sm3's catalog ----
 *
 * These exist so a table can be made and opened before sm3 does
 * it by name (DESIGN.md section 1). They will be replaced by
 * opening through sm3's catalog; the row calls will not change.
 */

/* Create a store in the active transaction. */
int sm_mkstore(sm_session_t* session, uint64_t* storeid);

/* Drop a store and its rows in the active transaction. */
int sm_rmstore(sm_session_t* session, uint64_t storeid);

/* List the stores the transaction sees: *n ids; sm_free(*ids). */
int sm_lsstore(sm_session_t* session, uint64_t** ids, size_t* n);

/* Create the sequence name, handing out start, start+increment, ... */
int sm_mkseq(sm_session_t* session, const char* name, uint64_t start,
             uint64_t increment);

/* Drop the sequence name. */
int sm_rmseq(sm_session_t* session, const char* name);

/* ---- types and collations: sm2's ---- */

/* A type: sm2::type_t, opaque. */
typedef struct sm_type_t sm_type_t;

/* A collation for text and varchar keys: sm2::collation_t. */
typedef struct sm_collation_t sm_collation_t;

/* The type with this frozen code (sm2/TYPE.md), or NULL. */
const sm_type_t* sm_type(uint8_t code);

/*
 * Open the collation for locale, a BCP 47 tag or "C"/"POSIX" for
 * byte order. With expected_version non-empty, one whose ICU
 * version differs is refused; NULL means "".
 */
int sm_collation_open(const char* locale, bool deterministic,
                      const char* expected_version, sm_collation_t** out);

/* Close a collation; NULL is a no-op. */
void sm_collation_close(sm_collation_t* collation);

/* The collation's ICU version, for the catalog; "" for byte order. */
const char* sm_collation_version(const sm_collation_t* collation);

/*
 * A datum, or null. A fixed-width datum is its bytes in memory;
 * a variable-width one is a PostgreSQL varlena, header included
 * (sm2/VARLENA.md) -- in rows, index keys and bounds alike.
 */
typedef struct sm_datum_t {
    const void* data;
    size_t      len;
    bool        null;
} sm_datum_t;

/*
 * One attribute of a table's schema: its type and typmod, whether
 * it is dropped, and what a row older than it reads it as --
 * null, or missing[0..missing_len) when has_missing is set.
 */
typedef struct sm_attribute_t {
    const sm_type_t* type;
    int32_t          typmod;
    bool             dropped;
    bool             has_missing;
    const void*      missing;
    size_t           missing_len;
} sm_attribute_t;

/* One key column of an index: its type and how it sorts. */
typedef struct sm_key_column_t {
    const sm_type_t*      type;
    int32_t               typmod;
    const sm_collation_t* collation;   /* text, varchar; NULL: bytes */
    bool                  desc;        /* descending */
    bool                  nulls_first; /* nulls before every value */
} sm_key_column_t;

/*
 * A scan bound: the values of the index's first n columns, and
 * whether an entry equal to them is inside the bound. Bounds are
 * in the index's own order, after direction and null ordering.
 */
typedef struct sm_bound_t {
    const sm_datum_t* prefix;
    size_t            n;
    bool              inclusive;
} sm_bound_t;

/* ---- heap rels: a table's rows by rowno, sm2's ---- */

/* A heap rel. */
typedef struct sm_rel_t sm_rel_t;

/* A cursor over a heap rel's rows, in rowno order. */
typedef struct sm_rel_scan_t sm_rel_scan_t;

/*
 * Scaffolding, until sm3's catalog: open a heap rel over storeid
 * through session, rows under prefix, laid out by the n
 * attributes of schema, each new rowno drawn from the sequence
 * rowno_seq. schema is copied; the caller may free it at once.
 */
int sm_rel_open(sm_session_t* session, uint64_t storeid, uint16_t prefix,
                const sm_attribute_t* schema, size_t n,
                const char* rowno_seq, sm_rel_t** out);

/* Close a heap rel; NULL is a no-op. */
void sm_rel_close(sm_rel_t* rel);

/* The number of attributes in the rel's schema. */
size_t sm_rel_ncols(const sm_rel_t* rel);

/* Insert a row of n datums, one per attribute; *rowno is its id. */
int sm_rel_insert(sm_rel_t* rel, const sm_datum_t* datums, size_t n,
                  uint64_t* rowno);

/* Read the row at rowno: 0 with its tuple, 1 if none, or -1. */
int sm_rel_read(sm_rel_t* rel, uint64_t rowno, const void** tuple,
                size_t* len);

/* Read the newest committed version of the row at rowno. */
int sm_rel_read_latest(sm_rel_t* rel, uint64_t rowno, const void** tuple,
                       size_t* len);

/* Replace the row at rowno; old, if not NULL, gets the old tuple. */
int sm_rel_update(sm_rel_t* rel, uint64_t rowno, const sm_datum_t* datums,
                  size_t n, const void** old, size_t* old_len);

/* Delete the row at rowno; old, if not NULL, gets its tuple. */
int sm_rel_delete(sm_rel_t* rel, uint64_t rowno, const void** old,
                  size_t* old_len);

/* Open a cursor over the rows first..last, both included. */
int sm_rel_scan(sm_rel_t* rel, uint64_t first, uint64_t last,
                sm_rel_scan_t** out);

/* The next row: 0 with it, 1 at the end, or -1. */
int sm_rel_scannxt(sm_rel_scan_t* scan, uint64_t* rowno, const void** tuple,
                   size_t* len);

/* Move the cursor forward to rowno. */
int sm_rel_scan_seek(sm_rel_scan_t* scan, uint64_t rowno);

/* Close a cursor; NULL is a no-op. */
void sm_rel_scanend(sm_rel_scan_t* scan);

/*
 * One attribute of a tuple, by ordinal, into *out: a view into
 * the tuple, or null for a null or dropped attribute.
 */
int sm_rel_decode(sm_rel_t* rel, const void* tuple, size_t len,
                  size_t ordinal, sm_datum_t* out);

/* Every attribute of a tuple into out[0..n); n is sm_rel_ncols. */
int sm_rel_deform(sm_rel_t* rel, const void* tuple, size_t len,
                  sm_datum_t* out, size_t n);

/* ---- index rels: a table's index entries, sm2's ---- */

/* An index rel. */
typedef struct sm_idx_t sm_idx_t;

/* A cursor over an index's entries, in key order. */
typedef struct sm_idx_scan_t sm_idx_scan_t;

/*
 * Scaffolding, until sm3's catalog: open an index over storeid
 * through session, under prefix, keyed by the nkey columns of
 * key, unique or not, with the ninclude INCLUDE attributes of
 * include. Both are copied; the caller may free them at once.
 */
int sm_idx_open(sm_session_t* session, uint64_t storeid, uint16_t prefix,
                const sm_key_column_t* key, size_t nkey, bool unique,
                const sm_attribute_t* include, size_t ninclude,
                sm_idx_t** out);

/* Close an index; NULL is a no-op. */
void sm_idx_close(sm_idx_t* idx);

/*
 * The number of attributes in an entry's value: the rowno
 * (int8), whether the key was cut (bool), then the INCLUDE
 * columns.
 */
size_t sm_idx_ncols(const sm_idx_t* idx);

/*
 * Add the entry for the row at rowno. A unique index refuses a
 * key already present with SM_ERR_DUPKEY.
 */
int sm_idx_insert(sm_idx_t* idx, const sm_datum_t* key, size_t nkey,
                  uint64_t rowno, const sm_datum_t* include,
                  size_t ninclude);

/* Remove the entry sm_idx_insert added for key and rowno. */
int sm_idx_delete(sm_idx_t* idx, const sm_datum_t* key, size_t nkey,
                  uint64_t rowno);

/*
 * Open a cursor over the entries between lo and hi, either NULL
 * for no bound. A bound too long to encode widens the scan, and
 * *widened, if widened is not NULL, says so.
 */
int sm_idx_scan(sm_idx_t* idx, const sm_bound_t* lo, const sm_bound_t* hi,
                bool* widened, sm_idx_scan_t** out);

/* The next entry: 0 with it, 1 at the end, or -1. */
int sm_idx_scannxt(sm_idx_scan_t* scan, uint64_t* rowno, bool* cut,
                   const void** value, size_t* len);

/* Move the cursor forward to the first entry at or after lo. */
int sm_idx_scan_seek(sm_idx_scan_t* scan, const sm_bound_t* lo);

/* Close a cursor; NULL is a no-op. */
void sm_idx_scanend(sm_idx_scan_t* scan);

/* One attribute of an entry's value, as sm_rel_decode. */
int sm_idx_decode(sm_idx_t* idx, const void* value, size_t len,
                  size_t ordinal, sm_datum_t* out);

/* Every attribute of an entry's value; n is sm_idx_ncols. */
int sm_idx_deform(sm_idx_t* idx, const void* value, size_t len,
                  sm_datum_t* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SM_H */
