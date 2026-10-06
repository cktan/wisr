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
  // One more publish; an index it makes valid dates from it.
  uint64_t seq = cat->seq.load() + 1;
  std::vector<std::pair<uint64_t, uint64_t>> to_build; /* table, index */
  for (sm3::table_change_t &t : s->tables) {
    auto old = cat->tables.find(t.oid);
    // An index valid now and not before became valid here; one
    // that is gone needs its count no longer.
    auto was_valid = [&](uint64_t oid) {
      if (old == cat->tables.end()) {
        return false;
      }
      for (const sm3::index_def_t &x : old->second->indexes) {
        if (x.oid == oid) {
          return x.state == sm3::index_state_t::valid;
        }
      }
      return false;
    };
    if (old != cat->tables.end()) {
      for (const sm3::index_def_t &x : old->second->indexes) {
        cat->valid_seq.erase(x.oid);
      }
    }
    if (t.def) {
      for (const sm3::index_def_t &x : t.def->indexes) {
        if (x.state == sm3::index_state_t::valid && !was_valid(x.oid)) {
          cat->valid_seq[x.oid] = seq;
        }
        // A building index is the builder's from this commit on;
        // one queued already is not queued twice.
        if (x.state == sm3::index_state_t::building) {
          to_build.emplace_back(t.oid, x.oid);
        }
      }
      cat->tables[t.oid] = std::move(t.def);
    } else {
      cat->tables.erase(t.oid);
    }
  }
  s->tables.clear();
  cat->seq.store(seq);
  for (const auto &[table, index] : to_build) {
    sm3::build_enqueue(cat, table, index);
  }
  sm3::build_notify(cat);
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
  // Note how far the catalog has got before the snapshot is taken:
  // what was published by then, the snapshot sees whole.
  uint64_t seq = s->cat->seq.load();
  if (sm1::begin(s->sm1, level) != 0) {
    return -1;
  }
  drop_from(s, 0);
  s->level = level;
  s->depth = 0;
  s->snap_seq = seq;
  s->stmt_pinned = false;
  s->in_txn = true;
  return 0;
}

/* Begin a statement; see sm3.h. */
int stmt_begin(session_t *s) {
  // As begin does, and for the same reason; only a READ COMMITTED
  // statement takes a snapshot of its own.
  uint64_t seq = s->cat->seq.load();
  if (sm1::stmt_begin(s->sm1) != 0) {
    return -1;
  }
  if (s->level == sm1::xctlevel_t::read_committed) {
    s->snap_seq = seq;
    s->stmt_pinned = true;
  }
  return 0;
}

/* Begin a statement inside another; see sm3.h. */
int stmt_begin(session_t *s, stmt_t &outer) {
  outer.snap_seq = s->snap_seq;
  outer.pinned = s->stmt_pinned;
  uint64_t seq = s->cat->seq.load();
  if (sm1::stmt_begin(s->sm1, outer.sm1) != 0) {
    return -1;
  }
  if (s->level == sm1::xctlevel_t::read_committed) {
    s->snap_seq = seq;
    s->stmt_pinned = true;
  }
  return 0;
}

/* End a statement begun inside another; see sm3.h. */
int stmt_end(session_t *s, const stmt_t &outer) {
  if (sm1::stmt_end(s->sm1, outer.sm1) != 0) {
    return -1;
  }
  // The outer statement's view of the catalog, with its snapshot.
  s->snap_seq = outer.snap_seq;
  s->stmt_pinned = outer.pinned;
  return 0;
}

/* Whether session may scan an index; see internal.h. */
bool index_usable(const session_t *session, const table_def_t &def,
                  uint64_t oid) {
  const index_def_t *x = nullptr;
  for (const index_def_t &i : def.indexes) {
    if (i.oid == oid) {
      x = &i;
    }
  }
  if (!x || x->state != index_state_t::valid) {
    return false;
  }
  const catalog_t *cat = session->cat;
  std::shared_lock lock(cat->mu);
  // Not valid in the committed catalog: this transaction made it
  // valid, and reads what it wrote.
  auto t = cat->tables.find(def.oid);
  bool committed = false;
  if (t != cat->tables.end()) {
    for (const index_def_t &i : t->second->indexes) {
      committed |= i.oid == oid && i.state == index_state_t::valid;
    }
  }
  if (!committed) {
    return true;
  }
  // READ COMMITTED without statements reads each call at a fresh
  // snapshot, newer than anything the catalog holds.
  if (session->level == sm1::xctlevel_t::read_committed &&
      !session->stmt_pinned) {
    return true;
  }
  // Valid since the catalog was loaded, or since a count this
  // session's snapshot is not older than.
  auto v = cat->valid_seq.find(oid);
  return v == cat->valid_seq.end() || v->second <= session->snap_seq;
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
  // Committed or not, the transaction is over.
  s->in_txn = false;
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
  if (sm1::prepare(s->sm1, gid) != 0) {
    return -1;
  }
  // The prepared transaction is the coordinator's now.
  s->in_txn = false;
  return 0;
}

/* Abort, dropping the overlay. */
int abort(session_t *s) {
  drop_from(s, 0);
  s->in_txn = false;
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
