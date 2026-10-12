/*
 * test_temp.cc -- temporary tables (TEMPTABLE.md §4): tables in the
 * temporary namespace, defined in their session's memory alone.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>
#include <vector>

namespace {

/* A loaded catalog with two sessions, a and b, on their own sm1 sessions. */
class temp_two_t : public db_t {
public:
  /* Initialize and load the catalog; open a and b over it. */
  temp_two_t() {
    sa = open();
    sb = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, sa, a) == 0);
    REQUIRE(sm3::session_open(cat, sb, b) == 0);
  }

  /* Close both sessions and the catalog. */
  ~temp_two_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cat_close(cat);
  }

  sm3::catalog_t *cat = nullptr;
  sm1::session_t *sa = nullptr, *sb = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
};

/* Begin a transaction on s. */
void start(sm3::session_t *s,
           sm1::xctlevel_t level = sm1::xctlevel_t::read_committed,
           bool read_only = false) {
  REQUIRE_MESSAGE(sm3::begin(s, level, read_only) == 0, sm1::errmsg());
}

/* Commit s's transaction. */
void finish(sm3::session_t *s) {
  REQUIRE_MESSAGE(sm3::commit(s) == 0, sm1::errmsg());
}

/* Create the temporary table name (x int4) through s; its oid. */
uint64_t create(sm3::session_t *s, const char *name) {
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t oid = 0;
  REQUIRE_MESSAGE(sm3::create_temp_table(s, name, cols, oid) == 0,
                  sm1::errmsg());
  return oid;
}

/* Insert x into the table oid through s. */
void insert(sm3::session_t *s, uint64_t oid, int32_t x) {
  sm3::table_t *t = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IX, t) == 0,
                  sm1::errmsg());
  sm2::datum_t d = sm2::datum_from_int32(x);
  bool null = false;
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(
      sm3::tbl_insert(t, std::span(&d, 1), std::span(&null, 1), rowno) == 0,
      sm1::errmsg());
  sm3::tbl_close(t);
}

/* How many rows s sees in the table oid. */
size_t count(sm3::session_t *s, uint64_t oid) {
  sm3::table_t *t = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0,
                  sm1::errmsg());
  sm2::rel_scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
  uint64_t rowno;
  std::string_view tuple;
  size_t n = 0;
  int rc;
  while ((rc = sm2::rel_scannxt(scan, rowno, tuple)) == 0) {
    n++;
  }
  sm2::rel_scanend(scan);
  REQUIRE(rc == 1);
  sm3::tbl_close(t);
  return n;
}

/* How many entries s finds in the index of the table oid. */
size_t count_index(sm3::session_t *s, uint64_t oid, uint64_t index) {
  sm3::table_t *t = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0,
                  sm1::errmsg());
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, index, nullptr, nullptr, scan) == 0,
                  sm1::errmsg());
  uint64_t rowno;
  bool cut;
  std::string_view value;
  size_t n = 0;
  int rc;
  while ((rc = sm2::idx_scannxt(scan, rowno, cut, value)) == 0) {
    n++;
  }
  sm2::idx_scanend(scan);
  REQUIRE(rc == 1);
  sm3::tbl_close(t);
  return n;
}

} // namespace

TEST_CASE("a temporary table is in no namespace") {
  // Verify sm3 keeps no temporary namespace: the host names its own.
  // 1. A creates temporary t: its definition's namespace is nsp_temp.
  // 2. Confirm create_table refuses nsp_temp with SM_ERR_NOTFOUND.
  // 3. Confirm no namespace is named pg_temp, and one can be created.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t"), oid = 0;
  REQUIRE(sm3::cat_get_table(db.a, t));
  CHECK(sm3::cat_get_table(db.a, t)->nspoid == sm3::nsp_temp);
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  CHECK(sm3::create_table(db.a, sm3::nsp_temp, "u", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK_FALSE(sm3::cat_get_namespace(db.a, "pg_temp", oid));
  CHECK(sm3::create_namespace(db.a, "pg_temp", oid) == 0);
  finish(db.a);
}

TEST_CASE("a temporary table is its session's alone, commit after commit") {
  // Verify a temporary table is kept by its session and seen by no other.
  // 1. A creates temporary table t and inserts a row, and commits.
  // 2. In a new transaction A finds t by name and by oid, lists it, and
  //    counts one row.
  // 3. B finds no t by name or oid, lists none, and cannot open it.
  // 4. B creates its own temporary t: the two do not meet.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t");
  insert(db.a, t, 1);
  finish(db.a);

  start(db.a);
  REQUIRE(sm3::cat_get_table(db.a, sm3::nsp_temp, "t"));
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "t")->oid == t);
  CHECK(sm3::cat_get_table(db.a, t) != nullptr);
  CHECK(sm3::cat_list_tables(db.a, sm3::nsp_temp) ==
        std::vector<std::string>{"t"});
  CHECK(count(db.a, t) == 1);
  finish(db.a);
  CHECK(sm3::cat_get_table(db.cat, t) == nullptr);

  start(db.b);
  CHECK(sm3::cat_get_table(db.b, sm3::nsp_temp, "t") == nullptr);
  CHECK(sm3::cat_get_table(db.b, t) == nullptr);
  CHECK(sm3::cat_list_tables(db.b, sm3::nsp_temp).empty());
  sm3::table_t *tb = nullptr;
  CHECK(sm3::tbl_open(db.b, t, sm1::SM_LOCKMODE_IS, tb) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  uint64_t t2 = create(db.b, "t");
  CHECK(t2 != t);
  CHECK(count(db.b, t2) == 0);
  finish(db.b);

  start(db.a);
  CHECK(count(db.a, t) == 1);
  finish(db.a);
}

TEST_CASE("a temporary table writes nothing to the catalog's store") {
  // Verify a temporary table leaves store 0 and the lock table alone.
  // 1. In a read-only transaction, A creates temporary table t with an
  //    index, inserts a row, and commits.
  // 2. Confirm the commit succeeded, though the transaction was
  //    read-only: it wrote no catalog entry.
  // 3. Reload the catalog from the store: it holds no table t.
  temp_two_t db;
  start(db.a, sm1::xctlevel_t::read_committed, true);
  uint64_t t = create(db.a, "t"), ix = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE_MESSAGE(sm3::create_index(db.a, t, "t_i", keys, {}, true, ix) == 0,
                  sm1::errmsg());
  insert(db.a, t, 1);
  finish(db.a);

  sm3::catalog_t *again = nullptr;
  REQUIRE(sm3::cat_open(db.env(), "db", again) == 0);
  CHECK(sm3::cat_get_table(again, t) == nullptr);
  CHECK(sm3::cat_list_tables(again, sm3::nsp_temp).empty());
  sm3::cat_close(again);
}

TEST_CASE("temporary DDL follows the transaction") {
  // Verify making, dropping and renaming a temporary table can be undone.
  // 1. Create t and abort: it is not there.
  // 2. Create t under a savepoint and roll back: not there; create it
  //    again and commit: there.
  // 3. Drop t and abort: still there, with its row.
  // 4. Rename t to u and abort: still t. Rename and commit: u, not t.
  // 5. Drop u and commit: gone.
  temp_two_t db;
  start(db.a);
  create(db.a, "t");
  REQUIRE(sm3::abort(db.a) == 0);
  start(db.a);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "t") == nullptr);

  int sp = sm3::savepoint(db.a);
  create(db.a, "t");
  REQUIRE(sm3::rollback(db.a, sp) == 0);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "t") == nullptr);
  uint64_t t = create(db.a, "t");
  insert(db.a, t, 1);
  finish(db.a);

  start(db.a);
  REQUIRE_MESSAGE(sm3::drop_table(db.a, t) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_table(db.a, t) == nullptr);
  REQUIRE(sm3::abort(db.a) == 0);
  start(db.a);
  CHECK(count(db.a, t) == 1);

  REQUIRE_MESSAGE(sm3::rename_table(db.a, t, "u") == 0, sm1::errmsg());
  REQUIRE(sm3::abort(db.a) == 0);
  start(db.a);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "t") != nullptr);
  REQUIRE(sm3::rename_table(db.a, t, "u") == 0);
  finish(db.a);
  start(db.a);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "t") == nullptr);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "u") != nullptr);

  REQUIRE(sm3::drop_table(db.a, t) == 0);
  finish(db.a);
  start(db.a);
  CHECK(sm3::cat_get_table(db.a, sm3::nsp_temp, "u") == nullptr);
  CHECK(sm3::cat_get_table(db.a, t) == nullptr);
  finish(db.a);
}

TEST_CASE("a temporary name is taken once in its session") {
  // Verify the names of a session's temporary tables and indexes are
  // one name space, and are no one else's.
  // 1. A creates temporary t with index t_i, and commits.
  // 2. A second temporary t, a table named t_i, and an index named t
  //    each fail with SM_ERR_DUPKEY.
  // 3. A table t in public is another name: it succeeds.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t"), ix = 0, oid = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.a, t, "t_i", keys, {}, false, ix) == 0);
  finish(db.a);

  start(db.a);
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  CHECK(sm3::create_temp_table(db.a, "t", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_temp_table(db.a, "t_i", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_index(db.a, t, "t", keys, {}, false, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_table(db.a, sm3::nsp_public, "t", cols, oid) == 0);
  finish(db.a);
}

TEST_CASE("an index on a temporary table is built at once, in the session") {
  // Verify an index made on a temporary table that already has rows is
  // filled and usable without the builder.
  // 1. A creates temporary t, inserts three rows, and commits.
  // 2. In a later transaction A creates index t_i: it is valid at
  //    once, and a scan of it finds three entries.
  // 3. After commit it is still valid, a row inserted then is in it,
  //    and it is found by name.
  // 4. A drops the index: it is gone, and the rows stay.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t"), ix = 0;
  for (int32_t x : {1, 2, 3}) {
    insert(db.a, t, x);
  }
  finish(db.a);

  start(db.a);
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE_MESSAGE(sm3::create_index(db.a, t, "t_i", keys, {}, true, ix) == 0,
                  sm1::errmsg());
  CHECK(sm3::cat_get_index_state(db.a, ix).state == sm3::index_state_t::valid);
  CHECK(count_index(db.a, t, ix) == 3);
  finish(db.a);

  start(db.a);
  CHECK(sm3::cat_get_index_state(db.a, ix).state == sm3::index_state_t::valid);
  insert(db.a, t, 4);
  CHECK(count_index(db.a, t, ix) == 4);
  uint64_t table = 0, index = 0;
  CHECK(sm3::cat_get_index(db.a, sm3::nsp_temp, "t_i", table, index));
  CHECK(table == t);
  CHECK(index == ix);
  REQUIRE_MESSAGE(sm3::drop_index(db.a, t, ix) == 0, sm1::errmsg());
  finish(db.a);

  start(db.a);
  CHECK_FALSE(sm3::cat_get_index(db.a, sm3::nsp_temp, "t_i", table, index));
  CHECK(count(db.a, t) == 4);
  finish(db.a);
}

TEST_CASE("a temporary table can be truncated and altered") {
  // Verify the rest of the DDL on a temporary table.
  // 1. A creates temporary t with two rows, and commits.
  // 2. A truncates it and aborts: two rows still.
  // 3. A truncates it and commits: no rows; a new row gets a rowno
  //    above the old ones.
  // 4. A adds column y and drops column x: the definition has both
  //    changes after commit.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t");
  insert(db.a, t, 1);
  insert(db.a, t, 2);
  finish(db.a);

  start(db.a);
  REQUIRE_MESSAGE(sm3::truncate_table(db.a, t) == 0, sm1::errmsg());
  CHECK(count(db.a, t) == 0);
  REQUIRE(sm3::abort(db.a) == 0);
  start(db.a);
  CHECK(count(db.a, t) == 2);
  REQUIRE(sm3::truncate_table(db.a, t) == 0);
  finish(db.a);

  start(db.a);
  CHECK(count(db.a, t) == 0);
  sm3::table_t *h = nullptr;
  REQUIRE(sm3::tbl_open(db.a, t, sm1::SM_LOCKMODE_IX, h) == 0);
  sm2::datum_t d = sm2::datum_from_int32(9);
  bool null = false;
  uint64_t rowno = 0;
  REQUIRE(sm3::tbl_insert(h, std::span(&d, 1), std::span(&null, 1), rowno) ==
          0);
  CHECK(rowno > 2);
  sm3::tbl_close(h);
  REQUIRE_MESSAGE(sm3::add_column(db.a, t,
                                  {"y", sm2::type_int4.code, -1, false},
                                  std::nullopt) == 0,
                  sm1::errmsg());
  REQUIRE_MESSAGE(sm3::drop_column(db.a, t, "x") == 0, sm1::errmsg());
  finish(db.a);

  start(db.a);
  std::shared_ptr<const sm3::table_def_t> def = sm3::cat_get_table(db.a, t);
  REQUIRE(def);
  REQUIRE(def->columns.size() == 2);
  CHECK(def->columns[0].dropped);
  CHECK(def->columns[1].name == "y");
  finish(db.a);
}

TEST_CASE("what is not kept for a temporary table is refused") {
  // Verify the first cut's limits are errors, not silent gaps.
  // 1. A creates temporary t with a row.
  // 2. Analyzing it fails with SM_ERR_INVALID, and it has no statistics.
  // 3. A sequence in no namespace fails with SM_ERR_NOTFOUND.
  // 4. Preparing the transaction fails with SM_ERR_INVALID.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t"), oid = 0;
  insert(db.a, t, 1);
  sm3::table_t *h = nullptr;
  REQUIRE(sm3::tbl_open(db.a, t, sm1::SM_LOCKMODE_IS, h) == 0);
  CHECK(sm3::tbl_analyze(h, 10) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(h);
  std::shared_ptr<const sm3::stats_t> stats;
  CHECK(sm3::cat_get_stats(db.a, t, stats) == 0);
  CHECK(stats == nullptr);
  CHECK(sm3::create_sequence(db.a, sm3::nsp_temp, "q", 1, 1, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::prepare(db.a, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  finish(db.a);
}

TEST_CASE("closing its session ends a temporary table") {
  // Verify a temporary table goes with its session, store and all.
  // 1. A creates temporary t with a row, and commits.
  // 2. Close A's sm3 session and its sm1 session.
  // 3. A new session on the database finds no t, by name or by oid.
  temp_two_t db;
  start(db.a);
  uint64_t t = create(db.a, "t");
  insert(db.a, t, 1);
  finish(db.a);
  sm3::session_close(db.a);
  db.a = nullptr;
  REQUIRE(sm1::closedb(db.sa) == 0);

  start(db.b);
  CHECK(sm3::cat_get_table(db.b, sm3::nsp_temp, "t") == nullptr);
  CHECK(sm3::cat_get_table(db.b, t) == nullptr);
  finish(db.b);
}
