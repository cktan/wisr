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

/* Fail with last sm_ error. */
[[noreturn]] static void panic_sm_error(const char *action) {
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

/* (env-attach path) */
static Janet cfun_env_attach(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  const char *path = janet_getcstring(argv, 0);
  // Attach to the wisr environment.
  if (get_catalog().env_attach(path) != 0) {
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
  char **names = nullptr;
  size_t n = 0;
  // List all databases.
  if (sm_lsdb(env, &names, &n) != 0) {
    panic_sm_error("db-list");
  }
  JanetArray *arr = janet_array(n);
  for (size_t i = 0; i < n; i++) {
    janet_array_push(arr, janet_cstringv(names[i]));
  }
  sm_free(names);
  return janet_wrap_array(arr);
}

/* (db-open dbname [options]) */
static Janet cfun_db_open(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  std::string dbname = identifier_of(argv[0]);
  // Open default session on database.
  if (get_catalog().session_open("default", dbname, nullptr) != 0) {
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
  // Begin transaction at the requested isolation level.
  if (sm_begin(s->session, level) != 0) {
    panic_sm_error("begin");
  }
  return janet_wrap_nil();
}

/* (commit) */
static Janet cfun_commit(int32_t argc, Janet *argv) {
  (void)argc;
  (void)argv;
  session_ctx_t *s = require_session();
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
  sm_prepared_t *list = nullptr;
  size_t n = 0;
  if (sm_list_prepared(s->session, &list, &n) != 0) {
    panic_sm_error("prepared-list");
  }
  JanetArray *arr = janet_array(n);
  for (size_t i = 0; i < n; i++) {
    JanetTable *t = janet_table(2);
    janet_table_put(t, janet_ckeywordv("gid"), janet_cstringv(list[i].gid));
    janet_table_put(
        t, janet_ckeywordv("prepared-at"),
        janet_wrap_number(static_cast<double>(list[i].prepared_at)));
    janet_array_push(arr, janet_wrap_table(t));
  }
  sm_free(list);
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

/* (table-create name (columns ...)) */
static Janet cfun_table_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, -1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  std::vector<col_def_t> columns;

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
          }
        }
        if (typmod_of(c.type_code, mods, &c.typmod) != 0) {
          janet_panicf("table-create: type %s takes no such modifiers, for "
                       "column '%s'",
                       type_name_of(c.type_code), c.name.c_str());
        }
        columns.push_back(std::move(c));
      }
    }
  }

  if (columns.empty()) {
    janet_panic("table-create: table must have at least one column");
  }

  // sm3 makes its store, rowno sequence and catalog entry.
  std::vector<sm_column_spec_t> specs;
  for (const col_def_t &c : columns) {
    specs.push_back({c.name.c_str(), c.type_code, c.typmod, c.not_null});
  }
  uint64_t oid = 0;
  if (sm_cat_create_table(s->session, SM_NSP_PUBLIC, tbl_name.c_str(),
                          specs.data(), specs.size(), &oid) != 0) {
    panic_sm_error("table-create");
  }
  return janet_wrap_nil();
}

/* (table-drop name) */
static Janet cfun_table_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  uint64_t oid = 0;
  int rc = sm_cat_table_oid(s->session, SM_NSP_PUBLIC, tbl_name.c_str(), &oid);
  if (rc == 1) {
    janet_panicf("table-drop: table '%s' does not exist", tbl_name.c_str());
  }
  // Its store, rows, indexes and rowno sequence go with it.
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

/* (index-create idx_name (table tbl) (key ...) [:unique bool] [:include ...])
 */
static Janet cfun_index_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, -1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  std::string tbl_name;
  index_def_t idef;
  idef.name = idx_name;
  idef.unique = false;

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
        k.collation_name = identifier_of(k_items[0]);

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
  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCK_EXCLUSIVE, "index-create");

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

  // sm3 records it, under the table's next prefix.
  std::vector<sm_index_key_spec_t> keys;
  for (const key_col_def_t &k : idef.keys) {
    keys.push_back({static_cast<int32_t>(k.col_idx), k.desc, k.nulls_first});
  }
  std::vector<int32_t> includes;
  for (const include_col_def_t &inc : idef.includes) {
    includes.push_back(static_cast<int32_t>(inc.col_idx));
  }
  uint64_t oid = 0;
  if (sm_cat_create_index(s->session, p_tdef->def->oid, idx_name.c_str(),
                          keys.data(), keys.size(), includes.data(),
                          includes.size(), idef.unique, &oid) != 0) {
    panic_sm_error("index-create");
  }
  return janet_wrap_nil();
}

/* (index-drop idx_name) */
static Janet cfun_index_drop(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  uint64_t table = 0, index = 0;
  int rc = sm_cat_index_oid(s->session, SM_NSP_PUBLIC, idx_name.c_str(), &table,
                            &index);
  if (rc == 1) {
    janet_panicf("index-drop: index '%s' not found", idx_name.c_str());
  }
  if (rc != 0 || sm_cat_drop_index(s->session, table, index) != 0) {
    panic_sm_error("index-drop");
  }
  return janet_wrap_nil();
}

/*
 * The values of row at the columns an index names -- its key or
 * INCLUDE columns, each with a col_idx -- for an sm_idx_* call.
 */
template <class C>
static row_values_t values_at(const row_values_t &row,
                              const std::vector<C> &cols) {
  row_values_t out(cols.size());
  for (size_t i = 0; i < cols.size(); i++) {
    out.values[i] = row.values[cols[i].col_idx];
    out.isnull[i] = row.isnull[cols[i].col_idx];
  }
  return out;
}

/* (insert table [val1 val2 ...]) */
static Janet cfun_insert(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCK_INTENT_EXCLUSIVE, "insert");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("insert get_or_open_rel");
  }

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
    janet_panicf("insert: expected %zu values, got %d", p_tdef->columns.size(),
                 row_len);
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

  // Encode the tuple before the savepoint, so a bad row writes nothing.
  std::vector<sm_attribute_t> attrs = attributes_of(*p_tdef);
  void *tuple = nullptr;
  size_t tuple_len = 0;
  if (sm_tup_encode(attrs.data(), attrs.size(), row.values.data(),
                    row.isnull.get(), &tuple, &tuple_len) != 0) {
    panic_sm_error("insert sm_tup_encode");
  }

  uint64_t rowno = 0;
  int sp = sm_savepoint(s->session);
  if (sp < 0) {
    sm_free(tuple);
    panic_sm_error("insert sm_savepoint");
  }

  // 1. Insert into heap relation.
  int rc = sm_rel_insert_tuple(rel, tuple, tuple_len, &rowno);
  sm_free(tuple);
  if (rc != 0) {
    if (!sm_retryable()) {
      sm_rollback(s->session, sp);
      sm_forget(s->session, sp);
    }
    panic_sm_error("insert sm_rel_insert_tuple");
  }

  // 2. Insert into attached secondary indexes.
  for (auto &[iname, idef] : p_tdef->indexes) {
    sm_idx_t *idx = get_catalog().get_or_open_idx(s, idef, *p_tdef);
    if (idx == nullptr) {
      if (!sm_retryable()) {
        sm_rollback(s->session, sp);
        sm_forget(s->session, sp);
      }
      panic_sm_error("insert get_or_open_idx");
    }
    row_values_t kv = values_at(row, idef.keys);
    row_values_t iv = values_at(row, idef.includes);
    if (sm_idx_insert(idx, kv.values.data(), kv.isnull.get(), kv.values.size(),
                      rowno, iv.values.data(), iv.isnull.get(),
                      iv.values.size()) != 0) {
      if (!sm_retryable()) {
        sm_rollback(s->session, sp);
        sm_forget(s->session, sp);
      }
      panic_sm_error("insert sm_idx_insert");
    }
  }
  sm_forget(s->session, sp);

  return janet_wrap_number(static_cast<double>(rowno));
}

/* (read table rowno) */
static Janet cfun_read(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  uint64_t rowno = static_cast<uint64_t>(janet_getinteger64(argv, 1));

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCK_INTENT_SHARED, "read");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("read get_or_open_rel");
  }

  const void *tuple = nullptr;
  size_t len = 0;
  int rc = sm_rel_read(rel, rowno, &tuple, &len);
  if (rc == 1) {
    return janet_wrap_nil();
  }
  if (rc != 0) {
    panic_sm_error("read sm_rel_read");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t out(ncols);
  if (sm_rel_deform(rel, tuple, len, out.values.data(), out.isnull.get(),
                    ncols) != 0) {
    panic_sm_error("read sm_rel_deform");
  }

  Janet *vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    vals[i] =
        value_to_janet(out.values[i], out.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  return janet_wrap_tuple(janet_tuple_end(vals));
}

/* (read-latest table rowno) */
static Janet cfun_read_latest(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  uint64_t rowno = static_cast<uint64_t>(janet_getinteger64(argv, 1));

  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCK_INTENT_SHARED, "read-latest");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("read-latest get_or_open_rel");
  }

  const void *tuple = nullptr;
  size_t len = 0;
  int rc = sm_rel_read_latest(rel, rowno, &tuple, &len);
  if (rc == 1) {
    return janet_wrap_nil();
  }
  if (rc != 0) {
    panic_sm_error("read-latest sm_rel_read_latest");
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t out(ncols);
  if (sm_rel_deform(rel, tuple, len, out.values.data(), out.isnull.get(),
                    ncols) != 0) {
    panic_sm_error("read-latest sm_rel_deform");
  }

  Janet *vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    vals[i] =
        value_to_janet(out.values[i], out.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  return janet_wrap_tuple(janet_tuple_end(vals));
}

/* (update table rowno [new_val1 new_val2 ...]) */
static Janet cfun_update(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  uint64_t rowno = static_cast<uint64_t>(janet_getinteger64(argv, 1));

  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCK_INTENT_EXCLUSIVE, "update");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("update get_or_open_rel");
  }

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
    janet_panicf("update: expected %zu values, got %d", p_tdef->columns.size(),
                 row_len);
  }

  row_values_t row(row_len);
  for (int32_t i = 0; i < row_len; i++) {
    if (janet_to_value(row_items[i], p_tdef->columns[i], row.bufs[i],
                       &row.values[i], &row.isnull[i]) != 0) {
      janet_panicf("update: invalid value for column '%s'",
                   p_tdef->columns[i].name.c_str());
    }
  }

  int sp = sm_savepoint(s->session);
  if (sp < 0) {
    panic_sm_error("update sm_savepoint");
  }

  const void *old_tuple = nullptr;
  size_t old_len = 0;
  if (sm_rel_update(rel, rowno, row.values.data(), row.isnull.get(),
                    row.values.size(), &old_tuple, &old_len) != 0) {
    if (sm_errnum() == SM_ERR_UPDATED) {
      sm_forget(s->session, sp);
    } else if (!sm_retryable()) {
      sm_rollback(s->session, sp);
      sm_forget(s->session, sp);
    }
    panic_sm_error("update sm_rel_update");
  }

  // Copy the old tuple: it views session memory that the index
  // calls below may reuse, and its values must outlive them.
  std::string old_copy;
  if (old_tuple != nullptr) {
    old_copy.assign(static_cast<const char *>(old_tuple), old_len);
  }

  // Deform old tuple to update indexes.
  size_t ncols = p_tdef->columns.size();
  row_values_t old(ncols);
  if (old_tuple != nullptr &&
      sm_rel_deform(rel, old_copy.data(), old_copy.size(), old.values.data(),
                    old.isnull.get(), ncols) == 0) {
    for (auto &[iname, idef] : p_tdef->indexes) {
      sm_idx_t *idx = get_catalog().get_or_open_idx(s, idef, *p_tdef);
      if (idx == nullptr) {
        if (!sm_retryable()) {
          sm_rollback(s->session, sp);
          sm_forget(s->session, sp);
        }
        panic_sm_error("update get_or_open_idx");
      }
      row_values_t okv = values_at(old, idef.keys);
      row_values_t nkv = values_at(row, idef.keys);
      row_values_t niv = values_at(row, idef.includes);
      if (sm_idx_delete(idx, okv.values.data(), okv.isnull.get(),
                        okv.values.size(), rowno) != 0 ||
          sm_idx_insert(idx, nkv.values.data(), nkv.isnull.get(),
                        nkv.values.size(), rowno, niv.values.data(),
                        niv.isnull.get(), niv.values.size()) != 0) {
        if (!sm_retryable()) {
          sm_rollback(s->session, sp);
          sm_forget(s->session, sp);
        }
        panic_sm_error("update sm_idx_insert");
      }
    }
  }
  sm_forget(s->session, sp);

  if (old_tuple == nullptr) {
    return janet_wrap_nil();
  }
  Janet *old_vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    old_vals[i] =
        value_to_janet(old.values[i], old.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  return janet_wrap_tuple(janet_tuple_end(old_vals));
}

/* (delete table rowno) */
static Janet cfun_delete(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);
  uint64_t rowno = static_cast<uint64_t>(janet_getinteger64(argv, 1));

  table_def_t *p_tdef =
      need_table(s, tbl_name, SM_LOCK_INTENT_EXCLUSIVE, "delete");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("delete get_or_open_rel");
  }

  int sp = sm_savepoint(s->session);
  if (sp < 0) {
    panic_sm_error("delete sm_savepoint");
  }

  const void *old_tuple = nullptr;
  size_t old_len = 0;
  int rc = sm_rel_delete(rel, rowno, &old_tuple, &old_len);
  if (rc == 1) {
    sm_forget(s->session, sp);
    return janet_wrap_nil();
  }
  if (rc != 0) {
    if (sm_errnum() == SM_ERR_UPDATED) {
      sm_forget(s->session, sp);
    } else if (!sm_retryable()) {
      sm_rollback(s->session, sp);
      sm_forget(s->session, sp);
    }
    panic_sm_error("delete sm_rel_delete");
  }

  // Copy the old tuple: it views session memory that the index
  // calls below may reuse, and its values must outlive them.
  std::string old_copy;
  if (old_tuple != nullptr) {
    old_copy.assign(static_cast<const char *>(old_tuple), old_len);
  }

  size_t ncols = p_tdef->columns.size();
  row_values_t old(ncols);
  if (old_tuple != nullptr &&
      sm_rel_deform(rel, old_copy.data(), old_copy.size(), old.values.data(),
                    old.isnull.get(), ncols) == 0) {
    for (auto &[iname, idef] : p_tdef->indexes) {
      sm_idx_t *idx = get_catalog().get_or_open_idx(s, idef, *p_tdef);
      if (idx == nullptr) {
        if (!sm_retryable()) {
          sm_rollback(s->session, sp);
          sm_forget(s->session, sp);
        }
        panic_sm_error("delete get_or_open_idx");
      }
      row_values_t okv = values_at(old, idef.keys);
      if (sm_idx_delete(idx, okv.values.data(), okv.isnull.get(),
                        okv.values.size(), rowno) != 0) {
        if (!sm_retryable()) {
          sm_rollback(s->session, sp);
          sm_forget(s->session, sp);
        }
        panic_sm_error("delete sm_idx_delete");
      }
    }
  }
  sm_forget(s->session, sp);

  if (old_tuple == nullptr) {
    return janet_wrap_nil();
  }
  Janet *old_vals = janet_tuple_begin(ncols);
  for (size_t i = 0; i < ncols; i++) {
    old_vals[i] =
        value_to_janet(old.values[i], old.isnull[i],
                       p_tdef->columns[i].type_code, p_tdef->columns[i].typmod);
  }
  return janet_wrap_tuple(janet_tuple_end(old_vals));
}

/* (scan table [:from first] [:to last]) */
static Janet cfun_scan(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  std::string tbl_name = identifier_of(argv[0]);

  uint64_t first = 1;
  uint64_t last = UINT64_MAX;

  for (int32_t i = 1; i < argc; i++) {
    if (janet_checktype(argv[i], JANET_KEYWORD)) {
      std::string kw = identifier_of(argv[i]);
      if ((kw == "from" || kw == ":from") && i + 1 < argc) {
        first = static_cast<uint64_t>(janet_getinteger64(argv, i + 1));
        i++;
      } else if ((kw == "to" || kw == ":to") && i + 1 < argc) {
        last = static_cast<uint64_t>(janet_getinteger64(argv, i + 1));
        i++;
      }
    }
  }

  table_def_t *p_tdef = need_table(s, tbl_name, SM_LOCK_INTENT_SHARED, "scan");
  sm_rel_t *rel = get_catalog().get_or_open_rel(s, *p_tdef);
  if (rel == nullptr) {
    panic_sm_error("scan get_or_open_rel");
  }

  sm_rel_scan_t *rscan = nullptr;
  if (sm_rel_scan(rel, first, last, &rscan) != 0) {
    panic_sm_error("scan sm_rel_scan");
  }

  JanetArray *rows = janet_array(16);
  uint64_t rowno = 0;
  const void *tuple = nullptr;
  size_t len = 0;
  size_t ncols = p_tdef->columns.size();

  while (sm_rel_scannxt(rscan, &rowno, &tuple, &len) == 0) {
    Janet *vals = janet_tuple_begin(ncols);
    for (size_t i = 0; i < ncols; i++) {
      sm_datum_t val = 0;
      bool isnull = false;
      if (sm_rel_decode(rel, tuple, len, i, &val, &isnull) != 0) {
        sm_rel_scanend(rscan);
        panic_sm_error("scan sm_rel_decode");
      }
      vals[i] = value_to_janet(val, isnull, p_tdef->columns[i].type_code,
                               p_tdef->columns[i].typmod);
    }
    Janet t_val = janet_wrap_tuple(janet_tuple_end(vals));

    Janet *pair = janet_tuple_begin(2);
    pair[0] = janet_wrap_number(static_cast<double>(rowno));
    pair[1] = t_val;
    janet_array_push(rows, janet_wrap_tuple(janet_tuple_end(pair)));
  }

  sm_rel_scanend(rscan);
  return janet_wrap_array(rows);
}

/* (idx-scan idx_name [:ge lo] [:gt lo] [:le hi] [:lt hi]) */
static Janet cfun_idx_scan(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, -1);
  session_ctx_t *s = require_session();
  std::string idx_name = identifier_of(argv[0]);

  auto [p_tdef, p_idef] =
      need_index(s, idx_name, SM_LOCK_INTENT_SHARED, "idx-scan");
  sm_idx_t *idx = get_catalog().get_or_open_idx(s, *p_idef, *p_tdef);
  if (idx == nullptr) {
    panic_sm_error("idx-scan get_or_open_idx");
  }

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
        const Janet *b_items = nullptr;
        int32_t b_len = 0;
        if (janet_checktype(val, JANET_TUPLE)) {
          b_items = janet_unwrap_tuple(val);
          b_len = janet_tuple_length(b_items);
        } else if (janet_checktype(val, JANET_ARRAY)) {
          b_items = janet_unwrap_array(val)->data;
          b_len = janet_unwrap_array(val)->count;
        } else {
          b_items = &val;
          b_len = 1;
        }

        if (static_cast<size_t>(b_len) > p_idef->keys.size()) {
          janet_panicf("idx-scan: a bound of %d values on %zu key columns",
                       b_len, p_idef->keys.size());
        }
        row_values_t &vals = is_lower ? lo_vals : hi_vals;
        vals = row_values_t(b_len);
        for (int32_t j = 0; j < b_len; j++) {
          const auto &k = p_idef->keys[j];
          col_def_t tmp_col;
          tmp_col.name = p_tdef->columns[k.col_idx].name;
          tmp_col.type_code = k.type_code;
          tmp_col.typmod = k.typmod;
          tmp_col.not_null = false;
          if (janet_to_value(b_items[j], tmp_col, vals.bufs[j], &vals.values[j],
                             &vals.isnull[j]) != 0) {
            janet_panicf("idx-scan: invalid bound value for column '%s'",
                         tmp_col.name.c_str());
          }
        }

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
  sm_idx_scan_t *iscan = nullptr;
  if (sm_idx_scan(idx, has_lo ? &lo_b : nullptr, has_hi ? &hi_b : nullptr,
                  &widened, &iscan) != 0) {
    panic_sm_error("idx-scan sm_idx_scan");
  }

  JanetArray *res = janet_array(16);
  uint64_t rowno = 0;
  bool cut = false;
  const void *val = nullptr;
  size_t val_len = 0;

  size_t n_val_cols = sm_idx_ncols(idx);
  row_values_t entry(n_val_cols);

  while (sm_idx_scannxt(iscan, &rowno, &cut, &val, &val_len) == 0) {
    Janet inc_val = janet_wrap_nil();
    if (p_idef->includes.size() > 0 && val != nullptr) {
      if (sm_idx_deform(idx, val, val_len, entry.values.data(),
                        entry.isnull.get(), n_val_cols) == 0) {
        // In idx value: rowno (0), cut (1), then includes (2...)
        size_t n_inc = p_idef->includes.size();
        Janet *ivals = janet_tuple_begin(n_inc);
        for (size_t m = 0; m < n_inc; m++) {
          ivals[m] = value_to_janet(entry.values[2 + m], entry.isnull[2 + m],
                                    p_idef->includes[m].type_code,
                                    p_idef->includes[m].typmod);
        }
        inc_val = janet_wrap_tuple(janet_tuple_end(ivals));
      }
    }

    Janet *item = janet_tuple_begin(3);
    item[0] = janet_wrap_number(static_cast<double>(rowno));
    item[1] = janet_wrap_boolean(cut);
    item[2] = inc_val;
    janet_array_push(res, janet_wrap_tuple(janet_tuple_end(item)));
  }

  sm_idx_scanend(iscan);
  return janet_wrap_array(res);
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
    {"db-open", cfun_db_open, "(db-open name [opt]) -- open default session"},
    {"db-close", cfun_db_close, "(db-close) -- close active session"},

    {"_session-open", cfun_session_open, "internal"},
    {"_session-use", cfun_session_use, "internal"},
    {"_session-close", cfun_session_close, "internal"},

    {"begin", cfun_begin, "(begin [:rc|:rr|:ser]) -- begin transaction"},
    {"commit", cfun_commit, "(commit) -- commit transaction"},
    {"abort", cfun_abort, "(abort) -- abort transaction"},
    {"snapshot", cfun_snapshot, "(snapshot) -- pin/advance statement snapshot"},
    {"stmt-begin", cfun_snapshot, "(stmt-begin) -- alias for (snapshot)"},
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
    {"_table-drop", cfun_table_drop, "internal"},
    {"_seq-create", cfun_seq_create, "internal"},
    {"_seq-drop", cfun_seq_drop, "internal"},
    {"_nextseq", cfun_nextseq, "internal"},
    {"nextseq", cfun_nextseq, "(nextseq name) -- get next sequence value"},
    {"_index-create", cfun_index_create, "internal"},
    {"_index-drop", cfun_index_drop, "internal"},

    {"_insert", cfun_insert, "internal"},
    {"_read", cfun_read, "internal"},
    {"_read-latest", cfun_read_latest, "internal"},
    {"_update", cfun_update, "internal"},
    {"_delete", cfun_delete, "internal"},
    {"_scan", cfun_scan, "internal"},
    {"_idx-scan", cfun_idx_scan, "internal"},

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

(defmacro table-drop [name]
  ~(_table-drop (string ',name)))

(defmacro seq-create [name & rest]
  ~(_seq-create (string ',name) ,;rest))

(defmacro seq-drop [name]
  ~(_seq-drop (string ',name)))

(defmacro nextseq [name]
  ~(_nextseq (string ',name)))

(defmacro index-create [name & body]
  ~(_index-create (string ',name) ',body))

(defmacro index-drop [name]
  ~(_index-drop (string ',name)))

(defmacro insert [tbl row]
  ~(_insert (string ',tbl) ,row))

(defmacro read [tbl rowno]
  ~(_read (string ',tbl) ,rowno))

(defmacro read-latest [tbl rowno]
  ~(_read-latest (string ',tbl) ,rowno))

(defmacro update [tbl rowno row]
  ~(_update (string ',tbl) ,rowno ,row))

(defmacro delete [tbl rowno]
  ~(_delete (string ',tbl) ,rowno))

(defmacro scan [tbl & rest]
  ~(_scan (string ',tbl) ,;rest))

(defmacro idx-scan [idx & rest]
  ~(_idx-scan (string ',idx) ,;rest))

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
