/*
 * test_index.cc -- the index rel over sm1 (SM2_DESIGN.md §5):
 * entries in key order, scans between bounds, uniqueness, and
 * keys cut to fit.
 */

#include "datums.h"
#include "db.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

/* An entry as a test keeps it: its key datums and its rowno. */
struct entry_t {
  std::vector<std::optional<std::string>> key;
  uint64_t rowno;
};

/* The datums of the first n values of row. */
std::vector<sm2::datum_t>
datums_of(const std::vector<std::optional<std::string>> &row,
          size_t n = SIZE_MAX) {
  std::vector<sm2::datum_t> d;
  for (size_t i = 0; i < std::min(n, row.size()); i++) {
    d.push_back(
        {row[i] ? std::string_view(*row[i]) : std::string_view(), !row[i]});
  }
  return d;
}

/* encode_key of the first n values of row under the first n cols. */
std::string enc(const std::vector<sm2::key_column_t> &cols,
                const std::vector<std::optional<std::string>> &row, size_t n) {
  std::vector<sm2::datum_t> d = datums_of(row, n);
  std::string k;
  REQUIRE(sm2::encode_key(std::span(cols).first(n), d, k) == 0);
  return k;
}

/* Open an index; it must open. */
sm2::index_t *open_index(db_t &db, sm1::session_t *s,
                         const std::vector<sm2::key_column_t> &key, bool unique,
                         uint16_t prefix = 1,
                         std::span<const sm2::attribute_t> include = {}) {
  sm2::index_t *ix = nullptr;
  REQUIRE_MESSAGE(
      sm2::idx_open(s, db.store(), prefix, key, unique, include, ix) == 0,
      sm1::errmsg());
  return ix;
}

/* Every entry a scan between lo and hi yields, as rownos in order. */
std::vector<uint64_t> scan_all(sm2::index_t *ix, const sm2::bound_t *lo,
                               const sm2::bound_t *hi,
                               bool *widened = nullptr) {
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm2::idx_scan(ix, lo, hi, scan, widened) == 0, sm1::errmsg());
  std::vector<uint64_t> out;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
    out.push_back(rowno);
  }
  sm2::idx_scanend(scan);
  return out;
}

/* Types that make short keys, so that entries share prefixes. */
const std::vector<typed_t> &key_types() {
  static const std::vector<typed_t> types = {
      {&sm2::type_bool, -1},  {&sm2::type_int4, -1},
      {&sm2::type_text, -1},  {&sm2::type_float8, -1},
      {&sm2::type_bytea, -1}, {&sm2::type_decimal64, decimal_typmod(10, 2)}};
  return types;
}

} // namespace

TEST_CASE("an index scan yields exactly the entries between its bounds") {
  // Verify idx_scan's bounds by property, from test_seed(),
  // against a reference that compares encoded key prefixes
  // directly.
  // 1. Over random indexes of one to three key columns, each
  //    ascending or descending, nulls first or last, insert random
  //    entries that often share leading values, nulls included.
  // 2. For random bounds -- on one to all key columns, inclusive or
  //    not, either end open -- an entry is expected when its first
  //    j columns' encoding lies within the bound's, as bytes.
  // 3. The scan yields exactly the expected entries, in key order
  //    then rowno, and no bound here widens it.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  db_t db;
  sm1::session_t *s = db.open();
  const std::vector<typed_t> &types = key_types();
  for (int round = 0; round < 40; round++) {
    CAPTURE(round);
    std::vector<sm2::key_column_t> cols;
    for (size_t n = 1 + rng() % 3; n > 0; n--) {
      const typed_t &t = types[rng() % types.size()];
      cols.push_back(
          {t.type, t.typmod, nullptr, rng() % 2 == 0, rng() % 2 == 0});
    }
    sm2::index_t *ix =
        open_index(db, s, cols, false, static_cast<uint16_t>(round + 1));
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    std::vector<entry_t> entries;
    for (uint64_t r = 1; r <= 60; r++) {
      entry_t e{{}, r};
      for (size_t i = 0; i < cols.size(); i++) {
        if (!entries.empty() && rng() % 2) {
          e.key.push_back(entries[rng() % entries.size()].key[i]);
        } else if (rng() % 6 == 0) {
          e.key.push_back(std::nullopt);
        } else {
          e.key.push_back(gen_datum({cols[i].type, cols[i].typmod}, rng));
        }
      }
      std::vector<sm2::datum_t> d = datums_of(e.key);
      REQUIRE_MESSAGE(sm2::idx_insert(ix, d, r, {}) == 0, sm1::errmsg());
      entries.push_back(e);
    }
    // Key order: the whole encoding, then the rowno.
    std::sort(entries.begin(), entries.end(),
              [&](const entry_t &a, const entry_t &b) {
                std::string ka = enc(cols, a.key, cols.size());
                std::string kb = enc(cols, b.key, cols.size());
                return ka != kb ? ka < kb : a.rowno < b.rowno;
              });
    for (int q = 0; q < 30; q++) {
      // A bound from an entry's leading values, or random ones.
      auto make_bound = [&](std::vector<std::optional<std::string>> &vals,
                            sm2::bound_t &b) {
        size_t j = 1 + rng() % cols.size();
        const entry_t &from = entries[rng() % entries.size()];
        vals.assign(from.key.begin(), from.key.begin() + j);
        if (rng() % 4 == 0) {
          vals[j - 1] = gen_datum({cols[j - 1].type, cols[j - 1].typmod}, rng);
        }
        b.inclusive = rng() % 2 == 0;
      };
      std::vector<std::optional<std::string>> lo_vals, hi_vals;
      sm2::bound_t lo{}, hi{};
      bool has_lo = rng() % 4 != 0, has_hi = rng() % 4 != 0;
      std::vector<sm2::datum_t> lo_d, hi_d;
      if (has_lo) {
        make_bound(lo_vals, lo);
        lo_d = datums_of(lo_vals);
        lo.prefix = lo_d;
      }
      if (has_hi) {
        make_bound(hi_vals, hi);
        hi_d = datums_of(hi_vals);
        hi.prefix = hi_d;
      }
      std::vector<uint64_t> want;
      for (const entry_t &e : entries) {
        bool in = true;
        if (has_lo) {
          std::string p = enc(cols, lo_vals, lo_vals.size());
          std::string k = enc(cols, e.key, lo_vals.size());
          in &= lo.inclusive ? k >= p : k > p;
        }
        if (has_hi) {
          std::string p = enc(cols, hi_vals, hi_vals.size());
          std::string k = enc(cols, e.key, hi_vals.size());
          in &= hi.inclusive ? k <= p : k < p;
        }
        if (in) {
          want.push_back(e.rowno);
        }
      }
      bool widened = true;
      CHECK(scan_all(ix, has_lo ? &lo : nullptr, has_hi ? &hi : nullptr,
                     &widened) == want);
      CHECK_FALSE(widened);
    }
    REQUIRE(sm1::commit(s) == 0);
    sm2::idx_close(ix);
  }
}

TEST_CASE("a unique index refuses a duplicate key, but not nulls") {
  // Verify uniqueness in an index keyed without the rowno.
  // 1. A second entry with an existing key fails with sm1's
  //    SM_ERR_DUPKEY, and the transaction is still usable at
  //    READ COMMITTED.
  // 2. Entries whose key has a null column carry their rowno, so
  //    two of them with equal values stand side by side.
  // 3. After the first entry is deleted, its key can be inserted
  //    again.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false},
      {&sm2::type_text, -1, nullptr, false, false}};
  sm2::index_t *ix = open_index(db, s, cols, true);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::vector<std::optional<std::string>> key = {le(int32_t{1}),
                                                 sm2::make_varlena("a")};
  std::vector<sm2::datum_t> d = datums_of(key);
  REQUIRE(sm2::idx_insert(ix, d, 10, {}) == 0);
  CHECK(sm2::idx_insert(ix, d, 11, {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  std::vector<std::optional<std::string>> with_null = {le(int32_t{1}),
                                                       std::nullopt};
  std::vector<sm2::datum_t> n = datums_of(with_null);
  CHECK(sm2::idx_insert(ix, n, 12, {}) == 0);
  CHECK(sm2::idx_insert(ix, n, 13, {}) == 0);
  REQUIRE(sm2::idx_delete(ix, d, 10) == 0);
  CHECK(sm2::idx_insert(ix, d, 14, {}) == 0);
  CHECK(scan_all(ix, nullptr, nullptr) == std::vector<uint64_t>{14, 12, 13});
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}

TEST_CASE("under a non-deterministic collation, equal is ICU's equal") {
  // Verify a text index under the case-insensitive und-u-ks-level2
  // treats strings ICU calls equal as one key, as PostgreSQL does.
  // 1. A unique index takes "abc", then refuses "ABC" with
  //    SM_ERR_DUPKEY, and takes "abd".
  // 2. A non-unique index takes "abc" and "ABC" as two entries.
  // 3. A scan of the non-unique index between "aBc" and "aBc",
  //    inclusive, yields both of them and not "abd".
  db_t db;
  sm1::session_t *s = db.open();
  sm2::collation_t *ci = nullptr;
  REQUIRE(sm2::collation_open("und-u-ks-level2", false, "", ci) == 0);
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, ci, false, false}};
  sm2::index_t *uix = open_index(db, s, cols, true, 1);
  sm2::index_t *ix = open_index(db, s, cols, false, 2);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  auto row = [](const char *text) {
    return std::vector<std::optional<std::string>>{sm2::make_varlena(text)};
  };
  auto abc = row("abc"), upper = row("ABC"), abd = row("abd");
  auto mixed = row("aBc");
  std::vector<sm2::datum_t> d_abc = datums_of(abc), d_upper = datums_of(upper),
                            d_abd = datums_of(abd), d_mixed = datums_of(mixed);
  REQUIRE(sm2::idx_insert(uix, d_abc, 10, {}) == 0);
  CHECK(sm2::idx_insert(uix, d_upper, 11, {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm2::idx_insert(uix, d_abd, 12, {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_abc, 10, {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_upper, 11, {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_abd, 12, {}) == 0);
  sm2::bound_t eq{d_mixed, true};
  CHECK(scan_all(ix, &eq, &eq) == std::vector<uint64_t>{10, 11});
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
  sm2::idx_close(uix);
  sm2::collation_close(ci);
}

TEST_CASE("a key too long for sm1 is cut, flagged, and kept apart") {
  // Verify an over-long key in a unique index (SM2_DESIGN.md §3.2).
  // 1. Two 5000-byte texts that share their first 4500 bytes both
  //    insert: each is cut to fit, carries its rowno, and so never
  //    meets the other as a duplicate.
  // 2. The scan reports both as cut, with their INCLUDE column.
  // 3. A bound on such a text is itself too long: the scan says it
  //    was widened, and still yields both entries.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, nullptr, false, false}};
  std::vector<sm2::attribute_t> include = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::index_t *ix = open_index(db, s, cols, true, 1, include);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string common(4500, 'k');
  std::vector<std::string> texts = {common + std::string(500, 'a'),
                                    common + std::string(500, 'b')};
  for (size_t i = 0; i < texts.size(); i++) {
    std::vector<std::optional<std::string>> key = {sm2::make_varlena(texts[i])};
    std::vector<sm2::datum_t> d = datums_of(key);
    std::string inc = le(static_cast<int32_t>(i + 100));
    std::vector<sm2::datum_t> id = {{inc, false}};
    REQUIRE_MESSAGE(sm2::idx_insert(ix, d, i + 1, id) == 0, sm1::errmsg());
  }
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  uint64_t rowno;
  bool cut;
  std::string_view value;
  for (uint64_t want : {uint64_t{1}, uint64_t{2}}) {
    REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
    CHECK(rowno == want);
    CHECK(cut);
    std::optional<std::string_view> inc;
    REQUIRE(sm2::tup_decode(sm2::idx_value_schema(ix), value, 2, inc) == 0);
    REQUIRE(inc);
    CHECK(*inc == le(static_cast<int32_t>(want + 99)));
  }
  sm2::idx_scanend(scan);
  std::vector<std::optional<std::string>> b = {sm2::make_varlena(texts[1])};
  std::vector<sm2::datum_t> bd = datums_of(b);
  sm2::bound_t lo{bd, true};
  bool widened = false;
  CHECK(scan_all(ix, &lo, &lo, &widened) == std::vector<uint64_t>{1, 2});
  CHECK(widened);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}

TEST_CASE("index and heap entries share a store without meeting") {
  // Verify an index's prefix keeps it apart from the heap and from
  // other indexes in the same sm1 store.
  // 1. A heap rel under prefix 0 and two indexes under 1 and 2
  //    each get entries.
  // 2. Each index's unbounded scan yields its own entries only,
  //    and the heap's scan yields only its rows.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 0, schema, "rowno", rel) == 0);
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  sm2::index_t *a = open_index(db, s, cols, false, 1);
  sm2::index_t *b = open_index(db, s, cols, false, 2);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<uint64_t> rows;
  for (int32_t v = 0; v < 5; v++) {
    std::string bytes = le(v);
    std::vector<sm2::datum_t> d = {{bytes, false}};
    uint64_t rowno;
    REQUIRE(sm2::rel_insert(rel, d, rowno) == 0);
    rows.push_back(rowno);
    REQUIRE(sm2::idx_insert(v % 2 ? b : a, d, rowno, {}) == 0);
  }
  CHECK(scan_all(a, nullptr, nullptr) ==
        std::vector<uint64_t>{rows[0], rows[2], rows[4]});
  CHECK(scan_all(b, nullptr, nullptr) ==
        std::vector<uint64_t>{rows[1], rows[3]});
  sm2::rel_scan_t *scan = nullptr;
  REQUIRE(sm2::rel_scan(rel, 0, UINT64_MAX, scan) == 0);
  std::vector<uint64_t> seen;
  uint64_t rowno;
  std::string_view tuple;
  while (sm2::rel_scannxt(scan, rowno, tuple) == 0) {
    seen.push_back(rowno);
  }
  sm2::rel_scanend(scan);
  CHECK(seen == rows);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(a);
  sm2::idx_close(b);
  sm2::rel_close(rel);
}

TEST_CASE("an index scan seeks forward, and index calls refuse misuse") {
  // Verify idx_scan_seek and the SM_ERR_INVALID cases.
  // 1. After reading the first entry, seeking to 7 resumes at the
  //    entry for 7.
  // 2. idx_open refuses prefix 0xFFFF, TOAST's, and an index
  //    with no key columns; idx_insert refuses a wrong number of
  //    key datums; idx_scan refuses a bound with no columns or
  //    more than the index has.
  // 3. Deleting an entry that is not there is sm1's
  //    SM_ERR_NOTFOUND.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  sm2::index_t *ix = open_index(db, s, cols, false);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int32_t v = 0; v < 10; v++) {
    std::string bytes = le(v);
    std::vector<sm2::datum_t> d = {{bytes, false}};
    REQUIRE(sm2::idx_insert(ix, d, static_cast<uint64_t>(v + 1), {}) == 0);
  }
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  uint64_t rowno;
  bool cut;
  std::string_view value;
  REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
  std::string seven = le(int32_t{7});
  std::vector<sm2::datum_t> sd = {{seven, false}};
  REQUIRE(sm2::idx_scan_seek(scan, sm2::bound_t{sd, true}) == 0);
  REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
  CHECK(rowno == 8);
  sm2::idx_scanend(scan);

  sm2::index_t *out = nullptr;
  CHECK(sm2::idx_open(s, db.store(), 0xFFFF, cols, false, {}, out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm2::idx_open(s, db.store(), 3, {}, false, {}, out) == -1);
  std::vector<sm2::datum_t> two = {{seven, false}, {seven, false}};
  CHECK(sm2::idx_insert(ix, two, 1, {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm2::bound_t none{{}, true}, many{two, true};
  CHECK(sm2::idx_scan(ix, &none, nullptr, scan) == -1);
  CHECK(sm2::idx_scan(ix, &many, nullptr, scan) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string absent = le(int32_t{99});
  std::vector<sm2::datum_t> ad = {{absent, false}};
  CHECK(sm2::idx_delete(ix, ad, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}
