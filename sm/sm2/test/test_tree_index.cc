/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_tree_index.cc -- an index rel over a tree (../BTREEINDEX.md §11.3
 * step 6): the same scans as an index of sm1's entries, over the
 * same entries, and what a tree does apart from them.
 */

#include "datums.h"
#include "db.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

/* Open an index, a tree or not, made if a tree; it must open. */
sm2::index_t *open_kind(db_t &db, sm1::session_t *s,
                        const std::vector<sm2::key_column_t> &key, bool unique,
                        uint16_t relid, bool tree) {
  sm2::index_t *ix = nullptr;
  REQUIRE_MESSAGE(sm2::idx_open(s, db.store(), relid, key, unique, {}, false,
                                ix, tree) == 0,
                  sm1::errmsg());
  if (tree) {
    REQUIRE_MESSAGE(sm2::idx_create(ix) == 0, sm1::errmsg());
  }
  return ix;
}

/* A keyless row's key: its _id, 8 bytes big-endian. */
std::string row_of(uint64_t id) {
  std::string r(8, '\0');
  for (int i = 0; i < 8; i++) {
    r[i] = static_cast<char>(id >> (56 - 8 * i));
  }
  return r;
}

/* Every entry a scan between lo and hi yields, as _ids in order. */
std::vector<uint64_t> ids_of(sm2::index_t *ix, const sm2::bound_t *lo,
                             const sm2::bound_t *hi) {
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm2::idx_scan(ix, lo, hi, scan) == 0, sm1::errmsg());
  std::vector<uint64_t> out;
  uint64_t id;
  bool cut;
  std::string_view value;
  int rc;
  while ((rc = sm2::idx_scannxt(scan, id, cut, value)) == 0) {
    out.push_back(id);
  }
  CHECK(1 == rc);
  sm2::idx_scanend(scan);
  return out;
}

} // namespace

TEST_CASE("tree index: a load reports writes to a committed SSI reader") {
  // Verify a load retains a committed index reader's SSI dependency.
  // 1. Create an empty tree and a second store.
  // 2. The loader reads the second store in its serializable snapshot.
  // 3. Another transaction scans the tree.
  // 4. Insert into the second store in that transaction.
  // 5. Commit that transaction.
  // 6. Build the loaded tree.
  // 7. Require the loader's commit to fail serialization.
  for (bool unique : {false, true}) {
    CAPTURE(unique);
    db_t db;
    auto *reader = db.open();
    auto *writer = db.open();
    std::vector<sm2::key_column_t> cols = {
        {&sm2::type_int4, -1, nullptr, false, false}};
    REQUIRE(0 == sm1::begin(reader, sm1::xctlevel_t::read_committed));
    auto *a = open_kind(db, reader, cols, unique, 2, true);
    uint64_t other;
    REQUIRE(0 == sm1::mkstore(reader, other));
    REQUIRE(0 == sm1::commit(reader));
    sm2::index_t *b = nullptr;
    REQUIRE(0 == sm2::idx_open(writer, db.store(), 2, cols, unique, {}, false,
                               b, true));

    REQUIRE(0 == sm1::begin(writer, sm1::xctlevel_t::serializable));
    sm1::scan_t *scan = nullptr;
    REQUIRE(0 == sm1::scan(writer, other, nullptr, 0, 1, nullptr, 0, 0, scan));
    std::string_view scanned_key, scanned_value;
    REQUIRE(1 == sm1::scannxt(scan, scanned_key, scanned_value));
    sm1::scanend(scan);
    REQUIRE(0 == sm1::begin(reader, sm1::xctlevel_t::serializable));
    CHECK(ids_of(a, nullptr, nullptr).empty());
    REQUIRE(0 == sm1::insert(reader, other, "y", "value"));
    REQUIRE(0 == sm1::commit(reader));

    sm1::loader_t *load = nullptr;
    REQUIRE(0 == sm1::load_open(writer, db.store(), load));
    size_t entries, pages;
    REQUIRE(0 == sm2::idx_load_tree(b, load, entries, pages));
    sm2::datum_t value[] = {sm2::datum_from_int32(7)};
    bool nulls[] = {false};
    REQUIRE(0 ==
            sm2::idx_load(b, load, entries, value, nulls, row_of(1), {}, {}));
    REQUIRE(0 == sm2::idx_load_build(b, load, entries, pages));
    REQUIRE(0 == sm1::load_close(load));
    REQUIRE(0 == sm2::idx_load_root(b));
    CHECK(-1 == sm1::commit(writer));
    CHECK(sm1::SM_ERR_SERIALIZATION == sm1::errnum());
    sm2::idx_close(a);
    sm2::idx_close(b);
  }
}

TEST_CASE("tree index: scans give what sm1's entries give") {
  // Verify a tree's scans against an index of sm1's entries.
  // 1. Open two indexes on (int4, text), one a tree with small pages
  //    so it is several levels deep, and insert the same 3,000
  //    random entries, nulls among them, into both.
  // 2. Between random bounds, on one column or two, inclusive or
  //    not, either end open, the two scans give the same _ids.
  // 3. A seek forward gives the same as the other's.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false},
      {&sm2::type_text, -1, nullptr, true, false}};
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm2::index_t *a = open_kind(db, s, cols, false, 1, false);
  sm2::index_t *b = open_kind(db, s, cols, false, 2, true);
  std::mt19937_64 rng(42);
  for (uint64_t id = 1; id <= 3000; id++) {
    int32_t n = static_cast<int32_t>(rng() % 200);
    std::string t = "t" + std::to_string(rng() % 50);
    sm2::datum_t d[2] = {sm2::datum_from_int32(n), 0};
    std::string v = sm2::make_varlena(t);
    d[1] = sm2::datum_from_ptr(v.data());
    bool isnull[2] = {0 == id % 97, 0 == id % 89};
    std::string row = row_of(id);
    for (sm2::index_t *ix : {a, b}) {
      REQUIRE_MESSAGE(sm2::idx_insert(ix, d, isnull, row, {}, {}) == 0,
                      sm1::errmsg());
    }
  }
  for (int round = 0; round < 300; round++) {
    CAPTURE(round);
    int32_t lo_n = static_cast<int32_t>(rng() % 210);
    int32_t hi_n = lo_n + static_cast<int32_t>(rng() % 40);
    std::string lo_t = sm2::make_varlena("t" + std::to_string(rng() % 50));
    sm2::datum_t lo_d[2] = {sm2::datum_from_int32(lo_n),
                            sm2::datum_from_ptr(lo_t.data())};
    sm2::datum_t hi_d[1] = {sm2::datum_from_int32(hi_n)};
    bool nn[2] = {false, false};
    size_t lo_cols = 1 + rng() % 2;
    sm2::bound_t lo{std::span(lo_d, lo_cols), std::span(nn, lo_cols),
                    0 == rng() % 2};
    sm2::bound_t hi{std::span(hi_d, 1), std::span(nn, 1), 0 == rng() % 2};
    const sm2::bound_t *l = 0 == rng() % 5 ? nullptr : &lo;
    const sm2::bound_t *h = 0 == rng() % 5 ? nullptr : &hi;
    CHECK(ids_of(a, l, h) == ids_of(b, l, h));
  }
  // A seek forward, on both.
  sm2::datum_t at[1] = {sm2::datum_from_int32(100)};
  bool nn[1] = {false};
  sm2::bound_t seek{std::span(at, 1), std::span(nn, 1), true};
  std::vector<uint64_t> got[2];
  int k = 0;
  for (sm2::index_t *ix : {a, b}) {
    sm2::index_scan_t *scan = nullptr;
    REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
    uint64_t id;
    bool cut;
    std::string_view value;
    for (int i = 0; i < 10; i++) {
      REQUIRE(0 == sm2::idx_scannxt(scan, id, cut, value));
    }
    REQUIRE(0 == sm2::idx_scan_seek(scan, seek));
    while (0 == sm2::idx_scannxt(scan, id, cut, value)) {
      got[k].push_back(id);
    }
    sm2::idx_scanend(scan);
    k++;
  }
  CHECK(got[0] == got[1]);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(a);
  sm2::idx_close(b);
}

TEST_CASE("tree index: an entry stays, outside the transaction") {
  // Verify what a tree does apart from sm1's entries.
  // 1. Insert an entry in a transaction and abort it: the entry is
  //    still there, as no transaction's.
  // 2. Insert it again: no change, no error. Delete it: still there.
  // 3. The entry's key, from the scan, is the one idx_entry_key makes
  //    of the same columns and row; its value's INCLUDE is null, its
  //    row's key the row's.
  // 4. A unique tree takes a key with a null by idx_insert, and
  //    refuses one without; idx_find refuses a tree.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::key_column_t> cols = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  std::vector<sm2::attribute_t> include = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm2::index_t *ix = nullptr;
  REQUIRE(sm2::idx_open(s, db.store(), 3, cols, false, include, false, ix,
                        true) == 0);
  REQUIRE(sm2::idx_create(ix) == 0);
  CHECK(sm2::idx_is_tree(ix));
  REQUIRE(sm1::commit(s) == 0);

  sm2::datum_t d[1] = {sm2::datum_from_int32(7)};
  sm2::datum_t inc[1] = {sm2::datum_from_int32(70)};
  bool nn[1] = {false};
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::idx_insert(ix, d, nn, row_of(5), inc, nn) == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(std::vector<uint64_t>{5} == ids_of(ix, nullptr, nullptr));
  REQUIRE(sm2::idx_insert(ix, d, nn, row_of(5), inc, nn) == 0);
  REQUIRE(sm2::idx_delete(ix, d, nn, row_of(5)) == 0);
  CHECK(std::vector<uint64_t>{5} == ids_of(ix, nullptr, nullptr));

  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  uint64_t id;
  bool cut;
  std::string_view value;
  REQUIRE(0 == sm2::idx_scannxt(scan, id, cut, value));
  std::string want;
  REQUIRE(sm2::idx_entry_key(ix, d, nn, row_of(5), want) == 0);
  CHECK(want == sm2::idx_scan_entry_key(scan));
  CHECK(row_of(5) == sm2::idx_scan_row(scan));
  CHECK_FALSE(cut);
  sm2::datum_t got;
  bool got_null = false;
  sm2::tup_desc_t desc(sm2::idx_value_schema(ix));
  REQUIRE(sm2::tup_decode(desc, value, 1, got, got_null) == 0);
  CHECK(got_null);
  sm2::idx_scanend(scan);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(ix);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm2::index_t *u = open_kind(db, s, cols, true, 4, true);
  bool null1[1] = {true};
  CHECK(sm2::idx_insert(u, d, null1, row_of(1), {}, {}) == 0);
  CHECK(sm2::idx_insert(u, d, nn, row_of(2), {}, {}) == -1);
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  std::string row;
  CHECK(sm2::idx_find(u, d, nn, id, &row) == -1);
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(u);
}
