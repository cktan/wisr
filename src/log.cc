/*
 * log.cc -- the process-wide log hook (sm_set_log) and the
 * helper the engine reports through.
 *
 * One mutex guards the hook and serializes calls to it, so a
 * hook may be replaced at any time and never runs twice at
 * once. An atomic flag lets log_msg skip formatting entirely
 * when no hook is installed, the common case.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include <atomic>
#include <mutex>

namespace {

/* The hook, its argument, and the mutex guarding both. */
std::mutex g_log_mu;
sm_log_fn g_log_fn = nullptr;
void *g_log_arg = nullptr;

/* Whether a hook is set; read without the mutex. */
std::atomic<bool> g_log_on{false};

} // namespace

namespace wisr {

/* Format into a local buffer, then call the hook under its mutex. */
void log_msg(sm_loglevel_t level, const char *fmt, ...) {
  // No hook: format nothing.
  if (!g_log_on.load(std::memory_order_relaxed)) {
    return;
  }
  char buf[1024];
  va_list ap;
  // Format the message.
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  // Call the hook one at a time, under its mutex.
  std::lock_guard<std::mutex> guard(g_log_mu);
  // The hook may have been removed since the check above.
  if (g_log_fn) {
    g_log_fn(g_log_arg, level, buf);
  }
}

} // namespace wisr

/* Install or remove the process-wide log hook. */
void sm_set_log(sm_log_fn fn, void *arg) {
  // Swap the hook under its mutex, so no call sees half of it.
  std::lock_guard<std::mutex> guard(g_log_mu);
  g_log_fn = fn;
  g_log_arg = arg;
  // Publish whether a hook is set.
  g_log_on.store(fn != nullptr);
}
