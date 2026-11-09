/*
 * rel_catalog.cc -- rel's sessions, and the tables a statement opens
 * from sm3's catalog.
 */

#include "rel.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace rel {

/* Construct an empty catalog. */
catalog_t::catalog_t() : m_env(nullptr), m_env_path(""), m_active_session("") {}

/* Destroy catalog and release any dangling resources. */
catalog_t::~catalog_t() { reset(); }

/* Initialize a wisr home directory. */
int catalog_t::env_init(const char *path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  // sm_init formats the target directory as a wisr environment.
  return sm_init(path);
}

/* Attach an existing home directory. */
int catalog_t::env_attach(const char *path, bool toast,
                          const sm_env_options_t *opt) {
  if (m_env != nullptr) {
    env_detach(0);
  }
  sm_env_t *env = nullptr;
  // Attach to the storage environment, its block cache as opt sizes it.
  if (sm_attach(path, &env, opt) != 0) {
    return -1;
  }
  // rel evaluates its own partial and expression indexes; the
  // evaluator must be in place before a database is opened.
  // And whether long values go out of line, likewise.
  // And no btree reaper thread: a script counts a tree's entries, which a
  // tick would change, so it runs a pass by (btree-reaper-pass).
  if (sm_set_index_evaluator(env, index_eval, nullptr) != 0 ||
      sm_set_toast(env, toast) != 0 ||
      sm_set_btree_reaper_workers(env, 0) != 0) {
    sm_detach(env, 1);
    return -1;
  }
  m_env = env;
  m_env_path = path;
  return 0;
}

/* Detach the current home directory. */
int catalog_t::env_detach(int force) {
  if (m_env == nullptr) {
    return 0;
  }
  // Close all open tables and sessions before detaching.
  for (auto &[sname, sctx] : m_sessions) {
    end_cursors(&sctx);
    sctx.stmt_tables.clear();
    // Free the statements a script put aside and never ended.
    for (sm_stmt_t *outer : sctx.outer_stmts) {
      sm_stmt_end(sctx.session, outer);
    }
    sctx.outer_stmts.clear();
    for (auto &[handle, view] : sctx.views) {
      sm_stmt_free(view);
    }
    sctx.views.clear();

    if (sctx.session != nullptr) {
      sm_closedb(sctx.session);
      sctx.session = nullptr;
    }
  }
  m_sessions.clear();
  m_active_session.clear();

  // Detach from wisr environment.
  int rc = sm_detach(m_env, force);
  m_env = nullptr;
  m_env_path.clear();
  return rc;
}

/* Open a new named session on a database. */
int catalog_t::session_open(const std::string &name, const std::string &db_name,
                            const sm_options_t *opt) {
  if (m_env == nullptr) {
    return -1;
  }
  // If a session with this name is already open, close it first.
  if (m_sessions.find(name) != m_sessions.end()) {
    session_close(name);
  }

  sm_session_t *s = nullptr;
  // Open a new database session handle.
  if (sm_opendb(m_env, db_name.c_str(), opt, &s) != 0) {
    return -1;
  }

  session_ctx_t ctx;
  ctx.name = name;
  ctx.db_name = db_name;
  ctx.session = s;
  m_sessions[name] = std::move(ctx);

  if (m_active_session.empty()) {
    m_active_session = name;
  }
  return 0;
}

/* Switch the currently active session. */
int catalog_t::session_use(const std::string &name) {
  if (m_sessions.find(name) == m_sessions.end()) {
    return -1;
  }
  m_active_session = name;
  return 0;
}

/* Close a session and release its handles. */
int catalog_t::session_close(const std::string &name) {
  auto it = m_sessions.find(name);
  if (it == m_sessions.end()) {
    return 0;
  }

  end_cursors(&it->second);
  it->second.stmt_tables.clear();
  // Free the statements a script put aside and never ended.
  for (sm_stmt_t *outer : it->second.outer_stmts) {
    sm_stmt_end(it->second.session, outer);
  }
  it->second.outer_stmts.clear();
  for (auto &[handle, view] : it->second.views) {
    sm_stmt_free(view);
  }
  it->second.views.clear();

  if (it->second.session != nullptr) {
    sm_closedb(it->second.session);
  }

  m_sessions.erase(it);
  if (m_active_session == name) {
    m_active_session = m_sessions.empty() ? "" : m_sessions.begin()->first;
  }
  return 0;
}

/* Retrieve the active session context. */
session_ctx_t *catalog_t::active_session() {
  if (m_active_session.empty()) {
    return nullptr;
  }
  return get_session(m_active_session);
}

/* Retrieve a specific session context by name. */
session_ctx_t *catalog_t::get_session(const std::string &name) {
  auto it = m_sessions.find(name);
  if (it == m_sessions.end()) {
    return nullptr;
  }
  return &it->second;
}

/* Close a statement's table: its handles, then its definition. */
table_def_t::~table_def_t() { sm_tbl_close(tbl); }

/* End the cursor; its table handle closes after, as a member. */
cursor_t::~cursor_t() { sm_idx_scanend(scan); }

/* End every cursor s has open, before its transaction ends. */
void end_cursors(session_ctx_t *s) { s->cursors.clear(); }

/* Forget every cursor s has open, which its transaction's abort closed. */
void forget_cursors(session_ctx_t *s) {
  for (auto &[handle, c] : s->cursors) {
    // sm closed it; ending it again would free it twice. Its
    // handle's few bytes are left.
    c->scan = nullptr;
  }
  s->cursors.clear();
}

/* rel's view of the open table tbl, which it takes over. */
static std::unique_ptr<table_def_t> view_of(const std::string &name,
                                            sm_tbl_t *tbl) {
  auto t = std::make_unique<table_def_t>();
  const sm_table_def_t *def = sm_tbl_def(tbl);
  t->name = name;
  t->tbl = tbl;
  t->def = def;
  for (const sm_column_def_t &c : def->columns) {
    t->columns.push_back(
        {c.name, c.type_code, c.typmod, c.not_null, std::nullopt});
  }
  for (const sm_index_def_t &x : def->indexes) {
    index_def_t idef;
    idef.name = x.name;
    idef.unique = x.unique;
    idef.def = &x;
    // An expression key column is of a column the blob names.
    index_eval_t ev;
    parse_index_eval(x.eval, ev);
    size_t e = 0;
    for (size_t j = 0; j < x.key.size(); j++) {
      bool expr = x.key_attnums[j] < 0 && e < ev.exprs.size();
      int32_t att = expr ? ev.exprs[e].second : x.key_attnums[j];
      const col_def_t &c = t->columns[att];
      key_col_def_t k;
      if (expr) {
        k.expr = ev.exprs[e++].first;
      }
      k.col_idx = att;
      k.type_code = c.type_code;
      k.typmod = c.typmod;
      k.collation = nullptr;
      k.desc = x.key[j].desc;
      k.nulls_first = x.key[j].nulls_first;
      idef.keys.push_back(std::move(k));
    }
    for (int32_t att : x.include_attnums) {
      const col_def_t &c = t->columns[att];
      idef.includes.push_back(
          {c.name, static_cast<size_t>(att), c.type_code, c.typmod});
    }
    t->indexes[idef.name] = std::move(idef);
  }
  return t;
}

/* Lock the table oid in mode, and keep rel's view of it in s. */
static table_def_t *lock_into(session_ctx_t *s, const std::string &name,
                              uint64_t oid, sm_lockmode_t mode) {
  sm_tbl_t *tbl = nullptr;
  // The table-oid lock, held to the end of the transaction, with the
  // table's heap and indexes opened under it.
  if (sm_tbl_open(s->session, oid, mode, &tbl) != 0) {
    return nullptr;
  }
  if (nullptr == sm_tbl_def(tbl)) {
    sm_tbl_close(tbl);
    return nullptr;
  }
  s->stmt_tables.push_back(view_of(name, tbl));
  return s->stmt_tables.back().get();
}

/* Open the table name, temporary or in public; nullptr if none. */
table_def_t *catalog_t::open_table(session_ctx_t *s, const std::string &name,
                                   sm_lockmode_t mode) {
  uint64_t oid = 0;
  if (rel_table_oid(s, name, &oid) != 0) {
    return nullptr;
  }
  return lock_into(s, name, oid, mode);
}

/* Open the index name's table, locked in mode, and find the index. */
std::pair<table_def_t *, index_def_t *>
catalog_t::open_index(session_ctx_t *s, const std::string &name,
                      sm_lockmode_t mode) {
  uint64_t table = 0, index = 0;
  if (rel_index_oid(s, name, &table, &index) != 0) {
    return {nullptr, nullptr};
  }
  table_def_t *t = lock_into(s, "", table, mode);
  if (t == nullptr) {
    return {nullptr, nullptr};
  }
  t->name = t->def->name;
  auto it = t->indexes.find(name);
  if (it == t->indexes.end()) {
    return {nullptr, nullptr};
  }
  return {t, &it->second};
}

/* Reset all environment and session state. */
void catalog_t::reset() { env_detach(1); }

/* Return the process-wide singleton catalog. */
catalog_t &get_catalog() {
  static catalog_t s_catalog;
  return s_catalog;
}

} // namespace rel
