/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * aa_authz.cc -- who holds which privilege: the closures over the
 * membership graph that INHERIT, SET and ADMIN make, an ACL's mask for a
 * role, the grantor a GRANT is made as, and CONNECT at a connect, as
 * PostgreSQL's acl.c computes them.
 *
 * A session caches the roles it reads and, for each kind of grant, the
 * roles one role is a member of, as PostgreSQL's roles_is_member_of()
 * does; each cache holds while its stamp, the shared epochs and the
 * session's own version, is current, so a hit takes no lock.
 */
#include <unordered_set>

#include "aa_internal.h"

/* How many objects' masks a session keeps (aa_object_mask()). */
#define MASKS_KEPT 4096

/* Which grants a closure follows (roles_is_member_of()'s types). */
enum aa_recurse_t {
  AA_RECURSE_MEMBERS = 0, /* every grant */
  AA_RECURSE_PRIVS = 1,   /* grants with INHERIT */
  AA_RECURSE_SETROLE = 2  /* grants with SET */
};

/* The view s's caches are of now. */
static aa_stamp_t stamp_of(aa_session_t *s) {
  aa_stamp_t st;

  st.epoch = s->env->epoch.load();
  st.objects_epoch = s->env->objects_epoch.load();
  st.version = s->version;
  return st;
}

/* The roles as s sees them, or as last committed, or none with no env. */
static std::shared_ptr<const aa_roles_t> roles_of(aa_env_t *env,
                                                  aa_session_t *s) {
  static const std::shared_ptr<const aa_roles_t> none =
      std::make_shared<const aa_roles_t>();
  aa_stamp_t st;

  if (NULL == env)
    return none;
  if (NULL == s)
    return aa_roles(env);
  st = stamp_of(s);
  if (!(st == s->roles_stamp)) {
    s->roles = aa_session_roles(s);
    s->roles_stamp = st;
  }
  return s->roles;
}

/* Whether roleid is a superuser: the bootstrap superuser always is. */
bool aa_is_superuser(aa_env_t *env, aa_session_t *s, aa_oid_t roleid) {
  aa_stamp_t st;
  bool super = false;

  if (AA_BOOTSTRAP_SUPERUSER == roleid)
    return true;
  if (NULL != s && NULL != env) {
    st = stamp_of(s);
    if (st == s->super_stamp && s->super_role == roleid)
      return s->super;
  }
  for (const aa_role_t &r : roles_of(env, s)->roles)
    if (r.oid == roleid) {
      super = r.rolsuper;
      break;
    }
  if (NULL != s && NULL != env) {
    s->super_stamp = st;
    s->super_role = roleid;
    s->super = super;
  }
  return super;
}

/* The owner of the database dbid, a member of pg_database_owner; 0 if
 * there is no database. */
static aa_oid_t database_owner(aa_env_t *env, aa_session_t *s, uint64_t dbid) {
  aa_object_t o;

  if (AA_NO_DATABASE == dbid)
    return 0;
  if (NULL != env && aa_find_object(env, s, aa_key_t(dbid, 0), &o))
    return o.owner;
  return AA_BOOTSTRAP_SUPERUSER;
}

/*
 * The roles roleid is a member of through the grants type follows, roleid
 * first, the nearest next, breadth first; and if admin_of, into
 * *admin_role, the first of them granted admin_of WITH ADMIN OPTION, or 0.
 */
static std::vector<aa_oid_t> closure(const aa_roles_t &roles, aa_oid_t roleid,
                                     aa_recurse_t type, aa_oid_t dba,
                                     aa_oid_t admin_of, aa_oid_t *admin_role) {
  std::vector<aa_oid_t> out = {roleid};
  std::unordered_set<aa_oid_t> seen = {roleid};

  for (size_t i = 0; i < out.size(); i++) {
    aa_oid_t memberid = out[i];

    for (const aa_member_t &m : roles.members) {
      if (m.member != memberid)
        continue;
      if (0 != admin_of && m.roleid == admin_of && m.admin_option &&
          0 == *admin_role)
        *admin_role = memberid;
      if (AA_RECURSE_PRIVS == type && !m.inherit_option)
        continue;
      if (AA_RECURSE_SETROLE == type && !m.set_option)
        continue;
      /* A role may be reached twice: A in B, and A in C in B. */
      if (seen.insert(m.roleid).second)
        out.push_back(m.roleid);
    }
    /* A database's owner is in pg_database_owner, there. */
    if (0 != dba && memberid == dba && seen.insert(AA_PG_DATABASE_OWNER).second)
      out.push_back(AA_PG_DATABASE_OWNER);
  }
  return out;
}

/*
 * The roles roleid is a member of by type, as closure() makes them, from
 * s's cache if it holds them: valid until s's next call.
 */
static const std::vector<aa_oid_t> &member_of(aa_env_t *env, aa_session_t *s,
                                              uint64_t dbid, aa_oid_t roleid,
                                              aa_recurse_t type) {
  static thread_local std::vector<aa_oid_t> uncached;
  aa_oid_t none = 0;

  if (NULL == s || NULL == env) {
    uncached = closure(*roles_of(env, s), roleid, type,
                       database_owner(env, s, dbid), 0, &none);
    return uncached;
  }
  aa_member_cache_t &c = s->member_cache[type];
  aa_stamp_t st = stamp_of(s);

  if (!(st == c.stamp) || c.roleid != roleid || c.dbid != dbid) {
    c.roles = closure(*roles_of(env, s), roleid, type,
                      database_owner(env, s, dbid), 0, &none);
    c.stamp = st;
    c.roleid = roleid;
    c.dbid = dbid;
  }
  return c.roles;
}

/* Whether role is in roles. */
static bool contains(const std::vector<aa_oid_t> &roles, aa_oid_t role) {
  for (aa_oid_t r : roles)
    if (r == role)
      return true;
  return false;
}

/* Whether member has role's privileges, through grants with INHERIT. */
bool aa_has_privs_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                          aa_oid_t member, aa_oid_t role) {
  if (member == role || aa_is_superuser(env, s, member))
    return true;
  return contains(member_of(env, s, dbid, member, AA_RECURSE_PRIVS), role);
}

/* Whether member may SET ROLE to role, through grants with SET. */
bool aa_can_set_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     aa_oid_t member, aa_oid_t role) {
  if (member == role || aa_is_superuser(env, s, member))
    return true;
  return contains(member_of(env, s, dbid, member, AA_RECURSE_SETROLE), role);
}

/* Whether member is a member of role through any grants. */
bool aa_is_member_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                          aa_oid_t member, aa_oid_t role, bool nosuper) {
  if (member == role || (!nosuper && aa_is_superuser(env, s, member)))
    return true;
  return contains(member_of(env, s, dbid, member, AA_RECURSE_MEMBERS), role);
}

/*
 * The first role of member's closure by type granted role WITH ADMIN
 * OPTION, or 0: never cached, as PostgreSQL's is not.
 */
static aa_oid_t admin_in(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                         aa_oid_t member, aa_oid_t role, aa_recurse_t type) {
  aa_oid_t admin = 0;

  (void)closure(*roles_of(env, s), member, type, database_owner(env, s, dbid),
                role, &admin);
  return admin;
}

/* Whether member may administer role: never role itself. */
bool aa_is_admin_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                         aa_oid_t member, aa_oid_t role) {
  if (aa_is_superuser(env, s, member))
    return true;
  /* By policy, a role cannot have WITH ADMIN OPTION on itself. */
  if (member == role)
    return false;
  return 0 != admin_in(env, s, dbid, member, role, AA_RECURSE_MEMBERS);
}

/* The nearest role whose privileges member has with ADMIN on role. */
aa_oid_t aa_select_best_admin(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                              aa_oid_t member, aa_oid_t role) {
  if (member == role)
    return 0;
  return admin_in(env, s, dbid, member, role, AA_RECURSE_PRIVS);
}

/* Whether result answers mask: all of it, or with any, some. */
static bool done(uint64_t result, uint64_t mask, bool any) {
  return any ? 0 != result : result == mask;
}

/*
 * The privileges of mask roleid holds by the ACL items. Each step
 * returns once the privileges found answer mask (done()).
 * 1. If roleid has the owner's privileges, take every grant option of
 *    mask.
 * 2. Add the privileges of each item granted roleid directly, or
 *    PUBLIC.
 * 3. Add the privileges of each item granted a role whose privileges
 *    roleid has.
 */
uint64_t aa_acl_mask(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     const aa_acl_item_t *items, size_t n, aa_oid_t roleid,
                     aa_oid_t owner, uint64_t mask, bool any) {
  uint64_t result = 0, remaining;

  if (0 == mask)
    return 0;
  /* The owner, and so each who has its privileges, has every option. */
  if (0 != (mask & AA_ALL_GOPTION_BITS) &&
      aa_has_privs_of_role(env, s, dbid, roleid, owner)) {
    result = mask & AA_ALL_GOPTION_BITS;
    if (done(result, mask, any))
      return result;
  }
  for (size_t i = 0; i < n; i++)
    if (AA_PUBLIC == items[i].grantee || roleid == items[i].grantee) {
      result |= items[i].privs & mask;
      if (done(result, mask, any))
        return result;
    }
  /* A second pass, as a membership costs more to test than an item. */
  remaining = mask & ~result;
  for (size_t i = 0; i < n; i++) {
    if (AA_PUBLIC == items[i].grantee || roleid == items[i].grantee)
      continue;
    if (0 != (items[i].privs & remaining) &&
        aa_has_privs_of_role(env, s, dbid, roleid, items[i].grantee)) {
      result |= items[i].privs & mask;
      if (done(result, mask, any))
        return result;
      remaining = mask & ~result;
    }
  }
  return result;
}

/* The privileges of mask granted roleid itself, or held as the owner. */
uint64_t aa_acl_mask_direct(const aa_acl_item_t *items, size_t n,
                            aa_oid_t roleid, aa_oid_t owner, uint64_t mask,
                            bool any) {
  uint64_t result = 0;

  if (0 == mask)
    return 0;
  if (0 != (mask & AA_ALL_GOPTION_BITS) && roleid == owner) {
    result = mask & AA_ALL_GOPTION_BITS;
    if (done(result, mask, any))
      return result;
  }
  for (size_t i = 0; i < n; i++)
    if (roleid == items[i].grantee) {
      result |= items[i].privs & mask;
      if (done(result, mask, any))
        return result;
    }
  return result;
}

/* The number of bits set in v. */
static int bits(uint64_t v) { return __builtin_popcountll(v); }

/*
 * Who roleid grants or revokes privs as, into *grantor, and the grant
 * options of privs that role holds, into *goptions.
 * 1. If roleid is the owner or a superuser, pick the owner, with every
 *    grant option of privs.
 * 2. Else walk the roles whose privileges roleid has, roleid first, the
 *    nearest next.
 * 3. Pick the first role that holds every grant option of privs.
 * 4. Failing that, pick the nearest role with the most of them, or
 *    roleid with none if no role holds any.
 */
void aa_select_best_grantor(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                            aa_oid_t roleid, uint64_t privs,
                            const aa_acl_item_t *items, size_t n,
                            aa_oid_t owner, aa_oid_t *grantor,
                            uint64_t *goptions) {
  /* A privilege's grant option is its bit, shifted up (ACL_GRANT_OPTION_FOR).
   */
  uint64_t needed = (privs & 0xFFFFFFFFULL) << 32;
  int most = 0;

  if (roleid == owner || aa_is_superuser(env, s, roleid)) {
    *grantor = owner;
    *goptions = needed;
    return;
  }
  *grantor = roleid;
  *goptions = 0;
  for (aa_oid_t other : member_of(env, s, dbid, roleid, AA_RECURSE_PRIVS)) {
    uint64_t held = aa_acl_mask_direct(items, n, other, owner, needed, false);

    if (held == needed) {
      *grantor = other;
      *goptions = held;
      return;
    }
    if (0 != held && bits(held) > most) {
      *grantor = other;
      *goptions = held;
      most = bits(held);
    }
  }
}

/*
 * The owner and the ACL items of the object objid of the database dbid, of
 * class classid, as s sees it, each found apart: the owner a temporary
 * object's, else its entry's, else its database's; the ACL a temporary
 * object's, else its entry's, else the owner's default.
 */
static void owner_and_acl(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                          aa_oid_t objid, aa_oid_t classid, aa_oid_t *owner,
                          std::vector<aa_acl_item_t> *items) {
  aa_object_t o;
  std::string acl;
  bool entry = NULL != env && aa_find_object(env, s, aa_key_t(dbid, objid), &o);

  if (NULL == s || !aa_temp_owner(s, objid, owner))
    *owner = entry        ? o.owner
             : 0 == objid ? AA_BOOTSTRAP_SUPERUSER
                          : database_owner(env, s, dbid);
  if (NULL != s && aa_temp_acl(s, objid, &acl))
    *items = aa_acl_items(acl);
  else if (entry && o.has_acl)
    *items = aa_acl_items(o.acl);
  else
    *items = aa_acl_default(classid, *owner);
}

/* The privileges of mask roleid holds on an object, from s's cache. */
uint64_t aa_object_mask(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                        aa_oid_t objid, aa_oid_t classid, aa_oid_t roleid,
                        uint64_t mask) {
  std::vector<aa_acl_item_t> items;
  aa_oid_t owner;
  uint64_t all;

  if (NULL != s && NULL != env) {
    aa_stamp_t st = stamp_of(s);

    if (!(st == s->mask_stamp)) {
      s->masks.clear();
      s->mask_stamp = st;
    }
    auto hit = s->masks.find({dbid, objid, roleid});
    if (s->masks.end() != hit)
      return hit->second & mask;
  }
  /* Every privilege, so any mask asked of it later is answered here. */
  owner_and_acl(env, s, dbid, objid, classid, &owner, &items);
  all = aa_acl_mask(env, s, dbid, items.data(), items.size(), roleid, owner,
                    ~0ULL, false);
  if (NULL != s && NULL != env) {
    /* Bounded: a session that reads many objects starts again. */
    if (s->masks.size() >= MASKS_KEPT)
      s->masks.clear();
    s->masks[{dbid, objid, roleid}] = all;
  }
  return all & mask;
}

/* Whether s keeps a host's object's mask, and the mask; see aa.h. */
bool aa_kept_mask(aa_session_t *s, aa_oid_t classid, aa_oid_t objid,
                  aa_oid_t roleid, uint64_t *all) {
  aa_stamp_t st = stamp_of(s);

  if (!(st == s->kept_stamp)) {
    s->kept.clear();
    s->kept_stamp = st;
  }
  auto hit = s->kept.find({classid, objid, roleid});
  if (s->kept.end() == hit)
    return false;
  *all = hit->second;
  return true;
}

/*
 * Keep a host's object's mask in s, after aa_kept_mask() missed it: as of
 * the stamp that call saw, so a change since makes the next call drop it.
 */
void aa_keep_mask(aa_session_t *s, aa_oid_t classid, aa_oid_t objid,
                  aa_oid_t roleid, uint64_t all) {
  /* Bounded: a session that checks many objects starts again. */
  if (s->kept.size() >= MASKS_KEPT)
    s->kept.clear();
  s->kept[{classid, objid, roleid}] = all;
}

/* Forget every host's object's mask s keeps; see aa.h. */
void aa_forget_kept_masks(aa_session_t *s) { s->kept.clear(); }

/* Whether roleid may connect to the database dbid, and if not, why. */
bool aa_connect_okay(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     aa_oid_t roleid, const char **why) {
  std::vector<aa_acl_item_t> items;
  aa_object_t o;

  *why = NULL;
  if (aa_is_superuser(env, s, roleid))
    return true;
  if (NULL == env || !aa_find_object(env, s, aa_key_t(dbid, 0), &o)) {
    o.owner = AA_BOOTSTRAP_SUPERUSER;
    o.has_acl = false;
  }
  items = o.has_acl ? aa_acl_items(o.acl)
                    : aa_acl_default(AA_CLASS_DATABASE, o.owner);
  if (0 == aa_acl_mask(env, s, dbid, items.data(), items.size(), roleid,
                       o.owner, AA_CONNECT, true))
    *why = "has no CONNECT privilege on database";
  return NULL == *why;
}
