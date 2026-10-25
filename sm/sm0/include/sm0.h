/*
 * sm0.h -- level 0 of the stack: what every level above needs
 * and that is about no storage. The calling thread's error
 * state and the process-wide log hook, in namespace sm0.
 *
 * sm1.h brings each of these into namespace sm1, so the stack
 * and its callers name them sm1::errnum(), sm1::set_log() and
 * so on; ../../sm1/API.md documents them there.
 */
#ifndef SM0_H
#define SM0_H

#include "sm_error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace sm0 {

/*
 * Error codes, one namespace for the whole stack: every level's
 * codes are defined in the root's include/sm_error.h, as a C
 * enum the C wrapper shares. errnum_t names it here, and the
 * using-enum brings its SM_* values into namespace sm0.
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
const char *errmsg();

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
 * Where an SM_ERR_DUPKEY met its key, as far as the level that
 * raised it knows: the unique index's oid, 0 if not known, and the
 * key of a row that holds it in its table, past the relid, empty
 * if not known (../../ROWKEY.md §2.5): a host reads the row by it
 * to say the key.
 */
struct dupkey_t {
  uint64_t index = 0;
  std::string row;
};

/*
 * What the calling thread's last error knows of a duplicate key:
 * all zero unless setdup() was called after the error was set.
 * Valid until the thread's next error.
 */
const dupkey_t &errdup();

/*
 * Record d with the calling thread's error, the SM_ERR_DUPKEY
 * just set; a level above may call it again to add what it
 * knows. Setting the next error, or reseterr(), clears it.
 */
void setdup(const dupkey_t &d);

/*
 * The name of the current error code, without its SM_ prefix:
 * "ERR_UPDATED" for SM_ERR_UPDATED, "OK" for SM_OK. Static and
 * never NULL.
 */
const char *errname();

/*
 * Log levels for the hook below.
 */
enum class loglevel_t {
  error = 0, /* something failed and nobody was told */
  warn = 1,  /* something needs an operator's attention */
  info = 2   /* a notable event, such as recovery */
};

/*
 * A log hook: receives level and a NUL-terminated message
 * valid only for the duration of the call, plus the arg given
 * to sm1::set_log(). It is noexcept: it is called from wisr's
 * own threads and with a library lock held, where an error has
 * nowhere to go, so a hook that throws ends the process.
 */
using log_fn = void (*)(void *arg, loglevel_t level, const char *msg) noexcept;

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
void set_log(log_fn fn, void *arg);

/*
 * The compiler flags this level was built with, its Makefile's
 * OPTFLAGS: "-O3", or "-O3 -DNDEBUG" for digres's release build.
 */
const char *build_flags();

} // namespace sm0

#endif /* SM0_H */
