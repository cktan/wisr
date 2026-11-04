/*
 * aa.cc -- the process's state and a session's: pg_global as last
 * committed, which every session reads, and each session's changes to
 * it, with the protocol that commits them (ROLE.md section 3).
 *
 * The protocol, from the load to a session's commit:
 * 1. aa_load() reads every role, membership and object's entry once,
 *    into snapshots the sessions share. A lookup reads a snapshot, and
 *    no transaction of pg_global's.
 * 2. A session's role DDL changes a copy of the roles' snapshot, which
 *    the session alone sees.
 * 3. At aa_commit(), before the database's commit, the session writes
 *    what it changed, in one transaction of pg_global's, and replaces
 *    the snapshot. A row another session changed meanwhile fails the
 *    commit, rather than lose that change. Objects' entries that are to
 *    grant less are written here too, granting only what they grant
 *    before and after.
 * 4. After the database's commit, aa_post_commit() writes the objects'
 *    entries.
 */
#include <algorithm>

#include "aa_internal.h"

/* How many of the objects changed lately aa_env_t keeps. */
#define CHANGED_KEPT 4096

/* An entry's state before the database's commit, and what it is until. */
struct intermediate_t {
  aa_key_t key;
  aa_object_t base;
  aa_object_t middle;
  bool base_present = true; /* false: base is what no entry means */
};

/* Closes an sm session on pg_global as it goes out of scope. */
struct session_guard_t {
  sm_session_t *session = NULL;
  ~session_guard_t() { sm_closedb(session); }
};

/*
 * Read pg_global into a state every session shares, once, at the start:
 * AA_ERR_NODB if the home directory has none, AA_ERR_NOTABLE if it lacks
 * one of its tables.
 */
int aa_load(sm_env_t *env, aa_env_t **out) {
  session_guard_t guard;
  auto roles = std::make_shared<aa_roles_t>();
  auto objects = std::make_shared<aa_objects_t>();
  uint64_t dbid;
  aa_env_t *e;

  if (0 != sm_dbid(env, AA_DBNAME, &dbid)) {
    if (SM_ERR_NOTFOUND == sm_errnum())
      return aa_seterr(AA_ERR_NODB, "data directory has no database \"%s\"",
                       AA_DBNAME);
    return aa_sm_failed("look up the database");
  }
  if (0 != sm_opendb(env, AA_DBNAME, NULL, &guard.session))
    return aa_sm_failed("open the database");
  if (0 != aa_store_read_roles(guard.session, roles.get()) ||
      0 != aa_store_read_objects(guard.session, objects.get()))
    return -1;
  e = new aa_env_t();
  e->env = env;
  e->roles.store(roles, std::memory_order_relaxed);
  e->objects.store(objects, std::memory_order_relaxed);
  *out = e;
  return 0;
}

/* Free what aa_load() read: at the end, once no session uses it. */
void aa_close(aa_env_t *env) { delete env; }

/* The roles as last committed. */
std::shared_ptr<const aa_roles_t> aa_roles(aa_env_t *env) {
  /* The snapshot is replaced as a whole; loaded without the lock. */
  return env->roles.load(std::memory_order_acquire);
}

/* The objects' entries as last committed. */
std::shared_ptr<const aa_objects_t> aa_objects(aa_env_t *env) {
  return env->objects.load(std::memory_order_acquire);
}

/*
 * A session's state: its epochs 0, so its first aa_roles_moved() and
 * aa_objects_moved() report what it may have cached before it.
 */
aa_session_t *aa_session_new(aa_env_t *env) {
  aa_session_t *s = new aa_session_t();

  s->env = env;
  return s;
}

/* Free a session's state, and close its sm session on pg_global. */
void aa_session_free(aa_session_t *s) {
  if (NULL == s)
    return;
  sm_closedb(s->session);
  delete s;
}

/*
 * The roles as the session sees them: with its transaction's own
 * changes, else as last committed.
 */
std::shared_ptr<const aa_roles_t> aa_session_roles(aa_session_t *s) {
  if (nullptr != s->working)
    return s->working;
  return aa_roles(s->env);
}

/* The session's sm session on pg_global, opened at its first need. */
static int write_session(aa_session_t *s, sm_session_t **out) {
  /* For as long as the session lasts: it hands out oids, and commits. */
  if (NULL == s->session &&
      0 != sm_opendb(s->env->env, AA_DBNAME, NULL, &s->session))
    return aa_sm_failed("open the database");
  *out = s->session;
  return 0;
}

/* A new oid for a role or a membership, from pg_global's sequence. */
int aa_new_oid(aa_session_t *s, aa_oid_t *out) {
  sm_session_t *session;
  uint64_t oid;

  if (0 != write_session(s, &session))
    return -1;
  if (0 != sm_nextseq(session, "role_oid", &oid))
    return aa_sm_failed("draw an oid");
  if (oid > UINT32_MAX)
    return aa_seterr(AA_ERR_LIMIT, "no role oid is left");
  *out = (aa_oid_t)oid;
  return 0;
}

/*
 * The session's working copy of the roles, to change: made from the
 * snapshot at its first change in the transaction.
 */
static aa_roles_t &working(aa_session_t *s) {
  s->version++;
  if (nullptr == s->working) {
    s->base = aa_roles(s->env);
    s->working = std::make_shared<aa_roles_t>(*s->base);
  }
  return *s->working;
}

/* Put row in rows by its oid, replacing one with that oid, in oid order. */
template <typename T> static void put_by_oid(std::vector<T> &rows, T row) {
  auto it =
      std::lower_bound(rows.begin(), rows.end(), row.oid,
                       [](const T &r, aa_oid_t oid) { return r.oid < oid; });

  if (rows.end() != it && it->oid == row.oid)
    *it = std::move(row);
  else
    rows.insert(it, std::move(row));
}

/* Remove the row with oid from rows, if there is one. */
template <typename T>
static void delete_by_oid(std::vector<T> &rows, aa_oid_t oid) {
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [oid](const T &r) { return r.oid == oid; }),
             rows.end());
}

/* Add or replace a role, in the session's transaction. */
void aa_put_role(aa_session_t *s, const aa_role_t &role) {
  put_by_oid(working(s).roles, role);
}

/* Remove the role oid, in the session's transaction. */
void aa_delete_role(aa_session_t *s, aa_oid_t oid) {
  delete_by_oid(working(s).roles, oid);
}

/* Add or replace a membership, in the session's transaction. */
void aa_put_member(aa_session_t *s, const aa_member_t &member) {
  put_by_oid(working(s).members, member);
}

/* Remove the membership oid, in the session's transaction. */
void aa_delete_member(aa_session_t *s, aa_oid_t oid) {
  delete_by_oid(working(s).members, oid);
}

/*
 * The entry of the object key as session s sees it, with its
 * transaction's changes, into *out: false if it has none.  s may be
 * NULL.
 */
bool aa_find_object(aa_env_t *env, aa_session_t *s, const aa_key_t &key,
                    aa_object_t *out) {
  std::shared_ptr<const aa_objects_t> all;

  if (NULL != s)
    for (auto it = s->changes.rbegin(); s->changes.rend() != it; ++it)
      if (it->key == key) {
        if (!it->present)
          return false;
        *out = it->object;
        return true;
      }
  all = aa_objects(env);
  auto found = all->find(key);
  if (all->end() == found)
    return false;
  *out = found->second;
  return true;
}

/* Make object the entry of its key, at the subtransaction's level. */
void aa_set_object(aa_session_t *s, int level, const aa_object_t &object) {
  s->version++;
  s->changes.push_back(
      {level, aa_key_t(object.dbid, object.objid), true, object});
}

/* Remove the entry of key, if it has one, at the subtransaction's level. */
void aa_forget_object(aa_session_t *s, int level, const aa_key_t &key) {
  aa_object_t object;

  if (aa_find_object(s->env, s, key, &object)) {
    s->version++;
    s->changes.push_back({level, key, false, object});
  }
}

/* The database dbid goes: every entry of its with it, after the commit. */
void aa_forget_database(aa_session_t *s, uint64_t dbid) {
  s->dropped_dbs.push_back(dbid);
}

/* Keep the temporary object objid's owner and ACL, to put back at level. */
static void keep_temp(aa_session_t *s, int level, aa_oid_t objid) {
  aa_temp_undo_t u{level, objid, false, 0, false, std::string()};
  u.had_owner = aa_temp_owner(s, objid, &u.owner);
  u.had_acl = aa_temp_acl(s, objid, &u.acl);
  s->temp_undo.push_back(std::move(u));
}

/*
 * Put back the temporary objects' owners and ACLs the subtransactions at
 * level and deeper changed, the last change first.
 */
static void undo_temp(aa_session_t *s, int level) {
  while (!s->temp_undo.empty() && s->temp_undo.back().level >= level) {
    const aa_temp_undo_t &u = s->temp_undo.back();
    if (u.had_owner)
      s->temp_owners[u.objid] = u.owner;
    else
      s->temp_owners.erase(u.objid);
    if (u.had_acl)
      s->temp_acls[u.objid] = u.acl;
    else
      s->temp_acls.erase(u.objid);
    s->temp_undo.pop_back();
  }
}

/* Make owner the owner of the session's temporary object objid. */
void aa_set_temp_owner(aa_session_t *s, int level, aa_oid_t objid,
                       aa_oid_t owner) {
  s->version++;
  keep_temp(s, level, objid);
  s->temp_owners[objid] = owner;
}

/* Make acl the ACL of the session's temporary object objid. */
void aa_set_temp_acl(aa_session_t *s, int level, aa_oid_t objid,
                     const std::string &acl) {
  s->version++;
  keep_temp(s, level, objid);
  s->temp_acls[objid] = acl;
}

/* The owner of the session's temporary object objid: false if none. */
bool aa_temp_owner(aa_session_t *s, aa_oid_t objid, aa_oid_t *owner) {
  auto it = s->temp_owners.find(objid);

  if (s->temp_owners.end() == it)
    return false;
  *owner = it->second;
  return true;
}

/* The ACL of the session's temporary object objid: false if none. */
bool aa_temp_acl(aa_session_t *s, aa_oid_t objid, std::string *acl) {
  auto it = s->temp_acls.find(objid);

  if (s->temp_acls.end() == it)
    return false;
  *acl = it->second;
  return true;
}

/* The session's temporary object objid goes: its owner and ACL too. */
void aa_forget_temp(aa_session_t *s, int level, aa_oid_t objid) {
  s->version++;
  keep_temp(s, level, objid);
  s->temp_owners.erase(objid);
  s->temp_acls.erase(objid);
}

/*
 * Whether the entry o's ACL names the role roleid, other than as its
 * owner: PostgreSQL records no dependency of an ACL on its object's
 * owner (updateAclDependencies()).
 */
static bool acl_names(const aa_object_t &o, aa_oid_t roleid) {
  if (!o.has_acl || o.owner == roleid)
    return false;
  for (const aa_acl_item_t &i : aa_acl_items(o.acl))
    if (i.grantee == roleid || i.grantor == roleid)
      return true;
  return false;
}

/*
 * The entries, as the session sees them, of the objects roleid owns,
 * into *owned, and of those whose ACL names it, into *granted, if not
 * NULL: of every database, a database's own among them (objid 0).
 */
void aa_objects_of(aa_env_t *env, aa_session_t *s, aa_oid_t roleid,
                   std::vector<aa_object_t> *owned,
                   std::vector<aa_object_t> *granted) {
  aa_objects_t all = *aa_objects(env);

  if (NULL != s)
    for (const aa_change_t &c : s->changes) {
      if (c.present)
        all[c.key] = c.object;
      else
        all.erase(c.key);
    }
  for (const auto &[key, o] : all) {
    if (NULL != owned && o.owner == roleid)
      owned->push_back(o);
    if (NULL != granted && acl_names(o, roleid))
      granted->push_back(o);
  }
}

/* A subtransaction at level commits: its changes are its parent's. */
void aa_subcommit(aa_session_t *s, int level) {
  for (aa_change_t &c : s->changes)
    if (c.level >= level)
      c.level = level - 1;
  for (aa_temp_undo_t &u : s->temp_undo)
    if (u.level >= level)
      u.level = level - 1;
}

/* A subtransaction at level aborts: its changes, the last made, go. */
void aa_subabort(aa_session_t *s, int level) {
  s->version++;
  while (!s->changes.empty() && s->changes.back().level >= level)
    s->changes.pop_back();
  undo_temp(s, level);
}

/* An entry's ACL items, or its owner's default ones if it keeps none. */
static std::vector<aa_acl_item_t> acl_or_default(const aa_object_t &o) {
  return o.has_acl ? aa_acl_items(o.acl) : aa_acl_default(o.classid, o.owner);
}

/* Whether two entries say the same: owner and ACL. */
static bool same_object(const aa_object_t &a, const aa_object_t &b) {
  return a.owner == b.owner && a.has_acl == b.has_acl &&
         (!a.has_acl || a.acl == b.acl);
}

/* The last change the session's transaction made to each entry. */
static std::map<aa_key_t, const aa_change_t *> last_changes(aa_session_t *s) {
  std::map<aa_key_t, const aa_change_t *> last;

  for (const aa_change_t &c : s->changes)
    last[c.key] = &c;
  return last;
}

/*
 * What an object with no entry is taken to be, as authorization takes it
 * (owner_and_acl()): its database's owner's, the bootstrap superuser's
 * for a database, with its owner's default ACL. Only for an object that
 * can exist with no entry -- a database, or one of the bootstrap's, below
 * AA_FIRST_NORMAL_OID -- in *out; false for one that is new, which grants
 * nothing before it is made.
 */
static bool implied(const aa_objects_t &all, aa_key_t key, aa_oid_t classid,
                    aa_object_t *out) {
  if (0 != key.second && key.second >= AA_FIRST_NORMAL_OID)
    return false;
  aa_object_t o;
  o.dbid = key.first;
  o.objid = key.second;
  o.classid = classid;
  o.has_acl = false;
  auto db = all.find(aa_key_t(key.first, 0));
  o.owner = 0 == key.second || all.end() == db ? AA_BOOTSTRAP_SUPERUSER
                                               : db->second.owner;
  *out = o;
  return true;
}

/*
 * The entries the session's transaction changes that grant less after
 * the database's commit than before it, in part: each as it is now
 * (base), and as it is to be until that commit (middle), granting what
 * both grant and no more -- the superuser its owner if the owner
 * changes, and of its ACL what both ACLs grant (aa_commit()).
 */
static std::vector<intermediate_t> intermediates(aa_session_t *s) {
  std::vector<intermediate_t> out;
  std::shared_ptr<const aa_objects_t> all;

  if (s->changes.empty())
    return out;
  all = aa_objects(s->env);
  for (const auto &[key, c] : last_changes(s)) {
    auto found = all->find(key);
    aa_object_t base, middle;
    bool present = all->end() != found;

    if (!c->present)
      continue;
    if (present) {
      base = found->second;
    } else if (!implied(*all, key, c->object.classid, &base)) {
      continue;
    }
    if (same_object(base, c->object))
      continue;
    middle = base;
    if (c->object.owner != middle.owner)
      middle.owner = AA_BOOTSTRAP_SUPERUSER;
    if (c->object.has_acl != middle.has_acl ||
        (middle.has_acl && c->object.acl != middle.acl)) {
      middle.has_acl = true;
      middle.acl = aa_acl_text(
          aa_acl_intersect(acl_or_default(base), acl_or_default(c->object)));
    }
    if (!same_object(middle, base))
      out.push_back({key, base, middle, present});
  }
  return out;
}

/*
 * Write to pg_object, by write(), in a transaction of pg_global's of its
 * own, the session's: all of it, or none.
 */
template <typename F> static int write_objects(aa_session_t *s, F write) {
  sm_session_t *session;
  sm_tbl_t *tbl;

  if (0 != write_session(s, &session))
    return -1;
  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 0))
    return aa_sm_failed("begin a transaction");
  if (0 != aa_store_open_objects(session, &tbl)) {
    (void)sm_abort(session);
    return -1;
  }
  if (0 != write(tbl)) {
    sm_tbl_close(tbl);
    (void)sm_abort(session);
    return -1;
  }
  sm_tbl_close(tbl);
  if (0 != sm_commit(session)) {
    aa_sm_failed("commit");
    (void)sm_abort(session);
    return -1;
  }
  return 0;
}

/*
 * Write the session's role changes, and the entries that are to grant
 * less, as before (intermediates()), in one transaction of pg_global's.
 */
static int write_roles_and(aa_session_t *s, sm_session_t *session,
                           const std::vector<intermediate_t> &before) {
  sm_tbl_t *tbl;

  if (nullptr != s->working &&
      0 != aa_store_write_roles(session, *s->base, *s->working))
    return -1;
  if (before.empty())
    return 0;
  if (0 != aa_store_open_objects(session, &tbl))
    return -1;
  for (const intermediate_t &i : before)
    if (0 != aa_store_write_object(tbl, i.key, &i.middle)) {
      sm_tbl_close(tbl);
      return -1;
    }
  sm_tbl_close(tbl);
  return 0;
}

/* Let go of the objects' commit order, if the session holds it. */
static void release_objects_order(aa_session_t *s) {
  if (s->objects_committing) {
    s->objects_committing = false;
    s->env->objects_commit_mutex.unlock();
  }
}

/* Releases the session's hold on the objects' commit order at its scope's
 * end: aa_post_commit()'s or aa_abort()'s, however it returns. */
struct objects_order_release_t {
  aa_session_t *s;
  ~objects_order_release_t() { release_objects_order(s); }
};

/* Wait out another session's commit of objects in flight; see aa.h. */
void aa_object_settled(aa_session_t *s) {
  if (s->objects_committing)
    return;
  // Taken and let go at once: held only while a commit is in flight.
  std::lock_guard<std::mutex> wait(s->env->objects_commit_mutex);
}

/*
 * At the commit of the session's transaction, before the database's:
 * write its changes to pg_global.
 * 1. If objects' entries changed, take the objects' commit order, held
 *    to the end of aa_post_commit() or aa_abort().
 * 2. Find each entry that is to grant less (intermediates()).
 * 3. If the roles changed, take the role commits' order.
 * 4. In one transaction of pg_global's, write the role changes, and each
 *    entry that is to grant less as granting only what it grants before
 *    and after; read the roles back, and commit.
 * 5. Keep those entries as they were, for aa_abort() to put back if the
 *    database's commit fails.
 * 6. If the roles changed, replace the roles' snapshot with those read.
 */
int aa_commit(aa_session_t *s) {
  bool roles = nullptr != s->working;
  sm_session_t *session;

  // Changes to objects' entries commit one transaction at a time, to the
  // end of aa_post_commit() or aa_abort(): read the entries under it, as
  // the commit before left them.
  if ((!s->changes.empty() || !s->dropped_dbs.empty()) &&
      !s->objects_committing) {
    s->env->objects_commit_mutex.lock();
    s->objects_committing = true;
  }
  std::vector<intermediate_t> before = intermediates(s);
  if (!roles && before.empty())
    return 0;
  // One role commit at a time, from before its transaction begins: its
  // snapshot then holds every role commit before it, and the roles it
  // reads back are pg_global's as they are at its commit.
  std::unique_lock<std::mutex> order(s->env->roles_commit_mutex,
                                     std::defer_lock);
  if (roles)
    order.lock();
  if (0 != write_session(s, &session))
    return -1;
  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 0))
    return aa_sm_failed("begin a transaction");

  /*
   * Each object that is to grant less grants, until the database
   * commits, only what it grants before and after: a crash between
   * leaves less granted, not more.
   */
  if (0 != write_roles_and(s, session, before)) {
    (void)sm_abort(session);
    return -1;
  }
  /*
   * The roles as this commit leaves them, read before it, so no read
   * that can fail comes after the commit point.
   */
  auto next = std::make_shared<aa_roles_t>();
  if (roles && 0 != aa_store_read_roles_in(session, next.get())) {
    (void)sm_abort(session);
    return -1;
  }
  if (0 != sm_commit(session)) {
    if (SM_ERR_UPDATED == sm_errnum() || sm_retryable())
      aa_seterr(AA_ERR_CONFLICT,
                "Another session changed pg_authid since this one read it.");
    else
      aa_sm_failed("commit");
    (void)sm_abort(session);
    return -1;
  }
  /* Put back if the database's commit fails (aa_abort()). */
  s->to_restore.clear();
  for (const intermediate_t &i : before)
    s->to_restore.push_back({i.key, i.base, i.base_present});
  if (!roles)
    return 0;
  s->base.reset();
  s->working.reset();
  s->version++;

  // Replace the snapshot with the roles read: the commits are in order,
  // so no later read is lost to an earlier one.
  std::lock_guard<std::mutex> lock(s->env->mutex);
  s->env->roles.store(next, std::memory_order_release);
  // The session's caches hold its own change already, but not one of
  // another's it has not seen: it is up to date only if it was before.
  bool fresh = s->epoch == s->env->epoch.load();
  uint64_t epoch = ++s->env->epoch;
  if (fresh)
    s->epoch = epoch;
  return 0;
}

/*
 * At the abort of the session's transaction: forget its changes, and put
 * back the entries aa_commit() wrote before the database's commit, which
 * failed.
 */
int aa_abort(aa_session_t *s) {
  std::vector<aa_restore_t> restore = std::move(s->to_restore);
  // The commit order goes once what is put back is written.
  objects_order_release_t release{s};

  s->base.reset();
  s->working.reset();
  s->changes.clear();
  s->version++;
  s->dropped_dbs.clear();
  s->to_restore.clear();
  // The temporary objects' owners and ACLs, as the transaction began.
  undo_temp(s, 0);
  if (restore.empty())
    return 0;
  return write_objects(s, [&](sm_tbl_t *tbl) {
    // An entry that was none goes, so its object is again what none means.
    for (const aa_restore_t &r : restore)
      if (0 != aa_store_write_object(tbl, r.key, r.present ? &r.object : NULL))
        return -1;
    return 0;
  });
}

/*
 * Replace the objects' snapshot with it as committed, with the changes
 * last and the databases dropped applied, under the lock; record which
 * objects changed, for other sessions' caches.
 */
static void publish_objects(aa_env_t *env,
                            const std::map<aa_key_t, const aa_change_t *> &last,
                            const std::vector<uint64_t> &dropped) {
  std::lock_guard<std::mutex> lock(env->mutex);
  auto next = std::make_shared<aa_objects_t>(
      *env->objects.load(std::memory_order_relaxed));
  uint64_t epoch = env->objects_epoch.load() + 1;

  for (const auto &[key, c] : last) {
    if (c->present)
      (*next)[key] = c->object;
    else
      next->erase(key);
    env->changed.push_back({epoch, key, c->present ? c->object.classid : 0});
  }
  for (uint64_t dbid : dropped) {
    next->erase(next->lower_bound(aa_key_t(dbid, 0)),
                next->lower_bound(aa_key_t(dbid + 1, 0)));
    env->changed.push_back({epoch, aa_key_t(dbid, 0), AA_CLASS_DATABASE});
  }
  // Trim to what is kept, noting the newest epoch that lost an entry:
  // one epoch may span the trim, its first entries gone, its last kept.
  while (env->changed.size() > CHANGED_KEPT) {
    env->changed_lost = std::max(env->changed_lost, env->changed.front().epoch);
    env->changed.pop_front();
  }
  env->objects.store(next, std::memory_order_release);
  env->objects_epoch.store(epoch);
}

/*
 * After the database's commit: write the transaction's changes to
 * objects' entries, in one transaction of pg_global's.
 * 1. Write each object's entry as the transaction left it.
 * 2. Remove the entries of the databases it dropped.
 * 3. If the writes succeed, replace the objects' snapshot
 *    (publish_objects()).
 * 4. Forget the changes, whether the writes succeed or not.
 */
int aa_post_commit(aa_session_t *s) {
  // The commit order goes once the snapshot is replaced.
  objects_order_release_t release{s};
  std::map<aa_key_t, const aa_change_t *> last;
  std::shared_ptr<const aa_objects_t> all;
  int rc;

  s->to_restore.clear();
  // The temporary objects' owners and ACLs stand: the transaction
  // committed.
  s->temp_undo.clear();
  if (s->changes.empty() && s->dropped_dbs.empty())
    return 0;
  last = last_changes(s);
  all = aa_objects(s->env);
  rc = write_objects(s, [&](sm_tbl_t *tbl) {
    for (const auto &[key, c] : last)
      if (0 != aa_store_write_object(tbl, key, c->present ? &c->object : NULL))
        return -1;
    for (uint64_t dbid : s->dropped_dbs)
      for (auto it = all->lower_bound(aa_key_t(dbid, 0));
           all->end() != it && it->first.first == dbid; ++it)
        if (0 != aa_store_write_object(tbl, it->first, NULL))
          return -1;
    return 0;
  });
  if (0 == rc)
    publish_objects(s->env, last, s->dropped_dbs);
  s->changes.clear();
  s->dropped_dbs.clear();
  s->version++;
  return rc;
}

/*
 * Whether another session has replaced the roles since this one last
 * asked: true once, and then they are taken to be fresh.
 */
bool aa_roles_moved(aa_session_t *s) {
  uint64_t epoch = s->env->epoch.load();

  if (epoch == s->epoch)
    return false;
  s->epoch = epoch;
  return true;
}

/* Whether another session has changed objects' entries; see aa.h. */
bool aa_objects_stale(const aa_session_t *s) {
  return s->env->objects_epoch.load() != s->objects_epoch;
}

/* Say what changed since this session last asked; see aa.h. */
bool aa_objects_moved(aa_session_t *s, uint64_t dbid, aa_moved_t *out) {
  aa_env_t *env = s->env;
  uint64_t epoch;

  *out = aa_moved_t();
  if (env->objects_epoch.load() == s->objects_epoch)
    return false;
  {
    std::lock_guard<std::mutex> lock(env->mutex);

    epoch = env->objects_epoch.load();
    // All, if what is kept may not name every object changed since:
    // nothing kept, or an entry of an epoch past this session's trimmed.
    out->all = env->changed.empty() || env->changed_lost > s->objects_epoch;
    out->namespaces = out->databases = out->all;
    if (!out->all)
      for (const aa_changed_t &c : env->changed) {
        if (c.epoch <= s->objects_epoch)
          continue;
        out->databases |= AA_CLASS_DATABASE == c.classid;
        if (c.key.first != dbid || 0 == c.key.second)
          continue;
        out->namespaces |= AA_CLASS_NAMESPACE == c.classid;
        out->oids.push_back(c.key.second);
      }
  }
  s->objects_epoch = epoch;
  return true;
}
