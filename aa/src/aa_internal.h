/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * aa_internal.h -- what aa's own files share, and no user of aa sees.
 */
#ifndef AA_INTERNAL_H
#define AA_INTERNAL_H

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "aa.h"
#include "sm.h"

/* Record an error; each returns -1, so a failing path is one line. */
int aa_seterr(aa_errnum_t code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int aa_sm_failed(const char *what);

/* Closes an sm session on pg_global as it goes out of scope. */
struct session_guard_t {
  sm_session_t *session = NULL;
  ~session_guard_t() { sm_closedb(session); }
};

/* One object an objects' replacement changed. */
struct aa_changed_t {
  uint64_t epoch;   /* the replacement's */
  aa_key_t key;     /* the object's; its objid 0 for a database dropped */
  aa_oid_t classid; /* its class; 0 for an entry that went */
};

/* The process's: pg_global as last committed, which every session reads. */
struct aa_env_t {
  sm_env_t *env = NULL; /* the home directory pg_global is in */
  std::mutex mutex;     /* orders the snapshots' replacement, and guards
                           changed */
  /* Orders the commits of role changes: held from before a commit's
   * transaction begins to its snapshot's replacement, so the roles it
   * reads back include every role commit before it. */
  std::mutex roles_commit_mutex;
  /* Orders the commits of objects' entries (AA_DESIGN.md section 5): held
   * by the session whose transaction changed some, from aa_commit() to the
   * end of aa_post_commit() or aa_abort(), across the database's commit,
   * so pg_global and the objects' snapshot change in one order, and a
   * change's read of an entry waits for it (aa_object_settled()). Taken
   * before roles_commit_mutex. */
  std::mutex objects_commit_mutex;
  std::atomic<std::shared_ptr<const aa_roles_t>> roles;
  /* How many times roles was replaced: a session's caches of them are
   * stale when it has moved (aa_roles_moved()). */
  std::atomic<uint64_t> epoch{0};
  /* Every object's entry, replaced as a whole under mutex, read without it. */
  std::atomic<std::shared_ptr<const aa_objects_t>> objects;
  /* How many times objects was replaced, and, for the last of them, which
   * object each replacement changed: a session drops what it caches of
   * those (aa_objects_moved()). */
  std::atomic<uint64_t> objects_epoch{0};
  std::deque<aa_changed_t> changed;
  /* The newest epoch an entry of changed was trimmed off of, 0 if none:
   * a session behind it may have missed an object, and drops all. */
  uint64_t changed_lost = 0;
};

/*
 * What a session's cache is of: the shared roles' and objects' epochs and
 * the session's own version; equal stamps, the same view.
 */
struct aa_stamp_t {
  uint64_t epoch = UINT64_MAX;
  uint64_t objects_epoch = UINT64_MAX;
  uint64_t version = UINT64_MAX;
  bool operator==(const aa_stamp_t &) const = default;
};

/* What aa_object_mask() caches a mask by: an object, and a role. */
struct aa_mask_key_t {
  uint64_t dbid;
  aa_oid_t objid;
  aa_oid_t roleid;
  bool operator==(const aa_mask_key_t &) const = default;
};

/* aa_mask_key_t's hash, for the session's map of masks. */
struct aa_mask_key_hash_t {
  size_t operator()(const aa_mask_key_t &k) const {
    return std::hash<uint64_t>()(k.dbid * 0x9E3779B97F4A7C15ULL ^
                                 ((uint64_t)k.objid << 32 | k.roleid));
  }
};

/* The roles a role is a member of, by one kind of grant, in a database. */
struct aa_member_cache_t {
  aa_stamp_t stamp;
  aa_oid_t roleid = 0;
  uint64_t dbid = 0;
  std::vector<aa_oid_t> roles;
};

/* A change to an object's entry the session's transaction made. */
struct aa_change_t {
  int level;    /* the subtransaction's nesting level that made it */
  aa_key_t key; /* the object's */
  bool present; /* false: the entry goes */
  aa_object_t object;
  /* The objects epoch the entry the change is based on was read at: a
   * commit fails if the entry changed since (aa_commit()). */
  uint64_t base_epoch;
};

/*
 * A temporary object's owner and ACL as they were before a change, to put
 * back if the subtransaction at level, or the transaction, aborts.
 */
struct aa_temp_undo_t {
  int level;
  aa_oid_t objid;
  bool had_owner;
  aa_oid_t owner;
  bool had_acl;
  std::string acl;
};

/* An entry to put back at an abort: as it was, or none if it had none. */
struct aa_restore_t {
  aa_key_t key;
  aa_object_t object;
  bool present = true;
  /* The entry aa_commit() wrote in its place until the final write. */
  aa_object_t middle;
};

/* A session's own: its changes not yet committed, and its sm session. */
struct aa_session_t {
  aa_env_t *env = NULL;
  /* An sm session on pg_global, opened at the first need. */
  sm_session_t *session = NULL;
  /* The epoch this session's caches of the roles are of. */
  uint64_t epoch = 0;
  /* While the session's transaction has changed roles: the snapshot the
   * changes began from, and it with the changes, which the session sees. */
  std::shared_ptr<const aa_roles_t> base;
  std::shared_ptr<aa_roles_t> working;

  /* The objects epoch this session's caches are of. */
  uint64_t objects_epoch = 0;
  /* The transaction's changes to objects' entries, in order. */
  std::vector<aa_change_t> changes;
  /* The databases dropped by the transaction, whose entries go. */
  std::vector<uint64_t> dropped_dbs;
  /* Entries written before the database's commit, as they were, to put
   * back if it fails (aa_commit()): an object that had none, as none. */
  std::vector<aa_restore_t> to_restore;
  /* The session's temporary objects' owners and ACLs, never written. */
  std::map<aa_oid_t, aa_oid_t> temp_owners;
  std::map<aa_oid_t, std::string> temp_acls;
  /* What the transaction changed of them, each as it was before, in
   * order: an abort puts them back. */
  std::vector<aa_temp_undo_t> temp_undo;
  /* Whether the session holds env->objects_commit_mutex, from its
   * aa_commit() to its aa_post_commit() or aa_abort(). */
  bool objects_committing = false;
  /* The objects epoch at the session's last aa_object_settled() in the
   * transaction, if it called it: what its reads of entries are as of. */
  bool settled = false;
  uint64_t settled_epoch = 0;

  /* Bumped at each change to what the session sees of the roles or the
   * objects' entries: with the shared epochs, what its caches are of. */
  uint64_t version = 0;
  /* The caches of aa_authz.cc, each valid while its stamp is current. */
  aa_stamp_t roles_stamp;
  std::shared_ptr<const aa_roles_t> roles;
  aa_stamp_t super_stamp;
  aa_oid_t super_role = 0;
  bool super = false;
  aa_member_cache_t member_cache[3]; /* by aa_recurse_t */
  /* Every privilege a role holds on an object, by (dbid, objid, role),
   * all of them as of mask_stamp (aa_object_mask()). */
  aa_stamp_t mask_stamp;
  std::unordered_map<aa_mask_key_t, uint64_t, aa_mask_key_hash_t> masks;
  /* The host's objects' masks, by (classid in dbid's place, objid, role),
   * as of kept_stamp (aa_kept_mask()). */
  aa_stamp_t kept_stamp;
  std::unordered_map<aa_mask_key_t, uint64_t, aa_mask_key_hash_t> kept;
};

/* ---- pg_global's tables (aa_store.cc) ---- */

/* Every role and membership, in a transaction of session's own. */
int aa_store_read_roles(sm_session_t *session, aa_roles_t *out);

/* The same, in session's transaction, which is open. */
int aa_store_read_roles_in(sm_session_t *session, aa_roles_t *out);

/* Every object's entry, likewise. */
int aa_store_read_objects(sm_session_t *session, aa_objects_t *out);

/*
 * Write the roles and memberships of working that differ from base, in
 * session's transaction; one not as base had it is another session's
 * change, AA_ERR_CONFLICT.
 */
int aa_store_write_roles(sm_session_t *session, const aa_roles_t &base,
                         const aa_roles_t &working);

/* pg_object, open to write in session's transaction. */
int aa_store_open_objects(sm_session_t *session, sm_tbl_t **out);

/* Make pg_object's row for key object, or none if object is NULL. */
int aa_store_write_object(sm_tbl_t *tbl, const aa_key_t &key,
                          const aa_object_t *object);

#endif /* AA_INTERNAL_H */
