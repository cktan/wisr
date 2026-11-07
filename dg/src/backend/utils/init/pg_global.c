/*-------------------------------------------------------------------------
 *
 * pg_global.c
 *	  The database of what every database shares: the roles, and each
 *	  object's owner and ACL.
 *
 * A role is the whole server's, and in sm no transaction spans two
 * databases, so the roles live in a database of their own, pg_global
 * (aa/ROLE.md section 3), which initdb makes beside postgres and no client
 * connects to.  aa (../aa) keeps it: its tables, the snapshots of it every
 * session reads, a session's changes to it, and the protocol that commits
 * them.  This file is digres's side: it turns catalog tuples and Acls into
 * aa's structs and back, passes aa the session's database and nesting
 * level, invalidates the caches aa's changes leave stale, and reports aa's
 * errors with ereport().
 *
 * All role data is in pg_global and only there: a database's directory
 * names no role, so a copy of it on another server finds that server's
 * roles (aa/ROLE.md, the invariant).
 *
 * src/backend/utils/init/pg_global.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <algorithm>
#include <map>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/datadir.h"
#include "utils/inval.h"
#include "utils/relcache.h"
#include "utils/pg_global.h"
#include "utils/timestamp.h"

/* Free aa's state of pg_global: never, as shmem is never freed. */
void pg_global_free_t::operator()(aa_env_t *env) const { aa_close(env); }

/* Free a session's state, and close its sm session on pg_global. */
void pg_global_session_free_t::operator()(aa_session_t *s) const {
  aa_session_free(s);
}

/* Report aa's last error at elevel, as PostgreSQL reports its kind. */
static void aa_failed(int elevel) {
  switch (aa_errnum()) {
  case AA_ERR_CONFLICT:
    ereport(elevel,
            (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
             errmsg("could not serialize access due to concurrent update"),
             errdetail("%s", aa_errmsg())));
    break;
  case AA_ERR_LIMIT:
    ereport(elevel, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("%s", aa_errmsg())));
    break;
  case AA_ERR_NODB:
    ereport(elevel, (errmsg("%s", aa_errmsg()),
                     errdetail("It was made before digres kept roles there."),
                     errhint("Run initdb to make a new data directory.")));
    break;
  case AA_ERR_NOTABLE:
    ereport(elevel, (errmsg("%s", aa_errmsg()),
                     errhint("Run initdb to make a new data directory.")));
    break;
  default:
    ereport(elevel, (errmsg("%s", aa_errmsg())));
  }
}

/*
 * Make pg_global in the attached home directory env, for initdb: aa's
 * tables, the bootstrap superuser, named superuser, with the SCRAM secret
 * of its password, or none if secret is NULL, and the predefined roles.
 * 0, or -1 with aa's error set.
 */
int pg_global_create(sm_env_t *env, const char *superuser, const char *secret) {
  return aa_create(env, superuser, secret);
}

/*
 * Read pg_global into the state every session shares.  The postmaster
 * calls this once, at startup, before any session.  A data directory with
 * no pg_global was made before digres kept roles, and is refused
 * (aa/ROLE.md section 8, question 5).
 */
void pg_global_load(sm_env_t *env) {
  aa_env_t *global;

  if (0 != aa_load(env, &global))
    aa_failed(FATAL);
  shmem->pg_global.reset(global);
}

/* Whether the roles were read: not in a process with no data directory. */
bool pg_global_is_loaded(void) { return nullptr != shmem->pg_global; }

/* aa's state of pg_global, for a call of aa's own; loaded at startup. */
aa_env_t *pg_global_env(void) { return shmem->pg_global.get(); }

/* This session's aa state, made at its first use; NULL with no session. */
static aa_session_t *session_state(void) {
  if (nullptr == tcb)
    return NULL;
  if (nullptr == tcb->pg_global)
    tcb->pg_global.reset(aa_session_new(shmem->pg_global.get()));
  return tcb->pg_global.get();
}

/*
 * This session's aa state, for a call of aa's own: made at its first use;
 * NULL with no session, or no pg_global loaded.
 */
aa_session_t *pg_global_session(void) {
  return pg_global_is_loaded() ? session_state() : NULL;
}

/* sm's id of this session's database, or AA_NO_DATABASE before it has one. */
uint64_t pg_global_aa_dbid(void) {
  return OidIsValid(MyDatabaseId) ? pg_global_dbid(MyDatabaseId)
                                  : AA_NO_DATABASE;
}

/* This session's aa state, if it made one; NULL if not. */
static aa_session_t *session_if_any(void) {
  return nullptr != tcb ? tcb->pg_global.get() : NULL;
}

/*
 * The roles as this session sees them: with its transaction's own changes,
 * else as last committed; none in a process that loaded no data directory.
 */
std::shared_ptr<const pg_global_roles_t> pg_global_roles(void) {
  aa_session_t *s;

  if (!pg_global_is_loaded())
    return std::make_shared<const pg_global_roles_t>();
  s = session_if_any();
  return NULL != s ? aa_session_roles(s) : aa_roles(shmem->pg_global.get());
}

/* The role whose oid is oid, into *out: false if there is none. */
bool pg_global_role_by_oid(Oid oid, pg_global_role_t *out) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();

  for (const pg_global_role_t &role : all->roles)
    if (role.oid == oid) {
      *out = role;
      return true;
    }
  return false;
}

/* The role named name, into *out: false if there is none. */
bool pg_global_role_by_name(const char *name, pg_global_role_t *out) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();

  for (const pg_global_role_t &role : all->roles)
    if (role.rolname == name) {
      *out = role;
      return true;
    }
  return false;
}

/* A new oid for a role or a membership, from pg_global's sequence. */
Oid pg_global_new_oid(void) {
  aa_oid_t oid;

  if (0 != aa_new_oid(session_state(), &oid))
    aa_failed(ERROR);
  return oid;
}

/*
 * Add or replace a role, pg_authid's tuple of desc, in the session's
 * transaction: the session sees it at its next command, every other at
 * the commit.
 */
void pg_global_put_role(HeapTuple tuple, TupleDesc desc) {
  Datum values[Natts_pg_authid];
  bool isnull[Natts_pg_authid];
  pg_global_role_t role;

  heap_deform_tuple(tuple, desc, values, isnull);
  role.oid = DatumGetObjectId(values[Anum_pg_authid_oid - 1]);
  role.rolname = NameStr(*DatumGetName(values[Anum_pg_authid_rolname - 1]));
  role.rolsuper = DatumGetBool(values[Anum_pg_authid_rolsuper - 1]);
  role.rolinherit = DatumGetBool(values[Anum_pg_authid_rolinherit - 1]);
  role.rolcreaterole = DatumGetBool(values[Anum_pg_authid_rolcreaterole - 1]);
  role.rolcreatedb = DatumGetBool(values[Anum_pg_authid_rolcreatedb - 1]);
  role.rolcanlogin = DatumGetBool(values[Anum_pg_authid_rolcanlogin - 1]);
  role.rolreplication = DatumGetBool(values[Anum_pg_authid_rolreplication - 1]);
  role.rolbypassrls = DatumGetBool(values[Anum_pg_authid_rolbypassrls - 1]);
  role.rolconnlimit = DatumGetInt32(values[Anum_pg_authid_rolconnlimit - 1]);
  role.has_password = !isnull[Anum_pg_authid_rolpassword - 1];
  if (role.has_password)
    role.rolpassword =
        TextDatumGetCString(values[Anum_pg_authid_rolpassword - 1]);
  role.has_validuntil = !isnull[Anum_pg_authid_rolvaliduntil - 1];
  if (role.has_validuntil)
    role.rolvaliduntil =
        DatumGetTimestampTz(values[Anum_pg_authid_rolvaliduntil - 1]);
  aa_put_role(session_state(), role);
  /* The session's caches of pg_authid go at its next command. */
  CacheInvalidateCatalog(AuthIdRelationId);
}

/* Remove the role oid, in the session's transaction. */
void pg_global_delete_role(Oid oid) {
  aa_delete_role(session_state(), oid);
  CacheInvalidateCatalog(AuthIdRelationId);
}

/* Add or replace a membership, pg_auth_members's tuple, likewise. */
void pg_global_put_member(HeapTuple tuple) {
  Form_pg_auth_members form = (Form_pg_auth_members)GETSTRUCT(tuple);
  pg_global_member_t member;

  member.oid = form->oid;
  member.roleid = form->roleid;
  member.member = form->member;
  member.grantor = form->grantor;
  member.admin_option = form->admin_option;
  member.inherit_option = form->inherit_option;
  member.set_option = form->set_option;
  aa_put_member(session_state(), member);
  /* The session's caches of pg_auth_members go at its next command. */
  CacheInvalidateCatalog(AuthMemRelationId);
}

/* Remove the membership oid, in the session's transaction. */
void pg_global_delete_member(Oid oid) {
  aa_delete_member(session_state(), oid);
  CacheInvalidateCatalog(AuthMemRelationId);
}

/*
 * The entries, as the session sees them, of the objects the role roleid
 * owns, into *owned, and of those whose ACL names it, into *granted, if
 * not NULL: of every database, a database's own among them (objid 0).
 */
void pg_global_objects_of(Oid roleid, std::vector<pg_global_object_t> *owned,
                          std::vector<pg_global_object_t> *granted) {
  if (pg_global_is_loaded())
    aa_objects_of(shmem->pg_global.get(), session_if_any(), roleid, owned,
                  granted);
}

/* One line of DROP ROLE's DETAIL, by the oid PostgreSQL sorts it by. */
struct dependent_line_t {
  Oid oid;
  char deptype; /* PostgreSQL's: 'a' an ACL's, 'o' an owner's */
  std::string text;
};

/*
 * The DETAIL line for the entry o, owner or ACL by deptype, into *line: in
 * this database, the object; a database, itself; false for an object of
 * another database, counted in *remote, or one gone.
 */
static bool dependent_line(const pg_global_object_t &o, char deptype,
                           std::map<uint64_t, int> *remote,
                           dependent_line_t *line) {
  const char *what = 'o' == deptype ? "owner of " : "privileges for ";
  ObjectAddress address;
  char *desc;

  if (InvalidOid == o.objid) {
    Oid dboid = FirstNormalObjectId + (Oid)o.dbid;
    char *name = datadir_database_name(dboid);

    if (NULL == name)
      return false;
    *line = {dboid, deptype, std::string(what) + "database " + name};
    return true;
  }
  if (pg_global_dbid(MyDatabaseId) != o.dbid) {
    (*remote)[o.dbid]++;
    return false;
  }
  address.classId = o.classid;
  address.objectId = o.objid;
  address.objectSubId = 0;
  if (NULL == (desc = getObjectDescription(&address, true)))
    return false;
  *line = {o.objid, deptype, std::string(what) + desc};
  return true;
}

/* The most lines DROP ROLE's DETAIL has, as PostgreSQL's MAX_REPORTED_DEPS. */
#define MAX_REPORTED 100

/*
 * What depends on the role roleid, as DROP ROLE's DETAIL lists it, one a
 * line, palloc'd; NULL if nothing does: what it owns and what an ACL
 * grants it, in this database and of the databases, a membership it
 * granted, each by its oid, as PostgreSQL's checkSharedDependencies()
 * sorts them; then how many in each other database.  All of it is in
 * pg_global (aa/ROLE.md section 4: no pg_shdepend).
 */
char *pg_global_role_dependents(Oid roleid) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();
  std::vector<pg_global_object_t> owned, granted;
  std::vector<dependent_line_t> lines;
  std::map<uint64_t, int> remote;
  dependent_line_t line;
  StringInfoData out;
  int shown = 0, hidden = 0;

  for (const pg_global_member_t &m : all->members) {
    pg_global_role_t member, role;

    if (m.grantor != roleid || !pg_global_role_by_oid(m.member, &member) ||
        !pg_global_role_by_oid(m.roleid, &role))
      continue;
    lines.push_back({m.oid, 'a',
                     "privileges for membership of role " + member.rolname +
                         " in role " + role.rolname});
  }
  pg_global_objects_of(roleid, &owned, &granted);
  for (const pg_global_object_t &o : owned)
    if (dependent_line(o, 'o', &remote, &line))
      lines.push_back(line);
  for (const pg_global_object_t &o : granted)
    if (dependent_line(o, 'a', &remote, &line))
      lines.push_back(line);
  std::sort(lines.begin(), lines.end(),
            [](const dependent_line_t &a, const dependent_line_t &b) {
              return a.oid != b.oid ? a.oid < b.oid : a.deptype < b.deptype;
            });

  initStringInfo(&out);
  for (const dependent_line_t &l : lines) {
    if (shown >= MAX_REPORTED) {
      hidden++;
      continue;
    }
    if (out.len > 0)
      appendStringInfoChar(&out, '\n');
    appendStringInfoString(&out, l.text.c_str());
    shown++;
  }
  for (const auto &[dbid, count] : remote) {
    char *name = datadir_database_name(FirstNormalObjectId + (Oid)dbid);

    if (NULL == name)
      continue;
    if (shown >= MAX_REPORTED) {
      hidden += count;
      continue;
    }
    if (out.len > 0)
      appendStringInfoChar(&out, '\n');
    appendStringInfo(&out,
                     ngettext("%d object in database %s",
                              "%d objects in database %s", count),
                     count, name);
    shown++;
  }
  if (hidden > 0)
    appendStringInfo(&out,
                     ngettext("\nand %d other object "
                              "(see server log for list)",
                              "\nand %d other objects "
                              "(see server log for list)",
                              hidden),
                     hidden);
  if (0 == out.len) {
    pfree(out.data);
    return NULL;
  }
  return out.data;
}
/* The items of PostgreSQL's ACL acl. */
static std::vector<aa_acl_item_t> acl_items_of(const Acl *acl) {
  std::vector<aa_acl_item_t> out;
  const AclItem *items = ACL_DAT(acl);

  for (int i = 0; i < ACL_NUM(acl); i++)
    out.push_back({items[i].ai_grantee, items[i].ai_grantor,
                   (uint64_t)items[i].ai_privs});
  return out;
}

/* PostgreSQL's ACL of the items given, palloc'd, as acl.c's allocacl(). */
static Acl *acl_of_items(const std::vector<aa_acl_item_t> &items) {
  int n = (int)items.size();
  Acl *acl = (Acl *)palloc0(ACL_N_SIZE(n));
  AclItem *dat;

  SET_VARSIZE(acl, ACL_N_SIZE(n));
  acl->ndim = 1;
  acl->dataoffset = 0;
  acl->elemtype = ACLITEMOID;
  ARR_LBOUND(acl)[0] = 1;
  ARR_DIMS(acl)[0] = n;
  dat = ACL_DAT(acl);
  for (int i = 0; i < n; i++) {
    dat[i].ai_grantee = items[i].grantee;
    dat[i].ai_grantor = items[i].grantor;
    dat[i].ai_privs = (AclMode)items[i].privs;
  }
  return acl;
}

/*
 * The entry of the object key as session s sees it, with its transaction's
 * changes, into *out: false if it has none.  s may be NULL.
 */
static bool find_object(aa_session_t *s, const aa_key_t &key,
                        pg_global_object_t *out) {
  return aa_find_object(shmem->pg_global.get(), s, key, out);
}

/* sm's id of the database whose oid is dboid (utils/init/datadir.c). */
uint64_t pg_global_dbid(Oid dboid) { return dboid - FirstNormalObjectId; }

/* The owner of the database dbid: its entry's, else the superuser. */
Oid pg_global_database_owner(uint64_t dbid) {
  pg_global_object_t object;

  if (pg_global_is_loaded() &&
      find_object(session_if_any(), aa_key_t(dbid, 0), &object))
    return object.owner;
  return BOOTSTRAP_SUPERUSERID;
}

/*
 * The owner of the object objid of this session's database, as the session
 * sees it: a temporary object's, the session's role that made it; else its
 * entry's; else, with none, as a crash between the commits may leave one,
 * the database's owner (aa/ROLE.md section 3).
 */
Oid pg_global_owner(Oid objid) {
  aa_session_t *s;
  pg_global_object_t object;
  aa_oid_t owner;
  uint64_t dbid;

  if (!pg_global_is_loaded())
    return BOOTSTRAP_SUPERUSERID;
  s = session_if_any();
  if (NULL != s && aa_temp_owner(s, objid, &owner))
    return owner;
  dbid = pg_global_dbid(MyDatabaseId);
  if (find_object(s, aa_key_t(dbid, objid), &object))
    return object.owner;
  return pg_global_database_owner(dbid);
}

/*
 * The owner of the object objid of this session's database by its entry,
 * into *owner: false if it has none, as an object initdb made has none
 * until its owner is changed.
 */
bool pg_global_owner_of(Oid objid, Oid *owner) {
  pg_global_object_t object;

  if (!pg_global_is_loaded() ||
      !find_object(session_if_any(),
                   aa_key_t(pg_global_dbid(MyDatabaseId), objid), &object))
    return false;
  *owner = object.owner;
  return true;
}

/*
 * The entry of the object objid, of class classid, in the database dbid,
 * as the session sees it, or a new one of no owner if it has none.
 */
static pg_global_object_t entry_of(aa_session_t *s, uint64_t dbid, Oid classid,
                                   Oid objid) {
  pg_global_object_t object;

  if (!find_object(s, aa_key_t(dbid, objid), &object)) {
    object.dbid = dbid;
    object.objid = objid;
    object.classid = classid;
  }
  return object;
}

/*
 * Make owner the owner of the object objid, of class classid, in the
 * database dbid (objid 0 for the database itself), in the session's
 * transaction: the session sees it at its next command, and it is written
 * after the database's commit.  A temporary object's is the session's
 * alone, and never written.
 */
void pg_global_set_owner(uint64_t dbid, Oid classid, Oid objid, Oid owner,
                         bool temp) {
  aa_session_t *s = session_state();

  if (temp)
    aa_set_temp_owner(s, GetCurrentTransactionNestLevel(), objid, owner);
  else {
    /* An entry another session's commit is writing is read as it leaves it. */
    aa_object_settled(s);
    pg_global_object_t object = entry_of(s, dbid, classid, objid);

    object.owner = owner;
    aa_set_object(s, GetCurrentTransactionNestLevel(), object);
  }
  CacheInvalidateCatalog(classid);
  /* The relation's cache entry has its owner, for a view's rules. */
  if (RelationRelationId == classid && pg_global_dbid(MyDatabaseId) == dbid)
    CacheInvalidateRelcacheByOid(objid);
}

/* The object objid of the database dbid goes: its entry with it. */
void pg_global_forget(uint64_t dbid, Oid objid) {
  aa_session_t *s = session_state();

  aa_forget_temp(s, GetCurrentTransactionNestLevel(), objid);
  aa_forget_object(s, GetCurrentTransactionNestLevel(), aa_key_t(dbid, objid));
}

/* The database dbid goes: every entry of its with it, after the commit. */
void pg_global_forget_database(uint64_t dbid) {
  aa_forget_database(session_state(), dbid);
  CacheInvalidateCatalog(DatabaseRelationId);
}

/*
 * Make acl the ACL of the object objid, of class classid, owned by owner,
 * in the database dbid (objid 0 for the database itself), in the
 * session's transaction, as pg_global_set_owner() makes an owner: GRANT
 * and REVOKE's.  A temporary object's is the session's alone.
 */
void pg_global_set_acl(uint64_t dbid, Oid classid, Oid objid, Oid owner,
                       const Acl *acl, bool temp) {
  aa_session_t *s = session_state();
  std::string text = aa_acl_text(acl_items_of(acl));
  pg_global_object_t object;

  if (temp) {
    aa_set_temp_acl(s, GetCurrentTransactionNestLevel(), objid, text);
  } else {
    if (!find_object(s, aa_key_t(dbid, objid), &object)) {
      object = entry_of(s, dbid, classid, objid);
      object.owner = owner;
    }
    object.has_acl = true;
    object.acl = text;
    aa_set_object(s, GetCurrentTransactionNestLevel(), object);
  }
  CacheInvalidateCatalog(classid);
  /* The relation's cache entry has no ACL, but is a view's: as an owner's. */
  if (RelationRelationId == classid && pg_global_dbid(MyDatabaseId) == dbid)
    CacheInvalidateRelcacheByOid(objid);
}

/*
 * The ACL of the object objid in the database dbid (objid 0 for the
 * database itself), as the session sees it, palloc'd; NULL if it keeps
 * none, which is its owner's default one.
 */
Acl *pg_global_acl(uint64_t dbid, Oid objid) {
  aa_session_t *s;
  pg_global_object_t object;
  std::string text;

  if (!pg_global_is_loaded())
    return NULL;
  s = session_if_any();
  if (NULL != s && pg_global_dbid(MyDatabaseId) == dbid &&
      aa_temp_acl(s, objid, &text))
    return acl_of_items(aa_acl_items(text));
  if (!find_object(s, aa_key_t(dbid, objid), &object) || !object.has_acl)
    return NULL;
  return acl_of_items(aa_acl_items(object.acl));
}

/*
 * ALTER ... OWNER TO: make owner the owner of the object objid, and its
 * ACL, if it keeps one, the same with the old owner's items the new
 * owner's, as PostgreSQL's aclnewowner() makes it.
 */
void pg_global_change_owner(uint64_t dbid, Oid classid, Oid objid, Oid owner,
                            bool temp) {
  /* The old owner and ACL as another session's commit in flight leaves them. */
  if (!temp)
    aa_object_settled(session_state());
  Oid old = DatabaseRelationId == classid ? pg_global_database_owner(dbid)
                                          : pg_global_owner(objid);
  Acl *acl = pg_global_acl(dbid, objid);

  /*
   * Record the ACL read's epoch before set_owner settles again. A revoke
   * between the two reads must conflict with this saved ACL at commit.
   */
  if (NULL != acl)
    pg_global_set_acl(dbid, classid, objid, owner, aclnewowner(acl, old, owner),
                      temp);
  pg_global_set_owner(dbid, classid, objid, owner, temp);
}

/* A subtransaction commits: its changes are its parent's. */
void pg_global_subcommit(void) {
  aa_session_t *s = session_if_any();

  if (NULL != s)
    aa_subcommit(s, GetCurrentTransactionNestLevel());
}

/*
 * A subtransaction aborts: its changes, the last ones made, go.  A
 * relation's cache entry it changed is invalidated by the invalidation
 * messages its abort processes.
 */
void pg_global_subabort(void) {
  aa_session_t *s = session_if_any();

  if (NULL != s)
    aa_subabort(s, GetCurrentTransactionNestLevel());
}

/*
 * Before a change reads the owners and ACLs it changes, as GRANT does:
 * wait out another session's commit of objects in flight, then take in
 * what it changed, so the caches read next are as it left them.
 */
void pg_global_settle(void) {
  aa_object_settled(session_state());
  AcceptInvalidationMessages();
}

/*
 * Whether pg_global_objects_moved() has something to say: asked at every
 * lock, so it is a load for nothing changed.
 */
bool pg_global_objects_stale(void) {
  aa_session_t *s;

  return pg_global_is_loaded() && NULL != (s = session_state()) &&
         aa_objects_stale(s);
}

/*
 * At the start of a transaction: drop what this session's relation cache
 * keeps of the relations whose owner another session changed since it
 * last looked; of every relation if they are more than aa keeps.  True if
 * any object's entry changed, so the caller drops what the syscache keeps
 * of owners and ACLs; *namespaces and *databases say whether a schema's or
 * a database's did, whose rows the plan cache watches.
 */
bool pg_global_objects_moved(bool *namespaces, bool *databases) {
  aa_session_t *s = session_state();
  aa_moved_t moved;

  *namespaces = *databases = false;
  if (!aa_objects_moved(s, pg_global_dbid(MyDatabaseId), &moved) ||
      !OidIsValid(MyDatabaseId))
    return false;
  if (moved.all)
    RelationCacheInvalidate(false);
  else
    for (Oid oid : moved.oids)
      RelationCacheInvalidateEntry(oid);
  *namespaces = moved.namespaces;
  *databases = moved.databases;
  return true;
}

/*
 * At the commit of the session's transaction, before its database's: write
 * what it changed of the roles to pg_global, and what is to grant less as
 * granting only what it grants before and after (aa_commit()).  An error
 * fails the commit.
 */
void pg_global_commit(void) {
  aa_session_t *s = session_if_any();

  if (NULL != s && 0 != aa_commit(s))
    aa_failed(ERROR);
}

/*
 * At the abort of the session's transaction: forget what it changed, and
 * put back the entries written before the database's commit, which failed.
 * An error is a warning: an abort has nowhere to send it, and an entry not
 * put back grants less, not more.
 */
void pg_global_abort(void) {
  aa_session_t *s = session_if_any();

  if (NULL != s && 0 != aa_abort(s))
    ereport(WARNING, (errmsg("could not put back an owner: %s", aa_errmsg())));
}

/*
 * After the database's commit: write the transaction's changes to objects'
 * entries, and the entries of the databases it dropped, which go, to
 * pg_global.  The transaction has committed, so an error is a warning: an
 * entry not written leaves its object its database's owner's.
 */
void pg_global_post_commit(void) {
  aa_session_t *s = session_if_any();

  if (NULL != s && 0 != aa_post_commit(s))
    ereport(WARNING, (errmsg("could not write an owner: %s", aa_errmsg())));
}

/*
 * Whether another session has replaced the roles since this one's caches
 * of them were made: true once, and then they are taken to be fresh.
 */
bool pg_global_epoch_moved(void) {
  aa_session_t *s;

  if (!pg_global_is_loaded() || NULL == (s = session_state()))
    return false;
  return aa_roles_moved(s);
}
