/*-------------------------------------------------------------------------
 *
 * pg_global.h
 *	  The database of what every database shares: the roles, and each
 *	  object's owner and ACL, which aa keeps (aa/ROLE.md section 3).
 *
 * src/include/utils/pg_global.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_GLOBAL_H
#define PG_GLOBAL_H

#include <memory>
#include <vector>

#include "aa.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "utils/acl.h"

/* The name of the database in sm; no client connects to it. */
#define PG_GLOBAL_DBNAME AA_DBNAME

/* What pg_global keeps, as aa has it (aa.h): a role, a membership, every
 * role and membership, an object's owner and ACL, and every object's. */
typedef aa_role_t pg_global_role_t;
typedef aa_member_t pg_global_member_t;
typedef aa_roles_t pg_global_roles_t;
typedef aa_object_t pg_global_object_t;
typedef aa_objects_t pg_global_objects_t;

extern int pg_global_create(sm_env_t *env, const char *superuser,
                            const char *secret);
extern void pg_global_load(sm_env_t *env);
extern bool pg_global_is_loaded(void);
extern aa_env_t *pg_global_env(void);
extern aa_session_t *pg_global_session(void);
extern uint64_t pg_global_aa_dbid(void);
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
extern void pg_global_set_acl(uint64_t dbid, Oid classid, Oid objid, Oid owner,
                              const Acl *acl, bool temp);
extern Acl *pg_global_acl(uint64_t dbid, Oid objid);
extern void pg_global_change_owner(uint64_t dbid, Oid classid, Oid objid,
                                   Oid owner, bool temp);
extern void pg_global_objects_of(Oid roleid,
                                 std::vector<pg_global_object_t> *owned,
                                 std::vector<pg_global_object_t> *granted);
extern void pg_global_subcommit(void);
extern void pg_global_subabort(void);
extern void pg_global_post_commit(void);
extern bool pg_global_objects_stale(void);
extern bool pg_global_objects_moved(bool *namespaces, bool *databases);
extern void pg_global_settle(void);

#endif /* PG_GLOBAL_H */
