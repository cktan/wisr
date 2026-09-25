/*
 * key.cc -- keys of several columns (TYPE.md §2): per column, a
 * null marker and the type's key, inverted for a descending
 * column, joined end to end with nothing between them.
 *
 * The marker is chosen so that nulls land where the column asks
 * in the finished key, after any inversion: 0x00 for a null and
 * 0x01 for a value when nulls sort low before inversion, 0x01
 * and 0x02 when they sort high. A descending column with nulls
 * first sorts them high before inverting, so that they end low.
 */

#include "native.h"
#include "sm2.h"

namespace {

constexpr char null_low = '\x00', value_mark = '\x01', null_high = '\x02';

/* The marker a null takes in col, before any inversion. */
char null_marker(const sm2::key_column_t &col) {
  return col.nulls_first != col.desc ? null_low : null_high;
}

/* Invert every byte of s from pos on: a descending column's key. */
void invert(std::string &s, size_t pos) {
  for (size_t i = pos; i < s.size(); i++) {
    s[i] = static_cast<char>(~s[i]);
  }
}

/* Fail with SM_ERR_INVALID: cols is not a list of typed columns. */
int bad_columns(const char *why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, std::string("key columns: ") + why);
}

} // namespace

namespace sm2 {

/* Append the key of datums under cols; key is unchanged on failure. */
int encode_key(std::span<const key_column_t> cols,
               std::span<const key_datum_t> datums, std::string &key) {
  return catch_nomem([&] {
    if (cols.size() != datums.size()) {
      return bad_columns("a datum for every column, no more");
    }
    size_t start = key.size();
    for (size_t i = 0; i < cols.size(); i++) {
      const key_column_t &col = cols[i];
      if (col.type == nullptr) {
        key.resize(start);
        return bad_columns("a column without a type");
      }
      // The marker, then the type's own key.
      size_t pos = key.size();
      if (datums[i].null) {
        key += null_marker(col);
      } else {
        key += value_mark;
        if (col.type->key_encode(datums[i].bytes, col.typmod, col.collation,
                                 key) != 0) {
          key.resize(start);
          return -1;
        }
      }
      // A descending column is the ascending one inverted.
      if (col.desc) {
        invert(key, pos);
      }
    }
    return 0;
  });
}

/* Decode a key encode_key made under cols, every byte of it. */
int decode_key(std::span<const key_column_t> cols, std::string_view key,
               std::vector<std::optional<std::string>> &datums) {
  return catch_nomem([&] {
    datums.clear();
    for (const key_column_t &col : cols) {
      if (col.type == nullptr) {
        return bad_columns("a column without a type");
      }
      // A descending column is read from an inverted copy of what
      // is left, and the key advances by what that copy used.
      std::string inverted;
      std::string_view rest = key;
      if (col.desc) {
        inverted.assign(key);
        invert(inverted, 0);
        rest = inverted;
      }
      size_t before = rest.size();
      if (rest.empty()) {
        return bad_key(col.type->name);
      }
      char marker = rest[0];
      rest.remove_prefix(1);
      if (marker == null_marker(col)) {
        datums.emplace_back(std::nullopt);
      } else if (marker == value_mark) {
        std::string datum;
        if (col.type->key_decode(rest, col.typmod, col.collation, datum) != 0) {
          return -1;
        }
        datums.emplace_back(std::move(datum));
      } else {
        return bad_key(col.type->name);
      }
      key.remove_prefix(before - rest.size());
    }
    if (!key.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "a key runs on past its last column");
    }
    return 0;
  });
}

} // namespace sm2
