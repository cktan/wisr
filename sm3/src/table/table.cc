/*
 * table.cc -- an open table and its writes (SM3_DESIGN.md §3): the
 * heap write, then each index the write changes, with the old
 * entries computed from the image the heap write hands back.
 */

#include "../catalog/internal.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct sm3::table_t {
  session_t *session;
  std::shared_ptr<const table_def_t> def; /* held under the schema lock */
  bool writable;                          /* the lock covers intent_exclusive */
  bool fresh;                             /* made by this transaction */
  sm2::rel_t *rel = nullptr;
  std::vector<sm2::index_t *> indexes; /* one per def->indexes, in order */
};

namespace {

/* A row's values and null flags, as a write takes or a tuple gives. */
struct vals_t {
  std::span<const sm2::datum_t> d;
  std::span<const bool> null;
};

/* The values of the columns atts of a row, in atts' order. */
struct picked_t {
  /* Pick the columns atts out of r. */
  picked_t(const vals_t &r, const std::vector<int32_t> &atts)
      : m_null(new bool[atts.size()]) {
    for (size_t i = 0; i < atts.size(); i++) {
      m_d.push_back(r.d[atts[i]]);
      m_null[i] = r.null[atts[i]];
    }
  }

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return {m_null.get(), m_d.size()}; }

private:
  std::vector<sm2::datum_t> m_d;
  std::unique_ptr<bool[]> m_null;
};

/* A tuple deformed under a table's schema; it views the tuple. */
struct deformed_t {
  /* Make room for n attributes. */
  explicit deformed_t(size_t n) : d(n), null(new bool[n]), n(n) {}

  vals_t row() const { return {d, {null.get(), n}}; }

  std::vector<sm2::datum_t> d;
  std::unique_ptr<bool[]> null;
  size_t n;
};

/* Whether mode lets its holder write the table's rows. */
bool covers_write(sm1::lockmode_t mode) {
  return mode == sm1::lockmode_t::intent_exclusive ||
         mode == sm1::lockmode_t::shared_intent_exclusive ||
         mode == sm1::lockmode_t::exclusive;
}

/* Fail unless t was opened to write. */
int need_writable(const sm3::table_t *t) {
  return t->writable ? 0
                     : sm1::seterr(sm1::SM_ERR_INVALID,
                                   "table: opened to read, not to write");
}

/*
 * Fail with SM_ERR_NOTNULL if the row of t has a NULL in a column
 * declared NOT NULL: the one constraint sm3 enforces, before the
 * row is written, so nothing is half done (SM3_DESIGN.md §3).
 */
int need_not_null(const sm3::table_t *t, std::span<const bool> isnull) {
  const std::vector<sm3::column_def_t> &cols = t->def->columns;
  for (size_t i = 0; i < cols.size() && i < isnull.size(); i++) {
    if (cols[i].not_null && isnull[i]) {
      return sm1::seterr(sm1::SM_ERR_NOTNULL,
                         "null value in column " + cols[i].name + " of " +
                             t->def->name + ", which is NOT NULL");
    }
  }
  return 0;
}

/* A non-null datum's native bytes: a varlena's without its header. */
int native(const sm2::type_t *type, int32_t typmod, const sm2::datum_t &d,
           std::string_view &out) {
  if (sm2::datum_bytes(type, typmod, d, out) != 0) {
    return -1;
  }
  if (type->len < 0) {
    out = sm2::varlena_payload(out);
  }
  return 0;
}

/*
 * Whether the columns atts of a row are the same before and after:
 * 1 if each compares equal by its type's cmp, or with by_cmp off
 * holds the same bytes, 0 if not, -1 on error.
 */
int cols_same(const sm3::table_def_t &def, const std::vector<int32_t> &atts,
              const vals_t &before, const vals_t &after, bool by_cmp) {
  for (int32_t att : atts) {
    // An expression has no column here; the evaluator answers for it.
    if (att < 0) {
      continue;
    }
    if (before.null[att] || after.null[att]) {
      if (before.null[att] && after.null[att]) {
        continue;
      }
      return 0;
    }
    const sm2::attribute_t &a = def.schema[att];
    std::string_view b, c;
    if (native(a.type, a.typmod, before.d[att], b) != 0 ||
        native(a.type, a.typmod, after.d[att], c) != 0) {
      return -1;
    }
    if (by_cmp ? a.type->cmp(b, c, a.typmod) != 0 : b != c) {
      return 0;
    }
  }
  return 1;
}

/*
 * Whether index x's entry for a row is the same before and after,
 * told from the row's columns alone: 1 if its key columns compare
 * equal by their types' cmp, and its INCLUDE columns and the
 * columns its predicate and expressions read hold the same bytes;
 * 0 if not; -1 on error. The key folds what cmp calls equal, so cmp
 * decides it. An INCLUDE column is stored as written, and what the
 * host makes of a column it reads is the host's, so only their
 * bytes do.
 */
int unchanged(const sm3::table_def_t &def, const sm3::index_def_t &x,
              const vals_t &before, const vals_t &after) {
  int rc = cols_same(def, x.key_attnums, before, after, true);
  if (rc == 1) {
    rc = cols_same(def, x.include_attnums, before, after, false);
  }
  if (rc == 1) {
    rc = cols_same(def, x.reads, before, after, false);
  }
  return rc;
}

/* Whether writes maintain index i of t: all but one whose build failed. */
bool maintained(const sm3::table_t *t, size_t i) {
  return t->def->indexes[i].state != sm3::index_state_t::failed;
}

/*
 * An index's entry for one row image: whether the row is in the
 * index, and its key. own holds the values the evaluator gave,
 * which the key points into.
 */
struct entry_t {
  bool belongs = true;
  std::vector<std::string> own;
  std::vector<sm2::datum_t> key;
  std::unique_ptr<bool[]> key_null;

  std::span<const sm2::datum_t> d() const { return key; }
  std::span<const bool> null() const { return {key_null.get(), key.size()}; }
};

/*
 * Index i's entry for a row of values r. A plain key column is
 * read from r; whether the row belongs, and each key column that
 * is an expression, are the evaluator's, which is shown r
 * (SM3_DESIGN.md §3).
 */
int make_entry(const sm3::table_t *t, size_t i, const vals_t &r, entry_t &e) {
  const sm3::index_def_t &x = t->def->indexes[i];
  size_t n = x.key_attnums.size(), nexpr = 0;
  for (int32_t att : x.key_attnums) {
    nexpr += att < 0;
  }
  e.key.assign(n, 0);
  e.key_null.reset(new bool[n]());
  std::unique_ptr<bool[]> own_null(new bool[nexpr]());
  if (!x.eval.empty()) {
    const sm3::index_eval_fn &eval = t->session->cat->eval;
    if (!eval) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "table: index " + x.name +
                             " is evaluated by the host, and the catalog "
                             "has no evaluator");
    }
    e.own.assign(nexpr, std::string());
    // The host's answer; its failure is this call's.
    if (eval(*t->def, x, r.d, r.null, e.belongs, e.own,
             {own_null.get(), nexpr}) != 0) {
      return -1;
    }
    if (!e.belongs) {
      return 0;
    }
  }
  size_t k = 0;
  for (size_t j = 0; j < n; j++) {
    int32_t att = x.key_attnums[j];
    if (att >= 0) {
      e.key[j] = r.d[att];
      e.key_null[j] = r.null[att];
      continue;
    }
    e.key_null[j] = own_null[k];
    if (!own_null[k]) {
      // The value's bytes as sm2::datum_bytes would give them: a
      // by-value type's inside the datum, any other's pointed at.
      const sm2::key_column_t &c = x.key[j];
      const std::string &b = e.own[k];
      int32_t width = sm2::datum_width(c.type, c.typmod);
      if (b.empty() || (width > 0 && b.size() != static_cast<size_t>(width))) {
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "table: the evaluator gave index " + x.name +
                               " a value of the wrong width");
      }
      if (c.type->byval) {
        memcpy(&e.key[j], b.data(), b.size());
      } else {
        e.key[j] = sm2::datum_from_ptr(b.data());
      }
    }
    k++;
  }
  return 0;
}

/* Whether two entries of index x have equal keys: 1, 0, or -1 on error. */
int keys_equal(const sm3::index_def_t &x, const entry_t &a, const entry_t &b) {
  for (size_t j = 0; j < x.key.size(); j++) {
    if (a.key_null[j] || b.key_null[j]) {
      if (a.key_null[j] && b.key_null[j]) {
        continue;
      }
      return 0;
    }
    const sm2::key_column_t &c = x.key[j];
    std::string_view p, q;
    if (native(c.type, c.typmod, a.key[j], p) != 0 ||
        native(c.type, c.typmod, b.key[j], q) != 0) {
      return -1;
    }
    if (c.type->cmp(p, q, c.typmod) != 0) {
      return 0;
    }
  }
  return 1;
}

/* Add index i's entry e for the row r at rowno. */
int add_entry(sm3::table_t *t, size_t i, const entry_t &e, const vals_t &r,
              uint64_t rowno) {
  picked_t inc(r, t->def->indexes[i].include_attnums);
  return sm2::idx_insert(t->indexes[i], e.d(), e.null(), rowno, inc.d(),
                         inc.null());
}

/*
 * Remove index i's entry e for the row at rowno. While the index
 * is building, an entry that is not there is no error: the build
 * has not reached the row (SM3_DESIGN.md §3).
 */
int remove_entry(sm3::table_t *t, size_t i, const entry_t &e, uint64_t rowno) {
  if (sm2::idx_delete(t->indexes[i], e.d(), e.null(), rowno) == 0) {
    return 0;
  }
  return t->def->indexes[i].state == sm3::index_state_t::building &&
                 sm1::errnum() == sm1::SM_ERR_NOTFOUND
             ? 0
             : -1;
}

} // namespace

namespace sm3 {

/* Lock the table, then open its heap and every index; see sm3.h. */
int tbl_open(session_t *session, uint64_t oid, sm1::lockmode_t mode,
             table_t *&out) {
  return catch_nomem([&] {
    std::unique_ptr<table_t, void (*)(table_t *)> t(new table_t, tbl_close);
    // Take the schema lock, so the definition holds to commit.
    if (cat_lock_table(session, oid, mode, t->def) != 0) {
      return -1;
    }
    t->session = session;
    t->writable = covers_write(mode);
    // No committed definition: this transaction made the table, and
    // no other can see it or its rows.
    t->fresh = cat_table(session->cat, oid) == nullptr;
    const table_def_t &def = *t->def;
    if (sm2::rel_open(session->sm1, def.storeid, 0, def.schema, def.rowno_seq,
                      t->rel) != 0) {
      return -1;
    }
    for (const index_def_t &x : def.indexes) {
      sm2::index_t *ix = nullptr;
      if (sm2::idx_open(session->sm1, def.storeid, x.prefix, x.key, x.unique,
                        x.include, ix) != 0) {
        return -1;
      }
      t->indexes.push_back(ix);
    }
    out = t.release();
    return 0;
  });
}

/* Close the heap and the indexes; the schema lock stays to commit. */
void tbl_close(table_t *table) {
  if (!table) {
    return;
  }
  for (sm2::index_t *ix : table->indexes) {
    sm2::idx_close(ix);
  }
  sm2::rel_close(table->rel);
  delete table;
}

/* The definition the table was opened with. */
const table_def_t &tbl_def(const table_t *table) { return *table->def; }

/* The session the table was opened in. */
session_t *tbl_session(const table_t *table) { return table->session; }

/* Whether the table's session may scan index now; see sm3.h. */
bool tbl_index_usable(const table_t *table, uint64_t index) {
  return index_usable(table->session, *table->def, index);
}

/* Insert the row, then its entry in every index; see sm3.h. */
int tbl_insert(table_t *table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t &rowno) {
  return catch_nomem([&] {
    if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
      return -1;
    }
    uint64_t r = 0;
    if (sm2::rel_insert(table->rel, values, isnull, r) != 0) {
      return -1;
    }
    vals_t row{values, isnull};
    for (size_t i = 0; i < table->indexes.size(); i++) {
      if (!maintained(table, i)) {
        continue;
      }
      entry_t e;
      if (make_entry(table, i, row, e) != 0 ||
          (e.belongs && add_entry(table, i, e, row, r) != 0)) {
        return -1;
      }
    }
    rowno = r;
    return 0;
  });
}

/* Replace the row, then fix each index it changed; see sm3.h. */
int tbl_update(table_t *table, uint64_t rowno,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, std::string *old) {
  return catch_nomem([&] {
    if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
      return -1;
    }
    std::string_view image;
    if (sm2::rel_update(table->rel, rowno, values, isnull, &image) != 0) {
      return -1;
    }
    // Copy the old image: it views session memory that the index
    // calls below reuse.
    std::string copy(image);
    const table_def_t &def = *table->def;
    deformed_t before(def.schema.size());
    if (!table->indexes.empty() &&
        sm2::tup_deform(def.desc, copy, before.d,
                        {before.null.get(), before.n}) != 0) {
      return -1;
    }
    vals_t after{values, isnull};
    for (size_t i = 0; i < table->indexes.size(); i++) {
      if (!maintained(table, i)) {
        continue;
      }
      const index_def_t &x = def.indexes[i];
      // No column the index depends on changed: leave it alone,
      // and the evaluator unasked.
      int same = unchanged(def, x, before.row(), after);
      if (same < 0) {
        return -1;
      }
      if (same) {
        continue;
      }
      entry_t was, is;
      if (make_entry(table, i, before.row(), was) != 0 ||
          make_entry(table, i, after, is) != 0) {
        return -1;
      }
      // A column it reads changed, yet the entry may not have: the
      // row is in the index both times, under an equal key, with
      // the same INCLUDE columns.
      if (!x.eval.empty() && was.belongs && is.belongs) {
        int kept = keys_equal(x, was, is);
        if (kept == 1) {
          kept = cols_same(def, x.include_attnums, before.row(), after, false);
        }
        if (kept < 0) {
          return -1;
        }
        if (kept) {
          continue;
        }
      }
      if ((was.belongs && remove_entry(table, i, was, rowno) != 0) ||
          (is.belongs && add_entry(table, i, is, after, rowno) != 0)) {
        return -1;
      }
    }
    if (old) {
      *old = std::move(copy);
    }
    return 0;
  });
}

/* Delete the row, then its entry in every index; see sm3.h. */
int tbl_delete(table_t *table, uint64_t rowno, std::string *old) {
  return catch_nomem([&] {
    if (need_writable(table) != 0) {
      return -1;
    }
    std::string_view image;
    if (sm2::rel_delete(table->rel, rowno, &image) != 0) {
      return -1;
    }
    // Copy the old image: it views session memory that the index
    // calls below reuse.
    std::string copy(image);
    const table_def_t &def = *table->def;
    deformed_t before(def.schema.size());
    if (!table->indexes.empty() &&
        sm2::tup_deform(def.desc, copy, before.d,
                        {before.null.get(), before.n}) != 0) {
      return -1;
    }
    for (size_t i = 0; i < table->indexes.size(); i++) {
      if (!maintained(table, i)) {
        continue;
      }
      entry_t was;
      if (make_entry(table, i, before.row(), was) != 0 ||
          (was.belongs && remove_entry(table, i, was, rowno) != 0)) {
        return -1;
      }
    }
    if (old) {
      *old = std::move(copy);
    }
    return 0;
  });
}

/* Read the row at rowno. */
int tbl_read(table_t *table, uint64_t rowno, std::string_view &tuple) {
  return sm2::rel_read(table->rel, rowno, tuple);
}

/* Read the newest committed version of the row at rowno. */
int tbl_read_latest(table_t *table, uint64_t rowno, std::string_view &tuple) {
  return sm2::rel_read_latest(table->rel, rowno, tuple);
}

/* Lock the row at rowno without writing it; see sm3.h. */
int tbl_lock(table_t *table, uint64_t rowno, sm1::lockmode_t mode) {
  // Only a shared lock sits under a table opened to read.
  if (mode != sm1::lockmode_t::shared && need_writable(table) != 0) {
    return -1;
  }
  // Held to the end of the transaction, as a write's lock is.
  return sm2::rel_lock(table->rel, rowno, mode);
}

/* Open a cursor over the rows first..last. */
int tbl_scan(table_t *table, uint64_t first, uint64_t last,
             sm2::rel_scan_t *&out) {
  return sm2::rel_scan(table->rel, first, last, out);
}

/* Open a cursor over one of the table's indexes; see sm3.h. */
int tbl_idx_scan(table_t *table, uint64_t index, const sm2::bound_t *lo,
                 const sm2::bound_t *hi, sm2::index_scan_t *&out,
                 bool *widened) {
  const table_def_t &def = *table->def;
  for (size_t i = 0; i < def.indexes.size(); i++) {
    if (def.indexes[i].oid == index) {
      if (!index_usable(table->session, def, index)) {
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "table: index " + def.indexes[i].name +
                               " is not usable by this snapshot");
      }
      return sm2::idx_scan(table->indexes[i], lo, hi, out, widened);
    }
  }
  return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                     "table: no index " + std::to_string(index));
}

/* One batch of an index's build; see sm3.h. */
int tbl_index_backfill(table_t *table, uint64_t index, uint64_t from,
                       size_t max_rows, uint64_t &next, bool &done) {
  return catch_nomem([&] {
    if (need_writable(table) != 0) {
      return -1;
    }
    const table_def_t &def = *table->def;
    size_t i = 0;
    while (i < def.indexes.size() && def.indexes[i].oid != index) {
      i++;
    }
    if (i == def.indexes.size()) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "table: no index " + std::to_string(index));
    }
    const index_def_t &x = def.indexes[i];
    if (x.state != index_state_t::building) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index build: index " + x.name + " is not building");
    }
    // Rows another transaction may write are read at their newest
    // version, which only READ COMMITTED can do.
    bool rc = table->session->level == sm1::xctlevel_t::read_committed;
    if (!table->fresh && !rc) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index build: not at READ COMMITTED");
    }
    // The batch's rows, by rowno; their tuples are read again below.
    std::vector<uint64_t> rownos;
    sm2::rel_scan_t *scan = nullptr;
    if (sm2::rel_scan(table->rel, from, UINT64_MAX, scan) != 0) {
      return -1;
    }
    uint64_t rowno;
    std::string_view tuple;
    int more = 0;
    while (rownos.size() < max_rows &&
           (more = sm2::rel_scannxt(scan, rowno, tuple)) == 0) {
      rownos.push_back(rowno);
    }
    sm2::rel_scanend(scan);
    if (more < 0) {
      return -1;
    }
    deformed_t row(def.schema.size());
    for (uint64_t r : rownos) {
      int found;
      if (table->fresh) {
        found = sm2::rel_read(table->rel, r, tuple);
      } else {
        // Hold the row shared to the end of the batch: a writer
        // changes the heap row first, so it and the build never
        // work on one row at once (SM3_DESIGN.md §3).
        if (sm2::rel_lock(table->rel, r, sm1::lockmode_t::shared) != 0) {
          return -1;
        }
        found = sm2::rel_read_latest(table->rel, r, tuple);
      }
      if (found < 0) {
        return -1;
      }
      // Deleted since the scan: nothing to cover.
      if (found == 1) {
        continue;
      }
      // Copy the tuple: the index calls below reuse session memory.
      std::string copy(tuple);
      if (sm2::tup_deform(def.desc, copy, row.d, {row.null.get(), row.n}) !=
          0) {
        return -1;
      }
      // A row a partial index does not take needs no entry.
      entry_t key;
      if (make_entry(table, i, row.row(), key) != 0) {
        return -1;
      }
      if (!key.belongs || add_entry(table, i, key, row.row(), r) == 0) {
        continue;
      }
      if (sm1::errnum() != sm1::SM_ERR_DUPKEY) {
        return -1;
      }
      // An entry is there already. With the rowno in its key it is
      // this row's, made by a writer; without, it may be another's.
      bool has_null = false;
      for (bool n : key.null()) {
        has_null |= n;
      }
      if (!x.unique || has_null) {
        continue;
      }
      sm2::bound_t eq{key.d(), key.null(), true};
      sm2::index_scan_t *is = nullptr;
      if (sm2::idx_scan(table->indexes[i], &eq, &eq, is) != 0) {
        return -1;
      }
      uint64_t owner;
      bool cut, mine = false;
      std::string_view value;
      while ((more = sm2::idx_scannxt(is, owner, cut, value)) == 0) {
        mine |= owner == r;
      }
      sm2::idx_scanend(is);
      if (more < 0) {
        return -1;
      }
      if (!mine) {
        return sm1::seterr(sm1::SM_ERR_DUPKEY,
                           "index build: two rows of " + def.name +
                               " with one key in " + x.name);
      }
    }
    done = rownos.size() < max_rows;
    next = rownos.empty() ? from : rownos.back() + 1;
    return 0;
  });
}

/* The value schema of one of the table's indexes; see sm3.h. */
std::span<const sm2::attribute_t> tbl_idx_value_schema(const table_t *table,
                                                       uint64_t index) {
  const table_def_t &def = *table->def;
  for (size_t i = 0; i < def.indexes.size(); i++) {
    if (def.indexes[i].oid == index) {
      return sm2::idx_value_schema(table->indexes[i]);
    }
  }
  return {};
}

} // namespace sm3
