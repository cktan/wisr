/*
 * index.cc -- the index rel (SM2_DESIGN.md §5): a table's index
 * entries in its sm1 store, under the index's prefix, keyed by
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

#include <optional>
#include <string>
#include <vector>

struct sm2::index_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t prefix;
  std::span<const key_column_t> key; /* borrowed */
  bool unique;
  std::vector<attribute_t> value; /* rowno, cut, then INCLUDE */
};

struct sm2::index_scan_t {
  sm1::scan_t *scan;
  const index_t *index;
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

/* The index's prefix, big-endian, which starts all its keys. */
std::string prefix_of(const sm2::index_t *ix) {
  std::string p;
  sm2::put_be(ix->prefix, p);
  return p;
}

/*
 * The sm1 key of the entry for key and rowno: the prefix, the
 * encoded key columns, and the rowno where the entry carries one;
 * cut is set if the columns were cut to fit.
 */
int entry_key(const sm2::index_t *ix, std::span<const sm2::datum_t> key,
              uint64_t rowno, std::string &out, bool &cut) {
  if (key.size() != ix->key.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index: a datum for every key column, no more");
  }
  // Encode the columns whole, then decide how they fit.
  std::string cols;
  if (sm2::encode_key(ix->key, key, cols) != 0) {
    return -1;
  }
  bool has_null = false;
  for (const sm2::datum_t &d : key) {
    has_null |= d.null;
  }
  bool suffix = !ix->unique || has_null;
  cut = cols.size() > sm2::max_key_len(suffix);
  // Cut at the one length every cut key and cut bound share.
  if (cut) {
    // A cut key keeps the rowno, so cut keys that meet stay apart.
    suffix = true;
    cols.resize(sm2::max_key_len(true));
  }
  out = prefix_of(ix) + cols;
  if (suffix) {
    sm2::put_be(rowno, out);
  }
  return 0;
}

/*
 * The encoding of a bound's columns, after the prefix, cut where
 * an entry's key is cut, so the cut bound orders against cut keys
 * as the whole one would against theirs; cut is then set.
 */
int bound_key(const sm2::index_t *ix, const sm2::bound_t &b, std::string &out,
              bool &cut) {
  if (b.prefix.empty() || b.prefix.size() > ix->key.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "index: a bound gives 1 to " +
                                                std::to_string(ix->key.size()) +
                                                " columns");
  }
  std::string cols;
  if (sm2::encode_key(ix->key.first(b.prefix.size()), b.prefix, cols) != 0) {
    return -1;
  }
  cut = cols.size() > sm2::max_key_len(true);
  if (cut) {
    cols.resize(sm2::max_key_len(true));
  }
  out = prefix_of(ix) + cols;
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
    // A successor exists: the prefix is below 0xFFFF.
    out = *successor(out);
  }
  return 0;
}

} // namespace

namespace sm2 {

/* Open an index; see sm2.h. */
int idx_open(sm1::session_t *session, uint64_t storeid, uint16_t prefix,
             std::span<const key_column_t> key, bool unique,
             std::span<const attribute_t> include, index_t *&out) {
  return catch_nomem([&] {
    if (session == nullptr || key.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: a session and at least one key column");
    }
    // 0xFFFF is TOAST's (SM2_DESIGN.md §5); below it, every key
    // has a successor.
    if (prefix == 0xFFFF) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: prefix 0xFFFF is TOAST's, not an index's");
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
    out = new index_t{session, storeid, prefix, key, unique, std::move(value)};
    return 0;
  });
}

/* Close an index; nullptr is a no-op. */
void idx_close(index_t *index) { delete index; }

/* The schema of an entry's value: rowno, cut, then INCLUDE. */
std::span<const attribute_t> idx_value_schema(const index_t *index) {
  return index->value;
}

/* Add the entry for the row at rowno; see sm2.h. */
int idx_insert(index_t *index, std::span<const datum_t> key, uint64_t rowno,
               std::span<const datum_t> include) {
  return catch_nomem([&] {
    std::string k;
    bool cut;
    if (entry_key(index, key, rowno, k, cut) != 0) {
      return -1;
    }
    if (include.size() + 2 != index->value.size()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index: a datum for every INCLUDE column, no more");
    }
    // The value: rowno, cut, then the INCLUDE columns.
    std::string r, c;
    store_le(static_cast<int64_t>(rowno), r);
    c.assign(1, cut ? '\1' : '\0');
    std::vector<datum_t> datums = {{r, false}, {c, false}};
    datums.insert(datums.end(), include.begin(), include.end());
    std::string value;
    if (tuple_encode(index->value, datums, value) != 0) {
      return -1;
    }
    return sm1::insert(index->session, index->storeid, k, value);
  });
}

/* Remove the entry for key and rowno. */
int idx_delete(index_t *index, std::span<const datum_t> key, uint64_t rowno) {
  return catch_nomem([&] {
    std::string k;
    bool cut;
    if (entry_key(index, key, rowno, k, cut) != 0) {
      return -1;
    }
    return sm1::erase(index->session, index->storeid, k);
  });
}

/* Open a cursor between lo and hi; see sm2.h. */
int idx_scan(index_t *index, const bound_t *lo, const bound_t *hi,
             index_scan_t *&out, bool *widened) {
  return catch_nomem([&] {
    bool wide = false;
    // The start: the bound, or the first of the index's keys.
    std::string start;
    if (lo) {
      if (lower_key(index, *lo, start, wide) != 0) {
        return -1;
      }
    } else {
      start = prefix_of(index);
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
      end = successor(prefix_of(index));
    }
    sm1::scan_t *c;
    if (sm1::scan(index->session, index->storeid, start.data(), start.size(), 1,
                  end ? end->data() : nullptr, end ? end->size() : 0,
                  end_inclusive, c) != 0) {
      return -1;
    }
    try {
      out = new index_scan_t{c, index};
    } catch (...) {
      sm1::scanend(c);
      throw;
    }
    if (widened) {
      *widened = wide;
    }
    return 0;
  });
}

/* The next entry of a cursor: 0, 1 at the end, or -1. */
int idx_scannxt(index_scan_t *scan, uint64_t &rowno, bool &cut,
                std::string_view &value) {
  return catch_nomem([&] {
    std::string_view key;
    int rc = sm1::scannxt(scan->scan, key, value);
    if (rc != 0) {
      return rc;
    }
    // The value opens with the rowno and the cut flag.
    std::optional<std::string_view> r, c;
    if (tuple_decode(scan->index->value, value, 0, r) != 0 ||
        tuple_decode(scan->index->value, value, 1, c) != 0) {
      return -1;
    }
    if (!r || !c) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT,
                         "index: an entry without its rowno or cut flag");
    }
    rowno = static_cast<uint64_t>(load_le<int64_t>(*r));
    cut = (*c)[0] != '\0';
    return 0;
  });
}

/* Move a cursor forward to lower bound lo. */
int idx_scan_seek(index_scan_t *scan, const bound_t &lo) {
  return catch_nomem([&] {
    std::string start;
    bool wide = false;
    if (lower_key(scan->index, lo, start, wide) != 0) {
      return -1;
    }
    return sm1::scan_seek(scan->scan, start, 1);
  });
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
