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

#include <memory>
#include <string>
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

#endif /* PG_GLOBAL_H */
