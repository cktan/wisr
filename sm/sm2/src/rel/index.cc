/*
 * index.cc -- the index rel (SM2_DESIGN.md §5): a table's index
 * entries in its sm1 store, under the index's relid, keyed by
 * their encoded key columns, holding the rowno, the cut flag and
 * the INCLUDE columns as a tuple.
 *
 * A scan's bounds rest on the key encoding being prefix-free
 * column by column (TYPE.md §2): the encoding P of an entry's
 * first j columns is a prefix of exactly the keys whose first j
 * columns hold those values, and those keys are contiguous. So a
 * bound is P or its successor, the least string above every key
 * that starts with P.
 */

#include "native.h"
#include "sm2.h"

#include <cstring>
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
  std::vector<attribute_t> value; /* rowno, cut, then INCLUDE */
  tup_desc_t value_desc;          /* over value */
};

struct sm2::index_scan_t {
  sm1::scan_t *scan = nullptr;
  const index_t *index = nullptr;
  std::string_view key; /* the last entry's sm1 key, sm1's to keep */
  bool cut = false;     /* the last entry's cut flag */
  std::vector<std::optional<std::string>> datums; /* idx_scan_key's */
  std::vector<std::string> varlenas;              /* idx_scan_key's */
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

/*
 * The sm1 key of the entry for key and rowno: the relid, the
 * encoded key columns, and the rowno where the entry carries one;
 * cut is set if the columns were cut to fit. A unique index's key
 * is never cut: too long, it fails with SM_ERR_KEYTOOLONG.
 */
int entry_key(const sm2::index_t *ix, std::span<const sm2::datum_t> key,
              std::span<const bool> isnull, uint64_t rowno, std::string &out,
              bool &cut) {
  if (key.size() != ix->key.size() || isnull.size() != ix->key.size()) {
    return sm1::seterr(
        sm1::SM_ERR_INVALID,
        "index: a datum and null flag for every key column, no more");
  }
  // Encode the columns whole, then decide how they fit.
  std::string cols;
  if (sm2::encode_key(ix->key, key, isnull, cols) != 0) {
    return -1;
  }
  bool has_null = false;
  for (bool n : isnull) {
    has_null |= n;
  }
  bool suffix = !ix->unique || has_null;
  cut = cols.size() > sm2::max_key_len(suffix);
  if (cut && ix->unique) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "index: a key too long for a unique index");
  }
  // Cut at the one length every cut key and cut bound share.
  if (cut) {
    // A cut key keeps the rowno, so cut keys that meet stay apart.
    suffix = true;
    cols.resize(sm2::max_key_len(true));
  }
  out = relid_of(ix) + cols;
  if (suffix) {
    sm2::put_be(rowno, out);
  }
  return 0;
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
  cut = cols.size() > sm2::max_key_len(true);
  if (cut) {
    cols.resize(sm2::max_key_len(true));
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

/* Open an index; see sm2.h. */
int idx_open(sm1::session_t *session, uint64_t storeid, uint16_t relid,
             std::span<const key_column_t> key, bool unique,
             std::span<const attribute_t> include, index_t *&out) {
  if (session == nullptr || key.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a session and at least one key column");
  }
  // 0xFFFF is TOAST's (SM2_DESIGN.md §5); below it, every key
  // has a successor.
  if (relid == 0xFFFF) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: relid 0xFFFF is TOAST's, not an index's");
  }
  for (const key_column_t &c : key) {
    if (c.type == nullptr) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: a key column without a type");
    }
  }
  std::vector<attribute_t> value = {{&type_int8, -1, false, std::nullopt},
                                    {&type_bool, -1, false, std::nullopt}};
  value.insert(value.end(), include.begin(), include.end());
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i].type == nullptr ||
        datum_width(value[i].type, value[i].typmod) == 0) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: INCLUDE column " + std::to_string(i - 2) +
                             " has no type, or a typmod its type refuses");
    }
  }
  out = new index_t{session, storeid, relid, key, unique, std::move(value), {}};
  // Lay the value out once, for every entry; it borrows out->value.
  out->value_desc = tup_desc_t(out->value);
  return 0;
}

/* Close an index; nullptr is a no-op. */
void idx_close(index_t *index) { delete index; }

/* The schema of an entry's value: rowno, cut, then INCLUDE. */
std::span<const attribute_t> idx_value_schema(const index_t *index) {
  return index->value;
}

/*
 * The entry of the row at rowno, as sm1 holds it: its key in k and
 * its value in value, which is the rowno, whether the key was cut,
 * and the INCLUDE columns.
 */
static int entry_of(index_t *index, std::span<const datum_t> key,
                    std::span<const bool> key_isnull, uint64_t rowno,
                    std::span<const datum_t> include,
                    std::span<const bool> include_isnull, std::string &k,
                    std::string &value) {
  bool cut;
  if (entry_key(index, key, key_isnull, rowno, k, cut) != 0) {
    return -1;
  }
  if (include.size() + 2 != index->value.size() ||
      include_isnull.size() != include.size()) {
    return sm1::seterr(
        sm1::SM_ERR_INVALID,
        "index: a datum and null flag for every INCLUDE column, no more");
  }
  // The value: rowno, cut, then the INCLUDE columns.
  std::vector<datum_t> datums = {datum_from_int64(static_cast<int64_t>(rowno)),
                                 datum_from_bool(cut)};
  datums.insert(datums.end(), include.begin(), include.end());
  auto nulls = std::make_unique<bool[]>(datums.size());
  for (size_t i = 0; i < include.size(); i++) {
    nulls[i + 2] = include_isnull[i];
  }
  return tup_encode(index->value_desc, datums,
                    std::span<const bool>(nulls.get(), datums.size()), value);
}

/* Add the entry for the row at rowno; see sm2.h. */
int idx_insert(index_t *index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, uint64_t rowno,
               std::span<const datum_t> include,
               std::span<const bool> include_isnull) {
  std::string k, value;
  if (entry_of(index, key, key_isnull, rowno, include, include_isnull, k,
               value) != 0) {
    return -1;
  }
  return sm1::insert(index->session, index->storeid, k, value);
}

/* Put the entry for the row at rowno into a load; see sm2.h. */
int idx_load(index_t *index, sm1::loader_t *loader, size_t stream,
             std::span<const datum_t> key, std::span<const bool> key_isnull,
             uint64_t rowno, std::span<const datum_t> include,
             std::span<const bool> include_isnull) {
  std::string k, value;
  if (entry_of(index, key, key_isnull, rowno, include, include_isnull, k,
               value) != 0) {
    return -1;
  }
  return sm1::load_put(loader, stream, k, value);
}

/* Remove the entry for key and rowno. */
int idx_delete(index_t *index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, uint64_t rowno) {
  std::string k;
  bool cut;
  if (entry_key(index, key, key_isnull, rowno, k, cut) != 0) {
    return -1;
  }
  int rc = sm1::erase(index->session, index->storeid, k);
  if (rc != 0 && sm1::errnum() == sm1::SM_ERR_UPDATED) {
    // At read committed the entry was committed after this
    // transaction began. An entry has no filter to re-check --
    // the caller decided on the heap row -- so read the newest
    // version, which is what lets sm1 take the erase again.
    std::string_view latest;
    if (sm1::read_latest(index->session, index->storeid, k, latest) < 0) {
      return -1;
    }
    rc = sm1::erase(index->session, index->storeid, k);
  }
  return rc;
}

/* Open a cursor between lo and hi; see sm2.h. */
int idx_scan(index_t *index, const bound_t *lo, const bound_t *hi,
             index_scan_t *&out, bool *widened) {
  bool wide = false;
  // The start: the bound, or the first of the index's keys.
  std::string start;
  if (lo) {
    if (lower_key(index, *lo, start, wide) != 0) {
      return -1;
    }
  } else {
    start = relid_of(index);
  }
  // The end: the bound or its successor, or past the index.
  std::optional<std::string> end;
  bool end_inclusive = false;
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
  sm1::scan_t *c;
  if (sm1::scan(index->session, index->storeid, start.data(), start.size(), 1,
                end ? end->data() : nullptr, end ? end->size() : 0,
                end_inclusive, c) != 0) {
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

/* The next entry of a cursor: 0, 1 at the end, or -1. */
int idx_scannxt(index_scan_t *scan, uint64_t &rowno, bool &cut,
                std::string_view &value) {
  std::string_view key;
  int rc = sm1::scannxt(scan->scan, key, value);
  if (rc != 0) {
    return rc;
  }
  scan->key = key;
  // The value opens with the rowno and the cut flag.
  datum_t r, c;
  bool r_null, c_null;
  if (tup_decode(scan->index->value_desc, value, 0, r, r_null) != 0 ||
      tup_decode(scan->index->value_desc, value, 1, c, c_null) != 0) {
    return -1;
  }
  if (r_null || c_null) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT,
                       "index: an entry without its rowno or cut flag");
  }
  rowno = static_cast<uint64_t>(datum_to_int64(r));
  cut = datum_to_bool(c);
  scan->cut = cut;
  return 0;
}

/*
 * A datum_t over a key column's decoded bytes: a by-value one
 * sign-extended to 8 bytes as tuples give them (sm2.h), a varlena
 * made for a variable-width one and kept in the cursor, else a
 * pointer to the bytes.
 */
static datum_t key_datum(index_scan_t *scan, const type_t *type,
                         const std::string &bytes) {
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
    scan->varlenas.push_back(make_varlena(bytes));
    return datum_from_ptr(scan->varlenas.back().data());
  }
  return datum_from_ptr(bytes.data());
}

/* The key columns of the cursor's last entry, if the key gives them. */
int idx_scan_key(index_scan_t *scan, std::span<datum_t> values,
                 std::span<bool> isnull) {
  const index_t *ix = scan->index;
  if (values.size() != ix->key.size() || isnull.size() != ix->key.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a datum and null flag for every key column");
  }
  if (scan->cut || !key_decodable(ix->key)) {
    return 1;
  }
  // The relid, the columns, then the rowno if the entry has one.
  std::string_view key = scan->key;
  if (key.size() < sizeof(ix->relid)) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "index: an entry key too short");
  }
  key.remove_prefix(sizeof(ix->relid));
  scan->varlenas.clear();
  scan->varlenas.reserve(ix->key.size());
  if (decode_key_front(ix->key, key, scan->datums) != 0) {
    return -1;
  }
  bool has_null = false;
  for (size_t i = 0; i < ix->key.size(); i++) {
    isnull[i] = !scan->datums[i].has_value();
    has_null |= isnull[i];
    values[i] =
        isnull[i] ? 0 : key_datum(scan, ix->key[i].type, *scan->datums[i]);
  }
  size_t suffix = !ix->unique || has_null ? sizeof(uint64_t) : 0;
  if (key.size() != suffix) {
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
  return sm1::scan_seek(scan->scan, start, 1);
}

/* Close a cursor; nullptr is a no-op. */
void idx_scanend(index_scan_t *scan) {
  if (scan == nullptr) {
    return;
  }
  // Release sm1's cursor and its snapshot.
  sm1::scanend(scan->scan);
  delete scan;
}

} // namespace sm2
