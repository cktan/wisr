/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * env.h -- the environment: one process's handle on a home
 * directory (../DBMGR.md §4).
 */
#ifndef WISR_ENV_H
#define WISR_ENV_H

#include "bufpool.h"
#include "sm1.h"

#include <stdint.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>

#include "memcount.h"

namespace wisr {
struct db_t;
}

/* Home directory format version, stored as "M" | "version". */
#define WISR_HOME_VERSION 1

/* Root db keys (../DBMGR.md §4). */
#define WISR_ROOT_DBNAME_PREFIX "D"
#define WISR_ROOT_NEXT_DBID "Mnext_dbid"
#define WISR_ROOT_VERSION "Mversion"
/*
 * The size each database's new log is zero-filled to, 0 for none: 8
 * bytes big-endian, decided by the home's first attach
 * (../DBMGR.md).
 */
#define WISR_ROOT_WAL_PREALLOC "Mwal_prealloc"

struct sm1::env_t {
  std::string homedir;
  /*
   * The block cache every instance below and every database's
   * reads through (env_options_t), made by attach.
   */
  std::shared_ptr<rocksdb::Cache> block_cache;
  /*
   * The pages of every database's trees (../BUFMGR.md), a
   * quarter of block_cache_bytes, made by attach.
   */
  std::unique_ptr<wisr::bufpool_t> bufpool;
  /*
   * The Env every database's instance writes through, its new logs
   * zero-filled (walfs.h, ../DBMGR.md); none, RocksDB's own, where
   * the home's first attach found zero-filling does not pay.
   */
  std::unique_ptr<rocksdb::Env> wal_env;
  rocksdb::DB *root = nullptr; /* {homedir}/global/root */
  /*
   * The temporary instance, {homedir}/base/pgsql_tmp/sm
   * (TEMPTABLE.md §2): made empty by each attach, and never logged.
   */
  rocksdb::DB *tmp = nullptr;
  /* The listener of tmp's memtables, for its stores' counts. */
  std::shared_ptr<wisr::memcount_listener_t> tmp_listener;
  std::mutex mu; /* guards the root db and the two sets below */

  /* Open databases by name, shared by their sessions. */
  std::unordered_map<std::string, wisr::db_t *> dbs;

  /* Every open session, for sm1::detach. */
  std::unordered_set<sm1::session_t *> sessions;

  /*
   * The db reaper: a thread that closes each database left with no
   * session once its idle time has passed, woken by idle_cv when a
   * database's last session closes, and stopped by detach. Both
   * under mu.
   */
  std::thread db_reaper;
  std::condition_variable idle_cv;
  bool stopping = false;

  /* Close the root db and the temporary instance. */
  ~env_t();
};

namespace wisr {

/* Start env's db reaper; an environment's first act once attached. */
void env_start_db_reaper(sm1::env_t *env);

/*
 * Stop env's db reaper and close every database still open, all of
 * them idle once their sessions are closed. Called with env->mu not
 * held.
 */
void env_stop_db_reaper(sm1::env_t *env);

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
