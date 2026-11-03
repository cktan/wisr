/*
 * log.cc -- the process-wide log hook (sm0::set_log) and the
 * helper the stack reports through.
 *
 * One mutex guards the hook and serializes calls to it, so a
 * hook may be replaced at any time and never runs twice at
 * once. fatal_v waits for that mutex only for a bounded time,
 * and never in a thread that is in the hook, so a stuck or
 * re-entered hook cannot stop the abort. An atomic flag lets log_msg skip
 * formatting entirely when no hook is installed, the common case. Messages are
 * formatted with std::format into a stack buffer.
 */

#include "sm0_log.h"

#include "sm0_format.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

/* The hook, its argument, and the mutex guarding both: timed, so
 * fatal_v can give up on a hook that does not return. */
std::timed_mutex g_log_mu;
sm0::log_fn g_log_fn = nullptr;
void *g_log_arg = nullptr;

/* Whether a hook is set; read without the mutex. */
std::atomic<bool> g_log_on{false};

/* Whether this thread is in the hook, and so holds g_log_mu. */
thread_local bool t_in_hook = false;

/* How long fatal_v waits for another thread's hook to return. */
constexpr std::chrono::seconds fatal_hook_wait{1};

} // namespace

namespace sm0 {

/* Whether a hook is installed. */
bool log_enabled() { return g_log_on.load(std::memory_order_relaxed); }

/* Format into a local buffer, then call the hook under its mutex. */
void log_v(loglevel_t level, std::string_view fmt, std::format_args args) {
  char buf[1024];
  // Format the message, cut short if it does not fit.
  format_into(buf, sizeof(buf), fmt, args);
  // Call the hook one at a time, under its mutex.
  std::lock_guard<std::timed_mutex> guard(g_log_mu);
  // The hook may have been removed since the caller checked.
  if (g_log_fn) {
    // Mark the thread as in the hook, for fatal_v.
    t_in_hook = true;
    g_log_fn(g_log_arg, level, buf);
    t_in_hook = false;
  }
}

/*
 * Report a fatal error and abort, for a state no caller can act on.
 * 1. Format "*** FATAL *** " and the message into a stack buffer.
 * 2. Write it to stderr, whatever the log hook does, and flush.
 * 3. Hand it to the log hook, if one is set, unless this thread
 *    is in the hook, or another thread's hook does not return
 *    within fatal_hook_wait.
 * 4. Abort the process.
 */
[[noreturn]] void fatal_v(std::string_view fmt, std::format_args args) {
  static constexpr char prefix[] = "*** FATAL *** ";
  constexpr size_t plen = sizeof(prefix) - 1;
  char buf[1024];
  memcpy(buf, prefix, plen);
  // Format the message after the prefix, cut short if it does not fit.
  format_into(buf + plen, sizeof(buf) - plen, fmt, args);
  // Tell stderr, whatever the log hook does.
  fprintf(stderr, "%s\n", buf);
  fflush(stderr);
  // Tell the log hook, at its most severe level. Skip it in a hook
  // that failed, which holds the mutex already, and give up on a
  // hook stuck in another thread: stderr has the message.
  if (!t_in_hook && g_log_mu.try_lock_for(fatal_hook_wait)) {
    if (g_log_fn) {
      t_in_hook = true;
      g_log_fn(g_log_arg, loglevel_t::error, buf);
    }
    g_log_mu.unlock();
  }
  // Abort: the process's state can no longer be trusted.
  abort();
}

/* Install or remove the process-wide log hook. */
void set_log(log_fn fn, void *arg) {
  // Swap the hook under its mutex, so no call sees half of it.
  std::lock_guard<std::timed_mutex> guard(g_log_mu);
  g_log_fn = fn;
  g_log_arg = arg;
  // Publish whether a hook is set.
  g_log_on.store(fn != nullptr);
}

} // namespace sm0
