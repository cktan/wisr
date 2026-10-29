/*
 * session.cc -- sm1::opendb and sm1::closedb, and the environment's
 * registry of open databases (SM1_DESIGN.md §1).
 */

#include "session.h"

#include "env.h"
#include "error.h"
#include "sm0_codec.h"

#include <memory>
#include <new>

using namespace wisr;

namespace {

/* Whether opt asks for other options than db was opened with. */
bool other_options(const db_t *db, const sm1::options_t *opt) {
  const sm1::options_t &had = db->opts;
  std::string rocks = opt->rocksdb_options ? opt->rocksdb_options : "";
  return rocks != db->rocksdb_options ||
         opt->durability_sync != had.durability_sync ||
         opt->group_commit_us != had.group_commit_us ||
         opt->deadlock_ms != had.deadlock_ms ||
         opt->ssi_max_entries != had.ssi_max_entries ||
         opt->ts_lease != had.ts_lease ||
         opt->commit_cache_entries != had.commit_cache_entries ||
         opt->savepoint_undo_bytes != had.savepoint_undo_bytes ||
         opt->idle_close_ms != had.idle_close_ms;
}

/*
 * Find dbname in the registry, or open it and register it.
 * One left idle that a session asks other options of is closed
 * and opened again with them, as no session is using it.
 * Called with env->mu held. Returns NULL with the error set.
 */
db_t *find_or_open(sm1::env_t *env, const char *dbname,
                   const sm1::options_t *opt) {
  // Look for the database among those already open.
  auto it = env->dbs.find(dbname);
  // Reuse an open database: later sessions join it, and while any
  // session has it their options are ignored.
  if (it != env->dbs.end()) {
    db_t *open = it->second;
    if (open->nsessions > 0 || !opt || !other_options(open, opt)) {
      return open;
    }
    // Idle, and asked for other options: close it, to open it anew.
    env->dbs.erase(it);
    delete open;
  }
  // Look up the database's dbid in the root db.
  std::string val;
  rocksdb::Status s =
      env->root->Get(rocksdb::ReadOptions(),
                     std::string(WISR_ROOT_DBNAME_PREFIX) + dbname, &val);
  // Fail if no database has this name.
  if (s.IsNotFound()) {
    set_error(sm1::SM_ERR_NOTFOUND, "database {} does not exist", dbname);
    return nullptr;
  }
  // Fail on any other root db error.
  if (!s.ok()) {
    set_status_error(s, "read root db");
    return nullptr;
  }
  // Reject a malformed dbid.
  if (val.size() != 8) {
    set_error(sm1::SM_ERR_CORRUPT, "bad dbid for database {}", dbname);
    return nullptr;
  }
  // Open the database: its first session in this process.
  uint64_t dbid = sm0::get_be64(val.data());
  db_t *db = db_open(env_db_path(env, dbid), dbname, opt, env->block_cache,
                     env->wal_env.get());
  // Register the newly opened database.
  if (db) {
    // Its temporary stores go in the environment's instance.
    db->tmp = env->tmp;
    db->tmp_listener = env->tmp_listener.get();
    db->dbid = dbid;
    env->dbs[dbname] = db;
  }
  return db;
}

/* sm1::opendb, minus the out-of-memory guard. */
sm1::session_t *open_session(sm1::env_t *env, const char *dbname,
                             const sm1::options_t *opt) {
  // Reject a NULL environment.
  if (!env) {
    set_error(sm1::SM_ERR_INVALID, "sm1_opendb: env is NULL");
    return nullptr;
  }
  // Validate the database name.
  if (check_name(dbname, sm1::max_dbname, "database") != 0) {
    return nullptr;
  }
  // Allocate the session before taking the lock.
  std::unique_ptr<sm1::session_t> s(new sm1::session_t);
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
  // Register the session for sm1::detach.
  env->sessions.insert(s.get());
  // And with its database, for a strong lock's sweep (§5.1).
  lock_register(s.get());
  return s.release();
}

} // namespace

/* Open dbname and start a session on it. */
int sm1::opendb(sm1::env_t *env, std::string_view dbname_arg,
                sm1::session_t *&out, const sm1::options_t *opt) {
  // Take the name as a C string, as the engine uses it.
  std::string dbname_str;
  if (copy_name(dbname_arg, "database", dbname_str) != 0) {
    return -1;
  }
  const char *dbname = dbname_str.c_str();
  // Open the session, handing it back only on success.
  sm1::session_t *s = open_session(env, dbname, opt);
  if (!s) {
    return -1;
  }
  out = s;
  return 0;
}

/*
 * Close a session: its cursors, then its active transaction.
 * The last session on a database leaves it open for its idle
 * time, for the next to find, or closes it at once with none.
 */
int sm1::closedb(sm1::session_t *session) {
  // Closing NULL is a no-op.
  if (!session) {
    return 0;
  }
  // Close the session's cursors first, and let go of its lent
  // row, so no iterator outlives the database.
  xct_end_scans(session);
  sm1::unpin(&session->readpin);
  // Abort an active transaction; a prepared one stays in doubt.
  if (session->active) {
    sm1::abort(session);
  }
  // Drop the temporary stores it still holds: they are no one else's.
  for (uint64_t storeid : session->temp_stores) {
    db_drop_store_cf(session->db, storeid);
  }
  session->temp_stores.clear();
  // Keep what is needed after the session is freed.
  sm1::env_t *env = session->env;
  db_t *db = session->db;
  {
    // Hold the env mutex while updating the registries.
    std::lock_guard<std::mutex> guard(env->mu);
    // Unregister the session, here and from its database's sweep.
    env->sessions.erase(session);
    lock_unregister(session);
    // Drop the session count; the last session closes the database,
    // or leaves it to the reaper for its idle time.
    if (--db->nsessions == 0) {
      // A failed database is closed at once, so the next open
      // recovers it rather than finding it failed.
      if (db->idle_close_ms == 0 || db->failed.load()) {
        // Unregister the database before closing it.
        env->dbs.erase(db->name);
        // Close the database: its last session is gone.
        delete db;
      } else {
        db->idle_since = std::chrono::steady_clock::now();
        // The reaper closes it once its idle time has passed.
        env->idle_cv.notify_one();
      }
    }
  }
  // Free the session.
  delete session;
  return 0;
}

/* Number the session for waits_for. */
int sm1::set_tag(sm1::session_t *session, uint64_t tag) {
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "NULL session");
  }
  session->tag.store(tag, std::memory_order_relaxed);
  return 0;
}

/*
 * The numbers of the sessions the session numbered tag waits for.
 * env->mu keeps every registered session, and its database, from
 * being freed; its database's lock_mu keeps its waits-for edges
 * still. An edge may name a session that has since closed, whose
 * holds are gone: only one still registered is dereferenced.
 */
int sm1::waits_for(sm1::env_t *env, uint64_t tag, std::vector<uint64_t> *out) {
  if (!env) {
    return set_error(sm1::SM_ERR_INVALID, "NULL env");
  }
  if (!out) {
    return set_error(sm1::SM_ERR_INVALID, "NULL out");
  }
  out->clear();
  if (tag == 0) {
    return 0;
  }
  // Hold the env mutex while reading the registry.
  std::lock_guard<std::mutex> guard(env->mu);
  sm1::session_t *waiter = nullptr;
  for (sm1::session_t *s : env->sessions) {
    if (s->tag.load(std::memory_order_relaxed) == tag) {
      waiter = s;
      break;
    }
  }
  if (!waiter) {
    return 0;
  }
  // Hold the database's lock_mu while reading the waiter's edges,
  // from the table: those its wait recorded may be gone.
  std::lock_guard<std::mutex> lock_guard(waiter->db->lock_mu);
  std::vector<sm1::session_t *> blockers;
  lock_blockers(waiter->db, waiter, &blockers);
  for (sm1::session_t *holder : blockers) {
    if (!env->sessions.count(holder)) {
      continue;
    }
    uint64_t t = holder->tag.load(std::memory_order_relaxed);
    if (t != 0) {
      out->push_back(t);
    }
  }
  return 0;
}

/*
 * Every lock of env's open databases, held or waited for. env->mu
 * keeps the databases and the registered sessions from being freed;
 * each database's lock_mu keeps its table and its sessions' waits
 * still. A holder is dereferenced only under its database's lock_mu,
 * which a session's release takes before it closes.
 */
int sm1::lock_status(sm1::env_t *env, std::vector<sm1::lock_status_t> &out) {
  if (!env) {
    return set_error(sm1::SM_ERR_INVALID, "NULL env");
  }
  out.clear();
  // Hold the env mutex while reading the databases and the registry.
  std::lock_guard<std::mutex> guard(env->mu);
  for (const auto &[name, db] : env->dbs) {
    // Hold each of its sessions' fp_mu, then lock_mu, while reading
    // the slots, the table, the lock lists and the waits: a session
    // adds to its list under fp_mu alone, for a slot.
    std::vector<std::unique_lock<std::mutex>> fps;
    for (sm1::session_t *s : env->sessions) {
      if (s->db == db) {
        fps.emplace_back(s->fp_mu);
      }
    }
    std::lock_guard<std::mutex> lock_guard(db->lock_mu);
    // A lock table key is the type byte, be64(storeid), then the key.
    auto entry = [&](const std::string &k, sm1::lockmode_t mode,
                     const sm1::session_t *s, bool granted) {
      out.push_back({name, db->dbid, static_cast<sm1::locktype_t>(k[0]),
                     sm0::get_be64(k.data() + 1), k.substr(9), mode,
                     s->tag.load(std::memory_order_relaxed), granted});
    };
    for (const auto &[k, ent] : db->lock_table) {
      for (const auto &h : ent.holders) {
        // Each of the holder's list entries the key stands for,
        // from the first: more than one if row keys hash alike.
        const auto &locks = h.session->locks;
        for (size_t i = h.idx, n = h.refs; n > 0 && i < locks.size(); i++) {
          if (locks[i].key == k) {
            entry(lock_name(locks[i]), locks[i].mode, h.session, true);
            n--;
          }
        }
      }
    }
    for (const sm1::session_t *s : env->sessions) {
      if (s->db != db) {
        continue;
      }
      // Its locks in slots, which the table does not hold.
      for (int i = 0; s->fp_used > 0 && i < sm1::session_t::fp_slots; i++) {
        if (s->fp[i].used) {
          const sm1::session_t::lockref_t &l = s->locks[s->fp[i].idx];
          entry(lock_name(l), l.mode, s, true);
        }
      }
      if (!s->waiting_key.empty()) {
        entry(s->waiting_key, s->waiting_mode, s, false);
      }
    }
  }
  return 0;
}

/* Set the session's wait before a deadlock check; 0 is its db's. */
int sm1::set_deadlock_ms(sm1::session_t *session, int ms) {
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "NULL session");
  }
  if (ms < 0) {
    return set_error(sm1::SM_ERR_INVALID, "deadlock_ms must not be negative");
  }
  session->deadlock_ms = ms;
  return 0;
}
