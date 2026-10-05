/*
 * ddl.cc -- changes to the catalog (CATALOG.md §5): cat_init,
 * and so far cat_create_table. Each writes its entries through
 * sm2's catalog key-value store, in the session's transaction, so
 * it commits and rolls back with the transaction's other writes.
 */

#include "internal.h"

#include <algorithm>
#include <set>
#include <string>

namespace {

using kv_ptr = std::unique_ptr<sm2::catkv_t, decltype(&sm2::catkv_close)>;

/* The sequence oids come from; they start at 1000 (CATALOG.md §1). */
constexpr const char *oid_seq = "sm3.oid";
constexpr uint64_t first_oid = 1000;

/*
 * Names of namespaces live in namespace 0, which is no namespace:
 * every name is in one, and a namespace's is in this.
 */
constexpr uint64_t nsp_of_namespaces = 0;

/* Open session's catalog store into kv. */
int open_kv(sm1::session_t *session, kv_ptr &kv) {
  sm2::catkv_t *raw = nullptr;
  if (sm2::catkv_open(session, raw) != 0) {
    return -1;
  }
  kv.reset(raw);
  return 0;
}

/* Insert name's NAME entry in nspoid, saying it names rec. */
int insert_name(sm2::catkv_t *kv, uint64_t nspoid, std::string_view name,
                const sm3::name_rec_t &rec) {
  std::string key, value;
  if (sm3::name_key(nspoid, name, key) != 0 ||
      sm3::encode_name(rec, value) != 0) {
    return -1;
  }
  return sm2::catkv_insert(kv, key, value);
}

/* Insert the entity oid of kind, holding blob. */
int insert_entity(sm2::catkv_t *kv, sm3::kind_t kind, uint64_t oid,
                  const std::string &blob) {
  std::string key;
  if (sm3::entity_key(kind, oid, key) != 0) {
    return -1;
  }
  return sm2::catkv_insert(kv, key, blob);
}

/* Fail with SM_ERR_INVALID for a DDL call's argument. */
int invalid(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, why);
}

/* Read the entity oid of kind into value; SM_ERR_NOTFOUND if none. */
int get_entity(sm2::catkv_t *kv, sm3::kind_t kind, uint64_t oid,
               std::string &value, const char *what) {
  std::string key;
  std::string_view v;
  if (sm3::entity_key(kind, oid, key) != 0) {
    return -1;
  }
  int rc = sm2::catkv_get(kv, key, v);
  if (rc != 0) {
    return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND, std::string(what) + " " +
                                                           std::to_string(oid) +
                                                           " does not exist")
                   : -1;
  }
  value.assign(v);
  return 0;
}

/*
 * Open session's catalog store into kv, take the table oid's
 * schema lock X, and read its record under an X lock on its TABLE
 * key too: the newest committed one, which no other DDL can
 * change until commit. At READ COMMITTED that is a read_latest,
 * which also lets this transaction overwrite a newer version; at
 * REPEATABLE READ the lock has already failed on one.
 */
int open_table(sm3::session_t *session, uint64_t oid, kv_ptr &kv,
               sm3::table_rec_t &rec) {
  std::string key, blob;
  std::string_view v;
  if (open_kv(session->sm1, kv) != 0 ||
      sm3::lock_table(kv.get(), oid, sm1::lockmode_t::exclusive) != 0 ||
      sm3::entity_key(sm3::kind_t::table, oid, key) != 0 ||
      sm2::catkv_lock(kv.get(), key, sm1::lockmode_t::exclusive) != 0) {
    return -1;
  }
  int rc = session->level == sm1::xctlevel_t::read_committed
               ? sm2::catkv_get_latest(kv.get(), key, v)
               : sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc == 1
               ? sm1::seterr(sm1::SM_ERR_NOTFOUND,
                             "table " + std::to_string(oid) + " does not exist")
               : -1;
  }
  blob.assign(v);
  return sm3::decode_table(blob, rec);
}

/* Rewrite the table oid's blob from rec. */
int write_table(sm2::catkv_t *kv, uint64_t oid, const sm3::table_rec_t &rec) {
  std::string key, blob;
  if (sm3::entity_key(sm3::kind_t::table, oid, key) != 0 ||
      sm3::encode_table(rec, blob) != 0) {
    return -1;
  }
  return sm2::catkv_update(kv, key, blob);
}

/* Erase name's NAME entry in nspoid. */
int erase_name(sm2::catkv_t *kv, uint64_t nspoid, std::string_view name) {
  std::string key;
  if (sm3::name_key(nspoid, name, key) != 0) {
    return -1;
  }
  return sm2::catkv_erase(kv, key);
}

/* The place of rec's live column name, or -1. */
int live_column(const sm3::table_rec_t &rec, std::string_view name) {
  for (size_t i = 0; i < rec.columns.size(); i++) {
    if (!rec.columns[i].dropped && rec.columns[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/* True if attnum names a live column of rec. */
bool live_attnum(const sm3::table_rec_t &rec, int32_t attnum) {
  return attnum >= 0 && static_cast<size_t>(attnum) < rec.columns.size() &&
         !rec.columns[attnum].dropped;
}

/*
 * Refuse a missing value that is not a datum of type under typmod:
 * a fixed-width type's exact bytes, or a whole, readable varlena.
 */
int check_missing(const sm2::type_t *type, int32_t typmod,
                  std::string_view missing) {
  int32_t w = sm2::datum_width(type, typmod);
  if (w > 0) {
    return static_cast<size_t>(w) == missing.size()
               ? 0
               : invalid("add column: a missing value of the wrong width");
  }
  // A varlena: its header, 1 byte or 4, must fit and say its length.
  size_t head = !missing.empty() && (missing[0] & 0x01) ? 1 : 4;
  std::string_view v;
  if (missing.size() < head ||
      sm2::datum_bytes(type, typmod, sm2::datum_from_ptr(missing.data()), v) !=
          0 ||
      v.size() != missing.size()) {
    return invalid("add column: a missing value that is not a varlena");
  }
  return 0;
}

/* Refuse columns the table cannot be laid out by. */
int check_columns(std::span<const sm3::column_spec_t> columns) {
  std::set<std::string> seen;
  for (const sm3::column_spec_t &c : columns) {
    const sm2::type_t *type = sm2::type_by_code(c.type_code);
    if (c.name.empty() || !seen.insert(c.name).second) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "create table: a column without a name, or two "
                         "with one: " +
                             c.name);
    }
    if (!type || sm2::datum_width(type, c.typmod) == 0) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "create table: column " + c.name +
                             " has a type code sm2 lacks, or a typmod its "
                             "type refuses");
    }
  }
  return 0;
}

/*
 * Build the definition rec describes, as s sees its collations, and
 * record it in s's overlay as the table oid's.
 */
int record_table(sm3::session_t *s, uint64_t oid, const sm3::table_rec_t &rec) {
  std::shared_ptr<const sm3::table_def_t> def;
  sm3::collation_lookup_t lookup = [s](uint64_t c,
                                       const sm2::collation_t *&out) {
    return sm3::session_collation(s, c, out);
  };
  if (sm3::build_table(lookup, oid, rec, def) != 0) {
    return -1;
  }
  sm3::overlay_table(s, oid, std::move(def));
  return 0;
}

} // namespace

namespace sm3 {

/* Make a new database's catalog; see sm3.h. */
int cat_init(sm1::session_t *session) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    std::string key, value;
    std::string_view v;
    if (open_kv(session, kv) != 0 || meta_key(key) != 0) {
      return -1;
    }
    // A database is initialized once.
    int rc = sm2::catkv_get(kv.get(), key, v);
    if (rc != 1) {
      return rc == 0 ? sm1::seterr(sm1::SM_ERR_EXISTS,
                                   "catalog: the database has one already")
                     : -1;
    }
    if (encode_meta(format_version, value) != 0 ||
        sm2::catkv_insert(kv.get(), key, value) != 0) {
      return -1;
    }
    // Oids for everything made from now on.
    if (sm1::mkseq(session, oid_seq, first_oid, 1) != 0) {
      return -1;
    }
    // The built-in namespaces, at their fixed oids.
    for (auto [oid, name] :
         {std::pair<uint64_t, const char *>{nsp_pg_catalog, "pg_catalog"},
          {nsp_public, "public"}}) {
      std::string blob;
      if (insert_name(kv.get(), nsp_of_namespaces, name,
                      {kind_t::nspace, oid, std::nullopt}) != 0 ||
          encode_nspace({name}, blob) != 0 ||
          insert_entity(kv.get(), kind_t::nspace, oid, blob) != 0) {
        return -1;
      }
    }
    return 0;
  });
}

/* Create a table; see sm3.h. */
int cat_create_table(session_t *session, uint64_t nspoid, std::string_view name,
                     std::span<const column_spec_t> columns, uint64_t &oid) {
  return catch_nomem([&] {
    if (name.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "create table: no name");
    }
    if (check_columns(columns) != 0) {
      return -1;
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    std::string key, blob;
    std::string_view v;
    if (open_kv(session->sm1, kv) != 0 ||
        entity_key(kind_t::nspace, nspoid, key) != 0) {
      return -1;
    }
    int rc = sm2::catkv_get(kv.get(), key, v);
    if (rc != 0) {
      return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND,
                                   "create table: no namespace " +
                                       std::to_string(nspoid))
                     : -1;
    }
    // The oid first: the name's entry says what it names.
    uint64_t t;
    if (sm1::nextseq(session->sm1, oid_seq, t) != 0) {
      return -1;
    }
    // The name next, so a taken one fails before a store is made.
    if (insert_name(kv.get(), nspoid, name, {kind_t::table, t, std::nullopt}) !=
        0) {
      return -1;
    }
    table_rec_t rec{
        nspoid, std::string(name), 0, "sm3.rowno." + std::to_string(t), 1, {},
        {}};
    // The table's store and its rowno sequence (DESIGN.md §5).
    if (sm1::mkstore(session->sm1, rec.storeid) != 0 ||
        sm1::mkseq(session->sm1, rec.rowno_seq, 1, 1) != 0) {
      return -1;
    }
    for (const column_spec_t &c : columns) {
      rec.columns.push_back({c.name, c.type_code, c.typmod, c.not_null, false,
                             std::nullopt, std::nullopt});
    }
    if (encode_table(rec, blob) != 0 ||
        insert_entity(kv.get(), kind_t::table, t, blob) != 0 ||
        record_table(session, t, rec) != 0) {
      return -1;
    }
    overlay_name(session, nspoid, name, t);
    oid = t;
    return 0;
  });
}

} // namespace sm3

namespace sm3 {

/* Drop a table; see sm3.h. */
int cat_drop_table(session_t *session, uint64_t oid) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    std::string key;
    if (open_table(session, oid, kv, rec) != 0) {
      return -1;
    }
    // Its names, then its definition.
    if (erase_name(kv.get(), rec.nspoid, rec.name) != 0) {
      return -1;
    }
    for (const index_rec_t &x : rec.indexes) {
      if (erase_name(kv.get(), rec.nspoid, x.name) != 0) {
        return -1;
      }
    }
    if (entity_key(kind_t::table, oid, key) != 0 ||
        sm2::catkv_erase(kv.get(), key) != 0) {
      return -1;
    }
    // Its store takes its rows and index entries with it (DESIGN.md §5).
    if (sm1::rmstore(session->sm1, rec.storeid) != 0 ||
        sm1::rmseq(session->sm1, rec.rowno_seq) != 0) {
      return -1;
    }
    overlay_table(session, oid, nullptr);
    overlay_name(session, rec.nspoid, rec.name, std::nullopt);
    return 0;
  });
}

/* Rename a table; see sm3.h. */
int cat_rename_table(session_t *session, uint64_t oid, std::string_view name) {
  return catch_nomem([&] {
    if (name.empty()) {
      return invalid("rename table: no name");
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    if (open_table(session, oid, kv, rec) != 0) {
      return -1;
    }
    if (rec.name == name) {
      return 0;
    }
    // The new name first, so a taken one leaves the old in place.
    if (insert_name(kv.get(), rec.nspoid, name,
                    {kind_t::table, oid, std::nullopt}) != 0 ||
        erase_name(kv.get(), rec.nspoid, rec.name) != 0) {
      return -1;
    }
    overlay_name(session, rec.nspoid, rec.name, std::nullopt);
    overlay_name(session, rec.nspoid, name, oid);
    rec.name = name;
    if (write_table(kv.get(), oid, rec) != 0) {
      return -1;
    }
    return record_table(session, oid, rec);
  });
}

/* Append a column; see sm3.h. */
int cat_add_column(session_t *session, uint64_t oid, const column_spec_t &col,
                   std::optional<std::string_view> missing) {
  return catch_nomem([&] {
    if (check_columns(std::span<const column_spec_t>(&col, 1)) != 0) {
      return -1;
    }
    const sm2::type_t *type = sm2::type_by_code(col.type_code);
    if (missing && check_missing(type, col.typmod, *missing) != 0) {
      return -1;
    }
    if (col.not_null && !missing) {
      return invalid("add column: " + col.name +
                     " is NOT NULL, so older rows need a missing value");
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    if (open_table(session, oid, kv, rec) != 0) {
      return -1;
    }
    if (live_column(rec, col.name) >= 0) {
      return invalid("add column: " + col.name + " is taken");
    }
    rec.columns.push_back(
        {col.name, col.type_code, col.typmod, col.not_null, false,
         missing ? std::optional<std::string>(*missing) : std::nullopt,
         std::nullopt});
    if (write_table(kv.get(), oid, rec) != 0) {
      return -1;
    }
    return record_table(session, oid, rec);
  });
}

/* Drop a column; see sm3.h. */
int cat_drop_column(session_t *session, uint64_t oid, std::string_view name) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    if (open_table(session, oid, kv, rec) != 0) {
      return -1;
    }
    int at = live_column(rec, name);
    if (at < 0) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "drop column: no column " + std::string(name));
    }
    for (const index_rec_t &x : rec.indexes) {
      bool used =
          std::find(x.includes.begin(), x.includes.end(), at) !=
              x.includes.end() ||
          std::find(x.reads.begin(), x.reads.end(), at) != x.reads.end();
      for (const index_key_rec_t &k : x.keys) {
        used |= k.attnum == at;
      }
      if (used) {
        return invalid("drop column: index " + x.name + " uses " +
                       std::string(name));
      }
    }
    // It keeps its place, and reads as null from now on.
    column_rec_t &c = rec.columns[at];
    c.dropped = true;
    c.not_null = false;
    c.missing = std::nullopt;
    if (write_table(kv.get(), oid, rec) != 0) {
      return -1;
    }
    return record_table(session, oid, rec);
  });
}

/* Create a plain index; see sm3.h. */
int cat_create_index(session_t *session, uint64_t table, std::string_view name,
                     std::span<const index_key_spec_t> keys,
                     std::span<const int32_t> includes, bool unique,
                     uint64_t &oid) {
  return cat_create_index(session, table, name, keys, includes, unique, {}, {},
                          oid);
}

/* Create an index, plain, partial or of expressions; see sm3.h. */
int cat_create_index(session_t *session, uint64_t table, std::string_view name,
                     std::span<const index_key_spec_t> keys,
                     std::span<const int32_t> includes, bool unique,
                     std::string_view eval, std::span<const int32_t> reads,
                     uint64_t &oid) {
  return catch_nomem([&] {
    if (name.empty() || keys.empty()) {
      return invalid("create index: no name, or no key column");
    }
    // A blob is what makes an index one the host evaluates.
    if (eval.empty() && !reads.empty()) {
      return invalid("create index: columns read, and nothing to evaluate");
    }
    if (!eval.empty() && !session->cat->eval) {
      return invalid("create index: the catalog has no evaluator");
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    std::string blob;
    if (open_table(session, table, kv, rec) != 0) {
      return -1;
    }
    index_rec_t x;
    x.oid = 0;
    x.name = std::string(name);
    x.prefix = rec.next_prefix;
    x.unique = unique;
    x.state = index_state_t::building;
    x.eval = std::string(eval);
    for (int32_t a : reads) {
      if (!live_attnum(rec, a)) {
        return invalid("create index: it reads no live column");
      }
      x.reads.push_back(a);
    }
    for (const index_key_spec_t &k : keys) {
      bool expr = k.attnum == -1;
      if (expr) {
        // An expression gives its own type, and needs the blob.
        const sm2::type_t *type = sm2::type_by_code(k.type_code);
        if (eval.empty() || !type || sm2::datum_width(type, k.typmod) == 0) {
          return invalid("create index: an expression with nothing to "
                         "evaluate, or of no type sm2 has");
        }
      } else if (!live_attnum(rec, k.attnum)) {
        return invalid("create index: a key names no live column");
      }
      if (k.collation) {
        uint8_t code = expr ? k.type_code : rec.columns[k.attnum].type_code;
        if (code != sm2::type_text.code && code != sm2::type_varchar.code) {
          return invalid("create index: a collation on a column not text");
        }
        if (get_entity(kv.get(), kind_t::collation, *k.collation, blob,
                       "collation") != 0) {
          return -1;
        }
      }
      x.keys.push_back({k.attnum, k.desc, k.nulls_first, k.collation,
                        expr ? k.type_code : uint8_t{0}, expr ? k.typmod : -1});
    }
    for (int32_t a : includes) {
      if (!live_attnum(rec, a)) {
        return invalid("create index: an INCLUDE names no live column");
      }
      x.includes.push_back(a);
    }
    // 0xFFFF is TOAST's (SM2_DESIGN.md §5); prefixes are never reused.
    if (rec.next_prefix == 0xFFFF) {
      return invalid("create index: the table has no index prefix left");
    }
    uint64_t i;
    if (sm1::nextseq(session->sm1, oid_seq, i) != 0 ||
        insert_name(kv.get(), rec.nspoid, name, {kind_t::index, i, table}) !=
            0) {
      return -1;
    }
    x.oid = i;
    rec.next_prefix++;
    rec.indexes.push_back(std::move(x));
    if (write_table(kv.get(), table, rec) != 0 ||
        record_table(session, table, rec) != 0) {
      return -1;
    }
    // No other transaction sees a table this one made, so the build
    // has nothing to wait for: run it here, over the rows this
    // transaction has written, and record the index valid. Any other
    // table's index stays building, for the build to fill.
    if (!cat_table(session->cat, table)) {
      table_t *t = nullptr;
      if (tbl_open(session, table, sm1::lockmode_t::exclusive, t) != 0) {
        return -1;
      }
      uint64_t from = 0;
      bool done = false;
      int rc = 0;
      while (rc == 0 && !done) {
        rc = tbl_index_backfill(t, i, from, 1024, from, done);
      }
      tbl_close(t);
      if (rc != 0) {
        return -1;
      }
      rec.indexes.back().state = index_state_t::valid;
      if (write_table(kv.get(), table, rec) != 0 ||
          record_table(session, table, rec) != 0) {
        return -1;
      }
    }
    oid = i;
    return 0;
  });
}

/* Drop an index; see sm3.h. */
int cat_drop_index(session_t *session, uint64_t table, uint64_t oid) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    if (open_table(session, table, kv, rec) != 0) {
      return -1;
    }
    auto it = std::find_if(rec.indexes.begin(), rec.indexes.end(),
                           [&](const index_rec_t &x) { return x.oid == oid; });
    if (it == rec.indexes.end()) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "drop index: no index " + std::to_string(oid));
    }
    if (erase_name(kv.get(), rec.nspoid, it->name) != 0) {
      return -1;
    }
    // Its entries are the keys under its prefix in the table's store;
    // sm1 has no range delete, so each is erased.
    uint16_t p = it->prefix, q = static_cast<uint16_t>(p + 1);
    std::string start{static_cast<char>(p >> 8), static_cast<char>(p & 0xFF)};
    std::string end{static_cast<char>(q >> 8), static_cast<char>(q & 0xFF)};
    std::vector<std::string> entries;
    sm1::scan_t *c = nullptr;
    if (sm1::scan(session->sm1, rec.storeid, start.data(), start.size(), 1,
                  end.data(), end.size(), 0, c) != 0) {
      return -1;
    }
    std::string_view k, v;
    int rc;
    while ((rc = sm1::scannxt(c, k, v)) == 0) {
      entries.emplace_back(k);
    }
    sm1::scanend(c);
    if (rc != 1) {
      return -1;
    }
    for (const std::string &e : entries) {
      if (sm1::erase(session->sm1, rec.storeid, e) != 0) {
        return -1;
      }
    }
    rec.indexes.erase(it);
    if (write_table(kv.get(), table, rec) != 0) {
      return -1;
    }
    return record_table(session, table, rec);
  });
}

/* Set an index's state; see sm3.h. */
int cat_set_index_state(session_t *session, uint64_t table, uint64_t oid,
                        index_state_t state, sm1::errnum_t error,
                        std::string_view reason) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    table_rec_t rec;
    if (open_table(session, table, kv, rec) != 0) {
      return -1;
    }
    auto it = std::find_if(rec.indexes.begin(), rec.indexes.end(),
                           [&](const index_rec_t &x) { return x.oid == oid; });
    if (it == rec.indexes.end()) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "index state: no index " + std::to_string(oid));
    }
    it->state = state;
    bool failed = state == index_state_t::failed;
    it->error = failed ? error : sm1::SM_OK;
    it->reason = failed ? std::string(reason) : std::string();
    if (write_table(kv.get(), table, rec) != 0) {
      return -1;
    }
    return record_table(session, table, rec);
  });
}

/* Create a collation; see sm3.h. */
int cat_create_collation(session_t *session, uint64_t nspoid,
                         std::string_view name, std::string_view locale,
                         bool deterministic, uint64_t &oid) {
  return catch_nomem([&] {
    if (name.empty()) {
      return invalid("create collation: no name");
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    std::string blob;
    if (open_kv(session->sm1, kv) != 0 ||
        get_entity(kv.get(), kind_t::nspace, nspoid, blob, "namespace") != 0) {
      return -1;
    }
    // Opening it checks the locale and gives the version to record.
    sm2::collation_t *raw = nullptr;
    if (sm2::collation_open(locale, deterministic, "", raw) != 0) {
      return -1;
    }
    std::unique_ptr<sm2::collation_t, decltype(&sm2::collation_close)> c(
        raw, &sm2::collation_close);
    std::string version(sm2::collation_version(c.get()));
    uint64_t i;
    if (sm1::nextseq(session->sm1, oid_seq, i) != 0 ||
        insert_name(kv.get(), nspoid, name,
                    {kind_t::collation, i, std::nullopt}) != 0 ||
        encode_collation(
            {std::string(name), std::string(locale), deterministic, version},
            blob) != 0 ||
        insert_entity(kv.get(), kind_t::collation, i, blob) != 0) {
      return -1;
    }
    // The overlay holds it open, for this transaction's DDL to use.
    overlay_collation(session, i, c.release());
    oid = i;
    return 0;
  });
}

/* Take a table's schema lock on its LOCK key; see internal.h. */
int lock_table(sm2::catkv_t *kv, uint64_t oid, sm1::lockmode_t mode) {
  std::string key;
  if (entity_key(kind_t::lock, oid, key) != 0) {
    return -1;
  }
  // Held to the end of the transaction: DDL waits out its users.
  return sm2::catkv_lock(kv, key, mode);
}

} // namespace sm3
