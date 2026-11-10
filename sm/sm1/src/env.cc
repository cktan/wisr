/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * env.cc -- sm1::init, sm1::attach, sm1::detach: the home directory,
 * its root db, and the sweep of unnamed database directories
 * (../DBMGR.md §4).
 *
 * At most one process, and one environment in it, may attach a
 * home directory at a time. The library does not enforce this:
 * serializing processes is the caller's job. A second attach
 * usually fails anyway, on RocksDB's own lock of the root db,
 * but the directory-level work here -- the sweep, sm1::mkdb,
 * sm1::rmdb -- relies on the rule and is unsafe without it.
 */

#include "env.h"
#include "db.h"
#include "walfs.h"

#include "error.h"
#include "log.h"
#include "sm0_codec.h"
#include "sm0_defer.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <new>
#include <unordered_set>
#include <vector>

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace {

/* {homedir}/global */
std::string global_dir(const std::string &homedir) {
  return homedir + "/global";
}

/* {homedir}/base */
std::string base_dir(const std::string &homedir) { return homedir + "/base"; }

/* {homedir}/global/root */
std::string root_path(const std::string &homedir) {
  return global_dir(homedir) + "/root";
}

/* {homedir}/base/pgsql_tmp: temporary files, the host's and sm's. */
std::string tmp_dir(const std::string &homedir) {
  return base_dir(homedir) + "/pgsql_tmp";
}

/* {homedir}/base/pgsql_tmp/sm */
std::string tmp_path(const std::string &homedir) {
  return tmp_dir(homedir) + "/sm";
}

/* Check that homedir names an existing directory. */
int check_homedir(const char *homedir) {
  // Reject a NULL or empty path.
  if (!homedir || !*homedir) {
    return set_error(sm1::SM_ERR_INVALID, "homedir is NULL or empty");
  }
  struct stat st;
  // The path must exist.
  if (stat(homedir, &st) != 0) {
    return set_errno_error(errno, homedir);
  }
  // It must be a directory.
  if (!S_ISDIR(st.st_mode)) {
    return set_error(sm1::SM_ERR_INVALID, "{}: not a directory", homedir);
  }
  return 0;
}

/* Parse a base/ entry name as a dbid; false if it is not one. */
bool parse_dbid(const char *name, uint64_t *dbid) {
  // An empty name is not a dbid.
  if (!*name) {
    return false;
  }
  uint64_t v = 0;
  // Accept decimal digits only.
  for (const char *p = name; *p; p++) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    // Accumulate the decimal value.
    v = v * 10 + (*p - '0');
  }
  // Hand back the parsed dbid.
  *dbid = v;
  return true;
}

/*
 * Decide how every database of the home writes its new logs, and make
 * env->wal_env if they are zero-filled (../DBMGR.md).
 * 1. Read the answer from the root db. If it is there, go to step 5.
 * 2. Else, off WSL, the answer is that they are not zero-filled.
 * 3. On WSL, time a sync that grows a file against one that does not,
 *    in base/. A probe that fails zero-fills nothing and keeps
 *    nothing, so the next attach probes again.
 * 4. Keep the answer in the root db, so every attach after reads it.
 * 5. Make env->wal_env if the logs are zero-filled.
 */
int wal_prealloc(sm1::env_t *env) {
  std::string val;
  rocksdb::Status s =
      env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_WAL_PREALLOC, &val);
  if (!s.ok() && !s.IsNotFound()) {
    return set_status_error(s, "read root db");
  }
  uint64_t bytes = 0;
  if (s.ok()) {
    if (val.size() != 8) {
      return set_error(sm1::SM_ERR_CORRUPT, "{}: bad {}",
                       root_path(env->homedir).c_str(), WISR_ROOT_WAL_PREALLOC);
    }
    bytes = sm0::get_be64(val.data());
  } else {
    double append_us, overwrite_us;
    // The first attach, on WSL: time the file system the databases are
    // on. Nowhere else has a sync's cost been worth 64 MB a log.
    if (wal_on_wsl()) {
      if (wal_probe(base_dir(env->homedir), &append_us, &overwrite_us) != 0) {
        log_msg(sm1::loglevel_t::warn,
                "{}: could not time a sync; new logs are not zero-filled",
                env->homedir.c_str());
        return 0;
      }
      bytes =
          wal_prealloc_pays(append_us, overwrite_us) ? wal_prealloc_bytes : 0;
      log_msg(sm1::loglevel_t::info,
              "{}: a sync takes {:.0f} us after an append, {:.0f} us after "
              "an overwrite: new logs are {}",
              env->homedir.c_str(), append_us, overwrite_us,
              bytes ? "zero-filled to 64 MB" : "not zero-filled");
    }
    std::string be;
    sm0::put_be64(be, bytes);
    rocksdb::WriteOptions wo;
    wo.sync = true;
    // Keep the answer, so no later attach probes again.
    s = env->root->Put(wo, WISR_ROOT_WAL_PREALLOC, be);
    if (!s.ok()) {
      return set_status_error(s, "write root db");
    }
  }
  if (bytes > 0) {
    env->wal_env = walfs_env(bytes);
  }
  return 0;
}

/*
 * Destroy every directory under base/ that no "D" row in the
 * root db names: the remains of a crashed sm1::mkdb or sm1::rmdb.
 * Entries that are not dbids are left alone.
 */
int sweep_base(sm1::env_t *env) {
  std::unordered_set<uint64_t> live;
  // Collect every dbid the root db names.
  std::unique_ptr<rocksdb::Iterator> it(
      env->root->NewIterator(rocksdb::ReadOptions()));
  for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
       it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
       it->Next()) {
    // Reject a malformed dbid.
    if (it->value().size() != 8) {
      return set_error(sm1::SM_ERR_CORRUPT, "{}: bad dbid for database {}",
                       root_path(env->homedir).c_str(),
                       it->key().ToString().c_str() + 1);
    }
    // This dbid is live.
    live.insert(sm0::get_be64(it->value().data()));
  }
  // Fail if the root db scan failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "scan root db");
  }

  std::vector<std::string> orphans;
  {
    // List the entries under base/.
    std::string base = base_dir(env->homedir);
    DIR *dir = opendir(base.c_str());
    // Fail if base/ cannot be opened.
    if (!dir) {
      return set_errno_error(errno, base.c_str());
    }
    // Close base/ when done reading it, before destroying anything.
    defer { closedir(dir); };
    // Collect numbered directories that no name points at.
    while (struct dirent *de = readdir(dir)) {
      uint64_t dbid;
      if (parse_dbid(de->d_name, &dbid) && !live.count(dbid)) {
        orphans.push_back(env_db_path(env, dbid));
      }
    }
  }

  // Destroy each orphan.
  for (const std::string &path : orphans) {
    // Destroy an unnamed database directory.
    if (env_destroy_db_dir(path) != 0) {
      return -1;
    }
    log_msg(sm1::loglevel_t::info, "destroyed unnamed database directory {}",
            path.c_str());
  }
  return 0;
}

/*
 * Recover every database the root db names: open it, which
 * replays its WAL and runs the rest of startup (SM1_DESIGN.md
 * ../DBMGR.md §9.1), then close it again. A prepared transaction stays in
 * doubt, restored again by the next sm1::opendb. Returns 0, or
 * -1 with the error set by the first database that fails.
 */
int recover_dbs(sm1::env_t *env) {
  std::vector<std::pair<std::string, uint64_t>> dbs;
  // Collect every database's name and dbid.
  std::unique_ptr<rocksdb::Iterator> it(
      env->root->NewIterator(rocksdb::ReadOptions()));
  for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
       it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
       it->Next()) {
    // sweep_base already rejected a malformed dbid.
    dbs.emplace_back(it->key().ToString().substr(1),
                     sm0::get_be64(it->value().data()));
  }
  // Fail if the root db scan failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "scan root db");
  }
  for (const auto &[name, dbid] : dbs) {
    std::string path = env_db_path(env, dbid);
    log_msg(sm1::loglevel_t::info, "recovering database {} in {}", name.c_str(),
            path.c_str());
    // Open the database with the defaults: this is its recovery,
    // done when db_open returns (WAL replay, then ../DBMGR.md §9.1 steps 2-6).
    db_t *db = db_open(path, name.c_str(), nullptr, env->block_cache,
                       env->wal_env.get());
    // Fail the attach: a database that cannot recover.
    if (!db) {
      return -1;
    }
    // Close it: the first sm1::opendb opens it again, with its options.
    delete db;
  }
  return 0;
}

/*
 * The new-handler sm1::attach installs: running out of memory
 * anywhere in the process is fatal (the top-level DESIGN.md §5).
 *
 * It should be very rare: on a default Linux system an allocation
 * seldom fails, since the kernel's OOM killer ends the process
 * first; it takes a hard memory limit to get here. But when it
 * does, a std::bad_alloc thrown from inside RocksDB leaves RocksDB
 * unreliable -- later writes hang, closing can crash, and a
 * record's fate is unknown -- and no caller can tell that case
 * from a harmless one. Called before operator new would throw, it
 * says so loudly, on stderr as well as the log hook, and aborts,
 * so the next open's WAL replay recovers every committed
 * transaction. Formatting allocates nothing.
 */
[[noreturn]] void oom_handler() {
  // Say so on stderr and the log hook, and abort the process.
  fatal("out of memory: the process is aborting. Reopen the databases: "
        "WAL replay recovers every committed transaction.");
}

/*
 * Make env's temporary instance anew: destroy what an earlier
 * attach left at its path, and open an empty one (TEMPTABLE.md §2).
 * Returns 0 or -1 with the error set.
 */
int open_tmp(sm1::env_t *env) {
  std::string dir = tmp_dir(env->homedir);
  // The host may have made its directory of temporary files already.
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    return set_errno_error(errno, dir.c_str());
  }
  std::string path = tmp_path(env->homedir);
  // Nothing in it is read after a restart: remove it whole.
  if (env_destroy_db_dir(path) != 0) {
    return -1;
  }
  // Its default CF as a fixed CF of a database's, through the cache.
  rocksdb::Options opt(rocksdb::DBOptions(),
                       fixed_cf_options(env->block_cache));
  opt.create_if_missing = true;
  opt.error_if_exists = true;
  // Count its stores' memtable bytes as RocksDB seals and flushes them.
  env->tmp_listener = std::make_shared<wisr::memcount_listener_t>();
  opt.listeners.push_back(env->tmp_listener);
  // Open the empty instance every temporary store will live in.
  rocksdb::Status s = rocksdb::DB::Open(opt, path, &env->tmp);
  if (!s.ok()) {
    return set_status_error(s, path.c_str());
  }
  return 0;
}

/* sm1::attach, minus the out-of-memory guard. */
sm1::env_t *attach(const char *homedir, const sm1::env_options_t *opt) {
  sm1::env_options_t defaults;
  // No options: use the defaults.
  if (!opt) {
    opt = &defaults;
  }
  // Reject a cache that could hold nothing.
  if (opt->block_cache_bytes < 1) {
    set_error(sm1::SM_ERR_INVALID, "block_cache_bytes must be at least 1");
    return nullptr;
  }
  // Validate the home directory path.
  if (check_homedir(homedir) != 0) {
    return nullptr;
  }
  // Allocate the environment.
  std::unique_ptr<sm1::env_t> env(new sm1::env_t);
  env->homedir = homedir;
  // One budget, two users: a quarter to the buffer pool's pages, the
  // rest to the block cache's rows (../BUFMGR.md §1.8). The pool
  // keeps a floor, as a thread's pins must find frames.
  size_t budget = static_cast<size_t>(opt->block_cache_bytes);
  size_t sessions = static_cast<size_t>(std::max(opt->max_sessions, 0));
  env->bufpool = std::make_unique<bufpool_t>(
      std::max({budget / 4 / bufpool_frame_bytes, bufpool_min_frames,
                sessions * bufpool_session_frames}));
  // The one block cache every instance of the environment reads
  // through, made before the first of them opens.
  env->block_cache = rocksdb::NewLRUCache(budget - budget / 4);

  // Open the root db. It must exist: attach creates nothing.
  std::string rootpath = root_path(env->homedir);
  struct stat st;
  // Refuse a directory sm1::init never prepared.
  if (stat(rootpath.c_str(), &st) != 0) {
    set_error(sm1::SM_ERR_NOTFOUND, "{}: not a wisr home directory", homedir);
    return nullptr;
  }
  // Its default CF as a fixed CF of a database's, through the cache.
  rocksdb::Options ropt(rocksdb::DBOptions(),
                        fixed_cf_options(env->block_cache));
  // Open the root db.
  rocksdb::Status s = rocksdb::DB::Open(ropt, rootpath, &env->root);
  // Fail if it cannot be opened.
  if (!s.ok()) {
    set_status_error(s, rootpath.c_str());
    return nullptr;
  }

  std::string version;
  // Read the recorded format version.
  s = env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_VERSION, &version);
  // Reject a home directory without one.
  if (!s.ok() || version.size() != 8) {
    set_error(sm1::SM_ERR_CORRUPT, "{}: no format version", rootpath.c_str());
    return nullptr;
  }
  uint64_t v = sm0::get_be64(version.data());
  // Reject a format this build does not understand.
  if (v != WISR_HOME_VERSION) {
    set_error(sm1::SM_ERR_VERSION, "{}: format version {}, expected {}",
              homedir, v, WISR_HOME_VERSION);
    return nullptr;
  }

  // How every database writes its new logs, before any is opened.
  if (wal_prealloc(env.get()) != 0) {
    return nullptr;
  }
  // Destroy database directories no name points at.
  if (sweep_base(env.get()) != 0) {
    return nullptr;
  }
  // Make the temporary instance, before any database is opened.
  if (open_tmp(env.get()) != 0) {
    return nullptr;
  }
  // Recover every database, so none waits for its first session.
  if (recover_dbs(env.get()) != 0) {
    return nullptr;
  }
  // The db reaper closes databases left idle, until detach stops it.
  env_start_db_reaper(env.get());
  // Hand the environment to the caller.
  return env.release();
}

} // namespace

/* Close the root db and the temporary instance. */
sm1::env_t::~env_t() {
  // Close the temporary instance, if it was opened.
  if (tmp) {
    tmp->Close();
    delete tmp;
  }
  // Close the root db, if it was opened.
  if (root) {
    root->Close();
    delete root;
  }
}

namespace {

/*
 * The db reaper's loop, until told to stop:
 * 1. Close each database whose last session closed longer ago than its
 *    idle time.
 * 2. Sleep until the next is due, or until a database's last session
 *    closes.
 * 3. Go back to step 1.
 */
void db_reaper_main(sm1::env_t *env) {
  std::unique_lock<std::mutex> guard(env->mu);
  while (!env->stopping) {
    auto now = std::chrono::steady_clock::now();
    auto next = std::chrono::steady_clock::time_point::max();
    for (auto it = env->dbs.begin(); it != env->dbs.end();) {
      db_t *db = it->second;
      auto due = db->idle_since + std::chrono::milliseconds(db->idle_close_ms);
      if (db->nsessions > 0) {
        ++it;
      } else if (due <= now) {
        // Unregister it, then close it: no session came in time.
        it = env->dbs.erase(it);
        delete db;
      } else {
        next = std::min(next, due);
        ++it;
      }
    }
    if (next == std::chrono::steady_clock::time_point::max()) {
      env->idle_cv.wait(guard);
    } else {
      env->idle_cv.wait_until(guard, next);
    }
  }
}

} // namespace

namespace wisr {

/* Start the db reaper; see env.h. */
void env_start_db_reaper(sm1::env_t *env) {
  env->db_reaper = std::thread(db_reaper_main, env);
}

/* Stop the db reaper and close the idle databases; see env.h. */
void env_stop_db_reaper(sm1::env_t *env) {
  {
    std::lock_guard<std::mutex> guard(env->mu);
    env->stopping = true;
  }
  env->idle_cv.notify_one();
  if (env->db_reaper.joinable()) {
    env->db_reaper.join();
  }
  // Every session is closed: what is left open is idle.
  std::lock_guard<std::mutex> guard(env->mu);
  for (auto &kv : env->dbs) {
    delete kv.second;
  }
  env->dbs.clear();
}

/* The directory of database dbid. */
std::string env_db_path(const sm1::env_t *env, uint64_t dbid) {
  return base_dir(env->homedir) + "/" + std::to_string(dbid);
}

/* Remove a database directory and everything in it. */
int env_destroy_db_dir(const std::string &path) {
  // Delete RocksDB's files.
  rocksdb::Status s = rocksdb::DestroyDB(path, rocksdb::Options());
  // Fail if RocksDB could not destroy it.
  if (!s.ok()) {
    return set_status_error(s, path.c_str());
  }
  std::error_code ec;
  // Remove whatever else is left.
  std::filesystem::remove_all(path, ec);
  // Fail if the directory could not be removed.
  if (ec) {
    return set_errno_error(ec.value(), path.c_str());
  }
  return 0;
}

} // namespace wisr

/* Make homedir a wisr home directory: global/, base/, root db. */
int sm1::init(std::string_view homedir_arg) {
  // Take the name as a C string, as the engine uses it.
  std::string homedir_str;
  if (copy_name(homedir_arg, "home directory", homedir_str) != 0) {
    return -1;
  }
  const char *homedir = homedir_str.c_str();
  // Validate the home directory path.
  if (check_homedir(homedir) != 0) {
    return -1;
  }
  std::string global = global_dir(homedir);
  std::string base = base_dir(homedir);

  // Create global/ first: this mkdir settles a race between
  // two sm1::init calls, since the loser sees EEXIST.
  if (mkdir(global.c_str(), 0700) != 0) {
    // global/ already exists: this is already a home directory.
    if (errno == EEXIST) {
      return set_error(sm1::SM_ERR_EXISTS, "{}: already a wisr home directory",
                       homedir);
    }
    return set_errno_error(errno, global.c_str());
  }
  // Create base/, for the database directories.
  if (mkdir(base.c_str(), 0700) != 0) {
    return set_errno_error(errno, base.c_str());
  }

  // Create the root db and record the format version.
  std::string rootpath = root_path(homedir);
  // Create the root db, failing if one is already there.
  rocksdb::Options opt;
  opt.create_if_missing = true;
  opt.error_if_exists = true;
  rocksdb::DB *root = nullptr;
  // Open it, creating it.
  rocksdb::Status s = rocksdb::DB::Open(opt, rootpath, &root);
  // Fail if it cannot be created.
  if (!s.ok()) {
    return set_status_error(s, rootpath.c_str());
  }
  // Close the root db on the way out: sm1::init opens nothing for later.
  defer {
    root->Close();
    delete root;
  };
  // Encode the format version and the first dbid.
  std::string version, next_dbid;
  sm0::put_be64(version, WISR_HOME_VERSION);
  sm0::put_be64(next_dbid, 1);
  // Record both in one synced batch.
  rocksdb::WriteBatch batch;
  batch.Put(WISR_ROOT_VERSION, version);
  batch.Put(WISR_ROOT_NEXT_DBID, next_dbid);
  rocksdb::WriteOptions wo;
  wo.sync = true;
  // Write the batch.
  s = root->Write(wo, &batch);
  // Fail if the batch did not make it.
  if (!s.ok()) {
    return set_status_error(s, rootpath.c_str());
  }
  return 0;
}

/* Create an environment on an initialized home directory. */
int sm1::attach(std::string_view homedir_arg, sm1::env_t *&out,
                const sm1::env_options_t *opt) {
  // Make running out of memory fatal, unless the host process
  // already has an out-of-memory policy of its own.
  if (!std::get_new_handler()) {
    std::set_new_handler(oom_handler);
  }
  // Take the name as a C string, as the engine uses it.
  std::string homedir_str;
  if (copy_name(homedir_arg, "home directory", homedir_str) != 0) {
    return -1;
  }
  const char *homedir = homedir_str.c_str();
  // Attach, handing the environment back only on success.
  sm1::env_t *env = ::attach(homedir, opt);
  if (!env) {
    return -1;
  }
  out = env;
  return 0;
}

/*
 * Release an environment. With force, close its sessions
 * first, each as sm1::closedb would; without, refuse while any is
 * open.
 */
int sm1::detach(sm1::env_t *env, int force) {
  // Detaching NULL is a no-op.
  if (!env) {
    return 0;
  }
  std::unordered_set<sm1::session_t *> sessions;
  {
    // Hold the env mutex while reading the session set.
    std::lock_guard<std::mutex> guard(env->mu);
    // Refuse while sessions are open, unless forced.
    if (!env->sessions.empty() && !force) {
      return set_error(sm1::SM_ERR_INUSE, "{} sessions are still open",
                       env->sessions.size());
    }
    // Copy the sessions to close outside the mutex.
    sessions = env->sessions;
  }
  // Close each session, as sm1::closedb would.
  for (sm1::session_t *s : sessions) {
    sm1::closedb(s);
  }
  // Close the databases left idle, and stop their db reaper.
  env_stop_db_reaper(env);
  // Close the root db and free the environment.
  delete env;
  return 0;
}

/* The flags this level was built with; see sm1.h. */
const char *sm1::build_flags() {
#ifdef SM1_BUILD_FLAGS
  return SM1_BUILD_FLAGS;
#else
  return "unknown";
#endif
}

/* The keys queued for the row reaper of dbname, with no session; see sm1.h. */
size_t sm1::row_reaps_pending(sm1::env_t *env, std::string_view dbname) {
  if (nullptr == env) {
    return 0;
  }
  // Under env's mutex, so the db reaper cannot close the database here.
  std::lock_guard<std::mutex> guard(env->mu);
  auto it = env->dbs.find(std::string(dbname));
  return env->dbs.end() == it ? 0 : it->second->reaps.size();
}
