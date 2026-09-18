/*
 * env.cc -- sm_init, sm_attach, sm_detach: the home directory,
 * its pidlock, its root db, and the sweep of unnamed database
 * directories (DESIGN.md §3.5).
 */

#include "env.h"

#include "codec.h"
#include "error.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <new>
#include <set>
#include <vector>

#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace
{

/* {homedir}/global */
std::string global_dir(const std::string& homedir)
{
    return homedir + "/global";
}

/* {homedir}/base */
std::string base_dir(const std::string& homedir)
{
    return homedir + "/base";
}

/* {homedir}/global/root */
std::string root_path(const std::string& homedir)
{
    return global_dir(homedir) + "/root";
}

/* {homedir}/pidlock.pid */
std::string pidlock_path(const std::string& homedir)
{
    return homedir + "/pidlock.pid";
}

/* Check that homedir names an existing directory. */
int check_homedir(const char* homedir)
{
    if (!homedir || !*homedir) {
        return set_error(SM_ERR_INVALID, "homedir is NULL or empty");
    }
    struct stat st;
    if (stat(homedir, &st) != 0) {
        return set_errno_error(errno, homedir);
    }
    if (!S_ISDIR(st.st_mode)) {
        return set_error(SM_ERR_INVALID, "%s: not a directory", homedir);
    }
    return 0;
}

/* Parse a base/ entry name as a dbid; false if it is not one. */
bool parse_dbid(const char* name, uint64_t* dbid)
{
    if (!*name) {
        return false;
    }
    uint64_t v = 0;
    for (const char* p = name; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + (*p - '0');
    }
    *dbid = v;
    return true;
}

/*
 * Destroy every directory under base/ that no "D" row in the
 * root db names: the remains of a crashed sm_mkdb or sm_rmdb.
 * Entries that are not dbids are left alone.
 */
int sweep_base(sm_env_t* env)
{
    std::set<uint64_t> live;
    std::unique_ptr<rocksdb::Iterator> it(
        env->root->NewIterator(rocksdb::ReadOptions()));
    for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
         it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
         it->Next()) {
        if (it->value().size() != 8) {
            return set_error(SM_ERR_CORRUPT, "%s: bad dbid for database %s",
                             root_path(env->homedir).c_str(),
                             it->key().ToString().c_str() + 1);
        }
        live.insert(get_be64(it->value().data()));
    }
    if (!it->status().ok()) {
        return set_status_error(it->status(), "scan root db");
    }

    std::string base = base_dir(env->homedir);
    DIR* dir = opendir(base.c_str());
    if (!dir) {
        return set_errno_error(errno, base.c_str());
    }
    std::vector<std::string> orphans;
    while (struct dirent* de = readdir(dir)) {
        uint64_t dbid;
        if (parse_dbid(de->d_name, &dbid) && !live.count(dbid)) {
            orphans.push_back(env_db_path(env, dbid));
        }
    }
    closedir(dir);

    for (const std::string& path : orphans) {
        // Destroy an unnamed database directory.
        if (env_destroy_db_dir(path) != 0) {
            return -1;
        }
    }
    return 0;
}

/* sm_attach, minus the out-of-memory guard. */
sm_env_t* attach(const char* homedir)
{
    if (check_homedir(homedir) != 0) {
        return nullptr;
    }
    std::unique_ptr<sm_env_t> env(new sm_env_t);
    env->homedir = homedir;

    // Take the pidlock. flock() is released by the kernel when
    // its holder exits, so a crashed process leaves no lock.
    std::string lockpath = pidlock_path(env->homedir);
    int fd = open(lockpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_errno_error(errno, lockpath.c_str());
        return nullptr;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;
        close(fd);
        if (err == EWOULDBLOCK) {
            set_error(SM_ERR_INUSE, "%s: home directory is in use", homedir);
        } else {
            set_errno_error(err, lockpath.c_str());
        }
        return nullptr;
    }
    env->lock_fd = fd;
    if (ftruncate(fd, 0) != 0 || dprintf(fd, "%d\n", (int)getpid()) < 0) {
        set_errno_error(errno, lockpath.c_str());
        return nullptr;
    }

    // Open the root db. It must exist: attach creates nothing.
    std::string rootpath = root_path(env->homedir);
    struct stat st;
    if (stat(rootpath.c_str(), &st) != 0) {
        set_error(SM_ERR_NOTFOUND, "%s: not a wisr home directory", homedir);
        return nullptr;
    }
    rocksdb::Options opt;
    rocksdb::Status s = rocksdb::DB::Open(opt, rootpath, &env->root);
    if (!s.ok()) {
        set_status_error(s, rootpath.c_str());
        return nullptr;
    }

    std::string version;
    s = env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_VERSION, &version);
    if (!s.ok() || version.size() != 8) {
        set_error(SM_ERR_CORRUPT, "%s: no format version", rootpath.c_str());
        return nullptr;
    }
    uint64_t v = get_be64(version.data());
    if (v != WISR_HOME_VERSION) {
        set_error(SM_ERR_VERSION, "%s: format version %llu, expected %d",
                  homedir, (unsigned long long)v, WISR_HOME_VERSION);
        return nullptr;
    }

    if (sweep_base(env.get()) != 0) {
        return nullptr;
    }
    return env.release();
}

} // namespace

/* Close the root db, then release the pidlock by closing its fd. */
sm_env_t::~sm_env_t()
{
    if (root) {
        root->Close();
        delete root;
    }
    if (lock_fd >= 0) {
        close(lock_fd);
    }
}

namespace wisr
{

/* The directory of database dbid. */
std::string env_db_path(const sm_env_t* env, uint64_t dbid)
{
    return base_dir(env->homedir) + "/" + std::to_string(dbid);
}

/* Remove a database directory and everything in it. */
int env_destroy_db_dir(const std::string& path)
{
    rocksdb::Status s = rocksdb::DestroyDB(path, rocksdb::Options());
    if (!s.ok()) {
        return set_status_error(s, path.c_str());
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        return set_errno_error(ec.value(), path.c_str());
    }
    return 0;
}

} // namespace wisr

/* Make homedir a wisr home directory: global/, base/, root db. */
int sm_init(const char* homedir)
{
    try {
        if (check_homedir(homedir) != 0) {
            return -1;
        }
        std::string global = global_dir(homedir);
        std::string base = base_dir(homedir);

        // Create global/ first: this mkdir settles a race between
        // two sm_init calls, since the loser sees EEXIST.
        if (mkdir(global.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                return set_error(SM_ERR_EXISTS,
                                 "%s: already a wisr home directory", homedir);
            }
            return set_errno_error(errno, global.c_str());
        }
        if (mkdir(base.c_str(), 0700) != 0) {
            return set_errno_error(errno, base.c_str());
        }

        // Create the root db and record the format version.
        std::string rootpath = root_path(homedir);
        rocksdb::Options opt;
        opt.create_if_missing = true;
        opt.error_if_exists = true;
        rocksdb::DB* root = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(opt, rootpath, &root);
        if (!s.ok()) {
            return set_status_error(s, rootpath.c_str());
        }
        std::string version, next_dbid;
        put_be64(version, WISR_HOME_VERSION);
        put_be64(next_dbid, 1);
        rocksdb::WriteBatch batch;
        batch.Put(WISR_ROOT_VERSION, version);
        batch.Put(WISR_ROOT_NEXT_DBID, next_dbid);
        rocksdb::WriteOptions wo;
        wo.sync = true;
        s = root->Write(wo, &batch);
        root->Close();
        delete root;
        if (!s.ok()) {
            return set_status_error(s, rootpath.c_str());
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_init: out of memory");
    }
}

/* Create an environment on an initialized home directory. */
sm_env_t* sm_attach(const char* homedir)
{
    try {
        return attach(homedir);
    } catch (const std::bad_alloc&) {
        set_error(SM_ERR_NOMEM, "sm_attach: out of memory");
        return nullptr;
    }
}

/*
 * Release an environment. With force, close its sessions
 * first, each as sm_close would; without, refuse while any is
 * open.
 */
int sm_detach(sm_env_t* env, int force)
{
    if (!env) {
        return 0;
    }
    std::set<sm_session_t*> sessions;
    {
        std::lock_guard<std::mutex> guard(env->mu);
        if (!env->sessions.empty() && !force) {
            return set_error(SM_ERR_INUSE, "%zu sessions are still open",
                             env->sessions.size());
        }
        sessions = env->sessions;
    }
    for (sm_session_t* s : sessions) {
        sm_close(s);
    }
    // Close the root db and release the pidlock.
    delete env;
    return 0;
}
