/*
 * ddl.cc -- DDL (../CATALOG.md §1, §6): what a CREATE,
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
 * Whether the table oid holds a live row now, in live: its store's keys
 * may all be rows deleted, which hold none. The newest committed state is
 * read, a row committed since this transaction's snapshot among it; the
 * caller holds the lock X, or the table is its session's own.
 */
int has_live_row(sm3::session_t *session, uint64_t oid, bool temp, bool &live) {
  sm3::table_t *t = nullptr;
  if (0 != sm3::tbl_open(session, oid,
                         temp ? sm1::SM_LOCKMODE_IS : sm1::SM_LOCKMODE_EX, t)) {
    return -1;
  }
  if (0 != sm1::view_latest(session->sm1, true)) {
    sm3::tbl_close(t);
    return -1;
  }
  sm3::scan_t *scan = nullptr;
  int rc = sm3::tbl_scan_rows(t, scan);
  if (0 == rc) {
    uint64_t rowno = 0;
    std::string_view tuple;
    rc = sm3::tbl_scannxt(scan, rowno, tuple);
    sm3::tbl_scanend(scan);
  }
  sm3::tbl_close(t);
  if (0 != sm1::view_latest(session->sm1, false) || rc < 0) {
    return -1;
  }
  live = 0 == rc;
  return 0;
}

/*
 * Refuse a NOT NULL column with no missing value, name, unless the
 * table oid holds no row, which it then goes on holding.
 * 1. Take the table's table-oid lock X if it is free now. Busy is
 *    SM_ERR_BLOCKED.
 * 2. Check the store is empty, or holds no live row now: deleted rows
 *    leave keys behind. A live row is SM_ERR_NOTNULL.
 * 3. Keep the lock only if the table holds no row.
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
    bool live = false;
    if (sm1::store_empty(session->sm1, d->storeid, empty) != 0 ||
        (!empty && has_live_row(session, oid, true, live) != 0)) {
      return -1;
    }
    empty = empty || !live;
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
  bool live = false;
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
  } else if (!empty && has_live_row(session, oid, false, live) != 0) {
    rc = -1;
  } else if (!empty && live) {
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
    // no default, insert or update, as PostgreSQL's is.
    if ((c.identity != 'a' && c.identity != 'd') || c.identity_seq == 0 ||
        !c.not_null || c.default_expr || c.on_update_expr ||
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

static bool constraint_name_taken(const table_rec_t &rec, std::string_view name,
                                  uint64_t oid);
static int name_not_null(session_t *session, table_rec_t &rec, size_t at,
                         std::string_view name);

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
                  "sm3.id." + std::to_string(t),
                  first_index_relid,
                  {},
                  {},
                  {},
                  {}};
  for (const column_spec_t &c : columns) {
    rec.columns.push_back({c.name, c.type_code, c.typmod, c.not_null, false,
                           std::nullopt, std::nullopt, c.default_expr,
                           c.identity, c.identity_seq, c.on_update_expr});
  }
  // Each NOT NULL column's constraint, in the columns' order.
  for (size_t i = 0; i < columns.size(); i++) {
    if (columns[i].not_null &&
        name_not_null(session, rec, i, columns[i].not_null_name) != 0) {
      return -1;
    }
  }
  // The table's store and its _id sequence (../DESIGN.md §5): for a
  // temporary table, both temporary, and so the session's alone
  // (../TEMPTABLE.md §4).
  const bool temp = nspoid == nsp_temp;
  // Its rows chained, a temporary table's too (../ROWSTORE.md §3.5).
  if (sm1::mkstore(session->sm1, rec.storeid, temp, true) != 0) {
    return -1;
  }
  // And the sequence its TOAST ids come from, used once the host
  // reads TOAST pointers (cat_set_toast).
  if (sm1::mkseq(session->sm1, rec.id_seq, 1, 1, temp) == 0 &&
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
    sm1::rmseq(session->sm1, rec.id_seq);
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

/*
 * Drop the sequences in nspoid that the table oid owns, by any of
 * its columns or by the column attnum alone. A sequence is owned
 * only in its table's namespace, so only that one is looked in.
 */
static int drop_owned_sequences(session_t *session, uint64_t oid,
                                uint64_t nspoid, int32_t attnum) {
  std::vector<std::string> names;
  if (nspoid == nsp_temp || cat_list_sequences(session, nspoid, names) != 0) {
    return nspoid == nsp_temp ? 0 : -1;
  }
  for (const std::string &name : names) {
    uint64_t q;
    sequence_rec_t rec;
    if (cat_get_sequence(session, nspoid, name, q) != 0) {
      return -1;
    }
    // The names are the shared catalog's: one committed since the
    // snapshot has no record there, and this table cannot own it.
    if (cat_get_sequence_rec(session, q, false, rec) != 0) {
      if (sm1::SM_ERR_NOTFOUND == sm1::errnum()) {
        continue;
      }
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

/*
 * Drop the sequences in nspoid whose owner table is gone; see
 * internal.h. A drop can miss a sequence a concurrent own_sequence
 * made the table's after the drop's snapshot (../SEQUENCE.md §3.5).
 * 1. List the sequences of the namespace.
 * 2. Read each record, skipping one with no owner or a live owner.
 * 3. Lock the record, read it again, and drop it if its owner is
 *    still gone.
 */
int reap_orphan_sequences(session_t *session, uint64_t nspoid) {
  std::vector<std::string> names;
  if (nspoid == nsp_temp || cat_list_sequences(session, nspoid, names) != 0) {
    return nspoid == nsp_temp ? 0 : -1;
  }
  for (const std::string &name : names) {
    uint64_t q;
    sequence_rec_t rec;
    if (cat_get_sequence(session, nspoid, name, q) != 0) {
      return -1;
    }
    // One gone since the list has nothing to reap.
    if (cat_get_sequence_rec(session, q, false, rec) != 0) {
      if (sm1::SM_ERR_NOTFOUND == sm1::errnum()) {
        continue;
      }
      return -1;
    }
    if (!rec.owner || cat_get_table(session, *rec.owner)) {
      continue;
    }
    // Locked and read again: the drop is of the newest record.
    if (cat_get_sequence_rec(session, q, true, rec) != 0) {
      if (sm1::SM_ERR_NOTFOUND == sm1::errnum()) {
        continue;
      }
      return -1;
    }
    if (rec.owner && !cat_get_table(session, *rec.owner) &&
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
  // A table a view reads stays (../VIEW.md §5).
  std::vector<view_reader_t> readers = cat_view_readers(session, oid);
  if (!readers.empty()) {
    return sm1::seterr(sm1::SM_ERR_INUSE, "drop table: " + rec.name +
                                              " is read by view " +
                                              readers[0].def->name);
  }
  // A parent another table's foreign key references stays; one of its
  // own goes with it (../FOREIGNKEY.md §5).
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
  // Its store takes its rows and index entries with it (../DESIGN.md §5).
  if (sm1::rmstore(session->sm1, rec.storeid) != 0 ||
      sm1::rmseq(session->sm1, rec.id_seq) != 0 ||
      sm1::rmseq(session->sm1, toast_seq_name(oid)) != 0) {
    return -1;
  }
  // Its commit sweeps the namespace for sequences it missed.
  if (rec.nspoid != nsp_temp &&
      std::find(session->drop_nsps.begin(), session->drop_nsps.end(),
                rec.nspoid) == session->drop_nsps.end()) {
    session->drop_nsps.push_back(rec.nspoid);
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
  // if the table is; the index relids and the _id sequence carry
  // over.
  const bool temp = rec.nspoid == nsp_temp;
  if (sm1::mkstore(session->sm1, rec.storeid, temp, true) != 0 ||
      cat_put_table(session, oid, rec) != 0) {
    return -1;
  }
  // Each tree, empty, in the new store, which the statement's mark,
  // moved past its making, lets it write (sm1::stmt_advance()).
  if (sm1::stmt_advance(session->sm1) != 0) {
    return -1;
  }
  for (const index_rec_t &x : rec.indexes) {
    if (x.tree &&
        sm2::idx_create_tree(session->sm1, rec.storeid, x.relid) != 0) {
      return -1;
    }
  }
  // The old store goes at commit, taking the rows and index
  // entries with it (../DESIGN.md §5).
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
       std::nullopt, col.default_expr, 0, 0, col.on_update_expr});
  if (col.not_null && name_not_null(session, rec, rec.columns.size() - 1,
                                    col.not_null_name) != 0) {
    return -1;
  }
  return cat_put_table(session, oid, rec);
}

/* Make a column NOT NULL, once no row holds a null in it; see sm3.h. */
int set_not_null(session_t *session, uint64_t oid, std::string_view name,
                 std::string_view constraint) {
  table_rec_t rec;
  if (0 != cat_get_table_rec(session, oid, rec)) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "set not null: no column " + std::string(name));
  }
  if (rec.columns[at].not_null) {
    return 0;
  }
  // Open exclusive, so no other transaction writes a row between
  // the scan and the catalog's change.
  table_t *t = nullptr;
  if (0 != tbl_open(session, oid, sm1::SM_LOCKMODE_EX, t)) {
    return -1;
  }
  // Every row as it is now, one committed since this transaction's
  // snapshot among them: under the lock X it stays so.
  if (0 != sm1::view_latest(session->sm1, true)) {
    tbl_close(t);
    return -1;
  }
  scan_t *scan = nullptr;
  int rc = tbl_scan_rows(t, scan);
  uint64_t rowno = 0;
  std::string_view tuple;
  while (0 == rc && 0 == (rc = tbl_scannxt(scan, rowno, tuple))) {
    sm2::datum_t d;
    bool isnull = false;
    // A row older than the column reads its missing value, or null.
    if (0 != sm2::tup_decode(tbl_def(t).desc, tuple, at, d, isnull)) {
      rc = -1;
    } else if (isnull) {
      rc = sm1::seterr(sm1::SM_ERR_NOTNULL, "set not null: column " +
                                                std::string(name) +
                                                " holds a null");
    }
  }
  tbl_scanend(scan);
  tbl_close(t);
  if (0 != sm1::view_latest(session->sm1, false)) {
    return -1;
  }
  // The scan's end, 1, is the one way through.
  if (1 != rc) {
    return -1;
  }
  rec.columns[at].not_null = true;
  if (0 != name_not_null(session, rec, at, constraint)) {
    return -1;
  }
  return cat_put_table(session, oid, rec);
}

/* Rename a column's NOT NULL constraint; see sm3.h. */
int rename_not_null(session_t *session, uint64_t oid, std::string_view name,
                    std::string_view to) {
  table_rec_t rec;
  if (0 != cat_get_table_rec(session, oid, rec)) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0 || 0 == rec.columns[at].not_null_oid) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "rename not null: no NOT NULL column " +
                           std::string(name));
  }
  column_rec_t &c = rec.columns[at];
  if (to.empty() || constraint_name_taken(rec, to, c.not_null_oid)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY, "rename not null: constraint " +
                                               std::string(to) +
                                               " is taken, or empty");
  }
  c.not_null_name = std::string(to);
  return cat_put_table(session, oid, rec);
}

/* Let a column hold a null again; see sm3.h. */
int drop_not_null(session_t *session, uint64_t oid, std::string_view name) {
  table_rec_t rec;
  if (0 != cat_get_table_rec(session, oid, rec)) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "drop not null: no column " + std::string(name));
  }
  column_rec_t &c = rec.columns[at];
  if (!c.not_null) {
    return 0;
  }
  // An identity column is NOT NULL (check_columns).
  if (0 != c.identity) {
    return invalid("drop not null: " + c.name + " is an identity column");
  }
  // A primary key's columns are NOT NULL (create_index).
  for (const index_rec_t &x : rec.indexes) {
    for (const index_key_rec_t &k : x.keys) {
      if (x.primary && k.attnum == at) {
        return sm1::seterr(sm1::SM_ERR_INUSE, "drop not null: " + c.name +
                                                  " is in the primary key " +
                                                  x.name);
      }
    }
  }
  c.not_null = false;
  c.not_null_oid = 0;
  c.not_null_name.clear();
  return cat_put_table(session, oid, rec);
}

/* Set or drop a column's default; see sm3.h. */
int set_default(session_t *session, uint64_t oid, std::string_view name,
                std::optional<std::string_view> expr) {
  table_rec_t rec;
  if (0 != cat_get_table_rec(session, oid, rec)) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "set default: no column " + std::string(name));
  }
  column_rec_t &c = rec.columns[at];
  // An identity column has no default (check_columns).
  if (0 != c.identity) {
    return invalid("set default: " + c.name + " is an identity column");
  }
  c.default_expr = expr ? std::optional<std::string>(*expr) : std::nullopt;
  return cat_put_table(session, oid, rec);
}

/*
 * What keeps rec's column at from changing type, a view first, into
 * use, with its name in by; see sm3.h. A plain index takes the new
 * type from the column.
 */
static int find_column_use(session_t *session, uint64_t oid,
                           const table_rec_t &rec, int32_t at,
                           column_use_t &use, std::string &by) {
  auto in = [at](const std::vector<int32_t> &v) {
    return std::find(v.begin(), v.end(), at) != v.end();
  };
  auto found = [&](column_use_t kind, const std::string &name) {
    use = kind;
    by = name;
    return 0;
  };
  // Another session's temporary view among them (../VIEW.md §12.3).
  for (const view_reader_t &v : cat_view_readers(session, oid)) {
    for (const view_read_t &r : v.def->reads) {
      if (r.relation == oid && r.attnum == at) {
        return found(column_use_t::view, v.def->name);
      }
    }
  }
  for (const index_rec_t &x : rec.indexes) {
    if (!x.eval.empty() && in(x.reads)) {
      return found(column_use_t::index, x.name);
    }
  }
  for (const fkey_rec_t &fk : rec.fkeys) {
    if (in(fk.attnums)) {
      return found(column_use_t::fkey, fk.name);
    }
  }
  for (const check_rec_t &c : rec.checks) {
    if (in(c.reads)) {
      return found(column_use_t::check, c.name);
    }
  }
  for (const fkey_ref_t &ref : rec.referenced_by) {
    table_rec_t child;
    if (cat_get_table_rec(session, ref.table, child) != 0) {
      return -1;
    }
    for (const fkey_rec_t &fk : child.fkeys) {
      if (fk.oid == ref.fkey && in(fk.ref_attnums)) {
        return found(column_use_t::referenced, fk.name);
      }
    }
  }
  return found(0 != rec.columns[at].identity ? column_use_t::identity
                                             : column_use_t::none,
               "");
}

/* What keeps a column from changing type; see sm3.h. */
int column_use(session_t *session, uint64_t oid, int32_t at, column_use_t &use,
               std::string &by) {
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  if (at < 0 || static_cast<size_t>(at) >= rec.columns.size()) {
    return invalid("column use: no such column");
  }
  return find_column_use(session, oid, rec, at, use, by);
}

/* Whether a column's values stay with no converter; see sm3.h. */
bool retype_keeps_values(uint8_t from_code, int32_t from_typmod,
                         uint8_t to_code, int32_t to_typmod) {
  auto text_like = [](uint8_t code) {
    return code == sm2::type_text.code || code == sm2::type_varchar.code;
  };
  // text and varchar store the same bytes and the same key
  // (../sm2/TYPE.md §3).
  return (from_code == to_code && from_typmod == to_typmod) ||
         (text_like(from_code) && text_like(to_code) &&
          (to_typmod < 0 || (from_typmod >= 0 && to_typmod >= from_typmod)));
}

/* Whether a value of the type code takes a collation. */
static bool collatable(uint8_t code) {
  return code == sm2::type_text.code || code == sm2::type_varchar.code ||
         code == sm2::type_bpchar.code;
}

/*
 * Change a column's type; see sm3.h.
 * 1. Lock the table exclusive, so no row is read or written meanwhile.
 * 2. Refuse a column that is not there, an identity column, one
 *    something would read stale (find_column_use()), and a change
 *    with no converter whose values would not stay as they are.
 * 3. Give the record the new type, default and update default.
 * 4. With a converter and rows, rewrite the table, each row's
 *    value converted; else put the record alone. The put erases
 *    the column's statistics, of the old type.
 */
int alter_column_type(session_t *session, uint64_t oid, std::string_view name,
                      const column_type_t &to, const column_conv_fn *conv) {
  const sm2::type_t *type = sm2::type_by_code(to.type_code);
  if (!type || sm2::datum_width(type, to.typmod) == 0) {
    return invalid("alter column type: a type code sm2 lacks, or a typmod "
                   "its type refuses");
  }
  // No other transaction reads or writes a row until this one ends.
  std::shared_ptr<const table_def_t> def;
  if (cat_lock_table(session, oid, sm1::SM_LOCKMODE_EX, def) != 0) {
    return -1;
  }
  table_rec_t rec;
  if (cat_get_table_rec(session, oid, rec) != 0) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "alter column type: no column " + std::string(name));
  }
  column_use_t use;
  std::string by;
  if (find_column_use(session, oid, rec, at, use, by) != 0) {
    return -1;
  }
  column_rec_t &c = rec.columns[at];
  if (column_use_t::identity == use) {
    return invalid("alter column type: " + c.name + " is an identity column");
  }
  if (column_use_t::none != use) {
    return sm1::seterr(sm1::SM_ERR_INUSE,
                       "alter column type: " + c.name + " is used by " + by);
  }
  // Without a converter every stored value must stay a value of the
  // new type.
  if (!conv &&
      !retype_keeps_values(c.type_code, c.typmod, to.type_code, to.typmod)) {
    return invalid("alter column type: " + c.name +
                   "'s values may change with its type, and no converter "
                   "was given");
  }
  c.type_code = to.type_code;
  c.typmod = to.typmod;
  c.default_expr = to.default_expr;
  c.on_update_expr = to.on_update_expr;
  // A type with no collation: the column has none, nor does any
  // index key on it, which create_index refuses for such a type.
  if (!collatable(to.type_code)) {
    c.collation = std::nullopt;
    for (index_rec_t &x : rec.indexes) {
      for (index_key_rec_t &k : x.keys) {
        if (k.attnum == at) {
          k.collation = std::nullopt;
        }
      }
    }
  }
  // A converted column has no row that reads its missing value of
  // the old type: a rewrite stores every row whole, and an empty
  // table has none.
  if (conv) {
    c.missing = std::nullopt;
  }
  bool empty = true;
  if (conv && sm1::store_empty(session->sm1, rec.storeid, empty) != 0) {
    return -1;
  }
  if (conv && !empty) {
    column_conv_t cv{at, conv};
    return rewrite_table(session, oid, std::move(rec), &cv);
  }
  return cat_put_table(session, oid, rec);
}

/* Make a column an identity column, owning its sequence; see sm3.h. */
int set_identity(session_t *session, uint64_t oid, std::string_view name,
                 char identity, uint64_t seq) {
  table_rec_t rec;
  if (0 != cat_get_table_rec(session, oid, rec)) {
    return -1;
  }
  int at = live_column(rec, name);
  if (at < 0) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "set identity: no column " + std::string(name));
  }
  column_rec_t &c = rec.columns[at];
  const sm2::type_t *type = sm2::type_by_code(c.type_code);
  // As create_table checks an identity column (check_columns).
  if (('a' != identity && 'd' != identity) || 0 == seq || 0 != c.identity ||
      !c.not_null || c.default_expr ||
      (type != &sm2::type_int2 && type != &sm2::type_int4 &&
       type != &sm2::type_int8)) {
    return invalid("set identity: column " + std::string(name) +
                   " is not a NOT NULL smallint, integer or bigint with no "
                   "default");
  }
  if (nsp_temp == rec.nspoid) {
    return invalid("set identity: column " + std::string(name) +
                   " in a temporary table, whose sequence would be");
  }
  // Read under its lock, as make_table reads one.
  sequence_rec_t s;
  if (0 != cat_get_sequence_rec(session, seq, true, s)) {
    return -1;
  }
  if (s.nspoid != rec.nspoid || s.owner) {
    return invalid("set identity: column " + std::string(name) +
                   "'s sequence is in another namespace, or owned");
  }
  c.identity = identity;
  c.identity_seq = seq;
  s.owner = oid;
  s.owner_attnum = at;
  if (0 != cat_put_table(session, oid, rec)) {
    return -1;
  }
  return cat_put_sequence_rec(session, seq, s);
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
  // A check reading it is the host's to drop first (../CHECK.md §5).
  for (const check_rec_t &c : rec.checks) {
    if (std::find(c.reads.begin(), c.reads.end(), at) != c.reads.end()) {
      return sm1::seterr(sm1::SM_ERR_INUSE, "drop column: check " + c.name +
                                                " reads " + std::string(name));
    }
  }
  // A column a view reads stays (../VIEW.md §5).
  // Another session's temporary view among them (../VIEW.md §12.3).
  for (const view_reader_t &v : cat_view_readers(session, oid)) {
    for (const view_read_t &r : v.def->reads) {
      if (r.relation == oid && r.attnum == at) {
        return sm1::seterr(sm1::SM_ERR_INUSE, "drop column: view " +
                                                  v.def->name + " reads " +
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
  c.not_null_oid = 0;
  c.not_null_name.clear();
  c.missing = std::nullopt;
  return cat_put_table(session, oid, rec);
}

/*
 * Put rec, the table oid's record with its primary key added or
 * dropped, keying its rows anew (../CLUSTER.md §8): an empty store
 * takes the new key as it is, under the table-oid lock exclusive,
 * and one with rows is rewritten into a new store.
 */
static int rekey(session_t *session, uint64_t oid, table_rec_t rec) {
  std::shared_ptr<const table_def_t> def;
  bool empty;
  // No other transaction writes a row meanwhile, nor reads one.
  if (cat_lock_table(session, oid, sm1::SM_LOCKMODE_EX, def) != 0 ||
      sm1::store_empty(session->sm1, rec.storeid, empty) != 0) {
    return -1;
  }
  return empty ? cat_put_table(session, oid, rec)
               : rewrite_table(session, oid, std::move(rec));
}

/* Create a plain index; see sm3.h. */
int create_index(session_t *session, uint64_t table, std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique, uint64_t &oid,
                 bool primary, bool constraint, bool tree) {
  return create_index(session, table, name, keys, includes, unique, {}, {}, oid,
                      primary, constraint, tree);
}

/* Create an index, plain, partial or of expressions; see sm3.h. */
int create_index(session_t *session, uint64_t table, std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 std::string_view eval, std::span<const int32_t> reads,
                 uint64_t &oid, bool primary, bool constraint, bool tree) {
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
  x.constraint = constraint;
  x.tree = tree;
  x.eval = std::string(eval);
  // A primary key's entries are the rows, which a tree's are not.
  if (tree && primary) {
    return invalid("create index: a tree that is a primary key");
  }
  if (constraint && (!unique || !eval.empty())) {
    return invalid("create index: a constraint's index not unique, or not "
                   "on columns alone");
  }
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
  // A constraint's name is unique among the table's constraints.
  if (constraint && constraint_name_taken(rec, name, 0)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "create index: constraint " + x.name + " is taken");
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
      if (!collatable(code)) {
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
  // 0xFFFF is TOAST's and 0xFFFE reserved, once the _id index's
  // (../ROWSTORE.md §8); relids are never reused.
  if (rec.next_relid >= sm2::id_relid) {
    return invalid("create index: the table has no index relid left");
  }
  uint64_t i;
  if (cat_new_oid(session, i) != 0) {
    return -1;
  }
  x.oid = i;
  rec.next_relid++;
  rec.indexes.push_back(std::move(x));
  // The rows are keyed by a primary key, which has no entries to
  // build: they are its entries (../CLUSTER.md §2).
  if (primary) {
    if (rekey(session, table, std::move(rec)) != 0 ||
        cat_put_index_state(session, table, i,
                            {index_state_t::valid, sm1::SM_OK, {}, {}}) != 0) {
      return -1;
    }
    oid = i;
    return 0;
  }
  // The put takes the index's name: one taken fails here.
  if (cat_put_table(session, table, rec) != 0) {
    return -1;
  }
  // A tree's pages, empty, before any writer or the build adds to
  // them; written outside the transaction, they are left by a
  // rollback, under a relid never given again (../BTREEINDEX.md §2).
  if (tree && sm2::idx_create_tree(session->sm1, rec.storeid,
                                   rec.indexes.back().relid) != 0) {
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
    std::string from;
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
                            {index_state_t::valid, sm1::SM_OK, {}, {}}) != 0) {
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
  // A primary key's entries are the rows, which are keyed anew.
  if (it->primary) {
    rec.indexes.erase(it);
    return rekey(session, table, std::move(rec));
  }
  // A tree's pages are no transaction's, and a rollback brings the
  // index back: they go once the drop has committed, under a relid
  // never given again; a crash first leaves them until the store
  // goes.
  if (it->tree) {
    sm1::session_t *s = session->sm1;
    uint64_t storeid = rec.storeid;
    uint16_t relid = it->relid;
    rec.indexes.erase(it);
    if (cat_put_table(session, table, rec) != 0) {
      return -1;
    }
    return sm1::on_commit(s, [s, storeid, relid] {
      // Its failure leaves the pages, as a crash would.
      sm2::idx_drop_tree(s, storeid, relid);
    });
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
    int erc = sm1::erase(session->sm1, rec.storeid, e);
    if (0 != erc && sm1::SM_ERR_UPDATED == sm1::errnum()) {
      // At read committed the entry was committed after this
      // transaction began, as an index build's batch commits them:
      // the newest version read lets sm1 take the erase again.
      std::string_view latest;
      if (sm1::read_latest(session->sm1, rec.storeid, e, latest) < 0) {
        return -1;
      }
      erc = sm1::erase(session->sm1, rec.storeid, e);
    }
    if (0 != erc) {
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
                    std::string_view reason, const sm1::dupkey_t &dup) {
  return cat_put_index_state(session, table, oid,
                             {state, error, std::string(reason), dup});
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

/*
 * Whether name is taken among rec's constraints, but by the one of
 * oid: its foreign keys, its checks, and its primary key's and unique
 * constraints' indexes, as a constraint's name is unique per table.
 */
static bool constraint_name_taken(const table_rec_t &rec, std::string_view name,
                                  uint64_t oid) {
  for (const fkey_rec_t &fk : rec.fkeys) {
    if (fk.oid != oid && fk.name == name) {
      return true;
    }
  }
  for (const check_rec_t &c : rec.checks) {
    if (c.oid != oid && c.name == name) {
      return true;
    }
  }
  for (const index_rec_t &x : rec.indexes) {
    if (x.constraint && x.oid != oid && x.name == name) {
      return true;
    }
  }
  for (const column_rec_t &c : rec.columns) {
    if (c.not_null_oid != 0 && c.not_null_oid != oid &&
        c.not_null_name == name) {
      return true;
    }
  }
  return false;
}

/*
 * Give rec's column at, NOT NULL, its constraint (../NOTNULL.md §1): a
 * new oid, and the name name, or with none table_column_not_null, a
 * number after it if that is taken. A name given that is taken among
 * rec's constraints is SM_ERR_DUPKEY.
 */
static int name_not_null(session_t *session, table_rec_t &rec, size_t at,
                         std::string_view name) {
  column_rec_t &c = rec.columns[at];
  std::string chosen(name);
  if (chosen.empty()) {
    std::string base = rec.name + "_" + c.name + "_not_null";
    chosen = base;
    for (int n = 1; constraint_name_taken(rec, chosen, 0); n++) {
      chosen = base + std::to_string(n);
    }
  } else if (constraint_name_taken(rec, chosen, 0)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "not null: constraint " + chosen + " is taken");
  }
  if (cat_new_oid(session, c.not_null_oid) != 0) {
    return -1;
  }
  c.not_null_name = std::move(chosen);
  return 0;
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
  // The child's lock X, then the parent's (../FOREIGNKEY.md §7).
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
  if (constraint_name_taken(child, def.name, 0)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "create foreign key: " + def.name + " is taken");
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
  return cat_validate_fkey(session, table, oid);
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
  if (constraint_name_taken(child, name, oid)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY, "rename foreign key: " +
                                               std::string(name) + " is taken");
  }
  fk->name = name;
  return cat_put_table(session, table, child);
}

/* The check constraint oid of rec, or nullptr. */
static check_rec_t *check_in(table_rec_t &rec, uint64_t oid) {
  for (check_rec_t &c : rec.checks) {
    if (c.oid == oid) {
      return &c;
    }
  }
  return nullptr;
}

/* The table's record, under its lock X, and its check oid in out. */
static int check_of(session_t *session, uint64_t table, uint64_t oid,
                    const char *what, table_rec_t &rec, check_rec_t *&out) {
  if (cat_get_table_rec(session, table, rec) != 0) {
    return -1;
  }
  out = check_in(rec, oid);
  if (!out) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       std::string(what) + ": no check " + std::to_string(oid));
  }
  return 0;
}

/* Add a check constraint to the table table; see sm3.h. */
int create_check(session_t *session, uint64_t table, const check_rec_t &def,
                 uint64_t &oid) {
  if (def.name.empty()) {
    return invalid("create check: no name");
  }
  table_rec_t rec;
  if (cat_get_table_rec(session, table, rec) != 0) {
    return -1;
  }
  for (int32_t at : def.reads) {
    if (!live_attnum(rec, at)) {
      return invalid("create check: a column that is not there");
    }
  }
  if (constraint_name_taken(rec, def.name, 0)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "create check: " + def.name + " is taken");
  }
  if (cat_new_oid(session, oid) != 0) {
    return -1;
  }
  rec.checks.push_back(def);
  rec.checks.back().oid = oid;
  return cat_put_table(session, table, rec);
}

/* Drop a check constraint of the table table; see sm3.h. */
int drop_check(session_t *session, uint64_t table, uint64_t oid) {
  table_rec_t rec;
  check_rec_t *c = nullptr;
  if (check_of(session, table, oid, "drop check", rec, c) != 0) {
    return -1;
  }
  std::erase_if(rec.checks, [&](const check_rec_t &k) { return k.oid == oid; });
  return cat_put_table(session, table, rec);
}

/* Mark a check constraint's rows checked; see sm3.h. */
int validate_check(session_t *session, uint64_t table, uint64_t oid) {
  table_rec_t rec;
  check_rec_t *c = nullptr;
  if (check_of(session, table, oid, "validate check", rec, c) != 0) {
    return -1;
  }
  c->validated = true;
  return cat_put_table(session, table, rec);
}

/* Rename a check constraint; see sm3.h. */
int rename_check(session_t *session, uint64_t table, uint64_t oid,
                 std::string_view name) {
  if (name.empty()) {
    return invalid("rename check: no name");
  }
  table_rec_t rec;
  check_rec_t *c = nullptr;
  if (check_of(session, table, oid, "rename check", rec, c) != 0) {
    return -1;
  }
  if (constraint_name_taken(rec, name, oid)) {
    return sm1::seterr(sm1::SM_ERR_DUPKEY,
                       "rename check: " + std::string(name) + " is taken");
  }
  c->name = name;
  return cat_put_table(session, table, rec);
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
 * transaction, and check it is one a view may read (../VIEW.md §5): a
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
  // Another session's temporary view has no lock: its session alone
  // uses it, and drops it at its next begin (../VIEW.md §12.4).
  std::shared_ptr<const view_def_t> old = other_view(session, oid);
  bool other = old != nullptr;
  // Exclusive: this waits out the view's readers, and a view being
  // made on it, which then fails to find it.
  if (!other && cat_lock_view(session, oid, sm1::SM_LOCKMODE_EX, old) != 0) {
    return -1;
  }
  std::vector<view_reader_t> readers = cat_view_readers(session, oid);
  if (!readers.empty()) {
    return sm1::seterr(sm1::SM_ERR_INUSE, "drop view: " + old->name +
                                              " is read by view " +
                                              readers[0].def->name);
  }
  if (other) {
    overlay_other_drop(session, oid);
    return 0;
  }
  return cat_del_view(session, oid, *old);
}

} // namespace sm3
