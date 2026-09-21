/*
 * sm.h -- the wisr storage manager's public C API.
 *
 * API.md is the interface contract; DESIGN.md is the design
 * behind it.
 *
 * Conventions that hold throughout:
 *
 *   - A function returning int returns 0 on success and -1 on
 *     error. Exactly two, sm_read() and sm_scannxt(), also
 *     return 1 to mean "no row", which is not an error.
 *   - A function returning a pointer returns NULL on error.
 *   - On any error return, sm_errno() and sm_errmsg() describe
 *     it. A successful call leaves both untouched, so neither
 *     can be used to test for success.
 *   - Byte strings passed in are read during the call and never
 *     retained; the caller may free them immediately after.
 *   - Byte strings handed out point into engine-owned memory,
 *     are not NUL-terminated, and must not be freed. See
 *     sm_read() and sm_scannxt() for their validity windows.
 *
 * Threading: an sm_env_t may be used from any number of threads
 * at once. An sm_session_t and its cursors may be used by one
 * thread at a time; concurrent use of one session from two
 * threads is a caller error and is not diagnosed. Distinct
 * sessions may be used concurrently without external
 * synchronization.
 *
 * Processes: at most one process, and one environment in it,
 * may attach a given home directory at a time. The library
 * does not enforce this; serializing processes is the
 * caller's job. Violating it is a caller error: a second
 * attach usually fails inside RocksDB, but nothing guarantees
 * it, and sm_attach(), sm_mkdb(), and sm_rmdb() assume they
 * are alone in the home directory.
 */
#ifndef SM_H
#define SM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Limits. A key or value beyond its bound, or a name outside
 * 1..bound bytes, fails with SM_ERR_INVALID.
 *
 * SM_MAX_DBNAME matches PostgreSQL's NAMEDATALEN - 1, so every
 * name PostgreSQL accepts for a database is one wisr accepts,
 * and SM_MAX_GID matches its GIDSIZE. An over-long name is an
 * error here, never a truncation: PostgreSQL emits a NOTICE
 * and truncates, and a C API has no channel for a notice.
 */
#define SM_MAX_KEYLEN  4096
#define SM_MAX_DATALEN (64 * 1024 * 1024)
#define SM_MAX_DBNAME  63
#define SM_MAX_SEQNAME 63
#define SM_MAX_GID     200
#define SM_MAX_SAVEPOINTS 256

/*
 * Error codes. Values are explicit and stable: new codes are
 * appended, existing values never change.
 */
typedef enum {
    SM_OK                = 0,

    /* caller/programming errors */
    SM_ERR_INVALID       = 1,   /* bad argument */
    SM_ERR_NOTRANS       = 2,   /* op requires an active transaction */
    SM_ERR_INTRANS       = 3,   /* op forbidden inside a transaction */
    SM_ERR_PREPARED      = 4,   /* op forbidden on a prepared transaction */
    SM_ERR_NOSAVEPOINT   = 5,   /* rollback with no savepoint set */

    /* data errors */
    SM_ERR_NOTFOUND      = 6,   /* relation or key does not exist */
    SM_ERR_DUPKEY        = 7,   /* insert onto a live key */
    SM_ERR_EXISTS        = 8,   /* home dir, database, or relation exists */
    SM_ERR_INUSE         = 9,   /* database or sessions still open */

    /* concurrency errors -- RETRYABLE */
    SM_ERR_CONFLICT      = 10,  /* write-write conflict */
    SM_ERR_SERIALIZATION = 11,  /* SSI validation failure */
    SM_ERR_DEADLOCK      = 12,  /* chosen as deadlock victim */

    /* environment errors */
    SM_ERR_CORRUPT       = 13,
    SM_ERR_IO            = 14,
    SM_ERR_NOMEM         = 15,
    SM_ERR_DISKFULL      = 16,
    SM_ERR_PERMISSION    = 17,
    SM_ERR_VERSION       = 18,  /* home dir format not supported */

    /* Not an abort: the transaction stays active (PLAN.md step 4). */
    SM_ERR_UPDATED       = 19   /* newer commit; sm_read_latest, then retry */
} sm_errno_t;

/*
 * The last error raised on the calling thread. Meaningful
 * only immediately after a call that returned an error
 * indication: a successful call leaves it untouched, so it
 * cannot be used to test for success.
 */
sm_errno_t sm_errno(void);

/*
 * The message for that error: NUL-terminated, owned by the
 * library, never NULL, and valid until the next failing sm_*
 * call on this thread.
 */
const char* sm_errmsg(void);

/*
 * 1 if the last error on this thread is retryable, else 0.
 *
 * SM_ERR_CONFLICT, SM_ERR_SERIALIZATION, and SM_ERR_DEADLOCK
 * mean the transaction cannot proceed but the same work may
 * succeed on a fresh try. On any of them the engine has
 * ALREADY aborted the transaction: do not call sm_abort(), do
 * not issue further data operations, just sm_begin() again and
 * redo the work.
 *
 * Test with this rather than against the three codes: a
 * retryable code added later is appended to the enum like any
 * other, so they are not guaranteed to stay contiguous.
 *
 * Every other error leaves the transaction active and
 * unchanged. The two exceptions are sm_commit(), after which
 * the transaction is never still active, and sm_prepare() on
 * SM_ERR_IO.
 */
int sm_retryable(void);

/*
 * Log levels for the hook below.
 */
typedef enum {
    SM_LOG_ERROR = 0,   /* something failed and nobody was told */
    SM_LOG_WARN  = 1,   /* something needs an operator's attention */
    SM_LOG_INFO  = 2    /* a notable event, such as recovery */
} sm_loglevel_t;

/*
 * A log hook: receives level and a NUL-terminated message
 * valid only for the duration of the call, plus the arg given
 * to sm_set_log().
 */
typedef void (*sm_log_fn)(void* arg, sm_loglevel_t level, const char* msg);

/*
 * Install fn as the process-wide log hook, replacing any
 * previous one; NULL removes it. With no hook, wisr formats
 * and reports nothing.
 *
 * The library never writes a log of its own. The hook hears
 * only what no caller would otherwise learn: failures in
 * wisr's background GC, a database entering its failed state,
 * versions the compaction filter could not judge, a snapshot
 * holding back garbage collection for over a minute, and what
 * sm_attach() and recovery clean up or restore. Errors a call
 * returns are not logged; its caller already has them.
 *
 * fn may be called from any thread, wisr's own included, one
 * call at a time. It must not call back into wisr: calls are
 * made with a library lock held.
 */
void sm_set_log(sm_log_fn fn, void* arg);

/*
 * An environment: one process's handle on a home directory,
 * through which every database and session call goes.
 */
typedef struct sm_env_t sm_env_t;

/* A session: one caller's connection to one database. */
typedef struct sm_session_t sm_session_t;

/* A scan cursor over a range of one relation. */
typedef struct sm_scan_t sm_scan_t;

/*
 * A counted, immutable list of names returned by a listing
 * call. Elements are NUL-terminated and owned by the list;
 * release the whole list with sm_strlist_free().
 *
 * A list holds no engine state -- no lock, no snapshot, no
 * session reference -- so failing to free one leaks memory
 * and nothing else. This is unlike sm_scan_t, where a
 * missed close pins the GC watermark.
 *
 * Names only. Keys and values may contain NUL and must not
 * travel in one.
 */
typedef struct sm_strlist_t sm_strlist_t;

/* The number of names in the list. */
size_t sm_strlist_count(const sm_strlist_t* sl);

/*
 * Name i, NUL-terminated and owned by the list, or NULL if
 * i is past the end. Valid until sm_strlist_free().
 */
const char* sm_strlist_at(const sm_strlist_t* sl, size_t i);

/* Release a list. Passing NULL is a no-op. */
void sm_strlist_free(sm_strlist_t* sl);

/*
 * Isolation levels.
 *
 * READ UNCOMMITTED is deliberately absent: under MVCC there is
 * no state in which a caller could observe an uncommitted
 * version, so the level has no meaning here.
 */
typedef enum {
    SM_XCT_READ_COMMITTED  = 0,
    SM_XCT_REPEATABLE_READ = 1,
    SM_XCT_SERIALIZABLE    = 2
} sm_xctlevel_t;

/*
 * Database options, taken by sm_mkdb() and by the sm_opendb()
 * that first opens a database. NULL means all defaults.
 *
 * durability_sync = 0 trades a bounded window of committed-
 * transaction loss on power failure for throughput. The
 * database stays crash-CONSISTENT; it is no longer crash-
 * DURABLE. It does not weaken isolation.
 *
 * group_commit_us is accepted but currently unused: concurrent
 * commits already share one synced WAL write through RocksDB's
 * write group. commit_cache_entries bounds the in-memory map
 * from transaction id to commit timestamp that spares readers
 * a status-table lookup; it must be at least 1.
 */
typedef struct sm_options_t sm_options_t;
struct sm_options_t {
    int         durability_sync;       /* default 1 */
    int         group_commit_us;       /* default 200, unused */
    int         deadlock_ms;           /* default 50 */
    int         ssi_max_entries;       /* default 1<<20, unused */
    long long   ts_lease;              /* default 1000000 */
    const char* rocksdb_options;       /* option string, NULL = defaults */
    int         commit_cache_entries;  /* default 65536 */
};

/*
 * Fill opt with the defaults. Call this before setting any
 * field, so that fields added in later versions keep theirs.
 */
void sm_options_init(sm_options_t* opt);

/*
 * Make homedir -- which must already exist -- a wisr home
 * directory: create global/ and base/, the root db inside
 * global/, and the format version inside that.
 *
 * Call it once in a home directory's life, not once per
 * process. It takes no lock and opens nothing. Returns 0, or
 * -1 with SM_ERR_EXISTS if homedir is already a wisr home.
 */
int sm_init(const char* homedir);

/*
 * Create an environment on a home directory sm_init() has
 * already prepared. Opens no database. The caller must ensure
 * no other environment, in this process or another, has the
 * home directory attached (see "Processes" above).
 *
 * Creates nothing: a homedir that was never initialized is an
 * error, not a new home directory. Returns NULL on error.
 */
sm_env_t* sm_attach(const char* homedir);

/*
 * Release an environment, leaving the home directory intact.
 * This undoes sm_attach(), not sm_init(); afterwards another
 * process may attach the home directory.
 *
 * With force == 0, returns -1 with SM_ERR_INUSE if any
 * session opened through env is still open, and env stays
 * valid. With force != 0, those sessions are closed first,
 * as sm_closedb() would: cursors are closed and active
 * transactions are aborted, but a prepared transaction is
 * left in doubt for its coordinator. Their sm_session_t
 * handles are then invalid.
 *
 * force does not preempt other threads: the caller must
 * know that none is inside an sm_* call on those sessions,
 * as for any other concurrent use of one. NULL is a no-op
 * returning 0.
 */
int sm_detach(sm_env_t* env, int force);

/*
 * Create the database dbname. It is not opened; call sm_opendb()
 * afterwards.
 *
 * A dbname is a name, never a path: it is a key in the
 * environment's root db, which maps it to the number naming
 * the directory the database actually lives in. So
 * sm_mkdb(env, "a/../b") creates a database called "a/../b"
 * and reaches nothing outside homedir. Names are compared
 * bytewise and never folded, so "Foo" and "foo" are two
 * databases.
 *
 * Errors: SM_ERR_EXISTS, SM_ERR_PERMISSION, SM_ERR_DISKFULL,
 * SM_ERR_IO, SM_ERR_INVALID.
 */
int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt);

/*
 * Delete the database dbname and all of its data. THIS IS
 * DESTRUCTIVE AND CANNOT BE UNDONE.
 *
 * Fails with SM_ERR_INUSE if any session in this environment
 * is open on it. No other process can be using it, provided
 * the caller keeps to one attached process (see "Processes").
 *
 * Errors: SM_ERR_INUSE, SM_ERR_NOTFOUND, SM_ERR_PERMISSION,
 * SM_ERR_IO, SM_ERR_INVALID.
 */
int sm_rmdb(sm_env_t* env, const char* dbname);

/*
 * List the databases in homedir, in bytewise order. On success
 * *out is a list to release with sm_strlist_free().
 *
 * A database directory is named by number, so the names live
 * only in the root db: reading homedir will not tell a caller
 * what is there.
 *
 * Errors: SM_ERR_NOMEM, SM_ERR_IO.
 */
int sm_lsdb(sm_env_t* env, sm_strlist_t** out);

/*
 * Open the database dbname and start a session on it. The
 * database must already exist; this never creates one.
 *
 * The first sm_opendb() on a database in this process opens it
 * and its opt takes effect; later sessions join the open
 * instance and their opt is ignored. Recovery -- restoring
 * prepared transactions and their locks -- happens during that
 * first open.
 *
 * A new session has no active transaction. It belongs to env,
 * which must not be released while the session is open.
 *
 * Returns NULL on error: SM_ERR_NOTFOUND, SM_ERR_CORRUPT,
 * SM_ERR_PERMISSION, SM_ERR_NOMEM, SM_ERR_IO, SM_ERR_INVALID.
 */
sm_session_t* sm_opendb(sm_env_t* env, const char* dbname,
                        const sm_options_t* opt);

/*
 * Create a sequence: a named counter that hands out start,
 * start + increment, start + 2 * increment, and so on. The
 * name is a NUL-terminated string of at most SM_MAX_SEQNAME
 * bytes, and increment must not be 0.
 *
 * Creating and dropping a sequence is transactional, like
 * sm_mkrel() -- but the values it hands out are not; see
 * sm_nextseq(). A name dropped by sm_rmseq() may be used
 * again: the new sequence starts at its own start, keeping
 * nothing from the old one.
 *
 * Errors: SM_ERR_EXISTS, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable -- concurrently created), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int sm_mkseq(sm_session_t* session, const char* name,
             uint64_t start, uint64_t increment);

/*
 * Drop a sequence. Transactional, as sm_mkseq().
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable), SM_ERR_INVALID, SM_ERR_PREPARED.
 */
int sm_rmseq(sm_session_t* session, const char* name);

/*
 * Hand out the sequence's next value in *out.
 *
 * The sequence is persistent and strictly increasing: a value
 * is handed out once, never again, including across restarts.
 *
 * It is not transactional, like a PostgreSQL sequence: it
 * needs no active transaction, never blocks, and an abort does
 * not give a value back. Values follow the increment but are
 * not dense: besides aborts, a crash skips the rest of a block
 * reserved in advance, leaving a gap. Distinct sessions may
 * call it concurrently.
 *
 * Inside a transaction it reads the sequence at that
 * transaction's snapshot, so one created by the transaction
 * itself can be used at once. A sequence whose last value has
 * been handed out -- the next one would exceed UINT64_MAX --
 * fails with SM_ERR_INVALID.
 *
 * Errors: SM_ERR_NOTFOUND (no such sequence), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO (the database has
 * stopped, or persisting the next block failed),
 * SM_ERR_DISKFULL.
 */
int sm_nextseq(sm_session_t* session, const char* name,
               uint64_t* out);

/*
 * Close a session. Open cursors are closed, and an active
 * transaction is forcibly aborted -- but a PREPARED one is
 * not: it stays in doubt and awaits its coordinator.
 *
 * session is invalid after this call whether it succeeded or
 * not. Passing NULL is a no-op returning 0. Closing the last
 * session on a database closes the database.
 */
int sm_closedb(sm_session_t* session);

/*
 * Begin a transaction at level, taking its snapshot if the
 * level calls for one. Exactly one transaction may be active
 * on a session; there is no nesting.
 *
 * Errors: SM_ERR_INTRANS, SM_ERR_INVALID (unknown level).
 */
int sm_begin(sm_session_t* session, sm_xctlevel_t level);

/*
 * Commit the active transaction, making its writes durable and
 * visible. Releases all locks, discards the savepoint, and
 * closes the session's open cursors. For a prepared
 * transaction this is the coordinator's commit decision and
 * always succeeds, validation having happened at prepare.
 *
 * On SM_ERR_SERIALIZATION the transaction is aborted and its
 * effects discarded; retry.
 *
 * On SM_ERR_IO the outcome is UNKNOWN: the commit record may
 * or may not have reached the log. DO NOT RETRY BLINDLY -- the
 * work may already be committed. The database enters a failed
 * state, where further writes, commits, and prepares fail with
 * SM_ERR_IO while reads on existing snapshots continue. After
 * a reopen the transaction is committed or not according to
 * what reached the log.
 *
 * Once called on an active transaction, the transaction is no
 * longer active whatever the result.
 *
 * Errors: SM_ERR_SERIALIZATION (retryable), SM_ERR_NOTRANS,
 * SM_ERR_IO (outcome unknown).
 */
int sm_commit(sm_session_t* session);

/*
 * Abort the active transaction, discarding its writes.
 * Releases all locks, discards the savepoint, and closes the
 * session's open cursors. Aborting a PREPARED transaction is
 * legal and is the coordinator's abort decision.
 *
 * Must not be called after a retryable error, which already
 * aborted the transaction.
 *
 * Errors: SM_ERR_NOTRANS.
 */
int sm_abort(sm_session_t* session);

/*
 * The mode of a row lock taken by sm_lock().
 *
 *   SM_LOCK_SHARED     others may hold it shared, none may
 *                      write the row
 *   SM_LOCK_EXCLUSIVE  no one else may hold it in any mode
 */
typedef enum {
  SM_LOCK_SHARED = 0,
  SM_LOCK_EXCLUSIVE = 1
} sm_lockmode_t;

/*
 * Lock a row without writing it: SQL's SELECT ... FOR SHARE
 * and FOR UPDATE. The key need not exist.
 *
 * The call BLOCKS while another transaction holds the lock in
 * a conflicting mode, exactly as a write does, and it can fail
 * with SM_ERR_DEADLOCK for the same reason. A transaction that
 * already holds the row shared and asks for it exclusive --
 * including by writing the row -- upgrades, waiting for the
 * other shared holders to finish; two transactions upgrading
 * the same row deadlock, and the younger one is the victim.
 *
 * Once the lock is held the call runs the same check a write
 * does: if another transaction committed a version of this key
 * after this transaction's snapshot, it fails with
 * SM_ERR_CONFLICT. So a successful sm_lock means what the
 * caller reads next under that lock is current, which is what
 * makes "lock, then read, then act" safe.
 *
 * The lock is held to the end of the transaction, or until an
 * sm_rollback() past the point it was taken. An upgrade is
 * never undone by a rollback: the row stays locked in the
 * stronger mode until the transaction ends.
 *
 * A transaction holding shared locks may be prepared; the
 * locks survive in the prepared transaction and are restored
 * after a crash, so nothing may write those rows until a
 * coordinator resolves it.
 *
 * Errors: SM_ERR_CONFLICT / SM_ERR_DEADLOCK (retryable),
 * SM_ERR_NOTFOUND (relation), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_INVALID.
 */
int sm_lock(sm_session_t* session, uint64_t relid,
            const char* key, size_t klen, sm_lockmode_t mode);

/*
 * Read the newest committed version of a key, ignoring the
 * snapshot, under a row lock this transaction already holds.
 * Returns as sm_read() does: 0 found, 1 no such key. At read
 * committed only. Under an exclusive lock it also lets the next
 * write of the key through, after SM_ERR_UPDATED (PLAN.md step
 * 4).
 *
 * Errors: SM_ERR_INVALID (not read committed, or no lock held),
 * SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_NOTFOUND (relation).
 */
int sm_read_latest(sm_session_t* session, uint64_t relid,
                   const char* key, size_t klen,
                   const char** data, size_t* datalen);

/*
 * Begin a statement: read committed then reads every row of
 * this statement at ONE snapshot, instead of taking a fresh
 * one per call.
 *
 * Without it, each sm_read() and each sm_scan() under
 * SM_XCT_READ_COMMITTED takes its own snapshot, so a caller
 * whose statement spans several calls -- an index lookup, say,
 * being one scan plus N reads -- sees N+1 committed states and
 * can return a mixture that never existed.
 *
 * The snapshot is taken by the first read after this call, not
 * by this call, so a statement may lock or wait first and
 * still see everything committed by the time it reads. The
 * statement runs until the next sm_stmt_begin(), which starts
 * another, or until the transaction ends.
 *
 * Once called, the transaction reads this way for the rest of
 * its life; there is no going back to a snapshot per call.
 * Under SM_XCT_REPEATABLE_READ and SM_XCT_SERIALIZABLE the
 * call is accepted and does nothing, the whole transaction
 * already reading at one snapshot.
 *
 * Writes are unaffected: they still check for conflicts
 * against the snapshot taken by sm_begin(), so an
 * SM_ERR_CONFLICT can still arrive on a row this statement
 * has just read.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID.
 */
int sm_stmt_begin(sm_session_t* session);

/*
 * Set a savepoint at the current point in the transaction and
 * return its DEPTH: 1 for the first, 2 for one set inside it,
 * and so on. Savepoints nest; a transaction may hold up to
 * SM_MAX_SAVEPOINTS of them, and they are all discarded at
 * commit or abort.
 *
 * A depth is reused once the mark holding it is gone, so a
 * depth from a savepoint already rolled past or forgotten may
 * name a DIFFERENT mark later. Callers that keep savepoints by
 * name must drop a name when its mark goes.
 *
 * Returns the depth (>= 1), or -1 on error.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID
 * (the stack is full).
 */
int sm_savepoint(sm_session_t* session);

/*
 * Undo every write made since the savepoint at depth. That
 * savepoint survives, so the same mark may be rolled back to
 * more than once, and the savepoints set inside it are gone.
 * Locks taken since the mark are released, locks held from
 * before it are kept, and the transaction stays active.
 *
 * Scan cursors opened AFTER the savepoint are closed, and
 * their sm_scan_t handles must not be used again; one opened
 * before it stays open and keeps its position. A cursor's view
 * is fixed when it is created, so a later one would otherwise
 * go on returning rows this call just undid.
 *
 * Requires a savepoint at that depth. To roll a transaction
 * back to its start, call sm_savepoint() immediately after
 * sm_begin() and roll back to the depth it returned.
 *
 * Rollback does NOT undo read tracking: under SERIALIZABLE,
 * reads performed and then rolled back still count towards
 * serialization checking. Conservative, never unsafe.
 *
 * Errors: SM_ERR_NOSAVEPOINT (no savepoint at that depth),
 * SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int sm_rollback(sm_session_t* session, int depth);

/*
 * Forget the savepoint at depth and every savepoint set inside
 * it, so none of them can be rolled back to again. SQL's
 * RELEASE SAVEPOINT.
 *
 * The writes made since the mark stay, and so do the locks
 * taken since it: they now belong to the enclosing savepoint,
 * or to the transaction if there is none.
 *
 * Errors: SM_ERR_NOSAVEPOINT (no savepoint at that depth),
 * SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int sm_forget(sm_session_t* session, int depth);

/*
 * Prepare the active transaction for commit under the global
 * transaction id gid, a NUL-terminated string of at most
 * SM_MAX_GID bytes, unique among this database's unresolved
 * prepared transactions.
 *
 * Runs serialization validation, then durably records the
 * prepared state. The transaction is then PREPARED: data
 * operations, savepoints, and rollback are rejected with
 * SM_ERR_PREPARED, and only sm_commit() and sm_abort() are
 * accepted. It survives a crash, is restored at the next
 * sm_opendb() still holding its row locks, and cannot be told
 * "no" by the engine.
 *
 * From here on the transaction belongs to the database, not
 * the session: any session may resolve it by gid, and
 * sm_closedb() leaves it in doubt. While it is unresolved, the
 * preparing session's sm_begin() fails with SM_ERR_INTRANS.
 * Once any session has resolved it, the preparing session has
 * no transaction: its sm_commit() and sm_abort() fail with
 * SM_ERR_NOTRANS, and sm_begin() succeeds.
 *
 * An abandoned prepared transaction blocks writers on its keys
 * and prevents reclamation indefinitely. That is inherent to
 * 2PC and is exposed rather than hidden;
 * sm_abort_prepared() is the operator escape hatch.
 *
 * On SM_ERR_IO the outcome is UNKNOWN: the prepared record may
 * or may not have reached the log. The transaction is no
 * longer active, and the database enters the same failed state
 * as after SM_ERR_IO from sm_commit(). After a reopen the
 * transaction is either listed by sm_list_prepared() under
 * gid, or was never prepared and is aborted. Since the
 * participant never voted yes, sm_abort_prepared() is always a
 * correct resolution.
 *
 * Errors: SM_ERR_SERIALIZATION (retryable; the transaction is
 * aborted and NOT prepared), SM_ERR_NOTRANS, SM_ERR_EXISTS
 * (gid in use), SM_ERR_INVALID, SM_ERR_IO (outcome unknown).
 */
int sm_prepare(sm_session_t* session, const char* gid);

/* One unresolved prepared transaction, as listed below. */
typedef struct sm_prepared_t sm_prepared_t;
struct sm_prepared_t {
    const char* gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
};

/*
 * List this database's unresolved prepared transactions,
 * including those restored from before a crash.
 *
 * On success *out points to *n entries allocated by the
 * library; release them with sm_free_prepared(). *n may be 0,
 * in which case *out is NULL.
 *
 * Needs no active transaction and starts none.
 *
 * Errors: SM_ERR_NOMEM, SM_ERR_IO.
 */
int sm_list_prepared(sm_session_t* session, sm_prepared_t** out,
                     size_t* n);

/* Release a list from sm_list_prepared(). NULL is a no-op. */
void sm_free_prepared(sm_prepared_t* list, size_t n);

/*
 * Resolve a prepared transaction by global id, committing or
 * discarding its writes and releasing its locks.
 *
 * Used by a coordinator, typically on a session other than the
 * one that prepared it -- in particular after a restart, when
 * the original session no longer exists. Neither call requires
 * an active transaction on session, and neither disturbs one.
 *
 * Errors: SM_ERR_NOTFOUND (no unresolved prepared transaction
 * with that gid), SM_ERR_DISKFULL, SM_ERR_IO.
 */
int sm_commit_prepared(sm_session_t* session, const char* gid);
int sm_abort_prepared(sm_session_t* session, const char* gid);

/*
 * Create a relation -- an ordered map from key to value -- and
 * hand out its relid in *relid. The library allocates it; a
 * relid is never reused, so it never names two relations, and
 * one dropped by sm_rmrel() never comes back.
 *
 * Relids rise in creation order, so the lowest one sm_lsrel()
 * lists is the first relation the database ever had: a program
 * can find its own catalog relation again that way.
 *
 * Transactional -- visible to others only at commit, and
 * undone by abort or by a rollback that spans it. A relid from
 * a transaction that aborts is simply never used.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_CONFLICT (retryable),
 * SM_ERR_INVALID, SM_ERR_PREPARED, SM_ERR_DISKFULL,
 * SM_ERR_IO.
 */
int sm_mkrel(sm_session_t* session, uint64_t* relid);

/*
 * Drop a relation and all of its rows. Transactional, as
 * sm_mkrel().
 *
 * Space is not reclaimed at commit, because transactions
 * holding an older snapshot may still legitimately read the
 * relation; physical removal happens in the background once no
 * snapshot can see it.
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable), SM_ERR_INVALID (relid 0), SM_ERR_PREPARED.
 */
int sm_rmrel(sm_session_t* session, uint64_t relid);

/*
 * List the relids of the relations the transaction sees, in
 * ascending order. Reads the catalog like sm_scan(): at the
 * transaction's snapshot, and under SERIALIZABLE the whole
 * list is recorded as read, so a concurrent sm_mkrel() or
 * sm_rmrel() is a conflict.
 *
 * On success *out points to *n relids allocated by the
 * library; release them with sm_free_rels(). *n may be 0, in
 * which case *out is NULL.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID,
 * SM_ERR_NOMEM, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int sm_lsrel(sm_session_t* session, uint64_t** out, size_t* n);

/* Release a list from sm_lsrel(). NULL is a no-op. */
void sm_free_rels(uint64_t* list);

/*
 * Write one row. All three calls require an active,
 * non-prepared transaction. When another transaction committed
 * a change to the same key after this transaction's snapshot,
 * all three fail with SM_ERR_CONFLICT at REPEATABLE READ and
 * SERIALIZABLE, aborting the transaction. At READ COMMITTED
 * sm_update and sm_delete fail with SM_ERR_UPDATED instead,
 * which does not abort: call sm_read_latest, then write again.
 * An sm_insert there is judged against the newest committed
 * version.
 *
 * Each blocks while another active transaction holds the write
 * lock on the key, until that transaction ends, and a blocked
 * call can return SM_ERR_DEADLOCK if it is chosen as the
 * victim of a deadlock cycle.
 *
 *   sm_insert  requires that the key not exist (SM_ERR_DUPKEY)
 *   sm_update  requires that it exist (SM_ERR_NOTFOUND), and
 *              replaces the value entirely -- no partial update
 *   sm_delete  requires that it exist (SM_ERR_NOTFOUND)
 *
 * There is no upsert: attempt sm_insert() and fall back to
 * sm_update() on SM_ERR_DUPKEY.
 *
 * A zero-length key is legal. A zero-length value is legal and
 * is distinct from a deleted key.
 *
 * THE OLD ROW. sm_update() and sm_delete() return the row they
 * replaced through old / oldlen, which a caller maintaining
 * secondary indexes needs in order to remove the entries the
 * old value produced. Pass NULL for both to skip it; they are
 * a pair, and giving one without the other is SM_ERR_INVALID.
 * The write already reads that row to check the key exists, so
 * asking for it costs a copy, not another seek.
 *
 * *old points into session-owned memory and is valid only
 * until the next sm_* call on this session -- the same rule as
 * sm_read(), and the same buffer, so either call invalidates
 * what the other handed out. Copy it to keep it. On failure,
 * and for a zero-length old value, *old may be NULL.
 *
 * Errors for all three: SM_ERR_CONFLICT / SM_ERR_DEADLOCK
 * (retryable), SM_ERR_NOTFOUND (relation, or key for
 * update/delete), SM_ERR_DUPKEY (insert), SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_INVALID, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int sm_insert(sm_session_t* session, uint64_t relid,
              const char* key, size_t klen,
              const char* data, size_t datalen);

int sm_update(sm_session_t* session, uint64_t relid,
              const char* key, size_t klen,
              const char* data, size_t datalen,
              const char** old, size_t* oldlen);

int sm_delete(sm_session_t* session, uint64_t relid,
              const char* key, size_t klen,
              const char** old, size_t* oldlen);

/*
 * Read one key.
 *
 * Returns 0 if the key exists, and *data / *datalen then
 * describe the value. Returns 1 if it does not, leaving *data
 * NULL and *datalen 0; this is not an error and does not set
 * the error state. Returns -1 on error.
 *
 * *data points into session-owned memory and is valid only
 * until the next sm_* call on this session -- including an
 * sm_read(), sm_insert(), or sm_commit() made on behalf of an
 * unrelated part of the program. Copy it to keep it.
 *
 * Reads never block and never take locks, so they cannot
 * return SM_ERR_CONFLICT or SM_ERR_DEADLOCK. Under
 * SERIALIZABLE a read is recorded and may contribute to an
 * SM_ERR_SERIALIZATION failure later, at commit.
 *
 * Errors: SM_ERR_NOTFOUND (the RELATION does not exist -- a
 * missing key returns 1), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_INVALID, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int sm_read(sm_session_t* session, uint64_t relid,
            const char* key, size_t klen,
            const char** data, size_t* datalen);

/*
 * Open a cursor over the keys of relid in ascending bytewise
 * order, from skey to ekey.
 *
 *   skey == NULL  starts at the first key; sklen and
 *                 skey_inclusive are ignored
 *   ekey == NULL  runs to the last key; eklen and
 *                 ekey_inclusive are ignored
 *
 * The *_inclusive flags say whether a key exactly equal to the
 * bound is returned. An empty range is legal and yields no
 * rows. Scanning is FORWARD ONLY; descending iteration is not
 * supported, and is not a planned addition -- no index serves
 * a backward scan (see OBJECTIVE.md). A caller that needs
 * descending order keeps a second relation with inverted key
 * bytes and scans that one forward.
 *
 * Both bounds are taken, rather than a start key alone,
 * because under SERIALIZABLE the range is the predicate
 * tracked for conflicts: scanning to the end of the relation
 * and discarding rows in the caller records a far larger
 * predicate and causes needless serialization failures.
 *
 * The cursor's view is fixed at this call, so it does NOT see
 * writes its own transaction makes afterwards. That is
 * deliberate -- it is Halloween protection, and it is what
 * makes "scan a range and update every row in it" safe. Writes
 * made before the cursor was opened are visible to it.
 *
 * The cursor's snapshot also pins storage reclamation, so one
 * left open indefinitely prevents space from being reclaimed.
 * Close cursors promptly.
 *
 * The cursor is owned by the session, must be used only from
 * the thread using that session, and is invalidated by
 * sm_commit(), sm_abort(), sm_closedb(), and an sm_rollback()
 * to a savepoint older than it. Multiple cursors may be open
 * on one session at once.
 *
 * Returns NULL on error: SM_ERR_NOTFOUND (no such relation),
 * SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID,
 * SM_ERR_NOMEM, SM_ERR_IO.
 */
sm_scan_t* sm_scan(sm_session_t* session, uint64_t relid,
                   const char* skey, size_t sklen, int skey_inclusive,
                   const char* ekey, size_t eklen, int ekey_inclusive);

/*
 * Advance to the next row.
 *
 * Returns 0 if a row was produced, and the four out-params
 * then describe it, valid until the next call on THIS CURSOR.
 * Returns 1 at end of range, leaving the out-params NULL and
 * 0; this is not an error and does not set the error state.
 * Returns -1 on error.
 *
 * Once it has returned 1 or -1, further calls return the same.
 *
 * Errors: SM_ERR_CORRUPT, SM_ERR_IO.
 */
int sm_scannxt(sm_scan_t* scan, const char** key, size_t* klen,
               const char** data, size_t* datalen);

/*
 * Move an open cursor to key, so the next sm_scannxt()
 * returns from there. This is how a caller probes a relation
 * repeatedly -- an index nested-loop join, an IN-list, a skip
 * scan -- without closing and reopening a cursor per probe.
 *
 * inclusive says whether a row at key itself is returned.
 *
 * The cursor keeps its snapshot and its view, so a reposition
 * does NOT see writes made since sm_scan(), and it keeps its
 * bounds: it returns rows only from the range sm_scan() was
 * given.
 *
 * Seeking moves FORWARD ONLY. A key before the last one the
 * cursor read fails with SM_ERR_INVALID, as does a key below
 * the scan's start key. A key past the scan's end key is
 * allowed and leaves the cursor at its end, where sm_scannxt()
 * returns 1.
 *
 * Under SM_XCT_SERIALIZABLE the keys skipped over still count
 * as read: the cursor's predicate covers its start key through
 * the furthest key it reached, gaps included. That is an
 * over-approximation, so it can only cause a serialization
 * failure that was not strictly necessary, never miss one.
 *
 * Errors: SM_ERR_INVALID (a backward seek, a key below the
 * start key, or a cursor that already failed).
 */
int sm_scan_seek(sm_scan_t* scan, const char* key, size_t klen,
                 int inclusive);

/*
 * Close a cursor and release its resources, including the
 * snapshot it was pinning. Passing NULL is a no-op.
 *
 * Under SERIALIZABLE this is where the range the cursor
 * actually traversed is registered for conflict tracking. A
 * cursor abandoned early registers the shorter range it really
 * read, which is both correct and tighter than the range
 * requested -- so closing early is preferable to draining a
 * scan whose rows are not needed.
 *
 * scan is invalid afterwards. Closing never fails, which is
 * why it returns nothing: a caller has no useful recovery from
 * a failure to release resources.
 */
void sm_scanend(sm_scan_t* scan);

#ifdef __cplusplus
}
#endif

#endif /* SM_H */
