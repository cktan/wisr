/*
 * error.cc -- the per-thread error state behind sm_errno(),
 * sm_errmsg(), and sm_retryable().
 *
 * The message is formatted eagerly, with std::format, straight
 * into a thread-local buffer: nothing is allocated, so the SSI
 * retry loops of SM1_DESIGN.md §12.1 stay cheap. Formatting lazily
 * waits until a profile says it matters.
 */

#include "error.h"

#include <errno.h>
#include <string.h>

#include <cstddef>

namespace {

/* Large enough for any message plus a path of PATH_MAX. */
constexpr size_t errmsg_size = 4096 + 256;

thread_local sm_errno_t t_errno = SM_OK;
thread_local char t_errmsg[errmsg_size] = "no error";

/*
 * An output iterator writing into a fixed buffer and silently
 * dropping whatever does not fit, so std::vformat_to can fill
 * a stack or thread-local buffer without allocating.
 */
class bounded_out_t {
public:
  using difference_type = std::ptrdiff_t;

  /* Write into [p, end). */
  bounded_out_t(char *p, char *end) : m_p(p), m_end(end) {}

  /* Dereference to self, so `*it = c` calls operator=. */
  bounded_out_t &operator*() { return *this; }

  /* Store c if there is room; drop it otherwise. */
  bounded_out_t &operator=(char c) {
    if (m_p < m_end) {
      *m_p++ = c;
    }
    return *this;
  }

  /* Advancing is done by operator=. */
  bounded_out_t &operator++() { return *this; }

  /* Advancing is done by operator=. */
  bounded_out_t operator++(int) { return *this; }

  /* Where the next character would go. */
  char *pos() const { return m_p; }

private:
  char *m_p;
  char *m_end;
};

} // namespace

namespace wisr {

/* Format into buf through a bounded iterator, then terminate it. */
size_t format_into(char *buf, size_t size, std::string_view fmt,
                   std::format_args args) {
  // Leave room for the terminating NUL.
  bounded_out_t out(buf, buf + size - 1);
  // Format, dropping whatever does not fit.
  char *end = std::vformat_to(out, fmt, args).pos();
  // Terminate the message.
  *end = '\0';
  return end - buf;
}

/* Record e and format its message into the thread's buffer. */
int set_error_v(sm_errno_t e, std::string_view fmt, std::format_args args) {
  // Format the message into the thread-local buffer.
  format_into(t_errmsg, sizeof(t_errmsg), fmt, args);
  // Record the error code.
  t_errno = e;
  return -1;
}

/* Map a system errno onto the sm_errno_t it corresponds to. */
int set_errno_error(int err, const char *what) {
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
  return set_error(e, "{}: {}", what, strerror(err));
}

/*
 * Map a RocksDB status onto an sm_errno_t. RocksDB reports
 * permission failures as plain IOErrors, so they surface as
 * SM_ERR_IO.
 */
int set_status_error(const rocksdb::Status &s, const char *what) {
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
  return set_error(e, "{}: {}", what, s.ToString().c_str());
}

/* Reject a NULL, empty, or over-long name. */
int check_name(const char *name, size_t max, const char *what) {
  // Reject a NULL name.
  if (!name) {
    return set_error(SM_ERR_INVALID, "{} name is NULL", what);
  }
  // Measure it, stopping just past the limit.
  size_t n = strnlen(name, max + 1);
  // Reject an empty or over-long name.
  if (n == 0 || n > max) {
    return set_error(SM_ERR_INVALID, "{} name must be 1..{} bytes", what, max);
  }
  return 0;
}

} // namespace wisr

/* The last error raised on this thread. */
sm_errno_t sm_errno(void) { return t_errno; }

/* The message for the last error raised on this thread. */
const char *sm_errmsg(void) { return t_errmsg; }

/* 1 if the last error on this thread is retryable, else 0. */
int sm_retryable(void) {
  // Only these three errors leave the work retryable.
  return t_errno == SM_ERR_CONFLICT || t_errno == SM_ERR_SERIALIZATION ||
         t_errno == SM_ERR_DEADLOCK;
}
