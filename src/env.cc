/*
 * env.cc -- sm_init, sm_attach, sm_detach: the home directory,
 * its root db, and the sweep of unnamed database directories
 * (DESIGN.md §3.5).
 *
 * At most one process, and one environment in it, may attach a
 * home directory at a time. The library does not enforce this:
 * serializing processes is the caller's job. A second attach
 * usually fails anyway, on RocksDB's own lock of the root db,
 * but the directory-level work here -- the sweep, sm_mkdb,
 * sm_rmdb -- relies on the rule and is unsafe without it.
 */

#include "env.h"

#include "codec.h"
#include "error.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <filesystem>
#include <memory>
#include <new>
#include <set>
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

/* Check that homedir names an existing directory. */
int check_homedir(const char *homedir) {
  // Reject a NULL or empty path.
  if (!homedir || !*homedir) {
    return set_error(SM_ERR_INVALID, "homedir is NULL or empty");
  }
  struct stat st;
  // The path must exist.
  if (stat(homedir, &st) != 0) {
    return set_errno_error(errno, homedir);
  }
  // It must be a directory.
  if (!S_ISDIR(st.st_mode)) {
    return set_error(SM_ERR_INVALID, "%s: not a directory", homedir);
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
 * Destroy every directory under base/ that no "D" row in the
 * root db names: the remains of a crashed sm_mkdb or sm_rmdb.
 * Entries that are not dbids are left alone.
 */
int sweep_base(sm_env_t *env) {
  std::set<uint64_t> live;
  // Collect every dbid the root db names.
  std::unique_ptr<rocksdb::Iterator> it(
      env->root->NewIterator(rocksdb::ReadOptions()));
  for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
       it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
       it->Next()) {
    // Reject a malformed dbid.
    if (it->value().size() != 8) {
      return set_error(SM_ERR_CORRUPT, "%s: bad dbid for database %s",
                       root_path(env->homedir).c_str(),
                       it->key().ToString().c_str() + 1);
    }
    // This dbid is live.
    live.insert(get_be64(it->value().data()));
  }
  // Fail if the root db scan failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "scan root db");
  }

  // List the entries under base/.
  std::string base = base_dir(env->homedir);
  DIR *dir = opendir(base.c_str());
  // Fail if base/ cannot be opened.
  if (!dir) {
    return set_errno_error(errno, base.c_str());
  }
  std::vector<std::string> orphans;
  // Collect numbered directories that no name points at.
  while (struct dirent *de = readdir(dir)) {
    uint64_t dbid;
    if (parse_dbid(de->d_name, &dbid) && !live.count(dbid)) {
      orphans.push_back(env_db_path(env, dbid));
    }
  }
  // Done reading base/.
  closedir(dir);

  // Destroy each orphan.
  for (const std::string &path : orphans) {
    // Destroy an unnamed database directory.
    if (env_destroy_db_dir(path) != 0) {
      return -1;
    }
    log_msg(SM_LOG_INFO, "destroyed unnamed database directory %s",
            path.c_str());
  }
  return 0;
}

/* sm_attach, minus the out-of-memory guard. */
sm_env_t *attach(const char *homedir) {
  // Validate the home directory path.
  if (check_homedir(homedir) != 0) {
    return nullptr;
  }
  // Allocate the environment.
  std::unique_ptr<sm_env_t> env(new sm_env_t);
  env->homedir = homedir;

  // Open the root db. It must exist: attach creates nothing.
  std::string rootpath = root_path(env->homedir);
  struct stat st;
  // Refuse a directory sm_init never prepared.
  if (stat(rootpath.c_str(), &st) != 0) {
    set_error(SM_ERR_NOTFOUND, "%s: not a wisr home directory", homedir);
    return nullptr;
  }
  rocksdb::Options opt;
  // Open the root db.
  rocksdb::Status s = rocksdb::DB::Open(opt, rootpath, &env->root);
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
    set_error(SM_ERR_CORRUPT, "%s: no format version", rootpath.c_str());
    return nullptr;
  }
  uint64_t v = get_be64(version.data());
  // Reject a format this build does not understand.
  if (v != WISR_HOME_VERSION) {
    set_error(SM_ERR_VERSION, "%s: format version %llu, expected %d", homedir,
              (unsigned long long)v, WISR_HOME_VERSION);
    return nullptr;
  }

  // Destroy database directories no name points at.
  if (sweep_base(env.get()) != 0) {
    return nullptr;
  }
  // Hand the environment to the caller.
  return env.release();
}

} // namespace

/* Close the root db. */
sm_env_t::~sm_env_t() {
  // Close the root db, if it was opened.
  if (root) {
    root->Close();
    delete root;
  }
}

namespace wisr {

/* The directory of database dbid. */
std::string env_db_path(const sm_env_t *env, uint64_t dbid) {
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
int sm_init(const char *homedir) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    // Validate the home directory path.
    if (check_homedir(homedir) != 0) {
      return -1;
    }
    std::string global = global_dir(homedir);
    std::string base = base_dir(homedir);

    // Create global/ first: this mkdir settles a race between
    // two sm_init calls, since the loser sees EEXIST.
    if (mkdir(global.c_str(), 0700) != 0) {
      // global/ already exists: this is already a home directory.
      if (errno == EEXIST) {
        return set_error(SM_ERR_EXISTS, "%s: already a wisr home directory",
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
    // Encode the format version and the first dbid.
    std::string version, next_dbid;
    put_be64(version, WISR_HOME_VERSION);
    put_be64(next_dbid, 1);
    // Record both in one synced batch.
    rocksdb::WriteBatch batch;
    batch.Put(WISR_ROOT_VERSION, version);
    batch.Put(WISR_ROOT_NEXT_DBID, next_dbid);
    rocksdb::WriteOptions wo;
    wo.sync = true;
    // Write the batch.
    s = root->Write(wo, &batch);
    // Close the root db: sm_init opens nothing for later.
    root->Close();
    delete root;
    // Fail if the batch did not make it.
    if (!s.ok()) {
      return set_status_error(s, rootpath.c_str());
    }
    return 0;
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_init: out of memory");
  }
}

/* Create an environment on an initialized home directory. */
sm_env_t *sm_attach(const char *homedir) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return attach(homedir);
  } catch (const std::bad_alloc &) {
    set_error(SM_ERR_NOMEM, "sm_attach: out of memory");
    return nullptr;
  }
}

/*
 * Release an environment. With force, close its sessions
 * first, each as sm_close would; without, refuse while any is
 * open.
 */
int sm_detach(sm_env_t *env, int force) {
  // Detaching NULL is a no-op.
  if (!env) {
    return 0;
  }
  std::set<sm_session_t *> sessions;
  {
    // Hold the env mutex while reading the session set.
    std::lock_guard<std::mutex> guard(env->mu);
    // Refuse while sessions are open, unless forced.
    if (!env->sessions.empty() && !force) {
      return set_error(SM_ERR_INUSE, "%zu sessions are still open",
                       env->sessions.size());
    }
    // Copy the sessions to close outside the mutex.
    sessions = env->sessions;
  }
  // Close each session, as sm_close would.
  for (sm_session_t *s : sessions) {
    sm_close(s);
  }
  // Close the root db and free the environment.
  delete env;
  return 0;
}
