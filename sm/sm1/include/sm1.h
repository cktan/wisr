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
 *     error. Exactly two, sm1::read() and sm1::scannxt(), also
 *     return 1 to mean "no row", which is not an error.
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
 * The last error raised on the calling thread. Meaningful
 * only immediately after a call that returned an error
 * indication: a successful call leaves it untouched, so it
 * cannot be used to test for success.
 */
errnum_t errnum();

/*
 * The message for that error: NUL-terminated, owned by the
 * library, never NULL, and valid until the next failing call on
 * this thread.
 */
const char* errmsg();

/*
 * 1 if the last error on this thread is retryable, else 0.
 *
 * SM_ERR_CONFLICT, SM_ERR_SERIALIZATION, and SM_ERR_DEADLOCK
 * mean the transaction cannot proceed but the same work may
 * succeed on a fresh try. On any of them the engine has
 * ALREADY aborted the transaction: do not call sm1::abort(), do
 * not issue further data operations, just sm1::begin() again and
 * redo the work.
 *
 * Test with this rather than against the three codes: a
 * retryable code added later is appended to the enum like any
 * other, so they are not guaranteed to stay contiguous.
 *
 * Every other error leaves the transaction active and
 * unchanged. The two exceptions are sm1::commit(), after which
 * the transaction is never still active, and sm1::prepare() on
 * SM_ERR_IO.
 */
int retryable();

/*
 * Record code and a copy of msg as the calling thread's error.
 * Any level of the stack reports through it. msg is cut short if
 * it does not fit the message buffer. Returns -1, so a failing
 * path is one line: return sm1::seterr(sm1::SM_ERR_..., "...");
 */
int seterr(errnum_t code, std::string_view msg);

/*
 * Clear the calling thread's error back to what a thread starts
 * with: SM_OK, and the message "no error". A successful call
 * leaves the state untouched, so this is the one way to start
 * a sequence of calls from a known state.
 */
void reseterr();

/*
 * The name of the current error code, without its SM_ prefix:
 * "ERR_UPDATED" for SM_ERR_UPDATED, "OK" for SM_OK. Static and
 * never NULL.
 */
const char* errname();

/*
 * Log levels for the hook below.
 */
enum class loglevel_t {
    error = 0,   /* something failed and nobody was told */
    warn  = 1,   /* something needs an operator's attention */
    info  = 2    /* a notable event, such as recovery */
};

/*
 * A log hook: receives level and a NUL-terminated message
 * valid only for the duration of the call, plus the arg given
 * to sm1::set_log().
 */
using log_fn = void (*)(void* arg, loglevel_t level, const char* msg);

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
 * sm1::attach() and recovery clean up or restore. Errors a call
 * returns are not logged; its caller already has them.
 *
 * fn may be called from any thread, wisr's own included, one
 * call at a time. It must not call back into wisr: calls are
 * made with a library lock held.
 */
void set_log(log_fn fn, void* arg);

/*
 * An environment: one process's handle on a home directory,
 * through which every database and session call goes.
 */
struct env_t;

/* A session: one caller's connection to one database. */
struct session_t;

/* A scan cursor over a range of one store. */
struct scan_t;

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
 * already prepared. Opens no database. The caller must ensure
 * no other environment, in this process or another, has the
 * home directory attached (see "Processes" above).
 *
 * Creates nothing: a homedir that was never initialized is an
 * error, not a new home directory. Returns 0 with the
 * environment in *out, or -1 on error.
 *
 * Unless the process already has one, installs a new-handler
 * (std::set_new_handler) that makes running out of memory inside
 * sm1 fatal: it reports "*** FATAL ***" on stderr and to the log
 * hook, then aborts. A bad_alloc from inside RocksDB leaves it
 * unreliable, and only a fresh open's WAL replay recovers safely.
 * Outside sm1 the handler throws std::bad_alloc, for the levels
 * above to report as SM_ERR_NOMEM. A handler the process already
 * has is kept, but one that throws is unsupported.
 */
int attach(std::string_view homedir, env_t*& out);

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
 * Errors: SM_ERR_EXISTS, SM_ERR_NOTRANS, SM_ERR_CONFLICT
 * (retryable -- concurrently created), SM_ERR_INVALID,
 * SM_ERR_PREPARED, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int mkseq(session_t* session, std::string_view name,
          uint64_t start, uint64_t increment);

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
 * SM_ERR_PREPARED, SM_ERR_CORRUPT, SM_ERR_IO (the database has
 * stopped, or persisting the next block failed),
 * SM_ERR_DISKFULL.
 */
int nextseq(session_t* session, std::string_view name,
            uint64_t& out);

/*
 * Report the last value the sequence handed out in out, drawing
 * none: no lease is extended and nothing is written.
 *
 * Exact once the sequence has handed out a value in this process.
 * Before that it is the value below the block reserved last, an
 * upper bound, since a restart skips the rest of that block.
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
 * session on a database closes the database.
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
 * Cut short the session's wait for another transaction: the wait
 * under way, on a lock or on a row another is writing, fails with
 * SM_ERR_CANCELLED. That is not an abort and not retryable: the
 * transaction stays active, holding what it held, and the caller
 * decides whether to abort it. This is what a host's query cancel,
 * statement timeout and lock timeout are made of.
 *
 * It is the one call on a session that another thread may make
 * while the session's own thread is inside a call. The session
 * must stay open until it returns.
 *
 * A cancelled statement stays cancelled. The cancel is not used
 * up by the wait it ends: every wait the session makes fails the
 * same way, one begun later included, until the session begins its
 * next statement (sm1::stmt_begin) or its next transaction
 * (sm1::begin). So a cancel sent just before a wait begins is not
 * lost, a caller that goes on after SM_ERR_CANCELLED cannot wait
 * again by mistake, and nothing has to be cleared by hand.
 *
 * Errors: SM_ERR_INVALID (no session).
 */
int cancel(session_t* session);

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
 * What a lock names, which decides whether it touches data at
 * all (LOCK.md). The values are persisted in a prepared
 * transaction's lock set, so they never change.
 *
 *   LOCKTYPE_ROW     a key in a store. The lock meets the
 *                    write path: it waits for an uncommitted
 *                    writer of the key, and once held it runs
 *                    the currency check below. storeid and key
 *                    must name a live store and a legal key.
 *   LOCKTYPE_OID     an object id in no store, which nothing
 *                    ever writes -- what a level above locks to
 *                    stand for an object of its own. The key
 *                    must be exactly 8 bytes, an oid
 *                    big-endian; no store is read, no version
 *                    is checked, and storeid is part of the
 *                    name and nothing more, so pass 0 unless
 *                    the name needs it. sm3 locks a table's
 *                    definition this way, and every sm3 object
 *                    draws its oid from one sequence, so one
 *                    type serves them all.
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
 * and FOR UPDATE. The key need not exist. With locktype_t
 * other than row it locks a name instead, and the paragraphs
 * below about the store, the uncommitted writer and the
 * currency check do not apply.
 *
 * The call BLOCKS while another transaction holds the lock in
 * a conflicting mode (lockmode_t's table), exactly as a write
 * does, and it can fail with SM_ERR_DEADLOCK for the same
 * reason. A transaction that already holds the row and asks for
 * a mode it does not cover -- including by writing the row --
 * converts to the weakest mode covering both, waiting for the
 * other holders that conflict with it; two transactions
 * converting on the same row deadlock, and the younger one is
 * the victim.
 *
 * Once the lock is held the call runs the same check a write
 * does: if another transaction committed a version of this key
 * after this transaction's snapshot, it fails with
 * SM_ERR_CONFLICT. So a successful sm1::lock means what the
 * caller reads next under that lock is current, which is what
 * makes "lock, then read, then act" safe.
 *
 * The lock is held to the end of the transaction, or until an
 * sm1::rollback() past the point it was taken. A conversion is
 * never undone by a rollback: the row stays locked in the
 * stronger mode until the transaction ends.
 *
 * A transaction holding locks it took with sm1::lock may be
 * prepared; the locks survive in the prepared transaction, each
 * in its mode, and are restored after a crash, so nothing
 * conflicting may take those rows until a coordinator resolves
 * it.
 *
 * Errors: SM_ERR_CONFLICT / SM_ERR_DEADLOCK (retryable),
 * SM_ERR_BLOCKED (under sm1::set_qa_nowait), SM_ERR_NOTFOUND
 * (store), SM_ERR_NOTRANS, SM_ERR_PREPARED, SM_ERR_INVALID.
 */
int lock(session_t* session, locktype_t type, uint64_t storeid,
         std::string_view key, lockmode_t mode);

/*
 * Read the newest committed version of a key, ignoring the
 * snapshot, under a row lock this transaction already holds, or
 * with a write of the key by this transaction still in effect,
 * which every other writer of it waits for.
 * Returns as sm1::read() does: 0 found, 1 no such key. At read
 * committed only. Under an exclusive lock it also lets the next
 * write of the key through, after SM_ERR_UPDATED (SM1_DESIGN.md
 * §5.4).
 *
 * Errors: SM_ERR_INVALID (not read committed, or neither a lock
 * held nor a write of the key), SM_ERR_NOTRANS, SM_ERR_PREPARED,
 * SM_ERR_NOTFOUND (store).
 */
int read_latest(session_t* session, uint64_t storeid,
                std::string_view key, std::string_view& data);

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
 * FOR QA ONLY: it lets one thread play several sessions in a
 * test, and is not how a caller asks for NOWAIT or SKIP LOCKED.
 * Those are to be explicit flags on the calls that can wait.
 *
 * Set whether the session waits for locks. Off, the default, a
 * write or an sm1::lock() BLOCKS while another transaction holds
 * the lock in a conflicting mode. On, such a call fails at once
 * with SM_ERR_BLOCKED instead. That is not an abort and it is
 * not retryable: the transaction stays active and holds exactly
 * what it held before, and the caller polls by repeating the
 * call, which fails the same way or goes through. The setting
 * lasts until it is changed, across transactions.
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
 * (the stack is full).
 */
int savepoint(session_t* session);

/*
 * Undo every write made since the savepoint at depth. That
 * savepoint survives, so the same mark may be rolled back to
 * more than once, and the savepoints set inside it are gone.
 * Locks taken since the mark are released, locks held from
 * before it are kept, and the transaction stays active.
 *
 * Scan cursors opened AFTER the savepoint are closed, and
 * their sm1::scan_t handles must not be used again; one opened
 * before it stays open and keeps its position. A cursor's view
 * is fixed when it is created, so a later one would otherwise
 * go on returning rows this call just undid.
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
 * rollback to one fails with SM_ERR_UNDOLIMIT, changes
 * nothing, and leaves the transaction to be aborted. A
 * savepoint set afterwards works as usual.
 *
 * Errors: SM_ERR_NOSAVEPOINT (no savepoint at that depth),
 * SM_ERR_UNDOLIMIT, SM_ERR_NOTRANS, SM_ERR_PREPARED.
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
 * On SM_ERR_IO the outcome is UNKNOWN: the prepared record may
 * or may not have reached the log. The transaction is no
 * longer active, and the database enters the same failed state
 * as after SM_ERR_IO from sm1::commit(). After a reopen the
 * transaction is either listed by sm1::list_prepared() under
 * gid, or was never prepared and is aborted. Since the
 * participant never voted yes, sm1::abort_prepared() is always a
 * correct resolution.
 *
 * Errors: SM_ERR_SERIALIZATION (retryable; the transaction is
 * aborted and NOT prepared), SM_ERR_NOTRANS, SM_ERR_EXISTS
 * (gid in use), SM_ERR_INVALID, SM_ERR_IO (outcome unknown).
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
 * Errors: SM_ERR_NOTRANS, SM_ERR_CONFLICT (retryable),
 * SM_ERR_INVALID, SM_ERR_PREPARED, SM_ERR_DISKFULL,
 * SM_ERR_IO.
 */
int mkstore(session_t* session, uint64_t& storeid);

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
 * mem is RocksDB's estimate from sampling its memtables: 0 for an
 * empty range, within about 2x for a large one, and noise for a
 * small one, measured both at 0 and above a range far larger.
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
 * sm1::lock(), until that transaction ends; a blocked call can
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
 * asking for it costs a copy, not another seek.
 *
 * *old views session-owned memory and is valid only until the
 * next call on this session -- the same rule as sm1::read(), and
 * the same buffer, so either call invalidates what the other
 * handed out. Copy it to keep it. On failure *old is empty.
 *
 * Errors for all three: SM_ERR_CONFLICT / SM_ERR_DEADLOCK
 * (retryable), SM_ERR_NOTFOUND (store, or key for
 * update/delete), SM_ERR_DUPKEY (insert), SM_ERR_NOTRANS,
 * SM_ERR_PREPARED, SM_ERR_INVALID, SM_ERR_DISKFULL, SM_ERR_IO.
 */
int insert(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data);

int update(session_t* session, uint64_t storeid,
           std::string_view key, std::string_view data,
           std::string_view* old = nullptr);

int erase(session_t* session, uint64_t storeid,
          std::string_view key, std::string_view* old = nullptr);

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
 * sm1::commit(), sm1::abort(), sm1::closedb(), and an sm1::rollback()
 * to a savepoint older than it. Multiple cursors may be open
 * on one session at once.
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

} // namespace sm1

#endif /* SM1_H */
