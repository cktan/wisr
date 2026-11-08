/*
 * rel_ops.cc -- Janet C function bindings for sm.h and relational simulation.
 */

#include "rel.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace rel {

/* (rm-rf path) -- recursive removal of path if it exists */
static Janet cfun_rm_rf(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  const char *path = janet_getcstring(argv, 0);
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return janet_wrap_nil();
}

/* Helper to extract a string identifier from a symbol, keyword, or string. */
static std::string identifier_of(Janet val) {
  if (janet_checktype(val, JANET_SYMBOL)) {
    return reinterpret_cast<const char *>(janet_unwrap_symbol(val));
  }
  if (janet_checktype(val, JANET_STRING)) {
    return reinterpret_cast<const char *>(janet_unwrap_string(val));
  }
  if (janet_checktype(val, JANET_KEYWORD)) {
    return reinterpret_cast<const char *>(janet_unwrap_keyword(val));
  }
  JanetBuffer *buf = janet_buffer(32);
  janet_description_b(buf, val);
  return std::string(reinterpret_cast<char *>(buf->data), buf->count);
}

/* Helper to check if an active session is open. */
static session_ctx_t *require_session() {
  session_ctx_t *s = get_catalog().active_session();
  if (s == nullptr || s->session == nullptr) {
    janet_panic("no active database session; call (db-open ...) first");
  }
  // A new statement: the last one's tables are closed.
  s->stmt_tables.clear();
  return s;
}

/* Helper to check if an environment is attached. */
static sm_env_t *require_env() {
  sm_env_t *env = get_catalog().env();
  if (env == nullptr) {
    janet_panic("no attached environment; call (env-attach ...) first");
  }
  return env;
}

/*
 * After a write on s failed: abort the transaction, as PostgreSQL
 * does on an error, keeping the error for the panic that follows.
 * The write sets no savepoint, so one that failed partway has left
 * its earlier steps done, and only an abort undoes them.
 * 1. Leave the transaction alone on an outcome that is not an
 *    error: SM_ERR_UPDATED, which a read-latest answers;
 *    SM_ERR_BLOCKED, which repeating the step answers; and
 *    SM_ERR_SELFMODIFIED, a row the statement already changed, which
 *    the caller skips.
 * 2. Leave it alone after a retryable error: the engine has aborted
 *    it already.
 * 3. End the cursors, which the abort would close under them.
 * 4. Abort.
 */
static void abort_failed_write(session_ctx_t *s) {
  if (sm_errnum() == SM_ERR_UPDATED || sm_errnum() == SM_ERR_BLOCKED ||
      sm_errnum() == SM_ERR_SELFMODIFIED || sm_retryable()) {
    return;
  }
  // The abort would close the cursors under them.
  end_cursors(s);
  // A successful abort leaves the error state as the write set it.
  sm_abort(s->session);
}

/*
 * Whether the engine aborted the transaction on the last error, so
 * closing its cursors: after a retryable error, SM_ERR_NOMEM or
 * SM_ERR_EXCEPTION (sm.h).
 */
static bool engine_aborted() {
  return sm_retryable() == 1 || sm_errnum() == SM_ERR_NOMEM ||
         sm_errnum() == SM_ERR_EXCEPTION;
}

/* Fail with last sm_ error. */
[[noreturn]] static void panic_sm_error(const char *action) {
  // An error the engine aborted the transaction on closed its cursors.
  session_ctx_t *s = get_catalog().active_session();
  if (s != nullptr && engine_aborted()) {
    forget_cursors(s);
  }
  janet_panicf("%s failed: [%s] %s", action, sm_errname(), sm_errmsg());
}

/* The table name for op, locked in mode; a panic if none, or on error. */
static table_def_t *need_table(session_ctx_t *s, const std::string &name,
                               sm_lockmode_t mode, const char *op) {
  sm_reseterr();
  table_def_t *t = get_catalog().open_table(s, name, mode);
  if (t == nullptr) {
    if (sm_errnum() != SM_OK) {
      panic_sm_error(op);
    }
    janet_panicf("%s: table '%s' not found", op, name.c_str());
  }
  return t;
}

/* The index name and its table for op, locked in mode, as need_table. */
static std::pair<table_def_t *, index_def_t *>
need_index(session_ctx_t *s, const std::string &name, sm_lockmode_t mode,
           const char *op) {
  sm_reseterr();
  auto found = get_catalog().open_index(s, name, mode);
  if (found.first == nullptr) {
    if (sm_errnum() != SM_OK) {
      panic_sm_error(op);
    }
    janet_panicf("%s: index '%s' not found", op, name.c_str());
  }
  return found;
}

/* (env-init path) */
static Janet cfun_env_init(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  const char *path = janet_getcstring(argv, 0);
  // Initialize target directory as wisr environment.
  if (get_catalog().env_init(path) != 0) {
    panic_sm_error("env-init");
  }
  return janet_wrap_nil();
}

/* (env-attach path [:toast bool] [:cache-bytes n]) */
static Janet cfun_env_attach(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 5);
  const char *path = janet_getcstring(argv, 0);
  bool toast = false;
  // sm's default block cache, unless :cache-bytes sizes it.
  sm_env_options_t opt;
  sm_env_options_init(&opt);
  for (int32_t i = 1; i < argc; i += 2) {
    std::string kw = identifier_of(argv[i]);
    if (kw == "toast" || kw == ":toast") {
      // :toast alone, last, means true.
      toast = i + 1 == argc || janet_getboolean(argv, i + 1);
    } else if ((kw == "cache-bytes" || kw == ":cache-bytes") && i + 1 < argc) {
      opt.block_cache_bytes = janet_getinteger64(argv, i + 1);
    } else {
      janet_panic("env-attach: the options after the path are :toast and "
                  ":cache-bytes");
    }
  }
  // Attach to the wisr environment.
  if (get_catalog().env_attach(path, toast, &opt) != 0) {
    panic_sm_error("env-attach");
  }
  return janet_wrap_nil();
}

/* (env-detach [:force bool]) */
static Janet cfun_env_detach(int32_t argc, Janet *argv) {
  int force = 0;
  if (argc >= 1) {
    if (janet_checktype(argv[0], JANET_KEYWORD)) {
      // Check for :force true
      if (argc >= 2) {
        force = janet_getboolean(argv, 1) ? 1 : 0;
      } else {
        force = 1;
      }
    } else if (janet_checktype(argv[0], JANET_BOOLEAN)) {
      force = janet_unwrap_boolean(argv[0]) ? 1 : 0;
    }
  }
  // Detach environment.
  if (get_catalog().env_detach(force) != 0) {
    panic_sm_error("env-detach");
  }
  return janet_wrap_nil();
}

/* (db-create dbname [options]) */
static Janet cfun_db_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  sm_env_t *env = require_env();
  std::string dbname = identifier_of(argv[0]);

  sm_options_t opt;
  sm_options_init(&opt);
  const sm_options_t *popt = &opt;

  if (argc >= 2 && janet_checktype(argv[1], JANET_STRUCT)) {
    JanetStruct st = janet_unwrap_struct(argv[1]);
    Janet ds = janet_struct_get(st, janet_ckeywordv("durability-sync"));
    if (!janet_checktype(ds, JANET_NIL)) {
      opt.durability_sync = janet_unwrap_integer(ds);
    }
    Janet dl = janet_struct_get(st, janet_ckeywordv("deadlock-ms"));
    if (!janet_checktype(dl, JANET_NIL)) {
      opt.deadlock_ms = janet_unwrap_integer(dl);
    }
  } else if (argc >= 2 && janet_checktype(argv[1], JANET_TABLE)) {
    JanetTable *tb = janet_unwrap_table(argv[1]);
    Janet ds = janet_table_get(tb, janet_ckeywordv("durability-sync"));
    if (!janet_checktype(ds, JANET_NIL)) {
      opt.durability_sync = janet_unwrap_integer(ds);
    }
    Janet dl = janet_table_get(tb, janet_ckeywordv("deadlock-ms"));
    if (!janet_checktype(dl, JANET_NIL)) {
      opt.deadlock_ms = janet_unwrap_integer(dl);
    }
  }

  // Create database in the environment.
  if (sm_mkdb(env, dbname.c_str(), popt) != 0) {
    panic_sm_error("db-create");
  }
  return janet_wrap_nil();
}

/* (db-drop dbname) */
static Janet cfun_db_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  sm_env_t *env = require_env();
  std::string dbname = identifier_of(argv[0]);
  // Remove database.
  if (sm_rmdb(env, dbname.c_str()) != 0) {
    panic_sm_error("db-drop");
  }
  return janet_wrap_nil();
}

/* (db-list) */
static Janet cfun_db_list(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  sm_env_t *env = require_env();
  std::vector<std::string> names;
  // List all databases.
  if (sm_lsdb(env, names) != 0) {
    panic_sm_error("db-list");
  }
  JanetArray *arr = janet_array(static_cast<int32_t>(names.size()));
  for (const std::string &name : names) {
    janet_array_push(arr, janet_cstringv(name.c_str()));
  }
  return janet_wrap_array(arr);
}

/* (db-open dbname [options]) */
static Janet cfun_db_open(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  std::string dbname = identifier_of(argv[0]);
  sm_options_t opt;
  sm_options_init(&opt);
  const sm_options_t *popt = nullptr;
  // {:savepoint-undo-bytes n}, for the first session on the database.
  if (argc >= 2) {
    Janet key = janet_ckeywordv("savepoint-undo-bytes");
    Janet u = janet_checktype(argv[1], JANET_STRUCT)
                  ? janet_struct_get(janet_unwrap_struct(argv[1]), key)
              : janet_checktype(argv[1], JANET_TABLE)
                  ? janet_table_get(janet_unwrap_table(argv[1]), key)
                  : janet_wrap_nil();
    if (!janet_checktype(u, JANET_NIL)) {
      opt.savepoint_undo_bytes = janet_unwrap_integer(u);
    }
    popt = &opt;
  }
  // Open default session on database.
  if (get_catalog().session_open("default", dbname, popt) != 0) {
    panic_sm_error("db-open");
  }
  return janet_wrap_nil();
}

/* (db-close) */
static Janet cfun_db_close(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = get_catalog().active_session();
  if (s != nullptr) {
    get_catalog().session_close(s->name);
  }
  return janet_wrap_nil();
}

/* (session-open name dbname [options]) */
static Janet cfun_session_open(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  std::string name = identifier_of(argv[0]);
  std::string dbname = identifier_of(argv[1]);
  // Open a named session.
  if (get_catalog().session_open(name, dbname, nullptr) != 0) {
    panic_sm_error("session-open");
  }
  return janet_wrap_nil();
}

/* (session-use name) */
static Janet cfun_session_use(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  std::string name = identifier_of(argv[0]);
  if (get_catalog().session_use(name) != 0) {
    janet_panicf("session '%s' does not exist", name.c_str());
  }
  return janet_wrap_nil();
}

/* (session-close name) */
static Janet cfun_session_close(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  std::string name = identifier_of(argv[0]);
  get_catalog().session_close(name);
  return janet_wrap_nil();
}

/* (begin [level]) */
static Janet cfun_begin(int32_t argc, Janet *argv) {
  janet_arity(argc, 0, 2);
  session_ctx_t *s = require_session();
  sm_xctlevel_t level = SM_XCT_READ_COMMITTED;
  if (argc >= 1) {
    std::string lvl = identifier_of(argv[0]);
    if (lvl == "rc" || lvl == "read-committed" || lvl == ":rc" ||
        lvl == ":read-committed") {
      level = SM_XCT_READ_COMMITTED;
    } else if (lvl == "rr" || lvl == "repeatable-read" || lvl == ":rr" ||
               lvl == ":repeatable-read") {
      level = SM_XCT_REPEATABLE_READ;
    } else if (lvl == "ser" || lvl == "serializable" || lvl == ":ser" ||
               lvl == ":serializable") {
      level = SM_XCT_SERIALIZABLE;
    } else {
      janet_panicf("unknown isolation level '%s'", lvl.c_str());
    }
  }
  int read_only = 0;
  if (argc == 2) {
    std::string option = identifier_of(argv[1]);
    if (option != "read-only" && option != ":read-only") {
      janet_panic("begin: the option after the level is :read-only");
    }
    read_only = 1;
  }
  // Begin transaction at the requested isolation level.
  if (sm_begin(s->session, level, read_only) != 0) {
    panic_sm_error("begin");
  }
  return janet_wrap_nil();
}

/* (commit) */
static Janet cfun_commit(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  // The transaction's end closes its cursors: end them first.
  end_cursors(s);
  // Commit transaction.
  if (sm_commit(s->session) != 0) {
    panic_sm_error("commit");
  }
  return janet_wrap_nil();
}

/* (abort) */
static Janet cfun_abort(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  // The transaction's end closes its cursors: end them first.
  end_cursors(s);
  // Abort transaction.
  if (sm_abort(s->session) != 0) {
    panic_sm_error("abort");
  }
  return janet_wrap_nil();
}

/* (snapshot) */
static Janet cfun_snapshot(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  if (sm_stmt_begin(s->session) != 0) {
    panic_sm_error("snapshot");
  }
  return janet_wrap_nil();
}

/*
 * (cat-epoch) -- the catalog's epoch as the session sees it
 * (sm_cat_epoch), or :own while its transaction has DDL of its own.
 */
static Janet cfun_cat_epoch(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  uint64_t epoch = sm_cat_epoch(s->session);
  if (UINT64_MAX == epoch) {
    return janet_ckeywordv("own");
  }
  return janet_wrap_number(static_cast<double>(epoch));
}

/* (stmt-inner) */
static Janet cfun_stmt_inner(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  sm_stmt_t *outer = nullptr;
  if (sm_stmt_begin_inner(s->session, &outer) != 0) {
    panic_sm_error("stmt-inner");
  }
  s->outer_stmts.push_back(outer);
  return janet_wrap_nil();
}

/* (stmt-end) */
static Janet cfun_stmt_end(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  if (s->outer_stmts.empty()) {
    janet_panic("stmt-end: no statement begun by (stmt-inner)");
  }
  sm_stmt_t *outer = s->outer_stmts.back();
  s->outer_stmts.pop_back();
  if (sm_stmt_end(s->session, outer) != 0) {
    panic_sm_error("stmt-end");
  }
  return janet_wrap_nil();
}

/* The view (stmt-save) gave as handle argv[n]; a panic if none. */
static sm_stmt_t *need_view(session_ctx_t *s, Janet *argv, int32_t n,
                            const char *op) {
  auto it = s->views.find(janet_getinteger(argv, n));
  if (it == s->views.end()) {
    janet_panicf("%s: no view %d", op, janet_getinteger(argv, n));
  }
  return it->second;
}

/* (stmt-save) -- the running statement's view, as a handle */
static Janet cfun_stmt_save(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  sm_stmt_t *view = nullptr;
  if (sm_stmt_save(s->session, &view) != 0) {
    panic_sm_error("stmt-save");
  }
  s->views[++s->next_view] = view;
  return janet_wrap_integer(s->next_view);
}

/* (stmt-resume v) */
static Janet cfun_stmt_resume(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  sm_stmt_t *view = need_view(s, argv, 0, "stmt-resume");
  sm_stmt_t *outer = nullptr;
  if (sm_stmt_resume(s->session, view, &outer) != 0) {
    panic_sm_error("stmt-resume");
  }
  // Ended by (stmt-end), as a statement (stmt-inner) began.
  s->outer_stmts.push_back(outer);
  return janet_wrap_nil();
}

/* (stmt-free v) */
static Janet cfun_stmt_free(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  sm_stmt_free(need_view(s, argv, 0, "stmt-free"));
  s->views.erase(janet_getinteger(argv, 0));
  return janet_wrap_nil();
}

/* (stmt-advance) */
static Janet cfun_stmt_advance(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  if (sm_stmt_advance(s->session) != 0) {
    panic_sm_error("stmt-advance");
  }
  return janet_wrap_nil();
}

/* (qa-nowait on) */
static Janet cfun_qa_nowait(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  if (sm_set_qa_nowait(s->session, janet_getboolean(argv, 0)) != 0) {
    panic_sm_error("qa-nowait");
  }
  return janet_wrap_nil();
}

/* (savepoint) */
static Janet cfun_savepoint(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  int depth = sm_savepoint(s->session);
  if (depth < 0) {
    panic_sm_error("savepoint");
  }
  return janet_wrap_integer(depth);
}

/* (rollback depth) */
static Janet cfun_rollback(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  int depth = janet_getinteger(argv, 0);
  if (sm_rollback(s->session, depth) != 0) {
    panic_sm_error("rollback");
  }
  return janet_wrap_nil();
}

/* (forget depth) */
static Janet cfun_forget(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  int depth = janet_getinteger(argv, 0);
  if (sm_forget(s->session, depth) != 0) {
    panic_sm_error("forget");
  }
  return janet_wrap_nil();
}

/* (prepare gid) */
static Janet cfun_prepare(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const char *gid = janet_getcstring(argv, 0);
  // The transaction's end closes its cursors: end them first.
  end_cursors(s);
  if (sm_prepare(s->session, gid) != 0) {
    panic_sm_error("prepare");
  }
  return janet_wrap_nil();
}

/* (prepared-list) */
static Janet cfun_prepared_list(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
  std::vector<sm_prepared_t> list;
  if (sm_list_prepared(s->session, list) != 0) {
    panic_sm_error("prepared-list");
  }
  JanetArray *arr = janet_array(static_cast<int32_t>(list.size()));
  for (const sm_prepared_t &p : list) {
    JanetTable *t = janet_table(2);
    janet_table_put(t, janet_ckeywordv("gid"), janet_cstringv(p.gid.c_str()));
    janet_table_put(t, janet_ckeywordv("prepared-at"),
                    janet_wrap_number(static_cast<double>(p.prepared_at)));
    janet_array_push(arr, janet_wrap_table(t));
  }
  return janet_wrap_array(arr);
}

/* (commit-prepared gid) */
static Janet cfun_commit_prepared(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const char *gid = janet_getcstring(argv, 0);
  if (sm_commit_prepared(s->session, gid) != 0) {
    panic_sm_error("commit-prepared");
  }
  return janet_wrap_nil();
}

/* (abort-prepared gid) */
static Janet cfun_abort_prepared(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const char *gid = janet_getcstring(argv, 0);
  if (sm_abort_prepared(s->session, gid) != 0) {
    panic_sm_error("abort-prepared");
  }
  return janet_wrap_nil();
}

/*
 * The table name's oid, as a search path of the session's temporary
 * tables and then public finds it: 0, 1 if there is none, or -1.
 */
int rel_table_oid(session_ctx_t *s, const std::string &name, uint64_t *oid) {
  int rc = sm_cat_table_oid(s->session, SM_NSP_TEMP, name.c_str(), oid);
  return rc == 1
             ? sm_cat_table_oid(s->session, SM_NSP_PUBLIC, name.c_str(), oid)
             : rc;
}

/* The index name and its table, found as rel_table_oid finds a table. */
int rel_index_oid(session_ctx_t *s, const std::string &name, uint64_t *table,
                  uint64_t *index) {
  int rc =
      sm_cat_index_oid(s->session, SM_NSP_TEMP, name.c_str(), table, index);
  return rc == 1 ? sm_cat_index_oid(s->session, SM_NSP_PUBLIC, name.c_str(),
                                    table, index)
                 : rc;
}

/*
 * Make the columns at attnums the primary key of the table oid, name,
 * as PostgreSQL names one: name_pkey. sm keys the table's rows by it,
 * rewriting a table that has rows (../CLUSTER.md section 8); op names
 * the command in a panic.
 */
static void create_primary_key(session_ctx_t *s, uint64_t oid,
                               const std::string &name,
                               const std::vector<int32_t> &attnums,
                               const char *op) {
  std::vector<sm_index_key_spec_t> keys;
  for (int32_t att : attnums) {
    keys.push_back({att, false, false, 0, -1});
  }
  uint64_t index = 0;
  if (sm_cat_create_index(s->session, oid, (name + "_pkey").c_str(), keys, {},
                          true, "", {}, &index, true, true) != 0) {
    panic_sm_error(op);
  }
}

/*
 * The places of the columns names among columns, in their order, as
 * a primary key takes them; a panic for none, a name not there, or
 * one named twice.
 */
static std::vector<int32_t> key_places(const std::vector<col_def_t> &columns,
                                       const std::vector<std::string> &names,
                                       const std::string &tbl_name,
                                       const char *op) {
  if (names.empty()) {
    janet_panicf("%s: a primary key needs a column", op);
  }
  std::vector<int32_t> places;
  for (const std::string &n : names) {
    auto it = std::find_if(columns.begin(), columns.end(),
                           [&n](const col_def_t &c) { return c.name == n; });
    if (it == columns.end()) {
      janet_panicf("%s: column '%s' not found in table '%s'", op, n.c_str(),
                   tbl_name.c_str());
    }
    int32_t place = static_cast<int32_t>(it - columns.begin());
    if (std::find(places.begin(), places.end(), place) != places.end()) {
      janet_panicf("%s: column '%s' named twice in the primary key", op,
                   n.c_str());
    }
    places.push_back(place);
  }
  return places;
}

/*
 * (table-create name (columns ...) [(primary-key c...)]) and
 * (temp-table-create ...)
 */
static Janet table_create(int32_t argc, Janet *argv, bool temp) {
  janet_arity(argc, 2, -1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  const char *op = temp ? "temp-table-create" : "table-create";

  std::vector<col_def_t> columns;
  std::vector<std::string> pk_names;
  bool has_pk = false;

  std::vector<Janet> clauses;
  if (argc == 2 && (janet_checktype(argv[1], JANET_TUPLE) ||
                    janet_checktype(argv[1], JANET_ARRAY))) {
    int32_t len = 0;
    const Janet *data = nullptr;
    if (janet_checktype(argv[1], JANET_TUPLE)) {
      data = janet_unwrap_tuple(argv[1]);
      len = janet_tuple_length(data);
    } else {
      data = janet_unwrap_array(argv[1])->data;
      len = janet_unwrap_array(argv[1])->count;
    }
    if (len > 0 && identifier_of(data[0]) == "columns") {
      clauses.push_back(argv[1]);
    } else {
      for (int32_t i = 0; i < len; i++) {
        clauses.push_back(data[i]);
      }
    }
  } else {
    for (int32_t i = 1; i < argc; i++) {
      clauses.push_back(argv[i]);
    }
  }

  for (Janet arg : clauses) {
    const Janet *items = nullptr;
    int32_t nitems = 0;
    if (janet_checktype(arg, JANET_TUPLE)) {
      items = janet_unwrap_tuple(arg);
      nitems = janet_tuple_length(items);
    } else if (janet_checktype(arg, JANET_ARRAY)) {
      items = janet_unwrap_array(arg)->data;
      nitems = janet_unwrap_array(arg)->count;
    } else {
      continue;
    }

    if (nitems == 0)
      continue;

    if (identifier_of(items[0]) == "columns") {
      for (int32_t j = 1; j < nitems; j++) {
        Janet col_tuple = items[j];
        const Janet *c_items = nullptr;
        int32_t c_n = 0;
        if (janet_checktype(col_tuple, JANET_TUPLE)) {
          c_items = janet_unwrap_tuple(col_tuple);
          c_n = janet_tuple_length(c_items);
        } else if (janet_checktype(col_tuple, JANET_ARRAY)) {
          c_items = janet_unwrap_array(col_tuple)->data;
          c_n = janet_unwrap_array(col_tuple)->count;
        }
        if (c_n < 2)
          continue;
        col_def_t c;
        c.name = identifier_of(c_items[0]);
        if (parse_type_code(c_items[1], &c.type_code) != 0) {
          janet_panicf("table-create: unknown column type for '%s'",
                       c.name.c_str());
        }
        c.typmod = -1;
        c.not_null = false;
        // Numbers after the type are its typmod, as in decimal(p, s).
        std::vector<int32_t> mods;
        for (int32_t k = 2; k < c_n; k++) {
          if (janet_checktype(c_items[k], JANET_NUMBER)) {
            mods.push_back(janet_unwrap_integer(c_items[k]));
            continue;
          }
          std::string flag = identifier_of(c_items[k]);
          if (flag == "not-null" || flag == ":not-null") {
            c.not_null = true;
          } else if ("on-update" == flag) {
            // The host's update default, which sm keeps unread.
            if (k + 1 >= c_n ||
                !janet_checktype(c_items[k + 1], JANET_STRING)) {
              janet_panicf("table-create: column '%s' takes :on-update and "
                           "a string",
                           c.name.c_str());
            }
            c.on_update_expr = identifier_of(c_items[++k]);
          } else if ("default" == flag) {
            // The host's default, which sm keeps unread.
            if (k + 1 >= c_n ||
                !janet_checktype(c_items[k + 1], JANET_STRING)) {
              janet_panicf("table-create: column '%s' takes :default and a "
                           "string",
                           c.name.c_str());
            }
            c.default_expr = identifier_of(c_items[++k]);
          }
        }
        if (typmod_of(c.type_code, mods, &c.typmod) != 0) {
          janet_panicf("table-create: type %s takes no such modifiers, for "
                       "column '%s'",
                       type_name_of(c.type_code), c.name.c_str());
        }
        columns.push_back(std::move(c));
      }
    } else if (identifier_of(items[0]) == "primary-key") {
      if (has_pk) {
        janet_panicf("%s: a table has one primary key", op);
      }
      has_pk = true;
      for (int32_t j = 1; j < nitems; j++) {
        pk_names.push_back(identifier_of(items[j]));
      }
    }
  }

  if (columns.empty()) {
    janet_panic("table-create: table must have at least one column");
  }

  // A primary key's columns are NOT NULL, as SQL makes them.
  std::vector<int32_t> pk_places;
  if (has_pk) {
    pk_places = key_places(columns, pk_names, tbl_name, op);
    for (int32_t place : pk_places) {
      columns[place].not_null = true;
    }
  }

  // sm3 makes its store, _id sequence and catalog entry.
  std::vector<sm_column_spec_t> specs;
  for (const col_def_t &c : columns) {
    specs.push_back({c.name.c_str(), c.type_code, c.typmod, c.not_null,
                     c.default_expr ? c.default_expr->c_str() : nullptr, 0, 0,
                     c.on_update_expr ? c.on_update_expr->c_str() : nullptr});
  }
  uint64_t oid = 0;
  int rc =
      temp ? sm_cat_create_temp_table(s->session, tbl_name.c_str(), specs, &oid)
           : sm_cat_create_table(s->session, SM_NSP_PUBLIC, tbl_name.c_str(),
                                 specs, &oid);
  if (rc != 0) {
    panic_sm_error(op);
  }
  // Keyed from its birth: an empty table, so nothing is rewritten.
  if (has_pk) {
    create_primary_key(s, oid, tbl_name, pk_places, op);
  }
  return janet_wrap_nil();
}

/*
 * (add-primary-key table c...): make the columns, each NOT NULL, the
 * table's primary key, as ALTER TABLE ... ADD PRIMARY KEY; sm rewrites
 * a table that has rows, keyed by it.
 */
static Janet cfun_add_primary_key(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  JanetView names = janet_getindexed(argv, 1);
  std::vector<std::string> pk_names;
  for (int32_t i = 0; i < names.len; i++) {
    pk_names.push_back(identifier_of(names.items[i]));
  }
  // X, as the DDL takes it: the columns cannot change under us.
  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCKMODE_EX, "add-primary-key");
  std::vector<int32_t> places =
      key_places(p_tdef->columns, pk_names, tbl_name, "add-primary-key");
  create_primary_key(s, p_tdef->def->oid, tbl_name, places, "add-primary-key");
  return janet_wrap_nil();
}

/* A column's place, and its type before and after, for retype_conv. */
struct retype_t {
  size_t at;
  uint8_t from_code;
  int32_t from_typmod;
  uint8_t to_code;
  int32_t to_typmod;
};

/*
 * rel's converter for alter-column-type: the old value printed as
 * its type's canonical text, and that text parsed as the new type,
 * as a cast through text does.
 */
static int retype_conv(void *ctx, const sm_datum_t *values, const bool *isnull,
                       size_t n, std::string &out, bool *out_isnull) noexcept {
  const retype_t *r = static_cast<const retype_t *>(ctx);
  if (r->at >= n) {
    return sm_seterr(SM_ERR_INVALID, "alter-column-type: no such column");
  }
  *out_isnull = isnull[r->at];
  if (*out_isnull) {
    return 0;
  }
  std::string text, buf;
  sm_datum_t d = 0;
  if (sm_type_out(sm_type(r->from_code), r->from_typmod, values[r->at], text) !=
          0 ||
      sm_type_in(sm_type(r->to_code), r->to_typmod, text.data(), text.size(),
                 &d, buf) != 0) {
    return -1;
  }
  // A by-reference value is in buf; a by-value one is the datum's.
  if (!buf.empty()) {
    out = std::move(buf);
    return 0;
  }
  size_t width = sm_type_byval_width(sm_type(r->to_code));
  if (0 == width) {
    return sm_seterr(SM_ERR_INVALID, "alter-column-type: a type rel cannot "
                                     "convert to");
  }
  out.assign(reinterpret_cast<const char *>(&d), width);
  return 0;
}

/*
 * (alter-column-type table column type mods... [:keep]): change the
 * column's type, as ALTER TABLE ... ALTER COLUMN ... TYPE; each row's
 * value is converted through its text (retype_conv), and with :keep
 * none is, the record alone changing.
 */
static Janet cfun_alter_column_type(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, -1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  std::string col_name = identifier_of(argv[1]);
  uint8_t code = 0;
  if (parse_type_code(argv[2], &code) != 0) {
    janet_panicf("alter-column-type: unknown type for '%s'", col_name.c_str());
  }
  std::vector<int32_t> mods;
  bool keep = false;
  for (int32_t k = 3; k < argc; k++) {
    if (janet_checktype(argv[k], JANET_NUMBER)) {
      mods.push_back(janet_unwrap_integer(argv[k]));
    } else if ("keep" == identifier_of(argv[k])) {
      keep = true;
    } else {
      janet_panicf("alter-column-type: unknown flag");
    }
  }
  sm_column_type_t to{code, -1};
  if (typmod_of(code, mods, &to.typmod) != 0) {
    janet_panicf("alter-column-type: type %s takes no such modifiers",
                 type_name_of(code));
  }
  // X, as the DDL takes it: the columns cannot change under us.
  table_def_t *t = need_table(s, tbl_name, SM_LOCKMODE_EX, "alter-column-type");
  retype_t r{SIZE_MAX, 0, -1, code, to.typmod};
  for (size_t i = 0; i < t->columns.size(); i++) {
    if (t->columns[i].name == col_name) {
      r = {i, t->columns[i].type_code, t->columns[i].typmod, code, to.typmod};
    }
  }
  if (SIZE_MAX == r.at) {
    janet_panicf("alter-column-type: table '%s' has no column '%s'",
                 tbl_name.c_str(), col_name.c_str());
  }
  if (sm_cat_alter_column_type(s->session, t->def->oid, col_name.c_str(), to,
                               keep ? nullptr : retype_conv, &r) != 0) {
    panic_sm_error("alter-column-type");
  }
  return janet_wrap_nil();
}

static Janet tuple_of(const std::vector<Janet> &items);

/* The oid of the table name for op, under X, as the DDL takes it. */
static uint64_t ddl_table(session_ctx_t *s, const std::string &name,
                          const char *op) {
  return need_table(s, name, SM_LOCKMODE_EX, op)->def->oid;
}

/*
 * (set-not-null table column [name]): ALTER COLUMN ... SET NOT NULL,
 * its constraint named name, or by sm.
 */
static Janet cfun_set_not_null(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  session_ctx_t *s = require_session();
  std::string col = identifier_of(argv[1]);
  std::string name = 3 == argc ? identifier_of(argv[2]) : "";
  uint64_t oid = ddl_table(s, identifier_of(argv[0]), "set-not-null");
  if (sm_cat_set_not_null(s->session, oid, col.c_str(),
                          3 == argc ? name.c_str() : nullptr) != 0) {
    panic_sm_error("set-not-null");
  }
  return janet_wrap_nil();
}

/* (rename-not-null table column name): its NOT NULL constraint renamed. */
static Janet cfun_rename_not_null(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  std::string col = identifier_of(argv[1]);
  std::string to = identifier_of(argv[2]);
  uint64_t oid = ddl_table(s, identifier_of(argv[0]), "rename-not-null");
  if (sm_cat_rename_not_null(s->session, oid, col.c_str(), to.c_str()) != 0) {
    panic_sm_error("rename-not-null");
  }
  return janet_wrap_nil();
}

/*
 * (not-nulls table): the table's NOT NULL constraints, each [column
 * name], in the columns' order (NOTNULL.md).
 */
static Janet cfun_not_nulls(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const sm_table_def_t &def =
      *need_table(s, identifier_of(argv[0]), SM_LOCKMODE_IS, "not-nulls")->def;
  std::vector<Janet> out;
  for (const sm_column_def_t &c : def.columns) {
    if (0 != c.not_null_oid) {
      out.push_back(tuple_of({janet_csymbolv(c.name.c_str()),
                              janet_cstringv(c.not_null_name.c_str())}));
    }
  }
  return tuple_of(out);
}

/* (drop-not-null table column): ALTER COLUMN ... DROP NOT NULL. */
static Janet cfun_drop_not_null(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string col = identifier_of(argv[1]);
  uint64_t oid = ddl_table(s, identifier_of(argv[0]), "drop-not-null");
  if (sm_cat_drop_not_null(s->session, oid, col.c_str()) != 0) {
    panic_sm_error("drop-not-null");
  }
  return janet_wrap_nil();
}

/*
 * (set-default table column ["text"]): ALTER COLUMN ... SET DEFAULT,
 * the host's text, or DROP DEFAULT with none.
 */
static Janet cfun_set_default(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  session_ctx_t *s = require_session();
  std::string col = identifier_of(argv[1]);
  std::optional<std::string> expr;
  if (3 == argc) {
    expr = janet_getcstring(argv, 2);
  }
  uint64_t oid = ddl_table(s, identifier_of(argv[0]), "set-default");
  if (sm_cat_set_default(s->session, oid, col.c_str(),
                         expr ? expr->c_str() : nullptr) != 0) {
    panic_sm_error("set-default");
  }
  return janet_wrap_nil();
}

/* (table-create name (columns ...)) */
static Janet cfun_table_create(int32_t argc, Janet *argv) {
  return table_create(argc, argv, false);
}

/* (temp-table-create name (columns ...)) */
static Janet cfun_temp_table_create(int32_t argc, Janet *argv) {
  return table_create(argc, argv, true);
}

/* (table-drop name) */
static Janet cfun_table_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  uint64_t oid = 0;
  int rc = rel_table_oid(s, tbl_name, &oid);
  if (rc == 1) {
    janet_panicf("table-drop: table '%s' does not exist", tbl_name.c_str());
  }
  // Its store, rows, indexes and _id sequence go with it.
  if (rc != 0 || sm_cat_drop_table(s->session, oid) != 0) {
    panic_sm_error("table-drop");
  }
  return janet_wrap_nil();
}

/* (seq-create name [start] [increment]) */
static Janet cfun_seq_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 3);
  session_ctx_t *s = require_session();
  std::string name = identifier_of(argv[0]);
  uint64_t start = 1;
  uint64_t increment = 1;
  if (argc >= 2) {
    start = static_cast<uint64_t>(janet_getinteger64(argv, 1));
  }
  if (argc >= 3) {
    increment = static_cast<uint64_t>(janet_getinteger64(argv, 2));
  }
  if (sm_mkseq(s->session, name.c_str(), start, increment) != 0) {
    panic_sm_error("seq-create sm_mkseq");
  }
  return janet_wrap_nil();
}

/* (seq-drop name) */
static Janet cfun_seq_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string name = identifier_of(argv[0]);
  if (sm_rmseq(s->session, name.c_str()) != 0) {
    panic_sm_error("seq-drop sm_rmseq");
  }
  return janet_wrap_nil();
}

/* (nextseq name) */
static Janet cfun_nextseq(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string name = identifier_of(argv[0]);
  uint64_t val = 0;
  if (sm_nextseq(s->session, name.c_str(), &val) != 0) {
    panic_sm_error("nextseq sm_nextseq");
  }
  return janet_wrap_number(static_cast<double>(val));
}

/*
 * (index-create idx_name (table tbl) (key ...) [:unique bool] [:include ...]
 *               [:where (col value)] [:tree bool])
 */
static Janet cfun_index_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, -1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  std::string tbl_name;
  index_def_t idef;
  idef.name = idx_name;
  idef.unique = false;
  // :tree bool: a B+tree of its own pages (../BTREEINDEX.md).
  bool tree = false;
  // :where (column value): a partial index of the rows whose
  // column prints as value.
  bool has_where = false;
  std::string where_col, where_text;

  std::vector<Janet> clauses;
  if (argc == 2 && (janet_checktype(argv[1], JANET_TUPLE) ||
                    janet_checktype(argv[1], JANET_ARRAY))) {
    int32_t len = 0;
    const Janet *data = nullptr;
    if (janet_checktype(argv[1], JANET_TUPLE)) {
      data = janet_unwrap_tuple(argv[1]);
      len = janet_tuple_length(data);
    } else {
      data = janet_unwrap_array(argv[1])->data;
      len = janet_unwrap_array(argv[1])->count;
    }
    for (int32_t i = 0; i < len; i++) {
      clauses.push_back(data[i]);
    }
  } else {
    for (int32_t i = 1; i < argc; i++) {
      clauses.push_back(argv[i]);
    }
  }

  for (size_t i = 0; i < clauses.size(); i++) {
    Janet arg = clauses[i];
    if (janet_checktype(arg, JANET_KEYWORD)) {
      std::string kw = identifier_of(arg);
      if (kw == "unique" && i + 1 < clauses.size()) {
        idef.unique = janet_unwrap_boolean(clauses[i + 1]);
        i++;
      } else if (kw == "tree" && i + 1 < clauses.size()) {
        tree = janet_unwrap_boolean(clauses[i + 1]);
        i++;
      } else if (kw == "where" && i + 1 < clauses.size()) {
        Janet w = clauses[i + 1];
        i++;
        if (!janet_checktype(w, JANET_TUPLE) ||
            janet_tuple_length(janet_unwrap_tuple(w)) != 2) {
          janet_panic("index-create: :where takes (column value)");
        }
        const Janet *w_items = janet_unwrap_tuple(w);
        has_where = true;
        where_col = identifier_of(w_items[0]);
        JanetString text = janet_to_string(w_items[1]);
        where_text.assign(reinterpret_cast<const char *>(text),
                          janet_string_length(text));
      } else if (kw == "include" && i + 1 < clauses.size()) {
        Janet inc_arg = clauses[i + 1];
        i++;
        const Janet *inc_items = nullptr;
        int32_t inc_n = 0;
        if (janet_checktype(inc_arg, JANET_TUPLE)) {
          inc_items = janet_unwrap_tuple(inc_arg);
          inc_n = janet_tuple_length(inc_items);
        } else if (janet_checktype(inc_arg, JANET_ARRAY)) {
          inc_items = janet_unwrap_array(inc_arg)->data;
          inc_n = janet_unwrap_array(inc_arg)->count;
        } else {
          inc_items = &inc_arg;
          inc_n = 1;
        }
        for (int32_t j = 0; j < inc_n; j++) {
          include_col_def_t inc;
          inc.name = identifier_of(inc_items[j]);
          inc.col_idx = 0;
          inc.type_code = 0;
          inc.typmod = -1;
          idef.includes.push_back(std::move(inc));
        }
      }
      continue;
    }

    const Janet *items = nullptr;
    int32_t nitems = 0;
    if (janet_checktype(arg, JANET_TUPLE)) {
      items = janet_unwrap_tuple(arg);
      nitems = janet_tuple_length(items);
    } else if (janet_checktype(arg, JANET_ARRAY)) {
      items = janet_unwrap_array(arg)->data;
      nitems = janet_unwrap_array(arg)->count;
    } else {
      continue;
    }

    if (nitems < 1)
      continue;

    std::string head = identifier_of(items[0]);

    if (head == "table" && nitems >= 2) {
      tbl_name = identifier_of(items[1]);
    } else if (head == "key") {
      for (int32_t j = 1; j < nitems; j++) {
        Janet k_tuple = items[j];
        const Janet *k_items = nullptr;
        int32_t k_n = 0;
        if (janet_checktype(k_tuple, JANET_TUPLE)) {
          k_items = janet_unwrap_tuple(k_tuple);
          k_n = janet_tuple_length(k_items);
        } else if (janet_checktype(k_tuple, JANET_ARRAY)) {
          k_items = janet_unwrap_array(k_tuple)->data;
          k_n = janet_unwrap_array(k_tuple)->count;
        }
        if (k_n < 1)
          continue;
        key_col_def_t k;
        k.col_idx = 0;
        k.type_code = 0;
        k.typmod = -1;
        k.collation = nullptr;
        k.desc = false;
        k.nulls_first = false;
        // (lower column) or (upper column) in a column's place is
        // an expression, which rel's evaluator computes.
        if (janet_checktype(k_items[0], JANET_TUPLE)) {
          const Janet *e_items = janet_unwrap_tuple(k_items[0]);
          if (janet_tuple_length(e_items) != 2) {
            janet_panic("index-create: an expression is (lower column) or "
                        "(upper column)");
          }
          k.expr = identifier_of(e_items[0]);
          if (k.expr != "lower" && k.expr != "upper") {
            janet_panicf("index-create: no function '%s'", k.expr.c_str());
          }
          k.collation_name = identifier_of(e_items[1]);
        } else {
          k.collation_name = identifier_of(k_items[0]);
        }

        for (int32_t m = 1; m < k_n; m++) {
          std::string flag = identifier_of(k_items[m]);
          if (flag == "desc" || flag == ":desc")
            k.desc = true;
          else if (flag == "asc" || flag == ":asc")
            k.desc = false;
          else if (flag == "nulls-first" || flag == ":nulls-first")
            k.nulls_first = true;
          else if (flag == "nulls-last" || flag == ":nulls-last")
            k.nulls_first = false;
        }
        idef.keys.push_back(k);
      }
    } else if (head == "include") {
      for (int32_t j = 1; j < nitems; j++) {
        include_col_def_t inc;
        inc.name = identifier_of(items[j]);
        inc.col_idx = 0;
        inc.type_code = 0;
        inc.typmod = -1;
        idef.includes.push_back(std::move(inc));
      }
    }
  }

  if (tbl_name.empty()) {
    janet_panic("index-create: (table <table_name>) clause is required");
  }

  // X, as the DDL takes it: the columns cannot change under us.
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_EX, "index-create");

  // Resolve key columns against table schema.
  for (auto &k : idef.keys) {
    std::string col_name = k.collation_name;
    k.collation_name.clear();
    bool found = false;
    for (size_t idx = 0; idx < p_tdef->columns.size(); idx++) {
      if (p_tdef->columns[idx].name == col_name) {
        k.col_idx = idx;
        k.type_code = p_tdef->columns[idx].type_code;
        k.typmod = p_tdef->columns[idx].typmod;
        found = true;
        break;
      }
    }
    if (!found) {
      janet_panicf("index-create: column '%s' not found in table '%s'",
                   col_name.c_str(), tbl_name.c_str());
    }
  }

  // Resolve INCLUDE columns against table schema.
  for (auto &inc : idef.includes) {
    bool found = false;
    for (size_t idx = 0; idx < p_tdef->columns.size(); idx++) {
      if (p_tdef->columns[idx].name == inc.name) {
        inc.col_idx = idx;
        inc.type_code = p_tdef->columns[idx].type_code;
        inc.typmod = p_tdef->columns[idx].typmod;
        found = true;
        break;
      }
    }
    if (!found) {
      janet_panicf("index-create: include column '%s' not found in table '%s'",
                   inc.name.c_str(), tbl_name.c_str());
    }
  }

  // What rel's evaluator is to make of it: the predicate, and each
  // key column that is an expression, with the columns they read.
  index_eval_t ev;
  std::vector<int32_t> reads;
  auto read = [&](int32_t att) {
    if (std::find(reads.begin(), reads.end(), att) == reads.end()) {
      reads.push_back(att);
    }
  };
  if (has_where) {
    bool found = false;
    for (size_t idx = 0; idx < p_tdef->columns.size(); idx++) {
      if (p_tdef->columns[idx].name == where_col) {
        ev.has_where = true;
        ev.where_att = static_cast<int32_t>(idx);
        ev.where_code = p_tdef->columns[idx].type_code;
        ev.where_typmod = p_tdef->columns[idx].typmod;
        ev.where_text = where_text;
        read(ev.where_att);
        found = true;
      }
    }
    if (!found) {
      janet_panicf("index-create: column '%s' not found in table '%s'",
                   where_col.c_str(), tbl_name.c_str());
    }
  }

  // sm3 records it, under the table's next relid.
  std::vector<sm_index_key_spec_t> keys;
  for (const key_col_def_t &k : idef.keys) {
    int32_t att = static_cast<int32_t>(k.col_idx);
    if (k.expr.empty()) {
      keys.push_back({att, k.desc, k.nulls_first, 0, -1});
      continue;
    }
    // The expression is text of a text column, whose type it keeps.
    if (std::string(type_name_of(k.type_code)) != "text" &&
        std::string(type_name_of(k.type_code)) != "varchar") {
      janet_panicf("index-create: (%s ...) takes a text column",
                   k.expr.c_str());
    }
    ev.exprs.emplace_back(k.expr, att);
    read(att);
    keys.push_back({-1, k.desc, k.nulls_first, k.type_code, -1});
  }
  std::vector<int32_t> includes;
  for (const include_col_def_t &inc : idef.includes) {
    includes.push_back(static_cast<int32_t>(inc.col_idx));
  }
  uint64_t oid = 0;
  if (sm_cat_create_index(s->session, p_tdef->def->oid, idx_name.c_str(), keys,
                          includes, idef.unique, format_index_eval(ev), reads,
                          &oid, false, false, tree) != 0) {
    panic_sm_error("index-create");
  }
  return janet_wrap_nil();
}

/* (namespace-create name) */
static Janet cfun_namespace_create(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string name = identifier_of(argv[0]);
  uint64_t oid = 0;
  if (sm_cat_create_namespace(s->session, name.c_str(), &oid) != 0) {
    panic_sm_error("namespace-create");
  }
  return janet_wrap_nil();
}

/* (namespace-drop name) */
static Janet cfun_namespace_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string name = identifier_of(argv[0]);
  uint64_t oid = 0;
  int rc = sm_cat_namespace_oid(s->session, name.c_str(), &oid);
  if (rc == 1) {
    janet_panicf("namespace-drop: namespace '%s' does not exist", name.c_str());
  }
  // It must be empty, and not one of the two built in.
  if (rc != 0 || sm_cat_drop_namespace(s->session, oid) != 0) {
    panic_sm_error("namespace-drop");
  }
  return janet_wrap_nil();
}

/* (list-namespaces): the namespace names, sorted. */
static Janet cfun_list_namespaces(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  std::vector<std::string> names;
  if (sm_cat_list_namespaces(s->session, names) != 0) {
    panic_sm_error("list-namespaces");
  }
  JanetArray *out = janet_array(static_cast<int32_t>(names.size()));
  for (const std::string &name : names) {
    janet_array_push(out, janet_cstringv(name.c_str()));
  }
  return janet_wrap_array(out);
}

/* (list-tables): the table names in public, sorted. */
static Janet cfun_list_tables(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  std::vector<std::string> names;
  if (sm_cat_list_tables(s->session, SM_NSP_PUBLIC, names) != 0) {
    panic_sm_error("list-tables");
  }
  JanetArray *out = janet_array(static_cast<int32_t>(names.size()));
  for (const std::string &name : names) {
    janet_array_push(out, janet_cstringv(name.c_str()));
  }
  return janet_wrap_array(out);
}

/* A bracket tuple of items, as [...] reads, so = matches a literal. */
static Janet tuple_of(const std::vector<Janet> &items) {
  Janet *t = janet_tuple_begin(static_cast<int32_t>(items.size()));
  std::copy(items.begin(), items.end(), t);
  janet_tuple_flag(t) |= JANET_TUPLE_FLAG_BRACKETCTOR;
  return janet_wrap_tuple(janet_tuple_end(t));
}

/*
 * A column of def as table-create takes it:
 * [name :type mods... :not-null :default "text" :on-update "text"].
 */
static Janet column_form(const sm_column_def_t &c) {
  std::vector<Janet> items = {janet_csymbolv(c.name.c_str()),
                              janet_ckeywordv(type_name_of(c.type_code))};
  for (int32_t m : mods_of(c.type_code, c.typmod)) {
    items.push_back(janet_wrap_integer(m));
  }
  if (c.not_null) {
    items.push_back(janet_ckeywordv("not-null"));
  }
  if (c.has_default) {
    items.push_back(janet_ckeywordv("default"));
    items.push_back(janet_cstringv(c.default_expr.c_str()));
  }
  if (c.has_on_update) {
    items.push_back(janet_ckeywordv("on-update"));
    items.push_back(janet_cstringv(c.on_update_expr.c_str()));
  }
  return tuple_of(items);
}

/* An index of def as a struct of index-create's clauses. */
static Janet index_form(const sm_table_def_t &def, const sm_index_def_t &x) {
  index_eval_t ev;
  parse_index_eval(x.eval, ev);
  std::vector<Janet> keys, includes;
  size_t e = 0;
  for (size_t j = 0; j < x.key.size(); j++) {
    // A key column, or (function column) for an expression.
    Janet what;
    if (x.key_attnums[j] < 0 && e < ev.exprs.size()) {
      what = tuple_of(
          {janet_csymbolv(ev.exprs[e].first.c_str()),
           janet_csymbolv(def.columns[ev.exprs[e].second].name.c_str())});
      e++;
    } else {
      what = janet_csymbolv(def.columns[x.key_attnums[j]].name.c_str());
    }
    keys.push_back(
        tuple_of({what, janet_ckeywordv(x.key[j].desc ? "desc" : "asc"),
                  janet_ckeywordv(x.key[j].nulls_first ? "nulls-first"
                                                       : "nulls-last")}));
  }
  for (int32_t att : x.include_attnums) {
    includes.push_back(janet_csymbolv(def.columns[att].name.c_str()));
  }
  JanetKV *st = janet_struct_begin(4 + ev.has_where + x.primary + x.tree);
  janet_struct_put(st, janet_ckeywordv("name"), janet_cstringv(x.name.c_str()));
  janet_struct_put(st, janet_ckeywordv("unique"), janet_wrap_boolean(x.unique));
  // The primary key alone says so: a secondary index's struct is as before.
  if (x.primary) {
    janet_struct_put(st, janet_ckeywordv("primary"), janet_wrap_true());
  }
  // A tree alone says so, as a primary key does.
  if (x.tree) {
    janet_struct_put(st, janet_ckeywordv("tree"), janet_wrap_true());
  }
  janet_struct_put(st, janet_ckeywordv("key"), tuple_of(keys));
  janet_struct_put(st, janet_ckeywordv("include"), tuple_of(includes));
  if (ev.has_where) {
    janet_struct_put(
        st, janet_ckeywordv("where"),
        tuple_of({janet_csymbolv(def.columns[ev.where_att].name.c_str()),
                  janet_cstringv(ev.where_text.c_str())}));
  }
  return janet_wrap_struct(janet_struct_end(st));
}

/*
 * (describe-table name [:include-indices]): the table's columns in
 * table-create's form, and with the flag its indexes, under its
 * table-oid lock IS.
 */
static Janet cfun_describe_table(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  bool with_indexes = false;
  if (argc == 2) {
    if (identifier_of(argv[1]) != "include-indices") {
      janet_panic("describe-table: the only flag is :include-indices");
    }
    with_indexes = true;
  }
  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCKMODE_IS, "describe-table");
  const sm_table_def_t &def = *p_tdef->def;

  std::vector<Janet> columns;
  for (const sm_column_def_t &c : def.columns) {
    if (!c.dropped) {
      columns.push_back(column_form(c));
    }
  }
  JanetKV *st = janet_struct_begin(with_indexes ? 3 : 2);
  janet_struct_put(st, janet_ckeywordv("name"),
                   janet_cstringv(def.name.c_str()));
  janet_struct_put(st, janet_ckeywordv("columns"), tuple_of(columns));
  if (with_indexes) {
    std::vector<Janet> indexes;
    for (const sm_index_def_t &x : def.indexes) {
      indexes.push_back(index_form(def, x));
    }
    janet_struct_put(st, janet_ckeywordv("indexes"), tuple_of(indexes));
  }
  return janet_wrap_struct(janet_struct_end(st));
}

/* (index-drop idx_name) */
static Janet cfun_index_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  uint64_t table = 0, index = 0;
  int rc = rel_index_oid(s, idx_name, &table, &index);
  if (rc == 1) {
    janet_panicf("index-drop: index '%s' not found", idx_name.c_str());
  }
  if (rc != 0 || sm_cat_drop_index(s->session, table, index) != 0) {
    panic_sm_error("index-drop");
  }
  return janet_wrap_nil();
}

/* The action fkey-create's :on-delete or :on-update names, as sm keeps it. */
static char fkey_action(const std::string &name) {
  if (name == "no-action")
    return 'a';
  if (name == "restrict")
    return 'r';
  if (name == "cascade")
    return 'c';
  if (name == "set-null")
    return 'n';
  if (name == "set-default")
    return 'd';
  janet_panicf("fkey-create: no action '%s'", name.c_str());
}

/* An action as fkeys prints it. */
static const char *fkey_action_name(char a) {
  switch (a) {
  case 'r':
    return "restrict";
  case 'c':
    return "cascade";
  case 'n':
    return "set-null";
  case 'd':
    return "set-default";
  default:
    return "no-action";
  }
}

/* The place of the live column name in def, for op; a panic if none. */
static int32_t column_place(const sm_table_def_t &def, const std::string &name,
                            const char *op) {
  for (size_t i = 0; i < def.columns.size(); i++) {
    if (!def.columns[i].dropped && def.columns[i].name == name) {
      return static_cast<int32_t>(i);
    }
  }
  janet_panicf("%s: column '%s' not found in table '%s'", op, name.c_str(),
               def.name.c_str());
}

/* The foreign key named name of the table def, for op; a panic if none. */
static const sm_fkey_def_t &need_fkey(const sm_table_def_t &def,
                                      const std::string &name, const char *op) {
  for (const sm_fkey_def_t &f : def.fkeys) {
    if (f.name == name) {
      return f;
    }
  }
  janet_panicf("%s: no foreign key '%s' of table '%s'", op, name.c_str(),
               def.name.c_str());
}

/*
 * (fkey-create name (table child) (columns c...) (references parent [p...])
 *  [:index idx] [:on-delete action] [:on-update action] [:match :full]
 *  [:not-valid true]): the child's foreign key to the parent
 * (FOREIGNKEY.md). The parent's columns are its primary key's if none are
 * named, and its index the unique one whose keys they are, unless :index
 * names one. sm takes both tables' locks X; it reads no row, so :not-valid
 * is only what the catalog records.
 */
static Janet cfun_fkey_create(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  sm_fkey_def_t def;
  def.name = identifier_of(argv[0]);
  std::string child, parent, index;
  std::vector<std::string> columns, ref_columns;

  const Janet *clauses = nullptr;
  int32_t n = 0;
  if (!janet_indexed_view(argv[1], &clauses, &n)) {
    janet_panic("fkey-create: its clauses are a list");
  }
  for (int32_t i = 0; i < n; i++) {
    if (janet_checktype(clauses[i], JANET_KEYWORD)) {
      std::string kw = identifier_of(clauses[i]);
      if (i + 1 >= n) {
        janet_panicf("fkey-create: :%s takes a value", kw.c_str());
      }
      Janet v = clauses[++i];
      if (kw == "index") {
        index = identifier_of(v);
      } else if (kw == "on-delete") {
        def.on_delete = fkey_action(identifier_of(v));
      } else if (kw == "on-update") {
        def.on_update = fkey_action(identifier_of(v));
      } else if (kw == "match") {
        def.match = identifier_of(v) == "full" ? 'f' : 's';
      } else if (kw == "not-valid") {
        def.validated = !janet_truthy(v);
      } else {
        janet_panicf("fkey-create: no option :%s", kw.c_str());
      }
      continue;
    }
    const Janet *items = nullptr;
    int32_t nitems = 0;
    if (!janet_indexed_view(clauses[i], &items, &nitems) || nitems < 1) {
      janet_panic("fkey-create: a clause is (table ...), (columns ...) or "
                  "(references ...)");
    }
    std::string head = identifier_of(items[0]);
    if (head == "table" && nitems == 2) {
      child = identifier_of(items[1]);
    } else if (head == "columns") {
      for (int32_t j = 1; j < nitems; j++) {
        columns.push_back(identifier_of(items[j]));
      }
    } else if (head == "references" && nitems >= 2) {
      parent = identifier_of(items[1]);
      for (int32_t j = 2; j < nitems; j++) {
        ref_columns.push_back(identifier_of(items[j]));
      }
    } else {
      janet_panicf("fkey-create: no clause (%s ...)", head.c_str());
    }
  }
  if (child.empty() || parent.empty()) {
    janet_panic("fkey-create: (table ...) and (references ...) are required");
  }

  // The definitions, read as sm keeps them; sm locks both tables itself.
  const sm_table_def_t &cdef =
      *need_table(s, child, SM_LOCKMODE_IS, "fkey-create")->def;
  const sm_table_def_t &pdef =
      *need_table(s, parent, SM_LOCKMODE_IS, "fkey-create")->def;
  for (const std::string &c : columns) {
    def.columns.push_back(column_place(cdef, c, "fkey-create"));
  }
  const sm_index_def_t *x = nullptr;
  if (ref_columns.empty()) {
    for (const sm_index_def_t &i : pdef.indexes) {
      x = i.primary ? &i : x;
    }
    if (x == nullptr) {
      janet_panicf("fkey-create: table '%s' has no primary key",
                   parent.c_str());
    }
    def.ref_columns = x->key_attnums;
  } else {
    for (const std::string &c : ref_columns) {
      def.ref_columns.push_back(column_place(pdef, c, "fkey-create"));
    }
  }
  // The index: the one named, or a unique one on the columns, any order.
  for (const sm_index_def_t &i : pdef.indexes) {
    if (!index.empty()) {
      x = i.name == index ? &i : x;
      continue;
    }
    bool fits = x == nullptr && i.unique &&
                i.key_attnums.size() == def.ref_columns.size();
    for (int32_t a : i.key_attnums) {
      fits = fits && std::find(def.ref_columns.begin(), def.ref_columns.end(),
                               a) != def.ref_columns.end();
    }
    x = fits ? &i : x;
  }
  if (x == nullptr) {
    janet_panicf("fkey-create: no index of '%s' for the referenced columns",
                 parent.c_str());
  }
  def.ref_table = pdef.oid;
  def.ref_index = x->oid;
  uint64_t oid = 0;
  if (sm_cat_create_fkey(s->session, cdef.oid, def, &oid) != 0) {
    panic_sm_error("fkey-create");
  }
  return janet_wrap_nil();
}

/* The foreign key name of the table tbl, with its table, for op. */
static std::pair<uint64_t, uint64_t> fkey_of(session_ctx_t *s,
                                             const std::string &tbl,
                                             const std::string &name,
                                             const char *op) {
  const sm_table_def_t &def = *need_table(s, tbl, SM_LOCKMODE_IS, op)->def;
  return {def.oid, need_fkey(def, name, op).oid};
}

/* (fkey-drop table name) */
static Janet cfun_fkey_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  auto [table, fkey] =
      fkey_of(s, identifier_of(argv[0]), identifier_of(argv[1]), "fkey-drop");
  if (sm_cat_drop_fkey(s->session, table, fkey) != 0) {
    panic_sm_error("fkey-drop");
  }
  return janet_wrap_nil();
}

/* (fkey-validate table name): its rows checked, as the host says. */
static Janet cfun_fkey_validate(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  auto [table, fkey] = fkey_of(s, identifier_of(argv[0]),
                               identifier_of(argv[1]), "fkey-validate");
  if (sm_cat_validate_fkey(s->session, table, fkey) != 0) {
    panic_sm_error("fkey-validate");
  }
  return janet_wrap_nil();
}

/* (fkey-rename table name new_name) */
static Janet cfun_fkey_rename(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  auto [table, fkey] =
      fkey_of(s, identifier_of(argv[0]), identifier_of(argv[1]), "fkey-rename");
  std::string name = identifier_of(argv[2]);
  if (sm_cat_rename_fkey(s->session, table, fkey, name.c_str()) != 0) {
    panic_sm_error("fkey-rename");
  }
  return janet_wrap_nil();
}

/* The name of the table oid, as the session sees it; "?" if none. */
static std::string table_name_of(session_ctx_t *s, uint64_t oid) {
  sm_table_def_t def;
  return sm_cat_table_def_oid(s->session, oid, def) == 0 ? def.name : "?";
}

/*
 * (fkeys table): the table's foreign keys, each a struct of its name,
 * columns, the table and columns it references, its actions, match type
 * and whether its rows were checked.
 */
static Janet cfun_fkeys(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const sm_table_def_t &def =
      *need_table(s, identifier_of(argv[0]), SM_LOCKMODE_IS, "fkeys")->def;
  std::vector<Janet> out;
  for (const sm_fkey_def_t &f : def.fkeys) {
    sm_table_def_t pdef;
    if (sm_cat_table_def_oid(s->session, f.ref_table, pdef) != 0) {
      panic_sm_error("fkeys");
    }
    std::vector<Janet> cols, refs;
    for (int32_t a : f.columns) {
      cols.push_back(janet_csymbolv(def.columns[a].name.c_str()));
    }
    for (int32_t a : f.ref_columns) {
      refs.push_back(janet_csymbolv(pdef.columns[a].name.c_str()));
    }
    JanetKV *st = janet_struct_begin(8);
    janet_struct_put(st, janet_ckeywordv("name"),
                     janet_cstringv(f.name.c_str()));
    janet_struct_put(st, janet_ckeywordv("columns"), tuple_of(cols));
    janet_struct_put(st, janet_ckeywordv("references"),
                     janet_cstringv(pdef.name.c_str()));
    janet_struct_put(st, janet_ckeywordv("ref-columns"), tuple_of(refs));
    janet_struct_put(st, janet_ckeywordv("on-delete"),
                     janet_ckeywordv(fkey_action_name(f.on_delete)));
    janet_struct_put(st, janet_ckeywordv("on-update"),
                     janet_ckeywordv(fkey_action_name(f.on_update)));
    janet_struct_put(st, janet_ckeywordv("match"),
                     janet_ckeywordv(f.match == 'f' ? "full" : "simple"));
    janet_struct_put(st, janet_ckeywordv("validated"),
                     janet_wrap_boolean(f.validated));
    out.push_back(janet_wrap_struct(janet_struct_end(st)));
  }
  return tuple_of(out);
}

/* The check constraint named name of the table def, for op; a panic if none. */
static const sm_check_def_t &
need_check(const sm_table_def_t &def, const std::string &name, const char *op) {
  for (const sm_check_def_t &c : def.checks) {
    if (c.name == name) {
      return c;
    }
  }
  janet_panicf("%s: no check '%s' of table '%s'", op, name.c_str(),
               def.name.c_str());
}

/* The check name of the table tbl, with its table, for op. */
static std::pair<uint64_t, uint64_t> check_oid_of(session_ctx_t *s,
                                                  const std::string &tbl,
                                                  const std::string &name,
                                                  const char *op) {
  const sm_table_def_t &def = *need_table(s, tbl, SM_LOCKMODE_IS, op)->def;
  return {def.oid, need_check(def, name, op).oid};
}

/*
 * (check-create table name [column...] [:not-valid]): a check
 * constraint reading the columns, with no expression: sm keeps the
 * host's, which rel has none of (CHECK.md).
 */
static Janet cfun_check_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  session_ctx_t *s = require_session();
  const sm_table_def_t &def =
      *need_table(s, identifier_of(argv[0]), SM_LOCKMODE_IS, "check-create")
           ->def;
  sm_check_def_t c;
  c.name = identifier_of(argv[1]);
  JanetView cols = janet_getindexed(argv, 2);
  for (int32_t i = 0; i < cols.len; i++) {
    c.columns.push_back(
        column_place(def, identifier_of(cols.items[i]), "check-create"));
  }
  if (4 == argc) {
    if (!janet_keyeq(argv[3], "not-valid")) {
      janet_panic("check-create: the one option is :not-valid");
    }
    c.validated = false;
  }
  uint64_t oid = 0;
  if (sm_cat_create_check(s->session, def.oid, c, &oid) != 0) {
    panic_sm_error("check-create");
  }
  return janet_wrap_nil();
}

/* (check-drop table name) */
static Janet cfun_check_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  auto [table, check] = check_oid_of(s, identifier_of(argv[0]),
                                     identifier_of(argv[1]), "check-drop");
  if (sm_cat_drop_check(s->session, table, check) != 0) {
    panic_sm_error("check-drop");
  }
  return janet_wrap_nil();
}

/* (check-validate table name): its rows checked, as the host says. */
static Janet cfun_check_validate(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  auto [table, check] = check_oid_of(s, identifier_of(argv[0]),
                                     identifier_of(argv[1]), "check-validate");
  if (sm_cat_validate_check(s->session, table, check) != 0) {
    panic_sm_error("check-validate");
  }
  return janet_wrap_nil();
}

/* (check-rename table name new_name) */
static Janet cfun_check_rename(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  auto [table, check] = check_oid_of(s, identifier_of(argv[0]),
                                     identifier_of(argv[1]), "check-rename");
  std::string name = identifier_of(argv[2]);
  if (sm_cat_rename_check(s->session, table, check, name.c_str()) != 0) {
    panic_sm_error("check-rename");
  }
  return janet_wrap_nil();
}

/*
 * (checks table): the table's check constraints, each a struct of its
 * name, the columns it reads, and whether its rows were checked.
 */
static Janet cfun_checks(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const sm_table_def_t &def =
      *need_table(s, identifier_of(argv[0]), SM_LOCKMODE_IS, "checks")->def;
  std::vector<Janet> out;
  for (const sm_check_def_t &c : def.checks) {
    std::vector<Janet> cols;
    for (int32_t a : c.columns) {
      cols.push_back(janet_csymbolv(def.columns[a].name.c_str()));
    }
    JanetKV *st = janet_struct_begin(3);
    janet_struct_put(st, janet_ckeywordv("name"),
                     janet_cstringv(c.name.c_str()));
    janet_struct_put(st, janet_ckeywordv("columns"), tuple_of(cols));
    janet_struct_put(st, janet_ckeywordv("validated"),
                     janet_wrap_boolean(c.validated));
    out.push_back(janet_wrap_struct(janet_struct_end(st)));
  }
  return tuple_of(out);
}

/*
 * (referenced-by table): the foreign keys that reference the table, its
 * own among them, each [child-table fkey-name], sorted.
 */
static Janet cfun_referenced_by(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  const sm_table_def_t &def =
      *need_table(s, identifier_of(argv[0]), SM_LOCKMODE_IS, "referenced-by")
           ->def;
  std::vector<std::pair<std::string, std::string>> refs;
  for (const sm_fkey_ref_t &r : def.referenced_by) {
    uint64_t table = 0;
    sm_fkey_def_t f;
    if (sm_cat_fkey(s->session, r.fkey, &table, &f) != 0) {
      panic_sm_error("referenced-by");
    }
    refs.push_back({table_name_of(s, table), f.name});
  }
  std::sort(refs.begin(), refs.end());
  std::vector<Janet> out;
  for (const auto &[t, f] : refs) {
    out.push_back(
        tuple_of({janet_cstringv(t.c_str()), janet_cstringv(f.c_str())}));
  }
  return tuple_of(out);
}

/* (index-wait idx_name) */
static Janet cfun_index_wait(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  uint64_t table = 0, index = 0;
  int rc = rel_index_oid(s, idx_name, &table, &index);
  if (rc == 1) {
    janet_panicf("index-wait: index '%s' not found", idx_name.c_str());
  }
  // Blocks until the background build of the index has ended.
  if (rc != 0 || sm_cat_index_wait(s->session, table, index) != 0) {
    panic_sm_error("index-wait");
  }
  return janet_wrap_nil();
}

/* (tables-parked): the handles the session keeps for its next opens. */
static Janet cfun_tables_parked(int32_t argc, Janet *argv) {
  (void)argv;
  janet_fixarity(argc, 0);
  session_ctx_t *s = require_session();
  return janet_wrap_number(static_cast<double>(sm_tbl_parked(s->session)));
}

/* (table-version table): the version in the table's definition. */
static Janet cfun_table_version(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCKMODE_IS, "table-version");
  return janet_wrap_number(static_cast<double>(p_tdef->def->version));
}

/* (index-state idx_name) */
static Janet cfun_index_state(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  auto [p_tdef, p_idef] =
      need_index(s, idx_name, SM_LOCKMODE_IS, "index-state");
  (void)p_tdef;
  switch (p_idef->def->state) {
  case SM_INDEX_PENDING:
    return janet_ckeywordv("pending");
  case SM_INDEX_BUILDING:
    return janet_ckeywordv("building");
  case SM_INDEX_VALID:
    return janet_ckeywordv("valid");
  default:
    return janet_ckeywordv("failed");
  }
}

/* The table's primary key, or nullptr in a table without one. */
static const index_def_t *primary_of(const table_def_t *t) {
  for (const auto &[name, x] : t->indexes) {
    if (x.def->primary) {
      return &x;
    }
  }
  return nullptr;
}

/*
 * The values of the first of the index's key columns that v gives,
 * a tuple or array of them or one value alone, into vals: a scan's
 * bound, or a row's key; op names the command in a panic.
 */
static void key_values_of(const table_def_t *p_tdef, const index_def_t *p_idef,
                          Janet v, row_values_t &vals, const char *op) {
  const Janet *items = nullptr;
  int32_t n = 0;
  if (!janet_indexed_view(v, &items, &n)) {
    items = &v;
    n = 1;
  }
  if (static_cast<size_t>(n) > p_idef->keys.size()) {
    janet_panicf("%s: a bound of %d values on %d key columns", op, n,
                 static_cast<int>(p_idef->keys.size()));
  }
  vals = row_values_t(n);
  for (int32_t j = 0; j < n; j++) {
    const key_col_def_t &k = p_idef->keys[j];
    col_def_t col = {p_tdef->columns[k.col_idx].name, k.type_code, k.typmod,
                     false, std::nullopt};
    if (janet_to_value(items[j], col, vals.bufs[j], &vals.values[j],
                       &vals.isnull[j]) != 0) {
      janet_panicf("%s: invalid bound value for column '%s'", op,
                   col.name.c_str());
    }
  }
}

/*
 * A row's name, as a verb is given it: in a table without a primary
 * key its _id; in one with, its key's values, in vals, which key
 * points at (../ROWSTORE.md section 2.1).
 */
struct row_name_t {
  const index_def_t *pk = nullptr;
  uint64_t id = 0;
  row_values_t vals{0};
  sm_row_key_t key = {nullptr, nullptr, 0};
};

/* The row argv[n] names in the table, into name; op names the command. */
static void row_name_of(const table_def_t *p_tdef, Janet *argv, int32_t n,
                        row_name_t &name, const char *op) {
  name.pk = primary_of(p_tdef);
  if (name.pk == nullptr) {
    name.id = static_cast<uint64_t>(janet_getinteger64(argv, n));
    return;
  }
  key_values_of(p_tdef, name.pk, argv[n], name.vals, op);
  if (name.vals.values.size() != name.pk->keys.size()) {
    janet_panicf("%s: a key of %d values on a primary key of %d columns", op,
                 static_cast<int>(name.vals.values.size()),
                 static_cast<int>(name.pk->keys.size()));
  }
  name.key = {name.vals.values.data(), name.vals.isnull.get(),
              name.vals.values.size()};
}

/*
 * The key of a row of the table, whose primary key is pk, as a verb
 * gives it back: a tuple of the values of pk's columns in vals, each
 * a Janet value already, in the row's column order.
 */
static Janet key_form(const index_def_t *pk, const Janet *vals) {
  std::vector<Janet> items;
  for (const key_col_def_t &k : pk->keys) {
    items.push_back(vals[k.col_idx]);
  }
  return tuple_of(items);
}

/*
 * What insert and load give back for a row of row's values: its key
 * in a table with a primary key, else its _id, rowno.
 */
static Janet inserted_form(const table_def_t *p_tdef, const row_values_t &row,
                           uint64_t rowno) {
  const index_def_t *pk = primary_of(p_tdef);
  if (pk == nullptr) {
    return janet_wrap_number(static_cast<double>(rowno));
  }
  std::vector<Janet> vals(row.values.size(), janet_wrap_nil());
  for (const key_col_def_t &k : pk->keys) {
    vals[k.col_idx] = value_to_janet(
        row.values[k.col_idx], row.isnull[k.col_idx], k.type_code, k.typmod);
  }
  return key_form(pk, vals.data());
}

/* (insert table [val1 val2 ...]) */
static Janet cfun_insert(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IX, "insert");
  sm_tbl_t *tbl = p_tdef->tbl;

  const Janet *row_items = nullptr;
  int32_t row_len = 0;
  if (janet_checktype(argv[1], JANET_TUPLE)) {
    row_items = janet_unwrap_tuple(argv[1]);
    row_len = janet_tuple_length(row_items);
  } else if (janet_checktype(argv[1], JANET_ARRAY)) {
    row_items = janet_unwrap_array(argv[1])->data;
    row_len = janet_unwrap_array(argv[1])->count;
  } else {
    janet_panic("insert: second argument must be a tuple or array");
  }

  if (static_cast<size_t>(row_len) != p_tdef->columns.size()) {
    janet_panicf("insert: expected %d values, got %d",
                 static_cast<int>(p_tdef->columns.size()), row_len);
  }

  row_values_t row(row_len);
  for (int32_t i = 0; i < row_len; i++) {
    if (janet_to_value(row_items[i], p_tdef->columns[i], row.bufs[i],
                       &row.values[i], &row.isnull[i]) != 0) {
      janet_panicf("insert: invalid value for column '%s' (type %s)",
                   p_tdef->columns[i].name.c_str(),
                   type_name_of(p_tdef->columns[i].type_code));
    }
  }

  // The heap row and its entry in every index, in one call.
  uint64_t rowno = 0;
  if (sm_tbl_insert(tbl, row.values.data(), row.isnull.get(), row.values.size(),
                    &rowno) != 0) {
    abort_failed_write(s);
    panic_sm_error("insert sm_tbl_insert");
  }

  return inserted_form(p_tdef, row, rowno);
}

/* (read table id), or (read table key) in a table with a primary key */
static Janet cfun_read(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "read");
  sm_tbl_t *tbl = p_tdef->tbl;
  row_name_t name;
  row_name_of(p_tdef, argv, 1, name, "read");

  const void *tuple = nullptr;
  size_t len = 0;
  int rc = name.pk ? sm_tbl_read(tbl, name.key, &tuple, &len)
                   : sm_tbl_read(tbl, name.id, &tuple, &len);
  if (rc == 1) {
    return janet_wrap_nil();
  }
  if (rc != 0) {
    panic_sm_error("read sm_tbl_read");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t out(ncols);
  if (sm_tbl_deform(tbl, tuple, len, out.values.data(), out.isnull.get(),
                    ncols) != 0) {
    panic_sm_error("read sm_tbl_deform");
  }

  Janet *vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    vals[i] = value_to_janet(out.values[i], out.isnull[i],
                             p_tdef->columns[i].type_code,
                             p_tdef->columns[i].typmod, tbl);
  }
  return janet_wrap_tuple(janet_tuple_end(vals));
}

/*
 * (read-keys table [key ...]) in a table with a primary key: each
 * key's row as (read) gives it, nil for none, read in one
 * sm_tbl_read_keys (../ROWSTORE.md).
 */
static Janet cfun_read_keys(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "read-keys");
  sm_tbl_t *tbl = p_tdef->tbl;
  if (primary_of(p_tdef) == nullptr) {
    janet_panicf("read-keys: %s has no primary key", tbl_name.c_str());
  }
  JanetView keys = janet_getindexed(argv, 1);
  size_t n = static_cast<size_t>(keys.len);
  // Each key encoded as an index entry names its row.
  std::vector<std::string> enc(n);
  for (size_t i = 0; i < n; i++) {
    row_name_t name;
    Janet k = keys.items[i];
    row_name_of(p_tdef, &k, 0, name, "read-keys");
    if (sm_tbl_row_key(tbl, name.key, enc[i]) != 0) {
      panic_sm_error("read-keys sm_tbl_row_key");
    }
  }
  std::vector<const void *> kp(n);
  std::vector<size_t> kl(n);
  for (size_t i = 0; i < n; i++) {
    kp[i] = enc[i].data();
    kl[i] = enc[i].size();
  }
  std::vector<sm_pin_ref_t> refs(n, sm_pin_ref_t{});
  std::vector<const void *> tuples(n);
  std::vector<size_t> lens(n);
  std::vector<int> rc(n);
  if (sm_tbl_read_keys(tbl, kp.data(), kl.data(), n, refs.data(), tuples.data(),
                       lens.data(), rc.data()) != 0) {
    panic_sm_error("read-keys sm_tbl_read_keys");
  }

  size_t ncols = p_tdef->columns.size();
  JanetArray *out = janet_array(static_cast<int32_t>(n));
  for (size_t i = 0; i < n; i++) {
    if (rc[i] == 1) {
      janet_array_push(out, janet_wrap_nil());
      continue;
    }
    row_values_t row(ncols);
    if (sm_tbl_deform(tbl, tuples[i], lens[i], row.values.data(),
                      row.isnull.get(), ncols) != 0) {
      panic_sm_error("read-keys sm_tbl_deform");
    }
    Janet *vals = janet_tuple_begin(static_cast<int32_t>(ncols));
    for (size_t c = 0; c < ncols; c++) {
      vals[c] = value_to_janet(row.values[c], row.isnull[c],
                               p_tdef->columns[c].type_code,
                               p_tdef->columns[c].typmod, tbl);
    }
    janet_array_push(out, janet_wrap_tuple(janet_tuple_end(vals)));
    // The row is copied out: its pin is no one's now.
    sm_unpin(&refs[i]);
  }
  return janet_wrap_array(out);
}

/* (read-latest table id), or by key as (read) */
static Janet cfun_read_latest(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "read-latest");
  sm_tbl_t *tbl = p_tdef->tbl;
  row_name_t name;
  row_name_of(p_tdef, argv, 1, name, "read-latest");

  const void *tuple = nullptr;
  size_t len = 0;
  int rc = name.pk ? sm_tbl_read_latest(tbl, name.key, &tuple, &len)
                   : sm_tbl_read_latest(tbl, name.id, &tuple, &len);
  if (rc == 1) {
    return janet_wrap_nil();
  }
  if (rc != 0) {
    panic_sm_error("read-latest sm_tbl_read_latest");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t out(ncols);
  if (sm_tbl_deform(tbl, tuple, len, out.values.data(), out.isnull.get(),
                    ncols) != 0) {
    panic_sm_error("read-latest sm_tbl_deform");
  }

  Janet *vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    vals[i] = value_to_janet(out.values[i], out.isnull[i],
                             p_tdef->columns[i].type_code,
                             p_tdef->columns[i].typmod, tbl);
  }
  return janet_wrap_tuple(janet_tuple_end(vals));
}

/* (lock-row table id :shared|:exclusive), or by key as (read) */
static Janet cfun_lock_row(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  std::string how = identifier_of(argv[2]);
  if (how != "shared" && how != "exclusive") {
    janet_panic("lock-row: the mode is :shared or :exclusive");
  }
  bool exclusive = how == "exclusive";

  // The table lock under the row's: IS for shared, IX for exclusive.
  table_def_t *p_tdef = need_table(
      s, tbl_name, exclusive ? SM_LOCKMODE_IX : SM_LOCKMODE_IS, "lock-row");
  row_name_t name;
  row_name_of(p_tdef, argv, 1, name, "lock-row");
  sm_lockmode_t mode = exclusive ? SM_LOCKMODE_EX : SM_LOCKMODE_SH;
  if ((name.pk ? sm_tbl_lock(p_tdef->tbl, name.key, mode)
               : sm_tbl_lock(p_tdef->tbl, name.id, mode)) != 0) {
    panic_sm_error("lock-row sm_tbl_lock");
  }
  return janet_wrap_nil();
}

/* (truncate table) */
static Janet cfun_truncate(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  // The table lock exclusive, as TRUNCATE takes it.
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_EX, "truncate");
  // The handle's definition, given out already: a truncate must leave
  // it, and all it holds, where its holders point (sm.h).
  const sm_table_def_t *def = sm_tbl_def(p_tdef->tbl);
  if (!def) {
    panic_sm_error("truncate sm_tbl_def");
  }
  const sm_column_def_t *columns = def->columns.data();
  if (sm_tbl_truncate(p_tdef->tbl) != 0) {
    panic_sm_error("truncate sm_tbl_truncate");
  }
  if (sm_tbl_def(p_tdef->tbl) != def || def->columns.data() != columns) {
    janet_panic("truncate: the table's definition moved");
  }
  if (def->version != sm_tbl_version(p_tdef->tbl)) {
    janet_panic("truncate: the table's definition has an old version");
  }
  return janet_wrap_nil();
}

/*
 * (load table rows): a bulk load of rows, an array of rows, into the
 * table (sm_tbl_load_begin, _add, _end): into an empty table, as
 * sorted files or one batch, under the table's lock exclusive; into
 * one that has rows, as inserts. Returns the rows' numbers.
 */
static Janet cfun_load(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IX, "load");
  sm_tbl_t *tbl = p_tdef->tbl;
  JanetView rows = janet_getindexed(argv, 1);
  if (sm_tbl_load_begin(tbl) != 0) {
    abort_failed_write(s);
    panic_sm_error("load sm_tbl_load_begin");
  }
  JanetArray *out = janet_array(rows.len);
  for (int32_t r = 0; r < rows.len; r++) {
    JanetView items;
    if (!janet_indexed_view(rows.items[r], &items.items, &items.len) ||
        static_cast<size_t>(items.len) != p_tdef->columns.size()) {
      janet_panicf("load: row %d must hold %d values", r,
                   static_cast<int>(p_tdef->columns.size()));
    }
    row_values_t row(items.len);
    for (int32_t i = 0; i < items.len; i++) {
      if (janet_to_value(items.items[i], p_tdef->columns[i], row.bufs[i],
                         &row.values[i], &row.isnull[i]) != 0) {
        janet_panicf("load: invalid value for column '%s' (type %s)",
                     p_tdef->columns[i].name.c_str(),
                     type_name_of(p_tdef->columns[i].type_code));
      }
    }
    uint64_t rowno = 0;
    if (sm_tbl_load_add(tbl, row.values.data(), row.isnull.get(),
                        row.values.size(), &rowno) != 0) {
      abort_failed_write(s);
      panic_sm_error("load sm_tbl_load_add");
    }
    janet_array_push(out, inserted_form(p_tdef, row, rowno));
  }
  // Every row handed over goes into the table.
  if (sm_tbl_load_end(tbl) != 0) {
    abort_failed_write(s);
    panic_sm_error("load sm_tbl_load_end");
  }
  return janet_wrap_array(out);
}

/*
 * (compact table): compact the table's store through GC, as VACUUM
 * (sm_tbl_compact): what no reader needs is dropped, and a version
 * an aborted writer left is queued for GC to put back.
 */
static Janet cfun_compact(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  // A reader's lock, as VACUUM takes it: writers go on meanwhile.
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "compact");
  if (sm_tbl_compact(p_tdef->tbl) != 0) {
    panic_sm_error("compact sm_tbl_compact");
  }
  return janet_wrap_nil();
}

/*
 * (cleaner-pass): one pass of the btree cleaner of the session's
 * database (sm_cleaner_run), on this thread. A script gets no cleaner
 * thread.
 */
static Janet cfun_cleaner_pass(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 0);
  (void)argv;
  session_ctx_t *s = require_session();
  if (sm_cleaner_run(get_catalog().env(), s->db_name.c_str()) != 0) {
    panic_sm_error("cleaner-pass sm_cleaner_run");
  }
  return janet_wrap_nil();
}

/*
 * (table-size table): the bytes of the table's rows, older versions
 * included, in sorted files and in memtables, summed (sm_tbl_size).
 */
static Janet cfun_table_size(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "table-size");
  uint64_t disk = 0, mem = 0;
  if (sm_tbl_size(p_tdef->tbl, &disk, &mem) != 0) {
    panic_sm_error("table-size sm_tbl_size");
  }
  return janet_wrap_number(static_cast<double>(disk + mem));
}

/* (analyze table [target]): analyze the table, target 100 if none. */
static Janet cfun_analyze(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  int32_t target = argc == 2 ? janet_getinteger(argv, 1) : 100;
  // A reader's lock, as ANALYZE takes it: writers go on meanwhile.
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "analyze");
  if (sm_tbl_analyze(p_tdef->tbl, target) != 0) {
    panic_sm_error("analyze sm_tbl_analyze");
  }
  return janet_wrap_nil();
}

/* A statistic as a Janet struct, its values of type code and typmod. */
static Janet stat_form(const sm_stat_t &st, uint8_t code, int32_t typmod) {
  const sm_type_t *type = sm_type(code);
  auto values = [&](const std::vector<std::string> &bytes) {
    std::vector<Janet> out;
    for (const std::string &b : bytes) {
      out.push_back(
          value_to_janet(sm_datum_of_bytes(type, b), false, code, typmod));
    }
    return tuple_of(out);
  };
  std::vector<Janet> freqs;
  for (float f : st.mcv_freq) {
    freqs.push_back(janet_wrap_number(f));
  }
  JanetKV *kv = janet_struct_begin(7);
  janet_struct_put(kv, janet_ckeywordv("null-frac"),
                   janet_wrap_number(st.null_frac));
  janet_struct_put(kv, janet_ckeywordv("width"), janet_wrap_integer(st.width));
  janet_struct_put(kv, janet_ckeywordv("distinct"),
                   janet_wrap_number(st.distinct));
  janet_struct_put(kv, janet_ckeywordv("correlation"),
                   st.has_correlation ? janet_wrap_number(st.correlation)
                                      : janet_wrap_nil());
  janet_struct_put(kv, janet_ckeywordv("mcv"), values(st.mcv));
  janet_struct_put(kv, janet_ckeywordv("freqs"), tuple_of(freqs));
  janet_struct_put(kv, janet_ckeywordv("bounds"), values(st.bounds));
  return janet_wrap_struct(janet_struct_end(kv));
}

/*
 * (stats table): the table's statistics, nil if never analyzed: its
 * counts and sizes, each column's by name, and each index's by name
 * with its expressions' by place.
 */
static Janet cfun_stats(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, "stats");
  const sm_table_def_t &def = *p_tdef->def;
  std::shared_ptr<const sm_stats_t> held;
  int rc = sm_tbl_stats(s->session, def.oid, held);
  if (rc == 1) {
    return janet_wrap_nil();
  }
  if (rc != 0) {
    panic_sm_error("stats sm_tbl_stats");
  }
  const sm_stats_t &st = *held;
  JanetTable *columns = janet_table(0);
  JanetTable *indexes = janet_table(0);
  for (const sm_stats_index_t &x : st.indexes) {
    for (const sm_index_def_t &d : def.indexes) {
      if (d.oid != x.oid) {
        continue;
      }
      // Each expression's statistics, by its place among the keys.
      JanetTable *exprs = janet_table(0);
      const index_def_t &ri = p_tdef->indexes.at(d.name);
      for (const sm_stat_t &e : st.stats) {
        if (e.has_index && e.index == x.oid) {
          const key_col_def_t &k = ri.keys.at(e.position);
          janet_table_put(exprs, janet_wrap_integer(e.position),
                          stat_form(e, k.type_code, k.typmod));
        }
      }
      JanetKV *kv = janet_struct_begin(5);
      janet_struct_put(kv, janet_ckeywordv("belongs"),
                       janet_wrap_number(x.belongs));
      janet_struct_put(kv, janet_ckeywordv("entries"),
                       janet_wrap_number(x.entries));
      janet_struct_put(kv, janet_ckeywordv("disk"),
                       janet_wrap_number(static_cast<double>(x.disk)));
      janet_struct_put(kv, janet_ckeywordv("mem"),
                       janet_wrap_number(static_cast<double>(x.mem)));
      janet_struct_put(kv, janet_ckeywordv("exprs"), janet_wrap_table(exprs));
      janet_table_put(indexes, janet_cstringv(d.name.c_str()),
                      janet_wrap_struct(janet_struct_end(kv)));
    }
  }
  for (const sm_stat_t &c : st.stats) {
    if (!c.has_index) {
      const sm_column_def_t &col = def.columns.at(c.position);
      janet_table_put(columns, janet_cstringv(col.name.c_str()),
                      stat_form(c, col.type_code, col.typmod));
    }
  }
  JanetKV *kv = janet_struct_begin(10);
  janet_struct_put(kv, janet_ckeywordv("target"),
                   janet_wrap_integer(st.target));
  janet_struct_put(kv, janet_ckeywordv("live-rows"),
                   janet_wrap_number(st.live_rows));
  janet_struct_put(kv, janet_ckeywordv("rows-sampled"),
                   janet_wrap_number(static_cast<double>(st.rows_sampled)));
  janet_struct_put(kv, janet_ckeywordv("rowno-limit"),
                   janet_wrap_number(static_cast<double>(st.rowno_limit)));
  janet_struct_put(kv, janet_ckeywordv("run-length"),
                   janet_wrap_integer(st.run_length));
  janet_struct_put(kv, janet_ckeywordv("avg-row-bytes"),
                   janet_wrap_integer(st.avg_row_bytes));
  janet_struct_put(kv, janet_ckeywordv("heap-disk"),
                   janet_wrap_number(static_cast<double>(st.heap_disk)));
  janet_struct_put(kv, janet_ckeywordv("heap-mem"),
                   janet_wrap_number(static_cast<double>(st.heap_mem)));
  janet_struct_put(kv, janet_ckeywordv("columns"), janet_wrap_table(columns));
  janet_struct_put(kv, janet_ckeywordv("indexes"), janet_wrap_table(indexes));
  return janet_wrap_struct(janet_struct_end(kv));
}

/*
 * (update table id [new_val1 new_val2 ...]), or by key as (read); new
 * values of a primary key's columns move the row to the new key.
 */
static Janet cfun_update(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IX, "update");
  sm_tbl_t *tbl = p_tdef->tbl;
  row_name_t name;
  row_name_of(p_tdef, argv, 1, name, "update");

  const Janet *row_items = nullptr;
  int32_t row_len = 0;
  if (janet_checktype(argv[2], JANET_TUPLE)) {
    row_items = janet_unwrap_tuple(argv[2]);
    row_len = janet_tuple_length(row_items);
  } else if (janet_checktype(argv[2], JANET_ARRAY)) {
    row_items = janet_unwrap_array(argv[2])->data;
    row_len = janet_unwrap_array(argv[2])->count;
  } else {
    janet_panic("update: third argument must be a tuple or array");
  }

  if (static_cast<size_t>(row_len) != p_tdef->columns.size()) {
    janet_panicf("update: expected %d values, got %d",
                 static_cast<int>(p_tdef->columns.size()), row_len);
  }

  row_values_t row(row_len);
  for (int32_t i = 0; i < row_len; i++) {
    if (janet_to_value(row_items[i], p_tdef->columns[i], row.bufs[i],
                       &row.values[i], &row.isnull[i]) != 0) {
      janet_panicf("update: invalid value for column '%s'",
                   p_tdef->columns[i].name.c_str());
    }
  }

  // The heap write and every index it changes, in one call; the
  // row replaced pinned until its values are Janet's.
  sm_pin_ref_t ref = {};
  const void *old_tuple = nullptr;
  size_t old_len = 0;
  if (0 !=
      (name.pk
           ? sm_tbl_update(tbl, name.key, row.values.data(), row.isnull.get(),
                           row.values.size(), &ref, &old_tuple, &old_len)
           : sm_tbl_update(tbl, name.id, row.values.data(), row.isnull.get(),
                           row.values.size(), &ref, &old_tuple, &old_len))) {
    abort_failed_write(s);
    panic_sm_error("update sm_tbl_update");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t old(ncols);
  if (sm_tbl_deform(tbl, old_tuple, old_len, old.values.data(),
                    old.isnull.get(), ncols) != 0) {
    panic_sm_error("update sm_tbl_deform");
  }
  Janet *old_vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    old_vals[i] =
        value_to_janet(old.values[i], old.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  sm_unpin(&ref);
  return janet_wrap_tuple(janet_tuple_end(old_vals));
}

/* (delete table id), or by key as (read) */
static Janet cfun_delete(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IX, "delete");
  sm_tbl_t *tbl = p_tdef->tbl;
  row_name_t name;
  row_name_of(p_tdef, argv, 1, name, "delete");

  // The row deleted, pinned until its values are Janet's.
  sm_pin_ref_t ref = {};
  const void *old_tuple = nullptr;
  size_t old_len = 0;
  if (0 != (name.pk
                ? sm_tbl_delete(tbl, name.key, &ref, &old_tuple, &old_len)
                : sm_tbl_delete(tbl, name.id, &ref, &old_tuple, &old_len))) {
    abort_failed_write(s);
    panic_sm_error("delete sm_tbl_delete");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t old(ncols);
  if (sm_tbl_deform(tbl, old_tuple, old_len, old.values.data(),
                    old.isnull.get(), ncols) != 0) {
    panic_sm_error("delete sm_tbl_deform");
  }
  Janet *old_vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    old_vals[i] =
        value_to_janet(old.values[i], old.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  sm_unpin(&ref);
  return janet_wrap_tuple(janet_tuple_end(old_vals));
}

/*
 * Open a cursor over the table argv[0] names, over the _ids the rest of
 * argv gives, as (scan) takes them, or in a table with a primary key
 * over its keys; op names the command in a panic. Sets p_tdef to the
 * table.
 */
static sm_rel_scan_t *open_rel_cursor(session_ctx_t *s, int32_t argc,
                                      Janet *argv, const char *op,
                                      table_def_t *&p_tdef) {
  std::string tbl_name = identifier_of(argv[0]);
  p_tdef = need_table(s, tbl_name, SM_LOCKMODE_IS, op);
  const index_def_t *pk = primary_of(p_tdef);

  uint64_t first = 1;
  uint64_t last = UINT64_MAX;
  // A keyed table's bounds: its key's values, or the first of them.
  row_values_t lo_vals(0), hi_vals(0);
  sm_bound_t lo_b = {nullptr, nullptr, 0, true};
  sm_bound_t hi_b = {nullptr, nullptr, 0, true};
  bool has_lo = false;
  bool has_hi = false;

  for (int32_t i = 1; i < argc; i++) {
    if (janet_checktype(argv[i], JANET_KEYWORD)) {
      std::string kw = identifier_of(argv[i]);
      bool from = kw == "from" || kw == ":from";
      if ((from || kw == "to" || kw == ":to") && i + 1 < argc) {
        i++;
        if (pk == nullptr) {
          (from ? first : last) =
              static_cast<uint64_t>(janet_getinteger64(argv, i));
          continue;
        }
        row_values_t &vals = from ? lo_vals : hi_vals;
        key_values_of(p_tdef, pk, argv[i], vals, op);
        sm_bound_t &b = from ? lo_b : hi_b;
        b = sm_bound_t{vals.values.data(), vals.isnull.get(),
                       vals.values.size(), true};
        (from ? has_lo : has_hi) = true;
      }
    }
  }

  sm_rel_scan_t *rscan = nullptr;
  if (pk == nullptr) {
    if (sm_tbl_scan(p_tdef->tbl, first, last, &rscan) != 0) {
      panic_sm_error((std::string(op) + " sm_tbl_scan").c_str());
    }
    return rscan;
  }
  // The rows themselves over the key's range, as digres scans its
  // primary key (../CLUSTER.md section 6).
  if (sm_tbl_idx_scan_rows(p_tdef->tbl, pk->def->oid, has_lo ? &lo_b : nullptr,
                           has_hi ? &hi_b : nullptr, nullptr, &rscan) != 0) {
    panic_sm_error((std::string(op) + " sm_tbl_idx_scan_rows").c_str());
  }
  return rscan;
}

/*
 * (scan table [:from first] [:to last]): [id row] for each row, or
 * [key row] in a table with a primary key.
 */
static Janet cfun_scan(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  sm_rel_scan_t *rscan = open_rel_cursor(s, argc, argv, "scan", p_tdef);
  sm_tbl_t *tbl = p_tdef->tbl;
  const index_def_t *pk = primary_of(p_tdef);

  JanetArray *rows = janet_array(16);
  uint64_t rowno = 0;
  const void *tuple = nullptr;
  size_t len = 0;
  size_t ncols = p_tdef->columns.size();

  int rc;
  while ((rc = sm_rel_scannxt(rscan, &rowno, &tuple, &len)) == 0) {
    Janet *vals = janet_tuple_begin(ncols);
    for (size_t i = 0; i < ncols; i++) {
      sm_datum_t val = 0;
      bool isnull = false;
      if (sm_tbl_decode(tbl, tuple, len, i, &val, &isnull) != 0) {
        sm_rel_scanend(rscan);
        panic_sm_error("scan sm_tbl_decode");
      }
      vals[i] = value_to_janet(val, isnull, p_tdef->columns[i].type_code,
                               p_tdef->columns[i].typmod, tbl);
    }
    Janet *pair = janet_tuple_begin(2);
    pair[0] =
        pk ? key_form(pk, vals) : janet_wrap_number(static_cast<double>(rowno));
    pair[1] = janet_wrap_tuple(janet_tuple_end(vals));
    janet_array_push(rows, janet_wrap_tuple(janet_tuple_end(pair)));
  }

  // A cursor that failed, not one at its end: what it gave is not
  // the scan's rows. One the engine's abort closed is not ended.
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_rel_scanend(rscan);
    }
    panic_sm_error("scan sm_rel_scannxt");
  }
  sm_rel_scanend(rscan);
  return janet_wrap_array(rows);
}

/*
 * (scan-count table [:from first] [:to last]): the rows (scan) gives,
 * counted, none decoded. Each is stepped to in the cursor's pin, as
 * digres's scans step (sm_rel_scannxt with a ref).
 */
static Janet cfun_scan_count(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  sm_rel_scan_t *rscan = open_rel_cursor(s, argc, argv, "scan-count", p_tdef);
  uint64_t rowno = 0;
  sm_pin_ref_t ref;
  int64_t n = 0;
  int rc;
  while ((rc = sm_rel_scannxt(rscan, &rowno, &ref)) == 0) {
    n++;
  }
  // As (scan): a failed cursor's count is not the scan's.
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_rel_scanend(rscan);
    }
    panic_sm_error("scan-count sm_rel_scannxt");
  }
  sm_rel_scanend(rscan);
  return janet_wrap_number(static_cast<double>(n));
}

/*
 * Open a cursor over the index argv[0] names, between the bounds the
 * rest of argv gives, as (idx-scan) takes them; op names the command
 * in a panic. Sets p_tdef and p_idef to the index's table and index.
 * With rows, a cursor over the rows themselves goes there instead, as
 * sm_tbl_idx_scan_rows opens one, and nullptr is returned.
 */
static sm_idx_scan_t *open_idx_cursor(session_ctx_t *s, int32_t argc,
                                      Janet *argv, const char *op,
                                      table_def_t *&p_tdef,
                                      index_def_t *&p_idef,
                                      sm_rel_scan_t **rows = nullptr) {
  std::string idx_name = identifier_of(argv[0]);

  std::tie(p_tdef, p_idef) = need_index(s, idx_name, SM_LOCKMODE_IS, op);
  sm_tbl_t *tbl = p_tdef->tbl;
  uint64_t index = p_idef->def->oid;

  sm_bound_t lo_b = {nullptr, nullptr, 0, true};
  sm_bound_t hi_b = {nullptr, nullptr, 0, true};
  bool has_lo = false;
  bool has_hi = false;

  // Each bound's values, and the varlenas they point at.
  row_values_t lo_vals(0), hi_vals(0);

  for (int32_t i = 1; i < argc; i++) {
    if (janet_checktype(argv[i], JANET_KEYWORD)) {
      std::string kw = identifier_of(argv[i]);
      bool inc = (kw == "ge" || kw == "le" || kw == ":ge" || kw == ":le");
      bool is_lower = (kw == "ge" || kw == "gt" || kw == ":ge" || kw == ":gt");
      bool is_upper = (kw == "le" || kw == "lt" || kw == ":le" || kw == ":lt");

      if ((is_lower || is_upper) && i + 1 < argc) {
        Janet val = argv[i + 1];
        i++;
        row_values_t &vals = is_lower ? lo_vals : hi_vals;
        key_values_of(p_tdef, p_idef, val, vals, op);

        sm_bound_t &b = is_lower ? lo_b : hi_b;
        b.prefix = vals.values.data();
        b.isnull = vals.isnull.get();
        b.n = vals.values.size();
        b.inclusive = inc;
        (is_lower ? has_lo : has_hi) = true;
      }
    }
  }

  bool widened = false;
  // The rows themselves, as digres reads a tree or a primary key.
  if (rows != nullptr) {
    if (sm_tbl_idx_scan_rows(tbl, index, has_lo ? &lo_b : nullptr,
                             has_hi ? &hi_b : nullptr, &widened, rows) != 0) {
      panic_sm_error((std::string(op) + " sm_tbl_idx_scan_rows").c_str());
    }
    return nullptr;
  }
  sm_idx_scan_t *iscan = nullptr;
  if (sm_tbl_idx_scan(tbl, index, has_lo ? &lo_b : nullptr,
                      has_hi ? &hi_b : nullptr, &widened, &iscan) != 0) {
    panic_sm_error((std::string(op) + " sm_tbl_idx_scan").c_str());
  }
  return iscan;
}

/*
 * The key of the row of the entry iscan last gave, in a table whose
 * primary key is pk, as (scan) gives it: read from the row, as the
 * entry holds it only encoded; op names the command in a panic.
 */
static Janet entry_key(table_def_t *p_tdef, const index_def_t *pk,
                       sm_idx_scan_t *iscan, const char *op) {
  sm_tbl_t *tbl = p_tdef->tbl;
  const void *key = nullptr;
  size_t len = 0;
  sm_pin_ref_t ref = {};
  const void *tuple = nullptr;
  size_t tlen = 0;
  // The entry is one the transaction sees, and so is its row.
  if (sm_idx_scan_row(iscan, &key, &len) != 0 ||
      sm_tbl_read_key(tbl, key, len, &ref, &tuple, &tlen) != 0) {
    panic_sm_error(op);
  }
  std::vector<Janet> vals(p_tdef->columns.size(), janet_wrap_nil());
  for (const key_col_def_t &k : pk->keys) {
    sm_datum_t v = 0;
    bool isnull = false;
    if (sm_tbl_decode(tbl, tuple, tlen, k.col_idx, &v, &isnull) != 0) {
      sm_unpin(&ref);
      panic_sm_error(op);
    }
    vals[k.col_idx] = value_to_janet(v, isnull, k.type_code, k.typmod, tbl);
  }
  sm_unpin(&ref);
  return key_form(pk, vals.data());
}

/*
 * An index entry as [rowno cut include-tuple], or [key cut
 * include-tuple] in a table with a primary key, include-tuple nil for
 * an index without INCLUDE columns; entry holds the deformed values.
 */
static Janet idx_entry_to_janet(table_def_t *p_tdef, index_def_t *p_idef,
                                sm_idx_scan_t *iscan, uint64_t rowno, bool cut,
                                const void *val, size_t val_len,
                                row_values_t &entry, const char *op) {
  sm_tbl_t *tbl = p_tdef->tbl;
  uint64_t index = p_idef->def->oid;
  const index_def_t *pk = primary_of(p_tdef);
  Janet row = pk ? entry_key(p_tdef, pk, iscan, op)
                 : janet_wrap_number(static_cast<double>(rowno));
  Janet inc_val = janet_wrap_nil();
  if (p_idef->includes.size() > 0 && val != nullptr) {
    if (sm_tbl_idx_deform(tbl, index, val, val_len, entry.values.data(),
                          entry.isnull.get(), entry.values.size()) == 0) {
      // In idx value: cut (0), then includes (1...), then the row key
      size_t n_inc = p_idef->includes.size();
      Janet *ivals = janet_tuple_begin(n_inc);
      for (size_t m = 0; m < n_inc; m++) {
        ivals[m] = value_to_janet(entry.values[1 + m], entry.isnull[1 + m],
                                  p_idef->includes[m].type_code,
                                  p_idef->includes[m].typmod);
      }
      inc_val = janet_wrap_tuple(janet_tuple_end(ivals));
    }
  }

  Janet *item = janet_tuple_begin(3);
  item[0] = row;
  item[1] = janet_wrap_boolean(cut);
  item[2] = inc_val;
  return janet_wrap_tuple(janet_tuple_end(item));
}

/* (idx-scan idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]) */
static Janet cfun_idx_scan(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  index_def_t *p_idef = nullptr;
  sm_idx_scan_t *iscan =
      open_idx_cursor(s, argc, argv, "idx-scan", p_tdef, p_idef);

  JanetArray *res = janet_array(16);
  uint64_t rowno = 0;
  bool cut = false;
  const void *val = nullptr;
  size_t val_len = 0;

  row_values_t entry(sm_tbl_idx_ncols(p_tdef->tbl, p_idef->def->oid));

  int rc;
  while ((rc = sm_idx_scannxt(iscan, &rowno, &cut, &val, &val_len)) == 0) {
    janet_array_push(res, idx_entry_to_janet(p_tdef, p_idef, iscan, rowno, cut,
                                             val, val_len, entry, "idx-scan"));
  }

  // As (scan): a failed cursor's entries are not the scan's.
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_idx_scanend(iscan);
    }
    panic_sm_error("idx-scan sm_idx_scannxt");
  }
  sm_idx_scanend(iscan);
  return janet_wrap_array(res);
}

/*
 * (idx-rows idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]): [id row], or
 * [key row] in a table with a primary key, for each row a scan of the
 * index's rows gives, as digres reads a tree's or a primary key's.
 */
static Janet cfun_idx_rows(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  index_def_t *p_idef = nullptr;
  sm_rel_scan_t *rscan = nullptr;
  open_idx_cursor(s, argc, argv, "idx-rows", p_tdef, p_idef, &rscan);
  sm_tbl_t *tbl = p_tdef->tbl;
  const index_def_t *pk = primary_of(p_tdef);
  JanetArray *rows = janet_array(16);
  uint64_t rowno = 0;
  const void *tuple = nullptr;
  size_t len = 0;
  size_t ncols = p_tdef->columns.size();
  int rc;
  while ((rc = sm_rel_scannxt(rscan, &rowno, &tuple, &len)) == 0) {
    Janet *vals = janet_tuple_begin(ncols);
    for (size_t i = 0; i < ncols; i++) {
      sm_datum_t val = 0;
      bool isnull = false;
      if (sm_tbl_decode(tbl, tuple, len, i, &val, &isnull) != 0) {
        sm_rel_scanend(rscan);
        panic_sm_error("idx-rows sm_tbl_decode");
      }
      vals[i] = value_to_janet(val, isnull, p_tdef->columns[i].type_code,
                               p_tdef->columns[i].typmod, tbl);
    }
    Janet *pair = janet_tuple_begin(2);
    pair[0] =
        pk ? key_form(pk, vals) : janet_wrap_number(static_cast<double>(rowno));
    pair[1] = janet_wrap_tuple(janet_tuple_end(vals));
    janet_array_push(rows, janet_wrap_tuple(janet_tuple_end(pair)));
  }
  // As (scan): a failed cursor's rows are not the scan's.
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_rel_scanend(rscan);
    }
    panic_sm_error("idx-rows sm_rel_scannxt");
  }
  sm_rel_scanend(rscan);
  return janet_wrap_array(rows);
}

/*
 * (idx-only idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]): a tree's
 * index-only scan, as digres's (sm_rel_scan_entries): {:keys, each
 * entry's key columns, decoded from its key, :unread, how many came
 * with no row read, their leaves all visible (../BTREEINDEX.md §8)}.
 */
static Janet cfun_idx_only(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  index_def_t *p_idef = nullptr;
  sm_rel_scan_t *rscan = nullptr;
  open_idx_cursor(s, argc, argv, "idx-only", p_tdef, p_idef, &rscan);
  if (0 != sm_rel_scan_entries(rscan)) {
    sm_rel_scanend(rscan);
    panic_sm_error("idx-only sm_rel_scan_entries");
  }
  size_t nkeys = p_idef->keys.size();
  std::vector<sm_datum_t> vals(nkeys);
  std::unique_ptr<bool[]> nulls(new bool[nkeys]);
  JanetArray *keys = janet_array(16);
  int64_t unread = 0;
  uint64_t rowno = 0;
  sm_pin_ref_t ref;
  int rc;
  while ((rc = sm_rel_scannxt(rscan, &rowno, &ref)) == 0) {
    if (0 != sm_rel_scan_cols(rscan, vals.data(), nulls.get(), nkeys)) {
      sm_rel_scanend(rscan);
      panic_sm_error("idx-only sm_rel_scan_cols");
    }
    Janet *key = janet_tuple_begin(static_cast<int32_t>(nkeys));
    for (size_t i = 0; i < nkeys; i++) {
      const key_col_def_t &k = p_idef->keys[i];
      key[i] =
          value_to_janet(vals[i], nulls[i], k.type_code, k.typmod, p_tdef->tbl);
    }
    janet_array_push(keys, janet_wrap_tuple(janet_tuple_end(key)));
    unread += nullptr == ref.pin ? 1 : 0;
  }
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_rel_scanend(rscan);
    }
    panic_sm_error("idx-only sm_rel_scannxt");
  }
  sm_rel_scanend(rscan);
  JanetKV *out = janet_struct_begin(2);
  janet_struct_put(out, janet_ckeywordv("keys"), janet_wrap_array(keys));
  janet_struct_put(out, janet_ckeywordv("unread"),
                   janet_wrap_number(static_cast<double>(unread)));
  return janet_wrap_struct(janet_struct_end(out));
}

/*
 * (idx-entries idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]): how many
 * entries the index holds between the bounds, none read: a tree's
 * dead entries among them, until vacuum (compact) removes them.
 */
static Janet cfun_idx_entries(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  index_def_t *p_idef = nullptr;
  sm_idx_scan_t *iscan =
      open_idx_cursor(s, argc, argv, "idx-entries", p_tdef, p_idef);
  uint64_t rowno = 0;
  bool cut = false;
  const void *val = nullptr;
  size_t val_len = 0;
  int64_t n = 0;
  int rc;
  while ((rc = sm_idx_scannxt(iscan, &rowno, &cut, &val, &val_len)) == 0) {
    n++;
  }
  if (rc < 0) {
    if (!engine_aborted()) {
      sm_idx_scanend(iscan);
    }
    panic_sm_error("idx-entries sm_idx_scannxt");
  }
  sm_idx_scanend(iscan);
  return janet_wrap_number(static_cast<double>(n));
}

/* (idx-open idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]) */
static Janet cfun_idx_open(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  table_def_t *p_tdef = nullptr;
  index_def_t *p_idef = nullptr;
  sm_idx_scan_t *iscan =
      open_idx_cursor(s, argc, argv, "idx-open", p_tdef, p_idef);

  // The cursor takes the table handle over from the statement, as it
  // reads through it after the statement ends.
  auto c = std::make_unique<cursor_t>();
  auto owned = std::find_if(s->stmt_tables.begin(), s->stmt_tables.end(),
                            [p_tdef](const std::unique_ptr<table_def_t> &t) {
                              return t.get() == p_tdef;
                            });
  c->table = std::move(*owned);
  s->stmt_tables.erase(owned);
  c->idef = p_idef;
  c->scan = iscan;
  int32_t handle = ++s->next_cursor;
  s->cursors[handle] = std::move(c);
  return janet_wrap_integer(handle);
}

/* The open cursor handle argv[n] names in s, for op; a panic if none. */
static cursor_t *need_cursor(session_ctx_t *s, Janet *argv, int32_t n,
                             const char *op) {
  int32_t handle = janet_getinteger(argv, n);
  auto it = s->cursors.find(handle);
  if (it == s->cursors.end()) {
    janet_panicf("%s: no open cursor %d", op, handle);
  }
  return it->second.get();
}

/* (cursor-next c) */
static Janet cfun_cursor_next(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  cursor_t *c = need_cursor(s, argv, 0, "cursor-next");

  uint64_t rowno = 0;
  bool cut = false;
  const void *val = nullptr;
  size_t val_len = 0;
  switch (sm_idx_scannxt(c->scan, &rowno, &cut, &val, &val_len)) {
  case 0:
    break;
  case 1:
    return janet_wrap_nil();
  default:
    panic_sm_error("cursor-next sm_idx_scannxt");
  }
  row_values_t entry(sm_tbl_idx_ncols(c->table->tbl, c->idef->def->oid));
  return idx_entry_to_janet(c->table.get(), c->idef, c->scan, rowno, cut, val,
                            val_len, entry, "cursor-next");
}

/* (cursor-close c) */
static Janet cfun_cursor_close(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  need_cursor(s, argv, 0, "cursor-close");
  // End the cursor, then close the table handle it read through.
  s->cursors.erase(janet_getinteger(argv, 0));
  return janet_wrap_nil();
}

/* (sm:errnum) */
static Janet cfun_sm_errnum(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  return janet_wrap_integer(sm_errnum());
}

/* (sm:errname) */
static Janet cfun_sm_errname(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  return janet_cstringv(sm_errname());
}

/* (sm:errmsg) */
static Janet cfun_sm_errmsg(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  return janet_cstringv(sm_errmsg());
}

/* (sm:retryable) */
static Janet cfun_sm_retryable(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  return janet_wrap_boolean(sm_retryable() != 0);
}

/* (sm:reseterr) */
static Janet cfun_sm_reseterr(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  sm_reseterr();
  return janet_wrap_nil();
}

/* Recursive structural equality helper comparing tuples and arrays
 * interchangeably. */
static bool janet_values_equal(Janet a, Janet b) {
  if (janet_equals(a, b))
    return true;
  bool a_seq =
      janet_checktype(a, JANET_TUPLE) || janet_checktype(a, JANET_ARRAY);
  bool b_seq =
      janet_checktype(b, JANET_TUPLE) || janet_checktype(b, JANET_ARRAY);
  if (a_seq && b_seq) {
    const Janet *a_items = janet_checktype(a, JANET_TUPLE)
                               ? janet_unwrap_tuple(a)
                               : janet_unwrap_array(a)->data;
    int32_t a_len = janet_checktype(a, JANET_TUPLE)
                        ? janet_tuple_length(a_items)
                        : janet_unwrap_array(a)->count;
    const Janet *b_items = janet_checktype(b, JANET_TUPLE)
                               ? janet_unwrap_tuple(b)
                               : janet_unwrap_array(b)->data;
    int32_t b_len = janet_checktype(b, JANET_TUPLE)
                        ? janet_tuple_length(b_items)
                        : janet_unwrap_array(b)->count;
    if (a_len != b_len)
      return false;
    for (int32_t i = 0; i < a_len; i++) {
      if (!janet_values_equal(a_items[i], b_items[i]))
        return false;
    }
    return true;
  }
  return false;
}

/* (assert-eq actual expected [msg]) */
static Janet cfun_assert_eq(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  if (!janet_values_equal(argv[0], argv[1])) {
    const char *msg =
        (argc >= 3) ? janet_getcstring(argv, 2) : "assert-eq failed";
    JanetBuffer *buf = janet_buffer(64);
    janet_description_b(buf, argv[0]);
    std::string actual_str(reinterpret_cast<char *>(buf->data), buf->count);
    buf->count = 0;
    janet_description_b(buf, argv[1]);
    std::string expected_str(reinterpret_cast<char *>(buf->data), buf->count);
    janet_panicf("%s: expected %s, got %s", msg, expected_str.c_str(),
                 actual_str.c_str());
  }
  return janet_wrap_boolean(1);
}

/* (assert-nil val [msg]) */
static Janet cfun_assert_nil(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  if (!janet_checktype(argv[0], JANET_NIL)) {
    const char *msg =
        (argc >= 2) ? janet_getcstring(argv, 1) : "assert-nil failed";
    janet_panicf("%s: expected nil", msg);
  }
  return janet_wrap_boolean(1);
}

/* (assert-count n list [msg]) */
static Janet cfun_assert_count(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  int32_t expected = janet_getinteger(argv, 0);
  int32_t actual = 0;
  if (janet_checktype(argv[1], JANET_ARRAY)) {
    actual = janet_unwrap_array(argv[1])->count;
  } else if (janet_checktype(argv[1], JANET_TUPLE)) {
    actual = janet_tuple_length(janet_unwrap_tuple(argv[1]));
  } else {
    janet_panic("assert-count: second argument must be array or tuple");
  }
  if (actual != expected) {
    const char *msg =
        (argc >= 3) ? janet_getcstring(argv, 2) : "assert-count failed";
    janet_panicf("%s: expected count %d, got %d", msg, expected, actual);
  }
  return janet_wrap_boolean(1);
}

/* Registration table for Janet cfuns. */
static const JanetReg s_rel_cfuns[] = {
    {"env-init", cfun_env_init, "(env-init path) -- format home directory"},
    {"env-attach", cfun_env_attach,
     "(env-attach path) -- attach home directory"},
    {"env-detach", cfun_env_detach,
     "(env-detach [force]) -- detach home directory"},
    {"rm-rf", cfun_rm_rf,
     "(rm-rf path) -- recursively remove directory or file"},
    {"db-create", cfun_db_create, "(db-create name [opt]) -- create database"},
    {"db-drop", cfun_db_drop, "(db-drop name) -- drop database"},
    {"db-list", cfun_db_list, "(db-list) -- list databases"},
    {"db-open", cfun_db_open,
     "(db-open name [{:savepoint-undo-bytes n}]) -- open default session"},
    {"db-close", cfun_db_close, "(db-close) -- close active session"},

    {"_session-open", cfun_session_open, "internal"},
    {"_session-use", cfun_session_use, "internal"},
    {"_session-close", cfun_session_close, "internal"},

    {"begin", cfun_begin, "(begin [:rc|:rr|:ser]) -- begin transaction"},
    {"commit", cfun_commit, "(commit) -- commit transaction"},
    {"abort", cfun_abort, "(abort) -- abort transaction"},
    {"snapshot", cfun_snapshot, "(snapshot) -- pin/advance statement snapshot"},
    {"stmt-begin", cfun_snapshot, "(stmt-begin) -- alias for (snapshot)"},
    {"cat-epoch", cfun_cat_epoch,
     "(cat-epoch) -- the catalog's epoch, or :own with DDL of the "
     "transaction's own"},
    {"stmt-inner", cfun_stmt_inner,
     "(stmt-inner) -- begin a statement inside the running one"},
    {"stmt-end", cfun_stmt_end,
     "(stmt-end) -- end the last (stmt-inner), resuming the outer"},
    {"stmt-save", cfun_stmt_save,
     "(stmt-save) -- save the running statement's view; returns a handle"},
    {"stmt-resume", cfun_stmt_resume,
     "(stmt-resume v) -- run view v's statement; (stmt-end) ends it"},
    {"stmt-free", cfun_stmt_free, "(stmt-free v) -- free view v"},
    {"stmt-advance", cfun_stmt_advance,
     "(stmt-advance) -- the running statement sees its writes so far"},
    {"qa-nowait", cfun_qa_nowait,
     "(qa-nowait on) -- fail with ERR_BLOCKED where a lock would wait"},
    {"savepoint", cfun_savepoint,
     "(savepoint) -- create savepoint, returns depth"},
    {"rollback", cfun_rollback, "(rollback depth) -- rollback to savepoint"},
    {"forget", cfun_forget, "(forget depth) -- forget savepoint"},
    {"prepare", cfun_prepare, "(prepare gid) -- prepare 2PC transaction"},
    {"prepared-list", cfun_prepared_list,
     "(prepared-list) -- list prepared transactions"},
    {"commit-prepared", cfun_commit_prepared,
     "(commit-prepared gid) -- commit prepared transaction"},
    {"abort-prepared", cfun_abort_prepared,
     "(abort-prepared gid) -- abort prepared transaction"},

    {"_table-create", cfun_table_create, "internal"},
    {"_temp-table-create", cfun_temp_table_create, "internal"},
    {"_table-drop", cfun_table_drop, "internal"},
    {"_namespace-create", cfun_namespace_create, "internal"},
    {"_namespace-drop", cfun_namespace_drop, "internal"},
    {"list-namespaces", cfun_list_namespaces,
     "(list-namespaces) -- the namespace names, sorted"},
    {"_seq-create", cfun_seq_create, "internal"},
    {"_seq-drop", cfun_seq_drop, "internal"},
    {"_nextseq", cfun_nextseq, "internal"},
    {"nextseq", cfun_nextseq, "(nextseq name) -- get next sequence value"},
    {"_index-create", cfun_index_create, "internal"},
    {"_add-primary-key", cfun_add_primary_key, "internal"},
    {"_alter-column-type", cfun_alter_column_type, "internal"},
    {"_set-not-null", cfun_set_not_null, "internal"},
    {"_drop-not-null", cfun_drop_not_null, "internal"},
    {"_rename-not-null", cfun_rename_not_null, "internal"},
    {"_not-nulls", cfun_not_nulls, "internal"},
    {"_set-default", cfun_set_default, "internal"},
    {"_index-drop", cfun_index_drop, "internal"},
    {"_index-wait", cfun_index_wait, "internal"},
    {"_index-state", cfun_index_state, "internal"},
    {"_table-version", cfun_table_version, "internal"},
    {"tables-parked", cfun_tables_parked,
     "(tables-parked) -- the handles the session keeps for its next opens"},
    {"list-tables", cfun_list_tables,
     "(list-tables) -- the table names in public, sorted"},
    {"_describe-table", cfun_describe_table, "internal"},
    {"_fkey-create", cfun_fkey_create, "internal"},
    {"_fkey-drop", cfun_fkey_drop, "internal"},
    {"_fkey-validate", cfun_fkey_validate, "internal"},
    {"_fkey-rename", cfun_fkey_rename, "internal"},
    {"_fkeys", cfun_fkeys, "internal"},
    {"_referenced-by", cfun_referenced_by, "internal"},
    {"_check-create", cfun_check_create, "internal"},
    {"_check-drop", cfun_check_drop, "internal"},
    {"_check-validate", cfun_check_validate, "internal"},
    {"_check-rename", cfun_check_rename, "internal"},
    {"_checks", cfun_checks, "internal"},

    {"_insert", cfun_insert, "internal"},
    {"_read", cfun_read, "internal"},
    {"_read-latest", cfun_read_latest, "internal"},
    {"_read-keys", cfun_read_keys, "internal"},
    {"_lock-row", cfun_lock_row, "internal"},
    {"_truncate", cfun_truncate, "internal"},
    {"_compact", cfun_compact, "internal"},
    {"cleaner-pass", cfun_cleaner_pass, "one pass of the btree cleaner"},
    {"_load", cfun_load, "internal"},
    {"_table-size", cfun_table_size, "internal"},
    {"_analyze", cfun_analyze, "internal"},
    {"_stats", cfun_stats, "internal"},
    {"_update", cfun_update, "internal"},
    {"_delete", cfun_delete, "internal"},
    {"_scan", cfun_scan, "internal"},
    {"_scan-count", cfun_scan_count, "internal"},
    {"_idx-scan", cfun_idx_scan, "internal"},
    {"_idx-rows", cfun_idx_rows, "internal"},
    {"_idx-entries", cfun_idx_entries, "internal"},
    {"_idx-only", cfun_idx_only, "internal"},
    {"_idx-open", cfun_idx_open, "internal"},
    {"cursor-next", cfun_cursor_next,
     "(cursor-next c) -- the cursor's next index entry, or nil at its end"},
    {"cursor-close", cfun_cursor_close, "(cursor-close c) -- end the cursor"},

    {"sm:errnum", cfun_sm_errnum, "(sm:errnum) -- last error code"},
    {"sm:errname", cfun_sm_errname, "(sm:errname) -- last error name"},
    {"sm:errmsg", cfun_sm_errmsg, "(sm:errmsg) -- last error message"},
    {"sm:retryable", cfun_sm_retryable,
     "(sm:retryable) -- true if retryable error"},
    {"sm:reseterr", cfun_sm_reseterr, "(sm:reseterr) -- reset error state"},

    {"assert-eq", cfun_assert_eq,
     "(assert-eq actual expected [msg]) -- equality check"},
    {"assert-nil", cfun_assert_nil, "(assert-nil val [msg]) -- nil check"},
    {"assert-count", cfun_assert_count,
     "(assert-count n list [msg]) -- count check"},
    {nullptr, nullptr, nullptr}};

/* Macro script evaluated into env at startup. */
static const char *s_prelude = R"janet(
(defmacro table-create [name & body]
  ~(_table-create (string ',name) ',body))

(defmacro temp-table-create [name & body]
  ~(_temp-table-create (string ',name) ',body))

(defmacro table-drop [name]
  ~(_table-drop (string ',name)))

(defmacro namespace-create [name]
  ~(_namespace-create (string ',name)))

(defmacro namespace-drop [name]
  ~(_namespace-drop (string ',name)))

(defmacro seq-create [name & rest]
  ~(_seq-create (string ',name) ,;rest))

(defmacro seq-drop [name]
  ~(_seq-drop (string ',name)))

(defmacro nextseq [name]
  ~(_nextseq (string ',name)))

(defmacro index-create [name & body]
  ~(_index-create (string ',name) ',body))

(defmacro add-primary-key [tbl & cols]
  ~(_add-primary-key (string ',tbl) ',cols))

(defmacro index-drop [name]
  ~(_index-drop (string ',name)))

(defmacro index-wait [name]
  ~(_index-wait (string ',name)))

(defmacro index-state [name]
  ~(_index-state (string ',name)))
(defmacro table-version [name]
  ~(_table-version (string ',name)))

(defmacro describe-table [name & flags]
  ~(_describe-table (string ',name) ,;flags))

(defmacro fkey-create [name & body]
  ~(_fkey-create (string ',name) ',body))

(defmacro fkey-drop [tbl name]
  ~(_fkey-drop (string ',tbl) (string ',name)))

(defmacro fkey-validate [tbl name]
  ~(_fkey-validate (string ',tbl) (string ',name)))

(defmacro alter-column-type [tbl col type & rest]
  ~(_alter-column-type (string ',tbl) (string ',col) ',type ,;rest))

(defmacro set-not-null [tbl col & rest]
  ~(_set-not-null (string ',tbl) (string ',col) ,;(map (fn [n] ~(string ',n)) rest)))

(defmacro rename-not-null [tbl col to]
  ~(_rename-not-null (string ',tbl) (string ',col) (string ',to)))

(defmacro not-nulls [tbl]
  ~(_not-nulls (string ',tbl)))

(defmacro drop-not-null [tbl col]
  ~(_drop-not-null (string ',tbl) (string ',col)))

(defmacro set-default [tbl col & rest]
  ~(_set-default (string ',tbl) (string ',col) ,;rest))

(defmacro fkey-rename [tbl name new-name]
  ~(_fkey-rename (string ',tbl) (string ',name) (string ',new-name)))

(defmacro fkeys [tbl]
  ~(_fkeys (string ',tbl)))

(defmacro referenced-by [tbl]
  ~(_referenced-by (string ',tbl)))

(defmacro check-create [tbl name cols & opts]
  ~(_check-create (string ',tbl) (string ',name) ',cols ,;opts))

(defmacro check-drop [tbl name]
  ~(_check-drop (string ',tbl) (string ',name)))

(defmacro check-validate [tbl name]
  ~(_check-validate (string ',tbl) (string ',name)))

(defmacro check-rename [tbl name new-name]
  ~(_check-rename (string ',tbl) (string ',name) (string ',new-name)))

(defmacro checks [tbl]
  ~(_checks (string ',tbl)))

(defmacro insert [tbl row]
  ~(_insert (string ',tbl) ,row))

(defmacro read [tbl rowno]
  ~(_read (string ',tbl) ,rowno))

(defmacro read-latest [tbl rowno]
  ~(_read-latest (string ',tbl) ,rowno))

(defmacro read-keys [tbl keys]
  ~(_read-keys (string ',tbl) ,keys))

(defmacro lock-row [tbl rowno mode]
  ~(_lock-row (string ',tbl) ,rowno ,mode))

(defmacro truncate [tbl]
  ~(_truncate (string ',tbl)))

(defmacro load [tbl rows]
  ~(_load (string ',tbl) ,rows))

(defmacro compact [tbl]
  ~(_compact (string ',tbl)))

(defmacro table-size [tbl]
  ~(_table-size (string ',tbl)))

(defmacro analyze [tbl & rest]
  ~(_analyze (string ',tbl) ,;rest))

(defmacro stats [tbl]
  ~(_stats (string ',tbl)))

(defmacro update [tbl rowno row]
  ~(_update (string ',tbl) ,rowno ,row))

(defmacro delete [tbl rowno]
  ~(_delete (string ',tbl) ,rowno))

(defmacro scan [tbl & rest]
  ~(_scan (string ',tbl) ,;rest))
(defmacro scan-count [tbl & rest]
  ~(_scan-count (string ',tbl) ,;rest))

(defmacro idx-scan [idx & rest]
  ~(_idx-scan (string ',idx) ,;rest))

(defmacro idx-rows [idx & rest]
  ~(_idx-rows (string ',idx) ,;rest))
(defmacro idx-only [idx & rest]
  ~(_idx-only (string ',idx) ,;rest))

(defmacro idx-entries [idx & rest]
  ~(_idx-entries (string ',idx) ,;rest))

(defmacro idx-open [idx & rest]
  ~(_idx-open (string ',idx) ,;rest))

(defmacro session-open [name dbname & rest]
  ~(_session-open (string ',name) ,dbname ,;rest))

(defmacro session-use [name]
  ~(_session-use (string ',name)))

(defmacro session-close [name]
  ~(_session-close (string ',name)))

(defmacro assert-err [expected expr]
  ~(try
     (do
       ,expr
       (error "assert-err: expected error but expression succeeded"))
     ([err]
      (def err-str (string err))
      (def exp-str (string/replace-all "-" "_" (string/ascii-upper (string ,expected))))
      (unless (string/find exp-str err-str)
        (error (string "assert-err: expected " exp-str " but got " err-str))))))
)janet";

/* Register all rel functions into the Janet environment. */
void register_rel_cfuns(JanetTable *env) {
  janet_cfuns(env, nullptr, s_rel_cfuns);
  // Evaluate prelude macros into environment.
  Janet result;
  janet_dostring(env, s_prelude, "prelude.janet", &result);
}

} // namespace rel
