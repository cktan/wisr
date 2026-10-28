/*-------------------------------------------------------------------------
 *
 * pg_global.c
 *	  The database of what every database shares: the roles.
 *
 * A role is the whole server's, and in sm no transaction spans two
 * databases, so the roles live in a database of their own, pg_global
 * (sm/ROLE.md section 3), which initdb makes beside postgres and no client
 * connects to.  It is an sm database like any other, holding tables of
 * digres's: pg_authid, a role a row, and pg_auth_members, a membership a
 * row, each keyed by its oid, which the sequence role_oid hands out.  sm
 * knows nothing of roles.
 *
 * All role data is in pg_global and only there: a database's directory
 * names no role, so a copy of it on another server finds that server's
 * roles (sm/ROLE.md, the invariant).
 *
 * The postmaster reads every role and membership at startup into one
 * snapshot the sessions share (shmem->pg_global); a lookup reads the
 * snapshot, and no transaction of pg_global's.  Role DDL, which runs
 * outside a transaction block, changes a copy of the snapshot the session
 * alone sees, and at its commit writes what it changed to pg_global, in
 * one transaction there, and replaces the snapshot.  A row another
 * session changed meanwhile fails the commit, as a serialization failure,
 * rather than lose that change.  Every other session drops what it
 * cached of the roles at its next transaction (pg_global_epoch_moved()).
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
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class_d.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/pg_global.h"
#include "utils/timestamp.h"

/* The roles every session reads: shmem->pg_global. */
struct pg_global_t {
  sm_env_t *env = NULL; /* the home directory pg_global is in */
  std::mutex mutex;     /* guards roles, and orders their replacement */
  std::shared_ptr<const pg_global_roles_t> roles;
  /* How many times roles was replaced: a session's caches of them are
   * stale when it has moved (pg_global_epoch_moved()). */
  std::atomic<uint64_t> epoch{0};
  /* Every object's owner and privileges, replaced as a whole, under mutex. */
  std::shared_ptr<const pg_global_objects_t> objects;
  /* How many times objects was replaced, and, for the last of them, which
   * object each replacement changed: a session drops what it caches of
   * those (pg_global_objects_moved()). */
  std::atomic<uint64_t> objects_epoch{0};
  std::deque<std::pair<uint64_t, std::pair<uint64_t, Oid>>> changed;
};

/* How many of the objects changed lately pg_global_t keeps. */
#define CHANGED_KEPT 4096

/* An object's key: the database's id and the object's oid there. */
typedef std::pair<uint64_t, Oid> object_key_t;

/* A change to an object's entry the session's transaction made. */
struct object_change_t {
  int level;        /* the subtransaction's nesting level that made it */
  object_key_t key; /* the object's */
  bool present;     /* false: the entry goes */
  pg_global_object_t object;
};

/* Free the shared roles: never, as shmem is never freed. */
void pg_global_free_t::operator()(pg_global_t *global) const { delete global; }

/* A session's own: its role DDL not yet committed, and its sm session. */
struct pg_global_session_t {
  /* An sm session on pg_global, opened at the first need. */
  sm_session_t *session = NULL;
  /* The shared epoch this session's caches of the roles are of. */
  uint64_t epoch = 0;
  /* While the session's transaction has changed roles: the snapshot the
   * changes began from, and it with the changes, which the session sees. */
  std::shared_ptr<const pg_global_roles_t> base;
  std::shared_ptr<pg_global_roles_t> working;

  /* The objects epoch this session's relation cache is of. */
  uint64_t objects_epoch = 0;
  /* The transaction's changes to objects' entries, in order. */
  std::vector<object_change_t> changes;
  /* The databases dropped by the transaction, whose entries go. */
  std::vector<uint64_t> dropped_dbs;
  /* Entries written before the database's commit, as they were, to put
   * back if it fails (pg_global_commit()). */
  std::vector<std::pair<object_key_t, pg_global_object_t>> to_restore;
  /* The session's temporary objects' owners, never written. */
  std::map<Oid, Oid> temp_owners;

  ~pg_global_session_t() { sm_closedb(session); }
};

/* Free a session's state, and close its sm session on pg_global. */
void pg_global_session_free_t::operator()(pg_global_session_t *s) const {
  delete s;
}

/* pg_authid's columns in pg_global, in order. */
enum authid_column_t {
  A_OID,
  A_ROLNAME,
  A_ROLSUPER,
  A_ROLINHERIT,
  A_ROLCREATEROLE,
  A_ROLCREATEDB,
  A_ROLCANLOGIN,
  A_ROLREPLICATION,
  A_ROLBYPASSRLS,
  A_ROLCONNLIMIT,
  A_ROLPASSWORD,
  A_ROLVALIDUNTIL,
  A_NATTS
};

/* pg_auth_members's columns in pg_global, in order. */
enum members_column_t {
  M_OID,
  M_ROLEID,
  M_MEMBER,
  M_GRANTOR,
  M_ADMIN_OPTION,
  M_INHERIT_OPTION,
  M_SET_OPTION,
  M_NATTS
};

/* pg_object's columns in pg_global, in order. */
enum object_column_t { O_DBID, O_OBJID, O_CLASSID, O_OWNER, O_ACL, O_NATTS };

/* The tables of pg_global, in its schema public, and its oid sequence. */
#define AUTHID_TABLE "pg_authid"
#define MEMBERS_TABLE "pg_auth_members"
#define OBJECT_TABLE "pg_object"
#define OID_SEQUENCE "role_oid"

/* sm's type codes (sm/sm2/TYPE.md section 3) of the columns used here. */
#define CODE_BOOL 1
#define CODE_INT4 3
#define CODE_INT8 4
#define CODE_TEXT 8
#define CODE_TIMESTAMPTZ 14

/* Closes an sm session on pg_global as it goes out of scope. */
struct session_guard_t {
  sm_session_t *session = NULL;
  ~session_guard_t() { sm_closedb(session); }
};

/* Whether two roles are the same in every column. */
static bool same_role(const pg_global_role_t &a, const pg_global_role_t &b) {
  return a.oid == b.oid && a.rolname == b.rolname && a.rolsuper == b.rolsuper &&
         a.rolinherit == b.rolinherit && a.rolcreaterole == b.rolcreaterole &&
         a.rolcreatedb == b.rolcreatedb && a.rolcanlogin == b.rolcanlogin &&
         a.rolreplication == b.rolreplication &&
         a.rolbypassrls == b.rolbypassrls && a.rolconnlimit == b.rolconnlimit &&
         a.has_password == b.has_password &&
         (!a.has_password || a.rolpassword == b.rolpassword) &&
         a.has_validuntil == b.has_validuntil &&
         (!a.has_validuntil || a.rolvaliduntil == b.rolvaliduntil);
}

/* Whether two memberships are the same in every column. */
static bool same_member(const pg_global_member_t &a,
                        const pg_global_member_t &b) {
  return a.oid == b.oid && a.roleid == b.roleid && a.member == b.member &&
         a.grantor == b.grantor && a.admin_option == b.admin_option &&
         a.inherit_option == b.inherit_option && a.set_option == b.set_option;
}

/* pg_authid's columns, as sm_cat_create_table takes them. */
static std::vector<sm_column_spec_t> authid_columns(void) {
  std::vector<sm_column_spec_t> c(A_NATTS);

  c[A_OID] = {"oid", CODE_INT8, -1, true};
  c[A_ROLNAME] = {"rolname", CODE_TEXT, -1, true};
  c[A_ROLSUPER] = {"rolsuper", CODE_BOOL, -1, true};
  c[A_ROLINHERIT] = {"rolinherit", CODE_BOOL, -1, true};
  c[A_ROLCREATEROLE] = {"rolcreaterole", CODE_BOOL, -1, true};
  c[A_ROLCREATEDB] = {"rolcreatedb", CODE_BOOL, -1, true};
  c[A_ROLCANLOGIN] = {"rolcanlogin", CODE_BOOL, -1, true};
  c[A_ROLREPLICATION] = {"rolreplication", CODE_BOOL, -1, true};
  c[A_ROLBYPASSRLS] = {"rolbypassrls", CODE_BOOL, -1, true};
  c[A_ROLCONNLIMIT] = {"rolconnlimit", CODE_INT4, -1, true};
  c[A_ROLPASSWORD] = {"rolpassword", CODE_TEXT, -1, false};
  c[A_ROLVALIDUNTIL] = {"rolvaliduntil", CODE_TIMESTAMPTZ, -1, false};
  return c;
}

/* pg_auth_members's columns, as sm_cat_create_table takes them. */
static std::vector<sm_column_spec_t> members_columns(void) {
  std::vector<sm_column_spec_t> c(M_NATTS);

  c[M_OID] = {"oid", CODE_INT8, -1, true};
  c[M_ROLEID] = {"roleid", CODE_INT8, -1, true};
  c[M_MEMBER] = {"member", CODE_INT8, -1, true};
  c[M_GRANTOR] = {"grantor", CODE_INT8, -1, true};
  c[M_ADMIN_OPTION] = {"admin_option", CODE_BOOL, -1, true};
  c[M_INHERIT_OPTION] = {"inherit_option", CODE_BOOL, -1, true};
  c[M_SET_OPTION] = {"set_option", CODE_BOOL, -1, true};
  return c;
}

/* pg_object's columns, as sm_cat_create_table takes them. */
static std::vector<sm_column_spec_t> object_columns(void) {
  std::vector<sm_column_spec_t> c(O_NATTS);

  c[O_DBID] = {"dbid", CODE_INT8, -1, true};
  c[O_OBJID] = {"objid", CODE_INT8, -1, true};
  c[O_CLASSID] = {"classid", CODE_INT8, -1, true};
  c[O_OWNER] = {"owner", CODE_INT8, -1, true};
  c[O_ACL] = {"acl", CODE_TEXT, -1, false};
  return c;
}

/* An index's key columns, those at attnums, ascending. */
static std::vector<sm_index_key_spec_t>
key_on(std::initializer_list<int32_t> attnums) {
  std::vector<sm_index_key_spec_t> keys;

  for (int32_t attnum : attnums)
    keys.push_back({attnum, false, false, 0, -1});
  return keys;
}

/* role as pg_authid's row in pg_global, into values and isnull. */
static void role_row(const pg_global_role_t &role, sm_datum_t *values,
                     bool *isnull) {
  memset(isnull, 0, A_NATTS * sizeof(bool));
  values[A_OID] = SM_DATUM_FROM_INT64(role.oid);
  values[A_ROLNAME] = SM_DATUM_FROM_PTR(cstring_to_text(role.rolname.c_str()));
  values[A_ROLSUPER] = SM_DATUM_FROM_BOOL(role.rolsuper);
  values[A_ROLINHERIT] = SM_DATUM_FROM_BOOL(role.rolinherit);
  values[A_ROLCREATEROLE] = SM_DATUM_FROM_BOOL(role.rolcreaterole);
  values[A_ROLCREATEDB] = SM_DATUM_FROM_BOOL(role.rolcreatedb);
  values[A_ROLCANLOGIN] = SM_DATUM_FROM_BOOL(role.rolcanlogin);
  values[A_ROLREPLICATION] = SM_DATUM_FROM_BOOL(role.rolreplication);
  values[A_ROLBYPASSRLS] = SM_DATUM_FROM_BOOL(role.rolbypassrls);
  values[A_ROLCONNLIMIT] = SM_DATUM_FROM_INT32(role.rolconnlimit);
  isnull[A_ROLPASSWORD] = !role.has_password;
  values[A_ROLPASSWORD] =
      role.has_password
          ? SM_DATUM_FROM_PTR(cstring_to_text(role.rolpassword.c_str()))
          : 0;
  isnull[A_ROLVALIDUNTIL] = !role.has_validuntil;
  values[A_ROLVALIDUNTIL] = SM_DATUM_FROM_INT64(role.rolvaliduntil);
}

/* member as pg_auth_members's row in pg_global, into values and isnull. */
static void member_row(const pg_global_member_t &member, sm_datum_t *values,
                       bool *isnull) {
  memset(isnull, 0, M_NATTS * sizeof(bool));
  values[M_OID] = SM_DATUM_FROM_INT64(member.oid);
  values[M_ROLEID] = SM_DATUM_FROM_INT64(member.roleid);
  values[M_MEMBER] = SM_DATUM_FROM_INT64(member.member);
  values[M_GRANTOR] = SM_DATUM_FROM_INT64(member.grantor);
  values[M_ADMIN_OPTION] = SM_DATUM_FROM_BOOL(member.admin_option);
  values[M_INHERIT_OPTION] = SM_DATUM_FROM_BOOL(member.inherit_option);
  values[M_SET_OPTION] = SM_DATUM_FROM_BOOL(member.set_option);
}

/* object as pg_object's row in pg_global, into values and isnull. */
static void object_row(const pg_global_object_t &object, sm_datum_t *values,
                       bool *isnull) {
  memset(isnull, 0, O_NATTS * sizeof(bool));
  values[O_DBID] = SM_DATUM_FROM_INT64(object.dbid);
  values[O_OBJID] = SM_DATUM_FROM_INT64(object.objid);
  values[O_CLASSID] = SM_DATUM_FROM_INT64(object.classid);
  values[O_OWNER] = SM_DATUM_FROM_INT64(object.owner);
  isnull[O_ACL] = !object.has_acl;
  values[O_ACL] = object.has_acl
                      ? SM_DATUM_FROM_PTR(cstring_to_text(object.acl.c_str()))
                      : 0;
}

/* The text value of a row's column, a varlena in the row, as a string. */
static std::string text_value(sm_datum_t value) {
  char *s = TextDatumGetCString((Datum)value);
  std::string out = s;

  pfree(s);
  return out;
}

/* pg_authid's row in pg_global, values and isnull, as a role. */
static pg_global_role_t role_of_row(const sm_datum_t *v, const bool *isnull) {
  pg_global_role_t role;

  role.oid = (Oid)SM_DATUM_TO_INT64(v[A_OID]);
  role.rolname = text_value(v[A_ROLNAME]);
  role.rolsuper = SM_DATUM_TO_BOOL(v[A_ROLSUPER]);
  role.rolinherit = SM_DATUM_TO_BOOL(v[A_ROLINHERIT]);
  role.rolcreaterole = SM_DATUM_TO_BOOL(v[A_ROLCREATEROLE]);
  role.rolcreatedb = SM_DATUM_TO_BOOL(v[A_ROLCREATEDB]);
  role.rolcanlogin = SM_DATUM_TO_BOOL(v[A_ROLCANLOGIN]);
  role.rolreplication = SM_DATUM_TO_BOOL(v[A_ROLREPLICATION]);
  role.rolbypassrls = SM_DATUM_TO_BOOL(v[A_ROLBYPASSRLS]);
  role.rolconnlimit = SM_DATUM_TO_INT32(v[A_ROLCONNLIMIT]);
  role.has_password = !isnull[A_ROLPASSWORD];
  if (role.has_password)
    role.rolpassword = text_value(v[A_ROLPASSWORD]);
  role.has_validuntil = !isnull[A_ROLVALIDUNTIL];
  if (role.has_validuntil)
    role.rolvaliduntil = SM_DATUM_TO_INT64(v[A_ROLVALIDUNTIL]);
  return role;
}

/* pg_object's row in pg_global, values and isnull, as an object. */
static pg_global_object_t object_of_row(const sm_datum_t *v,
                                        const bool *isnull) {
  pg_global_object_t object;

  object.dbid = (uint64_t)SM_DATUM_TO_INT64(v[O_DBID]);
  object.objid = (Oid)SM_DATUM_TO_INT64(v[O_OBJID]);
  object.classid = (Oid)SM_DATUM_TO_INT64(v[O_CLASSID]);
  object.owner = (Oid)SM_DATUM_TO_INT64(v[O_OWNER]);
  object.has_acl = !isnull[O_ACL];
  if (object.has_acl)
    object.acl = text_value(v[O_ACL]);
  return object;
}

/* pg_auth_members's row in pg_global, values, as a membership. */
static pg_global_member_t member_of_row(const sm_datum_t *v) {
  pg_global_member_t member;

  member.oid = (Oid)SM_DATUM_TO_INT64(v[M_OID]);
  member.roleid = (Oid)SM_DATUM_TO_INT64(v[M_ROLEID]);
  member.member = (Oid)SM_DATUM_TO_INT64(v[M_MEMBER]);
  member.grantor = (Oid)SM_DATUM_TO_INT64(v[M_GRANTOR]);
  member.admin_option = SM_DATUM_TO_BOOL(v[M_ADMIN_OPTION]);
  member.inherit_option = SM_DATUM_TO_BOOL(v[M_INHERIT_OPTION]);
  member.set_option = SM_DATUM_TO_BOOL(v[M_SET_OPTION]);
  return member;
}

/*
 * PostgreSQL 18's predefined roles, with their oids, as its pg_authid.dat
 * has them: INHERIT, and nothing else.
 */
static const struct {
  Oid oid;
  const char *name;
} predefined_roles[] = {
    {6171, "pg_database_owner"},
    {6181, "pg_read_all_data"},
    {6182, "pg_write_all_data"},
    {3373, "pg_monitor"},
    {3374, "pg_read_all_settings"},
    {3375, "pg_read_all_stats"},
    {3377, "pg_stat_scan_tables"},
    {4569, "pg_read_server_files"},
    {4570, "pg_write_server_files"},
    {4571, "pg_execute_server_program"},
    {4200, "pg_signal_backend"},
    {4544, "pg_checkpoint"},
    {6337, "pg_maintain"},
    {4550, "pg_use_reserved_connections"},
    {6304, "pg_create_subscription"},
    {6392, "pg_signal_autovacuum_worker"},
};

static int insert_role(sm_session_t *session, uint64_t table,
                       const pg_global_role_t &role);

/* The roles pg_monitor has, as PostgreSQL's system_functions.sql grants. */
static const Oid monitor_members[] = {3374, 3375, 3377};
#define PG_MONITOR_OID 3373

/* Insert member into pg_auth_members, the table oid, likewise. */
static int insert_member(sm_session_t *session, uint64_t table,
                         const pg_global_member_t &member) {
  sm_datum_t values[M_NATTS];
  bool isnull[M_NATTS];
  sm_tbl_t *tbl;
  uint64_t id;
  int rc;

  member_row(member, values, isnull);
  /* IX: a write to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    return -1;
  rc = sm_tbl_insert(tbl, values, isnull, M_NATTS, &id);
  sm_tbl_close(tbl);
  return rc;
}

/*
 * Insert PostgreSQL's predefined roles into pg_authid, the table authid,
 * and pg_monitor's members into pg_auth_members, the table members, as
 * initdb leaves PostgreSQL's.  0, or -1 with sm's error set.
 */
static int insert_predefined(sm_session_t *session, uint64_t authid,
                             uint64_t members) {
  for (const auto &p : predefined_roles) {
    pg_global_role_t role;

    role.oid = p.oid;
    role.rolname = p.name;
    if (0 != insert_role(session, authid, role))
      return -1;
  }
  for (Oid roleid : monitor_members) {
    pg_global_member_t m;
    uint64_t oid;

    if (0 != sm_nextseq(session, OID_SEQUENCE, &oid))
      return -1;
    m.oid = (Oid)oid;
    m.roleid = roleid;
    m.member = PG_MONITOR_OID;
    m.grantor = BOOTSTRAP_SUPERUSERID;
    m.inherit_option = true;
    m.set_option = true;
    if (0 != insert_member(session, members, m))
      return -1;
  }
  return 0;
}

/* Insert role into pg_authid, the table oid, in the session's transaction. */
static int insert_role(sm_session_t *session, uint64_t table,
                       const pg_global_role_t &role) {
  sm_datum_t values[A_NATTS];
  bool isnull[A_NATTS];
  sm_tbl_t *tbl;
  uint64_t id;
  int rc;

  role_row(role, values, isnull);
  /* IX: a write to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    return -1;
  rc = sm_tbl_insert(tbl, values, isnull, A_NATTS, &id);
  sm_tbl_close(tbl);
  return rc;
}

/*
 * Make pg_global in the attached home directory env, for initdb: the
 * database, its tables and oid sequence, and in pg_authid the bootstrap
 * superuser, named superuser, with the SCRAM secret of its password, or
 * none if secret is NULL.  0, or -1 with sm's error set.
 */
int pg_global_create(sm_env_t *env, const char *superuser, const char *secret) {
  session_guard_t guard;
  pg_global_role_t role;
  uint64_t authid, members, objects, index;

  if (0 != sm_mkdb(env, PG_GLOBAL_DBNAME, NULL) ||
      0 != sm_opendb(env, PG_GLOBAL_DBNAME, NULL, &guard.session))
    return -1;

  /*
   * The bootstrap superuser, as PostgreSQL's pg_authid.dat has it, but
   * REPLICATION: digres has no replication.
   */
  role.oid = BOOTSTRAP_SUPERUSERID;
  role.rolname = superuser;
  role.rolsuper = true;
  role.rolcreaterole = true;
  role.rolcreatedb = true;
  role.rolcanlogin = true;
  role.rolbypassrls = true;
  role.has_password = NULL != secret;
  if (NULL != secret)
    role.rolpassword = secret;

  /* One transaction: a pg_global is whole, or initdb fails. */
  if (0 != sm_begin(guard.session, SM_XCT_READ_COMMITTED, 0))
    return -1;
  if (0 != sm_cat_create_table(guard.session, SM_NSP_PUBLIC, AUTHID_TABLE,
                               authid_columns(), &authid) ||
      0 != sm_cat_create_index(guard.session, authid, "pg_authid_oid_index",
                               key_on({A_OID}), {}, true, "", {}, &index,
                               true /* primary */) ||
      0 != sm_cat_create_index(guard.session, authid, "pg_authid_rolname_index",
                               key_on({A_ROLNAME}), {}, true, "", {}, &index) ||
      0 != sm_cat_create_table(guard.session, SM_NSP_PUBLIC, MEMBERS_TABLE,
                               members_columns(), &members) ||
      0 != sm_cat_create_index(guard.session, members,
                               "pg_auth_members_oid_index", key_on({M_OID}), {},
                               true, "", {}, &index, true /* primary */) ||
      0 != sm_cat_create_index(guard.session, members,
                               "pg_auth_members_role_member_index",
                               key_on({M_ROLEID, M_MEMBER, M_GRANTOR}), {},
                               true, "", {}, &index) ||
      0 != sm_cat_create_table(guard.session, SM_NSP_PUBLIC, OBJECT_TABLE,
                               object_columns(), &objects) ||
      0 != sm_cat_create_index(guard.session, objects, "pg_object_index",
                               key_on({O_DBID, O_OBJID}), {}, true, "", {},
                               &index, true /* primary */) ||
      /* A role's oid, and a membership's, are above the built-in ones. */
      0 != sm_mkseq(guard.session, OID_SEQUENCE, FirstNormalObjectId, 1) ||
      0 != insert_role(guard.session, authid, role) ||
      0 != insert_predefined(guard.session, authid, members) ||
      0 != sm_commit(guard.session)) {
    int saved = sm_errnum();
    std::string msg = sm_errmsg();

    (void)sm_abort(guard.session);
    sm_seterr((sm_errnum_t)saved, msg.c_str());
    return -1;
  }
  return 0;
}

/* Report the failure of what, reading or writing pg_global, at elevel. */
static void sm_failed(int elevel, const char *what) {
  ereport(elevel, (errmsg("could not %s in database \"%s\": %s", what,
                          PG_GLOBAL_DBNAME, sm_errmsg())));
}

/*
 * The oid of pg_global's table name, in session's transaction; at elevel
 * if there is none, as in a data directory an older initdb made.
 */
static uint64_t table_oid(sm_session_t *session, const char *name, int elevel) {
  uint64_t table;
  int rc = sm_cat_table_oid(session, SM_NSP_PUBLIC, name, &table);

  if (1 == rc)
    ereport(elevel, (errmsg("database \"%s\" has no table \"%s\"",
                            PG_GLOBAL_DBNAME, name),
                     errhint("Run initdb to make a new data directory.")));
  if (0 != rc)
    sm_failed(elevel, "look up a table");
  return table;
}

/*
 * Read every row of pg_global's table name, in its key's order, the oid,
 * in session's transaction: each, natts values and their null flags, to
 * row.
 */
template <typename F>
static void read_rows(sm_session_t *session, const char *name, size_t natts,
                      int elevel, F row) {
  uint64_t table = table_oid(session, name, elevel);
  sm_tbl_t *tbl;
  sm_rel_scan_t *scan;
  const void *tuple;
  size_t len;
  uint64_t id;
  int rc;

  /* IS: a read of the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IS, &tbl))
    sm_failed(elevel, "read a table");
  if (0 != sm_tbl_scan_rows(tbl, &scan)) {
    sm_tbl_close(tbl);
    sm_failed(elevel, "read a table");
  }
  /* 1 at the end of the rows, -1 on an error. */
  while (0 == (rc = sm_rel_scannxt(scan, &id, &tuple, &len))) {
    std::vector<sm_datum_t> v(natts);
    std::unique_ptr<bool[]> isnull(new bool[natts]);

    if (0 != sm_tbl_deform(tbl, tuple, len, v.data(), isnull.get(), natts)) {
      rc = -1;
      break;
    }
    row(v.data(), isnull.get());
  }
  sm_rel_scanend(scan);
  sm_tbl_close(tbl);
  if (0 > rc)
    sm_failed(elevel, "read a table");
}

/* Read every role and membership of pg_global, through session. */
static std::shared_ptr<pg_global_roles_t> read_roles(sm_session_t *session,
                                                     int elevel) {
  auto out = std::make_shared<pg_global_roles_t>();

  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 1))
    sm_failed(elevel, "begin a transaction");
  read_rows(session, AUTHID_TABLE, A_NATTS, elevel,
            [&](const sm_datum_t *v, const bool *isnull) {
              out->roles.push_back(role_of_row(v, isnull));
            });
  read_rows(session, MEMBERS_TABLE, M_NATTS, elevel,
            [&](const sm_datum_t *v, const bool *isnull) {
              out->members.push_back(member_of_row(v));
            });
  if (0 != sm_commit(session))
    sm_failed(elevel, "commit");
  return out;
}

/* Read every object's entry of pg_global, through session. */
static std::shared_ptr<pg_global_objects_t> read_objects(sm_session_t *session,
                                                         int elevel) {
  auto out = std::make_shared<pg_global_objects_t>();

  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 1))
    sm_failed(elevel, "begin a transaction");
  read_rows(session, OBJECT_TABLE, O_NATTS, elevel,
            [&](const sm_datum_t *v, const bool *isnull) {
              pg_global_object_t o = object_of_row(v, isnull);

              (*out)[object_key_t(o.dbid, o.objid)] = o;
            });
  if (0 != sm_commit(session))
    sm_failed(elevel, "commit");
  return out;
}

/*
 * Read pg_global's roles into the snapshot every session shares.  The
 * postmaster calls this once, at startup, before any session.  A data
 * directory with no pg_global was made before digres kept roles, and is
 * refused (sm/ROLE.md section 8, question 5).
 */
void pg_global_load(sm_env_t *env) {
  session_guard_t guard;
  uint64_t dbid;

  if (0 != sm_dbid(env, PG_GLOBAL_DBNAME, &dbid)) {
    if (SM_ERR_NOTFOUND == sm_errnum())
      ereport(FATAL, (errmsg("data directory has no database \"%s\"",
                             PG_GLOBAL_DBNAME),
                      errdetail("It was made before digres kept roles there."),
                      errhint("Run initdb to make a new data directory.")));
    sm_failed(FATAL, "look up the database");
  }
  if (0 != sm_opendb(env, PG_GLOBAL_DBNAME, NULL, &guard.session))
    sm_failed(FATAL, "open the database");
  shmem->pg_global.reset(new pg_global_t());
  shmem->pg_global->env = env;
  shmem->pg_global->roles = read_roles(guard.session, FATAL);
  shmem->pg_global->objects = read_objects(guard.session, FATAL);
}

/* Whether the roles were read: not in a process with no data directory. */
bool pg_global_is_loaded(void) { return nullptr != shmem->pg_global; }

/* The roles as last committed, copied under the lock. */
static std::shared_ptr<const pg_global_roles_t> shared_roles(void) {
  /* The snapshot is replaced as a whole; copy the pointer under the lock. */
  std::lock_guard<std::mutex> lock(shmem->pg_global->mutex);
  return shmem->pg_global->roles;
}

/* This session's own state, made at its first use; NULL with no session. */
static pg_global_session_t *session_state(void) {
  if (nullptr == tcb)
    return NULL;
  if (nullptr == tcb->pg_global)
    tcb->pg_global.reset(new pg_global_session_t());
  return tcb->pg_global.get();
}

/*
 * The roles as this session sees them: with its transaction's own changes,
 * else as last committed; none in a process that loaded no data directory.
 */
std::shared_ptr<const pg_global_roles_t> pg_global_roles(void) {
  pg_global_session_t *s;

  if (!pg_global_is_loaded())
    return std::make_shared<const pg_global_roles_t>();
  s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  if (NULL != s && nullptr != s->working)
    return s->working;
  return shared_roles();
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

/* This session's sm session on pg_global, opened at its first need. */
static sm_session_t *write_session(void) {
  pg_global_session_t *s = session_state();

  /* For as long as the session lasts: it hands out oids, and commits. */
  if (NULL == s->session && 0 != sm_opendb(shmem->pg_global->env,
                                           PG_GLOBAL_DBNAME, NULL, &s->session))
    sm_failed(ERROR, "open the database");
  return s->session;
}

/* A new oid for a role or a membership, from pg_global's sequence. */
Oid pg_global_new_oid(void) {
  uint64_t oid;

  if (0 != sm_nextseq(write_session(), OID_SEQUENCE, &oid))
    sm_failed(ERROR, "draw an oid");
  if (oid > PG_UINT32_MAX)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("no role oid is left")));
  return (Oid)oid;
}

/*
 * The session's working copy of the roles, to change: made from the
 * snapshot at its first change in the transaction.
 */
static pg_global_roles_t &working(void) {
  pg_global_session_t *s = session_state();

  if (nullptr == s->working) {
    s->base = shared_roles();
    s->working = std::make_shared<pg_global_roles_t>(*s->base);
  }
  return *s->working;
}

/* Put row in rows by its oid, replacing one with that oid, in oid order. */
template <typename T> static void put_by_oid(std::vector<T> &rows, T row) {
  auto it = std::lower_bound(rows.begin(), rows.end(), row.oid,
                             [](const T &r, Oid oid) { return r.oid < oid; });

  if (rows.end() != it && it->oid == row.oid)
    *it = std::move(row);
  else
    rows.insert(it, std::move(row));
}

/* Remove the row with oid from rows, if there is one. */
template <typename T> static void delete_by_oid(std::vector<T> &rows, Oid oid) {
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [oid](const T &r) { return r.oid == oid; }),
             rows.end());
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
  put_by_oid(working().roles, std::move(role));
  /* The session's caches of pg_authid go at its next command. */
  CacheInvalidateCatalog(AuthIdRelationId);
}

/* Remove the role oid, in the session's transaction. */
void pg_global_delete_role(Oid oid) {
  delete_by_oid(working().roles, oid);
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
  put_by_oid(working().members, member);
  /* The session's caches of pg_auth_members go at its next command. */
  CacheInvalidateCatalog(AuthMemRelationId);
}

/* Remove the membership oid, in the session's transaction. */
void pg_global_delete_member(Oid oid) {
  delete_by_oid(working().members, oid);
  CacheInvalidateCatalog(AuthMemRelationId);
}

/*
 * What depends on the role roleid, as DROP ROLE's DETAIL lists it, one a
 * line, palloc'd; NULL if nothing does.  So far only a membership it
 * granted (sm/ROLE.md section 4: no pg_shdepend, pg_global alone).
 */
char *pg_global_role_dependents(Oid roleid) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();
  StringInfoData out;

  initStringInfo(&out);
  for (const pg_global_member_t &m : all->members) {
    pg_global_role_t member, role;

    if (m.grantor != roleid || !pg_global_role_by_oid(m.member, &member) ||
        !pg_global_role_by_oid(m.roleid, &role))
      continue;
    if (out.len > 0)
      appendStringInfoChar(&out, '\n');
    appendStringInfo(&out, "privileges for membership of role %s in role %s",
                     member.rolname.c_str(), role.rolname.c_str());
  }
  if (0 == out.len) {
    pfree(out.data);
    return NULL;
  }
  return out.data;
}

/* Fail the commit, a row of table changed meanwhile by another session. */
static void concurrent_change(const char *table) {
  ereport(
      ERROR,
      (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
       errmsg("could not serialize access due to concurrent update"),
       errdetail("Another session changed %s since this one read it.", table)));
}

/*
 * Write to pg_global's table name, natts columns, the rows of rows that
 * differ from base, by oid: insert, update or delete each.  A row not as
 * base had it is another session's change, and fails the commit.  row()
 * makes a row's values, decode() reads one, same() compares two.
 */
template <typename T, typename R, typename D, typename S>
static void write_changes(sm_session_t *session, const char *name, size_t natts,
                          const std::vector<T> &base,
                          const std::vector<T> &rows, R row, D decode, S same) {
  uint64_t table = table_oid(session, name, ERROR);
  std::map<Oid, std::pair<const T *, const T *>> changes;
  sm_tbl_t *tbl;

  for (const T &r : base)
    changes[r.oid].first = &r;
  for (const T &r : rows)
    changes[r.oid].second = &r;
  /* IX: writes to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    sm_failed(ERROR, "open a table");
  for (const auto &[oid, change] : changes) {
    const T *before = change.first, *after = change.second;
    sm_datum_t key = SM_DATUM_FROM_INT64(oid);
    bool key_null = false;
    sm_row_key_t rowkey = {&key, &key_null, 1};
    std::vector<sm_datum_t> v(natts);
    std::unique_ptr<bool[]> isnull(new bool[natts]);
    const void *tuple;
    size_t len;
    uint64_t id;
    int rc;

    if (NULL != before && NULL != after && same(*before, *after))
      continue;
    /* The row as pg_global has it now: as this session read it, or not. */
    rc = sm_tbl_read(tbl, rowkey, &tuple, &len);
    if (0 > rc)
      sm_failed(ERROR, "read a row");
    if ((1 == rc) != (NULL == before))
      concurrent_change(name);
    if (0 == rc) {
      if (0 != sm_tbl_deform(tbl, tuple, len, v.data(), isnull.get(), natts))
        sm_failed(ERROR, "read a row");
      if (!same(decode(v.data(), isnull.get()), *before))
        concurrent_change(name);
    }
    if (NULL == after)
      rc = sm_tbl_delete(tbl, rowkey);
    else {
      row(*after, v.data(), isnull.get());
      rc = NULL == before
               ? sm_tbl_insert(tbl, v.data(), isnull.get(), natts, &id)
               : sm_tbl_update(tbl, rowkey, v.data(), isnull.get(), natts);
    }
    if (0 != rc) {
      if (SM_ERR_DUPKEY == sm_errnum() || SM_ERR_UPDATED == sm_errnum())
        concurrent_change(name);
      sm_failed(ERROR, "write a row");
    }
  }
  sm_tbl_close(tbl);
}

/* The objects' entries as last committed, copied under the lock. */
static std::shared_ptr<const pg_global_objects_t> shared_objects(void) {
  std::lock_guard<std::mutex> lock(shmem->pg_global->mutex);
  return shmem->pg_global->objects;
}

/*
 * The entry of the object key as session s sees it, with its transaction's
 * changes, into *out: false if it has none.  s may be NULL.
 */
static bool find_object(const pg_global_session_t *s, const object_key_t &key,
                        pg_global_object_t *out) {
  std::shared_ptr<const pg_global_objects_t> all;

  if (NULL != s)
    for (auto it = s->changes.rbegin(); s->changes.rend() != it; ++it)
      if (it->key == key) {
        if (!it->present)
          return false;
        *out = it->object;
        return true;
      }
  all = shared_objects();
  auto found = all->find(key);
  if (all->end() == found)
    return false;
  *out = found->second;
  return true;
}

/* sm's id of the database whose oid is dboid (utils/init/datadir.c). */
uint64_t pg_global_dbid(Oid dboid) { return dboid - FirstNormalObjectId; }

/* The owner of the database dbid: its entry's, else the superuser. */
Oid pg_global_database_owner(uint64_t dbid) {
  pg_global_object_t object;

  if (pg_global_is_loaded() &&
      find_object(nullptr != tcb ? tcb->pg_global.get() : NULL,
                  object_key_t(dbid, 0), &object))
    return object.owner;
  return BOOTSTRAP_SUPERUSERID;
}

/*
 * The owner of the object objid of this session's database, as the session
 * sees it: a temporary object's, the session's role that made it; else its
 * entry's; else, with none, as a crash between the commits may leave one,
 * the database's owner (sm/ROLE.md section 3).
 */
Oid pg_global_owner(Oid objid) {
  pg_global_session_t *s;
  pg_global_object_t object;
  uint64_t dbid;

  if (!pg_global_is_loaded())
    return BOOTSTRAP_SUPERUSERID;
  s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  if (NULL != s) {
    auto temp = s->temp_owners.find(objid);

    if (s->temp_owners.end() != temp)
      return temp->second;
  }
  dbid = pg_global_dbid(MyDatabaseId);
  if (find_object(s, object_key_t(dbid, objid), &object))
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
      !find_object(nullptr != tcb ? tcb->pg_global.get() : NULL,
                   object_key_t(pg_global_dbid(MyDatabaseId), objid), &object))
    return false;
  *owner = object.owner;
  return true;
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
  pg_global_session_t *s = session_state();
  object_key_t key(dbid, objid);
  pg_global_object_t object;

  if (temp)
    s->temp_owners[objid] = owner;
  else {
    if (!find_object(s, key, &object)) {
      object.dbid = dbid;
      object.objid = objid;
      object.classid = classid;
    }
    object.owner = owner;
    s->changes.push_back({GetCurrentTransactionNestLevel(), key, true, object});
  }
  /* The relation's cache entry has its owner, for a view's rules. */
  if (RelationRelationId == classid && pg_global_dbid(MyDatabaseId) == dbid)
    CacheInvalidateRelcacheByOid(objid);
}

/* The object objid of the database dbid goes: its entry with it. */
void pg_global_forget(uint64_t dbid, Oid objid) {
  pg_global_session_t *s = session_state();
  object_key_t key(dbid, objid);
  pg_global_object_t object;

  s->temp_owners.erase(objid);
  if (find_object(s, key, &object))
    s->changes.push_back(
        {GetCurrentTransactionNestLevel(), key, false, object});
}

/* The database dbid goes: every entry of its with it, after the commit. */
void pg_global_forget_database(uint64_t dbid) {
  session_state()->dropped_dbs.push_back(dbid);
}

/* A subtransaction commits: its changes are its parent's. */
void pg_global_subcommit(void) {
  pg_global_session_t *s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  int level = GetCurrentTransactionNestLevel();

  if (NULL != s)
    for (object_change_t &c : s->changes)
      if (c.level >= level)
        c.level = level - 1;
}

/*
 * A subtransaction aborts: its changes, the last ones made, go.  A
 * relation's cache entry it changed is invalidated by the invalidation
 * messages its abort processes.
 */
void pg_global_subabort(void) {
  pg_global_session_t *s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  int level = GetCurrentTransactionNestLevel();

  if (NULL == s)
    return;
  while (!s->changes.empty() && s->changes.back().level >= level)
    s->changes.pop_back();
}

/*
 * The entries whose owner the session's transaction changes, as they are
 * now, to be the superuser's before the database's commit
 * (pg_global_commit()).
 */
static std::vector<std::pair<object_key_t, pg_global_object_t>>
intermediates(const pg_global_session_t *s) {
  std::map<object_key_t, const object_change_t *> last;
  std::vector<std::pair<object_key_t, pg_global_object_t>> out;
  std::shared_ptr<const pg_global_objects_t> all;

  if (s->changes.empty())
    return out;
  for (const object_change_t &c : s->changes)
    last[c.key] = &c;
  all = shared_objects();
  for (const auto &[key, c] : last) {
    auto found = all->find(key);

    if (all->end() != found && c->present &&
        c->object.owner != found->second.owner)
      out.push_back(*found);
  }
  return out;
}

/* pg_object of pg_global, open to write in session's transaction. */
static sm_tbl_t *open_objects(sm_session_t *session) {
  uint64_t table = table_oid(session, OBJECT_TABLE, ERROR);
  sm_tbl_t *tbl;

  /* IX: writes to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    sm_failed(ERROR, "open a table");
  return tbl;
}

/*
 * Make pg_object's row for key object, or no row if object is NULL,
 * through tbl.
 */
static void write_object(sm_tbl_t *tbl, const object_key_t &key,
                         const pg_global_object_t *object) {
  sm_datum_t keyv[2] = {SM_DATUM_FROM_INT64(key.first),
                        SM_DATUM_FROM_INT64(key.second)};
  bool keyn[2] = {false, false};
  sm_row_key_t rowkey = {keyv, keyn, 2};
  sm_datum_t v[O_NATTS];
  bool isnull[O_NATTS];
  const void *tuple;
  size_t len;
  uint64_t id;
  int rc = sm_tbl_read(tbl, rowkey, &tuple, &len);

  if (0 > rc)
    sm_failed(ERROR, "read a row");
  if (NULL == object) {
    if (0 == rc && 0 != sm_tbl_delete(tbl, rowkey))
      sm_failed(ERROR, "delete a row");
    return;
  }
  object_row(*object, v, isnull);
  rc = 0 == rc ? sm_tbl_update(tbl, rowkey, v, isnull, O_NATTS)
               : sm_tbl_insert(tbl, v, isnull, O_NATTS, &id);
  if (0 != rc)
    sm_failed(ERROR, "write a row");
}

/*
 * Write to pg_object, by write(), in a transaction of pg_global's of its
 * own; an error is a warning, saying what failed.  True if it committed.
 */
template <typename F> static bool write_quietly(const char *what, F write) {
  sm_session_t *session = NULL;
  MemoryContext cxt = CurrentMemoryContext;
  bool done = false;

  PG_TRY();
  {
    session = write_session();
    if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 0))
      sm_failed(ERROR, "begin a transaction");
    sm_tbl_t *tbl = open_objects(session);

    write(tbl);
    sm_tbl_close(tbl);
    if (0 != sm_commit(session))
      sm_failed(ERROR, "commit");
    done = true;
  }
  PG_CATCH();
  {
    ErrorData *err;

    MemoryContextSwitchTo(cxt);
    err = CopyErrorData();
    FlushErrorState();
    if (NULL != session)
      (void)sm_abort(session);
    ereport(WARNING, (errmsg("could not %s: %s", what, err->message)));
    FreeErrorData(err);
  }
  PG_END_TRY();
  return done;
}

/*
 * Replace the objects' snapshot with it as committed, with the changes
 * last and the databases dropped applied, under the lock; record which
 * objects changed, for other sessions' caches.
 */
static void
publish_objects(const std::map<object_key_t, const object_change_t *> &last,
                const std::vector<uint64_t> &dropped) {
  pg_global_t *g = shmem->pg_global.get();
  std::lock_guard<std::mutex> lock(g->mutex);
  auto next = std::make_shared<pg_global_objects_t>(*g->objects);
  uint64_t epoch = g->objects_epoch.load() + 1;

  for (const auto &[key, c] : last) {
    if (c->present)
      (*next)[key] = c->object;
    else
      next->erase(key);
    g->changed.push_back({epoch, key});
  }
  for (uint64_t dbid : dropped)
    next->erase(next->lower_bound(object_key_t(dbid, 0)),
                next->lower_bound(object_key_t(dbid + 1, 0)));
  while (g->changed.size() > CHANGED_KEPT)
    g->changed.pop_front();
  g->objects = next;
  g->objects_epoch.store(epoch);
}

/*
 * At the start of a transaction: drop what this session's relation cache
 * keeps of the relations whose owner another session changed since it
 * last looked; of every relation if they are more than pg_global keeps.
 */
void pg_global_objects_moved(void) {
  pg_global_session_t *s;
  std::vector<Oid> oids;
  bool all = false;
  uint64_t epoch, dbid;

  if (!pg_global_is_loaded() || NULL == (s = session_state()))
    return;
  if (shmem->pg_global->objects_epoch.load() == s->objects_epoch)
    return;
  dbid = pg_global_dbid(MyDatabaseId);
  {
    pg_global_t *g = shmem->pg_global.get();
    std::lock_guard<std::mutex> lock(g->mutex);

    epoch = g->objects_epoch.load();
    all = g->changed.empty() || g->changed.front().first > s->objects_epoch + 1;
    if (!all)
      for (const auto &[e, key] : g->changed)
        if (e > s->objects_epoch && key.first == dbid)
          oids.push_back(key.second);
  }
  s->objects_epoch = epoch;
  if (!OidIsValid(MyDatabaseId))
    return;
  if (all)
    RelationCacheInvalidate(false);
  else
    for (Oid oid : oids)
      RelationCacheInvalidateEntry(oid);
}

/*
 * At the commit of the session's transaction, before its database's: write
 * what it changed of the roles to pg_global, in one transaction there, and
 * replace the snapshot every session reads.  An error fails the commit.
 */
void pg_global_commit(void) {
  pg_global_session_t *s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  std::vector<std::pair<object_key_t, pg_global_object_t>> before;
  bool roles;
  sm_session_t *session;

  if (NULL == s)
    return;
  roles = nullptr != s->working;
  before = intermediates(s);
  if (!roles && before.empty())
    return;
  session = write_session();
  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 0))
    sm_failed(ERROR, "begin a transaction");
  PG_TRY();
  {
    if (roles) {
      write_changes(session, AUTHID_TABLE, A_NATTS, s->base->roles,
                    s->working->roles, role_row, role_of_row, same_role);
      write_changes(
          session, MEMBERS_TABLE, M_NATTS, s->base->members,
          s->working->members, member_row,
          [](const sm_datum_t *v, const bool *) { return member_of_row(v); },
          same_member);
    }
    if (!before.empty()) {
      sm_tbl_t *tbl = open_objects(session);

      /*
       * Each object whose owner changes is the superuser's until the
       * database commits: a crash between leaves less granted, not more.
       */
      for (const auto &[key, object] : before) {
        pg_global_object_t middle = object;

        middle.owner = BOOTSTRAP_SUPERUSERID;
        write_object(tbl, key, &middle);
      }
      sm_tbl_close(tbl);
    }
    if (0 != sm_commit(session)) {
      if (SM_ERR_UPDATED == sm_errnum() || sm_retryable())
        concurrent_change(AUTHID_TABLE);
      sm_failed(ERROR, "commit");
    }
  }
  PG_CATCH();
  {
    (void)sm_abort(session);
    PG_RE_THROW();
  }
  PG_END_TRY();
  /* Put back if the database's commit fails (pg_global_abort()). */
  s->to_restore = std::move(before);
  if (!roles)
    return;
  s->base.reset();
  s->working.reset();

  /*
   * Replace the snapshot with pg_global as it is now, read under the lock:
   * the last to replace it read last, so no later read is lost to an
   * earlier one.
   */
  std::lock_guard<std::mutex> lock(shmem->pg_global->mutex);
  shmem->pg_global->roles = read_roles(session, ERROR);
  s->epoch = ++shmem->pg_global->epoch;
}

/*
 * At the abort of the session's transaction: forget what it changed, and
 * put back the entries written before the database's commit, which failed.
 * An error is a warning: an abort has nowhere to send it, and an entry not
 * put back leaves its object the superuser's.
 */
void pg_global_abort(void) {
  pg_global_session_t *s = nullptr != tcb ? tcb->pg_global.get() : NULL;

  if (NULL == s)
    return;
  s->base.reset();
  s->working.reset();
  s->changes.clear();
  s->dropped_dbs.clear();
  if (!s->to_restore.empty()) {
    std::vector<std::pair<object_key_t, pg_global_object_t>> restore =
        std::move(s->to_restore);

    s->to_restore.clear();
    write_quietly("put back an owner", [&](sm_tbl_t *tbl) {
      for (const auto &[key, object] : restore)
        write_object(tbl, key, &object);
    });
  }
}

/*
 * After the database's commit: write the transaction's changes to objects'
 * entries, and the entries of the databases it dropped, which go, to
 * pg_global, and replace the snapshot every session reads.  The
 * transaction has committed, so an error is a warning: an entry not
 * written leaves its object its database's owner's.
 */
void pg_global_post_commit(void) {
  pg_global_session_t *s = nullptr != tcb ? tcb->pg_global.get() : NULL;
  std::map<object_key_t, const object_change_t *> last;
  std::vector<uint64_t> dropped;
  std::shared_ptr<const pg_global_objects_t> all;

  if (NULL == s)
    return;
  s->to_restore.clear();
  if (s->changes.empty() && s->dropped_dbs.empty())
    return;
  for (const object_change_t &c : s->changes)
    last[c.key] = &c;
  dropped = s->dropped_dbs;
  all = shared_objects();
  if (write_quietly("write an owner", [&](sm_tbl_t *tbl) {
        for (const auto &[key, c] : last)
          write_object(tbl, key, c->present ? &c->object : NULL);
        for (uint64_t dbid : dropped)
          for (auto it = all->lower_bound(object_key_t(dbid, 0));
               all->end() != it && it->first.first == dbid; ++it)
            write_object(tbl, it->first, NULL);
      }))
    publish_objects(last, dropped);
  s->changes.clear();
  s->dropped_dbs.clear();
}

/*
 * Whether another session has replaced the roles since this one's caches
 * of them were made: true once, and then they are taken to be fresh.
 */
bool pg_global_epoch_moved(void) {
  pg_global_session_t *s;
  uint64_t epoch;

  if (!pg_global_is_loaded() || NULL == (s = session_state()))
    return false;
  epoch = shmem->pg_global->epoch.load();
  if (epoch == s->epoch)
    return false;
  s->epoch = epoch;
  return true;
}
