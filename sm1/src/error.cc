/*
 * error.cc -- the per-thread error state behind sm1::errnum(),
 * sm1::errmsg(), sm1::retryable(), sm1::seterr(),
 * sm1::reseterr(), and sm1::errname().
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

thread_local sm1::errnum_t t_errno = sm1::SM_OK;
thread_local char t_errmsg[errmsg_size] = "no error";

} // namespace

namespace wisr {

/* Record e and format its message into the thread's buffer. */
int set_error_v(sm1::errnum_t e, std::string_view fmt, std::format_args args) {
  // Format the message into the thread-local buffer.
  sm1::format_into(t_errmsg, sizeof(t_errmsg), fmt, args);
  // Record the error code.
  t_errno = e;
  return -1;
}

/* Map a system errno onto the sm1::errnum_t it corresponds to. */
int set_errno_error(int err, const char *what) {
  sm1::errnum_t e;
  // Classify the errno.
  switch (err) {
  case ENOENT:
    e = sm1::SM_ERR_NOTFOUND;
    break;
  case EEXIST:
    e = sm1::SM_ERR_EXISTS;
    break;
  case EACCES:
  case EPERM:
  case EROFS:
    e = sm1::SM_ERR_PERMISSION;
    break;
  case ENOSPC:
  case EDQUOT:
    e = sm1::SM_ERR_DISKFULL;
    break;
  case ENOMEM:
    e = sm1::SM_ERR_NOMEM;
    break;
  case ENOTDIR:
  case ENAMETOOLONG:
    e = sm1::SM_ERR_INVALID;
    break;
  default:
    e = sm1::SM_ERR_IO;
    break;
  }
  // Record the code with the call and the system's message.
  return set_error(e, "{}: {}", what, strerror(err));
}

/*
 * Map a RocksDB status onto an sm1::errnum_t. RocksDB reports
 * permission failures as plain IOErrors, so they surface as
 * SM_ERR_IO.
 */
int set_status_error(const rocksdb::Status &s, const char *what) {
  // Default to SM_ERR_IO, then refine by the status's kind.
  sm1::errnum_t e = sm1::SM_ERR_IO;
  if (s.IsNoSpace()) {
    e = sm1::SM_ERR_DISKFULL;
  } else if (s.IsCorruption()) {
    e = sm1::SM_ERR_CORRUPT;
  } else if (s.IsMemoryLimit()) {
    e = sm1::SM_ERR_NOMEM;
  } else if (s.IsNotFound()) {
    e = sm1::SM_ERR_NOTFOUND;
  } else if (s.IsInvalidArgument() || s.IsNotSupported()) {
    e = sm1::SM_ERR_INVALID;
  }
  // Record the code with the call and RocksDB's message.
  return set_error(e, "{}: {}", what, s.ToString().c_str());
}

/* Reject a NULL, empty, or over-long name. */
int check_name(const char *name, size_t max, const char *what) {
  // Reject a NULL name.
  if (!name) {
    return set_error(sm1::SM_ERR_INVALID, "{} name is NULL", what);
  }
  // Measure it, stopping just past the limit.
  size_t n = strnlen(name, max + 1);
  // Reject an empty or over-long name.
  if (n == 0 || n > max) {
    return set_error(sm1::SM_ERR_INVALID, "{} name must be 1..{} bytes", what,
                     max);
  }
  return 0;
}

/* Copy a name argument into out, refusing an embedded NUL. */
int copy_name(std::string_view name, const char *what, std::string &out) {
  // A NUL would end the name early in every C-string use of it.
  if (name.find('\0') != std::string_view::npos) {
    return set_error(sm1::SM_ERR_INVALID, "{} name holds a NUL byte", what);
  }
  // Copy it, terminated.
  out.assign(name);
  return 0;
}

} // namespace wisr

namespace sm1 {

/* The last error raised on this thread. */
errnum_t errnum() { return t_errno; }

/* The message for the last error raised on this thread. */
const char *errmsg() { return t_errmsg; }

/* 1 if the last error on this thread is retryable, else 0. */
int retryable() {
  // Only these three errors leave the work retryable.
  return t_errno == sm1::SM_ERR_CONFLICT ||
         t_errno == sm1::SM_ERR_SERIALIZATION ||
         t_errno == sm1::SM_ERR_DEADLOCK;
}

/* Record code and a copy of msg, cut to fit, as this thread's error. */
int seterr(errnum_t code, std::string_view msg) {
  // Copy what fits, leaving room for the terminating NUL.
  size_t n =
      msg.size() < sizeof(t_errmsg) - 1 ? msg.size() : sizeof(t_errmsg) - 1;
  memcpy(t_errmsg, msg.data(), n);
  t_errmsg[n] = '\0';
  // Record the error code.
  t_errno = code;
  return -1;
}

/* Clear this thread's error back to SM_OK and "no error". */
void reseterr() {
  // Restore the state a thread starts with.
  static constexpr char none[] = "no error";
  memcpy(t_errmsg, none, sizeof(none));
  t_errno = sm1::SM_OK;
}

/* The name of this thread's error code, without its SM_ prefix. */
const char *errname() {
  // One name per code, indexed by its stable value.
  static constexpr const char *names[] = {
      "OK",
      "ERR_INVALID",
      "ERR_NOTRANS",
      "ERR_INTRANS",
      "ERR_PREPARED",
      "ERR_NOSAVEPOINT",
      "ERR_NOTFOUND",
      "ERR_DUPKEY",
      "ERR_EXISTS",
      "ERR_INUSE",
      "ERR_CONFLICT",
      "ERR_SERIALIZATION",
      "ERR_DEADLOCK",
      "ERR_CORRUPT",
      "ERR_IO",
      "ERR_NOMEM",
      "ERR_DISKFULL",
      "ERR_PERMISSION",
      "ERR_VERSION",
      "ERR_UPDATED",
      "ERR_KEYTOOLONG",
      "ERR_EXCEPTION",
      "ERR_ROWTOOLONG",
      "ERR_BLOCKED",
      "ERR_NOTNULL",
      "ERR_UNDOLIMIT",
  };
  static_assert(sizeof(names) / sizeof(names[0]) == sm1::SM_ERR_LAST + 1,
                "every error code needs its name");
  // A value outside the table was never a code.
  size_t i = static_cast<size_t>(t_errno);
  return i < sizeof(names) / sizeof(names[0]) ? names[i] : "ERR_UNKNOWN";
}

} // namespace sm1
