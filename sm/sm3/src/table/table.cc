/*
 * table.cc -- an open table and its writes (SM3_DESIGN.md §3): the
 * heap write, then each index the write changes, with the old
 * entries computed from the image the heap write hands back. A
 * datum the heap holds out of line (sm2's TOAST) is fetched where
 * an index needs its value, and an old row's are erased only once
 * the indexes are fixed.
 */

#include "../catalog/internal.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

/*
 * A bulk load into a table (BULKLOAD.md §5, §13). load is sm1's
 * loader on the table's store while the rows go to it, and nullptr
 * once they go by tbl_insert: from the start, into a table that
 * had rows, or from the first row a load does not take.
 */
struct sm3::loader_t {
  table_t *table = nullptr;
  sm1::loader_t *load = nullptr;
  size_t heap = 0;             /* the stream of the table's rows */
  std::vector<size_t> streams; /* the stream of each index, in order */
};

struct sm3::table_t {
  session_t *session;
  std::shared_ptr<const table_def_t> def; /* held under the table-oid lock */
  bool writable;                          /* the lock covers intent_exclusive */
  bool exclusive;                         /* the lock is exclusive */
  bool fresh;                             /* made by this transaction */
  sm2::rel_t *rel = nullptr;
  std::vector<sm2::index_t *> indexes; /* one per def->indexes, in order */
  /*
   * Each index's state as the session saw it when the table was
   * opened. One that has changed since is seen late, as pending,
   * which is always safe: the index is maintained, and not scanned.
   */
  std::vector<sm3::index_state_t> states;
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

/*
 * A row's values, copied so that those out of line can be fetched
 * in place: own holds the fetched ones, which d then points at.
 */
struct image_t {
  /* Copy r's datums; its null flags are borrowed. */
  explicit image_t(const vals_t &r) : d(r.d.begin(), r.d.end()), null(r.null) {
    // A fetched value must not move once a datum points at it.
    own.reserve(d.size());
  }

  vals_t row() const { return {d, null}; }

  std::vector<sm2::datum_t> d;
  std::span<const bool> null;
  std::vector<std::string> own;
};

/* Whether column att of the row r is a TOAST pointer, not a value. */
bool is_pointer(const sm3::table_def_t &def, const vals_t &r, int32_t att) {
  return !r.null[att] && def.schema[att].type->len < 0 &&
         *static_cast<const char *>(sm2::datum_to_ptr(r.d[att])) ==
             sm2::varlena_toast;
}

/* The TOAST pointer column att of r is, as its varlena. */
std::string_view pointer_of(const vals_t &r, int32_t att) {
  return {static_cast<const char *>(sm2::datum_to_ptr(r.d[att])),
          sm2::toast_ptr_len};
}

/* Whether mode lets its holder write the table's rows. */
bool covers_write(sm1::lockmode_t mode) {
  return mode == sm1::SM_LOCKMODE_IX || mode == sm1::SM_LOCKMODE_SIX ||
         mode == sm1::SM_LOCKMODE_EX;
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
 * holds the same bytes, 0 if not, -1 on error. A column out of
 * line in either is the same only if both hold one pointer.
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
    // Out of line: one pointer both times is one datum, unchanged.
    // Anything else is taken as changed, with no fetch; the caller
    // fetches before it compares what the index holds.
    bool pb = is_pointer(def, before, att), pa = is_pointer(def, after, att);
    if (pb || pa) {
      if (pb && pa && pointer_of(before, att) == pointer_of(after, att)) {
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

/* Note the state of each index of t, as its session sees it now. */
void load_states(sm3::table_t *t) {
  t->states.clear();
  for (const sm3::index_def_t &x : t->def->indexes) {
    t->states.push_back(sm3::cat_get_index_state(t->session, x.oid).state);
  }
}

/* Whether writes maintain index i of t: all but one whose build failed. */
bool maintained(const sm3::table_t *t, size_t i) {
  return t->states[i] != sm3::index_state_t::failed;
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
 * Ask the host's evaluator about a row of values r. The evaluator
 * may not throw, and its type, a std::function, cannot say so:
 * asked from here, one that throws ends the process, where it
 * would otherwise unwind through a write or a build.
 */
int ask_evaluator(const sm3::index_eval_fn &eval, const sm3::table_def_t &def,
                  const sm3::index_def_t &x, const vals_t &r, bool &belongs,
                  std::vector<std::string> &own,
                  std::span<bool> own_null) noexcept {
  return eval(def, x, r.d, r.null, belongs, own, own_null);
}

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
    if (ask_evaluator(eval, *t->def, x, r, e.belongs, e.own,
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
 * is pending, an entry that is not there is no error: the build
 * has not reached the row (SM3_DESIGN.md §3).
 */
int remove_entry(sm3::table_t *t, size_t i, const entry_t &e, uint64_t rowno) {
  if (sm2::idx_delete(t->indexes[i], e.d(), e.null(), rowno) == 0) {
    return 0;
  }
  return t->states[i] == sm3::index_state_t::pending &&
                 sm1::errnum() == sm1::SM_ERR_NOTFOUND
             ? 0
             : -1;
}

/*
 * Fetch the columns atts of row that are out of line, in place: a
 * TOAST pointer among them becomes its value. An expression's
 * place, below 0, is no column.
 */
int fetch_cols(sm3::table_t *t, image_t &row,
               const std::vector<int32_t> &atts) {
  for (int32_t att : atts) {
    if (att < 0 || !is_pointer(*t->def, row.row(), att)) {
      continue;
    }
    std::string value;
    if (sm2::toast_fetch(t->rel, pointer_of(row.row(), att), value) != 0) {
      return -1;
    }
    row.own.push_back(sm2::make_varlena(value));
    row.d[att] = sm2::datum_from_ptr(row.own.back().data());
  }
  return 0;
}

/*
 * Fetch what index x needs of row: its key columns, whose values
 * make the key; its INCLUDE columns, so that an entry never holds
 * a pointer; and the columns its evaluator reads, which is shown
 * values (SM3_DESIGN.md §3).
 */
int fetch_for(sm3::table_t *t, const sm3::index_def_t &x, image_t &row) {
  return fetch_cols(t, row, x.key_attnums) != 0 ||
                 fetch_cols(t, row, x.include_attnums) != 0 ||
                 fetch_cols(t, row, x.reads) != 0
             ? -1
             : 0;
}

} // namespace

namespace sm3 {

namespace {

/*
 * The sequence table's heap draws its TOAST ids from, in out: ""
 * unless the host reads TOAST pointers and the table has one. A
 * table made before TOAST has none, and stores every datum in its
 * tuple as it always did.
 */
int toast_seq_of(const table_t *table, std::string &out) {
  out.clear();
  if (!table->session->cat->toast) {
    return 0;
  }
  std::string name = toast_seq_name(table->def->oid);
  uint64_t last;
  if (sm1::lastseq(table->session->sm1, name, last) < 0) {
    return sm1::errnum() == sm1::SM_ERR_NOTFOUND ? 0 : -1;
  }
  out = name;
  return 0;
}

/* Open table's heap and indexes on the store its definition names. */
int open_rels(table_t *table) {
  const table_def_t &def = *table->def;
  sm1::session_t *s = table->session->sm1;
  std::string toast_seq;
  if (toast_seq_of(table, toast_seq) != 0 ||
      sm2::rel_open(s, def.storeid, heap_relid, def.schema, def.rowno_seq,
                    toast_seq, table->rel) != 0) {
    return -1;
  }
  for (const index_def_t &x : def.indexes) {
    sm2::index_t *ix = nullptr;
    if (sm2::idx_open(s, def.storeid, x.relid, x.key, x.unique, x.include,
                      ix) != 0) {
      return -1;
    }
    table->indexes.push_back(ix);
  }
  return 0;
}

/* Close table's heap and indexes, leaving it with neither. */
void close_rels(table_t *table) {
  for (sm2::index_t *ix : table->indexes) {
    sm2::idx_close(ix);
  }
  table->indexes.clear();
  sm2::rel_close(table->rel);
  table->rel = nullptr;
}

} // namespace

/* Lock the table, then open its heap and every index; see sm3.h. */
int tbl_open(session_t *session, uint64_t oid, sm1::lockmode_t mode,
             table_t *&out) {
  std::unique_ptr<table_t, void (*)(table_t *)> t(new table_t, tbl_close);
  // Take the table-oid lock, so the definition holds to commit.
  if (cat_lock_table(session, oid, mode, t->def) != 0) {
    return -1;
  }
  t->session = session;
  t->writable = covers_write(mode);
  t->exclusive = mode == sm1::SM_LOCKMODE_EX;
  // No committed definition: this transaction made the table, and
  // no other can see it or its rows.
  t->fresh = cat_get_table(session->cat, oid) == nullptr;
  load_states(t.get());
  if (open_rels(t.get()) != 0) {
    return -1;
  }
  out = t.release();
  return 0;
}

/* Close the heap and the indexes; the table-oid lock stays to commit. */
void tbl_close(table_t *table) {
  if (!table) {
    return;
  }
  close_rels(table);
  delete table;
}

/* The definition the table was opened with. */
const table_def_t &tbl_def(const table_t *table) { return *table->def; }

/* The session the table was opened in. */
session_t *tbl_session(const table_t *table) { return table->session; }

/* Whether the table's session may scan index now; see sm3.h. */
/* An index's state as the handle has it; see sm3.h. */
index_state_t tbl_index_state(const table_t *table, uint64_t index) {
  for (size_t i = 0; i < table->def->indexes.size(); i++) {
    if (table->def->indexes[i].oid == index) {
      return table->states[i];
    }
  }
  return index_state_t::failed;
}

bool tbl_index_usable(const table_t *table, uint64_t index) {
  return index_usable(table->session, *table->def, index);
}

/* Insert the row, then its entry in every index; see sm3.h. */
int tbl_insert(table_t *table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t &rowno) {
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
    return -1;
  }
  uint64_t r = 0;
  if (sm2::rel_insert(table->rel, values, isnull, r) != 0) {
    return -1;
  }
  // A pointer the caller gave is fetched for the indexes that need
  // its value; the heap has stored a copy of its own.
  image_t row(vals_t{values, isnull});
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i)) {
      continue;
    }
    entry_t e;
    if (fetch_for(table, table->def->indexes[i], row) != 0 ||
        make_entry(table, i, row.row(), e) != 0 ||
        (e.belongs && add_entry(table, i, e, row.row(), r) != 0)) {
      return -1;
    }
  }
  rowno = r;
  return 0;
}

/* Replace the row, then fix each index it changed; see sm3.h. */
int tbl_update(table_t *table, uint64_t rowno,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, std::string *old) {
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
    return -1;
  }
  // The old row's datums out of line are held until its index
  // entries are found and removed, below.
  std::string_view image;
  if (sm2::rel_update(table->rel, rowno, values, isnull, &image, true) != 0) {
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
  image_t was_row(table->indexes.empty() ? vals_t{} : before.row());
  image_t is_row(after);
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
    if (fetch_for(table, x, was_row) != 0 || fetch_for(table, x, is_row) != 0 ||
        make_entry(table, i, was_row.row(), was) != 0 ||
        make_entry(table, i, is_row.row(), is) != 0) {
      return -1;
    }
    // A column it reads changed, yet the entry may not have: the
    // row is in the index both times, under an equal key, with
    // the same INCLUDE columns.
    if (!x.eval.empty() && was.belongs && is.belongs) {
      int kept = keys_equal(x, was, is);
      if (kept == 1) {
        kept = cols_same(def, x.include_attnums, was_row.row(), is_row.row(),
                         false);
      }
      if (kept < 0) {
        return -1;
      }
      if (kept) {
        continue;
      }
    }
    if ((was.belongs && remove_entry(table, i, was, rowno) != 0) ||
        (is.belongs && add_entry(table, i, is, is_row.row(), rowno) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the old row held out of line goes.
  if (sm2::rel_release(table->rel) != 0) {
    return -1;
  }
  if (old) {
    *old = std::move(copy);
  }
  return 0;
}

/* Delete the row, then its entry in every index; see sm3.h. */
int tbl_delete(table_t *table, uint64_t rowno, std::string *old) {
  if (need_writable(table) != 0) {
    return -1;
  }
  // The row's datums out of line are held until its index entries
  // are found and removed, below.
  std::string_view image;
  if (sm2::rel_delete(table->rel, rowno, &image, true) != 0) {
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
  image_t was_row(table->indexes.empty() ? vals_t{} : before.row());
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i)) {
      continue;
    }
    entry_t was;
    if (fetch_for(table, def.indexes[i], was_row) != 0 ||
        make_entry(table, i, was_row.row(), was) != 0 ||
        (was.belongs && remove_entry(table, i, was, rowno) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the row held out of line goes.
  if (sm2::rel_release(table->rel) != 0) {
    return -1;
  }
  if (old) {
    *old = std::move(copy);
  }
  return 0;
}

/* Begin a bulk load into the table; see sm3.h. */
int tbl_load_open(table_t *table, loader_t *&out) {
  if (need_writable(table) != 0) {
    return -1;
  }
  sm1::session_t *s = table->session->sm1;
  uint64_t storeid = table->def->storeid;
  std::unique_ptr<loader_t> l(new loader_t);
  l->table = table;
  // Only an empty table is loaded from files. Look before
  // locking, so that a load into a table with rows keeps no one
  // out.
  bool empty;
  if (sm1::store_empty(s, storeid, empty) != 0) {
    return -1;
  }
  // A temporary table is its session's alone: no one to keep out.
  if (empty && !table->exclusive && table->def->nspoid != nsp_temp) {
    // Keep every other transaction out, readers too, to the end of
    // this one: sm1's loader checks nothing against the store, and
    // waits for no one (BULKLOAD.md §3.1). The lock is taken only
    // if it is free now. A load does not wait for it: the rows can
    // be inserted instead, and waiting here, with the table's lock
    // already held to write, is how two loads would deadlock.
    int held = lock_table_now(s, table->def->oid, sm1::SM_LOCKMODE_EX);
    if (held < 0) {
      return -1;
    }
    if (held == 1) {
      empty = false;
    } else {
      table->exclusive = true;
      // A writer may have put a row in before the lock was taken.
      if (sm1::store_empty(s, storeid, empty) != 0) {
        return -1;
      }
    }
  }
  if (empty) {
    if (sm1::load_open(s, storeid, l->load) != 0 ||
        sm1::load_stream(l->load, true, l->heap) != 0) {
      return -1;
    }
    // An index's entries arrive in the heap's order, not its own.
    l->streams.resize(table->indexes.size());
    for (size_t &stream : l->streams) {
      if (sm1::load_stream(l->load, false, stream) != 0) {
        return -1;
      }
    }
  }
  out = l.release();
  return 0;
}

/* Hand a row to a bulk load; see sm3.h. */
int tbl_load_add(loader_t *l, std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull, uint64_t &rowno) {
  table_t *table = l->table;
  if (l->load == nullptr) {
    return tbl_insert(table, values, isnull, rowno);
  }
  if (need_not_null(table, isnull) != 0) {
    return -1;
  }
  uint64_t r = 0;
  int rc = sm2::rel_load(table->rel, l->load, l->heap, values, isnull, r);
  if (rc < 0) {
    return -1;
  }
  if (rc == 1) {
    // A row a load does not take, too long or to be stored out of
    // line: what is loaded so far goes into the table, and this
    // row and those after it are inserted.
    sm1::loader_t *load = l->load;
    l->load = nullptr;
    return sm1::load_close(load) != 0
               ? -1
               : tbl_insert(table, values, isnull, rowno);
  }
  // Its entry in every index, as tbl_insert makes it. No value is
  // out of line: rel_load took the row.
  vals_t row{values, isnull};
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i)) {
      continue;
    }
    entry_t e;
    if (make_entry(table, i, row, e) != 0) {
      return -1;
    }
    if (!e.belongs) {
      continue;
    }
    picked_t inc(row, table->def->indexes[i].include_attnums);
    if (sm2::idx_load(table->indexes[i], l->load, l->streams[i], e.d(),
                      e.null(), r, inc.d(), inc.null()) != 0) {
      return -1;
    }
  }
  rowno = r;
  return 0;
}

/* Finish a bulk load; see sm3.h. */
int tbl_load_close(loader_t *l) {
  std::unique_ptr<loader_t> own(l);
  return l->load == nullptr ? 0 : sm1::load_close(l->load);
}

/* Free a bulk load's handle, and no more; see sm3.h. */
void tbl_load_free(loader_t *l) { delete l; }

/* Read a datum out of line; see sm3.h. */
int tbl_fetch(table_t *table, std::string_view ptr, std::string &out) {
  return sm2::toast_fetch(table->rel, ptr, out);
}

/* Fetch a row's columns that are out of line; see internal.h. */
int tbl_fetch_cols(table_t *table, const std::vector<int32_t> &atts,
                   std::span<sm2::datum_t> row, std::span<const bool> isnull,
                   std::vector<std::string> &own) {
  image_t image(vals_t{row, isnull});
  if (fetch_cols(table, image, atts) != 0) {
    return -1;
  }
  // The values go to the caller, and the datums with them: a
  // string's bytes stay where they are when it is moved.
  std::copy(image.d.begin(), image.d.end(), row.begin());
  for (std::string &v : image.own) {
    own.push_back(std::move(v));
  }
  return 0;
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
  if (mode != sm1::SM_LOCKMODE_SH && need_writable(table) != 0) {
    return -1;
  }
  // Held to the end of the transaction, as a write's lock is.
  return sm2::rel_lock(table->rel, rowno, mode);
}

/* Empty the table, then reopen its heap and indexes; see sm3.h. */
int tbl_truncate(table_t *table) {
  if (!table->exclusive) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "truncate: the table is not opened exclusive");
  }
  const uint64_t oid = table->def->oid;
  std::shared_ptr<const table_def_t> def;
  if (truncate_table(table->session, oid) != 0 ||
      cat_lock_table(table->session, oid, sm1::SM_LOCKMODE_EX, def) != 0) {
    return -1;
  }
  // The handle's heap and indexes are on the old store.
  close_rels(table);
  table->def = std::move(def);
  load_states(table);
  return open_rels(table);
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

/* The heap's bytes, in files and in memtables. */
int tbl_size(table_t *table, uint64_t &disk, uint64_t &mem) {
  return sm2::relid_size(table->session->sm1, table->def->storeid, heap_relid,
                         disk, mem);
}

/* One index's bytes, in files and in memtables. */
int tbl_idx_size(table_t *table, uint64_t index, uint64_t &disk,
                 uint64_t &mem) {
  for (const index_def_t &x : table->def->indexes) {
    if (x.oid == index) {
      return sm2::relid_size(table->session->sm1, table->def->storeid, x.relid,
                             disk, mem);
    }
  }
  return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                     "table: no index " + std::to_string(index));
}

/* N, the last rowno the table's sequence handed out, 0 if none. */
int tbl_rowno_limit(table_t *table, uint64_t &n) {
  int rc = sm1::lastseq(table->session->sm1, table->def->rowno_seq, n);
  // A sequence that never handed out a rowno: no row ever was.
  if (rc == 1) {
    n = 0;
    return 0;
  }
  return rc;
}

/* The sorted runs a lookup in the table's store may visit. */
int tbl_probes(table_t *table, uint64_t &n) {
  return sm1::probes(table->session->sm1, table->def->storeid, n);
}

/* One batch of an index's build; see sm3.h. */
int tbl_index_backfill(table_t *table, uint64_t index, uint64_t from,
                       size_t max_rows, uint64_t &next, bool &done) {
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
  if (table->states[i] != index_state_t::pending) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index build: index " + x.name + " is not pending");
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
      if (sm2::rel_lock(table->rel, r, sm1::SM_LOCKMODE_SH) != 0) {
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
    if (sm2::tup_deform(def.desc, copy, row.d, {row.null.get(), row.n}) != 0) {
      return -1;
    }
    // A row a partial index does not take needs no entry.
    image_t values(row.row());
    entry_t key;
    if (fetch_for(table, x, values) != 0 ||
        make_entry(table, i, values.row(), key) != 0) {
      return -1;
    }
    if (!key.belongs || add_entry(table, i, key, values.row(), r) == 0) {
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
      return sm1::seterr(sm1::SM_ERR_DUPKEY, "index build: two rows of " +
                                                 def.name +
                                                 " with one key in " + x.name);
    }
  }
  done = rownos.size() < max_rows;
  next = rownos.empty() ? from : rownos.back() + 1;
  return 0;
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
