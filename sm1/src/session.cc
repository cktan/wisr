/*
 * session.cc -- sm1_opendb and sm1_closedb, and the environment's
 * registry of open databases (SM1_DESIGN.md §1).
 */

#include "session.h"

#include "codec.h"
#include "env.h"
#include "error.h"

#include <memory>
#include <new>

using namespace wisr;

namespace {

/*
 * Find dbname in the registry, or open it and register it.
 * Called with env->mu held. Returns NULL with the error set.
 */
db_t *find_or_open(sm1_env_t *env, const char *dbname,
                   const sm1_options_t *opt) {
  // Look for the database among those already open.
  auto it = env->dbs.find(dbname);
  // Reuse an open database: later sessions join it.
  if (it != env->dbs.end()) {
    return it->second; // later sessions join; their opt is ignored
  }
  // Look up the database's dbid in the root db.
  std::string val;
  rocksdb::Status s =
      env->root->Get(rocksdb::ReadOptions(),
                     std::string(WISR_ROOT_DBNAME_PREFIX) + dbname, &val);
  // Fail if no database has this name.
  if (s.IsNotFound()) {
    set_error(SM_ERR_NOTFOUND, "database {} does not exist", dbname);
    return nullptr;
  }
  // Fail on any other root db error.
  if (!s.ok()) {
    set_status_error(s, "read root db");
    return nullptr;
  }
  // Reject a malformed dbid.
  if (val.size() != 8) {
    set_error(SM_ERR_CORRUPT, "bad dbid for database {}", dbname);
    return nullptr;
  }
  // Open the database: its first session in this process.
  db_t *db = db_open(env_db_path(env, get_be64(val.data())), dbname, opt);
  // Register the newly opened database.
  if (db) {
    env->dbs[dbname] = db;
  }
  return db;
}

/* sm1_opendb, minus the out-of-memory guard. */
sm1_session_t *open_session(sm1_env_t *env, const char *dbname,
                            const sm1_options_t *opt) {
  // Reject a NULL environment.
  if (!env) {
    set_error(SM_ERR_INVALID, "sm1_opendb: env is NULL");
    return nullptr;
  }
  // Validate the database name.
  if (check_name(dbname, SM1_MAX_DBNAME, "database") != 0) {
    return nullptr;
  }
  // Allocate the session before taking the lock.
  std::unique_ptr<sm1_session_t> s(new sm1_session_t);
  // Hold the env mutex while touching the registries.
  std::lock_guard<std::mutex> guard(env->mu);
  // Find the open database, or open it.
  db_t *db = find_or_open(env, dbname, opt);
  // Fail if it could not be opened.
  if (!db) {
    return nullptr;
  }
  // Bind the session to its environment and database.
  s->env = env;
  s->db = db;
  // Count the session against the database.
  db->nsessions++;
  // Register the session for sm1_detach.
  env->sessions.insert(s.get());
  return s.release();
}

} // namespace

/* Open dbname and start a session on it. */
sm1_session_t *sm1_opendb(sm1_env_t *env, const char *dbname,
                          const sm1_options_t *opt) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return open_session(env, dbname, opt);
  } catch (const std::bad_alloc &) {
    set_error(SM_ERR_NOMEM, "sm1_opendb: out of memory");
    return nullptr;
  }
}

/*
 * Close a session: its cursors, then its active transaction.
 * The last session on a database closes the database.
 */
int sm1_closedb(sm1_session_t *session) {
  // Closing NULL is a no-op.
  if (!session) {
    return 0;
  }
  // Close the session's cursors first.
  xct_end_scans(session);
  // Abort an active transaction; a prepared one stays in doubt.
  if (session->active) {
    sm1_abort(session);
  }
  // Keep what is needed after the session is freed.
  sm1_env_t *env = session->env;
  db_t *db = session->db;
  {
    // Hold the env mutex while updating the registries.
    std::lock_guard<std::mutex> guard(env->mu);
    // Unregister the session.
    env->sessions.erase(session);
    // Drop the session count; the last session closes the database.
    if (--db->nsessions == 0) {
      // Unregister the database before closing it.
      env->dbs.erase(db->name);
      // Close the database: its last session is gone.
      delete db;
    }
  }
  // Free the session.
  delete session;
  return 0;
}
