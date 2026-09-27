/*
 * session.cc -- a session and its overlay (CATALOG.md §7): the
 * DDL a transaction makes is kept here, tagged with the savepoint
 * depth it was made at, seen by its own lookups first, undone by
 * rollback and abort, and published into the shared catalog when
 * sm1 has committed.
 */

#include "internal.h"

#include <mutex>

namespace {

/* Drop every change of s made at depth d or deeper. */
void drop_from(sm3::session_t *s, int d) {
  while (!s->tables.empty() && s->tables.back().depth >= d) {
    s->tables.pop_back();
  }
  while (!s->names.empty() && s->names.back().depth >= d) {
    s->names.pop_back();
  }
  while (!s->collations.empty() && s->collations.back().depth >= d) {
    sm2::collation_close(s->collations.back().collation);
    s->collations.pop_back();
  }
}

/*
 * Move every change of s into the shared catalog, then clear them.
 * The caller holds the catalog's mutex exclusive.
 */
void publish(sm3::session_t *s) {
  sm3::catalog_t *cat = s->cat;
  for (sm3::collation_change_t &c : s->collations) {
    cat->collations[c.oid] = c.collation;
  }
  s->collations.clear();
  for (sm3::table_change_t &t : s->tables) {
    if (t.def) {
      cat->tables[t.oid] = std::move(t.def);
    } else {
      cat->tables.erase(t.oid);
    }
  }
  s->tables.clear();
  for (sm3::name_change_t &n : s->names) {
    if (n.oid) {
      cat->table_names[n.name] = *n.oid;
    } else {
      cat->table_names.erase(n.name);
    }
  }
  s->names.clear();
}

} // namespace

namespace sm3 {

/* Open a session; see sm3.h. */
int session_open(catalog_t *cat, sm1::session_t *s, session_t *&out) {
  return catch_nomem([&] {
    if (!cat || !s) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "session_open: no catalog or no sm1 session");
    }
    out = new session_t{s, cat, sm1::xctlevel_t::read_committed, 0, {}, {}, {}};
    return 0;
  });
}

/* Close a session, dropping its overlay; nullptr is a no-op. */
void session_close(session_t *s) {
  if (s == nullptr) {
    return;
  }
  drop_from(s, 0);
  delete s;
}

/* The sm1 session under s. */
sm1::session_t *session_sm1(const session_t *s) { return s->sm1; }

/* Begin a transaction, with an empty overlay. */
int begin(session_t *s, sm1::xctlevel_t level) {
  if (sm1::begin(s->sm1, level) != 0) {
    return -1;
  }
  drop_from(s, 0);
  s->level = level;
  s->depth = 0;
  return 0;
}

/* Set a savepoint; changes from now are made at its depth. */
int savepoint(session_t *s) {
  int d = sm1::savepoint(s->sm1);
  if (d > 0) {
    s->depth = d;
  }
  return d;
}

/* Roll back to depth, dropping the DDL made since, as sm1 does. */
int rollback(session_t *s, int depth) {
  if (sm1::rollback(s->sm1, depth) != 0) {
    return -1;
  }
  // The mark at depth survives; everything made under it goes.
  drop_from(s, depth);
  s->depth = depth;
  return 0;
}

/*
 * Forget the savepoint at depth, keeping its DDL: those changes now
 * belong to the depth outside it, so a later mark that reuses the
 * depth cannot roll them back.
 */
int forget(session_t *s, int depth) {
  if (sm1::forget(s->sm1, depth) != 0) {
    return -1;
  }
  for (table_change_t &c : s->tables) {
    c.depth = std::min(c.depth, depth - 1);
  }
  for (name_change_t &c : s->names) {
    c.depth = std::min(c.depth, depth - 1);
  }
  for (collation_change_t &c : s->collations) {
    c.depth = std::min(c.depth, depth - 1);
  }
  s->depth = depth - 1;
  return 0;
}

/*
 * Commit in sm1 and publish the overlay; a failed commit drops it.
 * With DDL to publish, the shared catalog's mutex is held across
 * both: sm1's commit releases the schema locks, and a transaction
 * that gets one then waits on the mutex to read the definition,
 * so it sees the new one, never the old (CATALOG.md §4).
 */
int commit(session_t *s) {
  if (s->tables.empty() && s->names.empty() && s->collations.empty()) {
    return sm1::commit(s->sm1);
  }
  // Readers of the catalog wait from here until the change is in.
  std::unique_lock lock(s->cat->mu);
  if (sm1::commit(s->sm1) != 0) {
    drop_from(s, 0);
    return -1;
  }
  publish(s);
  return 0;
}

/* Prepare, if s's transaction made no DDL; see sm3.h. */
int prepare(session_t *s, std::string_view gid) {
  if (!s->tables.empty() || !s->names.empty() || !s->collations.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "prepare: a transaction that made DDL cannot be "
                       "prepared");
  }
  return sm1::prepare(s->sm1, gid);
}

/* Abort, dropping the overlay. */
int abort(session_t *s) {
  drop_from(s, 0);
  return sm1::abort(s->sm1);
}

/* Record a table's new definition, or its drop. */
void overlay_table(session_t *s, uint64_t oid,
                   std::shared_ptr<const table_def_t> def) {
  s->tables.push_back({s->depth, oid, std::move(def)});
}

/* Record a table name taken, or freed. */
void overlay_name(session_t *s, uint64_t nspoid, std::string_view name,
                  std::optional<uint64_t> oid) {
  s->names.push_back({s->depth, {nspoid, std::string(name)}, oid});
}

/* Record a collation created, which the overlay now owns. */
void overlay_collation(session_t *s, uint64_t oid,
                       sm2::collation_t *collation) {
  s->collations.push_back({s->depth, oid, collation});
}

/* A collation as s sees it: its own first, then the shared ones. */
bool session_collation(const session_t *s, uint64_t oid,
                       const sm2::collation_t *&out) {
  for (auto it = s->collations.rbegin(); it != s->collations.rend(); ++it) {
    if (it->oid == oid) {
      out = it->collation;
      return true;
    }
  }
  std::shared_lock lock(s->cat->mu);
  auto it = s->cat->collations.find(oid);
  out = it == s->cat->collations.end() ? nullptr : it->second;
  return out != nullptr;
}

/* A table as s sees it: its newest change, else the shared one. */
std::shared_ptr<const table_def_t> cat_table(const session_t *s, uint64_t oid) {
  for (auto it = s->tables.rbegin(); it != s->tables.rend(); ++it) {
    if (it->oid == oid) {
      return it->def;
    }
  }
  return cat_table(s->cat, oid);
}

/* Lock a table's schema, then read its definition; see sm3.h. */
int cat_lock_table(session_t *s, uint64_t oid, sm1::lockmode_t mode,
                   std::shared_ptr<const table_def_t> &out) {
  return catch_nomem([&] {
    sm2::catkv_t *kv = nullptr;
    if (sm2::catkv_open(s->sm1, kv) != 0) {
      return -1;
    }
    int rc = lock_table(kv, oid, mode);
    sm2::catkv_close(kv);
    if (rc != 0) {
      return -1;
    }
    // Under the lock, the definition cannot change until commit;
    // one dropped while this waited is gone.
    out = cat_table(s, oid);
    return out ? 0
               : sm1::seterr(sm1::SM_ERR_NOTFOUND, "table " +
                                                       std::to_string(oid) +
                                                       " does not exist");
  });
}

/* The table names in nspoid as s sees them: its changes over the shared. */
std::vector<std::string> cat_tables(const session_t *s, uint64_t nspoid) {
  std::map<std::string, bool> seen; /* name -> taken */
  for (const name_change_t &c : s->names) {
    if (c.name.first == nspoid) {
      seen[c.name.second] = c.oid.has_value();
    }
  }
  {
    std::shared_lock lock(s->cat->mu);
    for (const auto &[name, oid] : s->cat->table_names) {
      if (name.first == nspoid) {
        seen.emplace(name.second, true);
      }
    }
  }
  std::vector<std::string> out;
  for (const auto &[name, taken] : seen) {
    if (taken) {
      out.push_back(name);
    }
  }
  return out;
}

/* An index by name as s sees it: its tables, overlay first. */
bool cat_index(const session_t *s, uint64_t nspoid, std::string_view name,
               uint64_t &table, uint64_t &index) {
  // The session's own definitions, newest last, over the shared ones.
  std::map<uint64_t, std::shared_ptr<const table_def_t>> seen;
  for (const table_change_t &c : s->tables) {
    seen[c.oid] = c.def;
  }
  {
    std::shared_lock lock(s->cat->mu);
    for (const auto &[oid, def] : s->cat->tables) {
      seen.emplace(oid, def);
    }
  }
  for (const auto &[oid, def] : seen) {
    if (!def || def->nspoid != nspoid) {
      continue;
    }
    for (const index_def_t &x : def->indexes) {
      if (x.name == name) {
        table = oid;
        index = x.oid;
        return true;
      }
    }
  }
  return false;
}

/* A table by name as s sees it: its newest name change first. */
std::shared_ptr<const table_def_t>
cat_table(const session_t *s, uint64_t nspoid, std::string_view name) {
  for (auto it = s->names.rbegin(); it != s->names.rend(); ++it) {
    if (it->name.first == nspoid && it->name.second == name) {
      return it->oid ? cat_table(s, *it->oid) : nullptr;
    }
  }
  uint64_t oid;
  {
    std::shared_lock lock(s->cat->mu);
    auto it = s->cat->table_names.find({nspoid, std::string(name)});
    if (it == s->cat->table_names.end()) {
      return nullptr;
    }
    oid = it->second;
  }
  return cat_table(s, oid);
}

} // namespace sm3
