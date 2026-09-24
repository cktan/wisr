/*
 * error.h -- setting the calling thread's error state.
 *
 * Every function here records an error for sm1::errnum() and
 * sm1::errmsg() and returns -1, so an API entry point can write
 * `return set_error(...)`. Nothing here clears the state: a
 * successful call leaves it untouched (sm1.h).
 */
#ifndef WISR_ERROR_H
#define WISR_ERROR_H

#include "sm1.h"

#include <stddef.h>

#include <format>
#include <string>
#include <string_view>

#include <rocksdb/status.h>

namespace wisr {

/*
 * Format fmt with args into buf, a buffer of size bytes, cutting
 * the message short if it does not fit; always NUL-terminates.
 * Allocates nothing. Returns the length written.
 */
size_t format_into(char *buf, size_t size, std::string_view fmt,
                   std::format_args args);

/* Record e with a message formatted from fmt and args. Returns -1. */
int set_error_v(sm1::errnum_t e, std::string_view fmt, std::format_args args);

/*
 * Record e with a std::format message, whose format string is
 * checked against its arguments at compile time. Returns -1.
 */
template <typename... Args>
int set_error(sm1::errnum_t e, std::format_string<Args...> fmt,
              Args &&...args) {
  return set_error_v(e, fmt.get(), std::make_format_args(args...));
}

/* Record an errno value from a system call on what. Returns -1. */
int set_errno_error(int err, const char *what);

/* Record a failed RocksDB status from what. Returns -1. */
int set_status_error(const rocksdb::Status &s, const char *what);

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

} // namespace wisr

#endif /* WISR_ERROR_H */
