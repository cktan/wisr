/*
 * sm0_log.h -- reporting through the caller's log hook
 * (sm0::set_log).
 */
#ifndef SM0_LOG_H
#define SM0_LOG_H

#include "sm0.h"

#include <format>
#include <string_view>

namespace sm0 {

/* Whether a log hook is installed; one atomic load. */
bool log_enabled();

/* Format fmt with args and hand the message to the log hook. */
void log_v(loglevel_t level, std::string_view fmt, std::format_args args);

/*
 * Format a std::format message, checked at compile time, and
 * hand it to the log hook, if one is set. With no hook this
 * costs one atomic load and formats nothing.
 */
template <typename... Args>
void log_msg(loglevel_t level, std::format_string<Args...> fmt,
             Args &&...args) {
  // No hook: format nothing.
  if (!log_enabled()) {
    return;
  }
  log_v(level, fmt.get(), std::make_format_args(args...));
}

/*
 * Report a fatal error, "*** FATAL *** " and fmt formatted with
 * args, on stderr and to the log hook, then abort the process.
 * Allocates nothing, so an allocation's failure may call it. A
 * hook may call it too: the hook is then skipped, as it is when
 * another thread's hook does not return within a second.
 */
[[noreturn]] void fatal_v(std::string_view fmt, std::format_args args);

/*
 * Report a std::format message, checked at compile time, as a
 * fatal error, and abort the process (fatal_v).
 */
template <typename... Args>
[[noreturn]] void fatal(std::format_string<Args...> fmt, Args &&...args) {
  fatal_v(fmt.get(), std::make_format_args(args...));
}

} // namespace sm0

#endif /* SM0_LOG_H */
