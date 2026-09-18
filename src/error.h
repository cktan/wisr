/*
 * error.h -- setting the calling thread's error state.
 *
 * Every function here records an error for sm_errno() and
 * sm_errmsg() and returns -1, so an API entry point can write
 * `return set_error(...)`. Nothing here clears the state: a
 * successful call leaves it untouched (sm.h).
 */
#ifndef WISR_ERROR_H
#define WISR_ERROR_H

#include "sm.h"

#include <rocksdb/status.h>

namespace wisr
{

/* Record e with a printf-style message. Returns -1. */
int set_error(sm_errno_t e, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Record an errno value from a system call on what. Returns -1. */
int set_errno_error(int err, const char* what);

/* Record a failed RocksDB status from what. Returns -1. */
int set_status_error(const rocksdb::Status& s, const char* what);

/*
 * Check that name is a NUL-terminated string of 1..max bytes.
 * Returns 0, or -1 with SM_ERR_INVALID; what names the kind of
 * name in the message ("database", "relation").
 */
int check_name(const char* name, size_t max, const char* what);

} // namespace wisr

#endif /* WISR_ERROR_H */
