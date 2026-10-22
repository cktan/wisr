/*
 * test_sample.cc -- the sample an analyze estimates from
 * (SM3_DESIGN.md §6.4): Floyd's draws, the runs, the row count
 * against a full count, and the budget that bounds every read.
 */

#include "analyze.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

/* The rownos a sample of target may cover at most, a run aside. */
uint64_t budget(int32_t target) {
  return sm3::budget_factor * sm3::rows_per_target * target;
}

/*
 * A catalog on a fresh database with t (a int4, b text) in public,
 * and an sm3 session ss on it.
 */
class sample_db_t : public db_t {
public:
  /* Initialize the catalog, open a session, and make t. */
  sample_db_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, open(), ss) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"a", sm2::type_int4.code, -1, true},
        {"b", sm2::type_text.code, -1, false}};
    begin();
    REQUIRE(sm3::create_table(ss, sm3::nsp_public, "t", cols, oid) == 0);
    commit();
  }

  /* Close the session and the catalog. */
  ~sample_db_t() {
    sm3::session_close(ss);
    sm3::cat_close(cat);
  }

  /* Begin a transaction on ss. */
  void begin() {
    REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  }

  /* Commit ss's transaction. */
  void commit() { REQUIRE_MESSAGE(sm3::commit(ss) == 0, sm1::errmsg()); }

  /* Insert n rows (i, a 20-byte text), committed; rownos 1..n. */
  void fill(uint64_t n) {
    std::string text = sm2::make_varlena(std::string(20, 'x'));
    begin();
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t) == 0);
    for (uint64_t i = 1; i <= n; i++) {
      sm2::datum_t d[2] = {sm2::datum_from_int32(static_cast<int32_t>(i)),
                           sm2::datum_from_ptr(text.data())};
      bool isnull[2] = {false, false};
      uint64_t rowno = 0;
      REQUIRE(sm3::tbl_insert(t, d, isnull, rowno) == 0);
    }
    sm3::tbl_close(t);
    commit();
  }

  /* Delete every row whose rowno keep says no to, committed. */
  template <typename F> void thin(uint64_t n, F keep) {
    begin();
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IX, t) == 0);
    for (uint64_t r = 1; r <= n; r++) {
      if (!keep(r)) {
        REQUIRE(sm3::tbl_delete(t, r) == 0);
      }
    }
    sm3::tbl_close(t);
    commit();
  }

  /* Sample t for target with a fixed seed, in a transaction. */
  sm3::sample_t sample(int32_t target) {
    std::mt19937_64 rng(42);
    sm3::sample_t s;
    begin();
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(ss, oid, sm1::SM_LOCKMODE_IS, t) == 0);
    REQUIRE_MESSAGE(sm3::sample_table(t, target, rng, s) == 0, sm1::errmsg());
    // Each kept row is the table's row at its rowno.
    for (size_t i = 0; i < s.rows.size(); i++) {
      std::string_view tuple;
      REQUIRE(sm3::tbl_read(t, s.rows[i].id, tuple) == 0);
      CHECK(tuple == s.rows[i].tuple);
      if (i > 0) {
        CHECK(s.rows[i - 1].id < s.rows[i].id);
      }
    }
    sm3::tbl_close(t);
    commit();
    return s;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *ss = nullptr;
  uint64_t oid = 0;
};

} // namespace

TEST_CASE("Floyd draws k distinct numbers uniformly, and no others") {
  // Verify floyd.
  // 1. All of 0..9 when k is n, and none past n when k is more.
  // 2. Five of 10^18: distinct, sorted and in range, drawn at once,
  //    as nothing of the range is walked.
  // 3. Three of 0..9, 30,000 times: each number comes up 9,000
  //    times, within 5%.
  std::mt19937_64 rng(7);
  std::vector<uint64_t> all = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  CHECK(sm3::floyd(10, 10, rng) == all);
  CHECK(sm3::floyd(10, 99, rng) == all);
  uint64_t huge = 1000000000000000000ULL;
  std::vector<uint64_t> five = sm3::floyd(huge, 5, rng);
  REQUIRE(five.size() == 5);
  for (size_t i = 0; i < five.size(); i++) {
    CHECK(five[i] < huge);
    if (i > 0) {
      CHECK(five[i - 1] < five[i]);
    }
  }
  std::map<uint64_t, int> count;
  for (int i = 0; i < 30000; i++) {
    for (uint64_t v : sm3::floyd(10, 3, rng)) {
      count[v]++;
    }
  }
  for (uint64_t v = 0; v < 10; v++) {
    CHECK(std::abs(count[v] - 9000) < 450);
  }
}

TEST_CASE("a dense table is counted exactly, from runs within the budget") {
  // Verify the runs on a table with every rowno live.
  // 1. Fill 60,000 rows and sample for target 10, 3,000 rows.
  // 2. It is not read whole: the runs cover at most the budget.
  // 3. It keeps 3,000 rows, each the table's at its rowno, in
  //    rowno order, and counts 60,000 rows exactly.
  sample_db_t db;
  db.fill(60000);
  sm3::sample_t s = db.sample(10);
  CHECK(s.rowno_limit == 60000);
  CHECK_FALSE(s.whole);
  CHECK(s.run_length > 1);
  CHECK(s.ids_covered <= budget(10) + s.run_length);
  CHECK(s.rows.size() == 3000);
  CHECK(s.live_rows == 60000.0);
  CHECK(s.avg_row_bytes > 0);
}

TEST_CASE("a table the budget covers is read whole, its sample uniform") {
  // Verify a table no larger than the budget is read whole, though
  // fewer runs would give the rows wanted.
  // 1. Fill 60,000 rows and sample for target 100: 30,000 rows
  //    wanted, a budget of 300,000 rownos.
  // 2. Every rowno is covered, the count is exact, and 30,000 rows
  //    are kept.
  // 3. The kept rows are spread over the table, not clustered in
  //    runs: each tenth of the rownos holds 3,000 within 10%.
  sample_db_t db;
  db.fill(60000);
  sm3::sample_t s = db.sample(100);
  CHECK(s.whole);
  CHECK(s.ids_covered == 60000);
  CHECK(s.live_rows == 60000.0);
  REQUIRE(s.rows.size() == 30000);
  std::vector<int> tenth(10, 0);
  for (const sm3::sample_row_t &r : s.rows) {
    tenth[(r.id - 1) / 6000]++;
  }
  for (int c : tenth) {
    CHECK(std::abs(c - 3000) < 300);
  }
}

TEST_CASE("a sparse table is counted within a few percent, in the budget") {
  // Verify the sparse-table rule.
  // 1. Fill 60,000 rows and keep one rowno in ten: 6,000 live.
  // 2. Sample for target 10: the runs stay within the budget, and
  //    count 6,000 rows within 5%.
  // 3. The sample is short of 3,000, as a sparse table's may be,
  //    but not by half.
  sample_db_t db;
  db.fill(60000);
  db.thin(60000, [](uint64_t r) { return r % 10 == 0; });
  sm3::sample_t s = db.sample(10);
  CHECK_FALSE(s.whole);
  CHECK(s.ids_covered <= budget(10) + s.run_length);
  CHECK(std::abs(s.live_rows - 6000.0) < 300.0);
  CHECK(s.rows.size() <= 3000);
  CHECK(s.rows.size() > 1500);
}

TEST_CASE("an emptied table is sampled within the budget, never scanned") {
  // Verify a pilot that finds nothing live reads no more.
  // 1. Fill 60,000 rows and delete them all.
  // 2. Sample for target 10: no rows, a count of 0, and no more
  //    rownos covered than the budget allows.
  sample_db_t db;
  db.fill(60000);
  db.thin(60000, [](uint64_t) { return false; });
  sm3::sample_t s = db.sample(10);
  CHECK_FALSE(s.whole);
  CHECK(s.rows.empty());
  CHECK(s.live_rows == 0.0);
  CHECK(s.ids_covered <= budget(10) + s.run_length);
}

TEST_CASE("a small table is read whole and counted exactly") {
  // Verify the runs take every slot of a table under the sample.
  // 1. A table that never had a row: whole, nothing kept.
  // 2. Fill 1,000 rows and sample for target 10: every rowno is
  //    covered, every row kept, and the count is exact.
  // 3. A target below 1 is SM_ERR_INVALID.
  sample_db_t db;
  sm3::sample_t s = db.sample(10);
  CHECK(s.whole);
  CHECK(s.rows.empty());
  db.fill(1000);
  s = db.sample(10);
  CHECK(s.whole);
  CHECK(s.ids_covered == 1000);
  CHECK(s.rows.size() == 1000);
  CHECK(s.live_rows == 1000.0);
  db.begin();
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(db.ss, db.oid, sm1::SM_LOCKMODE_IS, t) == 0);
  std::mt19937_64 rng(1);
  CHECK(sm3::sample_table(t, 0, rng, s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  db.commit();
}
