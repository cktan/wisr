/*
 * test_key.cc -- key encoding (TYPE.md §1-§3, PLAN.md step 2):
 * each type's key against its own order, and keys of several
 * columns against the order of their columns.
 */

#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <unicode/ucol.h>
#include <unicode/uloc.h>

namespace {

/* v's little-endian bytes. */
template <typename T> std::string le(T v) {
  std::string out(sizeof(T), '\0');
  memcpy(out.data(), &v, sizeof(T));
  return out;
}

/* The sign of an int. */
int sign(int v) { return (v > 0) - (v < 0); }

/* A column's type and typmod, for the generators below. */
struct typed_t {
  const sm2::type_t *type;
  int32_t typmod;
};

/* decimal's typmod for (p, s). */
int32_t decimal_typmod(int p, int s) { return ((p << 16) | s) + 4; }

/* Every type, decimal at both of its widths. */
const std::vector<typed_t> &all_types() {
  static const std::vector<typed_t> types = {
      {&sm2::type_bool, -1},
      {&sm2::type_int2, -1},
      {&sm2::type_int4, -1},
      {&sm2::type_int8, -1},
      {&sm2::type_float4, -1},
      {&sm2::type_float8, -1},
      {&sm2::type_decimal, decimal_typmod(10, 2)},
      {&sm2::type_decimal, decimal_typmod(30, 5)},
      {&sm2::type_text, -1},
      {&sm2::type_varchar, -1},
      {&sm2::type_bytea, -1},
      {&sm2::type_date, -1},
      {&sm2::type_time, -1},
      {&sm2::type_timestamp, -1},
      {&sm2::type_timestamptz, -1},
      {&sm2::type_interval, -1},
      {&sm2::type_uuid, -1},
  };
  return types;
}

/* A random value of 0 .. n-1, n > 0, in 128 bits. */
unsigned __int128 below(std::mt19937_64 &rng, unsigned __int128 n) {
  unsigned __int128 r = static_cast<unsigned __int128>(rng()) << 64 | rng();
  return r % n;
}

/*
 * A random datum of t, drawn to meet the boundaries and the ties
 * the type has: the integers' ends, the floats' zeros and NaNs,
 * the datetimes' infinities, intervals that PostgreSQL calls
 * equal, byte strings that are prefixes of one another.
 */
std::string gen_datum(const typed_t &t, std::mt19937_64 &rng) {
  const sm2::type_t *type = t.type;
  if (type == &sm2::type_bool) {
    return std::string(1, static_cast<char>(rng() % 2));
  }
  if (type == &sm2::type_int2) {
    return le(gen_int<int16_t>(rng));
  }
  if (type == &sm2::type_int4 || type == &sm2::type_date) {
    return le(gen_int<int32_t>(rng));
  }
  if (type == &sm2::type_int8 || type == &sm2::type_timestamp ||
      type == &sm2::type_timestamptz) {
    return le(gen_int<int64_t>(rng));
  }
  if (type == &sm2::type_time) {
    return le(static_cast<int64_t>(rng() % 86400000001));
  }
  if (type == &sm2::type_float4) {
    return le(gen_float<float>(rng));
  }
  if (type == &sm2::type_float8) {
    return le(gen_float<double>(rng));
  }
  if (type == &sm2::type_decimal) {
    int p = (t.typmod - 4) >> 16;
    unsigned __int128 limit = 1;
    for (int i = 0; i < p; i++) {
      limit *= 10;
    }
    unsigned __int128 mag = rng() % 3 == 0 ? rng() % 3 : below(rng, limit);
    __int128 v =
        rng() % 2 ? -static_cast<__int128>(mag) : static_cast<__int128>(mag);
    return p <= 18 ? le(static_cast<int64_t>(v)) : le(v);
  }
  if (type == &sm2::type_text || type == &sm2::type_varchar) {
    return gen_utf8(rng);
  }
  if (type == &sm2::type_bytea) {
    std::string s;
    for (size_t n = rng() % 6; n > 0; n--) {
      s += static_cast<char>(rng() % 3 == 0   ? 0
                             : rng() % 3 == 0 ? 0xff
                                              : rng());
    }
    return s;
  }
  if (type == &sm2::type_interval) {
    // Small components collide under PostgreSQL's flattening.
    if (rng() % 2) {
      int32_t month = static_cast<int32_t>(rng() % 5) - 2;
      int32_t day = static_cast<int32_t>(rng() % 121) - 60;
      int64_t time = (static_cast<int64_t>(rng() % 5) - 2) * 86400000000;
      return le(time) + le(day) + le(month);
    }
    return le(gen_int<int64_t>(rng)) + le(gen_int<int32_t>(rng)) +
           le(gen_int<int32_t>(rng));
  }
  // uuid: random bytes, with shared prefixes now and then.
  std::string s = le(rng()) + le(rng());
  if (rng() % 3 == 0) {
    s.replace(0, 8, 8, '\0');
  }
  return s;
}

/* t's key of datum, which must encode. */
std::string key_of(const typed_t &t, std::string_view datum) {
  std::string key;
  REQUIRE(t.type->key_encode(datum, t.typmod, nullptr, key) == 0);
  return key;
}

/* memcmp's order on two keys, the shorter first on a tie. */
int key_order(const std::string &a, const std::string &b) {
  return sign(a.compare(b));
}

/* True for the types whose key folds datums PostgreSQL calls equal. */
bool folds(const typed_t &t) {
  return t.type == &sm2::type_float4 || t.type == &sm2::type_float8;
}

} // namespace

TEST_CASE("every type's key orders as the type, and is injective") {
  // Verify key_encode against cmp, type by type, from test_seed().
  // 1. Over generated pairs, where cmp decides, memcmp over the
  //    keys decides the same way.
  // 2. Where cmp calls two different datums equal, a float's keys
  //    are equal (-0 and 0, any two NaNs), and an interval's are
  //    not: its key refines PostgreSQL's order.
  // 3. For every other type, different datums get different keys.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  for (const typed_t &t : all_types()) {
    CAPTURE(t.type->name);
    CAPTURE(t.typmod);
    for (int i = 0; i < property_runs; i++) {
      std::string a = gen_datum(t, rng), b = gen_datum(t, rng);
      if (rng() % 8 == 0) {
        b = a;
      }
      int order = sign(t.type->cmp(a, b, t.typmod));
      int by_key = key_order(key_of(t, a), key_of(t, b));
      if (order != 0) {
        CHECK(by_key == order);
      } else if (a == b || folds(t)) {
        CHECK(by_key == 0);
      } else {
        CHECK(by_key != 0);
      }
    }
  }
}

TEST_CASE("every type's key decodes to its datum, or to what it keeps") {
  // Verify key_decode inverts key_encode, from test_seed().
  // 1. For every type but the floats, decoding a key gives back
  //    the datum, and uses the whole key.
  // 2. A float's key decodes to a datum cmp calls equal to the
  //    original -- 0 for -0, the quiet NaN for any NaN -- which is
  //    the loss TYPE.md §3.3 documents, and nothing more.
  // 3. A key cut short, or a bool key byte that is not 0 or 1, is
  //    SM_ERR_INVALID.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  for (const typed_t &t : all_types()) {
    CAPTURE(t.type->name);
    for (int i = 0; i < property_runs; i++) {
      std::string datum = gen_datum(t, rng), back;
      std::string key = key_of(t, datum);
      std::string_view rest = key;
      REQUIRE(t.type->key_decode(rest, t.typmod, nullptr, back) == 0);
      CHECK(rest.empty());
      if (folds(t)) {
        CHECK(t.type->cmp(back, datum, t.typmod) == 0);
        CHECK(key_of(t, back) == key);
      } else {
        CHECK(back == datum);
      }
      if (!key.empty()) {
        std::string_view cut = std::string_view(key).substr(0, key.size() - 1);
        CHECK(t.type->key_decode(cut, t.typmod, nullptr, back) == -1);
        CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
      }
    }
  }
  std::string_view two = "\x02";
  std::string back;
  CHECK(sm2::type_bool.key_decode(two, -1, nullptr, back) == -1);
}

TEST_CASE("the floats' keys fold -0 and NaN and put NaN last") {
  // Verify the float key's special values against PostgreSQL's
  // order (TYPE.md §3.3).
  // 1. -0 and 0 share a key.
  // 2. A negative NaN, a NaN with a payload and the quiet NaN
  //    share a key, above +Infinity's.
  // 3. -Infinity's key is below the most negative finite value's.
  for (const typed_t &t :
       {typed_t{&sm2::type_float4, -1}, typed_t{&sm2::type_float8, -1}}) {
    CAPTURE(t.type->name);
    bool f4 = t.type == &sm2::type_float4;
    auto key = [&](double v) {
      return key_of(t, f4 ? le(static_cast<float>(v)) : le(v));
    };
    auto bits_key = [&](uint64_t bits) {
      return key_of(t, f4 ? le(static_cast<uint32_t>(bits)) : le(bits));
    };
    CHECK(key(-0.0) == key(0.0));
    std::string nan = key(std::numeric_limits<double>::quiet_NaN());
    CHECK(bits_key(f4 ? 0xFFC00000 : 0xFFF8000000000000) == nan);
    CHECK(bits_key(f4 ? 0x7F800001 : 0x7FF0000000000001) == nan);
    CHECK(key(std::numeric_limits<double>::infinity()) < nan);
    double lowest = f4 ? std::numeric_limits<float>::lowest()
                       : std::numeric_limits<double>::lowest();
    CHECK(key(-std::numeric_limits<double>::infinity()) < key(lowest));
  }
}

TEST_CASE("an interval's key separates what PostgreSQL calls equal") {
  // Verify the interval key's tie-break (TYPE.md §3.10).
  // 1. '1 mon', '30 days' and 'PT720H' compare equal but have three
  //    different keys.
  // 2. Each sorts before '1 mon 1 microsecond', which is greater.
  const typed_t t{&sm2::type_interval, -1};
  std::string mon, days, hours, more;
  REQUIRE(t.type->in("P1M", -1, mon) == 0);
  REQUIRE(t.type->in("P30D", -1, days) == 0);
  REQUIRE(t.type->in("PT720H", -1, hours) == 0);
  REQUIRE(t.type->in("P1MT0.000001S", -1, more) == 0);
  CHECK(t.type->cmp(mon, days, -1) == 0);
  CHECK(t.type->cmp(mon, hours, -1) == 0);
  std::string km = key_of(t, mon), kd = key_of(t, days), kh = key_of(t, hours),
              kmore = key_of(t, more);
  CHECK(km != kd);
  CHECK(km != kh);
  CHECK(kd != kh);
  for (const std::string &k : {km, kd, kh}) {
    CHECK(k < kmore);
  }
}

namespace {

/*
 * The order of two rows under cols, from each column's own key:
 * nulls where the column puts them, values by their single-column
 * key -- which the tests above tie to the type's order -- reversed
 * for a descending column, the first difference deciding.
 */
int row_order(const std::vector<sm2::key_column_t> &cols,
              const std::vector<std::optional<std::string>> &a,
              const std::vector<std::optional<std::string>> &b) {
  for (size_t i = 0; i < cols.size(); i++) {
    const sm2::key_column_t &col = cols[i];
    int c;
    if (!a[i] || !b[i]) {
      c = a[i].has_value() == b[i].has_value() ? 0
          : !a[i]                              ? (col.nulls_first ? -1 : 1)
                                               : (col.nulls_first ? 1 : -1);
    } else {
      typed_t t{col.type, col.typmod};
      c = key_order(key_of(t, *a[i]), key_of(t, *b[i]));
      if (col.desc) {
        c = -c;
      }
    }
    if (c != 0) {
      return c;
    }
  }
  return 0;
}

/* Encode a row under cols; the row must encode. */
std::string encode_row(const std::vector<sm2::key_column_t> &cols,
                       const std::vector<std::optional<std::string>> &row) {
  std::vector<sm2::key_datum_t> datums;
  for (const auto &d : row) {
    datums.push_back({d ? std::string_view(*d) : std::string_view(), !d});
  }
  std::string key;
  REQUIRE(sm2::encode_key(cols, datums, key) == 0);
  return key;
}

} // namespace

TEST_CASE("a key of several columns orders by column, and decodes") {
  // Verify encode_key and decode_key by property, from
  // test_seed(), over random column lists: any types, ascending or
  // descending, nulls first or last.
  // 1. memcmp over two rows' keys agrees with the columns' order,
  //    nulls placed as each column asks, descending columns
  //    reversed -- so the null markers, the inversion and the
  //    prefix-free joins all hold.
  // 2. decode_key gives back every row's datums and nulls,
  //    floats aside, which come back as what their key keeps.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  const std::vector<typed_t> &types = all_types();
  for (int round = 0; round < 200; round++) {
    std::vector<sm2::key_column_t> cols;
    for (size_t n = 1 + rng() % 4; n > 0; n--) {
      const typed_t &t = types[rng() % types.size()];
      cols.push_back(
          {t.type, t.typmod, nullptr, rng() % 2 == 0, rng() % 2 == 0});
    }
    // Rows that share leading columns often, so later columns and
    // the joins between them are what decides.
    std::vector<std::vector<std::optional<std::string>>> rows;
    for (int r = 0; r < 50; r++) {
      std::vector<std::optional<std::string>> row;
      for (size_t i = 0; i < cols.size(); i++) {
        if (!rows.empty() && rng() % 2) {
          row.push_back(rows[rng() % rows.size()][i]);
        } else if (rng() % 5 == 0) {
          row.push_back(std::nullopt);
        } else {
          row.push_back(gen_datum({cols[i].type, cols[i].typmod}, rng));
        }
      }
      rows.push_back(row);
    }
    std::vector<std::string> keys;
    for (const auto &row : rows) {
      keys.push_back(encode_row(cols, row));
      std::vector<std::optional<std::string>> back;
      REQUIRE(sm2::decode_key(cols, keys.back(), back) == 0);
      REQUIRE(back.size() == row.size());
      for (size_t i = 0; i < row.size(); i++) {
        CHECK(back[i].has_value() == row[i].has_value());
        if (back[i] && row[i]) {
          typed_t t{cols[i].type, cols[i].typmod};
          CHECK(key_of(t, *back[i]) == key_of(t, *row[i]));
        }
      }
    }
    for (size_t i = 0; i < rows.size(); i++) {
      for (size_t j = 0; j < rows.size(); j++) {
        CHECK(key_order(keys[i], keys[j]) == row_order(cols, rows[i], rows[j]));
      }
    }
  }
}

TEST_CASE("nulls sort where each column asks, in either direction") {
  // Verify the null markers of TYPE.md §2 with one int4 column.
  // 1. Ascending, nulls first: null < 1 < 2. Nulls last: 1 < 2 <
  //    null.
  // 2. Descending, nulls first: null < 2 < 1. Nulls last: 2 < 1 <
  //    null.
  for (bool desc : {false, true}) {
    for (bool nulls_first : {false, true}) {
      CAPTURE(desc);
      CAPTURE(nulls_first);
      std::vector<sm2::key_column_t> cols = {
          {&sm2::type_int4, -1, nullptr, desc, nulls_first}};
      std::string null = encode_row(cols, {std::nullopt});
      std::string one = encode_row(cols, {le(int32_t{1})});
      std::string two = encode_row(cols, {le(int32_t{2})});
      CHECK((one < two) == !desc);
      CHECK((null < one) == nulls_first);
      CHECK((null < two) == nulls_first);
    }
  }
}

TEST_CASE("a byte string's key is a prefix of no other") {
  // Verify the escaping keeps two-column keys apart when the first
  // column of one is a prefix of the other's (TYPE.md §3.6).
  // 1. ('a', 'b') and ('a\0', 'b') get different keys, in the
  //    order of their first columns.
  // 2. ('a', 'z') sorts before ('ab', 'a'): the first column
  //    decides before the second is reached.
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_bytea, -1, nullptr, false, false},
      {&sm2::type_bytea, -1, nullptr, false, false}};
  std::string a_b = encode_row(cols, {std::string("a"), std::string("b")});
  std::string anul_b =
      encode_row(cols, {std::string("a\0", 2), std::string("b")});
  CHECK(a_b < anul_b);
  CHECK(encode_row(cols, {std::string("a"), std::string("z")}) <
        encode_row(cols, {std::string("ab"), std::string("a")}));
}

TEST_CASE("key encoding refuses what it cannot encode or decode") {
  // Verify the errors of encode_key, decode_key and the hooks.
  // 1. A datum count that differs from the column count, a column
  //    without a type, and a datum of the wrong width are
  //    SM_ERR_INVALID, and leave the key as it was.
  // 2. decode_key refuses a key with bytes left over, an unknown
  //    null marker, and one cut short.
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  std::string key = "keep";
  std::vector<sm2::key_datum_t> none;
  CHECK(sm2::encode_key(cols, none, key) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::vector<sm2::key_datum_t> wide = {{std::string_view("12345"), false}};
  CHECK(sm2::encode_key(cols, wide, key) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::vector<sm2::key_column_t> untyped = {
      {nullptr, -1, nullptr, false, false}};
  std::vector<sm2::key_datum_t> one = {{std::string_view("1234"), false}};
  CHECK(sm2::encode_key(untyped, one, key) == -1);
  CHECK(key == "keep");

  std::string good = encode_row(cols, {le(int32_t{7})});
  std::vector<std::optional<std::string>> back;
  CHECK(sm2::decode_key(cols, good + "x", back) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm2::decode_key(cols, "\x05" + good.substr(1), back) == -1);
  CHECK(sm2::decode_key(cols, good.substr(0, 3), back) == -1);
  CHECK(sm2::decode_key(cols, "", back) == -1);
}

namespace {

/* Open locale's collation, which must open. */
sm2::collation_t *open_collation(std::string_view locale) {
  sm2::collation_t *c = nullptr;
  REQUIRE_MESSAGE(sm2::collation_open(locale, true, "", c) == 0, sm1::errmsg());
  return c;
}

} // namespace

TEST_CASE("a collation opens by locale, and holds to its ICU version") {
  // Verify collation_open's checks (SM2_DESIGN.md §3.3).
  // 1. en-US, fr-CA, a keyword tag and the root locale open, each
  //    with ICU's collator version; C and POSIX open as byte
  //    order, with no version.
  // 2. Reopening with the version collation_version reported
  //    succeeds; with any other it is SM_ERR_INVALID.
  // 3. A non-deterministic collation, a language ICU does not
  //    know, a malformed tag, and a version on byte order are
  //    SM_ERR_INVALID.
  for (const char *locale :
       {"en-US", "fr-CA", "de-u-co-phonebk", "und-u-ks-level2", "und"}) {
    CAPTURE(locale);
    sm2::collation_t *c = open_collation(locale);
    std::string version(sm2::collation_version(c));
    CHECK_FALSE(version.empty());
    sm2::collation_t *again = nullptr;
    CHECK(sm2::collation_open(locale, true, version, again) == 0);
    sm2::collation_close(again);
    CHECK(sm2::collation_open(locale, true, "1.2.3.4", again) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    sm2::collation_close(c);
  }
  for (const char *locale : {"C", "POSIX"}) {
    sm2::collation_t *c = open_collation(locale);
    CHECK(sm2::collation_version(c).empty());
    sm2::collation_close(c);
    CHECK(sm2::collation_open(locale, true, "1", c) == -1);
  }
  sm2::collation_t *c = nullptr;
  for (std::string_view bad : {"zz", "xx-YY", "tlh", "en-US-!!"}) {
    CAPTURE(bad);
    CHECK(sm2::collation_open(bad, true, "", c) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  CHECK(sm2::collation_open("en-US", false, "", c) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm2::collation_close(nullptr);
}

TEST_CASE("a collated text key orders as ICU, its ties broken by bytes") {
  // Verify text and varchar keys under a collation, by property
  // from test_seed(), against ICU's own comparison.
  // 1. Under en-US, de-u-co-phonebk and the case-insensitive
  //    und-u-ks-level2, memcmp over two keys agrees with
  //    ucol_strcollUTF8, and where ICU calls two strings equal,
  //    with their bytes -- so different strings never share a key.
  // 2. Every key decodes to its string.
  // 3. Examples: under en-US "a" < "B", though "B" < "a" in bytes;
  //    under und-u-ks-level2 "a" and "A" are ICU-equal and keep
  //    two keys.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  for (const char *locale : {"en-US", "de-u-co-phonebk", "und-u-ks-level2"}) {
    CAPTURE(locale);
    sm2::collation_t *coll = open_collation(locale);
    char id[ULOC_FULLNAME_CAPACITY];
    UErrorCode status = U_ZERO_ERROR;
    uloc_forLanguageTag(locale, id, sizeof(id), nullptr, &status);
    UCollator *icu = ucol_open(id, &status);
    REQUIRE(U_SUCCESS(status));
    auto key = [&](const sm2::type_t *t, const std::string &s) {
      std::string k;
      REQUIRE(t->key_encode(s, -1, coll, k) == 0);
      return k;
    };
    for (const sm2::type_t *t : {&sm2::type_text, &sm2::type_varchar}) {
      CAPTURE(t->name);
      for (int i = 0; i < property_runs / 5; i++) {
        std::string a = gen_utf8(rng), b = gen_utf8(rng);
        if (rng() % 4 == 0) {
          // Case and accent variants of one string meet ICU ties.
          b = a;
          for (char &ch : b) {
            if (ch >= 'a' && ch <= 'z' && rng() % 2) {
              ch = static_cast<char>(ch - 'a' + 'A');
            }
          }
        }
        status = U_ZERO_ERROR;
        int want = sign(ucol_strcollUTF8(icu, a.data(), a.size(), b.data(),
                                         b.size(), &status));
        REQUIRE(U_SUCCESS(status));
        if (want == 0) {
          want = sign(a.compare(b));
        }
        std::string ka = key(t, a), kb = key(t, b);
        CHECK(sign(ka.compare(kb)) == want);
        std::string_view rest = ka;
        std::string back;
        REQUIRE(t->key_decode(rest, -1, coll, back) == 0);
        CHECK(back == a);
        CHECK(rest.empty());
      }
    }
    if (std::string_view(locale) == "en-US") {
      CHECK(key(&sm2::type_text, "a") < key(&sm2::type_text, "B"));
    }
    if (std::string_view(locale) == "und-u-ks-level2") {
      status = U_ZERO_ERROR;
      CHECK(ucol_strcollUTF8(icu, "a", 1, "A", 1, &status) == UCOL_EQUAL);
      CHECK(key(&sm2::type_text, "a") != key(&sm2::type_text, "A"));
    }
    ucol_close(icu);
    sm2::collation_close(coll);
  }
}

TEST_CASE("a collated column keys and decodes among others") {
  // Verify a text column under a collation inside encode_key.
  // 1. With (text en-US desc, int4 asc) columns, rows order by
  //    the text descending in ICU's order, then by the int.
  // 2. Every row decodes back to its datums.
  // 3. A byte-order collation, C, keys as nullptr does.
  sm2::collation_t *en = open_collation("en-US");
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, en, true, false},
      {&sm2::type_int4, -1, nullptr, false, false}};
  std::string a1 = encode_row(cols, {std::string("a"), le(int32_t{1})});
  std::string a2 = encode_row(cols, {std::string("a"), le(int32_t{2})});
  std::string b1 = encode_row(cols, {std::string("B"), le(int32_t{1})});
  CHECK(b1 < a1);
  CHECK(a1 < a2);
  for (const std::string &k : {a1, a2, b1}) {
    std::vector<std::optional<std::string>> back;
    CHECK(sm2::decode_key(cols, k, back) == 0);
  }
  std::vector<std::optional<std::string>> back;
  REQUIRE(sm2::decode_key(cols, b1, back) == 0);
  CHECK(back[0] == std::string("B"));
  sm2::collation_t *c = open_collation("C");
  std::string with_c, with_null;
  REQUIRE(sm2::type_text.key_encode("abc", -1, c, with_c) == 0);
  REQUIRE(sm2::type_text.key_encode("abc", -1, nullptr, with_null) == 0);
  CHECK(with_c == with_null);
  sm2::collation_close(c);
  sm2::collation_close(en);
}
