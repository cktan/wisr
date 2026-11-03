/*
 * test_stats.cc -- a table's statistics in the catalog
 * (SM3_DESIGN.md §6.6, §6.7): recorded in a transaction, published
 * at its commit, loaded on first use, and what DDL does to them.
 */

#include "db.h"
#include "doctest.h"
#include "internal.h"
#include "sm3.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

/*
 * A catalog with cat_init committed, on a fresh database, loaded,
 * with an sm3 session ss over the sm1 session s.
 */
class cat_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  cat_db_t() {
    s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
  }

  /* Close the session and the catalog. */
  ~cat_db_t() {
    sm3::session_close(ss);
    sm3::cat_close(cat);
  }

  /* Restart, then load the catalog and open a session again. */
  void reload() {
    sm3::session_close(ss);
    sm3::cat_close(cat);
    ss = nullptr;
    cat = nullptr;
    restart();
    s = open();
    load();
  }

  /* Begin a transaction on ss. */
  void begin() {
    REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  }

  /* Commit ss's transaction. */
  void commit() { REQUIRE_MESSAGE(sm3::commit(ss) == 0, sm1::errmsg()); }

  sm1::session_t *s = nullptr;
  sm3::session_t *ss = nullptr;
  sm3::catalog_t *cat = nullptr;

private:
  /* Load the catalog; open ss over s. */
  void load() {
    REQUIRE_MESSAGE(sm3::cat_open(env(), "db", cat) == 0, sm1::errmsg());
    REQUIRE(sm3::session_open(cat, s, ss) == 0);
  }
};

/* v's little-endian bytes, as sm2::datum_bytes gives an int4's. */
std::string le4(int32_t v) {
  std::string b(4, '\0');
  std::memcpy(b.data(), &v, 4);
  return b;
}

/* Make t (a int4, b text, c int8) in public, with an index on a. */
void make_t(cat_db_t &db, uint64_t &oid, uint64_t &index) {
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"b", sm2::type_text.code, -1, false},
      {"c", sm2::type_int8.code, -1, false}};
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, oid) == 0);
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE_MESSAGE(
      sm3::create_index(db.ss, oid, "t_a", keys, {}, false, index) == 0,
      sm1::errmsg());
}

/* Statistics for t: a's, with values, and b's, and the index's. */
sm3::stats_t sample_stats(uint64_t index, double live_rows) {
  sm3::stats_t st;
  st.target = 100;
  st.run_length = 64;
  st.rows_sampled = 3000;
  st.rowno_limit = 10000;
  st.live_rows = live_rows;
  st.heap_disk = 123456;
  st.heap_mem = 789;
  st.avg_row_bytes = 40;
  st.indexes = {{index, 2048, 16, live_rows, 1.0f}};
  sm3::stat_t a;
  a.position = 0;
  a.null_frac = 0.0f;
  a.width = 4;
  a.distinct = -1.0f;
  a.has_correlation = true;
  a.correlation = 0.5f;
  a.mcv = {le4(7), le4(-3)};
  a.mcv_freq = {0.25f, 0.125f};
  a.bounds = {le4(-100), le4(0), le4(100)};
  sm3::stat_t b;
  b.position = 1;
  b.null_frac = 0.5f;
  b.width = 9;
  b.distinct = 42.0f;
  b.mcv = {sm2::make_varlena("apple")};
  b.mcv_freq = {0.5f};
  b.bounds = {sm2::make_varlena(""), sm2::make_varlena("zebra")};
  st.stats = {a, b};
  return st;
}

/* Whether two sets of statistics are the same, field for field. */
void check_same(const sm3::stats_t &x, const sm3::stats_t &y) {
  CHECK(x.format == y.format);
  CHECK(x.target == y.target);
  CHECK(x.run_length == y.run_length);
  CHECK(x.rows_sampled == y.rows_sampled);
  CHECK(x.rowno_limit == y.rowno_limit);
  CHECK(x.live_rows == y.live_rows);
  CHECK(x.heap_disk == y.heap_disk);
  CHECK(x.heap_mem == y.heap_mem);
  CHECK(x.avg_row_bytes == y.avg_row_bytes);
  REQUIRE(x.indexes.size() == y.indexes.size());
  for (size_t i = 0; i < x.indexes.size(); i++) {
    CHECK(x.indexes[i].oid == y.indexes[i].oid);
    CHECK(x.indexes[i].disk == y.indexes[i].disk);
    CHECK(x.indexes[i].mem == y.indexes[i].mem);
    CHECK(x.indexes[i].entries == y.indexes[i].entries);
    CHECK(x.indexes[i].belongs == y.indexes[i].belongs);
  }
  REQUIRE(x.stats.size() == y.stats.size());
  for (size_t i = 0; i < x.stats.size(); i++) {
    CHECK(x.stats[i].index == y.stats[i].index);
    CHECK(x.stats[i].position == y.stats[i].position);
    CHECK(x.stats[i].null_frac == y.stats[i].null_frac);
    CHECK(x.stats[i].width == y.stats[i].width);
    CHECK(x.stats[i].distinct == y.stats[i].distinct);
    CHECK(x.stats[i].has_correlation == y.stats[i].has_correlation);
    CHECK(x.stats[i].correlation == y.stats[i].correlation);
    CHECK(x.stats[i].mcv == y.stats[i].mcv);
    CHECK(x.stats[i].mcv_freq == y.stats[i].mcv_freq);
    CHECK(x.stats[i].bounds == y.stats[i].bounds);
  }
}

/* How many tables' statistics the catalog holds in memory. */
size_t loaded(sm3::catalog_t *cat) {
  std::shared_lock lock(cat->mu);
  return cat->stats.size();
}

} // namespace

TEST_CASE("statistics are recorded, published at commit, and loaded back") {
  // Verify cat_put_stats and cat_get_stats across a commit and a
  // restart.
  // 1. A table never analyzed has none.
  // 2. Statistics put in a transaction are its own at once; another
  //    session sees none until the commit, then the same.
  // 3. After a restart nothing is in memory until a table is asked
  //    about; then they load, field for field and value for value.
  cat_db_t db;
  uint64_t t = 0, ix = 0;
  db.begin();
  make_t(db, t, ix);
  db.commit();
  std::shared_ptr<const sm3::stats_t> got;
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  CHECK(got == nullptr);

  sm3::stats_t st = sample_stats(ix, 9000.0);
  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  REQUIRE_MESSAGE(sm3::cat_put_stats(db.ss, t, st) == 0, sm1::errmsg());
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  REQUIRE(got != nullptr);
  check_same(*got, st);
  REQUIRE(sm3::cat_get_stats(other, t, got) == 0);
  CHECK(got == nullptr);
  db.commit();
  REQUIRE(sm3::cat_get_stats(other, t, got) == 0);
  REQUIRE(got != nullptr);
  check_same(*got, st);
  sm3::session_close(other);

  db.reload();
  CHECK(loaded(db.cat) == 0);
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  REQUIRE(got != nullptr);
  check_same(*got, st);
  CHECK(loaded(db.cat) == 1);
}

TEST_CASE("every session reads one copy of a table's statistics") {
  // Verify cat_get_stats hands out the copy it keeps, never another,
  // as sm.h passes it on to every client (OBJECTIVE.md).
  // 1. Record statistics and commit.
  // 2. Two sessions each ask twice: all four are one object, loaded
  //    once after a restart.
  // 3. Statistics recorded and committed since are a new object.
  cat_db_t db;
  uint64_t t = 0, ix = 0;
  db.begin();
  make_t(db, t, ix);
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 9000.0)) == 0);
  db.commit();
  db.reload();

  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  std::shared_ptr<const sm3::stats_t> a, b, c, d;
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, a) == 0);
  REQUIRE(sm3::cat_get_stats(db.ss, t, b) == 0);
  REQUIRE(sm3::cat_get_stats(other, t, c) == 0);
  REQUIRE(sm3::cat_get_stats(other, t, d) == 0);
  REQUIRE(a != nullptr);
  CHECK(a == b);
  CHECK(a == c);
  CHECK(a == d);
  CHECK(loaded(db.cat) == 1);
  db.commit();

  db.begin();
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 7000.0)) == 0);
  db.commit();
  REQUIRE(sm3::cat_get_stats(other, t, c) == 0);
  CHECK(c != a);
  CHECK(c->live_rows == 7000.0);
  sm3::session_close(other);
}

TEST_CASE("an aborted analyze leaves the old, and a held one is kept") {
  // Verify rollback and publish leave readers what they had.
  // 1. Record statistics with 9000 rows and commit.
  // 2. Record 5000 in a transaction and abort, then again under a
  //    savepoint rolled back to: 9000 both times.
  // 3. A planner holds the 9000 ones; 7000 are recorded and
  //    committed: the held ones still say 9000, a new read 7000.
  cat_db_t db;
  uint64_t t = 0, ix = 0;
  db.begin();
  make_t(db, t, ix);
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 9000.0)) == 0);
  db.commit();
  std::shared_ptr<const sm3::stats_t> got;

  db.begin();
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 5000.0)) == 0);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  REQUIRE(got != nullptr);
  CHECK(got->live_rows == 9000.0);
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 5000.0)) == 0);
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  CHECK(got->live_rows == 9000.0);
  db.commit();

  std::shared_ptr<const sm3::stats_t> held = got;
  db.begin();
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 7000.0)) == 0);
  db.commit();
  CHECK(held->live_rows == 9000.0);
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  CHECK(got->live_rows == 7000.0);
  db.commit();
}

TEST_CASE("what DDL does to statistics") {
  // Verify the table of SM3_DESIGN.md §6.7.
  // 1. Statistics of no column, of a column out of range, or whose
  //    values do not fit the type, are SM_ERR_INVALID; of a table
  //    that is not there, SM_ERR_NOTFOUND.
  // 2. After DROP INDEX and DROP COLUMN, a load leaves out the
  //    index's entry and the dropped column's statistic, also after
  //    a restart; the rest is as recorded.
  // 3. DROP TABLE takes them with it: after a restart the oid has
  //    none, and a table made again by that name has none either.
  cat_db_t db;
  uint64_t t = 0, ix = 0;
  db.begin();
  make_t(db, t, ix);
  sm3::stats_t st = sample_stats(ix, 9000.0);
  sm3::stats_t bad = st;
  bad.stats[0].position = 7;
  CHECK(sm3::cat_put_stats(db.ss, t, bad) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  bad = st;
  bad.stats[0].mcv[0] = "xyz";
  CHECK(sm3::cat_put_stats(db.ss, t, bad) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  bad = st;
  bad.stats[0].has_index = true;
  bad.stats[0].index = ix;
  CHECK(sm3::cat_put_stats(db.ss, t, bad) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_put_stats(db.ss, t + 999, st) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::cat_put_stats(db.ss, t, st) == 0);
  db.commit();

  db.begin();
  REQUIRE_MESSAGE(sm3::drop_index(db.ss, t, ix) == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm3::drop_column(db.ss, t, "a") == 0, sm1::errmsg());
  db.commit();
  db.reload();
  std::shared_ptr<const sm3::stats_t> got;
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  REQUIRE(got != nullptr);
  CHECK(got->indexes.empty());
  REQUIRE(got->stats.size() == 1);
  CHECK(got->stats[0].position == 1);
  CHECK(got->stats[0].bounds == st.stats[1].bounds);
  CHECK(got->live_rows == 9000.0);

  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  db.commit();
  db.reload();
  db.begin();
  REQUIRE(sm3::cat_get_stats(db.ss, t, got) == 0);
  CHECK(got == nullptr);
  uint64_t t2 = 0, ix2 = 0;
  make_t(db, t2, ix2);
  CHECK(t2 != t);
  REQUIRE(sm3::cat_get_stats(db.ss, t2, got) == 0);
  CHECK(got == nullptr);
  db.commit();
}

namespace {

/* How many STATS entries the table oid has, read in ss's transaction. */
size_t entries(cat_db_t &db, uint64_t oid) {
  sm2::catkv_t *kv = nullptr;
  REQUIRE(sm2::catkv_open(db.s, kv) == 0);
  std::string prefix;
  REQUIRE(sm3::stats_prefix(oid, prefix) == 0);
  sm2::catkv_scan_t *c = nullptr;
  REQUIRE(sm2::catkv_scan(kv, prefix, c) == 0);
  size_t n = 0;
  std::string_view k, v;
  while (sm2::catkv_scannxt(c, k, v) == 0) {
    n++;
  }
  sm2::catkv_scanend(c);
  sm2::catkv_close(kv);
  return n;
}

} // namespace

TEST_CASE("each statistic is an entry of its own, and DDL takes its own") {
  // Verify the STATS entries, one a statistic and one the table's.
  // 1. Statistics of two columns are three entries: the table's own,
  //    and one each.
  // 2. DROP INDEX of a plain index changes none; DROP COLUMN takes
  //    the column's entry with it, and the rest stay.
  // 3. An analyze that has no statistic for a column leaves it no
  //    entry: statistics of b alone, recorded again, are two.
  // 4. DROP TABLE leaves none.
  cat_db_t db;
  uint64_t t = 0, ix = 0;
  db.begin();
  make_t(db, t, ix);
  REQUIRE(sm3::cat_put_stats(db.ss, t, sample_stats(ix, 9000.0)) == 0);
  CHECK(entries(db, t) == 3);
  db.commit();

  db.begin();
  REQUIRE(sm3::drop_index(db.ss, t, ix) == 0);
  CHECK(entries(db, t) == 3);
  REQUIRE(sm3::drop_column(db.ss, t, "a") == 0);
  CHECK(entries(db, t) == 2);
  db.commit();

  db.begin();
  sm3::stats_t only_b = sample_stats(ix, 9000.0);
  only_b.stats.erase(only_b.stats.begin());
  REQUIRE(sm3::cat_put_stats(db.ss, t, only_b) == 0);
  CHECK(entries(db, t) == 2);
  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  CHECK(entries(db, t) == 0);
  db.commit();
}
