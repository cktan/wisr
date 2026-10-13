/*
 * sm.h -- wisr's API: the one entry into the stack, for
 * PostgreSQL to call (DESIGN.md sections 1 and 5). It is C in
 * style and compiled as C++: three C++ types may cross it and no
 * others (DESIGN.md section 5). std::string and std::vector
 * carry owned data. std::string_view is bytes that are someone
 * else's, read where they are.
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
 *     memory, is not NUL-terminated and must not be freed. How
 *     long it stays valid depends on what handed it out, so that
 *     a caller can use it in place and copy only what it keeps:
 *       - from a cursor (sm_rel_scannxt, sm_idx_scannxt), until
 *         the next call on that cursor, or until the cursor is
 *         closed, as the end of its transaction closes it. Calls
 *         on the session, on its tables and on its other cursors
 *         leave it alone: a scan's current row outlives the inner
 *         side of a join, a heap fetch and an insert.
 *       - from a point read (sm_tbl_read, sm_tbl_read_latest),
 *         until the next call on the same session.
 *     What sm_tbl_deform and sm_tbl_idx_deform hand out points
 *     into the row given to them, and lasts as long as it does.
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
#include <memory>
#include <optional>
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
 * them, and inside engine calls, where an error has nowhere to
 * go: so it is noexcept. A hook not declared so is not accepted,
 * and one that throws all the same ends the process.
 */
typedef void (*sm_log_fn)(void* arg, sm_loglevel_t level,
                          const char* msg) noexcept;

/* Install fn as the process-wide log hook; NULL removes it. */
int sm_set_log(sm_log_fn fn, void* arg);

/* ---- home directories and databases ---- */

/* Make homedir, which must exist, a wisr home directory. */
int sm_init(const char* homedir);

/* Attach the home directory homedir, recovering every database. */
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
 * The id of database dbname into *out: the number sm_mkdb gave it,
 * never given to another database of this home directory
 * (sm1::dbid). A SQL layer can use it as the database's oid.
 */
int sm_dbid(sm_env_t* env, const char* dbname, uint64_t* out);

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
 * A counter of the caller's own, with a name and no namespace. SQL's
 * CREATE SEQUENCE is sm_cat_create_sequence, a sequence in a
 * namespace with one of these behind it. A table's store and its rowno sequence are made and dropped with
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

/*
 * A value narrower than the datum is sign-extended, as PostgreSQL's
 * Int16GetDatum and Int32GetDatum extend theirs, so a datum from
 * here and one from there are the same bits: -1 is all ones.
 */
#define SM_DATUM_FROM_INT16(v) ((sm_datum_t)(int64_t)(int16_t)(v))
#define SM_DATUM_FROM_INT32(v) ((sm_datum_t)(int64_t)(int32_t)(v))
#define SM_DATUM_FROM_INT64(v) ((sm_datum_t)(uint64_t)(v))
#define SM_DATUM_FROM_BOOL(v)  ((sm_datum_t)((v) ? 1 : 0))
#define SM_DATUM_FROM_PTR(p)   ((sm_datum_t)(const void*)(p))

/*
 * The datum of type whose bytes are bytes, as a statistic keeps a
 * value (sm_stat_t): a by-value one built from them, a by-reference
 * one pointing into bytes, which must outlive it.
 */
sm_datum_t sm_datum_of_bytes(const sm_type_t* type, const std::string& bytes);

#define SM_DATUM_TO_INT16(d)   ((int16_t)(uint16_t)(d))
#define SM_DATUM_TO_INT32(d)   ((int32_t)(uint32_t)(d))
#define SM_DATUM_TO_INT64(d)   ((int64_t)(uint64_t)(d))
#define SM_DATUM_TO_BOOL(d)    (((uint8_t)(d)) != 0)
#define SM_DATUM_TO_PTR(d)     ((const void*)(uintptr_t)(d))

static inline sm_datum_t sm_datum_from_float(float f) {
    int32_t bits;
    memcpy(&bits, &f, sizeof(float));
    return SM_DATUM_FROM_INT32(bits);
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

/*
 * The next row: 0 with it, 1 at the end, or -1. The tuple is valid
 * until the next call on this cursor.
 */
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

/*
 * The next entry: 0 with it, 1 at the end, or -1. The value is
 * valid until the next call on this cursor.
 */
int sm_idx_scannxt(sm_idx_scan_t* scan, uint64_t* rowno, bool* cut,
                   const void** value, size_t* len);

/* Move the cursor forward to the first entry at or after lo. */
int sm_idx_scan_seek(sm_idx_scan_t* scan, const sm_bound_t* lo);

/*
 * The key columns of the entry sm_idx_scannxt last returned, into
 * values and isnull, n of each, one per key column, as a bound
 * gives them: 0; 1 if the key does not give them back -- it was
 * cut, or a text column is under a non-deterministic collation --
 * so the row must; or -1. An entry the cursor returns is one the
 * transaction sees, as its row is. A by-reference value is valid
 * until the next call on the cursor.
 */
int sm_idx_scan_key(sm_idx_scan_t* scan, sm_datum_t* values, bool* isnull,
                    size_t n);

/* Close a cursor; NULL is a no-op. */
void sm_idx_scanend(sm_idx_scan_t* scan);

/* ---- the catalog: sm3's ---- */

/* The built-in namespaces' oids (sm3::nsp_pg_catalog, nsp_public). */
#define SM_NSP_PG_CATALOG 11
#define SM_NSP_PUBLIC     2200

/*
 * The namespace of a session's temporary tables: none
 * (sm3::nsp_temp). The lookups by name take it to mean the
 * session's own temporary tables and their indexes, and a temporary
 * table's definition has it for its namespace.
 */
#define SM_NSP_TEMP 0

/* A column, as a table is created with it. */
struct sm_column_spec_t {
    const char* name;
    uint8_t     type_code;  /* sm2/TYPE.md section 3 */
    int32_t     typmod;
    bool        not_null;
    const char* default_expr = nullptr; /* the host's default, opaque */
    char        identity = 0;     /* 'a', 'd' or 0 (sm3::column_spec_t) */
    uint64_t    identity_seq = 0; /* an identity column's sequence */
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
    std::optional<std::string> default_expr; /* the host's, opaque */
    char        identity;     /* 'a', 'd' or 0 */
    uint64_t    identity_seq; /* an identity column's sequence */
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
 * The name of the namespace oid, as the session's transaction sees
 * it, into name: 0, or 1 if there is none.
 */
int sm_cat_namespace_name(sm_session_t* session, uint64_t oid,
                          std::string& name);

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
 * Create the sequence name in nspoid, SQL's CREATE SEQUENCE, in the
 * session's transaction, handing out start, start + increment, ...;
 * its oid is set in oid. Its name is a relation's: one a table or
 * an index of nspoid has fails with SM_ERR_DUPKEY. An increment of
 * 0 fails with SM_ERR_INVALID.
 */
int sm_cat_create_sequence(sm_session_t* session, uint64_t nspoid,
                           const char* name, uint64_t start,
                           uint64_t increment, uint64_t* oid);

/*
 * Drop the sequence oid, in the session's transaction. One a
 * column owns fails with SM_ERR_INUSE.
 */
int sm_cat_drop_sequence(sm_session_t* session, uint64_t oid);

/*
 * Make the column attnum (0-based) of the table table own the
 * sequence oid, SQL's ALTER SEQUENCE ... OWNED BY, in the session's
 * transaction: dropping the table or the column drops it. Table 0
 * is OWNED BY NONE. A table in another namespace, or a column it
 * does not have, fails with SM_ERR_INVALID; an identity column's
 * sequence, which is its for good, with SM_ERR_INUSE.
 */
int sm_cat_own_sequence(sm_session_t* session, uint64_t oid, uint64_t table,
                        int32_t attnum);

/*
 * The oid of the sequence named name in nspoid, as the session's
 * transaction sees it: 0, or 1 if there is none. It reads the
 * catalog in that transaction, which must be open.
 */
int sm_cat_sequence_oid(sm_session_t* session, uint64_t nspoid,
                        const char* name, uint64_t* oid);

/*
 * The namespace and name of the sequence oid, as the session's
 * transaction, which must be open, sees it: 0, or 1 if there is no
 * such sequence, an oid of a table or an index among them.
 */
int sm_cat_sequence_name(sm_session_t* session, uint64_t oid,
                         uint64_t* nspoid, std::string& name);

/*
 * The names of the sequences in nspoid, sorted, as the session's
 * transaction, which must be open, sees them, into names.
 */
int sm_cat_list_sequences(sm_session_t* session, uint64_t nspoid,
                          std::vector<std::string>& names);

/*
 * Hand out the next value of the sequence oid. It is not
 * transactional: a rollback does not give it back. A sequence not
 * there, as the session's transaction sees it, fails with
 * SM_ERR_NOTFOUND; one past UINT64_MAX with SM_ERR_INVALID.
 */
int sm_cat_nextval(sm_session_t* session, uint64_t oid, uint64_t* out);

/*
 * Wait until the build of the table's index has ended. An index
 * made on a table that has rows is built in the background, and
 * may not be scanned until it is valid; this is for a caller that
 * wants it at once. Returns 0 once it is valid. If the build
 * failed it fails with the build's own error, code and message,
 * as the index keeps them; SM_ERR_INTRANS inside a transaction;
 * SM_ERR_NOTFOUND if there is no such table or index.
 *
 * An interrupt to the calling thread, a bit in its wait mask
 * (sm_sys.h), ends the wait with SM_ERR_INTERRUPTED; the bit stays
 * pending, for the caller to take.
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
 * It is noexcept: it runs on the index build's workers and
 * inside a write, where what it throws has nowhere to go. It
 * fails by returning -1; an evaluator not declared noexcept is
 * not accepted, and one that throws all the same ends the
 * process. It must give the same answer for the same row every
 * time. It may be called from any thread,
 * the index build's workers among them, and must not call this
 * API on the session that is writing.
 */
typedef int (*sm_index_eval_fn)(void* ctx, uint64_t table, uint64_t index,
                                const std::string& eval,
                                const sm_datum_t* values, const bool* isnull,
                                size_t n, bool* belongs,
                                std::vector<std::string>& out,
                                bool* out_isnull) noexcept;

/*
 * Give env its evaluator: once, before any database is opened,
 * since opening one may at once resume the build of such an
 * index. Without one, no partial or expression index can be
 * created, and a write to a table that has one fails.
 */
int sm_set_index_evaluator(sm_env_t* env, sm_index_eval_fn fn, void* ctx);

/*
 * TOAST: a row's long values stored out of line
 * (sm2/SM2_DESIGN.md section 4.2).
 *
 * Off, the default, every value is in its row's tuple, and a row
 * is at most 65535 bytes. A host turns it on to say it reads TOAST
 * pointers: a tuple may then hold, where a variable-width value
 * was, a pointer to it, 22 bytes whose first is 0x01, which
 * sm_datum_is_toast tells and sm_tbl_fetch reads. sm moves a row's
 * longest values out once its tuple passes 2000 bytes.
 *
 * A write is given values. One of the row's own pointers, given
 * back to sm_tbl_update for a column it leaves alone, is kept
 * without a write; sm_tbl_insert given a pointer stores a copy of
 * its value. An index is made of values always: its key and its
 * INCLUDE columns are fetched, so an entry never holds a pointer,
 * and is held to 65535 bytes itself. sm_tbl_analyze counts a value
 * out of line as too wide to sort, and fetches none.
 */

/*
 * Turn TOAST on or off for env: once, before any database is
 * opened. Not to be turned off again over tables written with it
 * on.
 */
int sm_set_toast(sm_env_t* env, int on);

/* Whether val, a non-null variable-width value, is a TOAST pointer. */
static inline bool sm_datum_is_toast(sm_datum_t val) {
    return *(const char*)val == 1;
}

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
 * A BULK LOAD (BULKLOAD.md section 5, section 13): many rows into
 * one table, for COPY, a restore, CREATE TABLE AS.
 *
 * sm_tbl_load_begin, then sm_tbl_load_add for each row, then
 * sm_tbl_load_end; the table is one opened to write. The rows are
 * the transaction's own, as inserted ones are: no one else sees
 * them before it commits, and an abort leaves none.
 *
 * Into an EMPTY table -- a new one, a truncated one, any whose
 * store holds no version of a row -- the rows are written to
 * sorted files that the store takes whole when the load ends,
 * past its log and its compactions. For that the table's lock is
 * taken exclusive, to the end of the transaction: no one else
 * reads or writes the table meanwhile. The lock is taken only if
 * it is free when the load begins; the load never waits for it,
 * and with another transaction using the table, even to read,
 * the rows are inserted instead. A duplicate key is then
 * found when the load ends, not at its row. The first row that
 * must be stored out of line (sm_set_toast), or is too long,
 * ends the files: what was handed over goes into the table, and
 * the rest are inserted.
 *
 * Into a table that has rows, every row is inserted, as
 * sm_tbl_insert would, and no lock is taken but the table's own.
 *
 * While a load into an empty table is under way the transaction
 * cannot commit, and the table's other writes fail with
 * SM_ERR_INVALID. After one the transaction cannot be prepared,
 * and a savepoint set before it cannot be rolled back to. After a
 * failed call the transaction is to be aborted; a load not ended
 * is ended by its transaction's end, and its handle freed by
 * sm_tbl_close.
 */
int sm_tbl_load_begin(sm_tbl_t* tbl);

/* Hand over one row of n values and null flags; *rowno is its id. */
int sm_tbl_load_add(sm_tbl_t* tbl, const sm_datum_t* values,
                    const bool* isnull, size_t n, uint64_t* rowno);

/* End the load: every row handed over is in the table. */
int sm_tbl_load_end(sm_tbl_t* tbl);

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
 * As sm_tbl_lock, but never waiting: 0 with the row locked; 1,
 * with nothing taken, if another transaction holds the row in a
 * mode that conflicts or has written it and not yet ended; or -1.
 * SELECT ... FOR UPDATE NOWAIT fails on 1, and SKIP LOCKED passes
 * over the row.
 */
int sm_tbl_trylock(sm_tbl_t* tbl, uint64_t rowno, sm_lockmode_t mode);

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
 * Read the value the TOAST pointer val names, one sm_tbl_deform or
 * sm_tbl_decode gave of this table, into *out: its bytes, with no
 * varlena header. It reads as the session's transaction sees the
 * table, so a pointer is to be fetched in the statement that read
 * it. The old tuple sm_tbl_update and sm_tbl_delete hand back
 * names values that write erased: the statement that made the
 * write still sees them (sm_stmt_begin), and no later one does.
 * Fails with SM_ERR_INVALID if val is not a pointer of the table,
 * and SM_ERR_NOTFOUND if the value is gone.
 */
int sm_tbl_fetch(sm_tbl_t* tbl, sm_datum_t val, std::string* out);

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
 * ---- statistics, for the host's planner (sm3/SM3_DESIGN.md §6) ----
 *
 * What the table's LSM looks like now, read live and stored
 * nowhere: the heap's bytes and the index's, in sorted files
 * (disk) and in memtables (mem), apart, never summed; N, the last
 * rowno handed out, 0 if none; and the sorted runs a lookup in the
 * table's store may visit.
 */
int sm_tbl_size(sm_tbl_t* tbl, uint64_t* disk, uint64_t* mem);
int sm_tbl_idx_size(sm_tbl_t* tbl, uint64_t index, uint64_t* disk,
                    uint64_t* mem);
int sm_tbl_rowno_limit(sm_tbl_t* tbl, uint64_t* n);
int sm_tbl_probes(sm_tbl_t* tbl, uint64_t* n);

/*
 * The statistics of one column, or of one expression of an index:
 * PostgreSQL's six. A value is its bytes as a datum of the column's
 * or expression's type, a varlena for a variable-width one.
 */
struct sm_stat_t {
    bool                     has_index;   /* an expression's, not a column's */
    uint64_t                 index;
    int32_t                  position;    /* the column's 0-based place, or
                                             the expression's among the
                                             index's keys */
    float                    null_frac;
    int32_t                  width;
    float                    distinct;    /* a count, or minus a fraction
                                             of the rows */
    bool                     has_correlation;
    float                    correlation;
    std::vector<std::string> mcv;         /* most common values */
    std::vector<float>       mcv_freq;
    std::vector<std::string> bounds;      /* histogram bounds */
};

/* An index's sizes and entries as the analyze found them. */
struct sm_stats_index_t {
    uint64_t oid;
    uint64_t disk;
    uint64_t mem;
    double   entries;
    float    belongs;   /* the fraction of rows in a partial index */
};

/* A table's statistics, as its last analyze recorded them. */
struct sm_stats_t {
    int32_t                       target;
    int32_t                       run_length;
    uint64_t                      rows_sampled;
    uint64_t                      rowno_limit;
    double                        live_rows;
    uint64_t                      heap_disk;
    uint64_t                      heap_mem;
    int32_t                       avg_row_bytes;
    std::vector<sm_stats_index_t> indexes;
    std::vector<sm_stat_t>        stats;
};

/*
 * Analyze the table, in its transaction: sample up to 300 x target
 * of its rows, reading no more than ten times that in rownos
 * whatever its size, and record each column's statistics, each
 * partial or expression index's from the evaluator, and the sizes.
 * They commit and roll back with the transaction. A table whose
 * heap holds no bytes and whose sample reads no row, as a new or
 * truncated one, is not analyzed.
 */
int sm_tbl_analyze(sm_tbl_t* tbl, int32_t target);

/*
 * The table oid's statistics, as the session sees them: 0 with them
 * in out, or 1 if it was never analyzed. They are shared, read-only,
 * by every caller in the process: held from memory, loaded once on
 * first use, built into this form once, and taking no lock. A
 * holder keeps the ones it was given, whatever is analyzed since.
 */
int sm_tbl_stats(sm_session_t* session, uint64_t oid,
                 std::shared_ptr<const sm_stats_t>& out);

/*
 * Create the table name in nspoid with these columns: its store,
 * its rowno sequence and its definition, seen by the session's
 * transaction at once and by others at commit. An identity column
 * owns its sequence from then on (sm3::create_table).
 */
int sm_cat_create_table(sm_session_t* session, uint64_t nspoid,
                        const char* name,
                        const std::vector<sm_column_spec_t>& columns,
                        uint64_t* oid);

/*
 * Create the temporary table name with these columns
 * (TEMPTABLE.md): the session's alone, seen by no other and gone
 * when the session closes. It is in no namespace, and is found by
 * name with SM_NSP_TEMP. Its rows are never logged and take no
 * locks, but follow the session's transactions as any table's do.
 * The other calls take its oid as they take any table's, but
 * sm_tbl_analyze fails with SM_ERR_INVALID, and so does preparing
 * a transaction that made, changed or wrote one. A name another of
 * the session's temporary tables or their indexes has fails with
 * SM_ERR_DUPKEY.
 */
int sm_cat_create_temp_table(sm_session_t* session, const char* name,
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
