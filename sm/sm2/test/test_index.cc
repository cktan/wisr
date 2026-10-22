/*
 * test_index.cc -- the index rel over sm1 (SM2_DESIGN.md §5):
 * entries in key order, scans between bounds, uniqueness, keys
 * cut to fit, and keys decoded back.
 */

#include "datums.h"
#include "db.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <algorithm>
#include <memory>
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

/* The first n values of row, each native, as Datums under cols. */
values_t datums_of(const std::vector<sm2::key_column_t> &cols,
                   const std::vector<std::optional<std::string>> &row,
                   size_t n = SIZE_MAX) {
  return values_of(types_of(cols), row, true, n);
}

/* encode_key of the first n values of row under the first n cols. */
std::string enc(const std::vector<sm2::key_column_t> &cols,
                const std::vector<std::optional<std::string>> &row, size_t n) {
  values_t d = datums_of(cols, row, n);
  std::string k;
  REQUIRE(sm2::encode_key(std::span(cols).first(n), d.d(), d.null(), k) == 0);
  return k;
}

/* Open an index; it must open. */
sm2::index_t *open_index(db_t &db, sm1::session_t *s,
                         const std::vector<sm2::key_column_t> &key, bool unique,
                         uint16_t relid = 1,
                         std::span<const sm2::attribute_t> include = {}) {
  sm2::index_t *ix = nullptr;
  REQUIRE_MESSAGE(
      sm2::idx_open(s, db.store(), relid, key, unique, include, false, ix) == 0,
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
      values_t d = datums_of(cols, e.key);
      REQUIRE_MESSAGE(
          sm2::idx_insert(ix, d.d(), d.null(), r, sm2::id_key(r), {}, {}) == 0,
          sm1::errmsg());
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
      values_t lo_d, hi_d;
      if (has_lo) {
        make_bound(lo_vals, lo);
        lo_d = datums_of(cols, lo_vals);
        lo.prefix = lo_d.d();
        lo.isnull = lo_d.null();
      }
      if (has_hi) {
        make_bound(hi_vals, hi);
        hi_d = datums_of(cols, hi_vals);
        hi.prefix = hi_d.d();
        hi.isnull = hi_d.null();
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

/* The bytes of a non-null datum of type, as the key encoding reads them. */
std::string bytes_of(const sm2::type_t *type, int32_t typmod, sm2::datum_t d) {
  std::string_view v;
  REQUIRE_MESSAGE(sm2::datum_bytes(type, typmod, d, v) == 0, sm1::errmsg());
  return std::string(v);
}

TEST_CASE("an index entry gives back its key columns") {
  // Verify idx_scan_key returns the key columns idx_insert was given.
  // 1. A non-unique index on a descending int2 with nulls first, a
  //    text, a decimal and a float8 takes four entries, one with
  //    nulls, and one with a text past the 1-byte header's 126 bytes
  //    and a NUL in it; a unique int4 index takes a value and a
  //    null, whose entry carries its rowno.
  // 2. Each entry a scan yields decodes to its columns' bytes and
  //    nulls, and a negative int2 is sign-extended, as a tuple's is;
  //    a text is the varlena make_varlena makes, in either header.
  // 3. A key column count that does not match is refused.
  db_t db;
  sm1::session_t *s = db.open();
  int32_t dec = decimal_typmod(10, 2);
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int2, -1, nullptr, true, true},
      {&sm2::type_text, -1, nullptr, false, false},
      {&sm2::type_decimal64, dec, nullptr, false, false},
      {&sm2::type_float8, -1, nullptr, false, false}};
  std::vector<sm2::key_column_t> ucols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  sm2::index_t *ix = open_index(db, s, cols, false, 1);
  sm2::index_t *uix = open_index(db, s, ucols, true, 2);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::vector<std::vector<std::optional<std::string>>> rows = {
      {le(int16_t{-1}), std::string("hello"), le(int64_t{125 * 32 + 2}),
       le(1.5)},
      {std::nullopt, std::string(""), std::nullopt, le(-2.0)},
      {le(int16_t{300}), std::nullopt, le(int64_t{-7 * 32 + 2}), std::nullopt},
      {le(int16_t{5}), std::string(300, 'x') + std::string(1, '\0') + "y",
       le(int64_t{1 * 32 + 2}), le(0.5)}};
  std::vector<values_t> vals;
  for (size_t i = 0; i < rows.size(); i++) {
    vals.push_back(datums_of(cols, rows[i]));
    REQUIRE_MESSAGE(sm2::idx_insert(ix, vals[i].d(), vals[i].null(), i + 1,
                                    sm2::id_key(i + 1), {}, {}) == 0,
                    sm1::errmsg());
  }
  std::vector<std::vector<std::optional<std::string>>> urows = {
      {le(int32_t{7})}, {std::nullopt}};
  for (size_t i = 0; i < urows.size(); i++) {
    values_t u = datums_of(ucols, urows[i]);
    REQUIRE(sm2::idx_insert(uix, u.d(), u.null(), i + 1, sm2::id_key(i + 1), {},
                            {}) == 0);
  }
  struct case_t {
    sm2::index_t *index;
    const std::vector<sm2::key_column_t> *keys;
    const std::vector<std::vector<std::optional<std::string>>> *rows;
  };
  for (const case_t &k :
       {case_t{ix, &cols, &rows}, case_t{uix, &ucols, &urows}}) {
    const std::vector<sm2::key_column_t> *keys = k.keys;
    sm2::index_scan_t *scan = nullptr;
    REQUIRE(sm2::idx_scan(k.index, nullptr, nullptr, scan) == 0);
    uint64_t rowno;
    bool cut;
    std::string_view value;
    size_t seen = 0;
    while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
      std::vector<sm2::datum_t> got(keys->size());
      std::unique_ptr<bool[]> null = std::make_unique<bool[]>(keys->size());
      REQUIRE_MESSAGE(
          sm2::idx_scan_key(scan, got,
                            std::span<bool>(null.get(), keys->size())) == 0,
          sm1::errmsg());
      const auto &want = (*k.rows)[rowno - 1];
      values_t w = datums_of(*keys, want);
      for (size_t c = 0; c < keys->size(); c++) {
        REQUIRE(null[c] == w.null()[c]);
        if (!null[c]) {
          const sm2::key_column_t &k = (*keys)[c];
          CHECK(bytes_of(k.type, k.typmod, got[c]) ==
                bytes_of(k.type, k.typmod, w.d()[c]));
        }
      }
      if (k.index == ix && 1 == rowno) {
        CHECK(sm2::datum_from_int16(-1) == got[0]);
      }
      if (k.index == ix && !null[1]) {
        std::string vl = sm2::make_varlena(*want[1]);
        CHECK(vl == std::string_view(reinterpret_cast<const char *>(got[1]),
                                     vl.size()));
      }
      seen++;
    }
    CHECK(seen == k.rows->size());
    std::vector<sm2::datum_t> few(1);
    bool fewnull[1];
    if (keys->size() > 1) {
      CHECK(sm2::idx_scan_key(scan, few, fewnull) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
    sm2::idx_scanend(scan);
  }
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(uix);
  sm2::idx_close(ix);
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
                                                 std::string("a")};
  values_t d = datums_of(cols, key);
  REQUIRE(sm2::idx_insert(ix, d.d(), d.null(), 10, sm2::id_key(10), {}, {}) ==
          0);
  CHECK(sm2::idx_insert(ix, d.d(), d.null(), 11, sm2::id_key(11), {}, {}) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  std::vector<std::optional<std::string>> with_null = {le(int32_t{1}),
                                                       std::nullopt};
  values_t n = datums_of(cols, with_null);
  CHECK(sm2::idx_insert(ix, n.d(), n.null(), 12, sm2::id_key(12), {}, {}) == 0);
  CHECK(sm2::idx_insert(ix, n.d(), n.null(), 13, sm2::id_key(13), {}, {}) == 0);
  REQUIRE(sm2::idx_delete(ix, d.d(), d.null(), 10, sm2::id_key(10)) == 0);
  CHECK(sm2::idx_insert(ix, d.d(), d.null(), 14, sm2::id_key(14), {}, {}) == 0);
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
  // 4. An entry's key keeps no datum, so idx_scan_key returns 1.
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
    return std::vector<std::optional<std::string>>{std::string(text)};
  };
  auto abc = row("abc"), upper = row("ABC"), abd = row("abd");
  auto mixed = row("aBc");
  values_t d_abc = datums_of(cols, abc), d_upper = datums_of(cols, upper),
           d_abd = datums_of(cols, abd), d_mixed = datums_of(cols, mixed);
  REQUIRE(sm2::idx_insert(uix, d_abc.d(), d_abc.null(), 10, sm2::id_key(10), {},
                          {}) == 0);
  CHECK(sm2::idx_insert(uix, d_upper.d(), d_upper.null(), 11, sm2::id_key(11),
                        {}, {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm2::idx_insert(uix, d_abd.d(), d_abd.null(), 12, sm2::id_key(12), {},
                        {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_abc.d(), d_abc.null(), 10, sm2::id_key(10), {},
                          {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_upper.d(), d_upper.null(), 11, sm2::id_key(11),
                          {}, {}) == 0);
  REQUIRE(sm2::idx_insert(ix, d_abd.d(), d_abd.null(), 12, sm2::id_key(12), {},
                          {}) == 0);
  sm2::bound_t eq{d_mixed.d(), d_mixed.null(), true};
  CHECK(scan_all(ix, &eq, &eq) == std::vector<uint64_t>{10, 11});
  {
    // An entry's key under the collation keeps no datum to give back.
    sm2::index_scan_t *scan = nullptr;
    REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
    uint64_t rowno;
    bool cut;
    std::string_view value;
    sm2::datum_t got[1];
    bool null[1];
    REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
    CHECK(sm2::idx_scan_key(scan, got, null) == 1);
    sm2::idx_scanend(scan);
  }
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
  sm2::idx_close(uix);
  sm2::collation_close(ci);
}

TEST_CASE("a key too long for sm1 is cut, flagged, and kept apart") {
  // Verify an over-long key in a non-unique index (SM2_DESIGN.md
  // §3.2).
  // 1. Two 5000-byte texts that share their first 4500 bytes both
  //    insert: each is cut to fit and carries its rowno, so the two
  //    stay apart though their cut bytes are the same.
  // 2. The scan reports both as cut, with their INCLUDE column, and
  //    idx_scan_key returns 1: a cut key's columns are the row's.
  // 3. A bound on such a text is itself too long: the scan says it
  //    was widened, and still yields both entries.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, nullptr, false, false}};
  std::vector<sm2::attribute_t> include = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::index_t *ix = open_index(db, s, cols, false, 1, include);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string common(4500, 'k');
  std::vector<std::string> texts = {common + std::string(500, 'a'),
                                    common + std::string(500, 'b')};
  for (size_t i = 0; i < texts.size(); i++) {
    std::vector<std::optional<std::string>> key = {texts[i]};
    values_t d = datums_of(cols, key);
    sm2::datum_t id[1] = {sm2::datum_from_int32(static_cast<int32_t>(i + 100))};
    bool idn[1] = {false};
    REQUIRE_MESSAGE(sm2::idx_insert(ix, d.d(), d.null(), i + 1,
                                    sm2::id_key(i + 1), id, idn) == 0,
                    sm1::errmsg());
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
    sm2::datum_t got[1];
    bool null[1];
    CHECK(sm2::idx_scan_key(scan, got, null) == 1);
    sm2::datum_t inc;
    bool inc_null;
    REQUIRE(sm2::tup_decode(sm2::idx_value_schema(ix), value, 2, inc,
                            inc_null) == 0);
    REQUIRE_FALSE(inc_null);
    CHECK(sm2::datum_to_int32(inc) == static_cast<int32_t>(want + 99));
  }
  sm2::idx_scanend(scan);
  std::vector<std::optional<std::string>> b = {texts[1]};
  values_t bd = datums_of(cols, b);
  sm2::bound_t lo{bd.d(), bd.null(), true};
  bool widened = false;
  CHECK(scan_all(ix, &lo, &lo, &widened) == std::vector<uint64_t>{1, 2});
  CHECK(widened);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}

TEST_CASE("a key too long for sm1 is refused in a unique index") {
  // Verify a unique index never holds a cut key (SM2_DESIGN.md §3.2).
  // 1. Inserting a 5000-byte text fails with SM_ERR_KEYTOOLONG.
  // 2. So does one beside a null column, though that entry would
  //    carry its rowno and meet no other as a duplicate.
  // 3. Deleting by such a key fails the same way.
  // 4. The transaction is still usable: a short key inserts, and
  //    the scan yields that entry alone, not cut.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, nullptr, false, false},
      {&sm2::type_text, -1, nullptr, false, false}};
  sm2::index_t *ix = open_index(db, s, cols, true);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string big(5000, 'k');
  values_t whole = datums_of(cols, {"a", big});
  CHECK(sm2::idx_insert(ix, whole.d(), whole.null(), 1, sm2::id_key(1), {},
                        {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_KEYTOOLONG);
  values_t nulled = datums_of(cols, {std::nullopt, big});
  CHECK(sm2::idx_insert(ix, nulled.d(), nulled.null(), 2, sm2::id_key(2), {},
                        {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_KEYTOOLONG);
  CHECK(sm2::idx_delete(ix, whole.d(), whole.null(), 1, sm2::id_key(1)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_KEYTOOLONG);
  values_t small = datums_of(cols, {"a", "b"});
  REQUIRE_MESSAGE(sm2::idx_insert(ix, small.d(), small.null(), 3,
                                  sm2::id_key(3), {}, {}) == 0,
                  sm1::errmsg());
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  uint64_t rowno;
  bool cut;
  std::string_view value;
  REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
  CHECK(rowno == 3);
  CHECK_FALSE(cut);
  CHECK(sm2::idx_scannxt(scan, rowno, cut, value) == 1);
  sm2::idx_scanend(scan);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}

TEST_CASE("at read committed an entry committed since begin is still deleted") {
  // Verify idx_delete resolves SM_ERR_UPDATED itself (SM2_DESIGN.md §5).
  // 1. A begins at READ COMMITTED.
  // 2. B inserts the entry for row 1 and commits, after A began.
  // 3. A deletes that entry: sm1 refuses the erase of a key committed
  //    since A began until A reads its newest version, and idx_delete
  //    does that read and repeats the erase, so the call succeeds.
  // 4. A commits, and the index is empty.
  // 5. An entry that is not there still fails with SM_ERR_NOTFOUND.
  db_t db;
  sm1::session_t *a = db.open();
  sm1::session_t *b = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_text, -1, nullptr, false, false}};
  sm2::index_t *ia = open_index(db, a, cols, false);
  sm2::index_t *ib = open_index(db, b, cols, false);
  values_t d = datums_of(cols, {"taken"});
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::idx_insert(ib, d.d(), d.null(), 1, sm2::id_key(1), {}, {}) == 0);
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE_MESSAGE(sm2::idx_delete(ia, d.d(), d.null(), 1, sm2::id_key(1)) == 0,
                  sm1::errmsg());
  CHECK(sm2::idx_delete(ia, d.d(), d.null(), 1, sm2::id_key(1)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  CHECK(scan_all(ia, nullptr, nullptr).empty());
  REQUIRE(sm1::commit(a) == 0);
  sm2::idx_close(ia);
  sm2::idx_close(ib);
}

TEST_CASE("index and heap entries share a store without meeting") {
  // Verify an index's relid keeps it apart from the heap and from
  // other indexes in the same sm1 store.
  // 1. A heap rel under relid 0 and two indexes under 1 and 2
  //    each get entries.
  // 2. Each index's unbounded scan yields its own entries only,
  //    and the heap's scan yields only its rows.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 0, schema, {}, "rowno", "", rel) == 0);
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  sm2::index_t *a = open_index(db, s, cols, false, 1);
  sm2::index_t *b = open_index(db, s, cols, false, 2);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<uint64_t> rows;
  for (int32_t v = 0; v < 5; v++) {
    sm2::datum_t d[1] = {sm2::datum_from_int32(v)};
    bool dn[1] = {false};
    uint64_t rowno;
    REQUIRE(sm2::rel_insert(rel, d, dn, rowno) == 0);
    rows.push_back(rowno);
    REQUIRE(sm2::idx_insert(v % 2 ? b : a, d, dn, rowno, sm2::id_key(rowno), {},
                            {}) == 0);
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
  // 2. idx_open refuses relid 0xFFFF, TOAST's, and an index
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
    sm2::datum_t d[1] = {sm2::datum_from_int32(v)};
    bool dn[1] = {false};
    REQUIRE(sm2::idx_insert(ix, d, dn, static_cast<uint64_t>(v + 1),
                            sm2::id_key(static_cast<uint64_t>(v + 1)), {},
                            {}) == 0);
  }
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  uint64_t rowno;
  bool cut;
  std::string_view value;
  REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
  sm2::datum_t sd[1] = {sm2::datum_from_int32(7)};
  bool sn[1] = {false};
  REQUIRE(sm2::idx_scan_seek(scan, sm2::bound_t{sd, sn, true}) == 0);
  REQUIRE(sm2::idx_scannxt(scan, rowno, cut, value) == 0);
  CHECK(rowno == 8);
  sm2::idx_scanend(scan);

  sm2::index_t *out = nullptr;
  CHECK(sm2::idx_open(s, db.store(), 0xFFFF, cols, false, {}, false, out) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm2::idx_open(s, db.store(), 3, {}, false, {}, false, out) == -1);
  sm2::datum_t two[2] = {sd[0], sd[0]};
  bool twon[2] = {false, false};
  CHECK(sm2::idx_insert(ix, two, twon, 1, sm2::id_key(1), {}, {}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm2::bound_t none{{}, {}, true}, many{two, twon, true};
  CHECK(sm2::idx_scan(ix, &none, nullptr, scan) == -1);
  CHECK(sm2::idx_scan(ix, &many, nullptr, scan) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm2::datum_t ad[1] = {sm2::datum_from_int32(99)};
  CHECK(sm2::idx_delete(ix, ad, sn, 1, sm2::id_key(1)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);
}
