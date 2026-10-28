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

/* Every role, in oid order: one committed state of pg_global. */
typedef struct pg_global_roles_t {
  std::vector<pg_global_role_t> roles;
} pg_global_roles_t;

extern int pg_global_create(sm_env_t *env, const char *superuser,
                            const char *secret);
extern void pg_global_load(sm_env_t *env);
extern bool pg_global_is_loaded(void);
extern std::shared_ptr<const pg_global_roles_t> pg_global_roles(void);
extern bool pg_global_role_by_oid(Oid oid, pg_global_role_t *out);
extern bool pg_global_role_by_name(const char *name, pg_global_role_t *out);

#endif /* PG_GLOBAL_H */
