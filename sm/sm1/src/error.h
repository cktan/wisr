/*
 * error.h -- setting the calling thread's error state.
 *
 * The setters are sm0's (sm0_error.h), brought into namespace
 * wisr; set_status_error, which knows RocksDB, is sm1's own.
 * Every one records an error for sm1::errnum() and
 * sm1::errmsg() and returns -1, so an API entry point can write
 * `return set_error(...)`.
 */
#ifndef WISR_ERROR_H
#define WISR_ERROR_H

#include "sm0_error.h"
#include "sm1.h"

#include <rocksdb/status.h>

namespace wisr {

using sm0::check_name;
using sm0::copy_name;
using sm0::set_errno_error;
using sm0::set_error;
using sm0::set_error_v;

/* Record a failed RocksDB status from what. Returns -1. */
int set_status_error(const rocksdb::Status &s, const char *what);

} // namespace wisr

#endif /* WISR_ERROR_H */
