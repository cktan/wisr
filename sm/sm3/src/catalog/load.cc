/*
 * load.cc -- cat_open: the catalog read into memory with one scan
 * of each kind (CATALOG_INMEM.md §1, §2). Every definition is built
 * whole, its sm2 schema and layout with it, so a reader never lays
 * out a row itself.
 */

#include "internal.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace {

using kv_ptr = std::unique_ptr<sm2::catkv_t, decltype(&sm2::catkv_close)>;

/* Fail with SM_ERR_CORRUPT: the catalog contradicts itself. */
int corrupt(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "catalog: " + why);
}

/*
 * Every entry of kind, as its oid and its value, copied: a
 * cursor's views last only to its next call.
 */
int scan_kind(sm2::catkv_t *kv, sm3::kind_t kind,
              std::vector<std::pair<uint64_t, std::string>> &out) {
  std::string prefix;
  sm2::catkv_scan_t *c = nullptr;
  if (sm3::kind_prefix(kind, prefix) != 0 ||
      sm2::catkv_scan(kv, prefix, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::catkv_scan_t, decltype(&sm2::catkv_scanend)> guard(
      c, &sm2::catkv_scanend);
  std::string_view k, v;
  int rc;
  while ((rc = sm2::catkv_scannxt(c, k, v)) == 0) {
    uint64_t oid;
    if (sm3::entity_oid(kind, k, oid) != 0) {
      return -1;
    }
    out.emplace_back(oid, std::string(v));
  }
  return rc == 1 ? 0 : -1;
}

/*
 * Every part of kind, in key order, handed to f as its table's
 * oid, its place or oid, and its value, which lasts only for the
 * call.
 */
template <typename F> int scan_parts(sm2::catkv_t *kv, sm3::kind_t kind, F f) {
  std::string prefix;
  sm2::catkv_scan_t *c = nullptr;
  if (sm3::kind_prefix(kind, prefix) != 0 ||
      sm2::catkv_scan(kv, prefix, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::catkv_scan_t, decltype(&sm2::catkv_scanend)> guard(
      c, &sm2::catkv_scanend);
  std::string_view k, v;
  int rc;
  while ((rc = sm2::catkv_scannxt(c, k, v)) == 0) {
    uint64_t table, n;
    if (sm3::part_ids(kind, k, table, n) != 0 || f(table, n, v) != 0) {
      return -1;
    }
  }
  return rc == 1 ? 0 : -1;
}

/*
 * Set cat's collation_nsp, sequence_by_oid and sequence_names from
 * the NAME entries, in one scan of them.
 */
int member_nspaces(sm2::catkv_t *kv, sm3::catalog_t &cat) {
  std::string prefix;
  sm2::catkv_scan_t *c = nullptr;
  if (sm3::kind_prefix(sm3::kind_t::name, prefix) != 0 ||
      sm2::catkv_scan(kv, prefix, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::catkv_scan_t, decltype(&sm2::catkv_scanend)> guard(
      c, &sm2::catkv_scanend);
  std::string_view k, v;
  int rc;
  while ((rc = sm2::catkv_scannxt(c, k, v)) == 0) {
    sm3::name_rec_t rec;
    uint64_t nspoid;
    std::string name;
    if (sm3::decode_name(v, rec) != 0 ||
        sm3::name_nspoid(k, nspoid, name) != 0) {
      return -1;
    }
    if (rec.kind == sm3::kind_t::collation) {
      cat.collation_nsp[rec.oid] = nspoid;
    } else if (rec.kind == sm3::kind_t::sequence) {
      cat.sequence_by_oid[rec.oid] = {nspoid, name};
      cat.sequence_names[{nspoid, std::move(name)}] = rec.oid;
    }
  }
  return rc == 1 ? 0 : -1;
}

/* The collation oid names, or nullptr for none; false if unknown. */
bool collation_of(const sm3::collation_lookup_t &lookup,
                  std::optional<uint64_t> oid, const sm2::collation_t *&out) {
  out = nullptr;
  return !oid || lookup(*oid, out);
}

} // namespace

namespace sm3 {

/* Build a table's definition from its record; see internal.h. */
int build_table(const collation_lookup_t &lookup, uint64_t oid,
                const table_rec_t &rec,
                std::shared_ptr<const table_def_t> &out) {
  auto t = std::make_shared<sm3::table_def_t>();
  t->oid = oid;
  t->nspoid = rec.nspoid;
  t->name = rec.name;
  t->storeid = rec.storeid;
  t->id_seq = rec.id_seq;
  // Named once here, not at every open of the table.
  t->toast_seq = toast_seq_name(oid);
  t->next_relid = rec.next_relid;
  t->rec = rec;
  for (const sm3::column_rec_t &c : rec.columns) {
    const sm2::type_t *type = sm2::type_by_code(c.type_code);
    const sm2::collation_t *coll;
    if (!type || !collation_of(lookup, c.collation, coll)) {
      return corrupt("column " + c.name + " of " + rec.name +
                     " names an unknown type or collation");
    }
    t->columns.push_back({c.name, type, c.typmod, c.not_null, c.dropped,
                          c.missing, coll, c.default_expr, c.identity,
                          c.identity_seq, c.on_update_expr});
    t->schema.push_back({type, c.typmod, c.dropped, c.missing});
  }
  // Lay the rows out once, for every reader of the table.
  t->desc = sm2::tup_desc_t(t->schema);
  if (t->desc.bad != SIZE_MAX) {
    return corrupt("a column of " + rec.name + " sm2 cannot lay out");
  }
  for (const sm3::index_rec_t &x : rec.indexes) {
    sm3::index_def_t d;
    d.oid = x.oid;
    d.name = x.name;
    d.relid = x.relid;
    d.unique = x.unique;
    d.primary = x.primary;
    d.constraint = x.constraint;
    d.eval = x.eval;
    for (const sm3::index_key_rec_t &k : x.keys) {
      const sm2::collation_t *coll;
      if (k.attnum < -1 ||
          static_cast<int64_t>(k.attnum) >=
              static_cast<int64_t>(t->columns.size()) ||
          !collation_of(lookup, k.collation, coll)) {
        return corrupt("index " + x.name + " names an unknown column");
      }
      d.key_attnums.push_back(k.attnum);
      // An expression gives its own type; a column gives its column's.
      if (k.attnum < 0) {
        const sm2::type_t *type = sm2::type_by_code(k.type_code);
        if (!type) {
          return corrupt("index " + x.name + " has an expression of no type");
        }
        d.key.push_back({type, k.typmod, coll, k.desc, k.nulls_first});
        continue;
      }
      const sm3::column_def_t &col = t->columns[k.attnum];
      d.key.push_back({col.type, col.typmod, coll, k.desc, k.nulls_first});
    }
    for (int32_t a : x.reads) {
      if (a < 0 || static_cast<size_t>(a) >= t->columns.size()) {
        return corrupt("index " + x.name + " reads an unknown column");
      }
      d.reads.push_back(a);
    }
    for (int32_t a : x.includes) {
      if (a < 0 || static_cast<size_t>(a) >= t->columns.size()) {
        return corrupt("index " + x.name + " includes an unknown column");
      }
      const sm3::column_def_t &col = t->columns[a];
      d.include_attnums.push_back(a);
      d.include.push_back({col.type, col.typmod, false, std::nullopt});
    }
    // Lay its entries out once, for every open of the index.
    d.layout = std::make_shared<const sm2::idx_layout_t>(d.include);
    if (SIZE_MAX != d.layout->desc.bad) {
      return corrupt("an INCLUDE column of " + x.name + " sm2 cannot lay out");
    }
    t->indexes.push_back(std::move(d));
  }
  out = std::move(t);
  return 0;
}

} // namespace sm3

namespace {

/* Load every entry into cat. */
int load(sm2::catkv_t *kv, sm3::catalog_t &cat) {
  // The format first: nothing else is read under the wrong one.
  std::string key;
  std::string_view v;
  if (sm3::meta_key(key) != 0) {
    return -1;
  }
  int rc = sm2::catkv_get(kv, key, v);
  if (rc != 0) {
    return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND,
                                 "catalog: the database has none")
                   : -1;
  }
  int32_t version;
  if (sm3::decode_meta(v, version) != 0) {
    return -1;
  }
  if (version != sm3::format_version) {
    return sm1::seterr(sm1::SM_ERR_VERSION,
                       "catalog: format " + std::to_string(version) + ", not " +
                           std::to_string(sm3::format_version));
  }
  std::vector<std::pair<uint64_t, std::string>> entries;
  if (scan_kind(kv, sm3::kind_t::nspace, entries) != 0) {
    return -1;
  }
  for (auto &[oid, blob] : entries) {
    sm3::nspace_rec_t ns;
    if (sm3::decode_nspace(blob, ns) != 0) {
      return -1;
    }
    cat.nspaces[oid] = ns.name;
  }
  // Collations before tables, whose columns name them.
  entries.clear();
  if (scan_kind(kv, sm3::kind_t::collation, entries) != 0) {
    return -1;
  }
  for (auto &[oid, blob] : entries) {
    sm3::collation_rec_t co;
    sm2::collation_t *c = nullptr;
    if (sm3::decode_collation(blob, co) != 0 ||
        sm2::collation_open(co.locale, co.deterministic, co.icu_version, c) !=
            0) {
      return -1;
    }
    cat.collations[oid] = c;
  }
  // The namespace of each collation, which only its name records.
  if (member_nspaces(kv, cat) != 0) {
    return -1;
  }
  // Each table's own fields, then its columns and its indexes,
  // which have a key each and arrive in the order of their place.
  entries.clear();
  if (scan_kind(kv, sm3::kind_t::table, entries) != 0) {
    return -1;
  }
  std::map<uint64_t, sm3::table_rec_t> recs;
  for (auto &[oid, blob] : entries) {
    if (sm3::decode_table(blob, recs[oid]) != 0) {
      return -1;
    }
  }
  if (scan_parts(kv, sm3::kind_t::column,
                 [&](uint64_t table, uint64_t attnum, std::string_view v) {
                   auto it = recs.find(table);
                   // A column is at the place its key says.
                   if (it == recs.end() ||
                       attnum != it->second.columns.size()) {
                     return corrupt("a column of no table, or out of place");
                   }
                   it->second.columns.emplace_back();
                   return sm3::decode_column(v, it->second.columns.back());
                 }) != 0 ||
      scan_parts(kv, sm3::kind_t::index,
                 [&](uint64_t table, uint64_t oid, std::string_view v) {
                   auto it = recs.find(table);
                   sm3::index_rec_t x;
                   if (it == recs.end() || sm3::decode_index(v, x) != 0 ||
                       x.oid != oid) {
                     return corrupt("an index of no table, or under "
                                    "another's key");
                   }
                   it->second.indexes.push_back(std::move(x));
                   return 0;
                 }) != 0) {
    return -1;
  }
  // Each table's foreign keys, in the order of their oids; then each
  // table's referenced_by, which is derived from them and stored
  // nowhere (FOREIGNKEY.md §4).
  if (scan_parts(kv, sm3::kind_t::constraint,
                 [&](uint64_t table, uint64_t oid, std::string_view v) {
                   auto it = recs.find(table);
                   sm3::fkey_rec_t fk;
                   if (it == recs.end() || sm3::decode_constraint(v, fk) != 0 ||
                       fk.oid != oid) {
                     return corrupt("a constraint of no table, or under "
                                    "another's key");
                   }
                   it->second.fkeys.push_back(std::move(fk));
                   return 0;
                 }) != 0) {
    return -1;
  }
  for (auto &[oid, rec] : recs) {
    for (const sm3::fkey_rec_t &fk : rec.fkeys) {
      auto parent = recs.find(fk.ref_table);
      if (parent == recs.end()) {
        return corrupt("a foreign key references no table");
      }
      parent->second.referenced_by.push_back({oid, fk.oid});
    }
  }
  // Each index's state, for those that have one; the rest are
  // pending.
  if (scan_parts(kv, sm3::kind_t::index_state,
                 [&](uint64_t, uint64_t index, std::string_view v) {
                   return sm3::decode_index_state(v, cat.index_states[index]);
                 }) != 0) {
    return -1;
  }
  for (auto &[oid, rec] : recs) {
    std::shared_ptr<const sm3::table_def_t> def;
    // Collations from cat, which has them all by now.
    sm3::collation_lookup_t lookup = [&](uint64_t c,
                                         const sm2::collation_t *&o) {
      auto it = cat.collations.find(c);
      o = it == cat.collations.end() ? nullptr : it->second;
      return o != nullptr;
    };
    if (sm3::build_table(lookup, oid, rec, def) != 0) {
      return -1;
    }
    cat.table_names[{rec.nspoid, rec.name}] = oid;
    for (const sm3::index_def_t &x : def->indexes) {
      cat.index_names[{def->nspoid, x.name}] = {oid, x.oid};
    }
    cat.tables[oid] = std::move(def);
  }
  // Each view, its record whole under its one key.
  entries.clear();
  if (scan_kind(kv, sm3::kind_t::view, entries) != 0) {
    return -1;
  }
  for (auto &[oid, blob] : entries) {
    auto def = std::make_shared<sm3::view_def_t>();
    if (sm3::decode_view(blob, *def) != 0) {
      return -1;
    }
    cat.view_names[{def->nspoid, def->name}] = oid;
    cat.views[oid] = std::move(def);
  }
  return 0;
}

} // namespace

namespace sm3 {

/* Load a database's catalog; see sm3.h. */
int cat_open(sm1::env_t *env, std::string_view dbname, catalog_t *&out) {
  std::unique_ptr<catalog_t> cat;
  int rc = in_own_txn(env, dbname, nullptr, [&](sm1::session_t *session) {
    sm2::catkv_t *raw = nullptr;
    if (sm2::catkv_open(session, raw) != 0) {
      return -1;
    }
    kv_ptr kv(raw, &sm2::catkv_close);
    cat = std::make_unique<catalog_t>();
    // Kept for a load of the catalog's own later: statistics.
    cat->env = env;
    cat->dbname = std::string(dbname);
    return load(kv.get(), *cat);
  });
  // Hand it over only once its transaction has committed.
  if (rc == 0) {
    out = cat.release();
  }
  return rc;
}

/* Give the catalog its evaluator; see sm3.h. */
int cat_set_evaluator(catalog_t *cat, index_eval_fn eval) {
  if (!cat || !eval || cat->eval) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "evaluator: no catalog, none given, or one set already");
  }
  cat->eval = std::move(eval);
  return 0;
}

/* Say whether the host reads TOAST pointers; see sm3.h. */
int cat_set_toast(catalog_t *cat, bool on) {
  if (!cat) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "toast: no catalog");
  }
  cat->toast = on;
  return 0;
}

/* Close a catalog; nullptr is a no-op. */
void cat_close(catalog_t *cat) { delete cat; }

/* The committed tables' names in a namespace, sorted; see sm3.h. */
std::vector<std::string> cat_list_tables(const catalog_t *cat,
                                         uint64_t nspoid) {
  std::vector<std::string> out;
  {
    std::shared_lock lock(cat->mu);
    for (const auto &[name, oid] : cat->table_names) {
      if (name.first == nspoid) {
        out.push_back(name.second);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

/* A committed namespace by name; see sm3.h. */
bool cat_get_namespace(const catalog_t *cat, std::string_view name,
                       uint64_t &oid) {
  std::shared_lock lock(cat->mu);
  // Few namespaces, and no map by name: look at each.
  for (const auto &[o, n] : cat->nspaces) {
    if (n == name) {
      oid = o;
      return true;
    }
  }
  return false;
}

/* The committed namespaces' names, sorted; see sm3.h. */
std::vector<std::string> cat_list_namespaces(const catalog_t *cat) {
  std::vector<std::string> out;
  {
    std::shared_lock lock(cat->mu);
    for (const auto &[oid, name] : cat->nspaces) {
      out.push_back(name);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

/* A committed table by oid, or nullptr. */
std::shared_ptr<const table_def_t> cat_get_table(const catalog_t *cat,
                                                 uint64_t oid) {
  std::shared_lock lock(cat->mu);
  auto it = cat->tables.find(oid);
  return it == cat->tables.end() ? nullptr : it->second;
}

/* The committed table in store storeid, or nullptr: by a scan. */
std::shared_ptr<const table_def_t> cat_get_table_by_store(const catalog_t *cat,
                                                          uint64_t storeid) {
  std::shared_lock lock(cat->mu);
  for (const auto &[oid, def] : cat->tables) {
    if (def->storeid == storeid) {
      return def;
    }
  }
  return nullptr;
}

/* A committed table by namespace and name, or nullptr. */
std::shared_ptr<const table_def_t>
cat_get_table(const catalog_t *cat, uint64_t nspoid, std::string_view name) {
  uint64_t oid;
  {
    std::shared_lock lock(cat->mu);
    auto it = cat->table_names.find({nspoid, std::string(name)});
    if (it == cat->table_names.end()) {
      return nullptr;
    }
    oid = it->second;
  }
  return cat_get_table(cat, oid);
}

} // namespace sm3
