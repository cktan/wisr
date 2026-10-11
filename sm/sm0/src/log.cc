/*
 * log.cc -- the process-wide log hook (sm0::set_log) and the
 * helper the stack reports through.
 *
 * One mutex guards the hook and serializes calls to it, so a
 * hook may be replaced at any time and never runs twice at
 * once. An atomic flag lets log_msg skip formatting entirely
 * when no hook is installed, the common case. Messages are
 * formatted with std::format into a stack buffer.
 */

#include "sm0_log.h"

#include "sm0_format.h"

#include <atomic>
#include <mutex>

namespace {

/* The hook, its argument, and the mutex guarding both. */
std::mutex g_log_mu;
sm0::log_fn g_log_fn = nullptr;
void *g_log_arg = nullptr;

/* Whether a hook is set; read without the mutex. */
std::atomic<bool> g_log_on{false};

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
  std::lock_guard<std::mutex> guard(g_log_mu);
  // The hook may have been removed since the caller checked.
  if (g_log_fn) {
    g_log_fn(g_log_arg, level, buf);
  }
}

/* Install or remove the process-wide log hook. */
void set_log(log_fn fn, void *arg) {
  // Swap the hook under its mutex, so no call sees half of it.
  std::lock_guard<std::mutex> guard(g_log_mu);
  g_log_fn = fn;
  g_log_arg = arg;
  // Publish whether a hook is set.
  g_log_on.store(fn != nullptr);
}

} // namespace sm0
