/*
 * load.cc -- cat_open: the catalog read into memory with one scan
 * of each kind (CATALOG.md §4, §5). Every definition is built
 * whole, its sm2 schema and layout with it, so a reader never lays
 * out a row itself.
 */

#include "internal.h"

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

/* Set cat's collation_nsp from the NAME entries, in one scan of them. */
int collation_nspaces(sm2::catkv_t *kv, sm3::catalog_t &cat) {
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
    if (sm3::decode_name(v, rec) != 0 || sm3::name_nspoid(k, nspoid) != 0) {
      return -1;
    }
    if (rec.kind == sm3::kind_t::collation) {
      cat.collation_nsp[rec.oid] = nspoid;
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
  t->rowno_seq = rec.rowno_seq;
  t->next_relid = rec.next_relid;
  for (const sm3::column_rec_t &c : rec.columns) {
    const sm2::type_t *type = sm2::type_by_code(c.type_code);
    const sm2::collation_t *coll;
    if (!type || !collation_of(lookup, c.collation, coll)) {
      return corrupt("column " + c.name + " of " + rec.name +
                     " names an unknown type or collation");
    }
    t->columns.push_back(
        {c.name, type, c.typmod, c.not_null, c.dropped, c.missing, coll});
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
    d.state = x.state;
    d.error = x.error;
    d.reason = x.reason;
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
  if (collation_nspaces(kv, cat) != 0) {
    return -1;
  }
  entries.clear();
  if (scan_kind(kv, sm3::kind_t::table, entries) != 0) {
    return -1;
  }
  for (auto &[oid, blob] : entries) {
    sm3::table_rec_t rec;
    std::shared_ptr<const sm3::table_def_t> def;
    // Collations from cat, which has them all by now.
    sm3::collation_lookup_t lookup = [&](uint64_t c,
                                         const sm2::collation_t *&o) {
      auto it = cat.collations.find(c);
      o = it == cat.collations.end() ? nullptr : it->second;
      return o != nullptr;
    };
    if (sm3::decode_table(blob, rec) != 0 ||
        sm3::build_table(lookup, oid, rec, def) != 0) {
      return -1;
    }
    cat.table_names[{rec.nspoid, rec.name}] = oid;
    cat.tables[oid] = std::move(def);
  }
  return 0;
}

} // namespace

namespace sm3 {

/* Load a database's catalog; see sm3.h. */
int cat_open(sm1::env_t *env, std::string_view dbname, catalog_t *&out) {
  std::unique_ptr<catalog_t> cat;
  int rc = in_own_txn(env, dbname, nullptr, [&](sm1::session_t *session) {
    return catch_nomem([&] {
      sm2::catkv_t *raw = nullptr;
      if (sm2::catkv_open(session, raw) != 0) {
        return -1;
      }
      kv_ptr kv(raw, &sm2::catkv_close);
      cat = std::make_unique<catalog_t>();
      return load(kv.get(), *cat);
    });
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

/* Close a catalog; nullptr is a no-op. */
void cat_close(catalog_t *cat) { delete cat; }

/* Every committed table; see sm3.h. */
std::vector<std::shared_ptr<const table_def_t>>
cat_tables(const catalog_t *cat) {
  std::shared_lock lock(cat->mu);
  std::vector<std::shared_ptr<const table_def_t>> out;
  out.reserve(cat->tables.size());
  for (const auto &[oid, def] : cat->tables) {
    out.push_back(def);
  }
  return out;
}

/* A committed table by oid, or nullptr. */
std::shared_ptr<const table_def_t> cat_table(const catalog_t *cat,
                                             uint64_t oid) {
  std::shared_lock lock(cat->mu);
  auto it = cat->tables.find(oid);
  return it == cat->tables.end() ? nullptr : it->second;
}

/* A committed table by namespace and name, or nullptr. */
std::shared_ptr<const table_def_t>
cat_table(const catalog_t *cat, uint64_t nspoid, std::string_view name) {
  uint64_t oid;
  {
    std::shared_lock lock(cat->mu);
    auto it = cat->table_names.find({nspoid, std::string(name)});
    if (it == cat->table_names.end()) {
      return nullptr;
    }
    oid = it->second;
  }
  return cat_table(cat, oid);
}

} // namespace sm3
