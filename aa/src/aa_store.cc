/*
 * aa_store.cc -- pg_global's tables: pg_authid, a role a row,
 * pg_auth_members, a membership a row, each keyed by its oid, which the
 * sequence role_oid hands out, and pg_object, an object's owner and ACL
 * a row, keyed by its database and its oid there (ROLE.md section 3).
 * sm knows nothing of roles: they are rows of tables of aa's.
 */
#include <algorithm>
#include <cstring>

#include "aa_internal.h"

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
#define PG_OBJECT_TABLE "pg_object"
#define OID_SEQUENCE "role_oid"

/* sm's type codes (sm2/TYPE.md section 3) of the columns used here. */
#define CODE_BOOL 1
#define CODE_INT4 3
#define CODE_INT8 4
#define CODE_TEXT 8
#define CODE_TIMESTAMPTZ 14

/* A row being made or read: its values, their null flags, and the bytes
 * its text values point into. */
struct row_t {
  std::vector<sm_datum_t> v;
  std::unique_ptr<bool[]> isnull;
  std::string bufs[2];

  explicit row_t(size_t natts) : v(natts), isnull(new bool[natts]()) {}
};

/* Closes an sm session on pg_global as it goes out of scope. */
struct session_guard_t {
  sm_session_t *session = NULL;
  ~session_guard_t() { sm_closedb(session); }
};

/* Whether two roles are the same in every column. */
static bool same_role(const aa_role_t &a, const aa_role_t &b) {
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
static bool same_member(const aa_member_t &a, const aa_member_t &b) {
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

/* The text s as a datum, its varlena in buf. */
static int text_datum(const std::string &s, std::string &buf, sm_datum_t *out) {
  if (0 != sm_type_in(sm_type(CODE_TEXT), -1, s.data(), s.size(), out, buf))
    return aa_sm_failed("make a text value");
  return 0;
}

/* A text datum of a row read, as a string. */
static int text_value(sm_datum_t value, std::string *out) {
  if (0 != sm_type_out(sm_type(CODE_TEXT), -1, value, *out))
    return aa_sm_failed("read a text value");
  return 0;
}

/* role as pg_authid's row in pg_global, into row. */
static int role_row(const aa_role_t &role, row_t &row) {
  sm_datum_t *v = row.v.data();
  bool *isnull = row.isnull.get();

  memset(isnull, 0, A_NATTS * sizeof(bool));
  v[A_OID] = SM_DATUM_FROM_INT64(role.oid);
  if (0 != text_datum(role.rolname, row.bufs[0], &v[A_ROLNAME]))
    return -1;
  v[A_ROLSUPER] = SM_DATUM_FROM_BOOL(role.rolsuper);
  v[A_ROLINHERIT] = SM_DATUM_FROM_BOOL(role.rolinherit);
  v[A_ROLCREATEROLE] = SM_DATUM_FROM_BOOL(role.rolcreaterole);
  v[A_ROLCREATEDB] = SM_DATUM_FROM_BOOL(role.rolcreatedb);
  v[A_ROLCANLOGIN] = SM_DATUM_FROM_BOOL(role.rolcanlogin);
  v[A_ROLREPLICATION] = SM_DATUM_FROM_BOOL(role.rolreplication);
  v[A_ROLBYPASSRLS] = SM_DATUM_FROM_BOOL(role.rolbypassrls);
  v[A_ROLCONNLIMIT] = SM_DATUM_FROM_INT32(role.rolconnlimit);
  isnull[A_ROLPASSWORD] = !role.has_password;
  v[A_ROLPASSWORD] = 0;
  if (role.has_password &&
      0 != text_datum(role.rolpassword, row.bufs[1], &v[A_ROLPASSWORD]))
    return -1;
  isnull[A_ROLVALIDUNTIL] = !role.has_validuntil;
  v[A_ROLVALIDUNTIL] = SM_DATUM_FROM_INT64(role.rolvaliduntil);
  return 0;
}

/* member as pg_auth_members's row in pg_global, into row. */
static int member_row(const aa_member_t &member, row_t &row) {
  sm_datum_t *v = row.v.data();

  memset(row.isnull.get(), 0, M_NATTS * sizeof(bool));
  v[M_OID] = SM_DATUM_FROM_INT64(member.oid);
  v[M_ROLEID] = SM_DATUM_FROM_INT64(member.roleid);
  v[M_MEMBER] = SM_DATUM_FROM_INT64(member.member);
  v[M_GRANTOR] = SM_DATUM_FROM_INT64(member.grantor);
  v[M_ADMIN_OPTION] = SM_DATUM_FROM_BOOL(member.admin_option);
  v[M_INHERIT_OPTION] = SM_DATUM_FROM_BOOL(member.inherit_option);
  v[M_SET_OPTION] = SM_DATUM_FROM_BOOL(member.set_option);
  return 0;
}

/* object as pg_object's row in pg_global, into row. */
static int object_row(const aa_object_t &object, row_t &row) {
  sm_datum_t *v = row.v.data();
  bool *isnull = row.isnull.get();

  memset(isnull, 0, O_NATTS * sizeof(bool));
  v[O_DBID] = SM_DATUM_FROM_INT64(object.dbid);
  v[O_OBJID] = SM_DATUM_FROM_INT64(object.objid);
  v[O_CLASSID] = SM_DATUM_FROM_INT64(object.classid);
  v[O_OWNER] = SM_DATUM_FROM_INT64(object.owner);
  isnull[O_ACL] = !object.has_acl;
  v[O_ACL] = 0;
  if (object.has_acl && 0 != text_datum(object.acl, row.bufs[0], &v[O_ACL]))
    return -1;
  return 0;
}

/* pg_authid's row in pg_global as a role, into *out. */
static int role_of_row(const row_t &row, aa_role_t *out) {
  const sm_datum_t *v = row.v.data();
  const bool *isnull = row.isnull.get();
  aa_role_t role;

  role.oid = (aa_oid_t)SM_DATUM_TO_INT64(v[A_OID]);
  if (0 != text_value(v[A_ROLNAME], &role.rolname))
    return -1;
  role.rolsuper = SM_DATUM_TO_BOOL(v[A_ROLSUPER]);
  role.rolinherit = SM_DATUM_TO_BOOL(v[A_ROLINHERIT]);
  role.rolcreaterole = SM_DATUM_TO_BOOL(v[A_ROLCREATEROLE]);
  role.rolcreatedb = SM_DATUM_TO_BOOL(v[A_ROLCREATEDB]);
  role.rolcanlogin = SM_DATUM_TO_BOOL(v[A_ROLCANLOGIN]);
  role.rolreplication = SM_DATUM_TO_BOOL(v[A_ROLREPLICATION]);
  role.rolbypassrls = SM_DATUM_TO_BOOL(v[A_ROLBYPASSRLS]);
  role.rolconnlimit = SM_DATUM_TO_INT32(v[A_ROLCONNLIMIT]);
  role.has_password = !isnull[A_ROLPASSWORD];
  if (role.has_password && 0 != text_value(v[A_ROLPASSWORD], &role.rolpassword))
    return -1;
  role.has_validuntil = !isnull[A_ROLVALIDUNTIL];
  if (role.has_validuntil)
    role.rolvaliduntil = SM_DATUM_TO_INT64(v[A_ROLVALIDUNTIL]);
  *out = std::move(role);
  return 0;
}

/* pg_auth_members's row in pg_global as a membership, into *out. */
static int member_of_row(const row_t &row, aa_member_t *out) {
  const sm_datum_t *v = row.v.data();

  out->oid = (aa_oid_t)SM_DATUM_TO_INT64(v[M_OID]);
  out->roleid = (aa_oid_t)SM_DATUM_TO_INT64(v[M_ROLEID]);
  out->member = (aa_oid_t)SM_DATUM_TO_INT64(v[M_MEMBER]);
  out->grantor = (aa_oid_t)SM_DATUM_TO_INT64(v[M_GRANTOR]);
  out->admin_option = SM_DATUM_TO_BOOL(v[M_ADMIN_OPTION]);
  out->inherit_option = SM_DATUM_TO_BOOL(v[M_INHERIT_OPTION]);
  out->set_option = SM_DATUM_TO_BOOL(v[M_SET_OPTION]);
  return 0;
}

/* pg_object's row in pg_global as an object's entry, into *out. */
static int object_of_row(const row_t &row, aa_object_t *out) {
  const sm_datum_t *v = row.v.data();

  out->dbid = (uint64_t)SM_DATUM_TO_INT64(v[O_DBID]);
  out->objid = (aa_oid_t)SM_DATUM_TO_INT64(v[O_OBJID]);
  out->classid = (aa_oid_t)SM_DATUM_TO_INT64(v[O_CLASSID]);
  out->owner = (aa_oid_t)SM_DATUM_TO_INT64(v[O_OWNER]);
  out->has_acl = !row.isnull[O_ACL];
  out->acl.clear();
  if (out->has_acl && 0 != text_value(v[O_ACL], &out->acl))
    return -1;
  return 0;
}

/* Insert row, natts columns, into the table, in session's transaction. */
static int insert_row(sm_session_t *session, uint64_t table, row_t &row,
                      size_t natts) {
  sm_tbl_t *tbl;
  uint64_t id;
  int rc;

  /* IX: a write to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    return aa_sm_failed("open a table");
  rc = sm_tbl_insert(tbl, row.v.data(), row.isnull.get(), natts, &id);
  sm_tbl_close(tbl);
  if (0 != rc)
    return aa_sm_failed("insert a row");
  return 0;
}

/* Insert role into pg_authid, the table, in session's transaction. */
static int insert_role(sm_session_t *session, uint64_t table,
                       const aa_role_t &role) {
  row_t row(A_NATTS);

  if (0 != role_row(role, row))
    return -1;
  return insert_row(session, table, row, A_NATTS);
}

/* Insert member into pg_auth_members, the table, likewise. */
static int insert_member(sm_session_t *session, uint64_t table,
                         const aa_member_t &member) {
  row_t row(M_NATTS);

  member_row(member, row);
  return insert_row(session, table, row, M_NATTS);
}

/*
 * PostgreSQL 18's predefined roles, with their oids, as its pg_authid.dat
 * has them: INHERIT, and nothing else.
 */
static const struct {
  aa_oid_t oid;
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

/* The roles pg_monitor has, as PostgreSQL's system_functions.sql grants. */
static const aa_oid_t monitor_members[] = {3374, 3375, 3377};
#define PG_MONITOR_OID 3373

/*
 * Insert PostgreSQL's predefined roles into pg_authid, the table authid,
 * and pg_monitor's members into pg_auth_members, the table members, as
 * initdb leaves PostgreSQL's.
 */
static int insert_predefined(sm_session_t *session, uint64_t authid,
                             uint64_t members) {
  for (const auto &p : predefined_roles) {
    aa_role_t role;

    role.oid = p.oid;
    role.rolname = p.name;
    if (0 != insert_role(session, authid, role))
      return -1;
  }
  for (aa_oid_t roleid : monitor_members) {
    aa_member_t m;
    uint64_t oid;

    if (0 != sm_nextseq(session, OID_SEQUENCE, &oid))
      return aa_sm_failed("draw an oid");
    m.oid = (aa_oid_t)oid;
    m.roleid = roleid;
    m.member = PG_MONITOR_OID;
    m.grantor = AA_BOOTSTRAP_SUPERUSER;
    m.inherit_option = true;
    m.set_option = true;
    if (0 != insert_member(session, members, m))
      return -1;
  }
  return 0;
}

/* pg_global's tables, their indexes and its oid sequence, in session's
 * transaction. */
static int create_tables(sm_session_t *session, uint64_t *authid,
                         uint64_t *members) {
  uint64_t objects, index;

  if (0 != sm_cat_create_table(session, SM_NSP_PUBLIC, AUTHID_TABLE,
                               authid_columns(), authid) ||
      0 != sm_cat_create_index(session, *authid, "pg_authid_oid_index",
                               key_on({A_OID}), {}, true, "", {}, &index,
                               true /* primary */) ||
      0 != sm_cat_create_index(session, *authid, "pg_authid_rolname_index",
                               key_on({A_ROLNAME}), {}, true, "", {}, &index) ||
      0 != sm_cat_create_table(session, SM_NSP_PUBLIC, MEMBERS_TABLE,
                               members_columns(), members) ||
      0 != sm_cat_create_index(session, *members, "pg_auth_members_oid_index",
                               key_on({M_OID}), {}, true, "", {}, &index,
                               true /* primary */) ||
      0 != sm_cat_create_index(session, *members,
                               "pg_auth_members_role_member_index",
                               key_on({M_ROLEID, M_MEMBER, M_GRANTOR}), {},
                               true, "", {}, &index) ||
      0 != sm_cat_create_table(session, SM_NSP_PUBLIC, PG_OBJECT_TABLE,
                               object_columns(), &objects) ||
      0 != sm_cat_create_index(session, objects, "pg_object_index",
                               key_on({O_DBID, O_OBJID}), {}, true, "", {},
                               &index, true /* primary */) ||
      /* A role's oid, and a membership's, are above the built-in ones. */
      0 != sm_mkseq(session, OID_SEQUENCE, AA_FIRST_NORMAL_OID, 1))
    return aa_sm_failed("create a table");
  return 0;
}

/*
 * Make pg_global in the attached home directory env, for initdb: the
 * database, its tables and oid sequence, and in pg_authid the bootstrap
 * superuser, named superuser, with the SCRAM secret of its password, or
 * none if secret is NULL, and the predefined roles.
 */
int aa_create(sm_env_t *env, const char *superuser, const char *secret) {
  session_guard_t guard;
  aa_role_t role;
  uint64_t authid, members;

  if (0 != sm_mkdb(env, AA_DBNAME, NULL))
    return aa_sm_failed("create the database");
  if (0 != sm_opendb(env, AA_DBNAME, NULL, &guard.session))
    return aa_sm_failed("open the database");

  /*
   * The bootstrap superuser, as PostgreSQL's pg_authid.dat has it, but
   * REPLICATION: digres has no replication.
   */
  role.oid = AA_BOOTSTRAP_SUPERUSER;
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
    return aa_sm_failed("begin a transaction");
  if (0 != create_tables(guard.session, &authid, &members) ||
      0 != insert_role(guard.session, authid, role) ||
      0 != insert_predefined(guard.session, authid, members)) {
    (void)sm_abort(guard.session);
    return -1;
  }
  if (0 != sm_commit(guard.session)) {
    aa_sm_failed("commit");
    (void)sm_abort(guard.session);
    return -1;
  }
  return 0;
}

/*
 * The oid of pg_global's table name, in session's transaction:
 * AA_ERR_NOTABLE if there is none, as in a data directory an older
 * initdb made.
 */
static int table_oid(sm_session_t *session, const char *name, uint64_t *out) {
  int rc = sm_cat_table_oid(session, SM_NSP_PUBLIC, name, out);

  if (1 == rc)
    return aa_seterr(AA_ERR_NOTABLE, "database \"%s\" has no table \"%s\"",
                     AA_DBNAME, name);
  if (0 != rc)
    return aa_sm_failed("look up a table");
  return 0;
}

/*
 * Read every row of pg_global's table name, in its key's order, the oid,
 * in session's transaction: each, of natts columns, to row(), which
 * returns 0 or -1.
 */
template <typename F>
static int read_rows(sm_session_t *session, const char *name, size_t natts,
                     F row) {
  sm_tbl_t *tbl;
  sm_rel_scan_t *scan;
  const void *tuple;
  size_t len;
  uint64_t table, id;
  int rc;

  if (0 != table_oid(session, name, &table))
    return -1;
  /* IS: a read of the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IS, &tbl))
    return aa_sm_failed("read a table");
  if (0 != sm_tbl_scan_rows(tbl, &scan)) {
    sm_tbl_close(tbl);
    return aa_sm_failed("read a table");
  }
  /* 1 at the end of the rows, -1 on an error. */
  while (0 == (rc = sm_rel_scannxt(scan, &id, &tuple, &len))) {
    row_t r(natts);

    if (0 != sm_tbl_deform(tbl, tuple, len, r.v.data(), r.isnull.get(), natts))
      break;
    if (0 != row(r)) {
      rc = 2; /* row()'s error, already set */
      break;
    }
  }
  sm_rel_scanend(scan);
  sm_tbl_close(tbl);
  if (1 == rc)
    return 0;
  return 2 == rc ? -1 : aa_sm_failed("read a table");
}

/* Run read() in a read-only transaction of session's own. */
template <typename F>
static int in_read_transaction(sm_session_t *session, F read) {
  if (0 != sm_begin(session, SM_XCT_READ_COMMITTED, 1))
    return aa_sm_failed("begin a transaction");
  if (0 != read()) {
    (void)sm_abort(session);
    return -1;
  }
  if (0 != sm_commit(session))
    return aa_sm_failed("commit");
  return 0;
}

/* Every role and membership, in a transaction of session's own. */
int aa_store_read_roles(sm_session_t *session, aa_roles_t *out) {
  return in_read_transaction(session, [&] {
    return 0 != read_rows(session, AUTHID_TABLE, A_NATTS,
                          [&](const row_t &r) {
                            out->roles.emplace_back();
                            return role_of_row(r, &out->roles.back());
                          }) ||
                   0 != read_rows(session, MEMBERS_TABLE, M_NATTS,
                                  [&](const row_t &r) {
                                    out->members.emplace_back();
                                    return member_of_row(r,
                                                         &out->members.back());
                                  })
               ? -1
               : 0;
  });
}

/* Every object's entry, in a transaction of session's own. */
int aa_store_read_objects(sm_session_t *session, aa_objects_t *out) {
  return in_read_transaction(session, [&] {
    return read_rows(session, PG_OBJECT_TABLE, O_NATTS, [&](const row_t &r) {
      aa_object_t o;

      if (0 != object_of_row(r, &o))
        return -1;
      (*out)[aa_key_t(o.dbid, o.objid)] = o;
      return 0;
    });
  });
}

/* Fail the commit, a row of table changed meanwhile by another session. */
static int concurrent_change(const char *table) {
  return aa_seterr(AA_ERR_CONFLICT,
                   "Another session changed %s since this one read it.", table);
}

/*
 * Write to pg_global's table name, natts columns, the rows of rows that
 * differ from base, by oid. row() makes a row's values, decode() reads
 * one, same() compares two.
 * 1. Pair each oid's row in base with its row in rows; either may be
 *    missing.
 * 2. Skip a pair whose two rows are the same.
 * 3. Read the row as pg_global has it now. A row not as base had it is
 *    another session's change, AA_ERR_CONFLICT.
 * 4. Insert, update or delete the row, as the pair says.
 */
template <typename T, typename R, typename D, typename S>
static int write_changes(sm_session_t *session, const char *name, size_t natts,
                         const std::vector<T> &base, const std::vector<T> &rows,
                         R row, D decode, S same) {
  std::map<aa_oid_t, std::pair<const T *, const T *>> changes;
  sm_tbl_t *tbl;
  uint64_t table;
  int rc = 0;

  if (0 != table_oid(session, name, &table))
    return -1;
  for (const T &r : base)
    changes[r.oid].first = &r;
  for (const T &r : rows)
    changes[r.oid].second = &r;
  /* IX: writes to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    return aa_sm_failed("open a table");
  for (const auto &[oid, change] : changes) {
    const T *before = change.first, *after = change.second;
    sm_datum_t key = SM_DATUM_FROM_INT64(oid);
    bool key_null = false;
    sm_row_key_t rowkey = {&key, &key_null, 1};
    row_t r(natts);
    const void *tuple;
    size_t len;
    uint64_t id;
    T now;

    if (NULL != before && NULL != after && same(*before, *after))
      continue;
    /* The row as pg_global has it now: as this session read it, or not. */
    rc = sm_tbl_read(tbl, rowkey, &tuple, &len);
    if (0 > rc) {
      rc = aa_sm_failed("read a row");
      break;
    }
    if ((1 == rc) != (NULL == before)) {
      rc = concurrent_change(name);
      break;
    }
    if (0 == rc) {
      if (0 !=
          sm_tbl_deform(tbl, tuple, len, r.v.data(), r.isnull.get(), natts)) {
        rc = aa_sm_failed("read a row");
        break;
      }
      if (0 != (rc = decode(r, &now)))
        break;
      if (!same(now, *before)) {
        rc = concurrent_change(name);
        break;
      }
    }
    if (NULL == after)
      rc = sm_tbl_delete(tbl, rowkey);
    else if (0 != (rc = row(*after, r)))
      break;
    else
      rc = NULL == before
               ? sm_tbl_insert(tbl, r.v.data(), r.isnull.get(), natts, &id)
               : sm_tbl_update(tbl, rowkey, r.v.data(), r.isnull.get(), natts);
    if (0 != rc) {
      rc = SM_ERR_DUPKEY == sm_errnum() || SM_ERR_UPDATED == sm_errnum()
               ? concurrent_change(name)
               : aa_sm_failed("write a row");
      break;
    }
  }
  sm_tbl_close(tbl);
  return rc;
}

/*
 * Write the roles and memberships of working that differ from base, in
 * session's transaction; one not as base had it is another session's
 * change, AA_ERR_CONFLICT.
 */
int aa_store_write_roles(sm_session_t *session, const aa_roles_t &base,
                         const aa_roles_t &working) {
  if (0 != write_changes(session, AUTHID_TABLE, A_NATTS, base.roles,
                         working.roles, role_row, role_of_row, same_role))
    return -1;
  return write_changes(session, MEMBERS_TABLE, M_NATTS, base.members,
                       working.members, member_row, member_of_row, same_member);
}

/* pg_object, open to write in session's transaction. */
int aa_store_open_objects(sm_session_t *session, sm_tbl_t **out) {
  uint64_t table;

  if (0 != table_oid(session, PG_OBJECT_TABLE, &table))
    return -1;
  /* IX: writes to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, out))
    return aa_sm_failed("open a table");
  return 0;
}

/* Make pg_object's row for key object, or none if object is NULL. */
int aa_store_write_object(sm_tbl_t *tbl, const aa_key_t &key,
                          const aa_object_t *object) {
  sm_datum_t keyv[2] = {SM_DATUM_FROM_INT64(key.first),
                        SM_DATUM_FROM_INT64(key.second)};
  bool keyn[2] = {false, false};
  sm_row_key_t rowkey = {keyv, keyn, 2};
  row_t row(O_NATTS);
  const void *tuple;
  size_t len;
  uint64_t id;
  int rc = sm_tbl_read(tbl, rowkey, &tuple, &len);

  if (0 > rc)
    return aa_sm_failed("read a row");
  if (NULL == object) {
    if (0 == rc && 0 != sm_tbl_delete(tbl, rowkey))
      return aa_sm_failed("delete a row");
    return 0;
  }
  if (0 != object_row(*object, row))
    return -1;
  rc = 0 == rc
           ? sm_tbl_update(tbl, rowkey, row.v.data(), row.isnull.get(), O_NATTS)
           : sm_tbl_insert(tbl, row.v.data(), row.isnull.get(), O_NATTS, &id);
  if (0 != rc)
    return aa_sm_failed("write a row");
  return 0;
}
