/*
 * error.cc -- the per-thread error state behind sm_errno(),
 * sm_errmsg(), and sm_retryable().
 *
 * The message is formatted eagerly. DESIGN.md §12.1 suggests
 * formatting lazily for SSI retry loops; that waits until SSI
 * exists and a profile says it matters.
 */

#include "error.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{

/* Large enough for any message plus a path of PATH_MAX. */
constexpr size_t errmsg_size = 4096 + 256;

thread_local sm_errno_t t_errno = SM_OK;
thread_local char t_errmsg[errmsg_size] = "no error";

} // namespace

namespace wisr
{

/* Record e and format its message into the thread's buffer. */
int set_error(sm_errno_t e, const char* fmt, ...)
{
    va_list ap;
    // Format the message into the thread-local buffer.
    va_start(ap, fmt);
    vsnprintf(t_errmsg, sizeof(t_errmsg), fmt, ap);
    va_end(ap);
    // Record the error code.
    t_errno = e;
    return -1;
}

/* Map a system errno onto the sm_errno_t it corresponds to. */
int set_errno_error(int err, const char* what)
{
    sm_errno_t e;
    // Classify the errno.
    switch (err) {
    case ENOENT:
        e = SM_ERR_NOTFOUND;
        break;
    case EEXIST:
        e = SM_ERR_EXISTS;
        break;
    case EACCES:
    case EPERM:
    case EROFS:
        e = SM_ERR_PERMISSION;
        break;
    case ENOSPC:
    case EDQUOT:
        e = SM_ERR_DISKFULL;
        break;
    case ENOMEM:
        e = SM_ERR_NOMEM;
        break;
    case ENOTDIR:
    case ENAMETOOLONG:
        e = SM_ERR_INVALID;
        break;
    default:
        e = SM_ERR_IO;
        break;
    }
    // Record the code with the call and the system's message.
    return set_error(e, "%s: %s", what, strerror(err));
}

/*
 * Map a RocksDB status onto an sm_errno_t. RocksDB reports
 * permission failures as plain IOErrors, so they surface as
 * SM_ERR_IO.
 */
int set_status_error(const rocksdb::Status& s, const char* what)
{
    // Default to SM_ERR_IO, then refine by the status's kind.
    sm_errno_t e = SM_ERR_IO;
    if (s.IsNoSpace()) {
        e = SM_ERR_DISKFULL;
    } else if (s.IsCorruption()) {
        e = SM_ERR_CORRUPT;
    } else if (s.IsMemoryLimit()) {
        e = SM_ERR_NOMEM;
    } else if (s.IsNotFound()) {
        e = SM_ERR_NOTFOUND;
    } else if (s.IsInvalidArgument() || s.IsNotSupported()) {
        e = SM_ERR_INVALID;
    }
    // Record the code with the call and RocksDB's message.
    return set_error(e, "%s: %s", what, s.ToString().c_str());
}

/* Reject a NULL, empty, or over-long name. */
int check_name(const char* name, size_t max, const char* what)
{
    // Reject a NULL name.
    if (!name) {
        return set_error(SM_ERR_INVALID, "%s name is NULL", what);
    }
    // Measure it, stopping just past the limit.
    size_t n = strnlen(name, max + 1);
    // Reject an empty or over-long name.
    if (n == 0 || n > max) {
        return set_error(SM_ERR_INVALID, "%s name must be 1..%zu bytes", what,
                         max);
    }
    return 0;
}

} // namespace wisr

/* The last error raised on this thread. */
sm_errno_t sm_errno(void)
{
    return t_errno;
}

/* The message for the last error raised on this thread. */
const char* sm_errmsg(void)
{
    return t_errmsg;
}

/* 1 if the last error on this thread is retryable, else 0. */
int sm_retryable(void)
{
    // Only these three errors leave the work retryable.
    return t_errno == SM_ERR_CONFLICT || t_errno == SM_ERR_SERIALIZATION ||
           t_errno == SM_ERR_DEADLOCK;
}
