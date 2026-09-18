/*
 * env.h -- the environment: one process's handle on a home
 * directory (DESIGN.md §3.5).
 */
#ifndef WISR_ENV_H
#define WISR_ENV_H

#include "sm.h"

#include <stdint.h>

#include <mutex>
#include <string>

#include <rocksdb/db.h>

/* Home directory format version, stored as "M" | "version". */
#define WISR_HOME_VERSION 1

/* Root db keys (DESIGN.md §3.5). */
#define WISR_ROOT_DBNAME_PREFIX "D"
#define WISR_ROOT_NEXT_DBID     "Mnext_dbid"
#define WISR_ROOT_VERSION       "Mversion"

struct sm_env_t {
    std::string homedir;
    int lock_fd = -1;            /* holds the flock on pidlock.pid */
    rocksdb::DB* root = nullptr; /* {homedir}/global/root */
    std::mutex mu;               /* serializes root db updates */

    /* Close the root db, then release the pidlock. */
    ~sm_env_t();
};

namespace wisr
{

/* The directory of database dbid: {homedir}/base/{dbid}. */
std::string env_db_path(const sm_env_t* env, uint64_t dbid);

/*
 * Remove a database directory: DestroyDB for RocksDB's files,
 * then whatever else is left. Returns 0 or -1 with the error
 * set.
 */
int env_destroy_db_dir(const std::string& path);

} // namespace wisr

#endif /* WISR_ENV_H */
