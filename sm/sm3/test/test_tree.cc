/*
 * test_tree.cc -- a table's index made a tree (../BTREEINDEX.md §11.3 step
 * 7): writes maintain it as any index, and its scan of rows hands out
 * each row, as the snapshot shows it, only while the row still has
 * its entry's key.
 */

#include "../src/table/pin.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A row of (id int4, state text, owner text) as a write takes it. */
struct row_t {
  row_t(int32_t id, std::optional<std::string> state, std::string owner) {
    m_state = sm2::make_varlena(state.value_or(""));
    m_owner = sm2::make_varlena(owner);
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_state.data());
    m_d[2] = sm2::datum_from_ptr(m_owner.data());
    m_null[0] = false;
    m_null[1] = !state;
    m_null[2] = false;
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_state, m_owner;
  sm2::datum_t m_d[3];
  bool m_null[3];
};

/*
 * The evaluator: "free" makes an index partial, on state = 'free';
 * "lower" gives it one expression column, lower(owner).
 */
int tree_eval(const sm3::table_def_t &, const sm3::index_def_t &index,
              std::span<const sm2::datum_t> d, std::span<const bool> n,
              bool &belongs, std::vector<std::string> &values,
              std::span<bool> isnull) {
  auto text = [&](size_t i) {
    std::string_view v;
    sm2::datum_bytes(&sm2::type_text, -1, d[i], v);
    return std::string(sm2::varlena_payload(v));
  };
  belongs = index.eval.find("free") == std::string::npos ||
            (!n[1] && text(1) == "free");
  if (index.eval.find("lower") != std::string::npos) {
    isnull[0] = n[2];
    if (!n[2]) {
      std::string o = text(2);
      for (char &c : o) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      values[0] = sm2::make_varlena(o);
    }
  }
  return 0;
}

/*
 * A catalog with jobs (id, state, owner), its trees by_state on
 * (state), and by_owner on lower(owner) where state = 'free'; and
 * sessions a and b.
 */
class trees_t : public db_t {
public:
  /* Make the catalog, the table and its trees. */
  trees_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    open_catalog();
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"state", sm2::type_text.code, -1, false},
        {"owner", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> state = {
        {1, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> owner = {
        {-1, false, false, std::nullopt, sm2::type_text.code, -1}};
    std::vector<int32_t> reads = {1, 2};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "jobs", cols, jobs) == 0);
    REQUIRE_MESSAGE(sm3::create_index(a, jobs, "by_state", state, {}, false,
                                      by_state, false, false, true) == 0,
                    sm1::errmsg());
    REQUIRE_MESSAGE(sm3::create_index(a, jobs, "by_owner", owner, {}, false,
                                      "free lower", reads, by_owner, false,
                                      false, true) == 0,
                    sm1::errmsg());
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~trees_t() { close_catalog(); }

  /* Open the catalog and sessions a and b, with the evaluator. */
  void open_catalog() {
    sm1::session_t *s = open();
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    REQUIRE(sm3::cat_set_evaluator(cat, tree_eval) == 0);
  }

  /* Close the sessions and the catalog. */
  void close_catalog() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cleaner_detach(cat);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }

  /* Attach the cleaner, with workers threads running it. */
  void cleaner(size_t workers = 0) {
    REQUIRE(
        sm3::cleaner_attach(cat, [this] { return try_open(); }, workers) == 0);
  }

  /* Attach the catalog to the builder, and queue what is pending. */
  void builder(size_t max_workers, size_t batch_rows) {
    sm3::builder_set_max_workers(max_workers);
    REQUIRE(sm3::builder_attach(
                cat, [this] { return try_open(); }, batch_rows) == 0);
    REQUIRE(sm3::enqueue_pending_indexes(cat) == 0);
  }

  /* jobs, opened in s's transaction; it must open. */
  sm3::table_t *table(sm3::session_t *s) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, jobs, sm1::SM_LOCKMODE_IX, t) == 0,
                    sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t jobs = 0, by_state = 0, by_owner = 0;
};

/*
 * The ids of the rows a scan of index gives, from text lo to hi, both
 * inclusive, either empty for none, in the order given.
 */
std::vector<int32_t> ids(sm3::table_t *t, uint64_t index, std::string lo = "",
                         std::string hi = "") {
  std::string lv = sm2::make_varlena(lo), hv = sm2::make_varlena(hi);
  sm2::datum_t ld[1] = {sm2::datum_from_ptr(lv.data())};
  sm2::datum_t hd[1] = {sm2::datum_from_ptr(hv.data())};
  bool nn[1] = {false};
  sm2::bound_t l{ld, nn, true}, h{hd, nn, true};
  sm3::scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan_rows(t, index, lo.empty() ? nullptr : &l,
                                         hi.empty() ? nullptr : &h, scan) == 0,
                  sm1::errmsg());
  std::vector<int32_t> out;
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    sm2::datum_t d;
    bool isnull;
    REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 0, d, isnull) == 0);
    out.push_back(sm2::datum_to_int32(d));
  }
  CHECK(1 == rc);
  sm3::tbl_scanend(scan);
  return out;
}

} // namespace

TEST_CASE("tree: a scan gives each row while it has its entry's key") {
  // Verify a tree's scan of rows against the writes that change them.
  // 1. Insert rows of states a, b and c; a scan of b gives b's rows.
  // 2. Update one from b to c, one from b to a and back to b, and
  //    delete one: the scans of a, b and c each give the rows that
  //    have that state now, once each.
  // 3. An insert rolled back leaves an entry no scan gives.
  // 4. The catalog reloaded still has the trees, and scans the same.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t id[7];
  const char *states[] = {"a", "b", "b", "b", "b", "c", "c"};
  for (int i = 0; i < 7; i++) {
    row_t r(i, states[i], "Own" + std::to_string(i));
    REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0,
                    sm1::errmsg());
  }
  CHECK(std::vector<int32_t>{1, 2, 3, 4} == ids(t, f.by_state, "b", "b"));
  row_t to_c(1, "c", "Own1"), to_a(2, "a", "Own2"), back(2, "b", "Own2");
  REQUIRE(sm3::tbl_update(t, id[1], to_c.d(), to_c.null()) == 0);
  REQUIRE(sm3::tbl_update(t, id[2], to_a.d(), to_a.null()) == 0);
  REQUIRE(sm3::tbl_update(t, id[2], back.d(), back.null()) == 0);
  REQUIRE(sm3::tbl_delete(t, id[3]) == 0);
  CHECK(std::vector<int32_t>{0} == ids(t, f.by_state, "a", "a"));
  CHECK(std::vector<int32_t>{2, 4} == ids(t, f.by_state, "b", "b"));
  CHECK(std::vector<int32_t>{1, 5, 6} == ids(t, f.by_state, "c", "c"));
  CHECK(std::vector<int32_t>{0, 2, 4, 1, 5, 6} == ids(t, f.by_state));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t gone(9, "b", "Own9");
  uint64_t gid;
  REQUIRE(sm3::tbl_insert(t, gone.d(), gone.null(), gid) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  f.close_catalog();
  f.restart();
  f.open_catalog();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  for (const sm3::index_def_t &x : sm3::tbl_def(t).indexes) {
    CHECK(x.tree);
  }
  CHECK(std::vector<int32_t>{2, 4} == ids(t, f.by_state, "b", "b"));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a partial index of an expression is rechecked by both") {
  // Verify the recheck asks the evaluator, as a write does.
  // 1. Insert rows free and not, owners in mixed case.
  // 2. The tree on lower(owner) where state = 'free' gives the free
  //    rows by their lowered owner.
  // 3. A row no longer free, and one whose owner changed, are no
  //    longer given under their old entries.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t id[4];
  row_t r0(0, "free", "Bob"), r1(1, "busy", "Ann"), r2(2, "free", "ann"),
      r3(3, "free", "Cy");
  const row_t *rs[] = {&r0, &r1, &r2, &r3};
  for (int i = 0; i < 4; i++) {
    REQUIRE(sm3::tbl_insert(t, rs[i]->d(), rs[i]->null(), id[i]) == 0);
  }
  CHECK(std::vector<int32_t>{2, 0, 3} == ids(t, f.by_owner));
  row_t busy(0, "busy", "Bob"), renamed(3, "free", "Al");
  REQUIRE(sm3::tbl_update(t, id[0], busy.d(), busy.null()) == 0);
  REQUIRE(sm3::tbl_update(t, id[3], renamed.d(), renamed.null()) == 0);
  CHECK(std::vector<int32_t>{3, 2} == ids(t, f.by_owner));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: an older snapshot reads the rows it sees") {
  // Verify the row decides, at each reader's snapshot.
  // 1. Insert a row of state x, committed.
  // 2. b begins at repeatable read and scans x: the row.
  // 3. a moves the row to y and commits.
  // 4. b scans x: still the row; y: none, its entry naming a row b
  //    sees with x. A new transaction of b's sees it under y alone.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t x(1, "x", "O");
  uint64_t rid;
  REQUIRE(sm3::tbl_insert(t, x.d(), x.null(), rid) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *tb = f.table(f.b);
  CHECK(std::vector<int32_t>{1} == ids(tb, f.by_state, "x", "x"));

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t y(1, "y", "O");
  REQUIRE(sm3::tbl_update(t, rid, y.d(), y.null()) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  CHECK(std::vector<int32_t>{1} == ids(tb, f.by_state, "x", "x"));
  CHECK(ids(tb, f.by_state, "y", "y").empty());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  tb = f.table(f.b);
  CHECK(ids(tb, f.by_state, "x", "x").empty());
  CHECK(std::vector<int32_t>{1} == ids(tb, f.by_state, "y", "y"));
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("tree: long keys split its pages under the table's writes") {
  // Verify a tree of many levels through a table's inserts and updates.
  // 1. Insert 500 rows with keys of 1,006 bytes, about three a page,
  //    out of key order, so pages split at every level.
  // 2. A scan gives every row in key order; a probe gives its row.
  // 3. Update every tenth row to a key past them all: its old key gives
  //    no row, its new one the row, and a scan each row once.
  trees_t f;
  auto key_of = [](int n) {
    return std::to_string(100000 + n) + std::string(1000, 'k');
  };
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> rids(500);
  for (int i = 0; i < 500; i++) {
    int n = i * 7919 % 500;
    row_t r(n, key_of(n), "O");
    REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d(), r.null(), rids[n]) == 0,
                    sm1::errmsg());
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  std::vector<int32_t> want(500);
  for (int n = 0; n < 500; n++) {
    want[n] = n;
  }
  CHECK(want == ids(t, f.by_state));
  for (int n = 0; n < 500; n += 17) {
    CHECK(std::vector<int32_t>{n} == ids(t, f.by_state, key_of(n), key_of(n)));
  }
  for (int n = 0; n < 500; n += 10) {
    row_t r(n, key_of(1000 + n), "O");
    REQUIRE_MESSAGE(sm3::tbl_update(t, rids[n], r.d(), r.null()) == 0,
                    sm1::errmsg());
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(ids(t, f.by_state, key_of(0), key_of(0)).empty());
  CHECK(std::vector<int32_t>{0} ==
        ids(t, f.by_state, key_of(1000), key_of(1000)));
  std::vector<int32_t> all = ids(t, f.by_state);
  std::sort(all.begin(), all.end());
  CHECK(want == all);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: made on rows, emptied by a truncate, refused as a pkey") {
  // Verify the trees DDL makes, empties and refuses.
  // 1. A tree made on a table with rows, in a later transaction, is
  //    pending, and filled by the build: a scan gives every row.
  // 2. TRUNCATE gives the table new, empty trees: a scan gives none,
  //    and rows inserted after are given.
  // 3. A tree that is a primary key is refused.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 50; i++) {
    row_t r(i, "s" + std::to_string(i % 5), "O");
    uint64_t rid;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), rid) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::vector<sm3::index_key_spec_t> by_id = {{0, false, false, std::nullopt}};
  uint64_t tid = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::create_index(f.a, f.jobs, "by_id", by_id, {}, false, tid,
                                    false, false, true) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  f.builder(1, 7);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, tid) == 0, sm1::errmsg());
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(50u == ids(t, tid).size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::truncate_table(f.a, f.jobs) == 0);
  t = f.table(f.a);
  CHECK(ids(t, tid).empty());
  CHECK(ids(t, f.by_state).empty());
  row_t r(7, "s1", "O");
  uint64_t rid;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), rid) == 0);
  CHECK(std::vector<int32_t>{7} == ids(t, f.by_state, "s1", "s1"));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  uint64_t u = 0;
  CHECK(-1 == sm3::create_index(f.a, f.jobs, "u", by_id, {}, true, u, true,
                                false, true));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  REQUIRE(sm3::abort(f.a) == 0);
}

namespace {

/* How many entries a tree holds, dead or not, by a scan of entries. */
size_t entries_of(sm3::table_t *t, uint64_t index) {
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, index, nullptr, nullptr, scan) == 0,
                  sm1::errmsg());
  size_t n = 0;
  uint64_t id;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    n++;
  }
  sm2::idx_scanend(scan);
  return n;
}

} // namespace

TEST_CASE("tree: vacuum removes the entries no snapshot needs") {
  // Verify VACUUM's walk of a tree removes only settled dead entries.
  // 1. Insert 20 rows; move 10 to another state, delete 5, and roll
  //    back an insert: the tree holds 31 entries, for 15 live rows.
  // 2. With b's repeatable read snapshot from before the changes still
  //    open, vacuum removes only the rolled-back insert's entry: the
  //    others' rows b may still see with their old keys.
  // 3. Once b ends, vacuum leaves one entry a row: 15.
  // 4. The scans still give every live row once.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t id[20];
  for (int i = 0; i < 20; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *tb = f.table(f.b);
  CHECK(20u == ids(tb, f.by_state, "old", "old").size());

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  for (int i = 0; i < 10; i++) {
    row_t r(i, "new", "O");
    REQUIRE(sm3::tbl_update(t, id[i], r.d(), r.null()) == 0);
  }
  for (int i = 10; i < 15; i++) {
    REQUIRE(sm3::tbl_delete(t, id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t gone(99, "old", "O");
  uint64_t gid;
  REQUIRE(sm3::tbl_insert(t, gone.d(), gone.null(), gid) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(31u == entries_of(t, f.by_state));
  REQUIRE_MESSAGE(sm3::tbl_compact(t) == 0, sm1::errmsg());
  CHECK(30u == entries_of(t, f.by_state));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  CHECK(20u == ids(tb, f.by_state, "old", "old").size());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_compact(t) == 0);
  CHECK(15u == entries_of(t, f.by_state));
  CHECK(10u == ids(t, f.by_state, "new", "new").size());
  CHECK(5u == ids(t, f.by_state, "old", "old").size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a write skew through a tree's ranges fails one") {
  // Verify a serializable scan of a tree reads its range for SSI.
  // 1. a and b, serializable, each scan the rows of state "on": none.
  // 2. Each inserts a row of state "on", into the range the other
  //    read: a write skew no row read alone shows.
  // 3. a commits; b fails with SM_ERR_SERIALIZATION.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::serializable) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::serializable) == 0);
  sm3::table_t *ta = f.table(f.a);
  sm3::table_t *tb = f.table(f.b);
  CHECK(ids(ta, f.by_state, "on", "on").empty());
  CHECK(ids(tb, f.by_state, "on", "on").empty());
  row_t ra(1, "on", "A"), rb(2, "on", "B");
  uint64_t ia, ib;
  REQUIRE(sm3::tbl_insert(ta, ra.d(), ra.null(), ia) == 0);
  REQUIRE(sm3::tbl_insert(tb, rb.d(), rb.null(), ib) == 0);
  sm3::tbl_close(ta);
  sm3::tbl_close(tb);
  CHECK(0 == sm3::commit(f.a));
  CHECK(-1 == sm3::commit(f.b));
  CHECK(sm1::SM_ERR_SERIALIZATION == sm1::errnum());
}

TEST_CASE("tree: a unique tree refuses a key a live row has") {
  // Verify a unique tree's check, by the rows its entries name.
  // 1. Make a unique tree on owner; insert rows of owners x and y.
  // 2. Another x is refused with SM_ERR_DUPKEY, the tree's index in
  //    errdup; so is an update of y's row to x.
  // 3. Delete x's row, then insert x again: taken, its old entry dead.
  // 4. Move a row from y to z and back to y in one transaction: its
  //    own entries are no other row's.
  // 5. Null owners are many.
  trees_t f;
  std::vector<sm3::index_key_spec_t> owner = {{2, false, false, std::nullopt}};
  uint64_t u = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::create_index(f.a, f.jobs, "u_owner", owner, {}, true, u,
                                    false, false, true) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  f.builder(1, 10);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, u) == 0, sm1::errmsg());

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t x(1, "s", "x"), y(2, "s", "y");
  uint64_t ix, iy;
  REQUIRE(sm3::tbl_insert(t, x.d(), x.null(), ix) == 0);
  REQUIRE(sm3::tbl_insert(t, y.d(), y.null(), iy) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t x2(3, "s", "x");
  uint64_t i2;
  CHECK(-1 == sm3::tbl_insert(t, x2.d(), x2.null(), i2));
  CHECK(sm1::SM_ERR_DUPKEY == sm1::errnum());
  CHECK(u == sm1::errdup().index);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t y_to_x(2, "s", "x");
  CHECK(-1 == sm3::tbl_update(t, iy, y_to_x.d(), y_to_x.null()));
  CHECK(sm1::SM_ERR_DUPKEY == sm1::errnum());
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_delete(t, ix) == 0);
  REQUIRE_MESSAGE(sm3::tbl_insert(t, x2.d(), x2.null(), i2) == 0,
                  sm1::errmsg());
  row_t y_to_z(2, "s", "z");
  REQUIRE(sm3::tbl_update(t, iy, y_to_z.d(), y_to_z.null()) == 0);
  REQUIRE_MESSAGE(sm3::tbl_update(t, iy, y.d(), y.null()) == 0, sm1::errmsg());
  for (int n = 10; n < 13; n++) {
    row_t nul(n, "s", "");
    sm2::datum_t d[3] = {nul.d()[0], nul.d()[1], 0};
    bool isnull[3] = {false, false, true};
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, d, isnull, id) == 0);
  }
  CHECK(std::vector<int32_t>{3} == ids(t, u, "x", "x"));
  CHECK(std::vector<int32_t>{2} == ids(t, u, "y", "y"));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a unique tree waits for the writer of an equal key") {
  // Verify the check waits for a running writer, then decides by its
  // end, as sm1's unique keys do.
  // 1. a inserts owner w and holds its transaction open.
  // 2. b inserts owner w on another thread: it waits.
  // 3. a aborts: b's insert is taken. Again with a committing: b's
  //    fails with SM_ERR_DUPKEY.
  for (bool commit : {false, true}) {
    CAPTURE(commit);
    trees_t f;
    std::vector<sm3::index_key_spec_t> owner = {
        {2, false, false, std::nullopt}};
    uint64_t u = 0;
    REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_index(f.a, f.jobs, "u_owner", owner, {}, true, u, false,
                              false, true) == 0);
    REQUIRE(sm3::commit(f.a) == 0);
    f.builder(1, 10);
    REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, u) == 0, sm1::errmsg());

    REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
    sm3::table_t *ta = f.table(f.a);
    row_t wa(1, "s", "w");
    uint64_t ia;
    REQUIRE(sm3::tbl_insert(ta, wa.d(), wa.null(), ia) == 0);

    std::atomic<int> result{99};
    std::atomic<int> err{0};
    std::atomic<bool> started{false};
    std::thread bt([&] {
      sm3::begin(f.b, sm1::xctlevel_t::read_committed);
      sm3::table_t *tb = f.table(f.b);
      row_t wb(2, "s", "w");
      uint64_t ib;
      started = true;
      int rc = sm3::tbl_insert(tb, wb.d(), wb.null(), ib);
      err = rc == 0 ? 0 : static_cast<int>(sm1::errnum());
      sm3::tbl_close(tb);
      result = rc;
      if (rc == 0) {
        sm3::commit(f.b);
      } else {
        sm3::abort(f.b);
      }
    });
    while (!started) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(99 == result.load());
    sm3::tbl_close(ta);
    REQUIRE((commit ? sm3::commit(f.a) : sm3::abort(f.a)) == 0);
    bt.join();
    if (commit) {
      CHECK(-1 == result.load());
      CHECK(sm1::SM_ERR_DUPKEY == err.load());
    } else {
      CHECK(0 == result.load());
    }
  }
}

TEST_CASE("tree: a unique tree is an ON CONFLICT arbiter") {
  // Verify tbl_insert_arbiter with a unique tree as its arbiter.
  // 1. Insert owner x; an arbiter insert of x again says 1, the row
  //    found with its tuple, and inserts nothing.
  // 2. One of owner q inserts, 0, and is given by the tree.
  // 3. Delete x's row: an arbiter insert of x then inserts.
  trees_t f;
  std::vector<sm3::index_key_spec_t> owner = {{2, false, false, std::nullopt}};
  uint64_t u = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_index(f.a, f.jobs, "u_owner", owner, {}, true, u, false,
                            false, true) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  f.builder(1, 10);
  REQUIRE(sm3::builder_wait(f.a, f.jobs, u) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t x(1, "s", "x");
  uint64_t ix;
  REQUIRE(sm3::tbl_insert(t, x.d(), x.null(), ix) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  std::vector<uint64_t> arb = {u};
  row_t x2(2, "s", "x"), q(3, "s", "q");
  uint64_t id = 0;
  std::string tuple;
  CHECK(1 == sm3::tbl_insert_arbiter(t, x2.d(), x2.null(), arb, id, &tuple));
  CHECK(ix == id);
  sm2::datum_t d;
  bool isnull;
  REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 0, d, isnull) == 0);
  CHECK(1 == sm2::datum_to_int32(d));
  CHECK(0 == sm3::tbl_insert_arbiter(t, q.d(), q.null(), arb, id, &tuple));
  CHECK(std::vector<int32_t>{3} == ids(t, u, "q", "q"));
  REQUIRE(sm3::tbl_delete(t, ix) == 0);
  CHECK(0 == sm3::tbl_insert_arbiter(t, x2.d(), x2.null(), arb, id, &tuple));
  CHECK(std::vector<int32_t>{2} == ids(t, u, "x", "x"));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: its size is its pages'") {
  // Verify tbl_idx_size counts a tree's pages, for the planner.
  // 1. A tree of a table with no rows has its empty root alone.
  // 2. Insert 2,000 rows, and compact the store: its pages are in
  //    files, whose sizes are counted, not sampled as a memtable's
  //    are. The tree's bytes are more than none, and than 2,000
  //    entries' worth.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t disk = 0, mem = 0;
  REQUIRE(sm3::tbl_idx_size(t, f.by_state, disk, mem) == 0);
  uint64_t empty = disk + mem;
  for (int i = 0; i < 2000; i++) {
    row_t r(i, "state" + std::to_string(i), "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  REQUIRE(sm3::tbl_compact(t) == 0);
  REQUIRE(sm3::tbl_idx_size(t, f.by_state, disk, mem) == 0);
  CHECK(disk + mem > empty);
  CHECK(disk > 2000u * 10);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: scans agree with the rows under writers moving them") {
  // Verify a tree's scan against the table at one snapshot, while
  // writers move rows between keys, insert and delete them.
  // 1. Insert 400 rows of states s0..s9.
  // 2. Three writers, at read committed, each change 20 rows a
  //    transaction: a new state, a delete, or an insert; they commit,
  //    or now and then abort.
  // 3. Two readers, at repeatable read, scan the tree for a state and
  //    the whole table, at one snapshot: the ids agree, each once.
  // 4. At the end, and after vacuum, the same at a new snapshot.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 400; i++) {
    row_t r(i, "s" + std::to_string(i % 10), "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  // The ids of the rows of state s, by a scan of the whole table.
  auto by_table = [](sm3::table_t *t, const std::string &s) {
    std::vector<int32_t> out;
    sm3::scan_t *scan = nullptr;
    REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
    uint64_t id;
    std::string_view tuple;
    while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
      sm2::datum_t d[2];
      bool n[2];
      REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 0, d[0], n[0]) == 0);
      REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 1, d[1], n[1]) == 0);
      std::string_view v;
      REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], v) == 0);
      if (!n[1] && sm2::varlena_payload(v) == s) {
        out.push_back(sm2::datum_to_int32(d[0]));
      }
    }
    sm3::tbl_scanend(scan);
    std::sort(out.begin(), out.end());
    return out;
  };
  // The same ids, by a scan of the tree.
  auto by_tree = [&](sm3::table_t *t, const std::string &s) {
    std::vector<int32_t> out = ids(t, f.by_state, s, s);
    std::sort(out.begin(), out.end());
    return out;
  };

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0}, disagree{0}, commits{0}, scans{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 3; w++) {
    threads.emplace_back([&, w] {
      sm3::session_t *s = nullptr;
      if (sm3::session_open(f.cat, f.open(), s) != 0) {
        errors++;
        return;
      }
      std::mt19937_64 rng(w + 7);
      int next = 1000 * (w + 1);
      while (!stop) {
        if (sm3::begin(s, sm1::xctlevel_t::read_committed) != 0) {
          errors++;
          break;
        }
        sm3::table_t *tw = nullptr;
        bool ok = sm3::tbl_open(s, f.jobs, sm1::SM_LOCKMODE_IX, tw) == 0;
        for (int k = 0; ok && k < 20; k++) {
          uint64_t rowno = 1 + rng() % 400;
          int what = rng() % 10;
          if (what < 7) {
            row_t r(static_cast<int32_t>(rowno - 1),
                    "s" + std::to_string(rng() % 10), "O");
            int rc = sm3::tbl_update(tw, rowno, r.d(), r.null());
            // A row deleted, or changed by another first: let be.
            ok = rc == 0 || rc == 1 || sm1::errnum() == sm1::SM_ERR_UPDATED ||
                 sm1::errnum() == sm1::SM_ERR_NOTFOUND;
          } else if (what < 8) {
            int rc = sm3::tbl_delete(tw, rowno);
            ok = rc == 0 || rc == 1 || sm1::errnum() == sm1::SM_ERR_UPDATED ||
                 sm1::errnum() == sm1::SM_ERR_NOTFOUND;
          } else {
            row_t r(next++, "s" + std::to_string(rng() % 10), "O");
            uint64_t id;
            ok = sm3::tbl_insert(tw, r.d(), r.null(), id) == 0;
          }
          if (!ok && sm1::errnum() != sm1::SM_ERR_DEADLOCK &&
              sm1::errnum() != sm1::SM_ERR_UPDATED &&
              sm1::errnum() != sm1::SM_ERR_NOTFOUND) {
            errors++;
          }
        }
        sm3::tbl_close(tw);
        if (ok && rng() % 5 != 0 && sm3::commit(s) == 0) {
          commits++;
        } else {
          sm3::abort(s);
        }
      }
      sm3::session_close(s);
    });
  }
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&, r] {
      sm3::session_t *s = nullptr;
      if (sm3::session_open(f.cat, f.open(), s) != 0) {
        errors++;
        return;
      }
      std::mt19937_64 rng(r + 99);
      for (int round = 0; round < 300; round++) {
        if (sm3::begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
          errors++;
          break;
        }
        sm3::table_t *tr = nullptr;
        if (sm3::tbl_open(s, f.jobs, sm1::SM_LOCKMODE_IS, tr) != 0) {
          errors++;
          sm3::abort(s);
          break;
        }
        std::string st = "s" + std::to_string(rng() % 10);
        if (by_tree(tr, st) != by_table(tr, st)) {
          disagree++;
        }
        scans++;
        sm3::tbl_close(tr);
        sm3::commit(s);
      }
      sm3::session_close(s);
    });
  }
  for (size_t i = 3; i < threads.size(); i++) {
    threads[i].join();
  }
  stop = true;
  for (size_t i = 0; i < 3; i++) {
    threads[i].join();
  }
  CHECK(0 == errors.load());
  CHECK(0 == disagree.load());
  MESSAGE("writers' commits: " << commits.load());
  CHECK(commits.load() > 20);
  CHECK(600 == scans.load());
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  t = f.table(f.a);
  for (int i = 0; i < 10; i++) {
    std::string st = "s" + std::to_string(i);
    CHECK(by_tree(t, st) == by_table(t, st));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_compact(t) == 0);
  for (int i = 0; i < 10; i++) {
    std::string st = "s" + std::to_string(i);
    CHECK(by_tree(t, st) == by_table(t, st));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a unique tree keeps one live row a key under writers") {
  // Verify a unique tree's check under concurrent writers.
  // 1. Make a unique tree on owner.
  // 2. Four writers insert rows of 300 owners at random, half of them
  //    ON CONFLICT DO NOTHING, delete some and move some to another
  //    owner, committing or aborting; a duplicate fails its
  //    transaction, a deadlock too.
  // 3. At the end, at one snapshot, no owner has two rows, and the
  //    tree gives each owner's row, once.
  trees_t f;
  std::vector<sm3::index_key_spec_t> owner = {{2, false, false, std::nullopt}};
  uint64_t u = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_index(f.a, f.jobs, "u_owner", owner, {}, true, u, false,
                            false, true) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  f.builder(1, 10);
  REQUIRE(sm3::builder_wait(f.a, f.jobs, u) == 0);

  std::atomic<int> errors{0}, commits{0}, dups{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 4; w++) {
    threads.emplace_back([&, w] {
      sm3::session_t *s = nullptr;
      if (sm3::session_open(f.cat, f.open(), s) != 0) {
        errors++;
        return;
      }
      std::mt19937_64 rng(w + 31);
      std::vector<uint64_t> mine;
      for (int round = 0; round < 300; round++) {
        if (sm3::begin(s, sm1::xctlevel_t::read_committed) != 0) {
          errors++;
          break;
        }
        sm3::table_t *tw = nullptr;
        bool ok = sm3::tbl_open(s, f.jobs, sm1::SM_LOCKMODE_IX, tw) == 0;
        std::vector<uint64_t> added;
        for (int k = 0; ok && k < 3; k++) {
          std::string o = "o" + std::to_string(rng() % 300);
          int what = rng() % 6;
          if (what < 2 || mine.empty()) {
            row_t r(w * 10000 + round * 10 + k, "s", o);
            uint64_t id;
            ok = sm3::tbl_insert(tw, r.d(), r.null(), id) == 0;
            if (ok) {
              added.push_back(id);
            }
          } else if (what < 4) {
            // ON CONFLICT DO NOTHING: 1 is a row with the key.
            row_t r(w * 10000 + round * 10 + k, "s", o);
            uint64_t id;
            std::vector<uint64_t> arb = {u};
            int rc = sm3::tbl_insert_arbiter(tw, r.d(), r.null(), arb, id);
            ok = rc >= 0;
            if (rc == 0) {
              added.push_back(id);
            }
          } else if (what < 5) {
            ok = sm3::tbl_delete(tw, mine[rng() % mine.size()]) == 0;
          } else {
            row_t r(w, "s", o);
            ok = sm3::tbl_update(tw, mine[rng() % mine.size()], r.d(),
                                 r.null()) == 0;
          }
          if (!ok) {
            int e = static_cast<int>(sm1::errnum());
            if (e == sm1::SM_ERR_DUPKEY) {
              dups++;
            } else if (e != sm1::SM_ERR_DEADLOCK && e != sm1::SM_ERR_NOTFOUND &&
                       e != sm1::SM_ERR_UPDATED) {
              errors++;
            }
          }
        }
        sm3::tbl_close(tw);
        if (ok && rng() % 4 != 0 && sm3::commit(s) == 0) {
          commits++;
          mine.insert(mine.end(), added.begin(), added.end());
        } else {
          sm3::abort(s);
        }
      }
      sm3::session_close(s);
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  CHECK(0 == errors.load());
  MESSAGE("commits " << commits.load() << ", duplicates " << dups.load());
  CHECK(dups.load() > 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *t = f.table(f.a);
  std::map<std::string, int> seen;
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
    sm2::datum_t d;
    bool n;
    REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 2, d, n) == 0);
    std::string_view v;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d, v) == 0);
    seen[std::string(sm2::varlena_payload(v))]++;
  }
  sm3::tbl_scanend(scan);
  for (const auto &[o, count] : seen) {
    CAPTURE(o);
    CHECK(1 == count);
    CHECK(1u == ids(t, u, o, o).size());
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a cursor of rows aimed again gives each probe's rows") {
  // Verify tbl_scan_rebound, an IN list's probes on one cursor.
  // 1. Insert rows of states a to e.
  // 2. Open a tree's cursor on c; read part of it; aim it at a, then
  //    at e, then at b again, then at a state with no rows: each
  //    gives that state's rows, whole.
  // 3. A cursor not a tree's is refused.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 100; i++) {
    row_t r(i, std::string(1, static_cast<char>('a' + i % 5)), "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  auto bound = [](std::string &v, const std::string &s, sm2::datum_t *d,
                  bool *nn) {
    v = sm2::make_varlena(s);
    d[0] = sm2::datum_from_ptr(v.data());
    nn[0] = false;
    return sm2::bound_t{std::span(d, 1), std::span(nn, 1), true};
  };
  auto drain_ids = [&](sm3::scan_t *scan) {
    std::vector<int32_t> out;
    uint64_t id;
    std::string_view tuple;
    while (sm3::tbl_scannxt(scan, id, tuple) == 0) {
      sm2::datum_t d;
      bool n;
      REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 0, d, n) == 0);
      out.push_back(sm2::datum_to_int32(d));
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  auto want = [](int k) {
    std::vector<int32_t> out;
    for (int i = k; i < 100; i += 5) {
      out.push_back(i);
    }
    return out;
  };
  std::string v;
  sm2::datum_t d[1];
  bool nn[1];
  sm2::bound_t b = bound(v, "c", d, nn);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan_rows(t, f.by_state, &b, &b, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  REQUIRE(sm3::tbl_scannxt(scan, id, tuple) == 0);
  for (auto [s, k] :
       std::vector<std::pair<std::string, int>>{{"a", 0}, {"e", 4}, {"b", 1}}) {
    sm2::bound_t nb = bound(v, s, d, nn);
    REQUIRE(sm3::tbl_scan_rebound(scan, &nb, &nb) == 0);
    CHECK(want(k) == drain_ids(scan));
  }
  sm2::bound_t none = bound(v, "z", d, nn);
  REQUIRE(sm3::tbl_scan_rebound(scan, &none, &none) == 0);
  CHECK(drain_ids(scan).empty());
  sm3::tbl_scanend(scan);
  sm3::scan_t *rows = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, rows) == 0);
  CHECK(-1 == sm3::tbl_scan_rebound(rows, &b, &b));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  sm3::tbl_scanend(rows);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a cursor of rows goes on past a rollback to a savepoint") {
  // Verify a rollback to a savepoint, with a tree's cursor of rows
  // open, leaves the cursor to give the rest of its rows: a tree's
  // cursor has no rel scan for the rollback to close.
  // 1. Insert rows of states a to e, and commit.
  // 2. Open a tree's cursor on c and read 3 rows.
  // 3. Set a savepoint, insert a row of c, and roll back to it.
  // 4. The cursor gives the rest of c's rows, the rolled-back row
  //    not among them.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 100; i++) {
    row_t r(i, std::string(1, static_cast<char>('a' + i % 5)), "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  std::string v = sm2::make_varlena("c");
  sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
  bool nn[1] = {false};
  sm2::bound_t b{d, nn, true};
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan_rows(t, f.by_state, &b, &b, scan) == 0);
  std::vector<int32_t> got;
  uint64_t id;
  std::string_view tuple;
  auto next = [&]() {
    sm2::datum_t col;
    bool n;
    REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 0, col, n) == 0);
    got.push_back(sm2::datum_to_int32(col));
  };
  for (int i = 0; i < 3; i++) {
    REQUIRE(sm3::tbl_scannxt(scan, id, tuple) == 0);
    next();
  }

  REQUIRE(sm3::savepoint(f.a) == 1);
  row_t r(1000, std::string("c"), "O");
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  REQUIRE_MESSAGE(sm3::rollback(f.a, 1) == 0, sm1::errmsg());

  int rc;
  while (0 == (rc = sm3::tbl_scannxt(scan, id, tuple))) {
    next();
  }
  CHECK(1 == rc);
  std::sort(got.begin(), got.end());
  std::vector<int32_t> want;
  for (int i = 2; i < 100; i += 5) {
    want.push_back(i);
  }
  CHECK(want == got);
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a cursor of rows skips the iterator when ANALYZE found "
          "them scattered") {
  // Verify a tree's cursor reads its batches of rows without a RocksDB
  // iterator when the statistics give its leading key a correlation
  // near 0 with the rows' order, and with one otherwise, whatever the
  // rows' own order (../ROWSTORE.md §7.2).
  // 1. Insert 200 rows, 20 of each state, each state's rows together.
  // 2. With no statistics, a cursor on one state keeps its iterator:
  //    the rows are close.
  // 3. With statistics giving state a correlation of 0.9, or of -0.9,
  //    it keeps it.
  // 4. With a correlation of -0.1, it makes none.
  // 5. Each cursor gives the state's 20 rows.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  char buf[8];
  for (int i = 0; i < 200; i++) {
    snprintf(buf, sizeof buf, "s%02d", i / 20);
    row_t r(i, std::string(buf), "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  // The rows of state s03, and whether the cursor held an iterator.
  auto read = [&](bool &held) {
    REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
    sm3::table_t *t = f.table(f.a);
    std::string v = sm2::make_varlena("s03");
    sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
    bool nn[1] = {false};
    sm2::bound_t b{d, nn, true};
    sm3::scan_t *scan = nullptr;
    REQUIRE(sm3::tbl_idx_scan_rows(t, f.by_state, &b, &b, scan) == 0);
    int n = 0;
    uint64_t id;
    std::string_view tuple;
    while (0 == sm3::tbl_scannxt(scan, id, tuple)) {
      n++;
    }
    held = sm1::batch_iter_held(scan->iter);
    sm3::tbl_scanend(scan);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    return n;
  };
  // Record statistics giving state the correlation c.
  auto analyzed = [&](float c) {
    sm3::stats_t st;
    st.target = 100;
    st.run_length = 64;
    st.rows_sampled = 200;
    st.rowno_limit = 200;
    st.live_rows = 200;
    st.heap_disk = 0;
    st.heap_mem = 0;
    st.avg_row_bytes = 20;
    sm3::stat_t s;
    s.position = 1;
    s.null_frac = 0.0f;
    s.width = 4;
    s.distinct = 10.0f;
    s.has_correlation = true;
    s.correlation = c;
    st.stats = {s};
    REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE_MESSAGE(sm3::cat_put_stats(f.a, f.jobs, st) == 0, sm1::errmsg());
    REQUIRE(sm3::commit(f.a) == 0);
  };
  bool held = false;
  CHECK(20 == read(held));
  CHECK(held);
  analyzed(0.9f);
  CHECK(20 == read(held));
  CHECK(held);
  analyzed(-0.9f);
  CHECK(20 == read(held));
  CHECK(held);
  analyzed(-0.1f);
  CHECK(20 == read(held));
  CHECK(!held);
}

TEST_CASE("tree: a table rewritten keyed anew keeps its trees") {
  // Verify a rewrite into a new store, a primary key added to rows,
  // makes each tree anew and fills it.
  // 1. Insert rows; add a primary key on id: the rows are rewritten.
  // 2. The trees give each state's rows, now named by their key, and
  //    an insert after is given too.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 60; i++) {
    row_t r(i, "s" + std::to_string(i % 3), "O" + std::to_string(i));
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  std::vector<sm3::index_key_spec_t> by_id = {{0, false, false, std::nullopt}};
  uint64_t pkey = 0;
  REQUIRE_MESSAGE(sm3::create_index(f.a, f.jobs, "pkey", by_id, {}, true, pkey,
                                    true, true) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  for (int s = 0; s < 3; s++) {
    std::vector<int32_t> want;
    for (int i = s; i < 60; i += 3) {
      want.push_back(i);
    }
    std::vector<int32_t> got =
        ids(t, f.by_state, "s" + std::to_string(s), "s" + std::to_string(s));
    std::sort(got.begin(), got.end());
    CHECK(want == got);
  }
  row_t r(100, "s1", "late");
  uint64_t id;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  CHECK(21u == ids(t, f.by_state, "s1", "s1").size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a temporary table's tree") {
  // Verify a tree of a temporary table, whose pages are the temporary
  // instance's, unlogged.
  // 1. Make a temporary table and a tree on it; insert rows.
  // 2. Scans give each state's rows; an update moves one; vacuum
  //    leaves one entry a row.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  std::vector<sm3::column_spec_t> cols = {
      {"id", sm2::type_int4.code, -1, true},
      {"state", sm2::type_text.code, -1, false},
      {"owner", sm2::type_text.code, -1, false}};
  uint64_t tmp = 0;
  REQUIRE_MESSAGE(sm3::create_temp_table(f.a, "tmp", cols, tmp) == 0,
                  sm1::errmsg());
  std::vector<sm3::index_key_spec_t> state = {{1, false, false, std::nullopt}};
  uint64_t ts = 0;
  REQUIRE_MESSAGE(sm3::create_index(f.a, tmp, "tmp_state", state, {}, false, ts,
                                    false, false, true) == 0,
                  sm1::errmsg());
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(f.a, tmp, sm1::SM_LOCKMODE_IX, t) == 0);
  uint64_t rid[10];
  for (int i = 0; i < 10; i++) {
    row_t r(i, i < 5 ? "x" : "y", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), rid[i]) == 0);
  }
  CHECK(5u == ids(t, ts, "x", "x").size());
  row_t moved(0, "y", "O");
  REQUIRE(sm3::tbl_update(t, rid[0], moved.d(), moved.null()) == 0);
  CHECK(4u == ids(t, ts, "x", "x").size());
  CHECK(6u == ids(t, ts, "y", "y").size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(f.a, tmp, sm1::SM_LOCKMODE_IX, t) == 0);
  REQUIRE(sm3::tbl_compact(t) == 0);
  CHECK(10u == entries_of(t, ts));
  CHECK(6u == ids(t, ts, "y", "y").size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a dropped tree's pages go once the drop commits") {
  // Verify DROP INDEX of a tree drops its pages after the commit, and
  // a rollback keeps them.
  // 1. Insert rows; drop by_state and roll back: it still scans.
  // 2. Drop it and commit: its root is gone from the store.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  for (int i = 0; i < 50; i++) {
    row_t r(i, "s", "O");
    uint64_t id;
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  uint64_t storeid = sm3::tbl_def(t).storeid;
  uint16_t relid = 0;
  for (const sm3::index_def_t &x : sm3::tbl_def(t).indexes) {
    if (x.oid == f.by_state) {
      relid = x.relid;
    }
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::string root{static_cast<char>(relid >> 8),
                   static_cast<char>(relid & 0xFF)};
  root.append(8, '\0');
  auto has_root = [&] {
    sm1::page_t *p = nullptr;
    std::string_view bytes;
    int rc = sm1::pin_page(sm3::session_sm1(f.a), storeid, root,
                           sm1::page_mode_t::sh, 0, p, bytes);
    REQUIRE(rc >= 0);
    sm1::unpin_page(sm3::session_sm1(f.a), p);
    return rc == 0;
  };
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_index(f.a, f.jobs, f.by_state) == 0);
  REQUIRE(sm3::abort(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  CHECK(has_root());
  t = f.table(f.a);
  CHECK(50u == ids(t, f.by_state, "s", "s").size());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_index(f.a, f.jobs, f.by_state) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  CHECK_FALSE(has_root());
  REQUIRE(sm3::commit(f.a) == 0);
}

namespace {

/* What an index-only scan of a tree gave: each entry's state, and how many came
 * with no row read. */
struct only_t {
  std::vector<std::string> states;
  size_t unread = 0;
};

/*
 * An index-only scan of index from text lo to hi, both inclusive
 * (tbl_scan_entries_only): each entry's key column, decoded from its
 * key, and the count handed out with the row unread.
 */
only_t entries_only(sm3::table_t *t, uint64_t index, std::string lo,
                    std::string hi) {
  std::string lv = sm2::make_varlena(lo), hv = sm2::make_varlena(hi);
  sm2::datum_t ld[1] = {sm2::datum_from_ptr(lv.data())};
  sm2::datum_t hd[1] = {sm2::datum_from_ptr(hv.data())};
  bool nn[1] = {false};
  sm2::bound_t l{ld, nn, true}, h{hd, nn, true};
  sm3::scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan_rows(t, index, &l, &h, scan) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::tbl_scan_entries_only(scan) == 0);
  only_t out;
  uint64_t id;
  sm3::pin_ref_t ref;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, ref, tuple)) == 0) {
    sm2::datum_t d[1];
    bool isnull[1];
    int cols = sm3::tbl_scan_cols(scan, d, isnull);
    REQUIRE(cols >= 0);
    if (1 == cols) {
      // A cut key gives no columns back: its row was read for them.
      CHECK_FALSE(tuple.empty());
      out.states.emplace_back("(cut)");
      continue;
    }
    REQUIRE_FALSE(isnull[0]);
    std::string_view v;
    sm2::datum_bytes(&sm2::type_text, -1, d[0], v);
    out.states.emplace_back(sm2::varlena_payload(v));
    if (nullptr == ref.pin) {
      CHECK(tuple.empty());
      out.unread++;
    }
  }
  CHECK(1 == rc);
  sm3::tbl_scanend(scan);
  return out;
}

} // namespace

TEST_CASE("tree: an index-only scan reads no row on a leaf vacuum set") {
  // Verify an index-only scan of a tree skips the rows of entries on
  // leaves vacuum set all visible, and still gives what the snapshot
  // sees (../BTREEINDEX.md §8).
  // 1. Insert 2,000 rows of state "old", over many leaves; before a
  //    vacuum, every row is read.
  // 2. Vacuum: the scan of "old" gives 2,000 entries, none read.
  // 3. b takes a repeatable read snapshot; a moves rows 0 to 9 to
  //    "new" and deletes 10 to 14, and commits.
  // 4. a's scan gives 1,985 "old" and 10 "new", as a scan of rows
  //    does; some rows read, of the leaves the writes cleared, most
  //    not.
  // 5. b's scan still gives 2,000 "old": the leaves its rows' entries
  //    are on were cleared, so their rows were read at its snapshot.
  // 6. b ends; a vacuum removes the 15 dead entries and sets every
  //    leaf again: 1,985 "old", none read.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(2000);
  for (int i = 0; i < 2000; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  only_t o = entries_only(t, f.by_state, "old", "old");
  CHECK(2000u == o.states.size());
  CHECK(0u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE_MESSAGE(sm3::tbl_compact(t) == 0, sm1::errmsg());
  o = entries_only(t, f.by_state, "old", "old");
  CHECK(2000u == o.states.size());
  CHECK(2000u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *tb = f.table(f.b);
  CHECK(2000u == entries_only(tb, f.by_state, "old", "old").states.size());

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  for (int i = 0; i < 10; i++) {
    row_t r(i, "new", "O");
    REQUIRE(sm3::tbl_update(t, id[i], r.d(), r.null()) == 0);
  }
  for (int i = 10; i < 15; i++) {
    REQUIRE(sm3::tbl_delete(t, id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  o = entries_only(t, f.by_state, "old", "old");
  CHECK(1985u == o.states.size());
  CHECK(1985u == ids(t, f.by_state, "old", "old").size());
  CHECK(o.unread > 0);
  CHECK(o.unread < 1985);
  for (const std::string &s : o.states) {
    CHECK("old" == s);
  }
  only_t fresh = entries_only(t, f.by_state, "new", "new");
  CHECK(10u == fresh.states.size());
  CHECK(0u == fresh.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  CHECK(2000u == entries_only(tb, f.by_state, "old", "old").states.size());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_compact(t) == 0);
  CHECK(1995u == entries_of(t, f.by_state));
  o = entries_only(t, f.by_state, "old", "old");
  CHECK(1985u == o.states.size());
  CHECK(1985u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a leaf is cleaned with no transaction and no lock") {
  // Verify tbl_clean_leaf judges rows as VACUUM does, with no
  // transaction, no table lock, and no snapshot held (../BTREEINDEX.md
  // ../BTREEINDEX.md §9).
  // 1. Insert rows, move one to another state, and delete one.
  // 2. With a writer's table lock held exclusive, clean page 0 of
  //    by_state and by_owner: the dead entries go, no entry is left in
  //    doubt, and the row a scan needs is there.
  // 3. An index-only scan reads no row: the leaf is set.
  // 4. A write not yet settled stays in doubt, and its leaf unset.
  // 5. Publishes older than the catalog's: nothing is judged, and an
  //    entry dead by its row stays.
  // 6. A relid no index has is nothing to clean.
  trees_t f;
  sm1::session_t *cs = f.open();
  sm1::set_no_transaction(cs);
  sm3::session_t *c = nullptr;
  REQUIRE(sm3::session_open(f.cat, cs, c) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(6);
  for (int i = 0; i < 6; i++) {
    row_t r(i, "free", "Ann");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t moved(0, "busy", "Ann");
  REQUIRE(sm3::tbl_update(t, id[0], moved.d(), moved.null()) == 0);
  REQUIRE(sm3::tbl_delete(t, id[1]) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  auto def = sm3::cat_get_table(f.cat, f.jobs);
  REQUIRE(def);
  auto relid_of = [&](uint64_t oid) {
    for (const auto &x : def->indexes) {
      if (x.oid == oid) {
        return x.relid;
      }
    }
    FAIL("no such index");
    return uint16_t{0};
  };
  uint16_t by_state = relid_of(f.by_state), by_owner = relid_of(f.by_owner);
  uint64_t pubs = sm3::cat_publishes(c);
  bool doubt = true;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *held = nullptr;
  REQUIRE(sm3::tbl_open(f.a, f.jobs, sm1::SM_LOCKMODE_EX, held) == 0);
  REQUIRE_MESSAGE(sm3::tbl_clean_leaf(c, def, pubs, by_state, 0, doubt) == 0,
                  sm1::errmsg());
  CHECK_FALSE(doubt);
  REQUIRE_MESSAGE(sm3::tbl_clean_leaf(c, def, pubs, by_owner, 0, doubt) == 0,
                  sm1::errmsg());
  CHECK_FALSE(doubt);
  sm3::tbl_close(held);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(5u == entries_of(t, f.by_state));
  only_t o = entries_only(t, f.by_state, "busy", "free");
  CHECK(5u == o.states.size());
  CHECK(5u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t again(2, "later", "Ann");
  REQUIRE(sm3::tbl_update(t, id[2], again.d(), again.null()) == 0);
  REQUIRE_MESSAGE(sm3::tbl_clean_leaf(c, def, pubs, by_state, 0, doubt) == 0,
                  sm1::errmsg());
  CHECK(doubt);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  doubt = false;
  REQUIRE(sm3::tbl_clean_leaf(c, def, pubs - 1, by_state, 0, doubt) == 0);
  CHECK(doubt);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(6u == entries_of(t, f.by_state));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  CHECK(1 == sm3::tbl_clean_leaf(c, def, pubs, 999, 0, doubt));
  sm3::session_close(c);
  REQUIRE(sm1::closedb(cs) == 0);
}

TEST_CASE("tree: whoever finds a leaf not all visible queues it") {
  // Verify a reader and each kind of writer queue a leaf that needs
  // cleaning, once, and that a leaf set all visible queues nothing
  // (../BTREEINDEX.md §9).
  // 1. Insert rows with no cleaner attached: nothing can be queued.
  // 2. Attach it. A scan finds the leaf unset, and queues it, once.
  // 3. A pass cleans it: the queue is empty, and a scan queues nothing.
  // 4. An update by row id queues the old key's leaf; a second update
  //    on that leaf adds no page.
  // 5. A pass cleans it; an insert queues its leaf.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(4);
  for (int i = 0; i < 4; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  f.cleaner();
  CHECK(0u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(4u == ids(t, f.by_state, "old", "old").size());
  CHECK(1u == sm3::cleaner_queued(f.cat));
  CHECK(4u == ids(t, f.by_state, "old", "old").size());
  CHECK(1u == sm3::cleaner_queued(f.cat));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  CHECK(0u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(4u == ids(t, f.by_state, "old", "old").size());
  CHECK(0u == sm3::cleaner_queued(f.cat));

  row_t a(0, "aaa", "O"), b(1, "aab", "O");
  REQUIRE(sm3::tbl_update(t, id[0], a.d(), a.null()) == 0);
  CHECK(1u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::tbl_update(t, id[1], b.d(), b.null()) == 0);
  CHECK(1u == sm3::cleaner_queued(f.cat));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  CHECK(0u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t fresh(9, "new", "O");
  uint64_t rid = 0;
  REQUIRE(sm3::tbl_insert(t, fresh.d(), fresh.null(), rid) == 0);
  CHECK(1u == sm3::cleaner_queued(f.cat));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a temporary table's leaf is not queued") {
  // Verify the cleaner is told nothing of a temporary table: it cannot
  // read that session's instance (../BTREEINDEX.md §9).
  // 1. Make a temporary table and a tree on it, with the cleaner on.
  // 2. Insert, scan and update: nothing is queued.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  std::vector<sm3::column_spec_t> cols = {
      {"id", sm2::type_int4.code, -1, true},
      {"state", sm2::type_text.code, -1, false},
      {"owner", sm2::type_text.code, -1, false}};
  uint64_t tmp = 0;
  REQUIRE(sm3::create_temp_table(f.a, "tmp", cols, tmp) == 0);
  std::vector<sm3::index_key_spec_t> state = {{1, false, false, std::nullopt}};
  uint64_t ts = 0;
  REQUIRE(sm3::create_index(f.a, tmp, "tmp_state", state, {}, false, ts, false,
                            false, true) == 0);
  sm3::table_t *t = nullptr;
  REQUIRE(sm3::tbl_open(f.a, tmp, sm1::SM_LOCKMODE_IX, t) == 0);
  row_t r(1, "x", "O"), moved(1, "y", "O");
  uint64_t rid = 0;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), rid) == 0);
  CHECK(1u == ids(t, ts, "x", "x").size());
  REQUIRE(sm3::tbl_update(t, rid, moved.d(), moved.null()) == 0);
  CHECK(0u == sm3::cleaner_queued(f.cat));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a cursor has no key before its first row") {
  // Verify tbl_scan_key before a row gives no key: it indexed the rows
  // before the first, and crashed.
  // 1. Open a cursor of rows on a tree, and read no row.
  // 2. Its key is empty.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t r(1, "old", "O");
  uint64_t id = 0;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan_rows(t, f.by_state, nullptr, nullptr, scan) == 0);
  CHECK(sm3::tbl_scan_key(scan).empty());
  sm3::tbl_scanend(scan);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a pass cleans no more pages than its budget") {
  // Verify a busy system's pass stops at its budget, and the rest stay
  // queued (../BTREEINDEX.md §9).
  // 1. Insert enough rows to queue many leaves.
  // 2. A pass with a budget of 2 takes two pages off the queue.
  // 3. A pass with no budget takes the rest.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t id = 0;
  for (int i = 0; i < 2000; i++) {
    row_t r(i, "state" + std::to_string(i), "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  size_t queued = sm3::cleaner_queued(f.cat);
  REQUIRE(queued > 4);
  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat, 2) == 0, sm1::errmsg());
  CHECK(queued - 2 == sm3::cleaner_queued(f.cat));
  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  CHECK(0u == sm3::cleaner_queued(f.cat));
}

TEST_CASE("tree: a pass removes dead entries, and sets the leaf") {
  // Verify the cleaner does VACUUM's work on a queued leaf, with no
  // VACUUM (../BTREEINDEX.md §9).
  // 1. Insert rows, then move one and delete one, and abort an insert.
  // 2. A pass removes the three dead entries; the queue is empty.
  // 3. An index-only scan gives the rows left, none read.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(5);
  for (int i = 0; i < 5; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t moved(0, "new", "O");
  REQUIRE(sm3::tbl_update(t, id[0], moved.d(), moved.null()) == 0);
  REQUIRE(sm3::tbl_delete(t, id[1]) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t gone(9, "gone", "O");
  uint64_t rid = 0;
  REQUIRE(sm3::tbl_insert(t, gone.d(), gone.null(), rid) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  CHECK(0u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(4u == entries_of(t, f.by_state));
  only_t o = entries_only(t, f.by_state, "a", "z");
  CHECK(4u == o.states.size());
  CHECK(4u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a page waits for the watermark, then is cleaned") {
  // Verify a leaf an old snapshot keeps in doubt stays queued, unread,
  // until the watermark moves (../BTREEINDEX.md §9).
  // 1. b takes a repeatable read snapshot; a moves a row and commits.
  // 2. A pass cleans nothing: the page stays on the queue, and the
  //    entry is there for b.
  // 3. b reads the row by its old key, and commits.
  // 4. A new transaction moves the watermark on. A pass removes the
  //    entry, and the queue is empty.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t r(1, "old", "O");
  uint64_t id = 0;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  CHECK(0u == sm3::cleaner_queued(f.cat));

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  sm3::table_t *tb = f.table(f.b);
  CHECK(1u == ids(tb, f.by_state, "old", "old").size());
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t moved(1, "new", "O");
  REQUIRE(sm3::tbl_update(t, id, moved.d(), moved.null()) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  size_t queued = sm3::cleaner_queued(f.cat);
  REQUIRE(queued > 0);
  REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  CHECK(queued == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  CHECK(queued == sm3::cleaner_queued(f.cat));
  CHECK(1u == ids(tb, f.by_state, "old", "old").size());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  CHECK(0u == sm3::cleaner_queued(f.cat));
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(1u == entries_of(t, f.by_state));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE(
    "tree: a queued page of a tree gone, or no longer a leaf, is dropped") {
  // Verify what DDL and a split leave on the queue costs nothing
  // (../BTREEINDEX.md §9).
  // 1. Queue a leaf of by_owner, then drop the index: a pass drops it.
  // 2. Queue a leaf of by_state, then truncate the table: the old store
  //    finds no table, and a pass drops it.
  // 3. Queue page 0, then fill the tree so page 0 is an inner page:
  //    every page is handled, and the rows are all there.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t r(1, "free", "O");
  uint64_t id = 0;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::cleaner_queued(f.cat) > 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::drop_index(f.a, f.jobs, f.by_owner) == 0, sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  CHECK(0u == sm3::cleaner_queued(f.cat));

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t r2(2, "other", "O");
  REQUIRE(sm3::tbl_insert(t, r2.d(), r2.null(), id) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::cleaner_queued(f.cat) > 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::truncate_table(f.a, f.jobs) == 0, sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  CHECK(0u == sm3::cleaner_queued(f.cat));

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t first(0, "aaaa", "O");
  REQUIRE(sm3::tbl_insert(t, first.d(), first.null(), id) == 0);
  REQUIRE(sm3::cleaner_queued(f.cat) > 0);
  for (int i = 1; i < 2000; i++) {
    row_t row(i, "state" + std::to_string(i), "O");
    REQUIRE(sm3::tbl_insert(t, row.d(), row.null(), id) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE_MESSAGE(sm3::cleaner_run_once(f.cat) == 0, sm1::errmsg());
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(2000u == entries_of(t, f.by_state));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: the cleaner's thread runs with no VACUUM") {
  // Verify a worker cleans what is queued by itself, and stops at a
  // detach.
  // 1. Attach one worker. Move a row and commit.
  // 2. Within ten seconds the dead entry is gone.
  // 3. Detach stops it, and the fixture closes the catalog.
  trees_t f;
  f.cleaner(1);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  row_t r(1, "old", "O");
  uint64_t id = 0;
  REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t moved(1, "new", "O");
  REQUIRE(sm3::tbl_update(t, id, moved.d(), moved.null()) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  size_t entries = 2;
  for (int i = 0; i < 100 && 1 != entries; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
    t = f.table(f.a);
    entries = entries_of(t, f.by_state);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
  CHECK(1u == entries);
  sm3::cleaner_detach(f.cat);
}

TEST_CASE("tree: scans agree with the rows while the cleaner runs") {
  // Verify a cleaner running beside writers and readers removes no live
  // entry, and a scan sees every row it should.
  // 1. Insert 400 rows, split between states a and b.
  // 2. Two writers move rows between the states; a reader counts both
  //    states at repeatable read; a thread runs passes all the while.
  // 3. Each read sees 400 rows in all.
  // 4. Once all stop, a pass leaves one entry a row.
  trees_t f;
  f.cleaner();
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  constexpr int rows = 400;
  std::vector<uint64_t> id(rows);
  for (int i = 0; i < rows; i++) {
    row_t r(i, i % 2 ? "a" : "b", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::atomic<int> reads{0};
  std::thread cleaner([&] {
    while (!stop) {
      if (sm3::cleaner_run_once(f.cat) != 0) {
        bad++;
      }
    }
  });
  auto session = [&] {
    sm3::session_t *s = nullptr;
    REQUIRE(sm3::session_open(f.cat, f.open(), s) == 0);
    return s;
  };
  std::vector<sm3::session_t *> writers = {session(), session()};
  std::vector<std::thread> threads;
  for (int w = 0; w < 2; w++) {
    threads.emplace_back([&, w] {
      sm3::session_t *s = writers[w];
      for (int n = 0; n < 300; n++) {
        if (sm3::begin(s, sm1::xctlevel_t::read_committed) != 0) {
          bad++;
          return;
        }
        sm3::table_t *wt = nullptr;
        if (sm3::tbl_open(s, f.jobs, sm1::SM_LOCKMODE_IX, wt) != 0) {
          bad++;
          return;
        }
        int i = (w * rows / 2) + n % (rows / 2);
        row_t r(i, n % 2 ? "a" : "b", "O");
        if (sm3::tbl_update(wt, id[i], r.d(), r.null()) != 0) {
          bad++;
        }
        sm3::tbl_close(wt);
        sm3::commit(s);
      }
    });
  }
  threads.emplace_back([&] {
    for (int n = 0; n < 30; n++) {
      if (sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) != 0) {
        bad++;
        return;
      }
      sm3::table_t *rt = nullptr;
      if (sm3::tbl_open(f.b, f.jobs, sm1::SM_LOCKMODE_IS, rt) != 0) {
        bad++;
        return;
      }
      size_t seen = ids(rt, f.by_state, "a", "a").size() +
                    ids(rt, f.by_state, "b", "b").size();
      bad += rows == seen ? 0 : 1;
      reads++;
      sm3::tbl_close(rt);
      sm3::commit(f.b);
    }
  });
  for (std::thread &th : threads) {
    th.join();
  }
  stop = true;
  cleaner.join();
  CHECK(0 == bad);
  CHECK(30 == reads);
  for (sm3::session_t *s : writers) {
    sm3::session_close(s);
  }
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(rows == static_cast<int>(ids(t, f.by_state, "a", "a").size() +
                                 ids(t, f.by_state, "b", "b").size()));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  while (0 != sm3::cleaner_queued(f.cat)) {
    REQUIRE(sm3::cleaner_run_once(f.cat) == 0);
  }
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  CHECK(rows == static_cast<int>(entries_of(t, f.by_state)));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: a cut entry leaves its leaf set, and its row read") {
  // Verify a cut entry does not keep its leaf from being all visible,
  // and that an index-only scan still reads its row (../BTREEINDEX.md §9).
  // 1. Insert five short rows and one past the tree's key budget.
  // 2. Vacuum: an index-only scan gives all six, the five unread, as
  //    the leaf is set, the cut one read.
  // 3. Move the long row to a short state, and vacuum: its dead cut
  //    entry goes, and the six entries are short and unread.
  trees_t f;
  const std::string longest(2000, 'z');
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  uint64_t id = 0;
  for (int i = 0; i < 5; i++) {
    row_t r(i, std::string(1, static_cast<char>('a' + i)), "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id) == 0);
  }
  row_t big(5, longest, "O");
  uint64_t big_id = 0;
  REQUIRE(sm3::tbl_insert(t, big.d(), big.null(), big_id) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE_MESSAGE(sm3::tbl_compact(t) == 0, sm1::errmsg());
  only_t o = entries_only(t, f.by_state, "a", longest);
  CHECK(6u == o.states.size());
  CHECK(5u == o.unread);
  CHECK("(cut)" == o.states.back());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  row_t small(5, "f", "O");
  REQUIRE(sm3::tbl_update(t, big_id, small.d(), small.null()) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE_MESSAGE(sm3::tbl_compact(t) == 0, sm1::errmsg());
  CHECK(6u == entries_of(t, f.by_state));
  o = entries_only(t, f.by_state, "a", longest);
  CHECK(6u == o.states.size());
  CHECK(6u == o.unread);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("tree: an uncommitted write's leaf is read, not skipped") {
  // Verify a writer clears its leaves before it commits, so another
  // transaction's index-only scan reads the rows there and sees only
  // what its snapshot does; and that an abort leaves them read until
  // vacuum.
  // 1. Insert 500 rows of "old" and vacuum: every leaf set.
  // 2. a deletes row 0 and inserts a row of "old", uncommitted.
  // 3. b's scan gives 500 "old", with some rows read.
  // 4. a aborts; b's scan gives 500 again; a vacuum sets every leaf,
  //    and b's next scan reads no row.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(500);
  for (int i = 0; i < 500; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_compact(t) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_delete(t, id[0]) == 0);
  row_t extra(900, "old", "O");
  uint64_t xid;
  REQUIRE(sm3::tbl_insert(t, extra.d(), extra.null(), xid) == 0);

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *tb = f.table(f.b);
  only_t o = entries_only(tb, f.by_state, "old", "old");
  CHECK(500u == o.states.size());
  CHECK(o.unread < 500);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  tb = f.table(f.b);
  CHECK(500u == entries_only(tb, f.by_state, "old", "old").states.size());
  REQUIRE(sm3::tbl_compact(tb) == 0);
  o = entries_only(tb, f.by_state, "old", "old");
  CHECK(500u == o.states.size());
  CHECK(500u == o.unread);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("tree: a delete fails a reader whose index-only scan read no row") {
  // Verify a delete reports its tree entry's key to SSI, so a
  // serializable index-only scan that skipped the row, its leaf all
  // visible, still sees the delete as a write in its range.
  // 1. Insert 100 rows of "old" and vacuum: every leaf set.
  // 2. a and b, serializable: a counts "old" by an index-only scan,
  //    no row read; b counts "new", none.
  // 3. a inserts a row of "new", into b's range; b deletes a row of
  //    "old", in a's: a write skew only the entries show.
  // 4. a commits; b fails with SM_ERR_SERIALIZATION.
  trees_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.table(f.a);
  std::vector<uint64_t> id(100);
  for (int i = 0; i < 100; i++) {
    row_t r(i, "old", "O");
    REQUIRE(sm3::tbl_insert(t, r.d(), r.null(), id[i]) == 0);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  t = f.table(f.a);
  REQUIRE(sm3::tbl_compact(t) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::serializable) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::serializable) == 0);
  sm3::table_t *ta = f.table(f.a);
  sm3::table_t *tb = f.table(f.b);
  only_t o = entries_only(ta, f.by_state, "old", "old");
  CHECK(100u == o.states.size());
  CHECK(100u == o.unread);
  CHECK(entries_only(tb, f.by_state, "new", "new").states.empty());
  row_t ra(500, "new", "A");
  uint64_t ia;
  REQUIRE(sm3::tbl_insert(ta, ra.d(), ra.null(), ia) == 0);
  REQUIRE(sm3::tbl_delete(tb, id[50]) == 0);
  sm3::tbl_close(ta);
  sm3::tbl_close(tb);
  CHECK(0 == sm3::commit(f.a));
  CHECK(-1 == sm3::commit(f.b));
  CHECK(sm1::SM_ERR_SERIALIZATION == sm1::errnum());
}
