/*
 * table.cc -- an open table and its writes (SM3_DESIGN.md §3): the
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
  size_t heap = 0; /* the stream of the table's rows */
  size_t ids = 0;  /* of their _id entries, keyed by a primary key */
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
    sm1::setdup({pk->oid, {0, 0}, 0});
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

/*
 * Add index i's entry e for the row r with _id id and key row. A
 * key the index holds already is SM_ERR_DUPKEY, with the index in
 * sm1::errdup().
 */
int add_entry(sm3::table_t *t, size_t i, const entry_t &e, const vals_t &r,
              uint64_t id, std::string_view row) {
  picked_t inc(r, t->def->indexes[i].include_attnums);
  if (sm2::idx_insert(t->indexes[i], e.d(), e.null(), id, row, inc.d(),
                      inc.null()) == 0) {
    return 0;
  }
  // Which index has the key, for the host to say it.
  if (sm1::errnum() == sm1::SM_ERR_DUPKEY) {
    sm1::setdup({t->def->indexes[i].oid, {0, 0}, 0});
  }
  return -1;
}

/*
 * Remove index i's entry e for the row with _id id and key row.
 * While the index is pending, an entry that is not there is no
 * error: the build has not reached the row (SM3_DESIGN.md §3).
 */
int remove_entry(sm3::table_t *t, size_t i, const entry_t &e, uint64_t id,
                 std::string_view row) {
  if (sm2::idx_delete(t->indexes[i], e.d(), e.null(), id, row) == 0) {
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
 * Open table's rel and indexes on the store its definition names:
 * the rows keyed by the primary key, if it has one, which is read
 * as an index of the rows.
 */
int open_rels(table_t *table) {
  const table_def_t &def = *table->def;
  sm1::session_t *s = table->session->sm1;
  // The sequence its TOAST ids come from, made with every table, or
  // none unless the host reads TOAST pointers; named, never read.
  std::string toast_seq =
      table->session->cat->toast ? toast_seq_name(def.oid) : "";
  const index_def_t *pk = primary_of(def);
  sm2::rel_key_t key;
  if (pk) {
    key = {pk->key, pk->key_attnums};
  }
  if (sm2::rel_open(s, def.storeid, heap_relid, def.schema, key, def.id_seq,
                    toast_seq, table->rel) != 0) {
    return -1;
  }
  for (const index_def_t &x : def.indexes) {
    sm2::index_t *ix = nullptr;
    if ((x.primary ? sm2::idx_open_primary(table->rel, x.include,
                                           x.include_attnums, ix)
                   : sm2::idx_open(s, def.storeid, x.relid, x.key, x.unique,
                                   x.include, pk != nullptr, ix)) != 0) {
      return -1;
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
  int rc = wait ? cat_lock_table(session, oid, mode, t->def)
                : cat_trylock_table(session, oid, mode, t->def);
  if (rc != 0) {
    return rc;
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
 * index: under a new _id, or under reserved if it is not 0, as a
 * rewrite keeps a row's. id is set on success.
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
        (e.belongs && add_entry(table, i, e, row.row(), r, key) != 0)) {
      return -1;
    }
  }
  id = r;
  return 0;
}

/*
 * The key of the row with _id id, into key (../CLUSTER.md §5): 0,
 * 1 if there is none, 2 if nowait and the row's _id entry is
 * another's, or -1. A table without a primary key has it at once;
 * a table with one, from its _id entry as the snapshot shows it. At
 * READ COMMITTED a row the snapshot does not show -- one newer than
 * it, which an insert's conflict found -- is looked for now, under
 * a shared lock on its _id entry, which sm1's newest read needs.
 */
int key_of(table_t *t, uint64_t id, bool nowait, std::string &key) {
  int rc = sm2::rel_find(t->rel, id, key);
  if (rc != 1 || t->session->level != sm1::xctlevel_t::read_committed) {
    return rc;
  }
  // Held to the end of the transaction, as a row's lock is.
  rc = sm2::rel_lock_id(t->rel, id, sm1::SM_LOCKMODE_SH, nowait);
  if (rc != 0) {
    return rc < 0 ? -1 : 2;
  }
  return sm2::rel_find_latest(t->rel, id, key);
}

/* The key of the row with _id id to write, or SM_ERR_NOTFOUND. */
int key_to_write(table_t *t, uint64_t id, std::string &key) {
  int rc = key_of(t, id, false, key);
  if (rc == 1) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND, "table: no row " +
                                                 std::to_string(id) + " in " +
                                                 t->def->name);
  }
  return rc;
}

/*
 * Where the row with _id id went, at READ COMMITTED, once a write
 * or a newest read found it gone from key (../CLUSTER.md §4): its
 * key now, from the newest version of its _id entry, read under a
 * shared lock on it held to the end of the transaction, so that
 * none moves it again. 0 with to set if it moved; 1 if it did not,
 * being gone or still at key, or if the table has no primary key
 * or the level is another; or -1.
 */
int moved_to(table_t *t, uint64_t id, std::string_view key, std::string &to) {
  if (!sm2::rel_keyed(t->rel) ||
      t->session->level != sm1::xctlevel_t::read_committed) {
    return 1;
  }
  if (sm2::rel_lock_id(t->rel, id, sm1::SM_LOCKMODE_SH) != 0) {
    return -1;
  }
  int rc = sm2::rel_find_latest(t->rel, id, to);
  return rc != 0 ? rc : to == key ? 1 : 0;
}

/*
 * A write of the row with _id id at key, by write, followed to
 * where the row moved if it found the row gone (moved_to): sm1
 * says SM_ERR_NOTFOUND once the newest version at key, read after
 * SM_ERR_UPDATED, is the move's erase. key is left the key the row
 * was written at.
 */
template <typename F>
int write_followed(table_t *t, uint64_t id, std::string &key, F write) {
  int rc = write(key);
  if (rc == 0 || sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
    return rc;
  }
  std::string reason = sm1::errmsg(), to;
  int m = moved_to(t, id, key, to);
  if (m < 0) {
    return -1;
  }
  if (m == 1) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND, reason);
  }
  key = std::move(to);
  return write(key);
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
 * the snapshot, its _id in found: 0, 1 if none has, or -1. sm1
 * reads a newest version only under a lock on the key, and the
 * lock is given back at once, by a savepoint: the caller is left
 * holding nothing of the key, or it would stand between the row's
 * owner and the row. READ COMMITTED alone has such a read.
 */
int find_now(sm3::table_t *t, size_t i, const entry_t &e, uint64_t &found) {
  int mark = sm3::savepoint(t->session);
  if (mark < 0) {
    return -1;
  }
  int rc = sm2::idx_find_latest(t->indexes[i], e.d(), e.null(), found);
  // Releases the lock; nothing was written since the mark.
  if (rc < 0 || sm3::rollback(t->session, mark) != 0) {
    return -1;
  }
  return rc;
}

/* Insert the row unless an arbiter index has its key; see sm3.h. */
int tbl_insert_arbiter(table_t *table, std::span<const sm2::datum_t> values,
                       std::span<const bool> isnull,
                       std::span<const uint64_t> arbiters, uint64_t &id) {
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
    int rc =
        sm2::idx_find(table->indexes[i], entry[i].d(), entry[i].null(), found);
    if (rc == 0 && rc_level) {
      rc = find_now(table, i, entry[i], found);
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
  uint64_t r = 0;
  std::string key;
  if (sm2::rel_reserve(table->rel, r) != 0 ||
      sm2::rel_row_key(table->rel, r, values, isnull, key) != 0) {
    return -1;
  }
  for (size_t n = 0; n <= arb.size(); n++) {
    size_t i = n < arb.size() ? arb[n] : primary;
    int rc = n < arb.size() ? add_entry(table, i, entry[i], row.row(), r, key)
                            : sm2::rel_insert_at(table->rel, r, values, isnull);
    if (rc == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY || i == SIZE_MAX) {
      return n < arb.size() ? -1 : row_failed(table);
    }
    // The key is a row's that the snapshot did not show: take
    // back the entries put in so far, and say which row has it.
    for (size_t m = 0; m < n; m++) {
      if (remove_entry(table, arb[m], entry[arb[m]], r, key) != 0) {
        return -1;
      }
    }
    uint64_t found = 0;
    rc = find_now(table, i, entry[i], found);
    if (rc < 0) {
      return -1;
    }
    if (rc == 1) {
      // Gone again already, deleted and committed: look once more.
      goto look;
    }
    id = found;
    return 1;
  }
  // Its entries in the indexes that are no arbiter: a duplicate in
  // one of those is an error, as in tbl_insert.
  for (size_t i = 0; i < table->indexes.size(); i++) {
    if (!maintained(table, i) ||
        std::find(arb.begin(), arb.end(), i) != arb.end()) {
      continue;
    }
    entry_t e;
    if (fetch_for(table, def.indexes[i], row) != 0 ||
        make_entry(table, i, row.row(), e) != 0 ||
        (e.belongs && add_entry(table, i, e, row.row(), r, key) != 0)) {
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
static int update_row(table_t *table, uint64_t id,
                      std::span<const sm2::datum_t> values,
                      std::span<const bool> isnull, pin_ref_t *ref,
                      std::string_view *old) {
  // No old row until the write succeeds.
  if (old) {
    *old = {};
  }
  std::string key;
  if (need_writable(table) != 0 || need_not_null(table, isnull) != 0 ||
      key_to_write(table, id, key) != 0) {
    return -1;
  }
  // The old row is pinned, in a pin of this call's, so the index
  // calls below leave it where it is; and its datums out of line
  // are held until its index entries are found and removed. The
  // pin goes to ref only once all of it succeeds, as values may
  // point into the row ref holds.
  pin_t *p = pin_take(table->session);
  if (!p) {
    return -1;
  }
  bool kept = false;
  defer {
    if (!kept) {
      pin_put(p);
    }
  };
  // A row whose primary key changes moves, and every entry of it,
  // naming it by its key, moves with it (../CLUSTER.md §4).
  std::string_view image;
  std::string at;
  if (0 != write_followed(table, id, key, [&](const std::string &k) {
        return sm2::rel_update(table->rel, k, id, values, isnull, p->raw, image,
                               true, &at);
      })) {
    return row_failed(table);
  }
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
    if ((was.belongs && remove_entry(table, i, was, id, key) != 0) ||
        (is.belongs && add_entry(table, i, is, is_row.row(), id, at) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the old row held out of line goes.
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
  return update_row(table, id, values, isnull, nullptr, nullptr);
}

/* As above, pinning the row replaced into ref; see sm3.h. */
int tbl_update(table_t *table, uint64_t id,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, pin_ref_t &ref,
               std::string_view &old) {
  return update_row(table, id, values, isnull, &ref, &old);
}

/*
 * tbl_delete, with ref and old both given or both null: with them,
 * the row deleted is pinned into ref, and old views it.
 */
static int delete_row(table_t *table, uint64_t id, pin_ref_t *ref,
                      std::string_view *old) {
  // No old row until the write succeeds.
  if (old) {
    *old = {};
  }
  std::string key;
  if (need_writable(table) != 0 || key_to_write(table, id, key) != 0) {
    return -1;
  }
  // The row is pinned, in a pin of this call's, so the index calls
  // below leave it where it is; and its datums out of line are
  // held until its index entries are found and removed. The pin
  // goes to ref only once all of it succeeds.
  pin_t *p = pin_take(table->session);
  if (!p) {
    return -1;
  }
  bool kept = false;
  defer {
    if (!kept) {
      pin_put(p);
    }
  };
  std::string_view image;
  if (0 != write_followed(table, id, key, [&](const std::string &k) {
        return sm2::rel_delete(table->rel, k, p->raw, image, true);
      })) {
    return -1;
  }
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
        (was.belongs && remove_entry(table, i, was, id, key) != 0)) {
      return -1;
    }
  }
  // The indexes are fixed: what the row held out of line goes.
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
  return delete_row(table, id, nullptr, nullptr);
}

/* As above, pinning the row deleted into ref; see sm3.h. */
int tbl_delete(table_t *table, uint64_t id, pin_ref_t &ref,
               std::string_view &old) {
  return delete_row(table, id, &ref, &old);
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
    // A table keyed by a primary key gets its rows in no order,
    // and their _id entries in order (../CLUSTER.md §5).
    bool keyed = sm2::rel_keyed(table->rel);
    if (sm1::load_open(s, storeid, l->load) != 0 ||
        sm1::load_stream(l->load, !keyed, l->heap) != 0 ||
        (keyed && sm1::load_stream(l->load, true, l->ids) != 0)) {
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
  int rc = sm2::rel_load(table->rel, l->load, l->heap, l->ids, values, isnull,
                         r, &key);
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
                      e.null(), r, key, inc.d(), inc.null()) != 0) {
      // Which index has the key, when it is found now, not at the
      // close.
      if (sm1::errnum() == sm1::SM_ERR_DUPKEY) {
        sm1::setdup({table->def->indexes[i].oid, {0, 0}, 0});
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
  int rc = key_of(table, id, false, key);
  return rc != 0 ? rc : sm2::rel_read(table->rel, key, tuple);
}

/*
 * Read the newest committed version of the row with _id id, by
 * read, sm2::rel_read_latest at a key: where the snapshot has the
 * row, or where it moved since, taking the row's lock there
 * exclusive, as the write that met SM_ERR_UPDATED took it where it
 * was (../CLUSTER.md §4).
 */
template <typename F>
static int read_latest(table_t *table, uint64_t id, F read) {
  std::string key, to;
  int rc = key_of(table, id, false, key);
  if (rc != 0 || (rc = read(key)) != 1) {
    return rc;
  }
  int m = moved_to(table, id, key, to);
  if (m != 0) {
    return m < 0 ? -1 : 1;
  }
  return sm2::rel_lock(table->rel, to, sm1::SM_LOCKMODE_EX) != 0 ? -1
                                                                 : read(to);
}

/* Read the newest committed version of the row with _id id. */
int tbl_read_latest(table_t *table, uint64_t id, std::string_view &tuple) {
  return read_latest(table, id, [&](const std::string &key) {
    return sm2::rel_read_latest(table->rel, key, tuple);
  });
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
  if (!p) {
    return -1;
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
    int rc = key_of(table, id, false, key);
    return rc != 0 ? rc : sm2::rel_read(table->rel, key, raw, tuple);
  });
}

/* Read the row at key into a pin; see sm3.h. */
int tbl_read_key(table_t *table, std::string_view key, pin_ref_t &ref,
                 std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    return sm2::rel_read(table->rel, key, raw, tuple);
  });
}

/* Read the newest committed version of the row into a pin. */
int tbl_read_latest(table_t *table, uint64_t id, pin_ref_t &ref,
                    std::string_view &tuple) {
  return read_into(table, ref, tuple, [&](sm1::rawpin_t *raw) {
    return read_latest(table, id, [&](const std::string &key) {
      return sm2::rel_read_latest(table->rel, key, raw, tuple);
    });
  });
}

/*
 * Lock the row with _id id, waiting or not; see sm3.h. A row there
 * is none of has nothing to lock, and a read of it then finds it
 * gone.
 */
static int lock_row(table_t *table, uint64_t id, sm1::lockmode_t mode,
                    bool nowait) {
  // Only a shared lock sits under a table opened to read.
  if (mode != sm1::SM_LOCKMODE_SH && need_writable(table) != 0) {
    return -1;
  }
  std::string key;
  int rc = key_of(table, id, nowait, key);
  if (rc != 0) {
    return rc < 0 ? -1 : rc == 2 ? 1 : 0;
  }
  // Held to the end of the transaction, as a write's lock is.
  return nowait ? sm2::rel_trylock(table->rel, key, mode)
                : sm2::rel_lock(table->rel, key, mode);
}

/* Lock the row with _id id without writing it; see sm3.h. */
int tbl_lock(table_t *table, uint64_t id, sm1::lockmode_t mode) {
  return lock_row(table, id, mode, false);
}

/* Lock the row with _id id if it is free now; see sm3.h. */
int tbl_trylock(table_t *table, uint64_t id, sm1::lockmode_t mode) {
  return lock_row(table, id, mode, true);
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
  if (!p) {
    return -1;
  }
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

/* Open a cursor over the primary key's rows, with its pin; see sm3.h. */
int tbl_idx_scan_rows(table_t *table, uint64_t index, const sm2::bound_t *lo,
                      const sm2::bound_t *hi, scan_t *&out, bool *widened) {
  const table_def_t &def = *table->def;
  for (size_t i = 0; i < def.indexes.size(); i++) {
    const index_def_t &x = def.indexes[i];
    if (x.oid != index) {
      continue;
    }
    if (!x.primary || !index_usable(table->session, def, index)) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "table: index " + x.name +
                             " is not the primary key, or not usable");
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
  int rc = sm2::rel_scannxt(scan->rel, id, tuple);
  // Every ref to the row before goes stale, whatever comes next.
  pin_scan_move(scan->pin, rc == 0 ? tuple : std::string_view());
  if (rc == 0) {
    ref = pin_ref_t{scan->pin, scan->pin->gen};
  }
  return rc;
}

/* The next row's bytes; see sm3.h. */
int tbl_scannxt(scan_t *scan, uint64_t &id, std::string_view &tuple) {
  pin_ref_t ref;
  return tbl_scannxt(scan, id, ref, tuple);
}

/* Move the cursor forward to _id id; see sm3.h. */
int tbl_scan_seek(scan_t *scan, uint64_t id) {
  return sm2::rel_scan_seek(scan->rel, id);
}

/* Close a cursor, releasing its pin; see sm3.h. */
void tbl_scanend(scan_t *scan) {
  if (!scan) {
    return;
  }
  sm2::rel_scanend(scan->rel);
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

/* The rows' bytes, in files and in memtables. */
int tbl_size(table_t *table, uint64_t &disk, uint64_t &mem) {
  return sm2::relid_size(table->session->sm1, table->def->storeid, heap_relid,
                         disk, mem);
}

/* One index's bytes, in files and in memtables. */
int tbl_idx_size(table_t *table, uint64_t index, uint64_t &disk,
                 uint64_t &mem) {
  for (const index_def_t &x : table->def->indexes) {
    if (x.oid == index) {
      // A primary key's entries are the rows, which tbl_size counts:
      // what it adds to them is the _id index.
      return sm2::relid_size(table->session->sm1, table->def->storeid,
                             x.primary ? sm2::id_relid : x.relid, disk, mem);
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

/* The sorted runs a lookup in the table's store may visit. */
int tbl_probes(table_t *table, uint64_t &n) {
  return sm1::probes(table->session->sm1, table->def->storeid, n);
}

/* Compact the table's store into one sorted run. */
int tbl_compact(table_t *table) {
  return sm1::compact(table->session->sm1, table->def->storeid);
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
  // The batch's rows, by _id, each with its key; their tuples are
  // read again below.
  std::vector<std::pair<uint64_t, std::string>> rows;
  sm2::rel_scan_t *scan = nullptr;
  if (sm2::rel_scan(table->rel, from, UINT64_MAX, scan) != 0) {
    return -1;
  }
  uint64_t id;
  std::string_view tuple;
  int more = 0;
  while (rows.size() < max_rows &&
         (more = sm2::rel_scannxt(scan, id, tuple)) == 0) {
    rows.push_back({id, std::string(sm2::rel_scan_key(scan))});
  }
  sm2::rel_scanend(scan);
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
      // one row at once (SM3_DESIGN.md §3). A row moved since the
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
    if (!key.belongs ||
        add_entry(table, i, key, values.row(), r, row_key) == 0) {
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
    uint64_t owner, other = 0;
    bool cut, mine = false;
    std::string_view value;
    while ((more = sm2::idx_scannxt(is, owner, cut, value)) == 0) {
      mine |= owner == r;
      if (owner != r) {
        other = owner;
      }
    }
    sm2::idx_scanend(is);
    if (more < 0) {
      return -1;
    }
    if (!mine) {
      sm1::seterr(sm1::SM_ERR_DUPKEY, "index build: rows " + std::to_string(r) +
                                          " and " + std::to_string(other) +
                                          " of " + def.name +
                                          " have one key in " + x.name);
      // The rows, for the host to show the key they hold.
      sm1::setdup({x.oid, {r, other}, 2});
      return -1;
    }
  }
  done = rows.size() < max_rows;
  next = rows.empty() ? from : rows.back().first + 1;
  return 0;
}

/*
 * Copy the rows of from into to, which is the same table on a new
 * store keyed anew, each with its _id, its values out of line
 * fetched from from's store, and its entries in every index to
 * maintains (../CLUSTER.md §8).
 */
static int copy_rows(table_t *from, table_t *to) {
  const table_def_t &def = *from->def;
  std::vector<int32_t> all(def.schema.size());
  for (size_t i = 0; i < all.size(); i++) {
    all[i] = static_cast<int32_t>(i);
  }
  sm2::rel_scan_t *scan = nullptr;
  if (sm2::rel_scan(from->rel, 0, UINT64_MAX, scan) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::rel_scan_t, void (*)(sm2::rel_scan_t *)> guard(
      scan, sm2::rel_scanend);
  deformed_t row(def.schema.size());
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm2::rel_scannxt(scan, id, tuple)) == 0) {
    std::vector<std::string> own;
    uint64_t got;
    if (sm2::tup_deform(def.desc, tuple, row.d, {row.null.get(), row.n}) != 0 ||
        tbl_fetch_cols(from, all, row.d, {row.null.get(), row.n}, own) != 0) {
      return -1;
    }
    if (insert_row(to, row.d, {row.null.get(), row.n}, id, got) == 0) {
      continue;
    }
    if (sm1::errnum() != sm1::SM_ERR_DUPKEY) {
      return -1;
    }
    // Which row has the key, for the host to show the key the two
    // hold, as an index build says it.
    const index_def_t *pk = primary_of(*to->def);
    std::string key;
    uint64_t other = 0;
    if (pk &&
        sm2::rel_row_key(to->rel, id, row.d, {row.null.get(), row.n}, key) ==
            0 &&
        sm2::rel_read(to->rel, key, tuple, &other) == 0) {
      sm1::seterr(sm1::SM_ERR_DUPKEY, "rewrite: rows " + std::to_string(id) +
                                          " and " + std::to_string(other) +
                                          " of " + def.name +
                                          " have one key in " + pk->name);
      sm1::setdup({pk->oid, {id, other}, 2});
    }
    return -1;
  }
  return rc == 1 ? 0 : -1;
}

/* Rewrite the table into a new store, keyed as rec says; see internal.h. */
int rewrite_table(session_t *session, uint64_t oid, table_rec_t rec) {
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
  if (sm1::mkstore(session->sm1, rec.storeid, rec.nspoid == nsp_temp) != 0 ||
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
  if (open_rels(to.get()) != 0 || copy_rows(from, to.get()) != 0 ||
      cat_put_table(session, oid, rec) != 0) {
    return -1;
  }
  // The old store goes at commit, with the rows and entries in it,
  // as a truncate's does (CATALOG.md §5.2).
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
