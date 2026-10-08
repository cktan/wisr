/*
 * sm.h -- wisr's API: the one entry into the stack, for
 * PostgreSQL to call (DESIGN.md sections 1 and 5). It is C in
 * style and compiled as C++: std::string and std::vector may
 * cross it, for owned data only, and no other C++ type does
 * (DESIGN.md section 5).
 *
 * Every call is the sm1, sm2 or sm3 call of the same name with
 * sm_ in front, given a C signature: sm_commit is sm3::commit,
 * which is sm1::commit that also publishes the transaction's DDL.
 * The top level adds no notion of its own: it keeps each
 * database's sm3 catalog, and each session's sm3 session.
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
 *   - A row or an index entry handed out points into engine
 *     memory, is not NUL-terminated, must not be freed, and stays
 *     valid until the next call on the same session.
 *   - Anything else handed out is the caller's own: a list is a
 *     std::vector and bytes are a std::string, filled by the call.
 *     Nothing is freed by hand.
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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sm_error.h"
#include "sm_lock.h"
#include "sm_options.h"

/*
 * An attached home directory: sm1::env_t, and the sm3 catalog of
 * each database opened, shared by its sessions.
 */
typedef struct sm_env_t sm_env_t;

/*
 * One database session: an sm3::session_t over an sm1::session_t,
 * so the transaction calls keep its DDL's overlay in step.
 */
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

/*
 * Record code and msg as the calling thread's error and return -1,
 * so a callback of the host's can fail as a call of the stack's
 * does: return sm_seterr(...).
 */
int sm_seterr(sm_errnum_t code, const char* msg);

/* ---- the log hook ---- */

enum sm_loglevel_t {
    SM_LOG_ERROR = 0,   /* something failed and nobody was told */
    SM_LOG_WARN  = 1,   /* something needs an operator's attention */
    SM_LOG_INFO  = 2    /* a notable event, such as recovery */
};

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

/*
 * Create database dbname and its catalog (sm3::cat_init); opt
 * NULL means the defaults.
 */
int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt);

/* Drop database dbname, and its catalog in memory. */
int sm_rmdb(sm_env_t* env, const char* dbname);

/* List the databases into names, which it replaces. */
int sm_lsdb(sm_env_t* env, std::vector<std::string>& names);

/*
 * Open a session on dbname; opt NULL means the defaults. The
 * first session on a database loads its catalog (sm3::cat_open).
 */
int sm_opendb(sm_env_t* env, const char* dbname, const sm_options_t* opt,
              sm_session_t** out);

/* Close a session; NULL is a no-op. */
int sm_closedb(sm_session_t* session);

/* ---- transactions ---- */

enum sm_xctlevel_t {
    SM_XCT_READ_COMMITTED  = 0,
    SM_XCT_REPEATABLE_READ = 1,
    SM_XCT_SERIALIZABLE    = 2
};

/*
 * Begin a transaction at level. A nonzero read_only declares it
 * READ ONLY: a write fails with SM_ERR_READONLY, and at
 * SERIALIZABLE it costs SSI less (sm1/SM1_DESIGN.md §7.4).
 */
int sm_begin(sm_session_t* session, sm_xctlevel_t level, int read_only);

/* Commit the active transaction. */
int sm_commit(sm_session_t* session);

/*
 * Abort the active transaction. A transaction the engine has
 * already aborted, after a retryable error or SM_ERR_NOMEM, is
 * not an error: this returns 0 (DESIGN.md section 5).
 */
int sm_abort(sm_session_t* session);

/*
 * Begin a statement, a SQL command. Its reads see the
 * transaction's own writes made before it began, not those it
 * makes itself, as PostgreSQL's command id does; under READ
 * COMMITTED they also read at one snapshot, taken by the first
 * read. An update or delete of a row the statement has already
 * changed fails with SM_ERR_SELFMODIFIED and writes nothing: not
 * an abort, and the caller skips the row, or raises an error as
 * INSERT ... ON CONFLICT does. A statement runs until the next
 * sm_stmt_begin or the end of the transaction. A transaction that
 * never calls it sees all its own writes, call by call.
 */
int sm_stmt_begin(sm_session_t* session);

/* A statement put aside by sm_stmt_begin_inner: opaque. */
typedef struct sm_stmt_t sm_stmt_t;

/*
 * Begin a statement inside the running one, as a trigger's or a
 * function's commands run inside the statement that fired them.
 * The inner statement sees the outer's writes so far. The outer
 * is saved in *outer, which sm_stmt_end takes back and frees.
 */
int sm_stmt_begin_inner(sm_session_t* session, sm_stmt_t** outer);

/*
 * End a statement begun by sm_stmt_begin_inner: the outer one
 * resumes with its own snapshot, and again does not see its own
 * writes nor the inner one's. outer is freed either way.
 */
int sm_stmt_end(sm_session_t* session, sm_stmt_t* outer);

/*
 * FOR QA ONLY (rel's scripts); not how NOWAIT or SKIP LOCKED is
 * asked for, which is to be a flag on the calls that can wait.
 * Set whether the session waits for locks. On, a call that would
 * wait for another transaction's lock fails with SM_ERR_BLOCKED
 * instead, leaving the transaction active; the caller polls by
 * repeating the call. Off is the default.
 */
int sm_set_qa_nowait(sm_session_t* session, bool on);

/* Set a savepoint; returns its depth (>= 1), or -1. */
int sm_savepoint(sm_session_t* session);

/* Undo every write since the savepoint at depth, which survives. */
int sm_rollback(sm_session_t* session, int depth);

/* Forget the savepoint at depth and those inside it; keep writes. */
int sm_forget(sm_session_t* session, int depth);

/* ---- two-phase commit ---- */

/*
 * Prepare the active transaction under the global id gid. One that
 * made DDL is refused with SM_ERR_INVALID (sm3::prepare).
 */
int sm_prepare(sm_session_t* session, const char* gid);

/* One unresolved prepared transaction. */
struct sm_prepared_t {
    std::string gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
};

/* List prepared transactions into out, which it replaces. */
int sm_list_prepared(sm_session_t* session, std::vector<sm_prepared_t>& out);

/* Commit the prepared transaction gid. */
int sm_commit_prepared(sm_session_t* session, const char* gid);

/* Abort the prepared transaction gid. */
int sm_abort_prepared(sm_session_t* session, const char* gid);

/*
 * ---- sequences, below the catalog ----
 *
 * A sequence of the caller's own, as SQL's CREATE SEQUENCE. A
 * table's store and its rowno sequence are made and dropped with
 * the table, by sm_cat_create_table and sm_cat_drop_table, and are
 * not reachable here. Names beginning "sm3." are sm3's own, a
 * table's rowno sequence among them, and each call below refuses
 * one with SM_ERR_INVALID.
 */

/* Create the sequence name, handing out start, start+increment, ... */
int sm_mkseq(sm_session_t* session, const char* name, uint64_t start,
             uint64_t increment);

/* Drop the sequence name. */
int sm_rmseq(sm_session_t* session, const char* name);

/* Hand out the sequence's next value. */
int sm_nextseq(sm_session_t* session, const char* name, uint64_t* out);

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
 * A datum, as PostgreSQL's 8-byte Datum (postgres.h).
 * For a by-value type, the value itself fits in the datum (int2,
 * int4, int8, bool, float4, float8, date, time, timestamp,
 * timestamptz). For a by-reference type, it is a pointer to the
 * bytes or varlena.
 */
typedef uintptr_t sm_datum_t;

#define SM_DATUM_FROM_INT16(v) ((sm_datum_t)(uint16_t)(v))
#define SM_DATUM_FROM_INT32(v) ((sm_datum_t)(uint32_t)(v))
#define SM_DATUM_FROM_INT64(v) ((sm_datum_t)(uint64_t)(v))
#define SM_DATUM_FROM_BOOL(v)  ((sm_datum_t)((v) ? 1 : 0))
#define SM_DATUM_FROM_PTR(p)   ((sm_datum_t)(const void*)(p))

#define SM_DATUM_TO_INT16(d)   ((int16_t)(uint16_t)(d))
#define SM_DATUM_TO_INT32(d)   ((int32_t)(uint32_t)(d))
#define SM_DATUM_TO_INT64(d)   ((int64_t)(uint64_t)(d))
#define SM_DATUM_TO_BOOL(d)    (((uint8_t)(d)) != 0)
#define SM_DATUM_TO_PTR(d)     ((const void*)(uintptr_t)(d))

static inline sm_datum_t sm_datum_from_float(float f) {
    sm_datum_t d = 0;
    memcpy(&d, &f, sizeof(float));
    return d;
}
static inline sm_datum_t sm_datum_from_double(double f) {
    sm_datum_t d = 0;
    memcpy(&d, &f, sizeof(double));
    return d;
}
static inline float sm_datum_to_float(sm_datum_t d) {
    float f;
    memcpy(&f, &d, sizeof(float));
    return f;
}
static inline double sm_datum_to_double(sm_datum_t d) {
    double f;
    memcpy(&f, &d, sizeof(double));
    return f;
}

/*
 * Parse PostgreSQL's canonical text for a value of type under
 * typmod, as the type's typinput does (sm2/TYPE.md §3). A by-value
 * result is *val itself, and buf is left empty. A by-reference
 * one -- fixed-width bytes, or a varlena for a variable-width
 * type -- is put in buf, and *val points at it, so buf must
 * outlive *val and not be changed meanwhile. Only the canonical
 * text is accepted; any other is SM_ERR_INVALID.
 */
int sm_type_in(const sm_type_t* type, int32_t typmod, const char* text,
               size_t len, sm_datum_t* val, std::string& buf);

/*
 * Print a non-null value of type under typmod as PostgreSQL's
 * canonical text, as the type's typoutput does, into text.
 */
int sm_type_out(const sm_type_t* type, int32_t typmod, sm_datum_t val,
                std::string& text);

/*
 * One attribute of a table's schema: its type and typmod, whether
 * it is dropped, and what a row older than it reads it as --
 * null, or missing when has_missing is set.
 */
struct sm_attribute_t {
    const sm_type_t* type;
    int32_t          typmod;
    bool             dropped;
    bool             has_missing;
    std::string      missing;
};

/* One key column of an index: its type and how it sorts. */
struct sm_key_column_t {
    const sm_type_t*      type;
    int32_t               typmod;
    const sm_collation_t* collation;   /* text, varchar; NULL: bytes */
    bool                  desc;        /* descending */
    bool                  nulls_first; /* nulls before every value */
};

/*
 * A scan bound: the values of the index's first n columns, with
 * a null flag each, and whether an entry equal to them is inside
 * the bound. Bounds are in the index's own order, after direction
 * and null ordering.
 */
struct sm_bound_t {
    const sm_datum_t* prefix;
    const bool*           isnull;
    size_t                n;
    bool                  inclusive;
};

/*
 * ---- cursors over a table's rows and index entries, sm2's ----
 *
 * A row and an index entry are read and written through an open
 * table (sm_tbl_*, below), never through a heap or an index rel of
 * their own, so no caller can change a row and leave its index
 * entries behind (sm3/SM3_DESIGN.md section 3).
 */

/* A cursor over a table's rows, in rowno order; sm_tbl_scan opens one. */
typedef struct sm_rel_scan_t sm_rel_scan_t;

/* The next row: 0 with it, 1 at the end, or -1. */
int sm_rel_scannxt(sm_rel_scan_t* scan, uint64_t* rowno, const void** tuple,
                   size_t* len);

/* Move the cursor forward to rowno. */
int sm_rel_scan_seek(sm_rel_scan_t* scan, uint64_t rowno);

/* Close a cursor; NULL is a no-op. */
void sm_rel_scanend(sm_rel_scan_t* scan);

/* Encode values and isnull, one per attribute of schema, into tuple. */
int sm_tup_encode(const std::vector<sm_attribute_t>& schema,
                  const sm_datum_t* values, const bool* isnull,
                  std::string& tuple);

/*
 * A cursor over an index's entries, in key order; sm_tbl_idx_scan
 * opens one.
 */
typedef struct sm_idx_scan_t sm_idx_scan_t;

/* The next entry: 0 with it, 1 at the end, or -1. */
int sm_idx_scannxt(sm_idx_scan_t* scan, uint64_t* rowno, bool* cut,
                   const void** value, size_t* len);

/* Move the cursor forward to the first entry at or after lo. */
int sm_idx_scan_seek(sm_idx_scan_t* scan, const sm_bound_t* lo);

/* Close a cursor; NULL is a no-op. */
void sm_idx_scanend(sm_idx_scan_t* scan);

/* ---- the catalog: sm3's ---- */

/* The built-in namespaces' oids (sm3::nsp_pg_catalog, nsp_public). */
#define SM_NSP_PG_CATALOG 1
#define SM_NSP_PUBLIC     2

/* A column, as a table is created with it. */
struct sm_column_spec_t {
    const char* name;
    uint8_t     type_code;  /* sm2/TYPE.md section 3 */
    int32_t     typmod;
    bool        not_null;
};

/*
 * A key column, as an index is created with it; no collation yet.
 * With attnum -1 it is an expression, which has no column behind
 * it and so gives its own type code and typmod.
 */
struct sm_index_key_spec_t {
    int32_t attnum;         /* the column's 0-based place; -1: expression */
    bool    desc;
    bool    nulls_first;
    uint8_t type_code;      /* an expression's (sm2/TYPE.md section 3) */
    int32_t typmod;         /* an expression's */
};

/* A column of a table definition. */
struct sm_column_def_t {
    std::string name;
    uint8_t     type_code;
    int32_t     typmod;
    bool        not_null;
    bool        dropped;
};

/* Where an index's build stands (sm3::index_state_t). */
enum sm_index_state_t {
    SM_INDEX_PENDING  = 0,  /* maintained, not yet filled; not scanned */
    SM_INDEX_VALID    = 1,
    SM_INDEX_FAILED   = 2,  /* its build failed; it can only be dropped */
    SM_INDEX_BUILDING = 3   /* pending, and being filled now */
};

/*
 * An index of a table definition: its key and INCLUDE columns, by
 * 0-based place in the table and as sm2 lays them out; where its
 * build stands; and, if the build failed, the error it failed
 * with and that error's message.
 */
struct sm_index_def_t {
    uint64_t                     oid;
    std::string                  name;
    bool                         unique;
    std::vector<int32_t>         key_attnums;
    std::vector<sm_key_column_t> key;
    std::vector<int32_t>         include_attnums;
    std::vector<sm_attribute_t>  include;
    sm_index_state_t             state;
    int                          error;   /* SM_OK unless failed */
    std::string                  reason;
    /*
     * A partial or an expression index: eval is its predicate and
     * its expressions as the host gave them, empty for a plain
     * index; reads the columns they read. A key column that is an
     * expression has -1 in key_attnums.
     */
    std::string                  eval;
    std::vector<int32_t>         reads;
};

/*
 * A table definition: its columns, with schema their attributes
 * as sm2 lays a row out by, and its indexes.
 */
struct sm_table_def_t {
    uint64_t                     oid;
    uint64_t                     nspoid;
    std::string                  name;
    std::vector<sm_column_def_t> columns;
    std::vector<sm_attribute_t>  schema;
    std::vector<sm_index_def_t>  indexes;
};

/*
 * Create the namespace name, SQL's schema, in the session's
 * transaction, which sees it at once and others at commit; its oid
 * is set in oid. A name another namespace has fails with
 * SM_ERR_DUPKEY.
 */
int sm_cat_create_namespace(sm_session_t* session, const char* name,
                            uint64_t* oid);

/*
 * Drop the namespace oid. One that still holds a table or a
 * collation fails with SM_ERR_INUSE, and SM_NSP_PG_CATALOG and
 * SM_NSP_PUBLIC with SM_ERR_INVALID.
 */
int sm_cat_drop_namespace(sm_session_t* session, uint64_t oid);

/*
 * The oid of the namespace named name, as the session's transaction
 * sees it: 0, or 1 if there is none.
 */
int sm_cat_namespace_oid(sm_session_t* session, const char* name,
                         uint64_t* oid);

/*
 * The names of the namespaces, sorted, as the session's transaction
 * sees them, into names, which it replaces.
 */
int sm_cat_list_namespaces(sm_session_t* session,
                           std::vector<std::string>& names);

/*
 * The oid of the table named name in nspoid, as the session's
 * transaction sees it: 0, or 1 if there is none. It takes no
 * lock: sm_tbl_open then finds a table dropped since.
 */
int sm_cat_table_oid(sm_session_t* session, uint64_t nspoid,
                     const char* name, uint64_t* oid);

/*
 * The names of the tables in nspoid, sorted, as the session's
 * transaction sees them, into names, which it replaces.
 */
int sm_cat_list_tables(sm_session_t* session, uint64_t nspoid,
                       std::vector<std::string>& names);

/* The index named name in nspoid, and its table: 0, or 1 if none. */
int sm_cat_index_oid(sm_session_t* session, uint64_t nspoid,
                     const char* name, uint64_t* table, uint64_t* index);

/*
 * Wait until the build of the table's index has ended. An index
 * made on a table that has rows is built in the background, and
 * may not be scanned until it is valid; this is for a caller that
 * wants it at once. Returns 0 once it is valid. If the build
 * failed it fails with the build's own error, code and message,
 * as the index keeps them; SM_ERR_INTRANS inside a transaction;
 * SM_ERR_NOTFOUND if there is no such table or index.
 */
int sm_cat_index_wait(sm_session_t* session, uint64_t table, uint64_t index);

/*
 * The host's evaluator of partial and expression indexes
 * (sm3/SM3_DESIGN.md section 3). The stack evaluates nothing
 * itself: where it must know whether a row belongs in such an
 * index, and what its expression key columns are, it calls this.
 *
 * It is given the table's oid and the index's, the index's eval
 * as it was given to sm_cat_create_index, and a row image as n
 * values and null flags, one per column; a by-reference value
 * points at memory good only for the call. It sets *belongs, and
 * for each key column that is an expression, in key order, puts
 * the value's bytes in out -- a by-value type's bytes, a
 * fixed-width type's, or a varlena for a variable-width one -- or
 * sets its flag in out_isnull; both come sized to the count of
 * expression columns. It returns 0, or -1 with the error set.
 *
 * It must not throw nor longjmp out. It must give the same answer
 * for the same row every time. It may be called from any thread,
 * the index build's workers among them, and must not call this
 * API on the session that is writing.
 */
typedef int (*sm_index_eval_fn)(void* ctx, uint64_t table, uint64_t index,
                                const std::string& eval,
                                const sm_datum_t* values, const bool* isnull,
                                size_t n, bool* belongs,
                                std::vector<std::string>& out,
                                bool* out_isnull);

/*
 * Give env its evaluator: once, before any database is opened,
 * since opening one may at once resume the build of such an
 * index. Without one, no partial or expression index can be
 * created, and a write to a table that has one fails.
 */
int sm_set_index_evaluator(sm_env_t* env, sm_index_eval_fn fn, void* ctx);

/*
 * ---- tables: a row and its index entries, sm3's ----
 *
 * An open table: its definition under the table-oid lock, its heap
 * and its indexes. A write through it keeps every index of the
 * table in step with the heap (sm3/SM3_DESIGN.md section 3).
 */
typedef struct sm_tbl_t sm_tbl_t;

/*
 * Open the table oid in the session's transaction, taking its
 * table-oid lock in mode, held to the end of the transaction:
 * SM_LOCKMODE_IS to read its rows, SM_LOCKMODE_IX
 * to write them. Its definition cannot change until then. A table
 * dropped while the caller waited is SM_ERR_NOTFOUND. The handle
 * is good for this transaction only; close it before or after the
 * transaction ends.
 */
int sm_tbl_open(sm_session_t* session, uint64_t oid, sm_lockmode_t mode,
                sm_tbl_t** out);

/* Close a table; NULL is a no-op. */
void sm_tbl_close(sm_tbl_t* tbl);

/* The table's definition, valid until sm_tbl_close. */
const sm_table_def_t* sm_tbl_def(const sm_tbl_t* tbl);

/*
 * The writes. Each is the heap write, then a write or two to every
 * index it changes, and sets no savepoint: one that fails after
 * its first step leaves the earlier steps done, and the caller
 * rolls back to a savepoint of its own, or aborts, before going
 * on. SM_ERR_UPDATED needs no rollback; it comes from the heap
 * write, the first step. A table not opened to write fails with
 * SM_ERR_INVALID, and a row with a NULL in a NOT NULL column with
 * SM_ERR_NOTNULL; both before anything is written.
 */

/* Insert a row of n values and null flags; *rowno is its id. */
int sm_tbl_insert(sm_tbl_t* tbl, const sm_datum_t* values,
                  const bool* isnull, size_t n, uint64_t* rowno);

/*
 * Replace the row at rowno. old, if not NULL, gets the tuple
 * replaced -- at read committed the newest, not the one the caller
 * read.
 */
int sm_tbl_update(sm_tbl_t* tbl, uint64_t rowno, const sm_datum_t* values,
                  const bool* isnull, size_t n, std::string* old = nullptr);

/* Delete the row at rowno; old as sm_tbl_update's. */
int sm_tbl_delete(sm_tbl_t* tbl, uint64_t rowno, std::string* old = nullptr);

/*
 * Read the row at rowno: 0 with its tuple, 1 if none, or -1. The
 * tuple is valid only until the next call on this session.
 */
int sm_tbl_read(sm_tbl_t* tbl, uint64_t rowno, const void** tuple,
                size_t* len);

/*
 * Read the newest committed version of the row at rowno, which a
 * read committed writer does after SM_ERR_UPDATED, before it
 * writes again.
 */
int sm_tbl_read_latest(sm_tbl_t* tbl, uint64_t rowno, const void** tuple,
                       size_t* len);

/*
 * Lock the row at rowno in mode without writing it, to the end of
 * the transaction: SM_LOCKMODE_SH, as a foreign key check locks the
 * parent row and SELECT ... FOR SHARE a row it read, which other
 * readers of the key may share and a writer waits for; or
 * SM_LOCKMODE_EX, as SELECT ... FOR UPDATE, which needs the
 * table opened to write. The row need not exist. It waits as a
 * write does.
 */
int sm_tbl_lock(sm_tbl_t* tbl, uint64_t rowno, sm_lockmode_t mode);

/*
 * Empty the table, as SQL's TRUNCATE: its rows and every index's
 * entries go, in the table's transaction, at the same cost however
 * many there are. The table must be opened SM_LOCKMODE_EX,
 * SM_ERR_INVALID if not. Columns, indexes and the row numbering
 * are kept, so new rows go on from the last rowno handed out. The
 * handle stays open on the emptied table, and sm_tbl_def gives its
 * new definition. A transaction that began before the truncate
 * committed and first reads the table after finds it empty, as in
 * PostgreSQL.
 */
int sm_tbl_truncate(sm_tbl_t* tbl);

/* Open a cursor over the rows first..last, both included. */
int sm_tbl_scan(sm_tbl_t* tbl, uint64_t first, uint64_t last,
                sm_rel_scan_t** out);

/*
 * Every attribute of a row's tuple into values and isnull, n of
 * each, one per column. A by-reference value points into the tuple,
 * so it shares the tuple's lifetime.
 */
int sm_tbl_deform(sm_tbl_t* tbl, const void* tuple, size_t len,
                  sm_datum_t* values, bool* isnull, size_t n);

/*
 * One attribute of a row's tuple, by ordinal, into *val and
 * *isnull; a null or dropped attribute sets *isnull.
 */
int sm_tbl_decode(sm_tbl_t* tbl, const void* tuple, size_t len,
                  size_t ordinal, sm_datum_t* val, bool* isnull);

/*
 * Open a cursor over the entries of the table's index, by oid,
 * between lo and hi, either NULL for no bound; SM_ERR_NOTFOUND if
 * the table has no such index. A bound too long to encode widens
 * the scan, and *widened, if widened is not NULL, says so.
 */
int sm_tbl_idx_scan(sm_tbl_t* tbl, uint64_t index, const sm_bound_t* lo,
                    const sm_bound_t* hi, bool* widened,
                    sm_idx_scan_t** out);

/*
 * Whether the session may scan the table's index now. Not while it
 * is pending, never once it has failed, and not in a statement or
 * transaction whose snapshot is older than the index's build.
 * sm_tbl_idx_scan fails with SM_ERR_INVALID on one that may not be.
 */
bool sm_tbl_index_usable(const sm_tbl_t* tbl, uint64_t index);

/*
 * The number of attributes in an entry's value in that index: the
 * rowno, the cut flag, then the INCLUDE columns; 0 if there is no
 * such index.
 */
size_t sm_tbl_idx_ncols(const sm_tbl_t* tbl, uint64_t index);

/* Every attribute of an entry's value in that index, as sm_tbl_deform. */
int sm_tbl_idx_deform(sm_tbl_t* tbl, uint64_t index, const void* value,
                      size_t len, sm_datum_t* values, bool* isnull,
                      size_t n);

/*
 * Create the table name in nspoid with these columns: its store,
 * its rowno sequence and its definition, seen by the session's
 * transaction at once and by others at commit.
 */
int sm_cat_create_table(sm_session_t* session, uint64_t nspoid,
                        const char* name,
                        const std::vector<sm_column_spec_t>& columns,
                        uint64_t* oid);

/* Drop the table oid: its store, rows, indexes and sequence. */
int sm_cat_drop_table(sm_session_t* session, uint64_t oid);

/*
 * Create the index name on the table, keyed by keys, with the
 * INCLUDE columns includes by 0-based place. On a table this
 * transaction made it is filled and valid at once. On any other
 * it is recorded as pending, and filled in the background once
 * the transaction commits: sm_cat_index_wait waits for that.
 *
 * A partial or an expression index gives eval, its predicate and
 * its expressions serialized as the host likes -- the stack keeps
 * the bytes and hands them to the evaluator -- and reads, the
 * columns they read, so an update that changes none of them can
 * leave the index alone. A plain index gives both empty.
 */
int sm_cat_create_index(sm_session_t* session, uint64_t table,
                        const char* name,
                        const std::vector<sm_index_key_spec_t>& keys,
                        const std::vector<int32_t>& includes, bool unique,
                        const std::string& eval,
                        const std::vector<int32_t>& reads, uint64_t* oid);

/* Drop the index oid of the table, and its entries. */
int sm_cat_drop_index(sm_session_t* session, uint64_t table, uint64_t index);

#endif /* SM_H */
