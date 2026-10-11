/*
 * error.cc -- set_status_error: a failed RocksDB status as the
 * calling thread's error. The error state itself is sm0's.
 */

#include "error.h"

namespace wisr {

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

} // namespace wisr
