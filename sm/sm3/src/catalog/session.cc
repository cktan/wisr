/*
 * session.cc -- a session and its overlay (CATALOG.md §7): the
 * DDL a transaction makes is kept here, tagged with the savepoint
 * depth it was made at, seen by its own lookups first, undone by
 * rollback and abort, and published into the shared catalog when
 * sm1 has committed.
 */

#include "internal.h"

#include <algorithm>
#include <map>
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
  while (!s->nspaces.empty() && s->nspaces.back().depth >= d) {
    s->nspaces.pop_back();
  }
  while (!s->states.empty() && s->states.back().depth >= d) {
    s->states.pop_back();
  }
  while (!s->sequences.empty() && s->sequences.back().depth >= d) {
    s->sequences.pop_back();
  }
  while (!s->stats.empty() && s->stats.back().depth >= d) {
    s->stats.pop_back();
  }
}

/* Whether s's transaction has made any DDL. */
bool made_ddl(const sm3::session_t *s) {
  return !s->tables.empty() || !s->names.empty() || !s->collations.empty() ||
         !s->nspaces.empty() || !s->states.empty() || !s->sequences.empty() ||
         !s->stats.empty();
}

/* Whether a name change is of a temporary table's name. */
bool temp_name(const sm3::name_change_t &c) {
  return c.name.first == sm3::nsp_temp;
}

/*
 * Whether s's transaction has made DDL the shared catalog is to
 * learn of: any but a temporary table's.
 */
bool made_shared_ddl(const sm3::session_t *s) {
  auto shared = [](const auto &c) { return !c.temp; };
  return std::any_of(s->tables.begin(), s->tables.end(), shared) ||
         !std::all_of(s->names.begin(), s->names.end(), temp_name) ||
         std::any_of(s->states.begin(), s->states.end(), shared) ||
         !s->collations.empty() || !s->nspaces.empty() ||
         !s->sequences.empty() || !s->stats.empty();
}

/*
 * Move s's changes to temporary tables into its temporary catalog,
 * in the order they were made, and take them out of the overlay:
 * the shared catalog never learns of them (TEMPTABLE.md §4).
 */
void publish_temp(sm3::session_t *s) {
  for (const sm3::state_change_t &c : s->states) {
    if (!c.temp) {
      continue;
    }
    if (c.status) {
      s->temp_states[c.index] = *c.status;
    } else {
      s->temp_states.erase(c.index);
    }
  }
  std::erase_if(s->states, [](const auto &c) { return c.temp; });
  for (sm3::table_change_t &t : s->tables) {
    if (!t.temp) {
      continue;
    }
    if (t.def) {
      s->temp_tables[t.oid] = std::move(t.def);
    } else {
      s->temp_tables.erase(t.oid);
    }
  }
  std::erase_if(s->tables, [](const auto &c) { return c.temp; });
  for (const sm3::name_change_t &n : s->names) {
    if (!temp_name(n)) {
      continue;
    }
    if (n.oid) {
      s->temp_names[n.name.second] = *n.oid;
    } else {
      s->temp_names.erase(n.name.second);
    }
  }
  std::erase_if(s->names, temp_name);
}

/*
 * Move every change of s into the shared catalog, then clear them.
 * The caller holds the catalog's mutex exclusive.
 */
void publish(sm3::session_t *s) {
  sm3::catalog_t *cat = s->cat;
  // One more publish: what it changes is dated from it.
  uint64_t seq = cat->seq.load() + 1;
  for (sm3::collation_change_t &c : s->collations) {
    cat->collations[c.oid] = c.collation;
    cat->collation_nsp[c.oid] = c.nspoid;
  }
  s->collations.clear();
  for (sm3::nspace_change_t &n : s->nspaces) {
    if (n.name) {
      cat->nspaces[n.oid] = std::move(*n.name);
    } else {
      cat->nspaces.erase(n.oid);
    }
  }
  if (!s->nspaces.empty()) {
    cat->nsp_seq.store(seq);
  }
  s->nspaces.clear();
  for (sm3::sequence_change_t &q : s->sequences) {
    if (q.nspoid) {
      cat->sequence_nsp[q.oid] = *q.nspoid;
    } else {
      cat->sequence_nsp.erase(q.oid);
    }
  }
  s->sequences.clear();
  for (sm3::stats_change_t &c : s->stats) {
    if (c.stats) {
      cat->stats[c.oid] = std::move(c.stats);
      cat->stats_seq[c.oid] = seq;
    } else {
      cat->stats.erase(c.oid);
      cat->stats_seq.erase(c.oid);
    }
  }
  s->stats.clear();
  for (sm3::state_change_t &c : s->states) {
    auto old = cat->index_states.find(c.index);
    bool was_valid = old != cat->index_states.end() &&
                     old->second.state == sm3::index_state_t::valid;
    if (!c.status) {
      // The index has gone, and its count with it.
      cat->index_states.erase(c.index);
      cat->valid_seq.erase(c.index);
      continue;
    }
    // An index valid now and not before became valid here.
    if (c.status->state == sm3::index_state_t::valid && !was_valid) {
      cat->valid_seq[c.index] = seq;
    }
    cat->index_states[c.index] = std::move(*c.status);
  }
  s->states.clear();
  for (sm3::table_change_t &t : s->tables) {
    if (t.def) {
      cat->tables[t.oid] = std::move(t.def);
      cat->table_seq[t.oid] = seq;
    } else {
      cat->tables.erase(t.oid);
      cat->table_seq.erase(t.oid);
    }
  }
  s->tables.clear();
  cat->seq.store(seq);
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
  if (!cat || !s) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "session_open: no catalog or no sm1 session");
  }
  out = new session_t{};
  out->sm1 = s;
  out->cat = cat;
  out->level = sm1::xctlevel_t::read_committed;
  return 0;
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

/* The catalog s reads. */
catalog_t *session_cat(const session_t *s) { return s->cat; }

/* Begin a transaction, with an empty overlay. */
int begin(session_t *s, sm1::xctlevel_t level, bool read_only) {
  // Note how far the catalog has got before the snapshot is taken:
  // what was published by then, the snapshot sees whole.
  uint64_t seq = s->cat->seq.load();
  if (sm1::begin(s->sm1, level, read_only) != 0) {
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
  if (!x || cat_get_index_state(session, oid).state != index_state_t::valid) {
    return false;
  }
  const catalog_t *cat = session->cat;
  std::shared_lock lock(cat->mu);
  // Not valid in the committed catalog: this transaction made it
  // valid, and reads what it wrote.
  auto c = cat->index_states.find(oid);
  if (c == cat->index_states.end() || c->second.state != index_state_t::valid) {
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
  for (nspace_change_t &c : s->nspaces) {
    c.depth = std::min(c.depth, depth - 1);
  }
  for (state_change_t &c : s->states) {
    c.depth = std::min(c.depth, depth - 1);
  }
  for (sequence_change_t &c : s->sequences) {
    c.depth = std::min(c.depth, depth - 1);
  }
  for (stats_change_t &c : s->stats) {
    c.depth = std::min(c.depth, depth - 1);
  }
  s->depth = depth - 1;
  return 0;
}

/*
 * Commit in sm1 and publish the overlay; a failed commit drops it.
 * With DDL to publish, the shared catalog's mutex is held across
 * both: sm1's commit releases the table-oid locks, and a transaction
 * that gets one then waits on the mutex to read the definition,
 * so it sees the new one, never the old (CATALOG.md §4).
 */
int commit(session_t *s) {
  // Committed or not, the transaction is over.
  s->in_txn = false;
  if (!made_shared_ddl(s)) {
    // Nothing for the shared catalog: what there is, is the session's.
    if (sm1::commit(s->sm1) != 0) {
      drop_from(s, 0);
      return -1;
    }
    publish_temp(s);
    return 0;
  }
  // Readers of the catalog wait from here until the change is in.
  std::unique_lock lock(s->cat->mu);
  if (sm1::commit(s->sm1) != 0) {
    drop_from(s, 0);
    return -1;
  }
  publish_temp(s);
  publish(s);
  return 0;
}

/* Prepare, if s's transaction made no DDL; see sm3.h. */
int prepare(session_t *s, std::string_view gid) {
  if (made_ddl(s)) {
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
                   std::shared_ptr<const table_def_t> def, bool temp) {
  s->tables.push_back({s->depth, oid, std::move(def), temp});
}

/* Record a table name taken, or freed. */
void overlay_name(session_t *s, uint64_t nspoid, std::string_view name,
                  std::optional<uint64_t> oid) {
  s->names.push_back({s->depth, {nspoid, std::string(name)}, oid});
}

/* Record a collation created, which the overlay now owns. */
void overlay_collation(session_t *s, uint64_t oid, uint64_t nspoid,
                       sm2::collation_t *collation) {
  s->collations.push_back({s->depth, oid, nspoid, collation});
}

/* Record a table's new statistics, or their going with the table. */
void overlay_stats(session_t *s, uint64_t oid,
                   std::shared_ptr<const stats_t> stats) {
  s->stats.push_back({s->depth, oid, std::move(stats)});
}

/* Record a sequence created in nspoid, or dropped. */
void overlay_sequence(session_t *s, uint64_t oid,
                      std::optional<uint64_t> nspoid) {
  s->sequences.push_back({s->depth, oid, nspoid});
}

/* Record an index's state set, or its going with its index. */
void overlay_state(session_t *s, uint64_t index,
                   std::optional<index_status_t> status, bool temp) {
  s->states.push_back({s->depth, index, std::move(status), temp});
}

/* An index's committed state; see sm3.h. */
index_status_t cat_get_index_state(const catalog_t *cat, uint64_t index_oid) {
  std::shared_lock lock(cat->mu);
  auto it = cat->index_states.find(index_oid);
  return it == cat->index_states.end() ? index_status_t{} : it->second;
}

/* An index's state as s sees it: its own change first; see sm3.h. */
index_status_t cat_get_index_state(const session_t *s, uint64_t index_oid) {
  for (auto it = s->states.rbegin(); it != s->states.rend(); ++it) {
    if (it->index == index_oid) {
      return it->status ? *it->status : index_status_t{};
    }
  }
  // Its own temporary table's index, which the catalog never has.
  auto own = s->temp_states.find(index_oid);
  if (own != s->temp_states.end()) {
    return own->second;
  }
  return cat_get_index_state(s->cat, index_oid);
}

/* Record a namespace created, or dropped. */
void overlay_nspace(session_t *s, uint64_t oid,
                    std::optional<std::string_view> name) {
  s->nspaces.push_back(
      {s->depth, oid, name ? std::optional<std::string>(*name) : std::nullopt});
}

/*
 * The namespaces as s sees them, oid to name: the shared ones with
 * its own changes over them, the newest of each winning.
 */
static std::map<uint64_t, std::string> nspaces_of(const session_t *s) {
  std::map<uint64_t, std::string> out;
  {
    const catalog_t *cat = s->cat;
    std::shared_lock lock(cat->mu);
    out.insert(cat->nspaces.begin(), cat->nspaces.end());
  }
  for (const nspace_change_t &c : s->nspaces) {
    if (c.name) {
      out[c.oid] = *c.name;
    } else {
      out.erase(c.oid);
    }
  }
  return out;
}

/* The temporary tables as s sees them; see internal.h. */
std::map<uint64_t, std::shared_ptr<const table_def_t>>
temp_tables_of(const session_t *s) {
  std::map<uint64_t, std::shared_ptr<const table_def_t>> out(
      s->temp_tables.begin(), s->temp_tables.end());
  for (const table_change_t &c : s->tables) {
    if (!c.temp) {
      continue;
    }
    if (c.def) {
      out[c.oid] = c.def;
    } else {
      out.erase(c.oid);
    }
  }
  return out;
}

/* A namespace by name as s sees it; see sm3.h. */
bool cat_get_namespace(const session_t *s, std::string_view name,
                       uint64_t &oid) {
  // Few namespaces, and no map by name: look at each.
  for (const auto &[o, n] : nspaces_of(s)) {
    if (n == name) {
      oid = o;
      return true;
    }
  }
  return false;
}

/* The namespaces' names as s sees them, sorted; see sm3.h. */
std::vector<std::string> cat_list_namespaces(const session_t *s) {
  std::vector<std::string> out;
  for (const auto &[oid, name] : nspaces_of(s)) {
    out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

/* A namespace by oid as s sees it; see sm3.h. */
bool cat_get_namespace_name(const session_t *s, uint64_t oid,
                            std::string &name) {
  std::map<uint64_t, std::string> all = nspaces_of(s);
  auto it = all.find(oid);
  if (it == all.end()) {
    return false;
  }
  name = it->second;
  return true;
}

/* Whether a namespace holds anything, as s sees it; see internal.h. */
bool nspace_in_use(const session_t *s, uint64_t oid) {
  // Its own collations, and its own tables as they now stand.
  for (const collation_change_t &c : s->collations) {
    if (c.nspoid == oid) {
      return true;
    }
  }
  std::map<uint64_t, bool> own; /* table oid -> in the namespace */
  for (const table_change_t &c : s->tables) {
    own[c.oid] = c.def && c.def->nspoid == oid;
  }
  for (const auto &[t, in] : own) {
    if (in) {
      return true;
    }
  }
  // Its own sequences as they now stand, the last change to each.
  std::map<uint64_t, bool> own_seq; /* sequence oid -> in the namespace */
  for (const sequence_change_t &c : s->sequences) {
    own_seq[c.oid] = c.nspoid && *c.nspoid == oid;
  }
  for (const auto &[q, in] : own_seq) {
    if (in) {
      return true;
    }
  }
  // The shared ones, but for the tables and sequences it has changed.
  const catalog_t *cat = s->cat;
  std::shared_lock lock(cat->mu);
  for (const auto &[c, nsp] : cat->collation_nsp) {
    if (nsp == oid) {
      return true;
    }
  }
  for (const auto &[q, nsp] : cat->sequence_nsp) {
    if (nsp == oid && !own_seq.count(q)) {
      return true;
    }
  }
  for (const auto &[t, def] : cat->tables) {
    if (def->nspoid == oid && !own.count(t)) {
      return true;
    }
  }
  return false;
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
std::shared_ptr<const table_def_t> cat_get_table(const session_t *s,
                                                 uint64_t oid) {
  for (auto it = s->tables.rbegin(); it != s->tables.rend(); ++it) {
    if (it->oid == oid) {
      return it->def;
    }
  }
  // Its own temporary table, which the catalog never has.
  auto own = s->temp_tables.find(oid);
  if (own != s->temp_tables.end()) {
    return own->second;
  }
  return cat_get_table(s->cat, oid);
}

namespace {

/*
 * Lock a table's schema, waiting for it or not, then read its
 * definition: 0, or 1 with nothing taken if it would have waited, or
 * -1.
 */
int lock_and_get_table(session_t *s, uint64_t oid, sm1::lockmode_t mode,
                       bool wait, std::shared_ptr<const table_def_t> &out) {
  // A temporary table is the session's alone: no one to lock out.
  out = cat_get_table(s, oid);
  if (out && out->nspoid == nsp_temp) {
    return 0;
  }
  int rc =
      wait ? lock_table(s->sm1, oid, mode) : lock_table_now(s->sm1, oid, mode);
  if (rc != 0) {
    return rc;
  }
  // Under the lock, the definition cannot change until commit;
  // one dropped while this waited is gone.
  out = cat_get_table(s, oid);
  return out ? 0
             : sm1::seterr(sm1::SM_ERR_NOTFOUND,
                           "table " + std::to_string(oid) + " does not exist");
}

} // namespace

/* A table's version as s sees it; see sm3.h. */
uint64_t cat_table_version(const session_t *s, uint64_t oid) {
  // This transaction's own change of it, not yet published.
  for (const table_change_t &t : s->tables) {
    if (t.oid == oid) {
      return table_version_own;
    }
  }
  for (const stats_change_t &c : s->stats) {
    if (c.oid == oid) {
      return table_version_own;
    }
  }
  catalog_t *cat = s->cat;
  std::shared_lock lock(cat->mu);
  auto count = [](const std::unordered_map<uint64_t, uint64_t> &m,
                  uint64_t key) {
    auto it = m.find(key);
    return it == m.end() ? uint64_t{0} : it->second;
  };
  uint64_t v = std::max(count(cat->table_seq, oid), count(cat->stats_seq, oid));
  // An index of it made valid since: a plan may now use it.
  auto def = cat->tables.find(oid);
  if (def != cat->tables.end()) {
    for (const index_def_t &x : def->second->indexes) {
      v = std::max(v, count(cat->valid_seq, x.oid));
    }
  }
  return v;
}

/* The namespaces' epoch; see sm3.h. */
uint64_t cat_namespace_epoch(const session_t *s) {
  return s->cat->nsp_seq.load();
}

/* Lock a table's schema, then read its definition; see sm3.h. */
int cat_lock_table(session_t *s, uint64_t oid, sm1::lockmode_t mode,
                   std::shared_ptr<const table_def_t> &out) {
  return lock_and_get_table(s, oid, mode, true, out);
}

/* As cat_lock_table, if the lock is free now; see sm3.h. */
int cat_trylock_table(session_t *s, uint64_t oid, sm1::lockmode_t mode,
                      std::shared_ptr<const table_def_t> &out) {
  return lock_and_get_table(s, oid, mode, false, out);
}

/* The table names in nspoid as s sees them: its changes over the shared. */
std::vector<std::string> cat_list_tables(const session_t *s, uint64_t nspoid) {
  std::map<std::string, bool> seen; /* name -> taken */
  for (const name_change_t &c : s->names) {
    if (c.name.first == nspoid) {
      seen[c.name.second] = c.oid.has_value();
    }
  }
  if (nspoid == nsp_temp) {
    // Its own temporary tables: the catalog has none.
    for (const auto &[name, oid] : s->temp_names) {
      seen.emplace(name, true);
    }
  } else {
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
bool cat_get_index(const session_t *s, uint64_t nspoid, std::string_view name,
                   uint64_t &table, uint64_t &index) {
  // The session's own definitions, newest last, over the shared ones.
  std::map<uint64_t, std::shared_ptr<const table_def_t>> seen;
  for (const table_change_t &c : s->tables) {
    seen[c.oid] = c.def;
  }
  // Its own temporary tables, then the shared ones.
  for (const auto &[oid, def] : s->temp_tables) {
    seen.emplace(oid, def);
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
cat_get_table(const session_t *s, uint64_t nspoid, std::string_view name) {
  for (auto it = s->names.rbegin(); it != s->names.rend(); ++it) {
    if (it->name.first == nspoid && it->name.second == name) {
      return it->oid ? cat_get_table(s, *it->oid) : nullptr;
    }
  }
  uint64_t oid;
  if (nspoid == nsp_temp) {
    // Its own temporary tables: the catalog has none.
    auto it = s->temp_names.find(std::string(name));
    if (it == s->temp_names.end()) {
      return nullptr;
    }
    oid = it->second;
  } else {
    std::shared_lock lock(s->cat->mu);
    auto it = s->cat->table_names.find({nspoid, std::string(name)});
    if (it == s->cat->table_names.end()) {
      return nullptr;
    }
    oid = it->second;
  }
  return cat_get_table(s, oid);
}

} // namespace sm3
