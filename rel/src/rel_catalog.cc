/*
 * rel_catalog.cc -- session and table catalog tracking for rel.
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
int catalog_t::env_attach(const char *path) {
  if (m_env != nullptr) {
    env_detach(0);
  }
  sm_env_t *env = nullptr;
  // Attach to the storage environment.
  if (sm_attach(path, &env) != 0) {
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
    for (auto &[iname, idx] : sctx.open_indexes) {
      if (idx != nullptr) {
        sm_idx_close(idx);
      }
    }
    sctx.open_indexes.clear();

    for (auto &[tname, rel] : sctx.open_tables) {
      if (rel != nullptr) {
        sm_rel_close(rel);
      }
    }
    sctx.open_tables.clear();

    if (sctx.session != nullptr) {
      sm_closedb(sctx.session);
      sctx.session = nullptr;
    }
  }
  m_sessions.clear();
  m_active_session.clear();
  m_db_tables.clear();

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

  for (auto &[iname, idx] : it->second.open_indexes) {
    if (idx != nullptr) {
      sm_idx_close(idx);
    }
  }
  it->second.open_indexes.clear();

  for (auto &[tname, rel] : it->second.open_tables) {
    if (rel != nullptr) {
      sm_rel_close(rel);
    }
  }
  it->second.open_tables.clear();

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

/* Find a table definition within a database. */
table_def_t *catalog_t::find_table(const std::string &db_name,
                                   const std::string &table_name) {
  auto db_it = m_db_tables.find(db_name);
  if (db_it == m_db_tables.end()) {
    return nullptr;
  }
  auto t_it = db_it->second.find(table_name);
  if (t_it == db_it->second.end()) {
    return nullptr;
  }
  return &t_it->second;
}

/* Add a table definition to a database. */
void catalog_t::add_table(const std::string &db_name, table_def_t tdef) {
  m_db_tables[db_name][tdef.name] = std::move(tdef);
}

/* Remove a table definition from a database. */
void catalog_t::remove_table(const std::string &db_name,
                             const std::string &table_name) {
  // Close open handles across all sessions on this database.
  for (auto &[sname, sctx] : m_sessions) {
    if (sctx.db_name == db_name) {
      auto rel_it = sctx.open_tables.find(table_name);
      if (rel_it != sctx.open_tables.end()) {
        if (rel_it->second != nullptr) {
          sm_rel_close(rel_it->second);
        }
        sctx.open_tables.erase(rel_it);
      }
    }
  }
  auto db_it = m_db_tables.find(db_name);
  if (db_it != m_db_tables.end()) {
    db_it->second.erase(table_name);
  }
}

/* Find an index definition within a database. */
std::pair<table_def_t *, index_def_t *>
catalog_t::find_index(const std::string &db_name, const std::string &idx_name) {
  auto db_it = m_db_tables.find(db_name);
  if (db_it == m_db_tables.end()) {
    return {nullptr, nullptr};
  }
  for (auto &[tname, tdef] : db_it->second) {
    auto i_it = tdef.indexes.find(idx_name);
    if (i_it != tdef.indexes.end()) {
      return {&tdef, &i_it->second};
    }
  }
  return {nullptr, nullptr};
}

/* Remove an index definition from a database. */
void catalog_t::remove_index(const std::string &db_name,
                             const std::string &idx_name) {
  for (auto &[sname, sctx] : m_sessions) {
    if (sctx.db_name == db_name) {
      auto idx_it = sctx.open_indexes.find(idx_name);
      if (idx_it != sctx.open_indexes.end()) {
        if (idx_it->second != nullptr) {
          sm_idx_close(idx_it->second);
        }
        sctx.open_indexes.erase(idx_it);
      }
    }
  }
  auto [p_tdef, p_idef] = find_index(db_name, idx_name);
  if (p_tdef != nullptr) {
    p_tdef->indexes.erase(idx_name);
  }
}

/* The sm_attribute_t schema of a table's columns. */
std::vector<sm_attribute_t> attributes_of(const table_def_t &tdef) {
  std::vector<sm_attribute_t> schema;
  schema.reserve(tdef.columns.size());
  for (const auto &col : tdef.columns) {
    sm_attribute_t attr;
    attr.type = sm_type(col.type_code);
    attr.typmod = col.typmod;
    attr.dropped = false;
    attr.has_missing = false;
    attr.missing = nullptr;
    attr.missing_len = 0;
    schema.push_back(attr);
  }
  return schema;
}

/* Retrieve or lazily open the relation handle for a table on this session. */
sm_rel_t *catalog_t::get_or_open_rel(session_ctx_t *s,
                                     const table_def_t &tdef) {
  auto it = s->open_tables.find(tdef.name);
  if (it != s->open_tables.end() && it->second != nullptr) {
    return it->second;
  }

  std::vector<sm_attribute_t> schema = attributes_of(tdef);

  sm_rel_t *rel = nullptr;
  if (sm_rel_open(s->session, tdef.storeid, 0, schema.data(), schema.size(),
                  tdef.seq_name.c_str(), &rel) != 0) {
    return nullptr;
  }
  s->open_tables[tdef.name] = rel;
  return rel;
}

/* Retrieve or lazily open the index handle for an index on this session. */
sm_idx_t *catalog_t::get_or_open_idx(session_ctx_t *s, const index_def_t &idef,
                                     const table_def_t &tdef) {
  auto it = s->open_indexes.find(idef.name);
  if (it != s->open_indexes.end() && it->second != nullptr) {
    return it->second;
  }

  std::vector<sm_key_column_t> sm_keys;
  sm_keys.reserve(idef.keys.size());
  for (const auto &k : idef.keys) {
    sm_key_column_t sm_k;
    sm_k.type = sm_type(k.type_code);
    sm_k.typmod = k.typmod;
    sm_k.collation = nullptr;
    sm_k.desc = k.desc;
    sm_k.nulls_first = k.nulls_first;
    sm_keys.push_back(sm_k);
  }

  std::vector<sm_attribute_t> sm_includes;
  sm_includes.reserve(idef.includes.size());
  for (const auto &inc : idef.includes) {
    sm_attribute_t sm_inc;
    sm_inc.type = sm_type(inc.type_code);
    sm_inc.typmod = inc.typmod;
    sm_inc.dropped = false;
    sm_inc.has_missing = false;
    sm_inc.missing = nullptr;
    sm_inc.missing_len = 0;
    sm_includes.push_back(sm_inc);
  }

  sm_idx_t *idx = nullptr;
  if (sm_idx_open(s->session, tdef.storeid, idef.prefix, sm_keys.data(),
                  sm_keys.size(), idef.unique,
                  sm_includes.empty() ? nullptr : sm_includes.data(),
                  sm_includes.size(), &idx) != 0) {
    return nullptr;
  }
  s->open_indexes[idef.name] = idx;
  return idx;
}

/* Reset all environment and session state. */
void catalog_t::reset() { env_detach(1); }

/* Return the process-wide singleton catalog. */
catalog_t &get_catalog() {
  static catalog_t s_catalog;
  return s_catalog;
}

} // namespace rel
