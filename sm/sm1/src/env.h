/*
 * env.h -- the environment: one process's handle on a home
 * directory (SM1_DESIGN.md §3.5).
 */
#ifndef WISR_ENV_H
#define WISR_ENV_H

#include "sm1.h"

#include <stdint.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rocksdb/db.h>

namespace wisr {
struct db_t;
}

/* Home directory format version, stored as "M" | "version". */
#define WISR_HOME_VERSION 1

/* Root db keys (SM1_DESIGN.md §3.5). */
#define WISR_ROOT_DBNAME_PREFIX "D"
#define WISR_ROOT_NEXT_DBID "Mnext_dbid"
#define WISR_ROOT_VERSION "Mversion"

struct sm1::env_t {
  std::string homedir;
  rocksdb::DB *root = nullptr; /* {homedir}/global/root */
  std::mutex mu;               /* guards the root db and the two sets below */

  /* Open databases by name, shared by their sessions. */
  std::unordered_map<std::string, wisr::db_t *> dbs;

  /* Every open session, for sm1::detach. */
  std::unordered_set<sm1::session_t *> sessions;

  /* Close the root db. */
  ~env_t();
};

namespace wisr {

/* The directory of database dbid: {homedir}/base/{dbid}. */
std::string env_db_path(const sm1::env_t *env, uint64_t dbid);

/*
 * Remove a database directory: DestroyDB for RocksDB's files,
 * then whatever else is left. Returns 0 or -1 with the error
 * set.
 */
int env_destroy_db_dir(const std::string &path);

} // namespace wisr

#endif /* WISR_ENV_H */
