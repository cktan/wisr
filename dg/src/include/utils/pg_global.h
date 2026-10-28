/*-------------------------------------------------------------------------
 *
 * pg_global.h
 *	  The database of what every database shares: the roles
 *	  (sm/ROLE.md section 3).
 *
 * src/include/utils/pg_global.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_GLOBAL_H
#define PG_GLOBAL_H

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "access/htup.h"
#include "access/tupdesc.h"
#include "datatype/timestamp.h"

typedef struct sm_env_t sm_env_t;

/* The name of the database in sm; no client connects to it. */
#define PG_GLOBAL_DBNAME "pg_global"

/* A role, as pg_authid has it. */
typedef struct pg_global_role_t {
  Oid oid = InvalidOid;
  std::string rolname;
  bool rolsuper = false;
  bool rolinherit = true;
  bool rolcreaterole = false;
  bool rolcreatedb = false;
  bool rolcanlogin = false;
  bool rolreplication = false;
  bool rolbypassrls = false;
  int32 rolconnlimit = -1;
  /* The SCRAM secret of its password, if has_password. */
  bool has_password = false;
  std::string rolpassword;
  /* When its password stops being accepted, if has_validuntil. */
  bool has_validuntil = false;
  TimestampTz rolvaliduntil = 0;
} pg_global_role_t;

/* A membership of a role in another, as pg_auth_members has it. */
typedef struct pg_global_member_t {
  Oid oid = InvalidOid;
  Oid roleid = InvalidOid;  /* the role */
  Oid member = InvalidOid;  /* its member */
  Oid grantor = InvalidOid; /* who granted it */
  bool admin_option = false;
  bool inherit_option = false;
  bool set_option = false;
} pg_global_member_t;

/* Every role and membership, each in oid order: a state of pg_global. */
typedef struct pg_global_roles_t {
  std::vector<pg_global_role_t> roles;
  std::vector<pg_global_member_t> members;
} pg_global_roles_t;

/*
 * An object's owner and privileges, by the database it is in, by sm's id
 * (dbid), and its oid there: a table's, a view's, a sequence's or a
 * schema's; or, with objid 0, the database's own (sm/ROLE.md section 3).
 */
typedef struct pg_global_object_t {
  uint64_t dbid = 0;
  Oid objid = InvalidOid;
  Oid classid = InvalidOid; /* pg_class's, pg_namespace's or pg_database's */
  Oid owner = InvalidOid;
  /* Its aclitem[] in PostgreSQL's text form, if has_acl. */
  bool has_acl = false;
  std::string acl;
} pg_global_object_t;

/* Every object's entry, by dbid and objid: a state of pg_global. */
typedef std::map<std::pair<uint64_t, Oid>, pg_global_object_t>
    pg_global_objects_t;

extern int pg_global_create(sm_env_t *env, const char *superuser,
                            const char *secret);
extern void pg_global_load(sm_env_t *env);
extern bool pg_global_is_loaded(void);
extern std::shared_ptr<const pg_global_roles_t> pg_global_roles(void);
extern bool pg_global_role_by_oid(Oid oid, pg_global_role_t *out);
extern bool pg_global_role_by_name(const char *name, pg_global_role_t *out);

/* Role DDL: changes the session's transaction makes, written at commit. */
extern Oid pg_global_new_oid(void);
extern void pg_global_put_role(HeapTuple tuple, TupleDesc desc);
extern void pg_global_delete_role(Oid oid);
extern void pg_global_put_member(HeapTuple tuple);
extern void pg_global_delete_member(Oid oid);
extern char *pg_global_role_dependents(Oid roleid);
extern void pg_global_commit(void);
extern void pg_global_abort(void);
extern bool pg_global_epoch_moved(void);

/* Owners: an object's entry, written after the database's commit. */
extern uint64_t pg_global_dbid(Oid dboid);
extern Oid pg_global_owner(Oid objid);
extern bool pg_global_owner_of(Oid objid, Oid *owner);
extern Oid pg_global_database_owner(uint64_t dbid);
extern void pg_global_set_owner(uint64_t dbid, Oid classid, Oid objid,
                                Oid owner, bool temp);
extern void pg_global_forget(uint64_t dbid, Oid objid);
extern void pg_global_forget_database(uint64_t dbid);
extern void pg_global_subcommit(void);
extern void pg_global_subabort(void);
extern void pg_global_post_commit(void);
extern void pg_global_objects_moved(void);

#endif /* PG_GLOBAL_H */
