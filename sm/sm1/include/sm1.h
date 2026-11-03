/*
 * sm1.h -- the wisr storage manager's public C++ API, in
 * namespace sm1 (DESIGN.md section 5 at the top level). Names,
 * keys and values travel as std::string_view, lists as
 * std::vector; sm1::scan() alone keeps (pointer, length) bounds,
 * since the empty key is a key and a NULL bound means none.
 *
 * API.md is the interface contract; SM1_DESIGN.md is the design
 * behind it.
 *
 * Conventions that hold throughout:
 *
 *   - A function returning int returns 0 on success and -1 on
 *     error. A few also return 1, which is not an error: "no
 *     row" from the reads and sm1::scannxt(), "not had at once"
 *     from the trylocks; API.md lists them.
 *   - A call that yields a handle -- sm1::attach(), sm1::opendb(),
 *     sm1::scan() -- returns 0 or -1 like any other and hands the
 *     handle back through its last parameter, set only on success.
 *   - On any error return, sm1::errnum() and sm1::errmsg() describe
 *     it. A successful call leaves both untouched, so neither
 *     can be used to test for success.
 *   - Byte strings passed in are read during the call and never
 *     retained; the caller may free them immediately after.
 *   - Byte strings handed out point into engine-owned memory,
 *     are not NUL-terminated, and must not be freed. See
 *     sm1::read() and sm1::scannxt() for their validity windows.
 *
 * Threading: an sm1::env_t may be used from any number of threads
 * at once. An sm1::session_t and its cursors may be used by one
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
 * it, and sm1::attach(), sm1::mkdb(), and sm1::rmdb() assume they
 * are alone in the home directory.
 */
#ifndef SM1_H
#define SM1_H

#include "sm0.h"
#include "sm_error.h"
#include "sm_lock.h"
#include "sm_options.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sm1 {

/*
 * Limits. A key or value beyond its bound, or a name outside
 * 1..bound bytes, fails with SM_ERR_INVALID. So does a name
 * holding a NUL byte: names are passed as std::string_view, and
 * the engine keeps them as C strings.
 *
 * max_dbname matches PostgreSQL's NAMEDATALEN - 1, so every
 * name PostgreSQL accepts for a database is one wisr accepts,
 * and max_gid matches its GIDSIZE. An over-long name is an
 * error here, never a truncation: PostgreSQL emits a NOTICE
 * and truncates, and this API has no channel for a notice.
 */
inline constexpr size_t max_keylen      = 4096;
inline constexpr size_t max_datalen     = 64 * 1024 * 1024;
inline constexpr size_t max_dbname      = 63;
inline constexpr size_t max_seqname     = 63;
inline constexpr size_t max_gid         = 200;
inline constexpr int    max_savepoints  = 256;

/*
 * Store 0 is sm1's catalog. Callers may read, write, scan and
 * lock the keys in it that begin with upper_prefix, which sm1
 * keeps for the catalog of the level above and never reads
 * itself (SM1_DESIGN.md §11); any other key in store 0 is
 * SM_ERR_INVALID. A scan of store 0 must stay inside the
 * prefix: its start key begins with it, and its end key does
 * too, or is its successor, "V", exclusive.
 */
inline constexpr char   upper_prefix    = 'U';

/*
 * Error codes, one namespace for the whole stack: every level's
 * codes are defined in the root's include/sm_error.h, as a C
 * enum the C wrapper shares. errnum_t names it here, and the
 * using-enum brings its SM_* values into namespace sm1.
 */
using errnum_t = ::sm_errnum_t;
using enum ::sm_errnum_t;

/*
 * The calling thread's error state and the process-wide log
 * hook are level 0's (sm0.h, where each is documented), brought
 * into namespace sm1 so the stack and its callers name them
 * sm1::errnum(), sm1::set_log() and so on.
 */
using sm0::errnum;
using sm0::errmsg;
using sm0::retryable;
using sm0::seterr;
using sm0::reseterr;
using sm0::dupkey_t;
using sm0::errdup;
using sm0::setdup;
using sm0::errname;
using sm0::loglevel_t;
using sm0::log_fn;
using sm0::set_log;

/*
 * An environment: one process's handle on a home directory,
 * through which every database and session call goes.
 */
struct env_t;

/* A session: one caller's connection to one database. */
struct session_t;

/* A scan cursor over a range of one store. */
struct scan_t;

/* A bulk load into one store (load_open). */
struct loader_t;

/*
 * A raw pin: keeps one row read or replaced in memory, where
 * RocksDB has it, until it is emptied (../ROW.md §2). The caller
 * makes it with sm1::rawpin_new, keeps it, and frees it; sm1
 * keeps no list of them.
 */
struct rawpin_t;

/*
 * Isolation levels.
 *
 * READ UNCOMMITTED is deliberately absent: under MVCC there is
 * no state in which a caller could observe an uncommitted
 * version, so the level has no meaning here.
 */

enum class xctlevel_t {
    read_committed  = 0,
    repeatable_read = 1,
    serializable    = 2
};

/*
 * Database options, taken by sm1::mkdb() and by the sm1::opendb()
 * that first opens a database. NULL means all defaults, and a
 * default-constructed options_t holds them too. The fields and
 * their defaults are the root's include/sm_options.h, shared with
 * the C wrapper; options_t only adds the constructor.
 */
struct options_t : ::sm_options_t {
    options_t() { sm_options_init(this); }
};

/*
 * Environment options, taken by sm1::attach. NULL means all
 * defaults, and a default-constructed env_options_t holds them.
 * The fields and their defaults are the root's
 * include/sm_options.h.
 */
struct env_options_t : ::sm_env_options_t {
    env_options_t() { sm_env_options_init(this); }
};

/*
 * Make homedir -- which must already exist -- a wisr home
 * directory: create global/ and base/, the root db inside
 * global/, and the format version inside that.
 *
 * Call it once in a home directory's life, not once per
 * process. It takes no lock and opens nothing. Returns 0, or
 * -1 with SM_ERR_EXISTS if homedir is already a wisr home.
 */
int init(std::string_view homedir);

/*
 * Create an environment on a home directory sm1::init() has
 * already prepared. Recovers every database (SM1_DESIGN.md
 * §9.1), with the default options, and closes each again; one
 * that cannot be recovered fails the attach. The caller must ensure
 * no other environment, in this process or another, has the
 * home directory attached (see "Processes" above).
 *
 * Creates nothing: a homedir that was never initialized is an
 * error, not a new home directory. Returns 0 with the
 * environment in *out, or -1 on error.
 *
 * Unless the process already has one, installs a new-handler
 * (std::set_new_handler) that makes running out of memory fatal,
 * anywhere in the process: it reports "*** FATAL ***" on stderr
 * and to the log hook, then aborts. A bad_alloc from inside
 * RocksDB leaves it unreliable, and only a fresh open's WAL
 * replay recovers safely. A handler the process already has is
 * kept, but one that throws is unsupported.
 *
 * opt's block_cache_bytes sizes the one block cache every
 * database, every column family and the root and temporary
 * instances read through, and a quarter of it, or what opt's
 * max_sessions asks for if more, the buffer pool's frames; NULL
 * means the defaults. A size less than 1 fails with SM_ERR_INVALID.
 */
int attach(std::string_view homedir, env_t*& out,
           const env_options_t* opt = nullptr);

/*
 * Release an environment, leaving the home directory intact.
 * This undoes sm1::attach(), not sm1::init(); afterwards another
 * process may attach the home directory.
 *
 * With force == 0, returns -1 with SM_ERR_INUSE if any
 * session opened through env is still open, and env stays
 * valid. With force != 0, those sessions are closed first,
 * as sm1::closedb() would: cursors are closed and active
 * transactions are aborted, but a prepared transaction is
 * left in doubt for its coordinator. Their sm1::session_t
 * handles are then invalid.
 *
 * force does not preempt other threads: the caller must
 * know that none is inside an sm1_* call on those sessions,
 * as for any other concurrent use of one. NULL is a no-op
 * returning 0.
 */
int detach(env_t* env, int force);

/*
 * Create the database dbname. It is not opened; call sm1::opendb()
 * afterwards.
 *
 * A dbname is a name, never a path: it is a key in the
 * environment's root db, which maps it to the number naming
 * the directory the database actually lives in. So
 * sm1::mkdb(env, "a/../b") creates a database called "a/../b"
 * and reaches nothing outside homedir. Names are compared
 * bytewise and never folded, so "Foo" and "foo" are two
 * databases.
 *
 * Errors: SM_ERR_EXISTS, SM_ERR_PERMISSION, SM_ERR_DISKFULL,
 * SM_ERR_IO, SM_ERR_INVALID.
 */
int mkdb(env_t* env, std::string_view dbname,
         const options_t* opt = nullptr);

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
int rmdb(env_t* env, std::string_view dbname);

/*
 * List the databases in homedir, in bytewise order, into out,
 * which is replaced on success and untouched on failure.
 *
 * A database directory is named by number, so the names live
 * only in the root db: reading homedir will not tell a caller
 * what is there.
 *
 * Errors: SM_ERR_NOMEM, SM_ERR_IO.
 */
int lsdb(env_t* env, std::vector<std::string>& out);

/*
 * The dbid of the database dbname into *out: the number sm1::mkdb
 * gave it, which no other database in this home directory has had
 * or will have (SM1_DESIGN.md §3.5). It is the name of the
 * database's directory, and a caller may use it as the database's
 * id for as long as the database exists.
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_CORRUPT, SM_ERR_IO,
 * SM_ERR_INVALID.
 */
int dbid(env_t* env, std::string_view dbname, uint64_t* out);

/*
 * Open the database dbname and start a session on it. The
 * database must already exist; this never creates one.
 *
 * The first sm1::opendb() on a database in this process opens it
 * and its opt takes effect; later sessions join the open
 * instance and their opt is ignored. Recovery -- restoring
 * prepared transactions and their locks -- happens during that
 * first open.
 *
 * A new session has no active transaction. It belongs to env,
 * which must not be released while the session is open.
 *
 * Returns 0 with the session in out, or -1 on error:
 * SM_ERR_NOTFOUND, SM_ERR_CORRUPT, SM_ERR_PERMISSION,
 * SM_ERR_NOMEM, SM_ERR_IO, SM_ERR_INVALID.
 */
int opendb(env_t* env, std::string_view dbname, session_t*& out,
           const options_t* opt = nullptr);

/*
 * Create a sequence: a named counter that hands out start,
 * start + increment, start + 2 * increment, and so on. The
 * name is a NUL-terminated string of at most sm1::max_seqname
 * bytes, and increment must not be 0.
 *
 * Creating and dropping a sequence is transactional, like
 * sm1::mkstore() -- but the values it hands out are not; see
 * sm1::nextseq(). A name dropped by sm1::rmseq() may be used
 * again: the new sequence starts at its own start, keeping
 * nothing from the old one.
 *
 * A TEMPORARY SEQUENCE, asked for with temporary, is the
 * session's alone and is kept in its memory: it writes nothing
 * to the database, no other session knows its name, and it is
 * gone when the session closes. To its session the name means
 * the temporary sequence, whatever else has it. A transaction
 * declared read-only may make and drop one, and one that has
 * cannot sm1::prepare(). A name the transaction dropped is free
 * again once it commits.
 *
 * Errors: SM_ERR_EXISTS, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable -- concurrently created), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int mkseq(session_t* session, std::string_view name,
          uint64_t start, uint64_t increment, bool temporary = false);

/*
 * Drop a sequence. Transactional, as sm1::mkseq().
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable), SM_ERR_INVALID, SM_ERR_PREPARED.
 */
int rmseq(session_t* session, std::string_view name);

/*
 * Hand out the sequence's next value in out.
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
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO (persisting the
 * next block failed),
 * SM_ERR_DISKFULL.
 */
int nextseq(session_t* session, std::string_view name,
            uint64_t& out);

/*
 * A sequence as sm1::seqref looked it up, which sm1::nextseq takes
 * in place of its name, to draw with no look-up of the name, a read
 * of the catalog, each time: a session's temporary sequence by its
 * name, which is the session's own, and any other by its seqid. It
 * stays good while the sequence is not dropped, which the caller
 * sees to, as sm2 does by the table it has open.
 */
struct seqref_t {
  std::string name;   /* a temporary sequence's; else "" */
  uint64_t seqid = 0; /* 0 for a temporary sequence */
  uint64_t start = 0;
  uint64_t increment = 0;
};

/*
 * Look the sequence name up into ref, as sm1::nextseq does: at the
 * transaction's view if there is one, else at the newest committed
 * state. Errors as sm1::nextseq's.
 */
int seqref(session_t* session, std::string_view name, seqref_t& ref);

/* sm1::nextseq of the sequence ref holds. */
int nextseq(session_t* session, const seqref_t& ref, uint64_t& out);

/*
 * Report the last value the sequence handed out in out, drawing
 * none: no lease is extended and nothing is written.
 *
 * Exact, but after a crash: a clean close keeps each sequence's
 * place. Until a sequence hands out a value after a crash, it is
 * the value below the block reserved last, an upper bound, since
 * the crash skips the rest of that block.
 * Like sm1::nextseq it needs no transaction, and inside one reads
 * the definition at its snapshot.
 *
 * Returns 0 with the value in out, 1 if the sequence never
 * handed one out, leaving out untouched, or -1.
 *
 * Errors: SM_ERR_NOTFOUND (no such sequence), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int lastseq(session_t* session, std::string_view name,
            uint64_t& out);

/*
 * Close a session. Open cursors are closed, and an active
 * transaction is forcibly aborted -- but a PREPARED one is
 * not: it stays in doubt and awaits its coordinator.
 *
 * session is invalid after this call whether it succeeded or
 * not. Passing NULL is a no-op returning 0. Closing the last
 * session on a database leaves it open for its idle time,
 * options_t::idle_close_ms, for a session that comes in meanwhile;
 * then it is closed, keeping each sequence's place. A failed
 * database is closed at once, and an idle one opened anew for a
 * session that asks other options of it.
 */
int closedb(session_t* session);

/*
 * Begin a transaction at level, taking its snapshot if the
 * level calls for one. Exactly one transaction may be active
 * on a session; there is no nesting.
 *
 * read_only declares that the transaction will not write, as
 * SQL's READ ONLY: a write, mkstore, rmstore, mkseq or rmseq
 * then fails with SM_ERR_READONLY. At SERIALIZABLE, SSI treats
 * it as read-only from here on, and releases its SIREAD entries
 * once its snapshot is safe (SM1_DESIGN.md §7.4).
 *
 * Errors: SM_ERR_INTRANS, SM_ERR_INVALID (unknown level).
 */
int begin(session_t* session, xctlevel_t level, bool read_only = false);

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
 * A failed write or sync of the commit record aborts the
 * process (sm0::fatal): the record may or may not have reached
 * the log, so the outcome is unknown. After a reopen the
 * transaction is committed or not according to what reached
 * the log.
 *
 * On SM_ERR_IO the transaction did not commit: persisting the
 * lease that covers its commit timestamp failed, before its
 * commit record was written, and it is aborted.
 *
 * Once called on an active transaction, the transaction is no
 * longer active whatever the result.
 *
 * Errors: SM_ERR_SERIALIZATION (retryable), SM_ERR_NOTRANS,
 * SM_ERR_IO (no commit record was written).
 */
int commit(session_t* session);

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
int abort(session_t* session);

/*
 * Have fn run once the active transaction has committed: at the
 * end of the call that commits it, on that call's thread, outside
 * any transaction. It runs once, and must not throw. It does not
 * run if the transaction aborts or its commit fails.
 *
 * Functions run newest first: after on_commit(A) and on_commit(B),
 * B runs before A. A rollback to a savepoint drops the functions
 * added since it.
 *
 * The commit is durable before fn runs, and fn is not: if the
 * machine crashes right after the commit, fn never runs, though
 * the transaction committed. The caller must account for that,
 * by finding the work fn would have done when it next starts.
 *
 * A prepared transaction keeps its functions, and whichever call
 * commits it runs them: sm1::commit on the session that prepared
 * it, or sm1::commit_prepared on any session. They are memory, not
 * part of the prepared record: a transaction committed after a
 * restart runs none, so what a function does must be something
 * the caller can also find to do again when it starts.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID (no
 * function).
 */
int on_commit(session_t* session, std::function<void()> fn);

/*
 * Whether session prepared a transaction that no session has
 * resolved yet: then sm1::commit and sm1::abort on it resolve
 * that prepared transaction, not an active one. A caller that
 * must never undo a prepared transaction -- the C wrapper's
 * catch-all -- asks this before aborting. Only the session's own
 * thread can prepare, so a false answer stays false; a true one
 * may turn false when another session resolves the gid.
 *
 * NULL is false. Needs no transaction and sets no error.
 */
bool in_doubt(session_t* session);

/*
 * The mode a lock is taken in, Gray's five, with the conflict
 * table and what each mode means in the root's sm_lock.h. They
 * are defined there, as the error codes are, so the C wrapper
 * and sm1 share one enum: lockmode_t names it here, and the
 * using-enum brings its SM_LOCKMODE_* values into namespace
 * sm1. A write takes the name it writes EXCLUSIVE.
 */
using lockmode_t = ::sm_lockmode_t;
using enum ::sm_lockmode_t;

/*
 * What a lock names, as sm1::lock_status reports it (LOCK.md).
 * The values are persisted in a prepared transaction's lock
 * set, so they never change.
 *
 *   LOCKTYPE_ROW     a key in a store, taken by sm1::lock_row
 *                    or by a write of the key.
 *   LOCKTYPE_OID     an object id in no store, taken by
 *                    sm1::lock_oid; its key is the oid,
 *                    big-endian, and its storeid 0.
 *
 * Two locks of different types never conflict, however equal
 * their storeid and key.
 */
enum locktype_t : uint8_t {
  LOCKTYPE_ROW = 0,
  LOCKTYPE_OID = 1
};

/*
 * Lock a row without writing it: SQL's SELECT ... FOR SHARE
 * and FOR UPDATE. storeid and key must name a live store and a
 * legal key; the key need not exist. The caller passes the key
 * whole: sm1 hashes it for its lock table, and two keys hashing
 * alike are one lock: they wait for each other needlessly, and a
 * transaction that locks one holds both, in one mode.
 *
 * The call BLOCKS while another transaction holds the lock in
 * a conflicting mode (lockmode_t's table), or has written the
 * key and not yet ended, exactly as a write does, and it can
 * fail with SM_ERR_DEADLOCK for the same reason. A transaction
 * that already holds the row and asks for a mode it does not
 * cover -- including by writing the row -- converts to the
 * weakest mode covering both, waiting for the other holders that
 * conflict with it; two transactions converting on the same row
 * deadlock, and the younger one is the victim.
 *
 * Once the lock is held, above read committed, the call runs the
 * same check a write does: if another transaction committed a
 * version of this key after this transaction's snapshot, it
 * fails with SM_ERR_CONFLICT. So a successful sm1::lock_row
 * means what the caller reads next under that lock is current,
 * which is what makes "lock, then read, then act" safe. At read
 * committed there is no check and no SM_ERR_CONFLICT: the caller
 * reads the newest version with sm1::read_latest.
 *
 * as_read says the row is one the running statement read, as
 * SQL's FOR UPDATE and FOR SHARE lock it. Then a row whose newest
 * version this transaction wrote after the statement began
 * (sm1::stmt_begin) is not locked: the call fails with
 * SM_ERR_SELFMODIFIED, as a write of it does, since the statement
 * does not see that version. It is not an abort; PostgreSQL's
 * FOR UPDATE passes over such a row, a cursor's after a later
 * UPDATE or DELETE of its transaction. Without it, as a caller
 * locks a key to read its newest version, the transaction's own
 * included, no such check is made.
 *
 * The lock is held to the end of the transaction, or until an
 * sm1::rollback() past the point it was taken. A conversion is
 * never undone by a rollback: the row stays locked in the
 * stronger mode until the transaction ends.
 *
 * kind is the caller's, kept with the lock and never read by sm1:
 * lock_status reports it, as it does not the key. sm2 passes the
 * key's relid, so a host tells a row's lock from an index
 * entry's. A lock already held keeps the kind it was taken with.
 *
 * A transaction holding locks it took with sm1::lock_row or
 * sm1::lock_oid may be prepared; the locks survive in the
 * prepared transaction, each in its mode, and are restored after
 * a crash, so nothing conflicting may take them until a
 * coordinator resolves it.
 *
 * Errors: SM_ERR_CONFLICT / SM_ERR_DEADLOCK (retryable),
 * SM_ERR_BLOCKED (under sm1::set_qa_nowait), SM_ERR_SELFMODIFIED,
 * SM_ERR_NOTFOUND (store), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_INVALID.
 */
int lock_row(session_t* session, uint64_t storeid, std::string_view key,
             lockmode_t mode, bool as_read = false, uint16_t kind = 0);

/*
 * Take a row's lock as sm1::lock_row does, if it can be had at
 * once: this never waits. Returns 0 with the lock held, to the
 * end of the transaction; 1, with nothing taken or changed, if
 * it would have waited -- another transaction holds the lock in
 * a mode that conflicts, or has written the row and not yet
 * ended; or -1. SQL's NOWAIT and SKIP LOCKED ask like this.
 *
 * The lock taken is sm1::lock_row's, with its check: above READ
 * COMMITTED a row changed since the snapshot fails the call, and
 * the transaction, with SM_ERR_CONFLICT.
 *
 * Errors: as sm1::lock_row, but for SM_ERR_DEADLOCK,
 * SM_ERR_BLOCKED and SM_ERR_INTERRUPTED, which come of waiting.
 */
int trylock_row(session_t* session, uint64_t storeid,
                std::string_view key, lockmode_t mode,
                bool as_read = false, uint16_t kind = 0);

/*
 * Lock an object id in no store, which nothing ever writes --
 * what a level above locks to stand for an object of its own.
 * sm3 locks a table's definition this way, and every sm3 object
 * draws its oid from one sequence, so one oid names one object.
 * No store is read and no version is checked, at any isolation
 * level; taken IS or IX, the lock is a slot of the session's own
 * (SM1_DESIGN.md section 5.1, the fast path).
 *
 * It blocks, converts, is held and survives a prepare as
 * sm1::lock_row's lock does. An oid lock and a row lock never
 * conflict.
 *
 * Errors: SM_ERR_DEADLOCK (retryable), SM_ERR_BLOCKED (under
 * sm1::set_qa_nowait), SM_ERR_INTERRUPTED, SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_INVALID.
 */
int lock_oid(session_t* session, uint64_t oid, lockmode_t mode);

/*
 * Take an oid's lock as sm1::lock_oid does, if it can be had at
 * once: 0 with the lock held; 1, with nothing taken or changed,
 * if another transaction holds it in a mode that conflicts; or
 * -1. A bulk load asks for its table so (BULKLOAD.md section 13).
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID.
 */
int trylock_oid(session_t* session, uint64_t oid, lockmode_t mode);

/*
 * Read the newest committed version of a key, ignoring the
 * snapshot, under a row lock this transaction already holds, or
 * with a write of the key by this transaction still in effect,
 * which every other writer of it waits for.
 * Returns as sm1::read() does: 0 found, 1 no such key. At read
 * committed; above it only when the newest version is this
 * transaction's own, which its snapshot cannot hide and no other
 * writer can change, as an insert's arbiter reads a key its own
 * statement took. Under an exclusive lock it also lets the next
 * write of the key through, after SM_ERR_UPDATED (SM1_DESIGN.md
 * §5.4).
 *
 * Errors: SM_ERR_INVALID (above read committed with no version of
 * this transaction's own, or neither a lock held nor a write of
 * the key), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_NOTFOUND (store), SM_ERR_MOVED (the newest version is a
 * move's tombstone, sm1::erase_moved).
 */
int read_latest(session_t* session, uint64_t storeid,
                std::string_view key, std::string_view& data);

/* sm1::read_latest into a raw pin, as sm1::read into one. */
int read_latest(session_t* session, uint64_t storeid,
                std::string_view key, rawpin_t* pin,
                std::string_view& data);

/*
 * Begin a statement: read committed then reads every row of
 * this statement at ONE snapshot, instead of taking a fresh
 * one per call.
 *
 * Without it, each sm1::read() and each sm1::scan() under
 * sm1::xctlevel_t::read_committed takes its own snapshot, so a caller
 * whose statement spans several calls -- an index lookup, say,
 * being one scan plus N reads -- sees N+1 committed states and
 * can return a mixture that never existed.
 *
 * The snapshot is taken by the first read after this call, not
 * by this call, so a statement may lock or wait first and
 * still see everything committed by the time it reads. The
 * statement runs until the next sm1::stmt_begin(), which starts
 * another, or until the transaction ends.
 *
 * Once called, the transaction reads this way for the rest of
 * its life; there is no going back to a snapshot per call.
 * Under sm1::xctlevel_t::repeatable_read and sm1::xctlevel_t::serializable the
 * whole transaction already reads at one snapshot.
 *
 * At every level, the statement's reads see the transaction's
 * own writes made before it began, and not those it makes
 * itself, as PostgreSQL's command id does. A cursor or a point
 * read begun later in the statement does not see them either.
 *
 * Writes still check for conflicts against the snapshot taken
 * by sm1::begin(), so an SM_ERR_CONFLICT can still arrive on a
 * row this statement has just read. An update or erase of a key
 * the statement itself has already written fails with
 * SM_ERR_SELFMODIFIED and writes nothing; it is not an abort,
 * and the caller skips the row.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID.
 */
int stmt_begin(session_t* session);

/* What a statement replaces, for sm1::stmt_end() to put back. */
struct stmt_t {
    bool pinned = false;      /* statement snapshots on (§6.2) */
    bool taken = false;       /* snapshot_ts is the one to read at */
    uint64_t snapshot_ts = 0; /* the statement's snapshot */
    bool marked = false;      /* reads see own writes up to seq */
    uint32_t seq = 0;         /* the statement's mark */
};

/*
 * Begin a statement inside the running one, as a trigger's
 * commands run inside the statement that fired them, saving the
 * outer statement's snapshot and mark in outer. The inner one
 * sees the outer's writes so far. Errors as sm1::stmt_begin().
 */
int stmt_begin(session_t* session, stmt_t& outer);

/*
 * End a statement begun by the call above: the outer one resumes
 * with its own snapshot and mark, and again sees neither its own
 * writes nor the inner one's. Errors: SM_ERR_NOTRANS,
 * SM_ERR_PREPARED.
 */
int stmt_end(session_t* session, const stmt_t& outer);

/*
 * Advance the running statement's mark to now, as PostgreSQL's
 * CommandCounterIncrement does: its later reads see the
 * transaction's writes so far, the statement's own included,
 * at the statement's snapshot, which stays. A store or a
 * sequence the statement made is usable after it, as by a
 * statement that makes a table and fills it. With no statement
 * begun every own write is seen already, and it does nothing.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int stmt_advance(session_t* session);

/*
 * FOR QA ONLY: it lets one thread play several sessions in a
 * test, and is not how a caller asks for NOWAIT or SKIP LOCKED.
 * Those are to be explicit flags on the calls that can wait.
 *
 * Set whether the session waits for locks. Off, the default, a
 * write, an sm1::lock_row() or an sm1::lock_oid() BLOCKS while
 * another transaction holds the lock in a conflicting mode. On,
 * such a call fails at once with SM_ERR_BLOCKED instead. That is
 * not an abort and it is not retryable: the transaction stays
 * active and holds exactly what it held before, and the caller
 * polls by repeating the call, which fails the same way or goes
 * through. The setting lasts until it is changed, across
 * transactions.
 *
 * A session that polls waits on nobody as far as the lock table
 * knows, so it is never a deadlock victim, and sessions polling
 * each other's locks poll for ever: breaking that cycle is the
 * caller's (SM1_DESIGN.md §5.1).
 *
 * Errors: SM_ERR_INVALID.
 */
int set_qa_nowait(session_t* session, bool on);

/*
 * Give the session the host's number for it, by which waits_for
 * names it; 0, its number until this is called, is none. The
 * host keeps the numbers distinct among its open sessions.
 *
 * Errors: SM_ERR_INVALID.
 */
int set_tag(session_t* session, uint64_t tag);

/*
 * Set how long the session waits for a lock or a writer before it
 * looks for a cycle of waits through itself (SM1_DESIGN.md §5.1):
 * ms > 0, or 0 for its database's deadlock_ms, the default. As
 * PostgreSQL's deadlock_timeout, a session with a shorter one finds
 * a cycle first; the victim is still the cycle's youngest.
 *
 * Errors: SM_ERR_INVALID.
 */
int set_deadlock_ms(session_t* session, int ms);

/*
 * The numbers of the sessions the open session numbered tag
 * waits for now, into *out, cleared first: the holders of the
 * lock it waits on, or the transaction whose uncommitted version
 * it waits behind (SM1_DESIGN.md §5.1). Empty if it waits for
 * none, or no open session has the number; a session it waits
 * for that has no number, or is a prepared transaction, is left
 * out. Any thread may ask about any session of env: for
 * PostgreSQL's pg_blocking_pids().
 *
 * Errors: SM_ERR_INVALID.
 */
int waits_for(env_t* env, uint64_t tag, std::vector<uint64_t>* out);

/*
 * A lock of the lock table, held or waited for, as lock_status
 * lists it: its database (name and dbid), its id (type, storeid,
 * and word: an OID lock's oid, a row lock's key hash, as no key
 * is reported), the kind sm1::lock_row was given (0 for an OID
 * lock, or a row's a write took), the mode, the host's number for
 * the session (set_tag; 0 for none, or for a prepared
 * transaction), and whether the session holds it or waits for it.
 */
struct lock_status_t {
  std::string dbname;
  uint64_t    dbid;
  locktype_t  type;
  uint64_t    storeid;
  uint64_t    word;
  uint16_t    kind;
  lockmode_t  mode;
  uint64_t    tag;
  bool        granted;
};

/*
 * Every lock of env's open databases, into out, cleared first: one
 * for each holder of each lock, in the mode it holds, and one for
 * each session waiting in sm1::lock_row or sm1::lock_oid, in the
 * mode it asks. A writer waiting behind an uncommitted version
 * holds and asks no lock and is not listed: waits_for names whom
 * it waits for. Any thread may ask: for PostgreSQL's pg_locks.
 *
 * Errors: SM_ERR_INVALID.
 */
int lock_status(env_t* env, std::vector<lock_status_t>& out);

/*
 * Set a savepoint at the current point in the transaction and
 * return its DEPTH: 1 for the first, 2 for one set inside it,
 * and so on. Savepoints nest; a transaction may hold up to
 * sm1::max_savepoints of them, and they are all discarded at
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
 * (the stack is full, or a bulk load is open).
 */
int savepoint(session_t* session);

/*
 * Undo every write made since the savepoint at depth. That
 * savepoint survives, so the same mark may be rolled back to
 * more than once, and the savepoints set inside it are gone.
 * Locks taken since the mark are released, locks held from
 * before it are kept, and the transaction stays active.
 *
 * Scan cursors opened AFTER the savepoint are closed: a
 * sm1::scannxt() or sm1::scan_seek() on one fails with
 * SM_ERR_SCANCLOSED until sm1::scanend() frees it, as the
 * transaction's end does. One opened before it stays open
 * and keeps its position. A cursor's view is fixed when it
 * is created, so a later one would otherwise go on returning
 * rows this call just undid.
 *
 * Requires a savepoint at that depth. To roll a transaction
 * back to its start, call sm1::savepoint() immediately after
 * sm1::begin() and roll back to the depth it returned.
 *
 * Rollback does NOT undo read tracking: under SERIALIZABLE,
 * reads performed and then rolled back still count towards
 * serialization checking. Conservative, never unsafe.
 *
 * What a rollback deletes is remembered in memory, up to the
 * savepoint_undo_bytes option for each transaction. A
 * transaction that writes more than that while a savepoint is
 * set keeps writing, and the savepoints then set are lost: a
 * rollback to one fails with SM_ERR_UNDOLIMIT. A savepoint set
 * afterwards works as usual.
 *
 * A rollback that fails past its checks, at a lost savepoint or
 * on SM_ERR_IO, dooms the transaction: the writes it was to undo
 * are still there, so every later call on it but sm1::abort
 * fails with SM_ERR_DOOMED, the commit too.
 *
 * Errors: SM_ERR_NOSAVEPOINT (no savepoint at that depth),
 * SM_ERR_UNDOLIMIT, SM_ERR_IO, SM_ERR_INVALID (a bulk load is
 * open), SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_DOOMED.
 */
int rollback(session_t* session, int depth);

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
int forget(session_t* session, int depth);

/*
 * Prepare the active transaction for commit under the global
 * transaction id gid, a NUL-terminated string of at most
 * sm1::max_gid bytes, unique among this database's unresolved
 * prepared transactions.
 *
 * Runs serialization validation, then durably records the
 * prepared state. The transaction is then PREPARED: data
 * operations, savepoints, and rollback are rejected with
 * SM_ERR_PREPARED, and only sm1::commit() and sm1::abort() are
 * accepted. It survives a crash, is restored at the next
 * sm1::opendb() still holding its row locks, and cannot be told
 * "no" by the engine.
 *
 * From here on the transaction belongs to the database, not
 * the session: any session may resolve it by gid, and
 * sm1::closedb() leaves it in doubt. While it is unresolved, the
 * preparing session's sm1::begin() fails with SM_ERR_INTRANS.
 * Once any session has resolved it, the preparing session has
 * no transaction: its sm1::commit() and sm1::abort() fail with
 * SM_ERR_NOTRANS, and sm1::begin() succeeds.
 *
 * An abandoned prepared transaction blocks writers on its keys
 * and prevents reclamation indefinitely. That is inherent to
 * 2PC and is exposed rather than hidden;
 * sm1::abort_prepared() is the operator escape hatch.
 *
 * A failed write of the prepared record aborts the process
 * (sm0::fatal): the record may or may not have reached the log,
 * so the outcome is unknown. After a reopen the transaction is
 * either listed by sm1::list_prepared() under gid, or was never
 * prepared and is aborted. Since the participant never voted
 * yes, sm1::abort_prepared() is always a correct resolution.
 *
 * Errors: SM_ERR_SERIALIZATION (retryable; the transaction is
 * aborted and NOT prepared), SM_ERR_NOTRANS, SM_ERR_EXISTS
 * (gid in use), SM_ERR_INVALID, SM_ERR_IO.
 */
int prepare(session_t* session, std::string_view gid);

/* One unresolved prepared transaction, as listed below. */

struct prepared_t {
    std::string gid;
    long long   prepared_at;   /* wall-clock, seconds since epoch */
};

/*
 * List this database's unresolved prepared transactions,
 * including those restored from before a crash.
 *
 * On success out is replaced by the list, which may be empty;
 * on failure it is untouched.
 *
 * Needs no active transaction and starts none.
 *
 * Errors: SM_ERR_NOMEM, SM_ERR_IO.
 */
int list_prepared(session_t* session, std::vector<prepared_t>& out);


/*
 * Resolve a prepared transaction by global id, committing or
 * discarding its writes and releasing its locks.
 *
 * Used by a coordinator, typically on a session other than the
 * one that prepared it -- in particular after a restart, when
 * the original session no longer exists. Neither call requires
 * an active transaction on session, and neither disturbs one.
 *
 * A failed write of the COMMITTED or ABORTED record, or of the
 * commit's sync, aborts the process (sm0::fatal), as in
 * sm1::commit(): after a reopen the transaction is resolved or
 * still listed, according to what reached the log.
 *
 * Errors: SM_ERR_NOTFOUND (no unresolved prepared transaction
 * with that gid), SM_ERR_DISKFULL, SM_ERR_IO.
 */
int commit_prepared(session_t* session, std::string_view gid);
int abort_prepared(session_t* session, std::string_view gid);

/*
 * Create a store -- an ordered map from key to value -- and
 * hand out its storeid in storeid. The library allocates it; a
 * storeid is never reused, so it never names two stores, and
 * one dropped by sm1::rmstore() never comes back.
 *
 * Storeids rise in creation order, so the lowest one sm1::lsstore()
 * lists is the first store the database ever had. A layer above
 * keeps its own catalog in store 0 under upper_prefix instead,
 * which needs no finding.
 *
 * Transactional -- visible to others only at commit, and
 * undone by abort or by a rollback that spans it. A storeid from
 * a transaction that aborts is simply never used.
 *
 * A TEMPORARY STORE, asked for with temporary, is the session's
 * alone: to every other session its storeid names nothing
 * (SM_ERR_NOTFOUND), and sm1::lsstore() does not list it. It is
 * dropped when the session closes, and nothing of it survives a
 * restart. Its rows are not logged. They commit, abort and roll
 * back with the transaction as any row does, but take no lock,
 * meet no conflict, and are tracked by no SERIALIZABLE check;
 * sm1::lock_row() on one succeeds and holds nothing. A transaction
 * declared read-only may make, write and drop one. A transaction
 * that has done any of the three cannot sm1::prepare().
 *
 * A CHAINED STORE, asked for with chained, also holds chained keys
 * (the chain_ calls, ../VCHAIN.md): each key's newest version under
 * the plain key, the older ones chained behind it. Today's calls
 * reach its versioned keys as any store's. A temporary store may be
 * chained too.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_CONFLICT (retryable),
 * SM_ERR_INVALID, SM_ERR_PREPARED, SM_ERR_DISKFULL,
 * SM_ERR_IO.
 */
int mkstore(session_t* session, uint64_t& storeid, bool temporary = false,
            bool chained = false);

/*
 * Drop a store and all of its rows. Transactional, as
 * sm1::mkstore().
 *
 * Space is not reclaimed at commit, because transactions
 * holding an older snapshot may still legitimately read the
 * store; physical removal happens in the background once no
 * snapshot can see it.
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable), SM_ERR_INVALID (storeid 0), SM_ERR_PREPARED.
 */
int rmstore(session_t* session, uint64_t storeid);

/*
 * List the storeids of the stores the transaction sees, in
 * ascending order. Reads the catalog like sm1::scan(): at the
 * transaction's snapshot, and under SERIALIZABLE the whole
 * list is recorded as read, so a concurrent sm1::mkstore() or
 * sm1::rmstore() is a conflict.
 *
 * On success out is replaced by the storeids, which may be
 * none; on failure it is untouched.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID,
 * SM_ERR_NOMEM, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int lsstore(session_t* session, std::vector<uint64_t>& out);

/*
 * Estimate the bytes of the userkeys from lo up to but excluding
 * hi in the store, every version and tombstone of them counted:
 * in disk, those in sorted files, compressed, and in mem, those
 * in memtables, uncompressed. A NULL lo or hi leaves that end
 * open. Neither part is corrected, and they are not to be added
 * as they stand: rows not yet flushed are counted at full size.
 * mem is counted for a range of whole 2-byte key prefixes, lo and
 * hi each 2 bytes or NULL, as sm2's relids give: the bytes written
 * under them into memtables not yet flushed, each version's key and
 * value as sm1 writes it. Of any other range it is RocksDB's
 * estimate from sampling its memtables, an order of magnitude and
 * no better: for a range among others, measured from 0 to 4 times.
 *
 * Reads RocksDB's estimates, not the rows: it needs no
 * transaction, takes no lock and sees no snapshot. A store
 * created by a transaction not yet committed, and one dropped
 * and not yet reclaimed, are measured all the same.
 *
 * Errors: SM_ERR_NOTFOUND (no such store), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_IO.
 */
int approxsize(session_t* session, uint64_t storeid,
               const char* lo, size_t lolen,
               const char* hi, size_t hilen,
               uint64_t& disk, uint64_t& mem);

/*
 * Count in out the sorted runs a point lookup in the store may
 * visit now: the active memtable if it holds anything, each
 * immutable memtable, each file of level 0, and each level
 * below it that has a file. Needs no transaction, as
 * sm1::approxsize().
 *
 * Errors: SM_ERR_NOTFOUND (no such store), SM_ERR_INVALID,
 * SM_ERR_PREPARED.
 */
int probes(session_t* session, uint64_t storeid, uint64_t& out);

/*
 * Compact the store: flush its memtables, then merge every level
 * into its bottom one, so a point lookup visits one sorted run,
 * and the GC filter meets every version. It waits for the
 * compaction, seconds for a store of hundreds of megabytes, and
 * cannot be cut short; other stores' compactions go on meanwhile.
 * Needs no transaction, as sm1::approxsize(); the caller keeps the
 * store from being dropped until it returns.
 *
 * Errors: SM_ERR_NOTFOUND (no such store), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_IO.
 */
int compact(session_t* session, uint64_t storeid);

/*
 * Write one row. All three calls require an active,
 * non-prepared transaction. When another transaction committed
 * a change to the same key after this transaction's snapshot,
 * all three fail with SM_ERR_CONFLICT at REPEATABLE READ and
 * SERIALIZABLE, aborting the transaction. At READ COMMITTED
 * sm1::update and sm1::erase fail with SM_ERR_UPDATED instead,
 * which does not abort: call sm1::read_latest, then write again.
 * An sm1::insert there is judged against the newest committed
 * version.
 *
 * Each blocks while another active transaction has written the
 * key and not ended, or holds a lock on it taken with
 * sm1::lock_row(), until that transaction ends; a blocked call can
 * return SM_ERR_DEADLOCK if it is chosen as the victim of a
 * deadlock cycle. A write itself keeps no lock: its uncommitted
 * version is what the next writer waits for (WRITELOCK.md).
 *
 *   sm1::insert  requires that the key not exist (SM_ERR_DUPKEY)
 *   sm1::update  requires that it exist (SM_ERR_NOTFOUND), and
 *               replaces the value entirely -- no partial update
 *   sm1::erase  requires that it exist (SM_ERR_NOTFOUND)
 *
 * There is no upsert: attempt sm1::insert() and fall back to
 * sm1::update() on SM_ERR_DUPKEY.
 *
 * A zero-length key is legal. A zero-length value is legal and
 * is distinct from a deleted key.
 *
 * THE OLD ROW. sm1::update() and sm1::erase() return the row they
 * replaced through old, which a caller maintaining secondary
 * indexes needs in order to remove the entries the old value
 * produced. Leave it nullptr, the default, to skip it. The
 * write already reads that row to check the key exists, so
 * asking for it costs no seek, and no copy: the row is pinned
 * where RocksDB has it (../PIN.md).
 *
 * *old is valid only until the next call on this session -- the
 * same rule as sm1::read(), and the same pin, so either call
 * invalidates what the other handed out. The forms that take a
 * rawpin_t pin it there instead, valid until the pin is emptied,
 * as a read into a raw pin is. The row the pin held before is
 * let go only when the write succeeds, so data may view it. On
 * failure old is empty, and the pin keeps what it held.
 *
 * Errors for all three: SM_ERR_CONFLICT / SM_ERR_DEADLOCK
 * (retryable), SM_ERR_NOTFOUND (store, or key for
 * update/delete), SM_ERR_MOVED (update/delete of a key a move
 * erased, sm1::erase_moved), SM_ERR_DUPKEY (insert), SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_INVALID, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int insert(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data);

/*
 * sm1::insert of a key no version of has ever been written, which
 * the caller knows: one made from a sequence of its own, as sm2
 * makes a row's _id. With no version to check against, the write
 * reads nothing, where sm1::insert reads the key's newest version;
 * a lock taken on the key is still waited for, and under
 * SERIALIZABLE the write still finds the readers it invalidates. A
 * debug build reads the key, and asserts it has no version.
 */
int insert_new(session_t* session, uint64_t storeid,
               std::string_view key, std::string_view data);

int update(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data,
           std::string_view* old = nullptr);
int update(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data,
           rawpin_t* pin, std::string_view& old);

int erase(session_t* session, uint64_t storeid,
          std::string_view key, std::string_view* old = nullptr);
int erase(session_t* session, uint64_t storeid,
          std::string_view key, rawpin_t* pin, std::string_view& old);

/*
 * sm1::erase of a row that moved to another key (../ROWKEY.md
 * §2.3): its tombstone is marked a move's. A reader sees it as any
 * tombstone. A writer that meets it as the key's newest version,
 * an sm1::update or sm1::erase, fails with SM_ERR_MOVED where a
 * plain tombstone is SM_ERR_NOTFOUND, and so does sm1::read_latest
 * where a plain one returns 1: at READ COMMITTED, after
 * SM_ERR_UPDATED, the writer learns the row lives on elsewhere.
 * The row erased is pinned into pin, as sm1::erase's.
 */
int erase_moved(session_t* session, uint64_t storeid,
                std::string_view key, rawpin_t* pin, std::string_view& old);

/*
 * Read one key.
 *
 * Returns 0 if the key exists, and data then views the value.
 * Returns 1 if it does not, leaving data empty; this is not an
 * error and does not set the error state. Returns -1 on error,
 * also leaving data empty.
 *
 * data views session-owned memory and is valid only until the
 * next call on this session -- including an
 * sm1::read(), sm1::insert(), or sm1::commit() made on behalf of an
 * unrelated part of the program. Copy it to keep it.
 *
 * Reads never block and never take locks, so they cannot
 * return SM_ERR_CONFLICT or SM_ERR_DEADLOCK. Under
 * SERIALIZABLE a read is recorded and may contribute to an
 * SM_ERR_SERIALIZATION failure later, at commit.
 *
 * Errors: SM_ERR_NOTFOUND (the STORE does not exist -- a
 * missing key returns 1), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_INVALID, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int read(session_t* session, uint64_t storeid,
         std::string_view key, std::string_view& data);

/*
 * Read one key into a raw pin: as sm1::read, but the row is not
 * copied. The pin keeps it in memory, and data views it until
 * the pin is emptied: by the next read or write into the pin, by
 * sm1::unpin, or by sm1::rawpin_free. Nothing else lets it go:
 * not the end of the transaction, a rollback to a savepoint, nor
 * the drop of the row's store. A row the pin held before is let
 * go first. On 1 or -1 the pin is left empty, and data empty.
 */
int read(session_t* session, uint64_t storeid,
         std::string_view key, rawpin_t* pin, std::string_view& data);

/*
 * Make an empty raw pin. Every raw pin is emptied before the
 * database it read closes, since RocksDB closes none with an
 * iterator open.
 */
rawpin_t* rawpin_new();

/* Empty pin, then free it; nullptr is a no-op. */
void rawpin_free(rawpin_t* pin);

/* Let go of the row pin holds, if any; it is then empty. */
void unpin(rawpin_t* pin);

/*
 * Open a cursor over the keys of storeid in ascending bytewise
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
 * a backward scan (see SM1_OBJECTIVE.md). A caller that needs
 * descending order builds it into the key -- inverted key bytes,
 * scanned forward -- rather than keeping a mirror store for the
 * other direction (PART4.md §5).
 *
 * Both bounds are taken, rather than a start key alone,
 * because under SERIALIZABLE the range is the predicate
 * tracked for conflicts: scanning to the end of the store
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
 * sm1::commit(), sm1::abort() and sm1::closedb(). An
 * sm1::rollback() to a savepoint older than it closes it: it
 * then fails each read, and sm1::scanend() frees it. Multiple
 * cursors may be open on one session at once.
 *
 * Returns 0 with the cursor in out, or -1 on error:
 * SM_ERR_NOTFOUND (no such store), SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_INVALID, SM_ERR_NOMEM, SM_ERR_IO.
 */
int scan(session_t* session, uint64_t storeid,
         const char* skey, size_t sklen, int skey_inclusive,
         const char* ekey, size_t eklen, int ekey_inclusive,
         scan_t*& out);

/*
 * Advance to the next row.
 *
 * Returns 0 if a row was produced, and key and data then view
 * it, valid until the next call on THIS CURSOR. Returns 1 at
 * end of range, leaving both empty; this is not an error and
 * does not set the error state. Returns -1 on error, also
 * leaving both empty.
 *
 * Once it has returned 1 or -1, further calls return the same.
 *
 * Errors: SM_ERR_CORRUPT, SM_ERR_IO.
 */
int scannxt(scan_t* scan, std::string_view& key,
            std::string_view& data);

/*
 * Move an open cursor to key, so the next sm1::scannxt()
 * returns from there. This is how a caller probes a store
 * repeatedly -- an index nested-loop join, an IN-list, a skip
 * scan -- without closing and reopening a cursor per probe.
 *
 * inclusive says whether a row at key itself is returned.
 *
 * The cursor keeps its snapshot and its view, so a reposition
 * does NOT see writes made since sm1::scan(), and it keeps its
 * bounds: it returns rows only from the range sm1::scan() was
 * given.
 *
 * Seeking moves FORWARD ONLY. A key before the last one the
 * cursor read fails with SM_ERR_INVALID, as does a key below
 * the scan's start key. A key past the scan's end key is
 * allowed and leaves the cursor at its end, where sm1::scannxt()
 * returns 1.
 *
 * Under sm1::xctlevel_t::serializable the keys skipped over still count
 * as read: the cursor's predicate covers its start key through
 * the furthest key it reached, gaps included. That is an
 * over-approximation, so it can only cause a serialization
 * failure that was not strictly necessary, never miss one.
 *
 * Errors: SM_ERR_INVALID (a backward seek, a key below the
 * start key, or a cursor that already failed).
 */
int scan_seek(scan_t* scan, std::string_view key, int inclusive);

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
void scanend(scan_t* scan);

/*
 * Whether a rollback to a savepoint set before the cursor opened
 * has closed it: its last row is then gone, and it reads no more.
 */
bool scan_rolled_back(const scan_t* scan);

/*
 * CHAINED KEYS (../VCHAIN.md), in a store sm1::mkstore made chained.
 *
 * A chained key keeps its newest version under the plain key and
 * the older ones chained behind it, so a read is a Get, and one
 * that does not see the newest follows the chain. Its keys are
 * apart from the store's versioned ones: a chained key and a
 * versioned key of the same bytes are two keys.
 *
 * Each call is the call of its name without chain_, with the same
 * checks, conflicts, waits and errors (sm1::insert, sm1::insert_new,
 * sm1::update, sm1::erase, sm1::erase_moved, sm1::read,
 * sm1::read_latest, sm1::lock_row and sm1::trylock_row), and
 * SM_ERR_INVALID besides on a store that is not chained. An abort leaves its versions for
 * readers to pass and GC to remove; a rollback to a savepoint puts
 * the chain back.
 */
/*
 * Whether storeid was made chained (sm1::mkstore), in out: fixed
 * when the store is made, so read with no transaction. Errors:
 * SM_ERR_INVALID, SM_ERR_NOTFOUND.
 */
int store_chained(session_t* session, uint64_t storeid, bool& out);

int chain_insert(session_t* session, uint64_t storeid,
                 std::string_view key, std::string_view data);
int chain_insert_new(session_t* session, uint64_t storeid,
                     std::string_view key, std::string_view data);
int chain_update(session_t* session, uint64_t storeid,
                 std::string_view key, std::string_view data);
int chain_update(session_t* session, uint64_t storeid,
                 std::string_view key, std::string_view data,
                 rawpin_t* pin, std::string_view& old);
int chain_erase(session_t* session, uint64_t storeid,
                std::string_view key);
int chain_erase(session_t* session, uint64_t storeid,
                std::string_view key, rawpin_t* pin, std::string_view& old);
int chain_erase_moved(session_t* session, uint64_t storeid,
                      std::string_view key, rawpin_t* pin,
                      std::string_view& old);
int chain_read(session_t* session, uint64_t storeid,
               std::string_view key, rawpin_t* pin,
               std::string_view& data);
int chain_read(session_t* session, uint64_t storeid,
               std::string_view key, std::string_view& data);
int chain_read_latest(session_t* session, uint64_t storeid,
                      std::string_view key, std::string_view& data);
int chain_read_latest(session_t* session, uint64_t storeid,
                      std::string_view key, rawpin_t* pin,
                      std::string_view& data);
/*
 * A batch read's iterator, kept by its caller from one
 * sm1::chain_read_batch to the next, as a cursor reads its rows in
 * batches (../BATCHREAD.md §2.1). The caller makes it, keeps it, and
 * frees it. sm1 lets go of the iterator it holds at the end of the
 * transaction, at a rollback to a savepoint, and as the session
 * closes, so one left unfreed holds no RocksDB files.
 */
struct batch_iter_t;

/*
 * Make a batch read's iterator, holding none yet. With scattered, the
 * caller knows its keys are far apart in the store, as an
 * uncorrelated index's rows are: every batch is read by MultiGet
 * alone, and it never holds one.
 */
batch_iter_t* batch_iter_new(bool scattered = false);

/* Let go of iter's iterator, then free it; nullptr is a no-op. */
void batch_iter_free(batch_iter_t* iter);

/* Whether iter holds an iterator now: for tests. */
bool batch_iter_held(const batch_iter_t* iter);

/*
 * sm1::chain_read of n keys in one call (../BATCHREAD.md): key i's
 * row into pins[i] and data[i], rc[i] 0, or rc[i] 1 if the read sees
 * none, its pin then empty. The keys' newest versions are read
 * together, in RocksDB's MultiGet, in any order; each key is checked,
 * takes its SIREAD entry and is resolved as a single read of it is.
 * Keys in ascending order are read by one iterator: iter's, if
 * given, brought up to date, or one made and kept in it. A batch
 * whose keys are too far apart to step between marks iter, and
 * iter's later batches of the store are read by MultiGet alone.
 * Returns 0, or -1 on an error, the rows then undefined.
 */
int chain_read_batch(session_t* session, uint64_t storeid,
                     const std::string_view* keys, size_t n,
                     rawpin_t** pins, std::string_view* data, int* rc,
                     batch_iter_t* iter = nullptr);
int chain_lock_row(session_t* session, uint64_t storeid,
                   std::string_view key, lockmode_t mode,
                   bool as_read = false, uint16_t kind = 0);
int chain_trylock_row(session_t* session, uint64_t storeid,
                      std::string_view key, lockmode_t mode,
                      bool as_read = false, uint16_t kind = 0);

/*
 * sm1::approxsize of a chained store's chained keys in [lo, hi):
 * their newest versions alone; with old, the store's older versions
 * too, which no key range divides.
 */
int chain_approxsize(session_t* session, uint64_t storeid,
                     const char* lo, size_t lolen,
                     const char* hi, size_t hilen,
                     uint64_t& disk, uint64_t& mem, bool old = false);

/*
 * sm1::scan over a chained store's chained keys: the same bounds,
 * view, SIREAD range and forward-only order, and the same
 * sm1::scannxt, sm1::scan_seek and sm1::scanend. A key whose newest
 * version the cursor does not see is handed out as the version it
 * does, read from the chain as the store stood when the cursor
 * opened.
 */
int chain_scan(session_t* session, uint64_t storeid,
               const char* skey, size_t sklen, int skey_inclusive,
               const char* ekey, size_t eklen, int ekey_inclusive,
               scan_t*& out);

/*
 * BULK LOAD (BULKLOAD.md section 5, file mode).
 *
 * A load fills an empty store in the caller's transaction without
 * a write a key. Its keys are written to sorted files and handed
 * to RocksDB whole when the load is closed, past the log, the
 * memtable and the compactions; a load that ends small, under
 * options_t::load_small_bytes, is written as one batch instead.
 * Either way the keys are the transaction's own versions: no
 * other transaction sees them until it commits, and an abort
 * leaves none.
 *
 * The store must hold no key, a version of any kind, and THE
 * CALLER MUST KEEP EVERY OTHER TRANSACTION OUT OF IT, readers too,
 * from before the load opens until the transaction ends: sm3
 * holds the table's lock exclusive. sm1 cannot check that; it
 * checks the store is empty. Under that lock a load takes no lock
 * and no latch of its own, and checks nothing against the store:
 * a key is new if no other key of the load equals it. A store the
 * transaction made itself is reached by no one else already.
 *
 * While a load is open, the transaction's other writes to that
 * store are refused with SM_ERR_INVALID: they could not be
 * checked against keys still on their way.
 *
 * Keys go in by STREAM. A stream's keys are a range of the store
 * no other stream's keys fall in, as a table's rows are one range
 * and each of its indexes another. A stream declared ascending is
 * given its keys in order and is written straight to a file; any
 * other is sorted, in memory up to options_t::load_sort_bytes and
 * then in runs on disk that the close merges.
 *
 * While a load is open its transaction may not commit, set a
 * savepoint or roll back to one, and after one it may not
 * prepare: a load is whole. A rollback after the close to a
 * savepoint set before the load empties the store, which held
 * nothing else. An abort, of any kind, ends an open load and
 * frees it; the caller must not use it again.
 *
 * A load's files, the sorted ones and the runs alike, are kept
 * under the temporary instance, {homedir}/base/pgsql_tmp/sm/load,
 * until the store takes them: a database's own directory holds
 * nothing of a load that is not closed.
 */

/*
 * Open a load into storeid in session's transaction.
 *
 * Errors: SM_ERR_INVALID (the store holds a key already, or a
 * load is open), SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_READONLY, SM_ERR_IO.
 */
int load_open(session_t* session, uint64_t storeid, loader_t*& out);

/*
 * Whether storeid holds no key at all, in empty: no version of
 * any kind, live, dead or deleted, of any transaction; its pages
 * (sm1::pin_page), which are no versions, aside. It is what
 * load_open requires, asked without opening anything; without a
 * lock on the store the answer may change at once.
 *
 * Errors: SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_IO.
 */
int store_empty(session_t* session, uint64_t storeid, bool& empty);

/*
 * Declare a stream of the load, numbered from 0 in the order
 * declared, in stream. With ascending, every key put to it must be
 * greater than the one before. With chained, its keys are the
 * store's chained keys (sm1::chain_insert), which a chained store
 * alone has (SM_ERR_INVALID).
 */
int load_stream(loader_t* loader, bool ascending, size_t& stream,
                bool chained = false);

/*
 * Put a key and its value into a stream. A key equal to another of
 * the load fails with SM_ERR_DUPKEY, here or at the close; one out
 * of order in an ascending stream with SM_ERR_INVALID. After a
 * failure the transaction is to be aborted.
 *
 * Errors: SM_ERR_DUPKEY, SM_ERR_INVALID, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int load_put(loader_t* loader, size_t stream, std::string_view key,
             std::string_view value);

/*
 * Close the load: sort and merge what is left, check that no two
 * keys are equal and no two streams overlap, and put every key
 * into the store at once. The loader is freed whatever the call
 * returns. The transaction then reads what it loaded, and is
 * still to commit; after a failure the store holds nothing of
 * the load, and the transaction may go on.
 *
 * Errors: SM_ERR_DUPKEY, SM_ERR_INVALID (two streams overlap),
 * SM_ERR_DISKFULL, SM_ERR_IO.
 */
int load_close(loader_t* loader);

/*
 * The newest version of key in storeid, whether or not the snapshot
 * sees it, as vacuum judges a tree's entry by its row (../BTREE.md
 * §7): 0 with its row in pin and data, or 1 if it is gone, deleted or
 * never written, when the version is settled -- committed at or below
 * the GC watermark, so every snapshot sees it and no other; 2 when it
 * is not: written by a transaction still running, this one included,
 * or committed past the watermark. A chained store's key is read as
 * sm1::chain_read reads one. Locks nothing and waits for no one.
 *
 * Errors: SM_ERR_INVALID, SM_ERR_NOTFOUND, SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int read_settled(session_t* session, uint64_t storeid, std::string_view key,
                 rawpin_t* pin, std::string_view& data);

/*
 * The newest version of key in storeid, past the snapshot, as a
 * unique tree checks a key against the rows its equal entries name
 * (../BTREE.md §6): 0 with its row in pin and data, committed or
 * this transaction's own; 1 if it is gone, deleted or never written;
 * 2 if another transaction still running wrote it, writer set to
 * that transaction, for sm1::wait_writer. A chained store's key is
 * read as sm1::chain_read reads one. Locks nothing.
 *
 * Errors: SM_ERR_INVALID, SM_ERR_NOTFOUND, SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO.
 */
int read_newest(session_t* session, uint64_t storeid, std::string_view key,
                rawpin_t* pin, std::string_view& data, uint64_t& writer);

/*
 * Wait until the transaction writer, which sm1::read_newest named,
 * ends, or rolls back to a savepoint, as a write waits for a row's
 * uncommitted version: cut short as every wait is, and in the
 * deadlock check. The caller reads the key again after.
 *
 * Errors: SM_ERR_DEADLOCK, SM_ERR_LOCKTIMEOUT, SM_ERR_INTERRUPTED,
 * SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int wait_writer(session_t* session, uint64_t writer);

/*
 * Under serializable, an SIREAD on the keys of storeid from lo to hi,
 * either nullptr for no bound, each inclusive or not, read by no scan
 * of sm1's: a range of a tree's entries, read through its pages
 * (../BTREE.md §8, SSI). A write of a key in it by a concurrent
 * transaction makes an rw-edge, as a scan's range does
 * (SM1_DESIGN.md §7.1); the range is not narrowed. Below
 * serializable, and in a safe snapshot, nothing.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int ssi_read_range(session_t* session, uint64_t storeid, const char* lo,
                   size_t lolen, int lo_inclusive, const char* hi,
                   size_t hilen, int hi_inclusive);

/*
 * Under serializable, a write of key in storeid that is no version of
 * sm1's: a tree's entry, written to its pages. An rw-edge from every
 * concurrent reader whose SIREAD covers it, as a write of a version
 * makes (SM1_DESIGN.md §7.2). Below serializable, nothing.
 *
 * Errors: SM_ERR_NOTRANS, SM_ERR_PREPARED.
 */
int ssi_write_key(session_t* session, uint64_t storeid, std::string_view key);

/*
 * Pages (../BTREE.md §2): keys of a store the level above lays out
 * as B+tree pages, apart from its versions, and none of a
 * transaction's. A page is written outside every transaction, in one
 * RocksDB write batch that commits when it is made, whatever the
 * caller's transaction then does, and is read as it is, through the
 * environment's block cache, where its bytes are an entry of their
 * own, shared by every session. A page's key is the caller's, under
 * a class of its own: no scan, read or write of versions meets it.
 * Each call needs an active transaction, only to name the store.
 */

/* The most bytes a page may be: a frame of the buffer pool. */
constexpr size_t page_size = 4096;

/* The longest key a page may have: a frame's tag holds it. */
constexpr size_t page_key_max = 16;

/* A page pinned in the buffer pool, by sm1::pin_page. */
struct page_t;

/* How a page is pinned: a pin is a latch (../BUFFERPOOL.md). */
enum class page_mode_t { sh, ex };

/* An inner page of a tree, which the clock ages after the others. */
constexpr unsigned page_inner = 1;
/* A new page: pinned ex with its frame not read. */
constexpr unsigned page_no_read = 2;

/*
 * A page to write: its pin, exclusive, its key, and its bytes, or
 * erase for none.
 */
struct page_put_t {
    page_t*          page;
    std::string_view key;
    std::string_view bytes;
    bool             erase = false;
};

/*
 * Pin the page at key in storeid: 0 with it pinned in out and its
 * bytes in bytes, valid until sm1::unpin_page(session, out), which
 * the session's closing does not do for the caller; 1 if there is
 * none, out untouched. A miss reads it from the store into the buffer
 * pool, which may evict another. flags are page_inner and
 * page_no_read: with that, ex alone, a page not in the pool is not
 * read and bytes are empty, and unless it is written before it is
 * unpinned its frame goes back unused.
 *
 * A pin is a latch, held only as long as one: shared, or exclusive,
 * which no other pin of the page shares, so its bytes never change
 * under a pin. A pin that waits may be interrupted. Pages are pinned
 * exclusive in one order, so no two pins wait on each other: a
 * level's left before right, a child before its parent
 * (../BTREE.md §16). A thread may pin a page shared again, but no
 * other second pin of a page is allowed.
 *
 * Errors: SM_ERR_INVALID (store 0, a key over page_key_max),
 * SM_ERR_NOTFOUND, SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_IO,
 * SM_ERR_CORRUPT (a page over page_size), SM_ERR_INTERRUPTED.
 */
int pin_page(session_t* session, uint64_t storeid, std::string_view key,
             page_mode_t mode, unsigned flags, page_t*& out,
             std::string_view& bytes);

/* A store's pages, as sm1::page_enter hands them out. */
struct page_store_t;

/*
 * Make sm1::pin_page's checks of a transaction and a store once, for
 * many pins: 0 with storeid's handle in out, good until the session's
 * next call that is no pin of a page, so a descent of a tree makes
 * these checks once, not once a page. Errors: as sm1::pin_page's.
 */
int page_enter(session_t* session, uint64_t storeid,
               const page_store_t*& out);

/* sm1::pin_page by storeid's handle, with no check of its own. */
int pin_page(session_t* session, uint64_t storeid, const page_store_t* store,
             std::string_view key, page_mode_t mode, unsigned flags,
             page_t*& out, std::string_view& bytes);

/* Release a page sm1::pin_page pinned; nullptr is a no-op. */
void unpin_page(session_t* session, page_t* page);

/*
 * A session's copy of a page, read with no pin while the page's frame
 * is as it was when copied: for a page every descent of a tree reads,
 * whose pin would be a word every core writes (../BTREE.md §4.2). The
 * sessions that read a page at one version share one copy. A copy is
 * as fresh as a page pinned, read and unpinned.
 */

/* The most copies a session keeps; one more drops them all. */
constexpr size_t page_copies_max = 32;

/*
 * The session's copy of the page at key in storeid: true with its
 * bytes in bytes, good until the session's next sm1::keep_page_copy,
 * if the page has not changed, been dropped or left the pool since
 * the copy, and the page's frame aged as a pin would age it; false
 * if not, or if there is no copy.
 */
bool page_copy(session_t* session, uint64_t storeid, std::string_view key,
               std::string_view& bytes);

/*
 * Keep a copy of the page at key in storeid, which the caller holds
 * pinned, as page, with bytes, for sm1::page_copy.
 */
void keep_page_copy(session_t* session, uint64_t storeid, std::string_view key,
                    page_t* page, std::string_view bytes);

/* Whether the page at key in storeid is in the pool: for tests. */
bool page_cached(session_t* session, uint64_t storeid, std::string_view key);

/* The pages pinned exclusive now, by anyone: for tests. */
size_t pages_exclusive(session_t* session);

/*
 * Write puts to storeid at once, outside every transaction, each
 * page pinned exclusive by the caller: in RocksDB's WAL, unsynced, as
 * a later commit's sync covers it. Once RocksDB has them each page is
 * copied into its frame, which cannot fail; on an error every frame
 * is as it was. Bytes may not lie in a frame being written, and what
 * a view of a page's old bytes held is over.
 *
 * Errors: SM_ERR_INVALID (a page over page_size), SM_ERR_NOTFOUND,
 * SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_IO.
 */
int page_write(session_t* session, uint64_t storeid,
               const std::vector<page_put_t>& puts);

/*
 * The bytes of storeid's pages whose keys start with prefix, RocksDB's
 * estimate, in its sorted files, disk, and in its memtables, mem.
 *
 * Errors: as sm1::page_write.
 */
int page_approxsize(session_t* session, uint64_t storeid,
                    std::string_view prefix, uint64_t& disk, uint64_t& mem);

/*
 * Remove every page of storeid whose key starts with prefix, and its
 * frame, outside every transaction, as sm1::page_write. It waits out
 * the frames' pins, so no pin of one may be held. It is
 * how a tree goes: when one is made, whatever a tree before it under
 * its relid left, and when DROP INDEX of one commits. A rollback must
 * keep a dropped tree's pages, so that drop waits for the commit, and
 * only this page call may be made with no transaction active, from an
 * sm1::on_commit function: the store is then taken as the database
 * has it, and a store gone is no error.
 *
 * Errors: as sm1::page_write.
 */
int page_drop(session_t* session, uint64_t storeid, std::string_view prefix);

/*
 * The compiler flags this level was built with, its Makefile's
 * OPTFLAGS: "-O3", or "-O3 -DNDEBUG" for digres's release build.
 */
const char* build_flags();

} // namespace sm1

#endif /* SM1_H */
