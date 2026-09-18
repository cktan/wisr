/*
 * session.cc -- sm_open and sm_close, and the environment's
 * registry of open databases (DESIGN.md §1).
 */

#include "session.h"

#include "codec.h"
#include "env.h"
#include "error.h"

#include <memory>
#include <new>

using namespace wisr;

namespace
{

/*
 * Find dbname in the registry, or open it and register it.
 * Called with env->mu held. Returns NULL with the error set.
 */
db_t* find_or_open(sm_env_t* env, const char* dbname, const sm_options_t* opt)
{
    auto it = env->dbs.find(dbname);
    if (it != env->dbs.end()) {
        return it->second; // later sessions join; their opt is ignored
    }
    std::string val;
    rocksdb::Status s =
        env->root->Get(rocksdb::ReadOptions(),
                       std::string(WISR_ROOT_DBNAME_PREFIX) + dbname, &val);
    if (s.IsNotFound()) {
        set_error(SM_ERR_NOTFOUND, "database %s does not exist", dbname);
        return nullptr;
    }
    if (!s.ok()) {
        set_status_error(s, "read root db");
        return nullptr;
    }
    if (val.size() != 8) {
        set_error(SM_ERR_CORRUPT, "bad dbid for database %s", dbname);
        return nullptr;
    }
    // Open the database: its first session in this process.
    db_t* db = db_open(env_db_path(env, get_be64(val.data())), dbname, opt);
    if (db) {
        env->dbs[dbname] = db;
    }
    return db;
}

/* sm_open, minus the out-of-memory guard. */
sm_session_t* open_session(sm_env_t* env, const char* dbname,
                           const sm_options_t* opt)
{
    if (!env) {
        set_error(SM_ERR_INVALID, "sm_open: env is NULL");
        return nullptr;
    }
    if (check_name(dbname, SM_MAX_DBNAME, "database") != 0) {
        return nullptr;
    }
    std::unique_ptr<sm_session_t> s(new sm_session_t);
    std::lock_guard<std::mutex> guard(env->mu);
    db_t* db = find_or_open(env, dbname, opt);
    if (!db) {
        return nullptr;
    }
    s->env = env;
    s->db = db;
    db->nsessions++;
    env->sessions.insert(s.get());
    return s.release();
}

} // namespace

/* Open dbname and start a session on it. */
sm_session_t* sm_open(sm_env_t* env, const char* dbname,
                      const sm_options_t* opt)
{
    try {
        return open_session(env, dbname, opt);
    } catch (const std::bad_alloc&) {
        set_error(SM_ERR_NOMEM, "sm_open: out of memory");
        return nullptr;
    }
}

/*
 * Close a session: its cursors, then its active transaction.
 * The last session on a database closes the database.
 */
int sm_close(sm_session_t* session)
{
    if (!session) {
        return 0;
    }
    xct_end_scans(session);
    if (session->active) {
        sm_abort(session);
    }
    sm_env_t* env = session->env;
    db_t* db = session->db;
    {
        std::lock_guard<std::mutex> guard(env->mu);
        env->sessions.erase(session);
        if (--db->nsessions == 0) {
            env->dbs.erase(db->name);
            // Close the database: its last session is gone.
            delete db;
        }
    }
    delete session;
    return 0;
}
