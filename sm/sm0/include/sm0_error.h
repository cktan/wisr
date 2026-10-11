/*
 * sm0_error.h -- setting the calling thread's error state.
 *
 * Every function here records an error for sm0::errnum() and
 * sm0::errmsg() and returns -1, so an API entry point can write
 * `return set_error(...)`. Nothing here clears the state: a
 * successful call leaves it untouched (sm0.h).
 */
#ifndef SM0_ERROR_H
#define SM0_ERROR_H

#include "sm0.h"
#include "sm0_format.h"

#include <stddef.h>

#include <format>
#include <string>
#include <string_view>

namespace sm0 {

/* Record e with a message formatted from fmt and args. Returns -1. */
int set_error_v(errnum_t e, std::string_view fmt, std::format_args args);

/*
 * Record e with a std::format message, whose format string is
 * checked against its arguments at compile time. Returns -1.
 */
template <typename... Args>
int set_error(errnum_t e, std::format_string<Args...> fmt, Args &&...args) {
  return set_error_v(e, fmt.get(), std::make_format_args(args...));
}

/* Record an errno value from a system call on what. Returns -1. */
int set_errno_error(int err, const char *what);

/*
 * Check that name is a NUL-terminated string of 1..max bytes.
 * Returns 0, or -1 with SM_ERR_INVALID; what names the kind of
 * name in the message ("database", "store").
 */
int check_name(const char *name, size_t max, const char *what);

/*
 * Copy a name argument into out, NUL-terminated, for the engine's
 * C-string internals. Returns 0, or -1 with SM_ERR_INVALID if it
 * holds a NUL byte, which no name may; what names the kind of
 * name in the message ("database", "sequence").
 */
int copy_name(std::string_view name, const char *what, std::string &out);

} // namespace sm0

#endif /* SM0_ERROR_H */
