/*
 * index.cc -- the index rel (../ROWSTORE.md §8, ../CLUSTER.md §3):
 * a table's index entries in its sm1 store, under the index's
 * relid, keyed by their encoded key columns and naming their row by
 * its key, holding the cut flag, the INCLUDE columns and the row's
 * key, as a tuple (../ROWSTORE.md §2.2). A primary key is an index
 * whose entries are the rel's rows.
 *
 * A scan's bounds rest on the key encoding being prefix-free
 * column by column (TYPE.md §2): the encoding P of an entry's
 * first j columns is a prefix of exactly the keys whose first j
 * columns hold those values, and those keys are contiguous. So a
 * bound is P or its successor, the least string above every key
 * that starts with P.
 */

#include "../tree/tree.h"
#include "native.h"
#include "rel.h"
#include "sm2.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sm2::index_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  std::span<const key_column_t> key; /* borrowed */
  bool unique;
  bool keyed; /* its table's rows are keyed by a primary key */
  /* idx_open's layout of its INCLUDE columns; none from idx_open_layout */
  std::unique_ptr<idx_layout_t> own;
  const idx_layout_t *layout; /* own, or the one idx_open_layout borrows */
  /*
   * A primary key's: the rel whose rows are its entries, and the
   * attribute there of each INCLUDE column. nullptr otherwise.
   */
  sm2::rel_t *rel = nullptr;
  std::span<const int32_t> include_atts;
  /* A tree's (../BTREEINDEX.md): its entries are its own pages'. */
  bool tree = false;
  sm2::tree::tree_t pages;
  /* A tree's build in a bulk load, from idx_load_build to idx_load_root. */
  std::unique_ptr<sm2::tree::build_t> build;
  /* key_decodable(key), found at its first ask: -1 not yet (idx_decodable) */
  mutable int8_t decodable = -1;
};

struct sm2::index_scan_t {
  sm1::scan_t *scan = nullptr;
  /* A tree's cursor, in place of sm1's (../BTREEINDEX.md §5). */
  std::unique_ptr<sm2::tree::cursor_t> cursor;
  const index_t *index = nullptr;
  std::string_view key; /* the last entry's sm1 key, sm1's to keep */
  bool cut = false;     /* the last entry's cut flag */
  uint64_t id = 0;      /* the last entry's row's _id, 0 if keyed */
  std::string row;      /* its row's key, where made here */
  std::string value;    /* a primary key's entry value, made here */
  /* idx_scan_key's, a variable-width one made a varlena in place */
  std::vector<std::optional<std::string>> datums;
};

namespace {

/*
 * The least string above every string that starts with p, or
 * std::nullopt if there is none, p being all 0xFF: drop the
 * trailing 0xFF bytes and add one to the last byte left.
 */
std::optional<std::string> successor(std::string p) {
  while (!p.empty() && static_cast<uint8_t>(p.back()) == 0xFF) {
    p.pop_back();
  }
  if (p.empty()) {
    return std::nullopt;
  }
  p.back() = static_cast<char>(static_cast<uint8_t>(p.back()) + 1);
  return p;
}

/* The index's relid, big-endian, which starts all its keys. */
std::string relid_of(const sm2::index_t *ix) {
  std::string p;
  sm2::put_be(ix->relid, p);
  return p;
}

/* The most bytes a row key an entry of ix ends with may take. */
size_t row_cap(const sm2::index_t *ix) {
  return ix->keyed ? sm2::max_pk_len : sm2::id_len;
}

// A tree's entry keeps room for a key column past the longest row key
// a keyed table's entry ends with.
static_assert(sm2::tree::entry_max(sm2::tree::page_limit) >
                  sm2::tree::entry_size(0, 8) + sm2::relid_len +
                      sm2::max_pk_len + 256,
              "a tree's page limit too small for a keyed table's entries");

/*
 * The most bytes ix's key columns take before they are cut, of an
 * entry whose row key takes suffix bytes: what sm1 takes in a key,
 * or a tree's entry, a third of a page with a page id for its value
 * (../BTREEINDEX.md §4).
 */
size_t budget(const sm2::index_t *ix, size_t suffix) {
  if (!ix->tree) {
    return sm2::key_budget(suffix);
  }
  return sm2::tree::entry_max(ix->pages.limit) - sm2::tree::entry_size(0, 8) -
         sm2::relid_len - suffix;
}

/*
 * The sm1 key of the entry for key and the row at row: the relid,
 * the encoded key columns, and the row's key where the entry
 * carries it; cut is set if the columns were cut to fit. A unique
 * index's key is never cut: too long, it fails with
 * SM_ERR_KEYTOOLONG.
 */
int entry_key(const sm2::index_t *ix, std::span<const sm2::datum_t> key,
              std::span<const bool> isnull, std::string_view row,
              std::string &out, bool &cut, size_t *cols_len = nullptr) {
  if (key.size() != ix->key.size() || isnull.size() != ix->key.size()) {
    return sm1::seterr(
        sm1::SM_ERR_INVALID,
        "index: a datum and null flag for every key column, no more");
  }
  // The relid, then the columns encoded whole, in out, which a
  // caller may reuse; then decide how they fit.
  out.clear();
  sm2::put_be(ix->relid, out);
  if (sm2::encode_key(ix->key, key, isnull, out) != 0) {
    return -1;
  }
  size_t cols = out.size() - sm2::relid_len;
  bool has_null = false;
  for (bool n : isnull) {
    has_null |= n;
  }
  // A tree's entry always ends with its row's key: the row decides
  // whether it is live, and equal keys of dead rows sit beside the
  // live one (../BTREEINDEX.md §4).
  bool suffix = !ix->unique || has_null || ix->tree;
  cut = cols > budget(ix, suffix ? row_cap(ix) : 0);
  if (cut && ix->unique) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "index: a key too long for a unique index");
  }
  // Cut at the one length every cut key and cut bound share.
  if (cut) {
    // A cut key keeps the row's key, so cut keys that meet stay
    // apart.
    suffix = true;
    cols = budget(ix, row_cap(ix));
    out.resize(sm2::relid_len + cols);
  }
  if (cols_len) {
    *cols_len = cols;
  }
  if (suffix) {
    out += row;
  }
  return 0;
}

/* The bytes of a tree entry's value: its flags, and the columns' length. */
constexpr size_t tree_value_len = 3;

/*
 * A new tree entry's value (../BTREEINDEX.md §4): its flags, in doubt as
 * no vacuum has read its row (../BTREEINDEX.md §9), then the columns' length,
 * two bytes little-endian, which tells the row's key, at the entry key's end,
 * from them.
 */
std::string tree_value(bool cut, size_t cols_len) {
  std::string v;
  v.push_back(static_cast<char>(sm2::tree::entry_indoubt |
                                (cut ? sm2::tree::entry_cut : 0)));
  v.push_back(static_cast<char>(cols_len & 0xFF));
  v.push_back(static_cast<char>(cols_len >> 8));
  return v;
}

/*
 * The encoding of a bound's columns, after the relid, cut where
 * an entry's key is cut, so the cut bound orders against cut keys
 * as the whole one would against theirs; cut is then set.
 */
int bound_key(const sm2::index_t *ix, const sm2::bound_t &b, std::string &out,
              bool &cut) {
  if (b.prefix.empty() || b.prefix.size() > ix->key.size() ||
      b.isnull.size() != b.prefix.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: a bound gives 1 to " +
                                                std::to_string(ix->key.size()) +
                                                " columns");
  }
  std::string cols;
  if (sm2::encode_key(ix->key.first(b.prefix.size()), b.prefix, b.isnull,
                      cols) != 0) {
    return -1;
  }
  cut = cols.size() > budget(ix, row_cap(ix));
  if (cut) {
    cols.resize(budget(ix, row_cap(ix)));
  }
  out = relid_of(ix) + cols;
  return 0;
}

/*
 * Where a scan starts for lower bound lo, always inclusive: the
 * bound's key, or its successor to leave out the keys that equal
 * it. A cut bound is not left out, since keys past the whole bound
 * may start with the cut one.
 */
int lower_key(const sm2::index_t *ix, const sm2::bound_t &lo, std::string &out,
              bool &widened) {
  bool cut;
  if (bound_key(ix, lo, out, cut) != 0) {
    return -1;
  }
  widened |= cut;
  if (!lo.inclusive && !cut) {
    // A successor exists: the relid is below 0xFFFF.
    out = *successor(out);
  }
  return 0;
}

} // namespace

namespace sm2 {

/* Lay out an entry's value: cut, include, then the row's key. */
idx_layout_t::idx_layout_t(std::span<const attribute_t> include) {
  value = {{&type_bool, -1, false, std::nullopt}};
  value.insert(value.end(), include.begin(), include.end());
  value.push_back({&type_bytea, -1, false, std::nullopt});
  desc = tup_desc_t(value);
}

/*
 * Open an index whose entries' value is laid out by layout, which own
 * holds or the caller keeps alive: idx_open's and idx_open_layout's
 * body.
 */
static int open_index(sm1::session_t *session, uint64_t storeid, uint16_t relid,
                      std::span<const key_column_t> key, bool unique,
                      const idx_layout_t &layout,
                      std::unique_ptr<idx_layout_t> own, bool keyed,
                      index_t *&out, bool tree = false) {
  if (session == nullptr || key.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a session and at least one key column");
  }
  // 0xFFFF is TOAST's and 0xFFFE reserved (../CLUSTER.md §3); below
  // them, every key has a successor.
  if (relid >= id_relid) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: relid " + std::to_string(relid) +
                           " is TOAST's or the _id index's, not an index's");
  }
  for (const key_column_t &c : key) {
    if (c.type == nullptr) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: a key column without a type");
    }
  }
  // The cut flag and the row key always lay out: a refusal is an
  // INCLUDE column's.
  if (SIZE_MAX != layout.desc.bad) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: INCLUDE column " +
                           std::to_string(layout.desc.bad - 1) +
                           " has no type, or a typmod its type refuses");
  }
  auto ix = std::make_unique<index_t>();
  ix->session = session;
  ix->storeid = storeid;
  ix->relid = relid;
  ix->key = key;
  ix->unique = unique;
  ix->keyed = keyed;
  ix->own = std::move(own);
  ix->layout = &layout;
  if (tree) {
    ix->tree = true;
    tree::tree_open(ix->pages, session, storeid, relid);
  }
  out = ix.release();
  return 0;
}

/* Open an index, laying its entries' value out; see sm2.h. */
int idx_open(sm1::session_t *session, uint64_t storeid, uint16_t relid,
             std::span<const key_column_t> key, bool unique,
             std::span<const attribute_t> include, bool keyed, index_t *&out,
             bool tree) {
  auto own = std::make_unique<idx_layout_t>(include);
  const idx_layout_t &layout = *own;
  return open_index(session, storeid, relid, key, unique, layout,
                    std::move(own), keyed, out, tree);
}

/* Open an index laid out by a layout it borrows; see sm2.h. */
int idx_open_layout(sm1::session_t *session, uint64_t storeid, uint16_t relid,
                    std::span<const key_column_t> key, bool unique,
                    const idx_layout_t &layout, bool keyed, index_t *&out,
                    bool tree) {
  return open_index(session, storeid, relid, key, unique, layout, nullptr,
                    keyed, out, tree);
}

/* Make an index's tree, empty; see sm2.h. */
int idx_create(index_t *index) {
  return index->tree ? tree::tree_create(index->pages) : 0;
}

/* Make the tree under relid in storeid, empty; see sm2.h. */
int idx_create_tree(sm1::session_t *session, uint64_t storeid, uint16_t relid) {
  tree::tree_t t;
  tree::tree_open(t, session, storeid, relid);
  return tree::tree_create(t);
}

/* The bytes of a tree's pages; see sm2.h. */
int idx_tree_size(sm1::session_t *session, uint64_t storeid, uint16_t relid,
                  uint64_t &disk, uint64_t &mem) {
  std::string prefix;
  put_be(relid, prefix);
  return sm1::page_approxsize(session, storeid, prefix, disk, mem);
}

/* Remove a tree's pages; see sm2.h. */
int idx_drop_tree(sm1::session_t *session, uint64_t storeid, uint16_t relid) {
  return tree::tree_drop(session, storeid, relid);
}

/*
 * dead of the tree entry key, value: its row's key taken from it. A
 * cut entry is judged as any other: a scan rechecks it on its row on
 * any leaf (../BTREEINDEX.md §9).
 */
static int
judge_entry(const std::function<int(std::string_view, std::string_view)> &dead,
            std::string_view key, std::string_view value) {
  if (3 != value.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a tree entry's value");
  }
  size_t cols_len = static_cast<uint8_t>(value[1]) |
                    static_cast<size_t>(static_cast<uint8_t>(value[2])) << 8;
  if (relid_len + cols_len > key.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a tree entry's key");
  }
  return dead(key.substr(relid_len + cols_len), key);
}

/* Remove a tree's dead entries; see sm2.h. */
int idx_vacuum(
    index_t *index,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed) {
  if (!index->tree) {
    removed = 0;
    return 0;
  }
  return tree::tree_prune(
      index->pages,
      [&](std::string_view key, std::string_view value) {
        return judge_entry(dead, key, value);
      },
      removed);
}

/* Give a tree its leaf note; see sm2.h. */
void idx_set_leaf_note(index_t *index, leaf_note_fn fn, void *ctx) {
  index->pages.note = fn;
  index->pages.note_ctx = ctx;
}

/* Clean one leaf of a tree; see sm2.h. */
int idx_clean_leaf(
    index_t *index, uint64_t pageid,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed, bool &doubt) {
  return tree::tree_clean_leaf(
      index->pages, pageid,
      [&](std::string_view key, std::string_view value) {
        return judge_entry(dead, key, value);
      },
      removed, doubt);
}

/* Whether an index is a tree; see sm2.h. */
bool idx_is_tree(const index_t *index) { return index->tree; }

/*
 * Open rel's primary key as an index, its entries laid out by layout,
 * which own holds or the caller keeps alive: idx_open_primary's and
 * idx_open_primary_layout's body.
 */
static int open_primary(rel_t *rel, const idx_layout_t &layout,
                        std::unique_ptr<idx_layout_t> own,
                        std::span<const int32_t> include_atts, index_t *&out) {
  if (!rel_keyed(rel) || layout.value.size() != include_atts.size() + 2) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a primary key of a keyed rel, and an "
                       "attribute for every INCLUDE column");
  }
  index_t *ix = nullptr;
  if (open_index(rel->session, rel->storeid, rel->relid, rel->key, true, layout,
                 std::move(own), false, ix) != 0) {
    return -1;
  }
  ix->rel = rel;
  ix->include_atts = include_atts;
  out = ix;
  return 0;
}

/* Open a rel's primary key as an index; see sm2.h. */
int idx_open_primary(rel_t *rel, std::span<const attribute_t> include,
                     std::span<const int32_t> include_atts, index_t *&out) {
  auto own = std::make_unique<idx_layout_t>(include);
  const idx_layout_t &layout = *own;
  return open_primary(rel, layout, std::move(own), include_atts, out);
}

/* Open a rel's primary key as an index laid out by a layout it borrows. */
int idx_open_primary_layout(rel_t *rel, const idx_layout_t &layout,
                            std::span<const int32_t> include_atts,
                            index_t *&out) {
  return open_primary(rel, layout, nullptr, include_atts, out);
}

/* Close an index; nullptr is a no-op. */
void idx_close(index_t *index) { delete index; }

/* The schema of an entry's value: cut, INCLUDE, a row key. */
std::span<const attribute_t> idx_value_schema(const index_t *index) {
  return index->layout->value;
}

/* Fail on a write to a primary key, whose entries are its rows. */
static int need_entries(const index_t *index) {
  return index->rel == nullptr
             ? 0
             : sm1::seterr(sm1::SM_ERR_INVALID,
                           "index: a primary key has no entries to write");
}

/*
 * The entry of the row with key row, as sm1 holds it: its key in k
 * and its value in value, which is whether the key was cut, the
 * INCLUDE columns, and the row's key.
 */
static int entry_of(index_t *index, std::span<const datum_t> key,
                    std::span<const bool> key_isnull, std::string_view row,
                    std::span<const datum_t> include,
                    std::span<const bool> include_isnull, std::string &k,
                    std::string &value) {
  bool cut;
  size_t cols_len = 0;
  if (need_entries(index) != 0 ||
      entry_key(index, key, key_isnull, row, k, cut, &cols_len) != 0) {
    return -1;
  }
  if (index->tree) {
    // A tree's entry holds no INCLUDE column: they are read from the
    // row, which a scan reads anyway (../BTREEINDEX.md §4).
    value = tree_value(cut, cols_len);
    return 0;
  }
  if (include.size() + 2 != index->layout->value.size() ||
      include_isnull.size() != include.size()) {
    return sm1::seterr(
        sm1::SM_ERR_INVALID,
        "index: a datum and null flag for every INCLUDE column, no more");
  }
  // The value: cut, the INCLUDE columns, then the row's key.
  std::vector<datum_t> datums = {datum_from_bool(cut)};
  datums.insert(datums.end(), include.begin(), include.end());
  std::string row_varlena = make_varlena(row);
  datums.push_back(datum_from_ptr(row_varlena.data()));
  auto nulls = std::make_unique<bool[]>(datums.size());
  for (size_t i = 0; i < include.size(); i++) {
    nulls[i + 1] = include_isnull[i];
  }
  return tup_encode(index->layout->desc, datums,
                    std::span<const bool>(nulls.get(), datums.size()), value);
}

/* Add the entry for the row with key row; see sm2.h. */
int idx_insert(index_t *index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, std::string_view row,
               std::span<const datum_t> include,
               std::span<const bool> include_isnull) {
  std::string k, value;
  if (entry_of(index, key, key_isnull, row, include, include_isnull, k,
               value) != 0) {
    return -1;
  }
  if (index->tree) {
    if (index->unique) {
      // A unique tree checks its key against its rows (../BTREEINDEX.md
      // ../BTREEINDEX.md §7), which only idx_insert_unique is given the means
      // to.
      bool has_null = false;
      for (bool n : key_isnull) {
        has_null |= n;
      }
      if (!has_null) {
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "index: a unique tree's key is inserted by "
                           "idx_insert_unique");
      }
    }
    // An entry already there, the same key and row, is no change; a
    // serializable reader of its range learns of the write either way.
    if (tree::tree_insert(index->pages, k, value) < 0) {
      return -1;
    }
    return sm1::ssi_write_key(index->session, index->storeid, k);
  }
  return sm1::insert(index->session, index->storeid, k, value);
}

/* The columns' length a tree entry's value holds. */
static size_t cols_len_of(std::string_view value) {
  return static_cast<uint8_t>(value[1]) |
         static_cast<size_t>(static_cast<uint8_t>(value[2])) << 8;
}

/* Add a unique tree's entry unless its key conflicts; see sm2.h. */
int idx_insert_unique(
    index_t *index, std::span<const datum_t> key,
    std::span<const bool> key_isnull, std::string_view row,
    std::span<const datum_t> include, std::span<const bool> include_isnull,
    const std::function<int(std::string_view, std::string_view)> &check) {
  if (!index->tree || !index->unique) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: idx_insert_unique takes a unique tree");
  }
  bool has_null = false;
  for (bool n : key_isnull) {
    has_null |= n;
  }
  std::string k, value;
  if (entry_of(index, key, key_isnull, row, include, include_isnull, k,
               value) != 0) {
    return -1;
  }
  int rc;
  if (has_null) {
    rc = tree::tree_insert(index->pages, k, value);
  } else {
    // The key's prefix, the relid and its columns, uncut in a unique
    // index; each other entry of it is judged by its row.
    std::string_view prefix(k.data(), relid_len + cols_len_of(value));
    rc = tree::tree_insert_unique(
        index->pages, prefix, k, value,
        [&](std::string_view ek, std::string_view ev) {
          if (3 != ev.size() || relid_len + cols_len_of(ev) > ek.size()) {
            return sm1::seterr(sm1::SM_ERR_CORRUPT,
                               "index: a tree entry's value");
          }
          return check(ek.substr(relid_len + cols_len_of(ev)), ek);
        });
  }
  if (rc < 0 || rc == 2) {
    return rc;
  }
  return sm1::ssi_write_key(index->session, index->storeid, k);
}

/*
 * The sm1 key of a unique index's entry for key, in k: 0, or 1 if
 * the key has a NULL column, which makes it no other row's -- its
 * entry carries its row's key, and two such never meet -- or -1.
 * A primary key's is its row's key.
 */
static int unique_key(index_t *index, std::span<const datum_t> key,
                      std::span<const bool> key_isnull, std::string &k) {
  if (!index->unique) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: only a unique index has one row a key");
  }
  for (bool n : key_isnull) {
    if (n) {
      return 1;
    }
  }
  bool cut;
  return entry_key(index, key, key_isnull, {}, k, cut);
}

/*
 * The key of the row an entry names, and its _id, a keyless row's
 * from its key or 0 for a keyed row, from the entry's sm1 key k and
 * its value: a primary key's entry is the row, at its key.
 */
static int entry_row(const index_t *index, std::string_view k,
                     std::string_view value, uint64_t &id, std::string &row) {
  id = 0;
  if (index->rel != nullptr) {
    row.assign(k.substr(relid_len));
    return 0;
  }
  datum_t d;
  bool isnull;
  if (tup_decode(index->layout->desc, value, index->layout->value.size() - 1, d,
                 isnull) != 0) {
    return -1;
  }
  std::string_view v;
  if (datum_bytes(&type_bytea, -1, d, v) != 0) {
    return -1;
  }
  row.assign(varlena_payload(v));
  std::string_view r = row;
  if (!index->keyed && !get_be(r, id)) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a row key without its _id");
  }
  return 0;
}

/* Whether index is a primary key whose rows are a chained store's. */
static bool chained_rows(const index_t *index) {
  return index->rel != nullptr && index->rel->chained;
}

/* The row that has a unique key, as the snapshot shows; see sm2.h. */
int idx_find(index_t *index, std::span<const datum_t> key,
             std::span<const bool> key_isnull, uint64_t &id, std::string *row) {
  if (index->tree) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a tree's entries name rows only its table "
                       "can check");
  }
  std::string k;
  int rc = unique_key(index, key, key_isnull, k);
  if (rc != 0) {
    return rc;
  }
  std::string_view value;
  // A primary key's entries are its rows: a chained store's keys.
  rc = chained_rows(index)
           ? sm1::chain_read(index->session, index->storeid, k, value)
           : sm1::read(index->session, index->storeid, k, value);
  if (rc != 0) {
    return rc;
  }
  std::string r;
  if (entry_row(index, k, value, id, r) != 0) {
    return -1;
  }
  if (row) {
    *row = std::move(r);
  }
  return 0;
}

/* The row that has a unique key now, under a lock; see sm2.h. */
int idx_find_latest(index_t *index, std::span<const datum_t> key,
                    std::span<const bool> key_isnull, uint64_t &id,
                    std::string *row) {
  if (index->tree) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a tree's entries name rows only its table "
                       "can check");
  }
  std::string k;
  int rc = unique_key(index, key, key_isnull, k);
  if (rc != 0) {
    return rc;
  }
  // sm1 reads a newest version only under a lock that keeps it
  // the newest. Shared is enough: nothing is to be written.
  const bool chained = chained_rows(index);
  // The index's relid is the lock's kind, as a row's is its rel's.
  if ((chained
           ? sm1::chain_lock_row(index->session, index->storeid, k,
                                 sm1::SM_LOCKMODE_SH, false, index->relid)
           : sm1::lock_row(index->session, index->storeid, k,
                           sm1::SM_LOCKMODE_SH, false, index->relid)) != 0) {
    return -1;
  }
  std::string_view value;
  rc = chained
           ? sm1::chain_read_latest(index->session, index->storeid, k, value)
           : sm1::read_latest(index->session, index->storeid, k, value);
  if (rc != 0) {
    return rc;
  }
  std::string r;
  if (entry_row(index, k, value, id, r) != 0) {
    return -1;
  }
  if (row) {
    *row = std::move(r);
  }
  return 0;
}

/* Put the entry for the row with key row into a load; see sm2.h. */
int idx_load(index_t *index, sm1::loader_t *loader, size_t stream,
             std::span<const datum_t> key, std::span<const bool> key_isnull,
             std::string_view row, std::span<const datum_t> include,
             std::span<const bool> include_isnull) {
  std::string k, value;
  if (entry_of(index, key, key_isnull, row, include, include_isnull, k,
               value) != 0) {
    return -1;
  }
  if (!index->tree) {
    return sm1::load_put(loader, stream, k, value);
  }
  // A committed reader can retain an SIREAD after its table lock ends.
  // Report the full entry key before a unique sort removes its row key.
  if (0 != sm1::ssi_write_key(index->session, index->storeid, k)) {
    return -1;
  }
  // A unique tree's entries are sorted by what its check compares:
  // the key columns, and the row's key only after a NULL, which
  // equals no other. The row's key rides after the value, and the
  // build puts it back (../BTREELOAD_PLAN.md §3.4).
  bool has_null =
      std::find(key_isnull.begin(), key_isnull.end(), true) != key_isnull.end();
  if (index->unique && !has_null) {
    size_t cols = relid_len + cols_len_of(value);
    value.append(k, cols);
    k.resize(cols);
  }
  return sm1::load_put(loader, stream, k, value);
}

/* Make a tree empty and declare its streams for a load; see sm2.h. */
int idx_load_tree(index_t *index, sm1::loader_t *loader, size_t &entries,
                  size_t &pages) {
  if (!index->tree) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: not a tree");
  }
  // The table holds no version, so no entry is live: start empty,
  // which drops the dead entries of aborted inserts too.
  if (0 != tree::tree_create(index->pages) ||
      0 != sm1::load_drain_stream(loader, entries)) {
    return -1;
  }
  return sm1::load_page_stream(loader, pages);
}

/* Build a tree from a load's sorted entries; see sm2.h. */
int idx_load_build(index_t *index, sm1::loader_t *loader, size_t entries,
                   size_t pages) {
  index->build = std::make_unique<tree::build_t>();
  tree::build_open(index->pages, *index->build,
                   [loader, pages](std::string_view key, std::string_view b) {
                     return sm1::load_put(loader, pages, key, b);
                   });
  std::string k;
  std::string_view key, value;
  int rc;
  while (0 == (rc = sm1::load_drain(loader, entries, key, value))) {
    // A unique tree's row key, after the value's three bytes.
    std::string_view entry = key;
    if (value.size() > tree_value_len) {
      k.assign(key);
      k.append(value.substr(tree_value_len));
      entry = k;
      value = value.substr(0, tree_value_len);
    }
    if (0 != tree::build_add(*index->build, entry, value)) {
      return -1;
    }
  }
  return rc < 0 ? -1 : tree::build_end(*index->build);
}

/* Write a built tree's page 0; see sm2.h. */
int idx_load_root(index_t *index) {
  if (!index->build) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: no tree built");
  }
  std::unique_ptr<tree::build_t> build = std::move(index->build);
  return tree::build_root(*build);
}

/* Remove the entry for key and the row with key row. */
int idx_delete(index_t *index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, std::string_view row) {
  std::string k;
  bool cut;
  if (need_entries(index) != 0 ||
      entry_key(index, key, key_isnull, row, k, cut) != 0) {
    return -1;
  }
  // A tree's entry stays: its row, without the key now, makes it
  // dead, and vacuum removes it (../BTREEINDEX.md §6). It is in doubt, and
  // its leaf no longer all visible, the row written already (../BTREEINDEX.md
  // §9); and the key is a write for SSI, as an insert's is, since a
  // serializable reader whose index-only scan read no row holds only its range.
  if (index->tree) {
    if (0 != tree::tree_mark_in_doubt(index->pages, k)) {
      return -1;
    }
    return sm1::ssi_write_key(index->session, index->storeid, k);
  }
  int rc = sm1::erase(index->session, index->storeid, k);
  if (rc != 0 && sm1::errnum() == sm1::SM_ERR_UPDATED) {
    // At read committed the entry was committed after this
    // transaction began. An entry has no filter to re-check --
    // the caller decided on the row -- so read the newest
    // version, which is what lets sm1 take the erase again.
    std::string_view latest;
    if (sm1::read_latest(index->session, index->storeid, k, latest) < 0) {
      return -1;
    }
    rc = sm1::erase(index->session, index->storeid, k);
  }
  return rc;
}

/*
 * The sm1 keys a scan of index between lo and hi starts at, always
 * inclusive, and ends at, exclusive, none for no end; wide is set if
 * a bound was cut.
 */
static int scan_range(const index_t *index, const bound_t *lo,
                      const bound_t *hi, std::string &start,
                      std::optional<std::string> &end, bool &wide) {
  // The start: the bound, or the first of the index's keys.
  if (lo) {
    if (lower_key(index, *lo, start, wide) != 0) {
      return -1;
    }
  } else {
    start = relid_of(index);
  }
  // The end: the bound or its successor, or past the index.
  if (hi) {
    std::string p;
    bool cut;
    if (bound_key(index, *hi, p, cut) != 0) {
      return -1;
    }
    wide |= cut;
    // Keys that equal an inclusive or cut bound stay in.
    end = hi->inclusive || cut ? successor(p) : std::optional(p);
  } else {
    end = successor(relid_of(index));
  }
  return 0;
}

/* Open a cursor between lo and hi; see sm2.h. */
int idx_scan(index_t *index, const bound_t *lo, const bound_t *hi,
             index_scan_t *&out, bool *widened) {
  bool wide = false;
  std::string start;
  std::optional<std::string> end;
  if (scan_range(index, lo, hi, start, end, wide) != 0) {
    return -1;
  }
  if (index->tree) {
    // Under serializable, the range is an SIREAD of its own: no scan
    // of sm1's reads it (../BTREEINDEX.md §13, SSI).
    if (sm1::ssi_read_range(index->session, index->storeid, start.data(),
                            start.size(), 1, end ? end->data() : nullptr,
                            end ? end->size() : 0, 0) != 0) {
      return -1;
    }
    out = new index_scan_t;
    out->index = index;
    out->cursor = std::make_unique<tree::cursor_t>();
    tree::cursor_open(index->pages, *out->cursor, &start, true,
                      end ? &*end : nullptr, false);
    if (widened) {
      *widened = wide;
    }
    return 0;
  }
  sm1::scan_t *c;
  const char *e = end ? end->data() : nullptr;
  size_t elen = end ? end->size() : 0;
  // A primary key's entries are its rows: a chained store's keys.
  if ((chained_rows(index)
           ? sm1::chain_scan(index->session, index->storeid, start.data(),
                             start.size(), 1, e, elen, 0, c)
           : sm1::scan(index->session, index->storeid, start.data(),
                       start.size(), 1, e, elen, 0, c)) != 0) {
    return -1;
  }
  out = new index_scan_t;
  out->scan = c;
  out->index = index;
  if (widened) {
    *widened = wide;
  }
  return 0;
}

/* Open a cursor over a primary key's rows between lo and hi; see sm2.h. */
int idx_scan_rows(index_t *index, const bound_t *lo, const bound_t *hi,
                  rel_scan_t *&out, bool *widened) {
  if (index->rel == nullptr) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: only a primary key's entries are rows");
  }
  bool wide = false;
  std::string start;
  std::optional<std::string> end;
  if (scan_range(index, lo, hi, start, end, wide) != 0) {
    return -1;
  }
  sm1::scan_t *c;
  const char *e = end ? end->data() : nullptr;
  size_t elen = end ? end->size() : 0;
  // A chained store's rows are its chained keys.
  if ((chained_rows(index)
           ? sm1::chain_scan(index->session, index->storeid, start.data(),
                             start.size(), 1, e, elen, 0, c)
           : sm1::scan(index->session, index->storeid, start.data(),
                       start.size(), 1, e, elen, 0, c)) != 0) {
    return -1;
  }
  out =
      new rel_scan_t{c, index->session, index->storeid, index->relid, true, {}};
  if (widened) {
    *widened = wide;
  }
  return 0;
}

/*
 * The entry value of a primary key's entry, the row whose tuple is
 * row: not cut, the row's INCLUDE columns, and its key, into the
 * cursor's own value.
 */
static int primary_value(index_scan_t *scan, std::string_view row) {
  const index_t *ix = scan->index;
  scan->id = 0;
  size_t n = ix->include_atts.size();
  std::vector<datum_t> datums = {datum_from_bool(false)};
  auto nulls = std::make_unique<bool[]>(n + 2);
  for (size_t i = 0; i < n; i++) {
    datum_t d;
    if (tup_decode(*ix->rel->desc, row, ix->include_atts[i], d, nulls[i + 1]) !=
        0) {
      return -1;
    }
    datums.push_back(d);
  }
  std::string row_varlena = make_varlena(scan->key.substr(relid_len));
  datums.push_back(datum_from_ptr(row_varlena.data()));
  return tup_encode(ix->layout->desc, datums,
                    std::span<const bool>(nulls.get(), n + 2), scan->value);
}

/* The next entry of a cursor: 0, 1 at the end, or -1. */
int idx_scannxt(index_scan_t *scan, uint64_t &id, bool &cut,
                std::string_view &value) {
  if (scan->cursor) {
    std::string_view row;
    int rc = idx_scan_entry(scan, id, cut, row);
    if (rc != 0) {
      return rc;
    }
    // The value as an entry of sm1's holds it: cut, the INCLUDE
    // columns, none in a tree, and the row's key.
    const index_t *ix = scan->index;
    size_t n = ix->layout->value.size();
    std::vector<datum_t> datums(n, 0);
    auto nulls = std::make_unique<bool[]>(n);
    datums[0] = datum_from_bool(cut);
    for (size_t i = 1; i + 1 < n; i++) {
      nulls[i] = true;
    }
    std::string row_varlena = make_varlena(row);
    datums[n - 1] = datum_from_ptr(row_varlena.data());
    if (tup_encode(ix->layout->desc, datums,
                   std::span<const bool>(nulls.get(), n), scan->value) != 0) {
      return -1;
    }
    value = scan->value;
    return 0;
  }
  std::string_view key;
  int rc = sm1::scannxt(scan->scan, key, value);
  if (rc != 0) {
    return rc;
  }
  scan->key = key;
  scan->cut = false;
  if (scan->index->rel != nullptr) {
    // A row: its entry is made from it.
    if (primary_value(scan, value) != 0) {
      return -1;
    }
    id = scan->id;
    cut = false;
    value = scan->value;
    return 0;
  }
  // The value opens with the cut flag, and ends with the row's key.
  datum_t c;
  bool c_null;
  if (tup_decode(scan->index->layout->desc, value, 0, c, c_null) != 0) {
    return -1;
  }
  if (c_null) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT,
                       "index: an entry without its cut flag");
  }
  scan->value.assign(value);
  if (entry_row(scan->index, key, scan->value, scan->id, scan->row) != 0) {
    return -1;
  }
  id = scan->id;
  cut = datum_to_bool(c);
  scan->cut = cut;
  return 0;
}

/* Aim a tree's cursor at new bounds; see sm2.h. */
int idx_scan_rebound(index_scan_t *scan, const bound_t *lo, const bound_t *hi,
                     bool *widened) {
  if (!scan->cursor) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: not a tree's cursor");
  }
  const index_t *index = scan->index;
  bool wide = false;
  std::string start;
  std::optional<std::string> end;
  if (scan_range(index, lo, hi, start, end, wide) != 0 ||
      sm1::ssi_read_range(index->session, index->storeid, start.data(),
                          start.size(), 1, end ? end->data() : nullptr,
                          end ? end->size() : 0, 0) != 0) {
    return -1;
  }
  tree::cursor_rebound(*scan->cursor, &start, true, end ? &*end : nullptr,
                       false);
  if (widened) {
    *widened = wide;
  }
  return 0;
}

/* A tree's next entry, its key and its row's; see sm2.h. */
int idx_scan_entry(index_scan_t *scan, uint64_t &id, bool &cut,
                   std::string_view &row) {
  if (!scan->cursor) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: not a tree's cursor");
  }
  std::string_view key, value;
  int rc = tree::cursor_next(*scan->cursor, key, value);
  if (rc != 0) {
    return rc;
  }
  if (3 != value.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a tree entry's value");
  }
  size_t cols_len = static_cast<uint8_t>(value[1]) |
                    static_cast<size_t>(static_cast<uint8_t>(value[2])) << 8;
  if (relid_len + cols_len > key.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a tree entry's key");
  }
  const index_t *ix = scan->index;
  scan->key = key;
  scan->cut = 0 != (value[0] & tree::entry_cut);
  row = key.substr(relid_len + cols_len);
  // An index of a table gives its row's key from the entry's key
  // (idx_scan_row); only one with no table keeps a copy.
  if (nullptr == ix->rel) {
    scan->row.assign(row);
  }
  scan->id = 0;
  std::string_view r = row;
  if (!ix->keyed && !get_be(r, scan->id)) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: a row key without its _id");
  }
  id = scan->id;
  cut = scan->cut;
  return 0;
}

/* The key of a cursor's last entry; see sm2.h. */
std::string_view idx_scan_entry_key(const index_scan_t *scan) {
  return scan->key;
}

/* The entries a tree's cursor holds past its last; see sm2.h. */
size_t idx_scan_held(const index_scan_t *scan) {
  return scan->cursor ? tree::cursor_held(*scan->cursor) : 0;
}

/* The key a row's entry has in index; see sm2.h. */
int idx_entry_key(const index_t *index, std::span<const datum_t> key,
                  std::span<const bool> key_isnull, std::string_view row,
                  std::string &out) {
  bool cut;
  return entry_key(index, key, key_isnull, row, out, cut);
}

/* The key of the row of a cursor's last entry; see sm2.h. */
std::string_view idx_scan_row(const index_scan_t *scan) {
  const index_t *ix = scan->index;
  if (ix->rel != nullptr) {
    return scan->key.substr(relid_len);
  }
  return scan->row;
}

/*
 * A datum_t over a key column's decoded bytes: a by-value one
 * sign-extended to 8 bytes as tuples give them (sm2.h), a varlena
 * made in place for a variable-width one, decoded with room for
 * its header, else a pointer to the bytes.
 */
static datum_t key_datum(const type_t *type, std::string &bytes) {
  if (type->byval) {
    if (bytes.size() == sizeof(int32_t)) {
      int32_t v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return datum_from_int32(v);
    }
    if (bytes.size() == sizeof(int16_t)) {
      int16_t v;
      std::memcpy(&v, bytes.data(), sizeof(v));
      return datum_from_int16(v);
    }
    datum_t d = 0;
    std::memcpy(&d, bytes.data(), bytes.size());
    return d;
  }
  if (type->len < 0) {
    return datum_from_ptr(varlena_in_room(bytes));
  }
  return datum_from_ptr(bytes.data());
}

/* The key columns of the cursor's last entry, if the key gives them. */
int idx_scan_key(index_scan_t *scan, std::span<datum_t> values,
                 std::span<bool> isnull) {
  return idx_entry_cols(scan->index, scan->key, scan->cut, scan->datums, values,
                        isnull);
}

/* Whether a tree's cursor's last entry's leaf is all visible; see sm2.h. */
bool idx_scan_visible(const index_scan_t *scan) {
  return scan->cursor && scan->cursor->visible;
}

/* Whether an index's key columns decode from its keys; see sm2.h. */
bool idx_decodable(const index_t *index) {
  // The key columns never change, so the answer is found once.
  if (index->decodable < 0) {
    index->decodable = key_decodable(index->key) ? 1 : 0;
  }
  return 1 == index->decodable;
}

/* An entry's key columns, decoded from its key; see sm2.h. */
int idx_entry_cols(const index_t *ix, std::string_view key, bool cut,
                   std::vector<std::optional<std::string>> &datums,
                   std::span<datum_t> values, std::span<bool> isnull) {
  if (values.size() != ix->key.size() || isnull.size() != ix->key.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a datum and null flag for every key column");
  }
  if (cut || !idx_decodable(ix)) {
    return 1;
  }
  // The relid, the columns, then the row's key if the entry has it.
  if (key.size() < sizeof(ix->relid)) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: an entry key too short");
  }
  key.remove_prefix(sizeof(ix->relid));
  if (0 != decode_key_front(ix->key, key, datums, true)) {
    return -1;
  }
  bool has_null = false;
  for (size_t i = 0; i < ix->key.size(); i++) {
    isnull[i] = !datums[i].has_value();
    has_null |= isnull[i];
    values[i] = isnull[i] ? 0 : key_datum(ix->key[i].type, *datums[i]);
  }
  bool suffix = !ix->unique || has_null || ix->tree;
  if (suffix ? key.empty() || (!ix->keyed && key.size() != id_len)
             : !key.empty()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT,
                       "index: an entry key runs on past its columns");
  }
  return 0;
}

/* Move a cursor forward to lower bound lo. */
int idx_scan_seek(index_scan_t *scan, const bound_t &lo) {
  std::string start;
  bool wide = false;
  if (lower_key(scan->index, lo, start, wide) != 0) {
    return -1;
  }
  if (scan->cursor) {
    tree::cursor_seek(*scan->cursor, start, true);
    return 0;
  }
  return sm1::scan_seek(scan->scan, start, 1);
}

/* Close a cursor; nullptr is a no-op. */
void idx_scanend(index_scan_t *scan) {
  if (scan == nullptr) {
    return;
  }
  // Release sm1's cursor and its snapshot; a tree's holds neither.
  if (scan->scan) {
    sm1::scanend(scan->scan);
  }
  delete scan;
}

} // namespace sm2
