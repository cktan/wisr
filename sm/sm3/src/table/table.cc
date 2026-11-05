/*
 * table.cc -- an open table and its writes (../INDEXBUILD.md §2): the
 * row's write, then each index the write changes, with the old
 * entries computed from the image the row's write hands back. A
 * row is named by its _id, and found at its key, its primary key
 * in a table that has one, through sm2's _id index (../CLUSTER.md
 * §2). A datum the row holds out of line (sm2's TOAST) is fetched
 * where an index needs its value, and an old row's are erased only
 * once the indexes are fixed.
 */

#include "../catalog/internal.h"
#include "pin.h"
#include "sm0_defer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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
  bool settled = false; /* the cleaner's: no transaction, no lock */
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
 * row is written, so nothing is half done (../INDEXBUILD.md §2).
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
    t->states.push_back(sm3::cat_index_state(t->session, x.oid));
  }
}

/*
 * Whether writes maintain index i of t: all but one whose build
 * failed, and the primary key, whose entries are the rows.
 */
bool maintained(const sm3::table_t *t, size_t i) {
  return t->states[i] != sm3::index_state_t::failed &&
         !t->def->indexes[i].primary;
}

/*
 * A write of a row that failed: -1, and for a key the primary key
 * holds already, the index in sm1::errdup(), as add_entry says it.
 */
int row_failed(const sm3::table_t *t) {
  const sm3::index_def_t *pk = sm3::primary_of(*t->def);
  if (pk && sm1::errnum() == sm1::SM_ERR_DUPKEY) {
    sm1::setdup({pk->oid, {}});
  }
  return -1;
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
  size_t key_cap = 0;               /* key_null's room */
  std::unique_ptr<bool[]> own_null; /* the evaluator's null flags */
  size_t own_cap = 0;               /* own_null's room */

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
 * (../INDEXBUILD.md §2).
 */
int make_entry(const sm3::table_t *t, size_t i, const vals_t &r, entry_t &e) {
  const sm3::index_def_t &x = t->def->indexes[i];
  size_t n = x.key_attnums.size(), nexpr = 0;
  for (int32_t att : x.key_attnums) {
    nexpr += att < 0;
  }
  // An entry made again keeps its arrays: a tree's recheck makes one
  // a row.
  e.belongs = true;
  e.key.assign(n, 0);
  if (!e.key_null || e.key_cap < n) {
    e.key_null.reset(new bool[n]());
    e.key_cap = n;
  }
  std::fill(e.key_null.get(), e.key_null.get() + n, false);
  // The evaluator's flags, kept as the key's are; a plain key has
  // none to make.
  if (nexpr > 0 && (!e.own_null || e.own_cap < nexpr)) {
    e.own_null.reset(new bool[nexpr]);
    e.own_cap = nexpr;
  }
  bool *own_null = e.own_null.get();
  if (nexpr > 0) {
    std::fill(own_null, own_null + nexpr, false);
  }
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
                      {own_null, nexpr}) != 0) {
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

int unique_tree_insert(sm3::table_t *t, size_t i, const entry_t &e,
                       const picked_t &inc, std::string_view row);

int tree_find(sm3::table_t *t, size_t i, const entry_t &e, bool now,
              uint64_t &found, std::string &row);

/*
 * Add index i's entry e for the row r with key row. A
 * key the index holds already is SM_ERR_DUPKEY, with the index in
 * sm1::errdup().
 */
int add_entry(sm3::table_t *t, size_t i, const entry_t &e, const vals_t &r,
              std::string_view row) {
  picked_t inc(r, t->def->indexes[i].include_attnums);
  const sm3::index_def_t &x = t->def->indexes[i];
  if ((x.tree && x.unique ? unique_tree_insert(t, i, e, inc, row)
                          : sm2::idx_insert(t->indexes[i], e.d(), e.null(), row,
                                            inc.d(), inc.null())) == 0) {
    return 0;
  }
  // Which index has the key, for the host to say it.
  if (sm1::errnum() == sm1::SM_ERR_DUPKEY) {
    sm1::setdup({t->def->indexes[i].oid, {}});
  }
  return -1;
}

/*
 * Remove index i's entry e for the row with key row.
 * While the index is pending, an entry that is not there is no
 * error: the build has not reached the row (../INDEXBUILD.md §2).
 */
int remove_entry(sm3::table_t *t, size_t i, const entry_t &e,
                 std::string_view row) {
  if (sm2::idx_delete(t->indexes[i], e.d(), e.null(), row) == 0) {
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
    std::string_view ptr = pointer_of(row.row(), att);
    if ((t->settled ? sm2::toast_fetch_settled(t->rel, ptr, value)
                    : sm2::toast_fetch(t->rel, ptr, value)) != 0) {
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
 * values (../INDEXBUILD.md §2).
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
 * Open table's rel and indexes on the store its definition names:
 * the rows keyed by the primary key, if it has one, which is read
 * as an index of the rows.
 */
int open_rels(table_t *table) {
  const table_def_t &def = *table->def;
  sm1::session_t *s = table->session->sm1;
  // The sequence its TOAST ids come from, made with every table, or
  // none unless the host reads TOAST pointers; named, never read.
  std::string_view toast_seq =
      table->session->cat->toast ? std::string_view(def.toast_seq) : "";
  const index_def_t *pk = primary_of(def);
  sm2::rel_key_t key;
  if (pk) {
    key = {pk->key, pk->key_attnums};
  }
  // The definition's layouts, made once with it, which the table
  // keeps alive by holding the definition.
  if (sm2::rel_open_desc(s, def.storeid, heap_relid, def.desc, key, def.id_seq,
                         toast_seq, table->rel) != 0) {
    return -1;
  }
  for (const index_def_t &x : def.indexes) {
    sm2::index_t *ix = nullptr;
    if ((x.primary ? sm2::idx_open_primary_layout(table->rel, *x.layout,
                                                  x.include_attnums, ix)
                   : sm2::idx_open_layout(s, def.storeid, x.relid, x.key,
                                          x.unique, *x.layout, pk != nullptr,
                                          ix, x.tree)) != 0) {
      return -1;
    }
    // A leaf that needs cleaning is queued by whoever finds it. A
    // temporary table's cannot be: the cleaner cannot read its rows.
    cleaner_t *cleaner = table->session->cat->cleaner.get();
    if (nullptr != cleaner && x.tree && nsp_temp != def.nspoid) {
      sm2::idx_set_leaf_note(ix, cleaner_note, cleaner);
    }
    table->indexes.push_back(ix);
  }
  return 0;
}

/* Close table's rel and indexes, leaving it with neither. */
void close_rels(table_t *table) {
  for (sm2::index_t *ix : table->indexes) {
    sm2::idx_close(ix);
  }
  table->indexes.clear();
  sm2::rel_close(table->rel);
  table->rel = nullptr;
}

/*
 * Lock the table, waiting for it or not, then open its heap and every
 * index: 0, or 1 with nothing taken if the lock would have waited, or
 * -1.
 */
int open_table(session_t *session, uint64_t oid, sm1::lockmode_t mode,
               bool wait, table_t *&out) {
  std::unique_ptr<table_t, void (*)(table_t *)> t(new table_t, tbl_close);
  // Take the table-oid lock, so the definition holds to commit.
  bool fresh = false;
  int rc = wait ? cat_lock_table(session, oid, mode, t->def, &fresh)
                : cat_trylock_table(session, oid, mode, t->def, &fresh);
  if (rc != 0) {
    return rc;
  }
  t->session = session;
  t->writable = covers_write(mode);
  t->exclusive = mode == sm1::SM_LOCKMODE_EX;
  // No committed definition: this transaction made the table, and
  // no other can see it or its rows. The lock's lookup said.
  t->fresh = fresh;
  load_states(t.get());
  if (open_rels(t.get()) != 0) {
    return -1;
  }
  out = t.release();
  return 0;
}

} // namespace

/* Lock the table, then open its heap and every index; see sm3.h. */
int tbl_open(session_t *session, uint64_t oid, sm1::lockmode_t mode,
             table_t *&out) {
  return open_table(session, oid, mode, true, out);
}

/* As tbl_open, if the table-oid lock is free now; see sm3.h. */
int tbl_tryopen(session_t *session, uint64_t oid, sm1::lockmode_t mode,
                table_t *&out) {
  return open_table(session, oid, mode, false, out);
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

namespace {

/*
 * Insert the row of values and isnull, then its entry in every
 * index: a keyless row under a new _id, or under reserved if it is
 * not 0, as a rewrite keeps a row's; a keyed row with none, 0. id
 * is set on success.
 */
int insert_row(table_t *table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t reserved, uint64_t &id) {
  uint64_t r = reserved;
  std::string key;
  if ((reserved == 0 ? sm2::rel_insert(table->rel, values, isnull, r, &key)
                     : sm2::rel_insert_at(table->rel, reserved, values, isnull,
                                          &key)) != 0) {
    return row_failed(table);
  }
  // A pointer the caller gave is fetched for the indexes that need
  // its value; the row has stored a copy of its own.
  image_t row(vals_t{values, isnull});
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i)) {
      continue;
    }
    entry_t e;
    if (fetch_for(table, table->def->indexes[i], row) != 0 ||
        make_entry(table, i, row.row(), e) != 0 ||
        (e.belongs && add_entry(table, i, e, row.row(), key) != 0)) {
      return -1;
    }
  }
  id = r;
  return 0;
}

/*
 * The key of a keyless table's row with _id id, its _id encoded
 * (../ROWSTORE.md §2.1), into key; a table with a primary key names its
 * row by the key's values, not by _id: SM_ERR_INVALID.
 */
int id_key(table_t *t, uint64_t id, std::string &key) {
  if (sm2::rel_keyed(t->rel)) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "table: " + t->def->name +
                           " has a primary key; name its row by its key");
  }
  key = sm2::id_key(id);
  return 0;
}

} // namespace

/* Insert the row, then its entry in every index; see sm3.h. */
int tbl_insert(table_t *table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t &id) {
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
    return -1;
  }
  return insert_row(table, values, isnull, 0, id);
}

/*
 * The row that has entry e's key in unique index i of t now, past
 * the snapshot, its _id in found, and with tuple its newest
 * version's tuple there: 0, 1 if none has, or -1. READ COMMITTED
 * alone has such a read.
 * 1. Set a savepoint.
 * 2. Find the row by the key's newest version. sm1 reads a newest
 *    version only under a lock on the key.
 * 3. With tuple, lock the row shared and read its newest version.
 * 4. Roll back to the savepoint, which gives the locks back at once.
 *    The caller is left holding nothing of the key, or it would
 *    stand between the row's owner and the row.
 */
int find_now(sm3::table_t *t, size_t i, const entry_t &e, uint64_t &found,
             std::string *tuple) {
  int mark = sm3::savepoint(t->session);
  if (mark < 0) {
    return -1;
  }
  std::string row;
  int rc =
      t->def->indexes[i].tree
          ? tree_find(t, i, e, true, found, row)
          : sm2::idx_find_latest(t->indexes[i], e.d(), e.null(), found, &row);
  // The row's newest version too, under a lock on it, which the
  // rollback gives back with the entry's; gone since, it is none.
  std::string_view v;
  if (0 == rc && nullptr != tuple &&
      (0 != sm2::rel_lock(t->rel, row, sm1::SM_LOCKMODE_SH) ||
       (rc = sm2::rel_read_latest(t->rel, row, v)) < 0)) {
    rc = -1;
  }
  if (0 == rc && nullptr != tuple) {
    tuple->assign(v);
  }
  // Releases the locks; nothing was written since the mark.
  if (rc < 0 || sm3::rollback(t->session, mark) != 0) {
    return -1;
  }
  return rc;
}

/* Insert the row unless an arbiter index has its key; see sm3.h. */
int tbl_insert_arbiter(table_t *table, std::span<const sm2::datum_t> values,
                       std::span<const bool> isnull,
                       std::span<const uint64_t> arbiters, uint64_t &id,
                       std::string *tuple) {
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
    return -1;
  }
  const table_def_t &def = *table->def;
  // Each arbiter must be a unique index of the table.
  for (uint64_t oid : arbiters) {
    bool found = false;
    for (const index_def_t &x : def.indexes) {
      found = found || (x.oid == oid && x.unique);
    }
    if (!found) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "insert: " + std::to_string(oid) +
                                                  " is not a unique index of " +
                                                  def.name);
    }
  }
  image_t row(vals_t{values, isnull});
  // The arbiters, each with the row's entry in it, if it has one;
  // the primary key among them is the row itself, written last.
  std::vector<size_t> arb;
  std::vector<entry_t> entry(table->indexes.size());
  size_t primary = SIZE_MAX;
  for (size_t i = 0; i < table->indexes.size(); i++) {
    const index_def_t &x = def.indexes[i];
    bool arbiter = x.unique && (arbiters.empty() ||
                                std::find(arbiters.begin(), arbiters.end(),
                                          x.oid) != arbiters.end());
    if (!arbiter || (!x.primary && table->states[i] == index_state_t::failed)) {
      continue;
    }
    if (fetch_for(table, x, row) != 0 ||
        make_entry(table, i, row.row(), entry[i]) != 0) {
      return -1;
    }
    if (x.primary) {
      primary = i;
    } else if (entry[i].belongs) {
      // A row a partial index does not take has no key in it.
      arb.push_back(i);
    }
  }
  std::vector<size_t> looked = arb;
  if (primary != SIZE_MAX) {
    looked.push_back(primary);
  }
  const bool rc_level =
      table->session->level == sm1::xctlevel_t::read_committed;
look:
  // Look first, locking nothing: a row with the key that the
  // snapshot shows is the conflict. At READ COMMITTED the snapshot
  // may be behind, and show a row deleted since, which a caller
  // that came back for it would be shown for ever: there the row
  // is confirmed against the newest version.
  for (size_t i : looked) {
    uint64_t found = 0;
    std::string row;
    int rc = def.indexes[i].tree
                 ? tree_find(table, i, entry[i], false, found, row)
                 : sm2::idx_find(table->indexes[i], entry[i].d(),
                                 entry[i].null(), found, &row);
    if (0 == rc && rc_level) {
      rc = find_now(table, i, entry[i], found, tuple);
    } else if (0 == rc && nullptr != tuple) {
      // The row as the snapshot that showed its entry shows it.
      std::string_view v;
      rc = sm2::rel_read(table->rel, row, v);
      tuple->assign(v);
    }
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      id = found;
      return 1;
    }
  }
  // None seen, but one may have been put there since the
  // snapshot, or be on its way. So the arbiters' entries go in
  // before the row: sm1's insert waits for a transaction that has
  // not ended, and says at once if a row is there, when nothing
  // but these entries has been written. The row, under its key,
  // is the primary key's entry, and comes last of them.
  // A keyless row's key is its _id, drawn now; a keyed row has none.
  uint64_t r = 0;
  std::string key;
  if ((!sm2::rel_keyed(table->rel) && sm2::rel_reserve(table->rel, r) != 0) ||
      sm2::rel_row_key(table->rel, r, values, isnull, key) != 0) {
    return -1;
  }
  // A tree's entry goes in after its row, never before:
  // another's check would find it
  // naming no row, and call it dead. So a tree arbiter's is added
  // last, under a savepoint the row's write is taken back to if the
  // key turns out another's.
  std::vector<size_t> trees;
  for (auto it = arb.begin(); it != arb.end();) {
    if (def.indexes[*it].tree) {
      trees.push_back(*it);
      it = arb.erase(it);
    } else {
      ++it;
    }
  }
  int mark = 0;
  if (!trees.empty() && (mark = sm3::savepoint(table->session)) < 0) {
    return -1;
  }
  for (size_t n = 0; n <= arb.size(); n++) {
    size_t i = n < arb.size() ? arb[n] : primary;
    int rc = n < arb.size() ? add_entry(table, i, entry[i], row.row(), key)
             : r != 0       ? sm2::rel_insert_at(table->rel, r, values, isnull)
                            : sm2::rel_insert(table->rel, values, isnull, r);
    if (rc == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY || i == SIZE_MAX) {
      return n < arb.size() ? -1 : row_failed(table);
    }
    // The key is a row's that the snapshot did not show: take
    // back the entries put in so far, and say which row has it.
    for (size_t m = 0; m < n; m++) {
      if (remove_entry(table, arb[m], entry[arb[m]], key) != 0) {
        return -1;
      }
    }
    uint64_t found = 0;
    if (0 != mark && 0 != sm3::forget(table->session, mark)) {
      return -1;
    }
    rc = find_now(table, i, entry[i], found, tuple);
    if (rc < 0) {
      return -1;
    }
    if (rc == 1) {
      // Gone again already, deleted and committed: look once more.
      arb.insert(arb.end(), trees.begin(), trees.end());
      goto look;
    }
    id = found;
    return 1;
  }
  // The tree arbiters' entries, the row written: a key another's
  // takes the row back, and says which row has it.
  for (size_t i : trees) {
    if (add_entry(table, i, entry[i], row.row(), key) == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY) {
      return -1;
    }
    if (0 != sm3::rollback(table->session, mark) ||
        0 != sm3::forget(table->session, mark)) {
      return -1;
    }
    uint64_t found = 0;
    int rc = find_now(table, i, entry[i], found, tuple);
    if (rc < 0) {
      return -1;
    }
    if (rc == 1) {
      arb.insert(arb.end(), trees.begin(), trees.end());
      goto look;
    }
    id = found;
    return 1;
  }
  if (0 != mark && 0 != sm3::forget(table->session, mark)) {
    return -1;
  }
  // Its entries in the indexes that are no arbiter: a duplicate in
  // one of those is an error, as in tbl_insert. A tree arbiter's is in
  // already, above.
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i) ||
        std::find(arb.begin(), arb.end(), i) != arb.end() ||
        std::find(trees.begin(), trees.end(), i) != trees.end()) {
      continue;
    }
    entry_t e;
    if (fetch_for(table, def.indexes[i], row) != 0 ||
        make_entry(table, i, row.row(), e) != 0 ||
        (e.belongs && add_entry(table, i, e, row.row(), key) != 0)) {
      return -1;
    }
  }
  id = r;
  return 0;
}

/*
 * tbl_update, with ref and old both given or both null: with them,
 * the old row is pinned into ref, and old views it.
 */
static int update_row(table_t *table, uint64_t id, std::string_view key,
                      std::span<const sm2::datum_t> values,
                      std::span<const bool> isnull, pin_ref_t *ref,
                      std::string_view *old) {
  // No old row until the write succeeds.
  if (old) {
    *old = {};
  }
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0) {
    return -1;
  }
  // The old row is pinned, in a pin of this call's, so the index
  // calls below leave it where it is; and its datums out of line
  // are held until its index entries are found and removed. The
  // pin goes to ref only once all of it succeeds, as values may
  // point into the row ref holds.
  pin_t *p = pin_take(table->session);
  bool kept = false;
  defer {
    if (!kept) {
      pin_put(p);
    }
  };
  // A row whose primary key changes moves, and every entry of it,
  // naming it by its key, moves with it (../CLUSTER.md §4). A row
  // a concurrent transaction moved is SM_ERR_MOVED, not followed
  // (../ROWSTORE.md §2.5).
  std::string_view image;
  std::string at;
  if (0 != sm2::rel_update(table->rel, key, id, values, isnull, p->raw, image,
                           true, &at)) {
    return row_failed(table);
  }
  // What the old row held out of line is forgotten, not erased, if a
  // step below fails: the caller rolls back, and the old row is live.
  bool released = false;
  defer {
    if (!released) {
      sm2::rel_forget(table->rel);
    }
  };
  const bool moved = at != key;
  const table_def_t &def = *table->def;
  deformed_t before(def.schema.size());
  if (!table->indexes.empty() &&
      sm2::tup_deform(def.desc, image, before.d,
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
    int same = moved ? 0 : unchanged(def, x, before.row(), after);
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
    if (!moved && !x.eval.empty() && was.belongs && is.belongs) {
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
    if ((was.belongs && remove_entry(table, i, was, key) != 0) ||
        (is.belongs && add_entry(table, i, is, is_row.row(), at) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the old row held out of line goes.
  released = true;
  if (sm2::rel_release(table->rel) != 0) {
    return -1;
  }
  if (ref) {
    unpin(*ref);
    pin_hold(p, table->def, image, *ref);
    kept = true;
    *old = image;
  }
  return 0;
}

/* Replace the row, then fix each index it changed; see sm3.h. */
int tbl_update(table_t *table, uint64_t id,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull) {
  std::string key;
  return 0 != id_key(table, id, key)
             ? -1
             : update_row(table, id, key, values, isnull, nullptr, nullptr);
}

/* As above, pinning the row replaced into ref; see sm3.h. */
int tbl_update(table_t *table, uint64_t id,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, pin_ref_t &ref,
               std::string_view &old) {
  std::string key;
  return 0 != id_key(table, id, key)
             ? -1
             : update_row(table, id, key, values, isnull, &ref, &old);
}

/*
 * tbl_delete, with ref and old both given or both null: with them,
 * the row deleted is pinned into ref, and old views it.
 */
static int delete_row(table_t *table, std::string_view key, pin_ref_t *ref,
                      std::string_view *old) {
  // No old row until the write succeeds.
  if (old) {
    *old = {};
  }
  if (need_writable(table) != 0) {
    return -1;
  }
  // The row is pinned, in a pin of this call's, so the index calls
  // below leave it where it is; and its datums out of line are
  // held until its index entries are found and removed. The pin
  // goes to ref only once all of it succeeds.
  pin_t *p = pin_take(table->session);
  bool kept = false;
  defer {
    if (!kept) {
      pin_put(p);
    }
  };
  std::string_view image;
  if (0 != sm2::rel_delete(table->rel, key, p->raw, image, true)) {
    return -1;
  }
  // As an update's: forgotten, not erased, if a step below fails.
  bool released = false;
  defer {
    if (!released) {
      sm2::rel_forget(table->rel);
    }
  };
  const table_def_t &def = *table->def;
  deformed_t before(def.schema.size());
  if (!table->indexes.empty() &&
      sm2::tup_deform(def.desc, image, before.d,
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
        (was.belongs && remove_entry(table, i, was, key) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the row held out of line goes.
  released = true;
  if (sm2::rel_release(table->rel) != 0) {
    return -1;
  }
  if (ref) {
    unpin(*ref);
    pin_hold(p, table->def, image, *ref);
    kept = true;
    *old = image;
  }
  return 0;
}

/* Delete the row, then its entry in every index; see sm3.h. */
int tbl_delete(table_t *table, uint64_t id) {
  std::string key;
  return 0 != id_key(table, id, key) ? -1
                                     : delete_row(table, key, nullptr, nullptr);
}

/* As above, pinning the row deleted into ref; see sm3.h. */
int tbl_delete(table_t *table, uint64_t id, pin_ref_t &ref,
               std::string_view &old) {
  std::string key;
  return 0 != id_key(table, id, key) ? -1 : delete_row(table, key, &ref, &old);
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
  // A unique tree checks a key against its rows, and a file's rows are
  // in no store yet: such a table's rows are inserted, one at a time.
  for (const index_def_t &x : table->def->indexes) {
    empty = empty && !(x.tree && x.unique);
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
    // A table keyed by a primary key gets its rows in no order; a
    // keyless one in its _ids' (../CLUSTER.md §5).
    bool keyed = sm2::rel_keyed(table->rel);
    // A chained table's rows are its store's chained keys.
    if (sm1::load_open(s, storeid, l->load) != 0 ||
        sm1::load_stream(l->load, !keyed, l->heap,
                         sm2::rel_chained(table->rel)) != 0) {
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
                 std::span<const bool> isnull, uint64_t &id) {
  table_t *table = l->table;
  if (l->load == nullptr) {
    return tbl_insert(table, values, isnull, id);
  }
  if (need_not_null(table, isnull) != 0) {
    return -1;
  }
  uint64_t r = 0;
  std::string key;
  int rc = sm2::rel_load(table->rel, l->load, l->heap, values, isnull, r, &key);
  if (rc < 0) {
    return -1;
  }
  if (rc == 1) {
    // A row a load does not take, too long or to be stored out of
    // line: what is loaded so far goes into the table, and this
    // row and those after it are inserted.
    sm1::loader_t *load = l->load;
    l->load = nullptr;
    return sm1::load_close(load) != 0 ? -1
                                      : tbl_insert(table, values, isnull, id);
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
                      e.null(), key, inc.d(), inc.null()) != 0) {
      // Which index has the key, when it is found now, not at the
      // close.
      if (sm1::errnum() == sm1::SM_ERR_DUPKEY) {
        sm1::setdup({table->def->indexes[i].oid, {}});
      }
      return -1;
    }
  }
  id = r;
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

/* Read the row with _id id; see sm3.h. */
int tbl_read(table_t *table, uint64_t id, std::string_view &tuple) {
  std::string key;
  tuple = {};
  return 0 != id_key(table, id, key) ? -1
                                     : sm2::rel_read(table->rel, key, tuple);
}

/* Read the newest committed version of the row with _id id. */
int tbl_read_latest(table_t *table, uint64_t id, std::string_view &tuple) {
  std::string key;
  tuple = {};
  return 0 != id_key(table, id, key)
             ? -1
             : sm2::rel_read_latest(table->rel, key, tuple);
}

/*
 * Read the row with _id id into a pin, by read, sm2::rel_read or
 * sm2::rel_read_latest at a key, pointing ref at it; ref's pin is
 * released first, and on 1 or -1 ref names none.
 */
template <typename F>
static int read_into(table_t *table, pin_ref_t &ref, std::string_view &tuple,
                     F read) {
  tuple = {};
  // A read into ref replaces the row it held, in the same pin when
  // there is one, whose iterator sm1 may seek again.
  pin_t *p = pin_reread(ref);
  if (!p) {
    p = pin_take(table->session);
  }
  int rc = read(p->raw);
  if (rc != 0) {
    pin_put(p);
    tuple = {};
    return rc;
  }
  pin_hold(p, table->def, tuple, ref);
  return 0;
}

/* Read the row with _id id into a pin; see sm3.h. */
int tbl_read(table_t *table, uint64_t id, pin_ref_t &ref,
             std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    std::string key;
    return 0 != id_key(table, id, key)
               ? -1
               : sm2::rel_read(table->rel, key, raw, tuple);
  });
}

/* Read the row at key into a pin; see sm3.h. */
int tbl_read_key(table_t *table, std::string_view key, pin_ref_t &ref,
                 std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    return sm2::rel_read(table->rel, key, raw, tuple);
  });
}

/* Read the rows at keys into pins in one call; see sm3.h. */
int tbl_read_keys(table_t *table, const std::string_view *keys, size_t n,
                  pin_ref_t *refs, std::string_view *tuples, int *rc) {
  std::vector<pin_t *> pins(n, nullptr);
  std::vector<sm1::rawpin_t *> raws(n, nullptr);
  // Each pin given back, and each ref zero, if the read fails.
  auto give_back = [&] {
    for (size_t i = 0; i < n; i++) {
      if (pins[i]) {
        pin_put(pins[i]);
      }
      refs[i] = pin_ref_t{};
      tuples[i] = {};
    }
  };
  // A pin each: the one its ref holds, as read_into() rereads it, or
  // one from the session's pool.
  for (size_t i = 0; i < n; i++) {
    tuples[i] = {};
    pins[i] = pin_reread(refs[i]);
    if (!pins[i]) {
      pins[i] = pin_take(table->session);
    }
    raws[i] = pins[i]->raw;
  }
  if (sm2::rel_read_batch(table->rel, keys, n, raws.data(), tuples, rc) != 0) {
    give_back();
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    if (rc[i] == 0) {
      pin_hold(pins[i], table->def, tuples[i], refs[i]);
    } else {
      pin_put(pins[i]);
      refs[i] = pin_ref_t{};
      tuples[i] = {};
    }
  }
  return 0;
}

/* Read the newest committed version of the row into a pin. */
int tbl_read_latest(table_t *table, uint64_t id, pin_ref_t &ref,
                    std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    std::string key;
    return 0 != id_key(table, id, key)
               ? -1
               : sm2::rel_read_latest(table->rel, key, raw, tuple);
  });
}

/*
 * Lock a keyless table's row with _id id, waiting or not; see sm3.h.
 * A row there is none of has nothing to lock, and a read of it then
 * finds it gone.
 */
static int lock_row(table_t *table, uint64_t id, sm1::lockmode_t mode,
                    bool nowait) {
  // Only a shared lock sits under a table opened to read.
  if (mode != sm1::SM_LOCKMODE_SH && need_writable(table) != 0) {
    return -1;
  }
  std::string key;
  if (0 != id_key(table, id, key)) {
    return -1;
  }
  // Held to the end of the transaction, as a write's lock is: SQL's
  // lock of a row its statement read, refused on one changed since.
  return nowait ? sm2::rel_trylock(table->rel, key, mode, true)
                : sm2::rel_lock(table->rel, key, mode, true);
}

/* Lock the row with _id id without writing it; see sm3.h. */
int tbl_lock(table_t *table, uint64_t id, sm1::lockmode_t mode) {
  return lock_row(table, id, mode, false);
}

/* Lock the row with _id id if it is free now; see sm3.h. */
int tbl_trylock(table_t *table, uint64_t id, sm1::lockmode_t mode) {
  return lock_row(table, id, mode, true);
}

/*
 * The row key names: its key, encoded, into k, and its _id into id,
 * a keyless table's from the key, 0 for a keyed row, which has none
 * (../ROWSTORE.md §2.2). Nothing is read.
 */
static int named(table_t *table, const row_key_t &key, std::string &k,
                 uint64_t &id) {
  id = 0;
  if (0 != sm2::rel_key(table->rel, key.values, key.isnull, k)) {
    return -1;
  }
  if (!sm2::rel_keyed(table->rel)) {
    id = static_cast<uint64_t>(sm2::datum_to_int64(key.values[0]));
  }
  return 0;
}

/* Replace the row key names; see sm3.h. */
int tbl_update(table_t *table, const row_key_t &key,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull) {
  std::string k;
  uint64_t id;
  return 0 != named(table, key, k, id)
             ? -1
             : update_row(table, id, k, values, isnull, nullptr, nullptr);
}

/* As above, pinning the row replaced into ref; see sm3.h. */
int tbl_update(table_t *table, const row_key_t &key,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, pin_ref_t &ref,
               std::string_view &old) {
  old = {};
  std::string k;
  uint64_t id;
  return 0 != named(table, key, k, id)
             ? -1
             : update_row(table, id, k, values, isnull, &ref, &old);
}

/* Delete the row key names; see sm3.h. */
int tbl_delete(table_t *table, const row_key_t &key) {
  std::string k;
  uint64_t id;
  return 0 != named(table, key, k, id) ? -1
                                       : delete_row(table, k, nullptr, nullptr);
}

/* As above, pinning the row deleted into ref; see sm3.h. */
int tbl_delete(table_t *table, const row_key_t &key, pin_ref_t &ref,
               std::string_view &old) {
  old = {};
  std::string k;
  uint64_t id;
  return 0 != named(table, key, k, id) ? -1 : delete_row(table, k, &ref, &old);
}

/* Read the row key names; see sm3.h. */
int tbl_read(table_t *table, const row_key_t &key, std::string_view &tuple) {
  std::string k;
  uint64_t id;
  tuple = {};
  return 0 != named(table, key, k, id) ? -1
                                       : sm2::rel_read(table->rel, k, tuple);
}

/* Read the row key names into a pin; see sm3.h. */
int tbl_read(table_t *table, const row_key_t &key, pin_ref_t &ref,
             std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    std::string k;
    uint64_t id;
    return 0 != named(table, key, k, id)
               ? -1
               : sm2::rel_read(table->rel, k, raw, tuple);
  });
}

/* Read the newest committed version of the row key names. */
int tbl_read_latest(table_t *table, const row_key_t &key,
                    std::string_view &tuple) {
  std::string k;
  uint64_t id;
  tuple = {};
  return 0 != named(table, key, k, id)
             ? -1
             : sm2::rel_read_latest(table->rel, k, tuple);
}

/* As above, into a pin; see sm3.h. */
int tbl_read_latest(table_t *table, const row_key_t &key, pin_ref_t &ref,
                    std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    std::string k;
    uint64_t id;
    return 0 != named(table, key, k, id)
               ? -1
               : sm2::rel_read_latest(table->rel, k, raw, tuple);
  });
}

/*
 * Lock the row key names, waiting or not, as lock_row does a row
 * named by its _id.
 */
static int lock_key(table_t *table, const row_key_t &key, sm1::lockmode_t mode,
                    bool nowait) {
  // Only a shared lock sits under a table opened to read.
  if (sm1::SM_LOCKMODE_SH != mode && 0 != need_writable(table)) {
    return -1;
  }
  std::string k;
  uint64_t id;
  if (0 != named(table, key, k, id)) {
    return -1;
  }
  // Held to the end of the transaction, as a write's lock is: SQL's
  // lock of a row its statement read, refused on one changed since.
  return nowait ? sm2::rel_trylock(table->rel, k, mode, true)
                : sm2::rel_lock(table->rel, k, mode, true);
}

/* The encoding of a row's name; see sm3.h. */
int tbl_row_key(table_t *table, const row_key_t &key, std::string &out) {
  return sm2::rel_key(table->rel, key.values, key.isnull, out);
}

/* Lock the row key names without writing it; see sm3.h. */
int tbl_lock(table_t *table, const row_key_t &key, sm1::lockmode_t mode) {
  return lock_key(table, key, mode, false);
}

/* Lock the row key names if it is free now; see sm3.h. */
int tbl_trylock(table_t *table, const row_key_t &key, sm1::lockmode_t mode) {
  return lock_key(table, key, mode, true);
}

/* Empty the table, then reopen its rel and indexes; see sm3.h. */
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

/*
 * Open a cursor with its pin over the rows sm2's cursor, which open
 * makes, walks.
 */
template <typename F>
static int open_scan(table_t *table, scan_t *&out, F open) {
  out = nullptr;
  pin_t *p = pin_take(table->session);
  sm2::rel_scan_t *rel = nullptr;
  if (open(rel) != 0) {
    pin_put(p);
    return -1;
  }
  out = new scan_t{rel, p};
  pin_scan_hold(p, out, table->def);
  return 0;
}

/* Open a cursor over the rows first..last, with its pin. */
int tbl_scan(table_t *table, uint64_t first, uint64_t last, scan_t *&out) {
  return open_scan(table, out, [&](sm2::rel_scan_t *&rel) {
    return sm2::rel_scan(table->rel, first, last, rel);
  });
}

/* Open a cursor over every row in its key's order; see sm3.h. */
int tbl_scan_rows(table_t *table, scan_t *&out) {
  const index_def_t *pk = primary_of(*table->def);
  if (!pk) {
    return tbl_scan(table, 0, UINT64_MAX, out);
  }
  for (size_t i = 0; i < table->def->indexes.size(); i++) {
    if (&table->def->indexes[i] == pk) {
      return open_scan(table, out, [&](sm2::rel_scan_t *&rel) {
        return sm2::idx_scan_rows(table->indexes[i], nullptr, nullptr, rel);
      });
    }
  }
  return -1; /* not reached: pk is one of the table's indexes */
}

/*
 * Whether the row tuple, at key row, still has the tree entry key in
 * the table's index i: its entry, made of the row as a write would
 * make it, is key, and the row is in the index (../BTREEINDEX.md §5).
 * 1, 0, or -1 on error.
 */
static int still_has(table_t *t, size_t i, std::string_view tuple,
                     std::string_view row, std::string_view key,
                     std::string &k) {
  const table_def_t &def = *t->def;
  const index_def_t &x = def.indexes[i];
  // The row read only as far as the last column the key and the
  // evaluator read: a wide row's others cost nothing; they are null
  // to the evaluator, which reads none of them.
  size_t n = 0;
  for (int32_t att : x.key_attnums) {
    n = std::max(n, static_cast<size_t>(att + 1));
  }
  for (int32_t att : x.reads) {
    n = std::max(n, static_cast<size_t>(att + 1));
  }
  // The row's values and its entry, in buffers the thread keeps: a
  // recheck is made a row, and a row is read a few hundred
  // instructions apart. No call it makes rechecks again.
  thread_local std::vector<sm2::datum_t> d;
  thread_local std::unique_ptr<bool[]> nulls;
  thread_local size_t nulls_cap = 0;
  thread_local entry_t e;
  size_t natts = def.schema.size();
  d.assign(natts, 0);
  if (nulls_cap < natts) {
    nulls.reset(new bool[natts]);
    nulls_cap = natts;
  }
  bool *isnull = nulls.get();
  std::fill(isnull, isnull + natts, true);
  sm2::tup_reader_t reader;
  if (sm2::tup_read_begin(def.desc, tuple, reader) != 0 ||
      sm2::tup_read(reader, n, d, {isnull, natts}) != 0) {
    return -1;
  }
  vals_t r{d, {isnull, natts}};
  // A value out of line is fetched into an image of the row, so a
  // key or the evaluator reads it whole; most rows have none.
  bool out_of_line = false;
  for (const std::vector<int32_t> *atts : {&x.key_attnums, &x.reads}) {
    for (int32_t att : *atts) {
      out_of_line |= att >= 0 && is_pointer(def, r, att);
    }
  }
  if (out_of_line) {
    image_t img(r);
    if (fetch_cols(t, img, x.key_attnums) != 0 ||
        fetch_cols(t, img, x.reads) != 0 ||
        make_entry(t, i, img.row(), e) != 0) {
      return -1;
    }
    if (!e.belongs) {
      return 0;
    }
    // The image goes with this scope; the entry's key points into it.
    if (sm2::idx_entry_key(t->indexes[i], e.d(), e.null(), row, k) != 0) {
      return -1;
    }
    return k == key ? 1 : 0;
  }
  if (make_entry(t, i, r, e) != 0) {
    return -1;
  }
  if (!e.belongs) {
    return 0;
  }
  if (sm2::idx_entry_key(t->indexes[i], e.d(), e.null(), row, k) != 0) {
    return -1;
  }
  return k == key ? 1 : 0;
}

/* The most entries a tree's cursor reads the rows of in one call. */
constexpr size_t tree_batch_max = 64;

/*
 * Below this, the absolute correlation of a tree's leading key with
 * the rows' order says its rows are scattered (../ROWSTORE.md
 * ../ROWSTORE.md §7.2).
 */
constexpr float tree_scattered_below = 0.5f;

/*
 * Whether the last ANALYZE of t found the rows of its index i
 * scattered in the store: the correlation of the index's leading key
 * with the rows' order, its column's or its expression's, is near 0.
 * False with no statistics, or no correlation.
 */
static bool tree_scattered(table_t *t, size_t i) {
  std::shared_ptr<const stats_t> stats;
  if (0 != cat_get_stats(t->session, t->def->oid, stats) || !stats) {
    return false;
  }
  const index_def_t &x = t->def->indexes[i];
  if (x.key_attnums.empty()) {
    return false;
  }
  int32_t att = x.key_attnums[0];
  for (const stat_t &st : stats->stats) {
    bool mine = att >= 0
                    ? !st.has_index && att == st.position
                    : st.has_index && x.oid == st.index && 0 == st.position;
    if (mine && st.has_correlation) {
      return std::fabs(st.correlation) < tree_scattered_below;
    }
  }
  return false;
}

/*
 * Read a tree's cursor's next entries, scan->batch of them or what is
 * left, and their rows at the transaction's snapshot, in one call
 * (../ROWSTORE.md); the batch doubles for the next.
 */
static int tree_fill(scan_t *scan) {
  table_t *t = scan->table;
  scan->at = scan->n = 0;
  size_t want = scan->batch;
  scan->batch = std::min(scan->batch * 2, tree_batch_max);
  if (scan->keys.size() < want) {
    // Room for the largest batch at the second, so the doubling
    // grows no array again; a probe of one row stops at the first.
    if (want > 1 && scan->keys.capacity() < tree_batch_max) {
      for (auto *v : {&scan->keys, &scan->rows, &scan->tuples}) {
        v->reserve(tree_batch_max);
      }
      scan->key_at.reserve(tree_batch_max);
      scan->row_at.reserve(tree_batch_max);
      scan->ids.reserve(tree_batch_max);
      scan->rcs.reserve(tree_batch_max);
      scan->cuts.reserve(tree_batch_max);
      scan->unread.reserve(tree_batch_max);
      scan->raws.reserve(tree_batch_max);
    }
    scan->keys.resize(want);
    scan->rows.resize(want);
    scan->key_at.resize(want);
    scan->row_at.resize(want);
    scan->ids.resize(want);
    scan->tuples.resize(want);
    scan->rcs.resize(want);
    scan->cuts.resize(want);
    scan->unread.resize(want);
    // The first row is read into the cursor's own pin, so a probe of
    // one row, an IN list's, makes none.
    if (scan->raws.empty()) {
      scan->raws.push_back(scan->pin->raw);
    }
    // The others from the session's pool, which an ended cursor's
    // batch filled; new ones past it.
    std::vector<sm1::rawpin_t *> &pool = scan->session->raws_free;
    while (scan->raws.size() < want) {
      if (pool.empty()) {
        scan->raws.push_back(sm1::rawpin_new());
      } else {
        scan->raws.push_back(pool.back());
        pool.pop_back();
      }
    }
  }
  // The batch's keys end to end, viewed once all are in.
  scan->bytes.clear();
  while (scan->n < want && !scan->entries_done) {
    std::string_view row;
    bool cut;
    int rc = sm2::idx_scan_entry(scan->entries, scan->ids[scan->n], cut, row);
    if (rc < 0) {
      return -1;
    }
    if (rc == 1) {
      scan->entries_done = true;
      break;
    }
    std::string_view key = sm2::idx_scan_entry_key(scan->entries);
    scan->key_at[scan->n] = {scan->bytes.size(), key.size()};
    scan->bytes.append(key);
    scan->row_at[scan->n] = {scan->bytes.size(), row.size()};
    scan->bytes.append(row);
    scan->cuts[scan->n] = cut;
    // An index-only scan's entry on a leaf all visible is every
    // snapshot's: its row goes unread where the key gives its
    // columns back (../BTREEINDEX.md §8).
    scan->unread[scan->n] = scan->entries_only && !cut &&
                            sm2::idx_scan_visible(scan->entries) &&
                            sm2::idx_decodable(t->indexes[scan->index]);
    scan->tuples[scan->n] = {};
    scan->rcs[scan->n] = 0;
    scan->n++;
  }
  for (size_t i = 0; i < scan->n; i++) {
    scan->keys[i] = std::string_view(scan->bytes)
                        .substr(scan->key_at[i].first, scan->key_at[i].second);
    scan->rows[i] = std::string_view(scan->bytes)
                        .substr(scan->row_at[i].first, scan->row_at[i].second);
  }
  // The rows to read: each entry's but an unread one's.
  std::vector<size_t> &want_rows = scan->want_rows;
  want_rows.clear();
  for (size_t i = 0; i < scan->n; i++) {
    if (!scan->unread[i]) {
      want_rows.push_back(i);
    }
  }
  if (want_rows.empty()) {
    return 0;
  }
  if (1 == want_rows.size()) {
    size_t i = want_rows[0];
    int rc =
        sm2::rel_read(t->rel, scan->rows[i], scan->raws[i], scan->tuples[i]);
    scan->rcs[i] = rc;
    return rc < 0 ? -1 : 0;
  }
  size_t m = want_rows.size();
  std::vector<std::string_view> &keys = scan->read_keys;
  std::vector<std::string_view> &tuples = scan->read_tuples;
  std::vector<sm1::rawpin_t *> &raws = scan->read_raws;
  std::vector<int> &rcs = scan->read_rcs;
  keys.resize(m);
  tuples.resize(m);
  raws.resize(m);
  rcs.resize(m);
  for (size_t j = 0; j < m; j++) {
    keys[j] = scan->rows[want_rows[j]];
    raws[j] = scan->raws[want_rows[j]];
  }
  if (!scan->iter) {
    // Scattered rows skip the iterator from the first batch: a seek
    // costs more than a MultiGet's key, and buys one row.
    scan->iter = sm1::batch_iter_new(tree_scattered(t, scan->index));
  }
  if (0 != sm2::rel_read_batch(t->rel, keys.data(), m, raws.data(),
                               tuples.data(), rcs.data(), scan->iter)) {
    return -1;
  }
  for (size_t j = 0; j < m; j++) {
    scan->tuples[want_rows[j]] = tuples[j];
    scan->rcs[want_rows[j]] = rcs[j];
  }
  return 0;
}

/*
 * A tree's cursor's next row: the next entry's row, as the
 * transaction's snapshot shows it, that still has the entry's key; an
 * entry whose row does not is dead, and skipped. The row stays in its
 * batch's raw pin, which the cursor's pin then views, until the next
 * batch is read.
 */
static int tree_next(scan_t *scan, uint64_t &id, std::string_view &tuple) {
  table_t *t = scan->table;
  for (;;) {
    if (scan->at == scan->n) {
      if (scan->entries_done) {
        return 1;
      }
      if (tree_fill(scan) != 0) {
        return -1;
      }
      if (scan->n == 0) {
        return 1;
      }
    }
    size_t i = scan->at++;
    scan->last = i;
    if (scan->unread[i]) {
      id = scan->ids[i];
      tuple = {};
      return 0;
    }
    if (scan->rcs[i] != 0) {
      continue;
    }
    int rc = still_has(t, scan->index, scan->tuples[i], scan->rows[i],
                       scan->keys[i], scan->key);
    if (rc < 0) {
      return -1;
    }
    if (rc == 1) {
      id = scan->ids[i];
      tuple = scan->tuples[i];
      return 0;
    }
  }
}

} // namespace sm3

namespace {

/*
 * Add a unique tree's entry e, of index i, for the row with key row
 * (../BTREEINDEX.md §7).
 * 1. Judge each other entry of its key by its row's newest version,
 *    past the snapshot.
 * 2. A row committed, or this transaction's own, that still has the
 *    entry's key is a duplicate: fail with SM_ERR_DUPKEY.
 * 3. A row another transaction is still writing: wait for it,
 *    outside every latch, and check the key again from step 1.
 * 4. Take any other entry as dead.
 */
int unique_tree_insert(sm3::table_t *t, size_t i, const entry_t &e,
                       const picked_t &inc, std::string_view row) {
  sm3::pin_t *p = sm3::pin_take(t->session);
  uint64_t writer = 0;
  std::string kb;
  int rc;
  for (;;) {
    rc = sm2::idx_insert_unique(
        t->indexes[i], e.d(), e.null(), row, inc.d(), inc.null(),
        [&](std::string_view r, std::string_view key) {
          // Its own row's entry, an earlier key of it gone back to:
          // no other row's.
          if (r == row) {
            return 0;
          }
          std::string_view tuple;
          int n = sm2::rel_read_newest(t->rel, r, p->raw, tuple, writer);
          if (n != 0) {
            return n < 0 ? -1 : n == 1 ? 0 : 2;
          }
          int h = sm3::still_has(t, i, tuple, r, key, kb);
          if (h <= 0) {
            return h;
          }
          sm1::seterr(sm1::SM_ERR_DUPKEY, "duplicate key in unique index " +
                                              t->def->indexes[i].name);
          return 1;
        });
    sm1::unpin(p->raw);
    if (rc != 2) {
      break;
    }
    // Every latch is let go: wait for the writer, then look again.
    if (sm1::wait_writer(t->session->sm1, writer) != 0) {
      rc = -1;
      break;
    }
  }
  sm3::pin_put(p);
  return rc;
}

/*
 * The row a unique tree's key e names (../BTREEINDEX.md §7), as idx_find
 * and idx_find_latest give a unique index's: 0 with its _id and key
 * in row, 1 if no row has the key, -1.
 * 1. Return 1 for a key with a null column, which is no row's.
 * 2. Read the row of each entry of the key: as the snapshot shows
 *    it, or with now, its newest version.
 * 3. With now, if a writer of the row is running, wait for it and
 *    look the key up again from step 2.
 * 4. Take the first row that still has the key.
 * 5. With now, lock the row shared, as idx_find_latest locks the key.
 */
int tree_find(sm3::table_t *t, size_t i, const entry_t &e, bool now,
              uint64_t &found, std::string &row) {
  for (size_t j = 0; j < e.key.size(); j++) {
    if (e.key_null[j]) {
      return 1;
    }
  }
  sm2::bound_t b{e.d(), e.null(), true};
  sm3::pin_t *p = sm3::pin_take(t->session);
  std::string kb;
  int rc;
  for (;;) {
    sm2::index_scan_t *scan = nullptr;
    if (sm2::idx_scan(t->indexes[i], &b, &b, scan) != 0) {
      rc = -1;
      break;
    }
    uint64_t writer = 0;
    rc = 1;
    for (;;) {
      std::string_view r;
      bool cut;
      int n = sm2::idx_scan_entry(scan, found, cut, r);
      if (n != 0) {
        rc = n < 0 ? -1 : 1;
        break;
      }
      std::string key(sm2::idx_scan_entry_key(scan));
      std::string_view tuple;
      n = now ? sm2::rel_read_newest(t->rel, r, p->raw, tuple, writer)
              : sm2::rel_read(t->rel, r, p->raw, tuple);
      if (n < 0) {
        rc = -1;
        break;
      }
      if (n == 2) {
        rc = 2;
        break;
      }
      if (n == 1) {
        continue;
      }
      int h = sm3::still_has(t, i, tuple, r, key, kb);
      if (h < 0) {
        rc = -1;
        break;
      }
      if (h == 1) {
        row.assign(r);
        rc = 0;
        break;
      }
    }
    sm2::idx_scanend(scan);
    sm1::unpin(p->raw);
    if (rc == 0 && now) {
      // The row locked, as idx_find_latest locks the key; a writer
      // that got in first is waited for by the lock.
      if (sm2::rel_lock(t->rel, row, sm1::SM_LOCKMODE_SH) != 0) {
        rc = -1;
      }
      break;
    }
    if (rc != 2) {
      break;
    }
    // Every latch is let go: wait for the writer, then look again.
    if (sm1::wait_writer(t->session->sm1, writer) != 0) {
      rc = -1;
      break;
    }
  }
  sm3::pin_put(p);
  return rc;
}

} // namespace

namespace sm3 {

/* Open a cursor over the primary key's rows, with its pin; see sm3.h. */
int tbl_idx_scan_rows(table_t *table, uint64_t index, const sm2::bound_t *lo,
                      const sm2::bound_t *hi, scan_t *&out, bool *widened) {
  const table_def_t &def = *table->def;
  for (size_t i = 0; i < def.indexes.size(); i++) {
    const index_def_t &x = def.indexes[i];
    if (x.oid != index) {
      continue;
    }
    if (!(x.primary || x.tree) || !index_usable(table->session, def, index)) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "table: index " + x.name +
                             " is not the primary key or a tree, or not "
                             "usable");
    }
    if (x.tree) {
      pin_t *p = pin_take(table->session);
      sm2::index_scan_t *entries = nullptr;
      if (sm2::idx_scan(table->indexes[i], lo, hi, entries, widened) != 0) {
        pin_put(p);
        return -1;
      }
      out = new scan_t{nullptr, p, entries, table, i, table->session};
      pin_scan_hold(p, out, table->def);
      return 0;
    }
    return open_scan(table, out, [&](sm2::rel_scan_t *&rel) {
      return sm2::idx_scan_rows(table->indexes[i], lo, hi, rel, widened);
    });
  }
  return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                     "table: no index " + std::to_string(index));
}

/* The next row, in the cursor's pin; see sm3.h. */
int tbl_scannxt(scan_t *scan, uint64_t &id, pin_ref_t &ref,
                std::string_view &tuple) {
  ref = pin_ref_t{};
  int rc = scan->entries ? tree_next(scan, id, tuple)
                         : sm2::rel_scannxt(scan->rel, id, tuple);
  // Every ref to the row before goes stale, whatever comes next.
  pin_scan_move(scan->pin, rc == 0 ? tuple : std::string_view());
  // An entry handed out with its row unread names no row.
  if (rc == 0 && !(scan->entries && scan->unread[scan->last])) {
    ref = pin_ref_t{scan->pin, scan->pin->gen};
  }
  return rc;
}

/* Hand out a tree's entries with their rows unread; see sm3.h. */
int tbl_scan_entries_only(scan_t *scan) {
  if (!scan->entries) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "table: only a tree's cursor hands out entries");
  }
  scan->entries_only = true;
  return 0;
}

/* The key columns of a tree's cursor's last entry; see sm3.h. */
int tbl_scan_cols(scan_t *scan, std::span<sm2::datum_t> values,
                  std::span<bool> isnull) {
  if (!scan->entries || 0 == scan->n) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "table: no tree entry to give the columns of");
  }
  size_t i = scan->last;
  return sm2::idx_entry_cols(scan->table->indexes[scan->index], scan->keys[i],
                             scan->cuts[i], scan->datums, values, isnull);
}

/* The next row's bytes; see sm3.h. */
int tbl_scannxt(scan_t *scan, uint64_t &id, std::string_view &tuple) {
  pin_ref_t ref;
  return tbl_scannxt(scan, id, ref, tuple);
}

/* Aim a tree's cursor of rows at new bounds; see sm3.h. */
int tbl_scan_rebound(scan_t *scan, const sm2::bound_t *lo,
                     const sm2::bound_t *hi, bool *widened) {
  if (!scan->entries) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "table: only a tree's cursor is aimed again");
  }
  if (sm2::idx_scan_rebound(scan->entries, lo, hi, widened) != 0) {
    return -1;
  }
  // Its batch, read under the old bounds, goes; its buffers stay.
  pin_scan_move(scan->pin, std::string_view());
  scan->at = scan->n = 0;
  scan->batch = 1;
  scan->entries_done = false;
  return 0;
}

/* Move the cursor forward to _id id; see sm3.h. */
int tbl_scan_seek(scan_t *scan, uint64_t id) {
  if (scan->entries) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "table: a tree's cursor seeks "
                                            "no row");
  }
  return sm2::rel_scan_seek(scan->rel, id);
}

/* Move a cursor in key order forward to a key; see sm3.h. */
int tbl_scan_seek_key(scan_t *scan, std::string_view key) {
  if (scan->entries) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "table: a tree's cursor seeks "
                                            "no row");
  }
  return sm2::rel_scan_seek_key(scan->rel, key);
}

/* The key of the cursor's last row; see sm3.h. */
std::string_view tbl_scan_key(const scan_t *scan) {
  return scan->entries ? std::string_view(scan->rows[scan->at - 1])
                       : sm2::rel_scan_key(scan->rel);
}

/* Close a cursor, releasing its pin; see sm3.h. */
void tbl_scanend(scan_t *scan) {
  if (!scan) {
    return;
  }
  sm2::rel_scanend(scan->rel);
  sm2::idx_scanend(scan->entries);
  pin_release(scan->pin);
  delete scan;
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

/* The rows' bytes, their older versions too, in files and in memtables. */
int tbl_size(table_t *table, uint64_t &disk, uint64_t &mem) {
  return sm2::relid_size(table->session->sm1, table->def->storeid, heap_relid,
                         disk, mem, true);
}

/* One index's bytes, in files and in memtables. */
int tbl_idx_size(table_t *table, uint64_t index, uint64_t &disk,
                 uint64_t &mem) {
  for (const index_def_t &x : table->def->indexes) {
    if (x.oid == index) {
      // A primary key's entries are the rows, which tbl_size counts:
      // it adds nothing to them.
      if (x.primary) {
        disk = mem = 0;
        return 0;
      }
      // A tree's entries are its pages.
      if (x.tree) {
        return sm2::idx_tree_size(table->session->sm1, table->def->storeid,
                                  x.relid, disk, mem);
      }
      return sm2::relid_size(table->session->sm1, table->def->storeid, x.relid,
                             disk, mem);
    }
  }
  return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                     "table: no index " + std::to_string(index));
}

/* N, the last _id the table's sequence handed out, 0 if none. */
int tbl_id_limit(table_t *table, uint64_t &n) {
  int rc = sm1::lastseq(table->session->sm1, table->def->id_seq, n);
  // A sequence that never handed out an _id: no row ever was.
  if (rc == 1) {
    n = 0;
    return 0;
  }
  return rc;
}

/* The keys at fractions of the table's rows' bytes; see sm3.h. */
int tbl_keys_at(table_t *table, std::span<const double> fs,
                std::vector<std::string> &keys) {
  return sm2::rel_keys_at(table->rel, fs, keys);
}

/* The bytes of the table's rows in a key range; see sm3.h. */
int tbl_range_size(table_t *table, std::string_view lo, const std::string *hi,
                   uint64_t &out) {
  return sm2::rel_range_size(table->rel, lo, hi, out);
}

/* The sorted runs a lookup in the table's store may visit. */
int tbl_probes(table_t *table, uint64_t &n) {
  return sm1::probes(table->session->sm1, table->def->storeid, n);
}

/*
 * Judge index i's tree entry key by its row: 1 dead, the row settled
 * and gone or of another key; 2 settled and live; 0 not settled; or
 * -1 (../BTREEINDEX.md §9).
 */
static int judge_entry(table_t *table, size_t i, sm1::rawpin_t *pin,
                       std::string_view row, std::string_view key) {
  std::string_view tuple;
  int r = sm2::rel_read_settled(table->rel, row, pin, tuple);
  if (r != 0) {
    return r < 0 ? -1 : r == 1 ? 1 : 0;
  }
  std::string k;
  r = still_has(table, i, tuple, row, key, k);
  return r < 0 ? -1 : r == 0 ? 1 : 2;
}

/*
 * Clean a leaf with no transaction and no lock; see sm3.h.
 * 1. Make the table's rel and indexes from def, which reads nothing.
 * 2. Find the index by its relid. Gone, there is nothing to clean.
 * 3. Clean the leaf. After each row is read, compare the catalog's
 *    publishes with those noted before def was fetched. If they moved,
 *    def may be stale for that row, so leave its entry in doubt. A
 *    commit publishes before any later commit, so a row written under
 *    a newer definition is read only after the count moved.
 */
int tbl_clean_leaf(session_t *session, std::shared_ptr<const table_def_t> def,
                   uint64_t publishes, uint16_t relid, uint64_t pageid,
                   bool &doubt) {
  std::unique_ptr<table_t, void (*)(table_t *)> t(new table_t, tbl_close);
  t->session = session;
  t->def = std::move(def);
  t->writable = false;
  t->exclusive = false;
  t->fresh = false;
  t->settled = true;
  load_states(t.get());
  if (open_rels(t.get()) != 0) {
    return -1;
  }
  const table_def_t &d = *t->def;
  size_t i = 0;
  while (i < d.indexes.size() &&
         (d.indexes[i].relid != relid || !d.indexes[i].tree)) {
    i++;
  }
  if (i == d.indexes.size()) {
    return 1;
  }
  pin_t *p = pin_take(session);
  defer { pin_put(p); };
  size_t removed = 0;
  return sm2::idx_clean_leaf(
      t->indexes[i], pageid,
      [&](std::string_view row, std::string_view key) {
        int r = judge_entry(t.get(), i, p->raw, row, key);
        sm1::unpin(p->raw);
        return r > 0 && cat_publishes(session) != publishes ? 0 : r;
      },
      removed, doubt);
}

/* Compact the table's store into one sorted run. */
int tbl_compact(table_t *table) {
  if (sm1::compact(table->session->sm1, table->def->storeid) != 0) {
    return -1;
  }
  // Each tree's dead entries go: an entry whose row, settled, is gone
  // or has another key (../BTREEINDEX.md §9). One whose row a snapshot may
  // still see otherwise stays, for a later vacuum. Only an entry in
  // doubt is read, and a leaf with none left is set all visible
  // (../BTREEINDEX.md §9).
  const table_def_t &def = *table->def;
  for (size_t i = 0; i < def.indexes.size(); i++) {
    if (!def.indexes[i].tree) {
      continue;
    }
    pin_t *p = pin_take(table->session);
    size_t removed = 0;
    int rc = sm2::idx_vacuum(
        table->indexes[i],
        [&](std::string_view row, std::string_view key) {
          return judge_entry(table, i, p->raw, row, key);
        },
        removed);
    sm1::unpin(p->raw);
    pin_put(p);
    if (rc != 0) {
      return -1;
    }
  }
  return 0;
}

/* One batch of an index's build; see sm3.h. */
int tbl_index_backfill(table_t *table, uint64_t index, std::string_view from,
                       size_t max_rows, std::string &next, bool &done) {
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
  if (table->states[i] != index_state_t::pending || x.primary) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index build: index " + x.name +
                           " is not pending, or is the rows' own key");
  }
  // Rows another transaction may write are read at their newest
  // version, which only READ COMMITTED can do.
  bool rc = table->session->level == sm1::xctlevel_t::read_committed;
  if (!table->fresh && !rc) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index build: not at READ COMMITTED");
  }
  // The batch's rows, in key order from from on, each with its
  // _id; their tuples are read again below.
  std::vector<std::pair<uint64_t, std::string>> rows;
  scan_t *scan = nullptr;
  if (0 != tbl_scan_rows(table, scan)) {
    return -1;
  }
  uint64_t id;
  std::string_view tuple;
  int more = !from.empty() && 0 != tbl_scan_seek_key(scan, from) ? -1 : 0;
  while (0 == more && rows.size() < max_rows &&
         0 == (more = tbl_scannxt(scan, id, tuple))) {
    rows.push_back({id, std::string(tbl_scan_key(scan))});
  }
  tbl_scanend(scan);
  if (more < 0) {
    return -1;
  }
  deformed_t row(def.schema.size());
  for (const auto &[r, row_key] : rows) {
    int found;
    if (table->fresh) {
      found = sm2::rel_read(table->rel, row_key, tuple);
    } else {
      // Hold the row shared to the end of the batch: a writer
      // changes the row first, so it and the build never work on
      // one row at once (../INDEXBUILD.md §2). A row moved since the
      // scan is gone from its key; its mover made its entry.
      if (sm2::rel_lock(table->rel, row_key, sm1::SM_LOCKMODE_SH) != 0) {
        return -1;
      }
      found = sm2::rel_read_latest(table->rel, row_key, tuple);
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
    if (!key.belongs || add_entry(table, i, key, values.row(), row_key) == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY) {
      return -1;
    }
    // An entry is there already. With the row's key in its key it
    // is this row's, made by a writer; without, it may be another's.
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
    while (0 == (more = sm2::idx_scannxt(is, owner, cut, value))) {
      mine |= sm2::idx_scan_row(is) == row_key;
    }
    sm2::idx_scanend(is);
    if (more < 0) {
      return -1;
    }
    if (!mine) {
      sm1::seterr(sm1::SM_ERR_DUPKEY, "index build: two rows of " + def.name +
                                          " have one key in " + x.name);
      // A row, for the host to show the key it holds.
      sm1::setdup({x.oid, row_key});
      return -1;
    }
  }
  done = rows.size() < max_rows;
  // The least key past the last row's.
  next = rows.empty() ? std::string(from) : rows.back().second + '\0';
  return 0;
}

/*
 * Ask the host's converter for a row's new value. Its type, a
 * std::function, cannot say noexcept: asked from here, one that
 * throws ends the process, where it would unwind through a rewrite.
 */
static int ask_converter(const column_conv_fn &fn,
                         std::span<const sm2::datum_t> values,
                         std::span<const bool> isnull, std::string &out,
                         bool &out_null) noexcept {
  return fn(values, isnull, out, out_null);
}

/*
 * Put conv's value of the row's column into row: the converter's
 * bytes, held in own, as a datum of the column's type in to.
 */
static int convert_value(const table_t *to, const column_conv_t &conv,
                         deformed_t &row, std::string &own) {
  std::span<const sm2::datum_t> values(row.d.data(), row.d.size());
  std::span<const bool> isnull(row.null.get(), row.n);
  bool out_null = false;
  own.clear();
  if (ask_converter(*conv.fn, values, isnull, own, out_null) != 0) {
    return -1;
  }
  const column_def_t &c = to->def->columns[conv.attnum];
  // Checked here, as a key column's null would fail its key's
  // encoding first, with another error.
  if (out_null && c.not_null) {
    return sm1::seterr(sm1::SM_ERR_NOTNULL, "rewrite: the converter gave " +
                                                c.name + ", NOT NULL, a null");
  }
  row.null[conv.attnum] = out_null;
  if (out_null) {
    row.d[conv.attnum] = 0;
    return 0;
  }
  // The bytes as sm2::datum_bytes gives them, as make_entry takes an
  // evaluator's.
  int32_t width = sm2::datum_width(c.type, c.typmod);
  if (own.empty() || (width > 0 && own.size() != static_cast<size_t>(width))) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rewrite: the converter gave column " + c.name +
                           " a value of the wrong width");
  }
  if (c.type->byval) {
    row.d[conv.attnum] = 0;
    memcpy(&row.d[conv.attnum], own.data(), own.size());
  } else {
    row.d[conv.attnum] = sm2::datum_from_ptr(own.data());
  }
  return 0;
}

/*
 * Copy the rows of from into to, which is the same table on a new
 * store keyed anew, its values out of line fetched from from's
 * store, and its entries in every index to maintains
 * (../CLUSTER.md §8): a keyless row keeps its _id, a keyed row has
 * none, and the rows of a table that lost its primary key draw
 * theirs in key order (../ROWSTORE.md §2.6). With conv, each row's
 * value of its column is the converter's.
 */
static int copy_rows(table_t *from, table_t *to, const column_conv_t *conv) {
  const table_def_t &def = *from->def;
  std::vector<int32_t> all(def.schema.size());
  for (size_t i = 0; i < all.size(); i++) {
    all[i] = static_cast<int32_t>(i);
  }
  // In key order, one sorted run of the old store.
  scan_t *scan = nullptr;
  if (0 != tbl_scan_rows(from, scan)) {
    return -1;
  }
  std::unique_ptr<scan_t, void (*)(scan_t *)> guard(scan, tbl_scanend);
  deformed_t row(def.schema.size());
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = tbl_scannxt(scan, id, tuple)) == 0) {
    std::vector<std::string> own;
    uint64_t got;
    if (sm2::tup_deform(def.desc, tuple, row.d, {row.null.get(), row.n}) != 0 ||
        tbl_fetch_cols(from, all, row.d, {row.null.get(), row.n}, own) != 0) {
      return -1;
    }
    std::string converted;
    if (conv && convert_value(to, *conv, row, converted) != 0) {
      return -1;
    }
    if (insert_row(to, row.d, {row.null.get(), row.n}, id, got) == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY) {
      return -1;
    }
    // The row, by its key in the old store, which still holds the
    // table, for the host to show the key it holds: in the index the
    // insert met the duplicate in, the primary key if it named none.
    uint64_t index = sm1::errdup().index;
    const index_def_t *pk = primary_of(*to->def);
    if (0 == index && pk) {
      index = pk->oid;
    }
    for (const index_def_t &x : to->def->indexes) {
      if (x.oid == index) {
        sm1::seterr(sm1::SM_ERR_DUPKEY, "rewrite: two rows of " + def.name +
                                            " have one key in " + x.name);
        sm1::setdup({index, std::string(tbl_scan_key(scan))});
      }
    }
    return -1;
  }
  return rc == 1 ? 0 : -1;
}

/* Rewrite the table into a new store, keyed as rec says; see internal.h. */
int rewrite_table(session_t *session, uint64_t oid, table_rec_t rec,
                  const column_conv_t *conv) {
  // Every other transaction out, to the end of this one: the rows
  // are read from one store and written to another.
  table_t *from = nullptr;
  if (tbl_open(session, oid, sm1::SM_LOCKMODE_EX, from) != 0) {
    return -1;
  }
  std::unique_ptr<table_t, void (*)(table_t *)> own_from(from, tbl_close);
  const uint64_t old_store = from->def->storeid;
  // A new store, temporary if the table is, and the definition
  // after over it, not yet the catalog's: a copy that fails leaves
  // the table as it was.
  std::shared_ptr<const table_def_t> def;
  collation_lookup_t lookup = [session](uint64_t c,
                                        const sm2::collation_t *&out) {
    return session_collation(session, c, out);
  };
  // Its rows chained, a temporary table's too (../ROWSTORE.md §3.5).
  // The running statement's mark is moved past the store's making,
  // so the copy below may write it (sm1::stmt_advance()).
  const bool temp = rec.nspoid == nsp_temp;
  if (sm1::mkstore(session->sm1, rec.storeid, temp, true) != 0 ||
      sm1::stmt_advance(session->sm1) != 0 ||
      build_table(lookup, oid, rec, def) != 0) {
    return -1;
  }
  std::unique_ptr<table_t, void (*)(table_t *)> to(new table_t, tbl_close);
  to->session = session;
  to->def = def;
  to->writable = true;
  to->exclusive = true;
  to->fresh = from->fresh;
  load_states(to.get());
  if (open_rels(to.get()) != 0) {
    return -1;
  }
  // Each tree, empty, in the new store, before the rows fill it.
  for (sm2::index_t *ix : to->indexes) {
    if (sm2::idx_create(ix) != 0) {
      return -1;
    }
  }
  if (copy_rows(from, to.get(), conv) != 0 ||
      cat_put_table(session, oid, rec) != 0) {
    return -1;
  }
  // The old store goes at commit, with the rows and entries in it,
  // as a truncate's does (../CATALOG.md §8).
  return sm1::rmstore(session->sm1, old_store);
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

/* The flags this level was built with; see sm3.h. */
const char *build_flags() {
#ifdef SM3_BUILD_FLAGS
  return SM3_BUILD_FLAGS;
#else
  return "unknown";
#endif
}

} // namespace sm3
