/*
 * ddl.cc -- DDL (SM3_DESIGN.md §2, CATALOG.md §5): what a CREATE,
 * a DROP or an ALTER does. Each call makes or drops what its
 * object needs outside the catalog -- a store, a sequence, an
 * index's entries -- and puts the object's record in the catalog
 * or takes it out, all in the session's transaction, so it commits
 * and rolls back with the transaction's other writes. The catalog
 * itself only stores the records (catalog/ddl.cc).
 */

#include "../catalog/internal.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace {

/* Fail with SM_ERR_INVALID and why. */
int invalid(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, why);
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
  std::unordered_set<std::string> seen;
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

} // namespace

namespace sm3 {

/* Create a namespace; see sm3.h. */
int create_namespace(session_t *session, std::string_view name, uint64_t &oid) {
  if (name.empty()) {
    return invalid("create namespace: no name");
  }
  uint64_t n;
  if (cat_new_oid(session, n) != 0 ||
      cat_put_namespace(session, n, name) != 0) {
    return -1;
  }
  oid = n;
  return 0;
}

/* Drop a namespace; see sm3.h. */
int drop_namespace(session_t *session, uint64_t oid) {
  return cat_del_namespace(session, oid);
}

/* Create a table; see sm3.h. */
int create_table(session_t *session, uint64_t nspoid, std::string_view name,
                 std::span<const column_spec_t> columns, uint64_t &oid) {
  if (name.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "create table: no name");
  }
  if (check_columns(columns) != 0) {
    return -1;
  }
  uint64_t t;
  if (cat_new_oid(session, t) != 0) {
    return -1;
  }
  table_rec_t rec{nspoid,
                  std::string(name),
                  0,
                  "sm3.rowno." + std::to_string(t),
                  first_index_relid,
                  {},
                  {}};
  for (const column_spec_t &c : columns) {
    rec.columns.push_back({c.name, c.type_code, c.typmod, c.not_null, false,
                           std::nullopt, std::nullopt});
  }
  // The table's store and its rowno sequence (DESIGN.md §5): for a
  // temporary table, both temporary, and so the session's alone
  // (TEMPTABLE.md §4).
  const bool temp = nspoid == nsp_pg_temp;
  if (sm1::mkstore(session->sm1, rec.storeid, temp) != 0) {
    return -1;
  }
  if (sm1::mkseq(session->sm1, rec.rowno_seq, 1, 1, temp) == 0 &&
      cat_put_table(session, t, rec) == 0) {
    oid = t;
    return 0;
  }
  // Not made after all -- a taken name, no such namespace: take
  // the store and the sequence back, so a transaction that goes
  // on leaves neither behind. After a retryable error it has
  // been aborted, and they have gone with it.
  if (sm1::retryable() != 1) {
    sm1::errnum_t error = sm1::errnum();
    std::string reason = sm1::errmsg();
    sm1::rmseq(session->sm1, rec.rowno_seq);
    sm1::rmstore(session->sm1, rec.storeid);
    sm1::seterr(error, reason);
  }
  return -1;
}

/* Drop a table; see sm3.h. */
int drop_table(session_t *session, uint64_t oid) {
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0 ||
      cat_del_table(session, oid) != 0) {
    return -1;
  }
  // Its store takes its rows and index entries with it (DESIGN.md §5).
  if (sm1::rmstore(session->sm1, rec.storeid) != 0 ||
      sm1::rmseq(session->sm1, rec.rowno_seq) != 0) {
    return -1;
  }
  return 0;
}

/* Empty a table, by giving it a new store; see sm3.h. */
int truncate_table(session_t *session, uint64_t oid) {
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  const uint64_t old_store = rec.storeid;
  // A new, empty store for the rows and index entries, temporary
  // if the table is; the index relids and the rowno sequence carry
  // over.
  if (sm1::mkstore(session->sm1, rec.storeid, rec.nspoid == nsp_pg_temp) != 0 ||
      cat_put_table(session, oid, rec) != 0) {
    return -1;
  }
  // The old store goes at commit, taking the rows and index
  // entries with it (DESIGN.md §5).
  return sm1::rmstore(session->sm1, old_store);
}

/* Rename a table; see sm3.h. */
int rename_table(session_t *session, uint64_t oid, std::string_view name) {
  if (name.empty()) {
    return invalid("rename table: no name");
  }
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  if (rec.name == name) {
    return 0;
  }
  // The put takes the new name before it lets go of the old, so
  // a taken one leaves the table as it was.
  rec.name = name;
  return cat_put_table(session, oid, rec);
}

/* Append a column; see sm3.h. */
int add_column(session_t *session, uint64_t oid, const column_spec_t &col,
               std::optional<std::string_view> missing) {
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
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  if (live_column(rec, col.name) >= 0) {
    return invalid("add column: " + col.name + " is taken");
  }
  rec.columns.push_back(
      {col.name, col.type_code, col.typmod, col.not_null, false,
       missing ? std::optional<std::string>(*missing) : std::nullopt,
       std::nullopt});
  return cat_put_table(session, oid, rec);
}

/* Drop a column; see sm3.h. */
int drop_column(session_t *session, uint64_t oid, std::string_view name) {
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "drop column: no column " + std::string(name));
  }
  for (const index_rec_t &x : rec.indexes) {
    bool used = std::find(x.includes.begin(), x.includes.end(), at) !=
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
  return cat_put_table(session, oid, rec);
}

/* Create a plain index; see sm3.h. */
int create_index(session_t *session, uint64_t table, std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 uint64_t &oid) {
  return create_index(session, table, name, keys, includes, unique, {}, {},
                      oid);
}

/* Create an index, plain, partial or of expressions; see sm3.h. */
int create_index(session_t *session, uint64_t table, std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 std::string_view eval, std::span<const int32_t> reads,
                 uint64_t &oid) {
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
  table_rec_t rec;
  if (cat_get_table_rec(session, table, rec) != 0) {
    return -1;
  }
  index_rec_t x;
  x.oid = 0;
  x.name = std::string(name);
  x.relid = rec.next_relid;
  x.unique = unique;
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
      const sm2::collation_t *c = nullptr;
      if (!session_collation(session, *k.collation, c)) {
        return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                           "collation " + std::to_string(*k.collation) +
                               " does not exist");
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
  // 0xFFFF is TOAST's (SM2_DESIGN.md §5); relids are never reused.
  if (rec.next_relid == 0xFFFF) {
    return invalid("create index: the table has no index relid left");
  }
  uint64_t i;
  if (cat_new_oid(session, i) != 0) {
    return -1;
  }
  x.oid = i;
  rec.next_relid++;
  rec.indexes.push_back(std::move(x));
  // The put takes the index's name: one taken fails here.
  if (cat_put_table(session, table, rec) != 0) {
    return -1;
  }
  // No other transaction sees a table this one made, so the build
  // has nothing to wait for: run it here, over the rows this
  // transaction has written, and record the index valid. Any other
  // table's index stays pending, for the build to fill.
  if (!cat_get_table(session->cat, table)) {
    table_t *t = nullptr;
    if (tbl_open(session, table, sm1::SM_LOCKMODE_EX, t) != 0) {
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
    if (cat_put_index_state(session, table, i,
                            {index_state_t::valid, sm1::SM_OK, {}}) != 0) {
      return -1;
    }
  } else {
    // Once this transaction has committed, put the index on the
    // builder's queue; a rollback that undoes the index drops it.
    catalog_t *cat = session->cat;
    if (sm1::on_commit(session->sm1, [cat, table, i] {
          builder_enqueue(cat, table, i);
        }) != 0) {
      return -1;
    }
  }
  oid = i;
  return 0;
}

/* Drop an index; see sm3.h. */
int drop_index(session_t *session, uint64_t table, uint64_t oid) {
  table_rec_t rec;
  if (cat_get_table_rec(session, table, rec) != 0) {
    return -1;
  }
  auto it = std::find_if(rec.indexes.begin(), rec.indexes.end(),
                         [&](const index_rec_t &x) { return x.oid == oid; });
  if (it == rec.indexes.end()) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "drop index: no index " + std::to_string(oid));
  }
  // Its entries are the keys under its relid in the table's store;
  // sm1 has no range delete, so each is erased.
  uint16_t p = it->relid, q = static_cast<uint16_t>(p + 1);
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
  // The put lets go of its name.
  rec.indexes.erase(it);
  return cat_put_table(session, table, rec);
}

/* Queue what a restart left pending; see sm3.h. */
int enqueue_pending_indexes(catalog_t *cat) {
  for (const std::string &ns : cat_list_namespaces(cat)) {
    uint64_t nspoid;
    if (!cat_get_namespace(cat, ns, nspoid)) {
      continue;
    }
    for (const std::string &name : cat_list_tables(cat, nspoid)) {
      // One dropped since it was listed is simply not there.
      std::shared_ptr<const table_def_t> def = cat_get_table(cat, nspoid, name);
      for (size_t i = 0; def && i < def->indexes.size(); i++) {
        uint64_t x = def->indexes[i].oid;
        if (cat_get_index_state(cat, x).state == index_state_t::pending) {
          builder_enqueue(cat, def->oid, x);
        }
      }
    }
  }
  return 0;
}

/* Set an index's state; see sm3.h. */
int set_index_state(session_t *session, uint64_t table, uint64_t oid,
                    index_state_t state, sm1::errnum_t error,
                    std::string_view reason) {
  return cat_put_index_state(session, table, oid,
                             {state, error, std::string(reason)});
}

/* Create a collation; see sm3.h. */
int create_collation(session_t *session, uint64_t nspoid, std::string_view name,
                     std::string_view locale, bool deterministic,
                     uint64_t &oid) {
  if (name.empty()) {
    return invalid("create collation: no name");
  }
  uint64_t i;
  if (cat_new_oid(session, i) != 0 ||
      cat_put_collation(session, i, nspoid, name, locale, deterministic) != 0) {
    return -1;
  }
  oid = i;
  return 0;
}

/* Create a sequence; see sm3.h. */
int create_sequence(session_t *session, uint64_t nspoid, std::string_view name,
                    uint64_t start, uint64_t increment, uint64_t &oid) {
  if (name.empty()) {
    return invalid("create sequence: no name");
  }
  if (increment == 0) {
    return invalid("create sequence: an increment of 0");
  }
  uint64_t q;
  if (cat_new_oid(session, q) != 0 ||
      cat_put_sequence(session, q, nspoid, name, start, increment) != 0) {
    return -1;
  }
  oid = q;
  return 0;
}

/* Drop a sequence; see sm3.h. */
int drop_sequence(session_t *session, uint64_t oid) {
  return cat_del_sequence(session, oid);
}

/* A sequence's next value; see sm3.h. */
int nextval(session_t *session, uint64_t oid, uint64_t &out) {
  return sm1::nextseq(session->sm1, sequence_counter(oid), out);
}

} // namespace sm3
