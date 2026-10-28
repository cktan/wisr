/*-------------------------------------------------------------------------
 *
 * pg_global.c
 *	  The database of what every database shares: the roles.
 *
 * A role is the whole server's, and in sm no transaction spans two
 * databases, so the roles live in a database of their own, pg_global
 * (sm/ROLE.md section 3), which initdb makes beside postgres and no client
 * connects to.  It is an sm database like any other, holding tables of
 * digres's: pg_authid, a role a row, by oid.  sm knows nothing of roles.
 *
 * All role data is in pg_global and only there: a database's directory
 * names no role, so a copy of it on another server finds that server's
 * roles (sm/ROLE.md, the invariant).
 *
 * The postmaster reads every role at startup into one snapshot the
 * sessions share (shmem->pg_global); a lookup reads the snapshot, and no
 * transaction of pg_global's.
 *
 * src/backend/utils/init/pg_global.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <mutex>

#include "catalog/pg_authid_d.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/pg_global.h"

/* The roles every session reads: shmem->pg_global. */
struct pg_global_t {
  std::mutex mutex; /* guards roles */
  std::shared_ptr<const pg_global_roles_t> roles;
};

/* Free the shared roles: never, as shmem is never freed. */
void pg_global_free_t::operator()(pg_global_t *global) const { delete global; }

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

/* The table of roles in pg_global, in its schema public. */
#define AUTHID_TABLE "pg_authid"

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

/* An index's one key column, the column at attnum, ascending. */
static std::vector<sm_index_key_spec_t> key_on(int32_t attnum) {
  return {{attnum, false, false, 0, -1}};
}

/* Insert role into pg_authid, the table oid, in the session's transaction. */
static int insert_role(sm_session_t *session, uint64_t table,
                       const pg_global_role_t &role) {
  sm_datum_t values[A_NATTS];
  bool isnull[A_NATTS] = {};
  sm_tbl_t *tbl;
  uint64_t id;
  int rc;

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

  /* IX: a write to the table's rows. */
  if (0 != sm_tbl_open(session, table, SM_LOCKMODE_IX, &tbl))
    return -1;
  rc = sm_tbl_insert(tbl, values, isnull, A_NATTS, &id);
  sm_tbl_close(tbl);
  return rc;
}

/*
 * Make pg_global in the attached home directory env, for initdb: the
 * database, its pg_authid, and in it the bootstrap superuser, named
 * superuser, with the SCRAM secret of its password, or none if secret is
 * NULL.  0, or -1 with sm's error set.
 */
int pg_global_create(sm_env_t *env, const char *superuser, const char *secret) {
  session_guard_t guard;
  pg_global_role_t role;
  uint64_t table, index;

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
                               authid_columns(), &table) ||
      0 != sm_cat_create_index(guard.session, table, "pg_authid_oid_index",
                               key_on(A_OID), {}, true, "", {}, &index,
                               true /* primary */) ||
      0 != sm_cat_create_index(guard.session, table, "pg_authid_rolname_index",
                               key_on(A_ROLNAME), {}, true, "", {}, &index) ||
      0 != insert_role(guard.session, table, role) ||
      0 != sm_commit(guard.session)) {
    int saved = sm_errnum();
    std::string msg = sm_errmsg();

    (void)sm_abort(guard.session);
    sm_seterr((sm_errnum_t)saved, msg.c_str());
    return -1;
  }
  return 0;
}

/* Report the failure of what, reading pg_global at startup, and exit. */
static void load_failed(const char *what) {
  ereport(FATAL, (errmsg("could not %s in database \"%s\": %s", what,
                         PG_GLOBAL_DBNAME, sm_errmsg())));
}

/* The text value of a row's column, a varlena in the row, as a string. */
static std::string text_value(sm_datum_t value) {
  char *s = TextDatumGetCString((Datum)value);
  std::string out = s;

  pfree(s);
  return out;
}

/* Read every role of pg_global's pg_authid, in oid order. */
static std::shared_ptr<pg_global_roles_t> read_roles(sm_env_t *env) {
  auto out = std::make_shared<pg_global_roles_t>();
  session_guard_t guard;
  uint64_t table, id;
  sm_tbl_t *tbl;
  sm_rel_scan_t *scan;
  const void *tuple;
  size_t len;
  int rc;

  if (0 != sm_opendb(env, PG_GLOBAL_DBNAME, NULL, &guard.session))
    load_failed("open the database");
  if (0 != sm_begin(guard.session, SM_XCT_READ_COMMITTED, 1))
    load_failed("begin a transaction");
  rc = sm_cat_table_oid(guard.session, SM_NSP_PUBLIC, AUTHID_TABLE, &table);
  if (1 == rc)
    ereport(FATAL, (errmsg("database \"%s\" has no table \"%s\"",
                           PG_GLOBAL_DBNAME, AUTHID_TABLE),
                    errhint("Run initdb to make a new data directory.")));
  if (0 != rc)
    load_failed("look up " AUTHID_TABLE);
  /* IS: a read of the table's rows, in its primary key's order, by oid. */
  if (0 != sm_tbl_open(guard.session, table, SM_LOCKMODE_IS, &tbl) ||
      0 != sm_tbl_scan_rows(tbl, &scan))
    load_failed("read " AUTHID_TABLE);
  while (0 == (rc = sm_rel_scannxt(scan, &id, &tuple, &len))) {
    sm_datum_t v[A_NATTS];
    bool isnull[A_NATTS];
    pg_global_role_t role;

    if (0 != sm_tbl_deform(tbl, tuple, len, v, isnull, A_NATTS))
      load_failed("read " AUTHID_TABLE);
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
    out->roles.push_back(std::move(role));
  }
  if (0 > rc)
    load_failed("read " AUTHID_TABLE);
  sm_rel_scanend(scan);
  sm_tbl_close(tbl);
  if (0 != sm_commit(guard.session))
    load_failed("commit");
  return out;
}

/*
 * Read pg_global's roles into the snapshot every session shares.  The
 * postmaster calls this once, at startup, before any session.  A data
 * directory with no pg_global was made before digres kept roles, and is
 * refused (sm/ROLE.md section 8, question 5).
 */
void pg_global_load(sm_env_t *env) {
  uint64_t dbid;

  if (0 != sm_dbid(env, PG_GLOBAL_DBNAME, &dbid)) {
    if (SM_ERR_NOTFOUND == sm_errnum())
      ereport(FATAL, (errmsg("data directory has no database \"%s\"",
                             PG_GLOBAL_DBNAME),
                      errdetail("It was made before digres kept roles there."),
                      errhint("Run initdb to make a new data directory.")));
    load_failed("look up the database");
  }
  shmem->pg_global.reset(new pg_global_t());
  shmem->pg_global->roles = read_roles(env);
}

/* Whether the roles were read: not in a process with no data directory. */
bool pg_global_is_loaded(void) { return nullptr != shmem->pg_global; }

/*
 * The roles as last committed, for as long as the caller holds them; none
 * in a process that loaded no data directory.
 */
std::shared_ptr<const pg_global_roles_t> pg_global_roles(void) {
  if (!pg_global_is_loaded())
    return std::make_shared<const pg_global_roles_t>();
  /* The snapshot is replaced as a whole; copy the pointer under the lock. */
  std::lock_guard<std::mutex> lock(shmem->pg_global->mutex);
  return shmem->pg_global->roles;
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
