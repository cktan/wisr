/*
 * aa.h -- roles, authentication and authorization: the interface
 * (README.md).
 *
 * aa keeps what every database of a server shares about who may do
 * what: the roles and their memberships, and each object's owner and
 * ACL, in the sm database pg_global (ROLE.md section 3). It knows no
 * SQL and no PostgreSQL: digres calls it, and turns what it hands
 * back into catalog rows, privilege checks and messages.
 *
 * Every call returns 0 on success and -1 on failure, the failure's
 * code and message in the thread's error state (aa_errnum(),
 * aa_errmsg()); a lookup that finds nothing returns false. aa never
 * throws.
 */
#ifndef AA_H
#define AA_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* sm's home directory, which aa keeps pg_global in (sm.h). */
typedef struct sm_env_t sm_env_t;

/* An object's id: PostgreSQL's Oid. */
typedef uint32_t aa_oid_t;

/* The oids aa knows of: PostgreSQL's (access/transam.h, pg_authid.dat). */
#define AA_BOOTSTRAP_SUPERUSER 10
#define AA_FIRST_UNPINNED_OID 12000
#define AA_FIRST_NORMAL_OID 16384

/* The name of the sm database aa keeps; no client connects to it. */
#define AA_DBNAME "pg_global"

/* ---- errors ---- */

enum aa_errnum_t {
  AA_OK = 0,
  AA_ERR_SM = 1,       /* sm failed; the message is sm's */
  AA_ERR_NODB = 2,     /* no pg_global: a data directory made before it */
  AA_ERR_NOTABLE = 3,  /* pg_global lacks a table: an older initdb made it */
  AA_ERR_CONFLICT = 4, /* a row another session changed meanwhile */
  AA_ERR_LIMIT = 5     /* no oid is left */
};

/* The thread's last error, and its message. */
aa_errnum_t aa_errnum(void);
const char *aa_errmsg(void);

/* ---- what aa keeps ---- */

/* A role, as pg_authid has it. */
struct aa_role_t {
  aa_oid_t oid = 0;
  std::string rolname;
  bool rolsuper = false;
  bool rolinherit = true;
  bool rolcreaterole = false;
  bool rolcreatedb = false;
  bool rolcanlogin = false;
  bool rolreplication = false;
  bool rolbypassrls = false;
  int32_t rolconnlimit = -1;
  /* The SCRAM secret of its password, if has_password. */
  bool has_password = false;
  std::string rolpassword;
  /* When its password stops being accepted, if has_validuntil:
   * PostgreSQL's TimestampTz, microseconds since 2000-01-01 UTC. */
  bool has_validuntil = false;
  int64_t rolvaliduntil = 0;
};

/* A membership of a role in another, as pg_auth_members has it. */
struct aa_member_t {
  aa_oid_t oid = 0;
  aa_oid_t roleid = 0;  /* the role */
  aa_oid_t member = 0;  /* its member */
  aa_oid_t grantor = 0; /* who granted it */
  bool admin_option = false;
  bool inherit_option = false;
  bool set_option = false;
};

/* Every role and membership, each in oid order: a state of pg_global. */
struct aa_roles_t {
  std::vector<aa_role_t> roles;
  std::vector<aa_member_t> members;
};

/*
 * An object's owner and privileges, by the database it is in, by sm's
 * id (dbid), and its oid there: a table's, a view's, a sequence's or a
 * schema's; or, with objid 0, the database's own (ROLE.md section 3).
 */
struct aa_object_t {
  uint64_t dbid = 0;
  aa_oid_t objid = 0;
  aa_oid_t classid = 0; /* pg_class's, pg_namespace's or pg_database's */
  aa_oid_t owner = 0;
  /* Its ACL, aa_acl_text()'s, if has_acl. */
  bool has_acl = false;
  std::string acl;
};

/* An object's key: the database's id and the object's oid there. */
typedef std::pair<uint64_t, aa_oid_t> aa_key_t;

/* Every object's entry, by key: a state of pg_global. */
typedef std::map<aa_key_t, aa_object_t> aa_objects_t;

/*
 * An item of an ACL: its privilege bits, grant options among them, as
 * PostgreSQL's AclItem has them (utils/acl.h).
 */
struct aa_acl_item_t {
  aa_oid_t grantee = 0;
  aa_oid_t grantor = 0;
  uint64_t privs = 0;
};

/* ---- ACLs ---- */

/* The catalogs an object's class is named by: PostgreSQL's oids. */
#define AA_CLASS_RELATION 1259
#define AA_CLASS_DATABASE 1262
#define AA_CLASS_NAMESPACE 2615

/* The grantee every role is: PUBLIC, PostgreSQL's ACL_ID_PUBLIC. */
#define AA_PUBLIC 0

/* Privilege bits, PostgreSQL's (nodes/parsenodes.h, utils/acl.h). */
#define AA_INSERT (1ULL << 0)
#define AA_SELECT (1ULL << 1)
#define AA_UPDATE (1ULL << 2)
#define AA_DELETE (1ULL << 3)
#define AA_TRUNCATE (1ULL << 4)
#define AA_REFERENCES (1ULL << 5)
#define AA_TRIGGER (1ULL << 6)
#define AA_EXECUTE (1ULL << 7)
#define AA_USAGE (1ULL << 8)
#define AA_CREATE (1ULL << 9)
#define AA_CREATE_TEMP (1ULL << 10)
#define AA_CONNECT (1ULL << 11)
#define AA_MAINTAIN (1ULL << 14)
#define AA_ALL_RIGHTS_RELATION                                                 \
  (AA_INSERT | AA_SELECT | AA_UPDATE | AA_DELETE | AA_TRUNCATE |              \
   AA_REFERENCES | AA_TRIGGER | AA_MAINTAIN)
#define AA_ALL_RIGHTS_SCHEMA (AA_USAGE | AA_CREATE)
#define AA_ALL_RIGHTS_DATABASE (AA_CREATE | AA_CREATE_TEMP | AA_CONNECT)

/*
 * The ACL an object of class classid owned by owner has when it keeps
 * none, as PostgreSQL's acldefault(): a relation's and a schema's owner
 * has every right; a database's too, and PUBLIC its CONNECT and
 * TEMPORARY.
 */
std::vector<aa_acl_item_t> aa_acl_default(aa_oid_t classid, aa_oid_t owner);

/*
 * An ACL as pg_global keeps it: "grantee,grantor,privs" an item, by
 * role oid, space-separated, so a role's rename leaves it be.
 */
std::string aa_acl_text(const std::vector<aa_acl_item_t> &items);
std::vector<aa_acl_item_t> aa_acl_items(const std::string &text);

/* What two ACLs grant alike: the items of a in b, with what both grant. */
std::vector<aa_acl_item_t>
aa_acl_intersect(const std::vector<aa_acl_item_t> &a,
                 const std::vector<aa_acl_item_t> &b);

/* ---- the process's state: pg_global, as last committed ---- */

typedef struct aa_env_t aa_env_t;

/*
 * Make pg_global in the attached home directory env, for initdb: the
 * database, its tables and oid sequence, the bootstrap superuser,
 * named superuser, with the SCRAM secret of its password, or none if
 * secret is NULL, and PostgreSQL's predefined roles.
 */
int aa_create(sm_env_t *env, const char *superuser, const char *secret);

/*
 * Read pg_global into a state every session shares, once, at the
 * start: AA_ERR_NODB if the home directory has none, AA_ERR_NOTABLE
 * if it lacks one of its tables.
 */
int aa_load(sm_env_t *env, aa_env_t **out);

/* Free what aa_load() read: at the end, once no session uses it. */
void aa_close(aa_env_t *env);

/* The roles and the objects' entries, as last committed. */
std::shared_ptr<const aa_roles_t> aa_roles(aa_env_t *env);
std::shared_ptr<const aa_objects_t> aa_objects(aa_env_t *env);

/* ---- a session's: its transaction's changes ---- */

typedef struct aa_session_t aa_session_t;

/* A session's state, and its end: one a session of the host's. */
aa_session_t *aa_session_new(aa_env_t *env);
void aa_session_free(aa_session_t *s);

/*
 * The roles as the session sees them: with its transaction's own
 * changes, else as last committed.
 */
std::shared_ptr<const aa_roles_t> aa_session_roles(aa_session_t *s);

/* A new oid for a role or a membership, from pg_global's sequence. */
int aa_new_oid(aa_session_t *s, aa_oid_t *out);

/*
 * Role DDL: add or replace a role or a membership, or remove one by
 * oid, in the session's transaction; it sees them at once, and every
 * other session after aa_commit().
 */
void aa_put_role(aa_session_t *s, const aa_role_t &role);
void aa_delete_role(aa_session_t *s, aa_oid_t oid);
void aa_put_member(aa_session_t *s, const aa_member_t &member);
void aa_delete_member(aa_session_t *s, aa_oid_t oid);

/*
 * The entry of the object key as the session sees it, with its
 * transaction's changes, into *out: false if it has none.  s may be
 * NULL, for the entry as last committed.
 */
bool aa_find_object(aa_env_t *env, aa_session_t *s, const aa_key_t &key,
                    aa_object_t *out);

/*
 * Make object the entry of its key, or remove the entry of key, in the
 * session's transaction, at its subtransaction's nesting level, which
 * aa_subabort() at that level undoes: written at aa_commit() and
 * aa_post_commit().
 */
void aa_set_object(aa_session_t *s, int level, const aa_object_t &object);
void aa_forget_object(aa_session_t *s, int level, const aa_key_t &key);

/*
 * Wait out another session's commit of objects' entries in flight, from
 * its aa_commit() to its aa_post_commit() or aa_abort(): an entry read
 * after it is as that commit leaves it. For a change, before it reads the
 * entry it changes; a session in its own commit does not wait.
 */
void aa_object_settled(aa_session_t *s);

/* Remove every entry of the database dbid, after the commit. */
void aa_forget_database(aa_session_t *s, uint64_t dbid);

/*
 * A temporary object's owner and ACL: the session's alone, never
 * written, set at the subtransaction's nesting level, so an abort of it
 * or of the transaction undoes them, as an object's entry. A lookup is
 * false if the object has none.
 */
void aa_set_temp_owner(aa_session_t *s, int level, aa_oid_t objid,
                       aa_oid_t owner);
void aa_set_temp_acl(aa_session_t *s, int level, aa_oid_t objid,
                     const std::string &acl);
bool aa_temp_owner(aa_session_t *s, aa_oid_t objid, aa_oid_t *owner);
bool aa_temp_acl(aa_session_t *s, aa_oid_t objid, std::string *acl);
void aa_forget_temp(aa_session_t *s, int level, aa_oid_t objid);

/*
 * The entries, as the session sees them, of the objects roleid owns,
 * into *owned, and of those whose ACL names it other than as their
 * owner, into *granted, if not NULL: of every database.  s may be
 * NULL, for the entries as last committed.
 */
void aa_objects_of(aa_env_t *env, aa_session_t *s, aa_oid_t roleid,
                   std::vector<aa_object_t> *owned,
                   std::vector<aa_object_t> *granted);

/* A subtransaction at level commits, its changes its parent's; or aborts. */
void aa_subcommit(aa_session_t *s, int level);
void aa_subabort(aa_session_t *s, int level);

/*
 * At the commit of the session's transaction, before the database's:
 * write its role changes to pg_global, in one transaction there, a row
 * another session changed meanwhile failing it with AA_ERR_CONFLICT;
 * and each object's entry that is to grant less, as granting only what
 * it grants before and after (ROLE.md section 8, question 7).  On
 * failure nothing of it is written.
 */
int aa_commit(aa_session_t *s);

/*
 * After the database's commit: write each object's entry as the
 * transaction left it, and remove the entries of the databases it
 * dropped. The changes are forgotten either way; on failure the
 * entries not written leave their objects their database's owner's.
 */
int aa_post_commit(aa_session_t *s);

/*
 * At the abort of the session's transaction: forget its changes, and
 * put back what aa_commit() wrote before a database's commit that
 * failed. On failure the entries not put back grant less, not more.
 */
int aa_abort(aa_session_t *s);

/*
 * Whether another session has replaced the roles since this one last
 * asked: true once, and then they are taken to be fresh.
 */
bool aa_roles_moved(aa_session_t *s);

/* What aa_objects_moved() says changed. */
struct aa_moved_t {
  std::vector<aa_oid_t> oids; /* the objects of the database asked about */
  bool all = false;           /* more changed than aa keeps of */
  bool namespaces = false;    /* a schema's entry of that database */
  bool databases = false;     /* a database's entry, of any database */
};

/*
 * Whether aa_objects_moved() has something to say: a load, for a host
 * that asks at every lock.
 */
bool aa_objects_stale(const aa_session_t *s);

/*
 * Whether another session has changed objects' entries since this one
 * last asked: true once, with what changed for the database dbid in
 * *out. With out->all, oids is not whole, and namespaces and databases
 * are set. An entry that went has no class: neither flag says so, and
 * the schema's own drop is the caller's to see.
 */
bool aa_objects_moved(aa_session_t *s, uint64_t dbid, aa_moved_t *out);

/* ---- authentication: ROLE.md section 5 ---- */

/* How a client proves who it is. */
enum aa_method_t {
  AA_PEER = 1,    /* the OS user at the socket's other end */
  AA_PASSWORD = 2 /* the role's SCRAM-SHA-256 secret */
};

/*
 * What a client connecting as rolname must prove, before the exchange:
 * on the local socket (local), peer if os_user, the OS user at its other
 * end, is rolname, or if os_owner, that user owns the data directory and
 * rolname is the bootstrap superuser's name, *as_owner then set; else
 * the password. Over TCP, the password. os_user is NULL if the kernel
 * cannot say, or over TCP.
 */
aa_method_t aa_auth_method(aa_env_t *env, const char *rolname, bool local,
                           const char *os_user, bool os_owner, bool *as_owner);

/*
 * The SCRAM secret of rolname's password, into *secret, for the
 * exchange: false, with why for the log, if the role does not exist, has
 * no password, or is past VALID UNTIL at now (PostgreSQL's TimestampTz).
 */
bool aa_role_secret(aa_env_t *env, const char *rolname, int64_t now,
                    std::string *secret, std::string *why);

/*
 * After the exchange: whether role may log in. If not, *why says why, for
 * the log: not LOGIN, past VALID UNTIL at now, or past its CONNECTION
 * LIMIT with sessions of its own open, this one among them (a superuser
 * has no limit). as_owner, the data directory's owner, is always let in.
 */
bool aa_login_okay(const aa_role_t &role, int64_t now, int sessions,
                   bool as_owner, const char **why);

/* ---- authorization: who holds which privilege ---- */

/* PostgreSQL's pg_database_owner, the role a database's owner is in. */
#define AA_PG_DATABASE_OWNER 6171

/* The grant option bits of an ACL item's privileges, PostgreSQL's. */
#define AA_ALL_GOPTION_BITS (0xFFFFFFFFULL << 32)

/* The database of a call made before a session has one. */
#define AA_NO_DATABASE UINT64_MAX

/*
 * Each call below reads the roles and the objects' entries as session s
 * sees them, or as last committed if s is NULL, or none if env is NULL
 * too, in the database dbid, by sm's id, whose owner is a member of
 * pg_database_owner (AA_NO_DATABASE: none). s caches what they find,
 * until the roles or the entries change.
 */

/* Whether roleid is a superuser: the bootstrap superuser always is. */
bool aa_is_superuser(aa_env_t *env, aa_session_t *s, aa_oid_t roleid);

/*
 * Whether member has role's privileges: it is role, a superuser, or a
 * member of it through grants each with INHERIT (has_privs_of_role()).
 */
bool aa_has_privs_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                          aa_oid_t member, aa_oid_t role);

/*
 * Whether member may SET ROLE to role: it is role, a superuser, or a
 * member of it through grants each with SET (member_can_set_role()).
 */
bool aa_can_set_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     aa_oid_t member, aa_oid_t role);

/*
 * Whether member is role or a member of it through any grants, or, if
 * not nosuper, a superuser (is_member_of_role(), and its _nosuper).
 */
bool aa_is_member_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                          aa_oid_t member, aa_oid_t role, bool nosuper);

/*
 * Whether member may administer role: a superuser, or a member, through
 * any grants, of a role granted role WITH ADMIN OPTION; never role
 * itself (is_admin_of_role()).
 */
bool aa_is_admin_of_role(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                         aa_oid_t member, aa_oid_t role);

/*
 * The role whose privileges member has, nearest first, granted role
 * WITH ADMIN OPTION, superusers aside; 0 if none (select_best_admin()).
 */
aa_oid_t aa_select_best_admin(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                              aa_oid_t member, aa_oid_t role);

/*
 * The privileges of mask roleid holds by the ACL items, n of them, of an
 * object owner owns: granted it, PUBLIC, or a role whose privileges it
 * has, and as one of those the owner's grant options. With any, as soon
 * as one is found (PostgreSQL's aclmask(), ACLMASK_ANY).
 */
uint64_t aa_acl_mask(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     const aa_acl_item_t *items, size_t n, aa_oid_t roleid,
                     aa_oid_t owner, uint64_t mask, bool any);

/* Likewise, granted roleid itself only, or as the owner (aclmask_direct()). */
uint64_t aa_acl_mask_direct(const aa_acl_item_t *items, size_t n,
                            aa_oid_t roleid, aa_oid_t owner, uint64_t mask,
                            bool any);

/*
 * Who roleid grants or revokes privs as on an object of owner with the
 * ACL items: the owner, if roleid is it or a superuser; else the role
 * whose privileges it has with the most of their grant options, nearest
 * first, into *grantor, and those options into *goptions; roleid and none
 * if no role has any (select_best_grantor()).
 */
void aa_select_best_grantor(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                            aa_oid_t roleid, uint64_t privs,
                            const aa_acl_item_t *items, size_t n,
                            aa_oid_t owner, aa_oid_t *grantor,
                            uint64_t *goptions);

/*
 * The privileges of mask roleid holds on the object objid of the database
 * dbid, of class classid (AA_CLASS_RELATION, ...), as s sees it: by its
 * ACL, or its owner's default if it keeps none, its owner a temporary
 * object's, its entry's, or else its database's (aa_acl_mask()). Every
 * privilege the role holds on the object is computed once, and kept in
 * s until a role, a membership, an owner or an ACL changes: a check
 * repeated is a lookup, with no lock taken.
 */
uint64_t aa_object_mask(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                        aa_oid_t objid, aa_oid_t classid, aa_oid_t roleid,
                        uint64_t mask);

/*
 * A host's own objects' masks, kept in s as aa_object_mask() keeps its
 * own: for an object whose owner and ACL the host reads, not pg_global,
 * as digres's functions in pg_proc. aa_kept_mask() says whether s keeps
 * *all, every privilege roleid holds on objid of class classid, as
 * aa_keep_mask() put it after aa_kept_mask() missed it. Each holds until a
 * role, a membership or an object's entry changes, or aa_forget_kept_masks(),
 * the host's ACLs having changed.
 */
bool aa_kept_mask(aa_session_t *s, aa_oid_t classid, aa_oid_t objid,
                  aa_oid_t roleid, uint64_t *all);
void aa_keep_mask(aa_session_t *s, aa_oid_t classid, aa_oid_t objid,
                  aa_oid_t roleid, uint64_t all);
void aa_forget_kept_masks(aa_session_t *s);

/*
 * The last of ROLE.md section 5's checks, once the database dbid is
 * chosen: whether roleid may connect to it, a superuser, or one with
 * CONNECT on it by its ACL. If not, *why says why, for the log.
 */
bool aa_connect_okay(aa_env_t *env, aa_session_t *s, uint64_t dbid,
                     aa_oid_t roleid, const char **why);

#endif /* AA_H */
