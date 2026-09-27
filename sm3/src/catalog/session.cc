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

/* Move every change of s into the shared catalog, then clear them. */
void publish(sm3::session_t *s) {
  sm3::catalog_t *cat = s->cat;
  std::unique_lock lock(cat->mu);
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
    out = new session_t{s, cat, 0, {}, {}, {}};
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

/* Commit in sm1, then publish the overlay; a failed commit drops it. */
int commit(session_t *s) {
  if (sm1::commit(s->sm1) != 0) {
    drop_from(s, 0);
    return -1;
  }
  publish(s);
  return 0;
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
