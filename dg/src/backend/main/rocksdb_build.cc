/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * rocksdb_build.cc -- what "digres --build" says of the RocksDB the server
 * links: its version, from the headers it was built against, and
 * DG_ROCKSDB_BUILD, which the Makefile's link step gives: the system's
 * shared library, or one test/rocksdb.sh built, and its flags.  It is
 * compiled at each link, not with the server's other objects.
 */
#include <rocksdb/version.h>

#define DG_STR2(x) #x
#define DG_STR(x) DG_STR2(x)

#ifndef DG_ROCKSDB_BUILD
#define DG_ROCKSDB_BUILD "unknown"
#endif

extern "C" const char dg_rocksdb_build[] =
    DG_STR(ROCKSDB_MAJOR) "." DG_STR(ROCKSDB_MINOR) "." DG_STR(
        ROCKSDB_PATCH) ", " DG_ROCKSDB_BUILD;
