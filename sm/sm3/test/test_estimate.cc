/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_estimate.cc -- the estimators an analyze runs over its sample
 * (../STATISTICS.md §9.5), on values whose truth is known: uniform,
 * skewed, clustered and mostly null, and text under a collation that
 * calls two spellings one value; then tbl_analyze end to end.
 */

#include "analyze.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

/* An int4 key column, bytewise. */
const sm2::key_column_t int4_col = {&sm2::type_int4, -1, nullptr, false, false};

/* An int4 from those bytes. */
int32_t int4_of(const std::string &b) {
  int32_t v;
  std::memcpy(&v, b.data(), 4);
  return v;
}

/* The statistics of ints, a nullopt being null, for totalrows. */
sm3::stat_t ints(const std::vector<std::optional<int32_t>> &v, double totalrows,
                 int32_t target = 100) {
  std::vector<sm2::datum_t> d(v.size());
  std::unique_ptr<bool[]> null(new bool[v.size()]);
  for (size_t i = 0; i < v.size(); i++) {
    null[i] = !v[i];
    d[i] = sm2::datum_from_int32(v[i].value_or(0));
  }
  sm3::stat_t st;
  bool valid = false;
  REQUIRE(sm3::compute_scalar_stats(int4_col, d,
                                    std::span(null.get(), v.size()), target,
                                    totalrows, valid, st) == 0);
  REQUIRE(valid);
  return st;
}

} // namespace

TEST_CASE("uniform values: distinct estimated, a histogram, no order") {
  // Verify Duj1 and the histogram on a uniform column.
  // 1. 30,000 values drawn uniformly from 0..9,999, of a table of
  //    1,000,000: the distinct count is 10,000 within 10%, stored
  //    as a count, since it is under 10% of the rows.
  // 2. Values listed as common are those seen well over their
  //    share, each still rare; the histogram has 101 bounds,
  //    rising, from near 0 to near 9,999.
  // 3. The values are in no order: the correlation is near 0.
  std::mt19937_64 rng(1);
  std::uniform_int_distribution<int32_t> d(0, 9999);
  std::vector<std::optional<int32_t>> v;
  for (int i = 0; i < 30000; i++) {
    v.push_back(d(rng));
  }
  sm3::stat_t st = ints(v, 1000000.0);
  CHECK(st.null_frac == 0.0f);
  CHECK(st.width == 4);
  CHECK(st.distinct > 9000.0f);
  CHECK(st.distinct < 11000.0f);
  for (float f : st.mcv_freq) {
    CHECK(f < 0.001f);
  }
  REQUIRE(st.bounds.size() == 101);
  for (size_t i = 1; i < st.bounds.size(); i++) {
    CHECK(int4_of(st.bounds[i - 1]) < int4_of(st.bounds[i]));
  }
  CHECK(int4_of(st.bounds.front()) < 50);
  CHECK(int4_of(st.bounds.back()) > 9950);
  REQUIRE(st.has_correlation);
  CHECK(std::abs(st.correlation) < 0.05f);
}

TEST_CASE("skewed values: the common one listed, distinct scaling") {
  // Verify the common values on a skewed column.
  // 1. Half of 30,000 values are 7, the rest each its own, of a
  //    table of 100,000.
  // 2. 7 heads the common values at a frequency near 0.5, and no
  //    unique value is listed.
  // 3. The distinct count is Duj1's, n*d / (n - f1 + f1*n/N) with
  //    n 30,000, d 15,001, f1 15,000 and N 100,000: 23,078, over 10%
  //    of the rows, so stored as a fraction of them.
  std::vector<std::optional<int32_t>> v;
  for (int i = 0; i < 30000; i++) {
    v.push_back(i % 2 == 0 ? 7 : 1000 + i);
  }
  sm3::stat_t st = ints(v, 100000.0);
  REQUIRE(st.mcv.size() == 1);
  CHECK(int4_of(st.mcv[0]) == 7);
  CHECK(std::abs(st.mcv_freq[0] - 0.5f) < 0.001f);
  CHECK(std::abs(st.distinct + 0.23078f) < 0.00001f);
  CHECK(st.bounds.size() == 101);
}

TEST_CASE("clustered values: correlation follows the order") {
  // Verify the correlation of value order with sample order.
  // 1. Values that rise with the sample: correlation 1, and a
  //    unique column, distinct -1.
  // 2. Values that fall: correlation -1.
  std::vector<std::optional<int32_t>> up, down;
  for (int i = 0; i < 10000; i++) {
    up.push_back(i);
    down.push_back(-i);
  }
  sm3::stat_t a = ints(up, 10000.0), b = ints(down, 10000.0);
  REQUIRE(a.has_correlation);
  CHECK(a.correlation > 0.999f);
  CHECK(a.distinct == -1.0f);
  REQUIRE(b.has_correlation);
  CHECK(b.correlation < -0.999f);
}

TEST_CASE("few values, and nulls: a complete list, the null fraction") {
  // Verify the complete common-value list and the nulls.
  // 1. Values 1, 2, 3 only: distinct 3, all three listed with their
  //    frequencies, and no histogram.
  // 2. 95% nulls among unique values: a null fraction of 0.95, and
  //    distinct -0.05.
  // 3. Only nulls: a null fraction of 1, distinct 0.
  std::vector<std::optional<int32_t>> few, sparse, nulls;
  for (int i = 0; i < 3000; i++) {
    few.push_back(i % 3 + 1);
    sparse.push_back(i % 20 == 0 ? std::optional<int32_t>(i) : std::nullopt);
    nulls.push_back(std::nullopt);
  }
  sm3::stat_t a = ints(few, 100000.0);
  CHECK(a.distinct == 3.0f);
  CHECK(a.mcv.size() == 3);
  CHECK(a.bounds.empty());
  sm3::stat_t b = ints(sparse, 100000.0);
  CHECK(std::abs(b.null_frac - 0.95f) < 0.0001f);
  CHECK(std::abs(b.distinct + 0.05f) < 0.0001f);
  sm3::stat_t c = ints(nulls, 100000.0);
  CHECK(c.null_frac == 1.0f);
  CHECK(c.distinct == 0.0f);
}

TEST_CASE("text: a non-deterministic collation groups as it compares") {
  // Verify equality is the key's, under the column's collation.
  // 1. 3,000 values alternating "apple" and "APPLE": bytewise two
  //    values, each listed at a frequency of 0.5.
  // 2. Under a case-insensitive collation one value, listed once at
  //    a frequency of 1, as the first spelling seen.
  // 3. The width is a value's bytes, its varlena header included.
  std::vector<std::string> v;
  for (int i = 0; i < 3000; i++) {
    v.push_back(sm2::make_varlena(i % 2 == 0 ? "apple" : "APPLE"));
  }
  std::vector<sm2::datum_t> d;
  for (const std::string &s : v) {
    d.push_back(sm2::datum_from_ptr(s.data()));
  }
  std::unique_ptr<bool[]> null(new bool[v.size()]());
  sm2::collation_t *ci = nullptr;
  REQUIRE(sm2::collation_open("und-u-ks-level2", false, "", ci) == 0);
  sm2::key_column_t bytewise = {&sm2::type_text, -1, nullptr, false, false};
  sm2::key_column_t folded = {&sm2::type_text, -1, ci, false, false};
  sm3::stat_t a, b;
  bool valid = false;
  REQUIRE(sm3::compute_scalar_stats(bytewise, d,
                                    std::span(null.get(), v.size()), 100,
                                    3000.0, valid, a) == 0);
  REQUIRE(sm3::compute_scalar_stats(folded, d, std::span(null.get(), v.size()),
                                    100, 3000.0, valid, b) == 0);
  sm2::collation_close(ci);
  CHECK(a.distinct == 2.0f);
  CHECK(a.mcv.size() == 2);
  CHECK(b.distinct == 1.0f);
  REQUIRE(b.mcv.size() == 1);
  CHECK(b.mcv[0] == sm2::make_varlena("apple"));
  CHECK(b.mcv_freq[0] == 1.0f);
  CHECK(a.width == static_cast<int32_t>(v[0].size()));
}

TEST_CASE("tbl_analyze records each column's statistics and the sizes") {
  // Verify the analyze end to end, against what the rows are.
  // 1. Make t (a int4, b text, c int8) with an index on a, and fill
  //    50,000 rows: a the rowno, b one of ten words, c null but in
  //    one row in ten.
  // 2. Analyze for target 10 and commit: the statistics give 50,000
  //    rows, a heap size, and an entry for the index; a is unique
  //    and in rowno order, b has the ten words listed near 0.1
  //    each, and c a null fraction near 0.9. The index's size is
  //    unchecked: what of it is in files is RocksDB's estimate.
  // 3. After TRUNCATE the heap holds no bytes: an analyze records
  //    nothing, and the old statistics stay.
  db_t db;
  sm3::catalog_t *cat = nullptr;
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  REQUIRE(sm3::cat_open(db.env(), "db", cat) == 0);
  REQUIRE(sm3::session_open(cat, db.open(), ss) == 0);
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"b", sm2::type_text.code, -1, false},
      {"c", sm2::type_int8.code, -1, false}};
  uint64_t oid = 0, ix = 0;
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "t", cols, oid) == 0);
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(ss, oid, "t_a", keys, {}, false, ix) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t) == 0);
  std::vector<std::string> words;
  for (int w = 0; w < 10; w++) {
    words.push_back(sm2::make_varlena("word" + std::to_string(w)));
  }
  for (int32_t i = 1; i <= 50000; i++) {
    sm2::datum_t d[3] = {sm2::datum_from_int32(i),
                         sm2::datum_from_ptr(words[i % 10].data()),
                         sm2::datum_from_int64(i)};
    bool null[3] = {false, false, i % 10 != 0};
    uint64_t rowno = 0;
    REQUIRE(sm3::tbl_insert(t, d, null, rowno) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);

  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t) == 0);
  std::mt19937_64 rng(3);
  REQUIRE_MESSAGE(sm3::analyze_table(t, 10, rng) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  std::shared_ptr<const sm3::stats_t> st;
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_get_stats(ss, oid, st) == 0);
  REQUIRE(st != nullptr);
  CHECK(st->live_rows == 50000.0);
  CHECK(st->rows_sampled == 3000);
  CHECK(st->heap_disk + st->heap_mem > 0);
  REQUIRE(st->indexes.size() == 1);
  CHECK(st->indexes[0].oid == ix);
  REQUIRE(st->stats.size() == 3);
  const sm3::stat_t &a = st->stats[0], &b = st->stats[1], &c = st->stats[2];
  CHECK(a.position == 0);
  CHECK(a.distinct == -1.0f);
  REQUIRE(a.has_correlation);
  CHECK(a.correlation > 0.99f);
  CHECK(b.distinct == 10.0f);
  CHECK(b.mcv.size() == 10);
  for (float f : b.mcv_freq) {
    CHECK(std::abs(f - 0.1f) < 0.03f);
  }
  CHECK(std::abs(c.null_frac - 0.9f) < 0.03f);
  REQUIRE(sm3::commit(ss) == 0);

  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_EX, t) == 0);
  REQUIRE(sm3::tbl_truncate(t) == 0);
  REQUIRE(sm3::analyze_table(t, 10, rng) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_get_stats(ss, oid, st) == 0);
  REQUIRE(st != nullptr);
  CHECK(st->live_rows == 50000.0);
  REQUIRE(sm3::commit(ss) == 0);
  sm3::session_close(ss);
  sm3::cat_close(cat);
}

TEST_CASE("tbl_analyze asks the evaluator of partial and expression indexes") {
  // Verify the statistics of an index the host evaluates.
  // 1. Make t (a int4) with an evaluator that says "even" holds the
  //    rows whose a is even, and "mod10" keys a row by a % 10; a
  //    partial index on "even", an expression index on "mod10".
  // 2. Fill 20,000 rows, a the rowno, and analyze for target 10.
  // 3. The partial index holds half the rows, by its share and its
  //    entries; the plain columns' statistics are there as before.
  // 4. The expression's statistics are the index's, at its place,
  //    with its ten values each listed near 0.1.
  // 5. Loaded afresh, they come back the same.
  // 6. DROP INDEX takes the expression's statistic with it.
  db_t db;
  sm3::catalog_t *cat = nullptr;
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  REQUIRE(sm3::cat_open(db.env(), "db", cat) == 0);
  REQUIRE(sm3::cat_set_evaluator(
              cat, [](const sm3::table_def_t &, const sm3::index_def_t &x,
                      std::span<const sm2::datum_t> values,
                      std::span<const bool> isnull, bool &belongs,
                      std::vector<std::string> &out, std::span<bool> out_null) {
                int32_t a = sm2::datum_to_int32(values[0]);
                if (x.eval == "even") {
                  belongs = !isnull[0] && a % 2 == 0;
                  return 0;
                }
                int32_t m = a % 10;
                out[0].assign(reinterpret_cast<const char *>(&m), 4);
                out_null[0] = false;
                return 0;
              }) == 0);
  REQUIRE(sm3::session_open(cat, db.open(), ss) == 0);
  std::vector<sm3::column_spec_t> cols = {{"a", sm2::type_int4.code, -1, true}};
  uint64_t oid = 0, even = 0, mod10 = 0;
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> expr = {
      {-1, false, false, std::nullopt, sm2::type_int4.code, -1}};
  std::vector<int32_t> reads = {0};
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "t", cols, oid) == 0);
  REQUIRE(sm3::create_index(ss, oid, "t_even", on_a, {}, false, "even", reads,
                            even) == 0);
  REQUIRE(sm3::create_index(ss, oid, "t_mod10", expr, {}, false, "mod10", reads,
                            mod10) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t) == 0);
  for (int32_t i = 1; i <= 20000; i++) {
    sm2::datum_t d[1] = {sm2::datum_from_int32(i)};
    bool null[1] = {false};
    uint64_t rowno = 0;
    REQUIRE(sm3::tbl_insert(t, d, null, rowno) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(ss) == 0);

  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t) == 0);
  std::mt19937_64 rng(5);
  REQUIRE_MESSAGE(sm3::analyze_table(t, 10, rng) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  std::shared_ptr<const sm3::stats_t> st;
  REQUIRE(sm3::cat_get_stats(ss, oid, st) == 0);
  REQUIRE(st != nullptr);
  REQUIRE(st->indexes.size() == 2);
  CHECK(st->indexes[0].oid == even);
  CHECK(std::abs(st->indexes[0].belongs - 0.5f) < 0.03f);
  CHECK(std::abs(st->indexes[0].entries - 10000.0) < 600.0);
  CHECK(st->indexes[1].belongs == 1.0f);
  REQUIRE(st->stats.size() == 2);
  CHECK_FALSE(st->stats[0].index);
  const sm3::stat_t &e = st->stats[1];
  CHECK(e.has_index);
  CHECK(e.index == mod10);
  CHECK(e.position == 0);
  CHECK(e.distinct == 10.0f);
  REQUIRE(e.mcv.size() == 10);
  for (float f : e.mcv_freq) {
    CHECK(std::abs(f - 0.1f) < 0.03f);
  }
  std::vector<std::string> mcv = e.mcv;
  REQUIRE(sm3::commit(ss) == 0);

  // 5. Loaded afresh, the expression's values decode by its type.
  sm3::session_close(ss);
  sm3::cat_close(cat);
  REQUIRE(sm3::cat_open(db.env(), "db", cat) == 0);
  REQUIRE(sm3::session_open(cat, db.open(), ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_get_stats(ss, oid, st) == 0);
  REQUIRE(st != nullptr);
  REQUIRE(st->stats.size() == 2);
  CHECK(st->stats[1].index == mod10);
  CHECK(st->stats[1].mcv == mcv);
  REQUIRE(sm3::commit(ss) == 0);

  // 6. DROP INDEX takes its expression's statistic with it.
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_index(ss, oid, mod10) == 0);
  REQUIRE(sm3::commit(ss) == 0);
  sm3::session_close(ss);
  sm3::cat_close(cat);
  REQUIRE(sm3::cat_open(db.env(), "db", cat) == 0);
  REQUIRE(sm3::session_open(cat, db.open(), ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_get_stats(ss, oid, st) == 0);
  REQUIRE(st != nullptr);
  CHECK(st->stats.size() == 1);
  CHECK(st->indexes.size() == 1);
  REQUIRE(sm3::commit(ss) == 0);
  sm3::session_close(ss);
  sm3::cat_close(cat);
}
