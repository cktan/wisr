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
#include <functional>
#include <memory>
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

/*
 * Refuse a NOT NULL column with no missing value, name, unless the
 * table oid holds no row, which it then goes on holding: its
 * table-oid lock X is taken if it is free now, and kept only if
 * the store is empty. Busy is SM_ERR_BLOCKED; rows, SM_ERR_NOTNULL.
 */
int need_empty(sm3::session_t *session, uint64_t oid, const std::string &name) {
  std::shared_ptr<const sm3::table_def_t> d = sm3::cat_get_table(session, oid);
  if (!d) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "table " + std::to_string(oid) + " does not exist");
  }
  bool empty;
  // A temporary table is the session's alone: no one to keep out.
  if (d->nspoid == sm3::nsp_temp) {
    if (sm1::store_empty(session->sm1, d->storeid, empty) != 0) {
      return -1;
    }
    return empty ? 0
                 : sm1::seterr(sm1::SM_ERR_NOTNULL,
                               "add column: " + name +
                                   " is NOT NULL, and the table has rows");
  }
  // A mark to roll back to, which lets go of the lock taken after
  // it and keeps one held from before.
  int depth = sm3::savepoint(session);
  if (depth < 0) {
    return -1;
  }
  int held = sm3::lock_table_now(session->sm1, oid, sm1::SM_LOCKMODE_EX);
  int rc = 0;
  if (held < 0) {
    rc = -1;
  } else if (held == 1) {
    rc = sm1::seterr(sm1::SM_ERR_BLOCKED,
                     "add column: " + name +
                         " is NOT NULL, and the table is in use");
  } else if (!(d = sm3::cat_get_table(session, oid))) {
    // Dropped while the lock was free.
    rc = sm1::seterr(sm1::SM_ERR_NOTFOUND,
                     "table " + std::to_string(oid) + " does not exist");
  } else if (sm1::store_empty(session->sm1, d->storeid, empty) != 0) {
    rc = -1;
  } else if (!empty) {
    rc = sm1::seterr(sm1::SM_ERR_NOTNULL,
                     "add column: " + name +
                         " is NOT NULL, and the table has rows");
  }
  if (rc != 0) {
    // The error is set already; the rollback only lets the lock go.
    sm3::rollback(session, depth);
  }
  sm3::forget(session, depth);
  return rc;
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
    if (c.identity == 0) {
      continue;
    }
    // An identity column: a NOT NULL integer with a sequence and
    // no default, as PostgreSQL's is.
    if ((c.identity != 'a' && c.identity != 'd') || c.identity_seq == 0 ||
        !c.not_null || c.default_expr ||
        (type != &sm2::type_int2 && type != &sm2::type_int4 &&
         type != &sm2::type_int8)) {
      return invalid("create table: identity column " + c.name +
                     " is not a NOT NULL smallint, integer or bigint with "
                     "a sequence and no default");
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

/*
 * Create a table in nspoid, or a temporary one if nspoid is
 * nsp_temp: what create_table and create_temp_table share.
 */
static int make_table(session_t *session, uint64_t nspoid,
                      std::string_view name,
                      std::span<const column_spec_t> columns, uint64_t &oid) {
  if (name.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "create table: no name");
  }
  if (check_columns(columns) != 0) {
    return -1;
  }
  // Each identity column's sequence, read under its lock: in the
  // table's namespace and owned by none, so the column can own it.
  std::vector<std::pair<int32_t, sequence_rec_t>> owned;
  for (size_t i = 0; i < columns.size(); i++) {
    const column_spec_t &c = columns[i];
    if (c.identity == 0) {
      continue;
    }
    if (nspoid == nsp_temp) {
      return invalid("create table: identity column " + c.name +
                     " in a temporary table, whose sequence would be");
    }
    sequence_rec_t seq;
    if (cat_get_sequence_rec(session, c.identity_seq, true, seq) != 0) {
      return -1;
    }
    if (seq.nspoid != nspoid || seq.owner) {
      return invalid("create table: identity column " + c.name +
                     "'s sequence is in another namespace, or owned");
    }
    owned.push_back({static_cast<int32_t>(i), std::move(seq)});
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
                           std::nullopt, std::nullopt, c.default_expr,
                           c.identity, c.identity_seq});
  }
  // The table's store and its rowno sequence (DESIGN.md §5): for a
  // temporary table, both temporary, and so the session's alone
  // (TEMPTABLE.md §4).
  const bool temp = nspoid == nsp_temp;
  if (sm1::mkstore(session->sm1, rec.storeid, temp) != 0) {
    return -1;
  }
  // And the sequence its TOAST ids come from, used once the host
  // reads TOAST pointers (cat_set_toast).
  if (sm1::mkseq(session->sm1, rec.rowno_seq, 1, 1, temp) == 0 &&
      sm1::mkseq(session->sm1, toast_seq_name(t), 1, 1, temp) == 0 &&
      cat_put_table(session, t, rec) == 0) {
    // The identity columns own their sequences, as own_sequence
    // would make them.
    for (auto &[attnum, seq] : owned) {
      seq.owner = t;
      seq.owner_attnum = attnum;
      if (cat_put_sequence_rec(session, rec.columns[attnum].identity_seq,
                               seq) != 0) {
        return -1;
      }
    }
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
    sm1::rmseq(session->sm1, toast_seq_name(t));
    sm1::rmseq(session->sm1, rec.rowno_seq);
    sm1::rmstore(session->sm1, rec.storeid);
    sm1::seterr(error, reason);
  }
  return -1;
}

/* Create a table; see sm3.h. */
int create_table(session_t *session, uint64_t nspoid, std::string_view name,
                 std::span<const column_spec_t> columns, uint64_t &oid) {
  // No namespace is the temporary tables' alone (create_temp_table).
  if (nspoid == nsp_temp) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND, "namespace 0 does not exist");
  }
  return make_table(session, nspoid, name, columns, oid);
}

/* Create a temporary table; see sm3.h. */
int create_temp_table(session_t *session, std::string_view name,
                      std::span<const column_spec_t> columns, uint64_t &oid) {
  return make_table(session, nsp_temp, name, columns, oid);
}

/*
 * Drop the sequences in nspoid that the table oid owns, by any of
 * its columns or by the column attnum alone. A sequence is owned
 * only in its table's namespace, so only that one is looked in.
 */
/*
 * Whether a foreign key that references the table rec, under its
 * lock X, meets pred; by is then its name. Its child's definition is
 * read without the child's lock: a foreign key of it is made and
 * dropped only under rec's lock too.
 */
static bool referencing(session_t *session, const table_rec_t &rec,
                        const std::function<bool(const fkey_rec_t &)> &pred,
                        std::string &by) {
  for (const fkey_ref_t &r : rec.referenced_by) {
    std::shared_ptr<const table_def_t> child = cat_get_table(session, r.table);
    if (!child) {
      continue;
    }
    for (const fkey_rec_t &fk : child->rec.fkeys) {
      if (fk.oid == r.fkey && pred(fk)) {
        by = fk.name;
        return true;
      }
    }
  }
  return false;
}

/*
 * Take the foreign key fkey out of the referenced_by of its parent,
 * the table parent, under its lock X.
 */
static int unreference(session_t *session, uint64_t parent, uint64_t fkey) {
  table_rec_t p;
  if (cat_get_table_rec(session, parent, p) != 0) {
    return -1;
  }
  std::erase_if(p.referenced_by,
                [&](const fkey_ref_t &r) { return r.fkey == fkey; });
  return cat_put_table(session, parent, p);
}

static int drop_owned_sequences(session_t *session, uint64_t oid,
                                uint64_t nspoid, int32_t attnum) {
  std::vector<std::string> names;
  if (nspoid == nsp_temp || cat_list_sequences(session, nspoid, names) != 0) {
    return nspoid == nsp_temp ? 0 : -1;
  }
  for (const std::string &name : names) {
    uint64_t q;
    sequence_rec_t rec;
    if (cat_get_sequence(session, nspoid, name, q) != 0 ||
        cat_get_sequence_rec(session, q, false, rec) != 0) {
      return -1;
    }
    if (rec.owner != oid || (attnum >= 0 && rec.owner_attnum != attnum)) {
      continue;
    }
    // Locked and read again: the drop is of the newest record.
    if (cat_get_sequence_rec(session, q, true, rec) != 0) {
      return -1;
    }
    if (rec.owner == oid && (attnum < 0 || rec.owner_attnum == attnum) &&
        cat_del_sequence(session, q) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Drop a table; see sm3.h. */
int drop_table(session_t *session, uint64_t oid) {
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  // A table a view reads stays (VIEW.md §5).
  std::vector<uint64_t> readers = cat_view_readers(session, oid);
  if (!readers.empty()) {
    return sm1::seterr(sm1::SM_ERR_INUSE, "drop table: " + rec.name +
                                              " is read by view " +
                                              std::to_string(readers[0]));
  }
  // A parent another table's foreign key references stays; one of its
  // own goes with it (FOREIGNKEY.md §5).
  for (const fkey_ref_t &r : rec.referenced_by) {
    if (r.table != oid) {
      return sm1::seterr(sm1::SM_ERR_INUSE,
                         "drop table: " + rec.name +
                             " is referenced by a foreign key of table " +
                             std::to_string(r.table));
    }
  }
  // Its foreign keys leave their parents' referenced_by.
  for (const fkey_rec_t &fk : rec.fkeys) {
    if (fk.ref_table != oid &&
        unreference(session, fk.ref_table, fk.oid) != 0) {
      return -1;
    }
  }
  if (drop_owned_sequences(session, oid, rec.nspoid, -1) != 0 ||
      cat_del_table(session, oid) != 0) {
    return -1;
  }
  // Its store takes its rows and index entries with it (DESIGN.md §5).
  if (sm1::rmstore(session->sm1, rec.storeid) != 0 ||
      sm1::rmseq(session->sm1, rec.rowno_seq) != 0) {
    return -1;
  }
  // A table made before TOAST has no sequence for its ids.
  if (sm1::rmseq(session->sm1, toast_seq_name(oid)) != 0 &&
      sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
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
  if (sm1::mkstore(session->sm1, rec.storeid, rec.nspoid == nsp_temp) != 0 ||
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
  if (col.identity != 0) {
    return invalid("add column: " + col.name + " is an identity column");
  }
  // No row older than it may read it as null.
  if (col.not_null && !missing && need_empty(session, oid, col.name) != 0) {
    return -1;
  }
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  if (live_column(rec, col.name) >= 0) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "add column: " + col.name + " is taken");
  }
  rec.columns.push_back(
      {col.name, col.type_code, col.typmod, col.not_null, false,
       missing ? std::optional<std::string>(*missing) : std::nullopt,
       std::nullopt, col.default_expr});
  return cat_put_table(session, oid, rec);
}

/* Rename a column; see sm3.h. */
int rename_column(session_t *session, uint64_t oid, std::string_view name,
                  std::string_view new_name) {
  if (new_name.empty()) {
    return invalid("rename column: no name");
  }
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "rename column: no column " + std::string(name));
  }
  if (name == new_name) {
    return 0;
  }
  if (live_column(rec, new_name) >= 0) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "rename column: " + std::string(new_name) + " is taken");
  }
  // An index names its columns by place, so only the name changes.
  rec.columns[at].name = new_name;
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
  for (const fkey_rec_t &fk : rec.fkeys) {
    if (std::find(fk.attnums.begin(), fk.attnums.end(), at) !=
        fk.attnums.end()) {
      return sm1::seterr(sm1::SM_ERR_INUSE, "drop column: foreign key " +
                                                fk.name + " uses " +
                                                std::string(name));
    }
  }
  // A column a view reads stays (VIEW.md §5).
  for (uint64_t v : cat_view_readers(session, oid)) {
    std::shared_ptr<const view_def_t> def = cat_get_view(session, v);
    for (const view_read_t &r : def->reads) {
      if (r.relation == oid && r.attnum == at) {
        return sm1::seterr(sm1::SM_ERR_INUSE, "drop column: view " + def->name +
                                                  " reads " +
                                                  std::string(name));
      }
    }
  }
  // A column a foreign key references is a key of the parent's
  // unique index, which the index's check above has refused.
  // The sequences it owns go with it, as PostgreSQL's do.
  if (drop_owned_sequences(session, oid, rec.nspoid, at) != 0) {
    return -1;
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
                 std::span<const int32_t> includes, bool unique, uint64_t &oid,
                 bool primary) {
  return create_index(session, table, name, keys, includes, unique, {}, {}, oid,
                      primary);
}

/* Create an index, plain, partial or of expressions; see sm3.h. */
int create_index(session_t *session, uint64_t table, std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 std::string_view eval, std::span<const int32_t> reads,
                 uint64_t &oid, bool primary) {
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
  x.primary = primary;
  x.eval = std::string(eval);
  if (primary && (!unique || !eval.empty())) {
    return invalid("create index: a primary key not unique, or not on "
                   "columns alone");
  }
  for (const index_rec_t &other : rec.indexes) {
    if (primary && other.primary) {
      return invalid("create index: the table has a primary key, " +
                     other.name);
    }
  }
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
    } else if (primary && !rec.columns[k.attnum].not_null) {
      return invalid("create index: a primary key on " +
                     rec.columns[k.attnum].name + ", which may be null");
    }
    if (k.collation) {
      uint8_t code = expr ? k.type_code : rec.columns[k.attnum].type_code;
      if (code != sm2::type_text.code && code != sm2::type_varchar.code &&
          code != sm2::type_bpchar.code) {
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
  std::string by;
  if (referencing(
          session, rec,
          [&](const fkey_rec_t &fk) { return fk.ref_index == oid; }, by)) {
    return sm1::seterr(sm1::SM_ERR_INUSE,
                       "drop index: foreign key " + by + " uses " + it->name);
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

/* Drop a sequence, if no column owns it; see sm3.h. */
int drop_sequence(session_t *session, uint64_t oid) {
  sequence_rec_t rec;
  if (cat_get_sequence_rec(session, oid, true, rec) != 0) {
    return -1;
  }
  if (rec.owner) {
    return sm1::seterr(sm1::SM_ERR_INUSE, "drop sequence: " + rec.name +
                                              " is owned by table " +
                                              std::to_string(*rec.owner));
  }
  return cat_del_sequence(session, oid);
}

/* Make a table's column own a sequence, or none; see sm3.h. */
int own_sequence(session_t *session, uint64_t oid, uint64_t table,
                 int32_t attnum) {
  sequence_rec_t rec;
  std::shared_ptr<const table_def_t> def;
  // The table first, as a drop of it takes it before the sequence:
  // held to the end of the transaction, so the table outlives this.
  if (table != 0 &&
      cat_lock_table(session, table, sm1::SM_LOCKMODE_IS, def) != 0) {
    return -1;
  }
  if (cat_get_sequence_rec(session, oid, true, rec) != 0) {
    return -1;
  }
  // An identity column's sequence is its for good.
  if (rec.owner) {
    table_rec_t owner;
    if (cat_get_table_rec(session, *rec.owner, owner) != 0) {
      return -1;
    }
    if (live_attnum(owner, rec.owner_attnum) &&
        owner.columns[rec.owner_attnum].identity_seq == oid) {
      return sm1::seterr(sm1::SM_ERR_INUSE,
                         "own sequence: " + rec.name +
                             " is an identity column's of table " + owner.name);
    }
  }
  if (table == 0) {
    rec.owner = std::nullopt;
    rec.owner_attnum = -1;
    return cat_put_sequence_rec(session, oid, rec);
  }
  if (def->nspoid != rec.nspoid) {
    return invalid("own sequence: table " + def->rec.name +
                   " is not in the sequence's namespace");
  }
  if (!live_attnum(def->rec, attnum)) {
    return invalid("own sequence: table " + def->rec.name + " has no column " +
                   std::to_string(attnum));
  }
  rec.owner = table;
  rec.owner_attnum = attnum;
  return cat_put_sequence_rec(session, oid, rec);
}

/* A sequence's next value; see sm3.h. */
int nextval(session_t *session, uint64_t oid, uint64_t &out) {
  return sm1::nextseq(session->sm1, sequence_counter(oid), out);
}

/* Whether attnum is a live column of rec. */
static bool live_attnum(const table_rec_t &rec, int32_t attnum) {
  return attnum >= 0 && static_cast<size_t>(attnum) < rec.columns.size() &&
         !rec.columns[attnum].dropped;
}

/* The foreign key oid of rec, or nullptr. */
static fkey_rec_t *fkey_in(table_rec_t &rec, uint64_t oid) {
  for (fkey_rec_t &fk : rec.fkeys) {
    if (fk.oid == oid) {
      return &fk;
    }
  }
  return nullptr;
}

/* Add a foreign key to the table table; see sm3.h. */
int create_fkey(session_t *session, uint64_t table, const fkey_rec_t &def,
                uint64_t &oid) {
  size_t n = def.attnums.size();
  if (def.name.empty()) {
    return invalid("create foreign key: no name");
  }
  if (n < 1 || n > 32 || def.ref_attnums.size() != n) {
    return invalid("create foreign key: 1 to 32 columns at each end, as "
                   "many at one as at the other");
  }
  if (std::string_view("arcnd").find(def.on_update) == std::string_view::npos ||
      std::string_view("arcnd").find(def.on_delete) == std::string_view::npos ||
      (def.match != 's' && def.match != 'f')) {
    return invalid("create foreign key: an action or a match type unknown");
  }
  // The child's lock X, then the parent's (FOREIGNKEY.md §7).
  table_rec_t child, parent;
  bool self = def.ref_table == table;
  if (cat_get_table_rec(session, table, child) != 0 ||
      (!self && cat_get_table_rec(session, def.ref_table, parent) != 0)) {
    return -1;
  }
  table_rec_t &p = self ? child : parent;
  if ((child.nspoid == nsp_temp) != (p.nspoid == nsp_temp)) {
    return invalid("create foreign key: between a temporary table and a "
                   "permanent one");
  }
  for (size_t i = 0; i < n; i++) {
    if (!live_attnum(child, def.attnums[i]) ||
        !live_attnum(p, def.ref_attnums[i])) {
      return invalid("create foreign key: a column that is not there");
    }
  }
  for (const fkey_rec_t &fk : child.fkeys) {
    if (fk.name == def.name) {
      return sm1::seterr(sm1::SM_ERR_DUPKEY,
                         "create foreign key: " + def.name + " is taken");
    }
  }
  // The parent's unique index on its columns, in any order, plain and
  // valid, which a check finds the parent row by.
  const index_rec_t *x = nullptr;
  for (const index_rec_t &i : p.indexes) {
    x = i.oid == def.ref_index ? &i : x;
  }
  bool fits = x && x->unique && x->eval.empty() && x->keys.size() == n;
  for (size_t i = 0; fits && i < n; i++) {
    fits = x->keys[i].attnum >= 0 &&
           std::find(def.ref_attnums.begin(), def.ref_attnums.end(),
                     x->keys[i].attnum) != def.ref_attnums.end();
  }
  if (!fits ||
      cat_get_index_state(session, x->oid).state != index_state_t::valid) {
    return invalid("create foreign key: no valid unique index of the "
                   "referenced table has its columns for keys");
  }
  if (cat_new_oid(session, oid) != 0) {
    return -1;
  }
  child.fkeys.push_back(def);
  child.fkeys.back().oid = oid;
  p.referenced_by.push_back({table, oid});
  if (cat_put_table(session, table, child) != 0 ||
      (!self && cat_put_table(session, def.ref_table, parent) != 0)) {
    return -1;
  }
  return 0;
}

/* Drop a foreign key of the table table; see sm3.h. */
int drop_fkey(session_t *session, uint64_t table, uint64_t oid) {
  table_rec_t child;
  if (cat_get_table_rec(session, table, child) != 0) {
    return -1;
  }
  fkey_rec_t *fk = fkey_in(child, oid);
  if (!fk) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "drop foreign key: no foreign key " +
                           std::to_string(oid));
  }
  uint64_t parent = fk->ref_table;
  std::erase_if(child.fkeys, [&](const fkey_rec_t &k) { return k.oid == oid; });
  if (parent == table) {
    std::erase_if(child.referenced_by,
                  [&](const fkey_ref_t &r) { return r.fkey == oid; });
    return cat_put_table(session, table, child);
  }
  if (cat_put_table(session, table, child) != 0) {
    return -1;
  }
  return unreference(session, parent, oid);
}

/* Mark a foreign key's rows checked; see sm3.h. */
int validate_fkey(session_t *session, uint64_t table, uint64_t oid) {
  table_rec_t child;
  if (cat_get_table_rec(session, table, child) != 0) {
    return -1;
  }
  fkey_rec_t *fk = fkey_in(child, oid);
  if (!fk) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "validate foreign key: no foreign key " +
                           std::to_string(oid));
  }
  fk->validated = true;
  return cat_put_table(session, table, child);
}

/* Rename a foreign key; see sm3.h. */
int rename_fkey(session_t *session, uint64_t table, uint64_t oid,
                std::string_view name) {
  if (name.empty()) {
    return invalid("rename foreign key: no name");
  }
  table_rec_t child;
  if (cat_get_table_rec(session, table, child) != 0) {
    return -1;
  }
  fkey_rec_t *fk = fkey_in(child, oid);
  if (!fk) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "rename foreign key: no foreign key " +
                           std::to_string(oid));
  }
  for (const fkey_rec_t &k : child.fkeys) {
    if (k.oid != oid && k.name == name) {
      return sm1::seterr(sm1::SM_ERR_DUPKEY,
                         "rename foreign key: " + std::string(name) +
                             " is taken");
    }
  }
  fk->name = name;
  return cat_put_table(session, table, child);
}

} // namespace sm3

namespace sm3 {

/* Whether the view from reads target, directly or through other views. */
static bool view_reaches(const session_t *session, uint64_t from,
                         uint64_t target) {
  std::unordered_set<uint64_t> seen;
  std::vector<uint64_t> todo{from};
  while (!todo.empty()) {
    uint64_t v = todo.back();
    todo.pop_back();
    if (v == target) {
      return true;
    }
    std::shared_ptr<const view_def_t> def = cat_get_view(session, v);
    if (!def || !seen.insert(v).second) {
      continue;
    }
    for (const view_read_t &r : def->reads) {
      todo.push_back(r.relation);
    }
  }
  return false;
}

/*
 * Whether the relation oid is one of session's temporary tables or
 * views, which it alone reaches, and so locks none of.
 */
static bool temp_relation(const session_t *session, uint64_t oid) {
  std::shared_ptr<const table_def_t> t = cat_get_table(session, oid);
  if (t) {
    return t->nspoid == nsp_temp;
  }
  std::shared_ptr<const view_def_t> v = cat_get_view(session, oid);
  return v && v->nspoid == nsp_temp;
}

/*
 * Lock each relation reads names intent_shared, to the end of the
 * transaction, and check it is one a view may read (VIEW.md §5): a
 * table, with a live column where one is named; or a view, with none
 * named, that does not read self, the view being replaced, or 0 for a
 * new one. A temporary relation is read by a temporary view alone,
 * temp, and takes no lock (§12.2).
 */
static int check_reads(session_t *session,
                       const std::vector<view_read_t> &reads, uint64_t self,
                       bool temp) {
  std::unordered_set<uint64_t> locked;
  for (const view_read_t &r : reads) {
    if (self != 0 && r.relation == self) {
      return invalid("view: view " + std::to_string(self) + " reads itself");
    }
    if (temp_relation(session, r.relation)) {
      if (!temp) {
        return invalid("view: a permanent view cannot read a temporary "
                       "relation");
      }
    } else if (locked.insert(r.relation).second &&
               lock_table(session->sm1, r.relation, sm1::SM_LOCKMODE_IS) != 0) {
      // Held to the end of the transaction, as a reader's: a drop of
      // the relation waits for this one, then finds the view.
      return -1;
    }
    // Read under the lock, from memory, which has every commit by now.
    std::shared_ptr<const table_def_t> t = cat_get_table(session, r.relation);
    if (t) {
      if (r.attnum != -1 && !live_attnum(t->rec, r.attnum)) {
        return invalid("view: table " + t->name + " has no live column " +
                       std::to_string(r.attnum));
      }
      continue;
    }
    if (!cat_get_view(session, r.relation)) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND, "view: relation " +
                                                   std::to_string(r.relation) +
                                                   " does not exist");
    }
    if (r.attnum != -1) {
      return invalid("view: a read of a view names no column");
    }
    if (self != 0 && view_reaches(session, r.relation, self)) {
      return invalid("view: view " + std::to_string(self) +
                     " reads itself through view " +
                     std::to_string(r.relation));
    }
  }
  return 0;
}

/* Create a view; see sm3.h. */
int create_view(session_t *session, uint64_t nspoid, std::string_view name,
                const view_def_t &def, uint64_t &oid) {
  if (name.empty()) {
    return invalid("create view: no name");
  }
  if (check_reads(session, def.reads, 0, nspoid == nsp_temp) != 0) {
    return -1;
  }
  view_def_t rec = def;
  rec.nspoid = nspoid;
  rec.name = std::string(name);
  uint64_t v;
  if (cat_new_oid(session, v) != 0 ||
      cat_put_view(session, v, rec, nullptr) != 0) {
    return -1;
  }
  oid = v;
  return 0;
}

/* Replace a view's query and reads; see sm3.h. */
int replace_view(session_t *session, uint64_t oid, const view_def_t &def) {
  std::shared_ptr<const view_def_t> old;
  // Exclusive: this waits out the view's readers, and they it.
  if (cat_lock_view(session, oid, sm1::SM_LOCKMODE_EX, old) != 0 ||
      check_reads(session, def.reads, oid, old->nspoid == nsp_temp) != 0) {
    return -1;
  }
  view_def_t rec = def;
  rec.nspoid = old->nspoid;
  rec.name = old->name;
  return cat_put_view(session, oid, rec, old.get());
}

/* Rename a view; see sm3.h. */
int rename_view(session_t *session, uint64_t oid, std::string_view name) {
  if (name.empty()) {
    return invalid("rename view: no name");
  }
  std::shared_ptr<const view_def_t> old;
  // Exclusive, as any DDL on the view.
  if (cat_lock_view(session, oid, sm1::SM_LOCKMODE_EX, old) != 0) {
    return -1;
  }
  if (old->name == name) {
    return 0;
  }
  view_def_t rec = *old;
  rec.name = std::string(name);
  return cat_put_view(session, oid, rec, old.get());
}

/* Drop a view, if no other view reads it; see sm3.h. */
int drop_view(session_t *session, uint64_t oid) {
  std::shared_ptr<const view_def_t> old;
  // Exclusive: this waits out the view's readers, and a view being
  // made on it, which then fails to find it.
  if (cat_lock_view(session, oid, sm1::SM_LOCKMODE_EX, old) != 0) {
    return -1;
  }
  std::vector<uint64_t> readers = cat_view_readers(session, oid);
  if (!readers.empty()) {
    return sm1::seterr(sm1::SM_ERR_INUSE, "drop view: " + old->name +
                                              " is read by view " +
                                              std::to_string(readers[0]));
  }
  return cat_del_view(session, oid, *old);
}

} // namespace sm3
