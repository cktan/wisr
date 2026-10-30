/*
 * test_tree.cc -- a table's index made a tree (../BTREE.md §13 step
 * 7): writes maintain it as any index, and its scan of rows hands out
 * each row, as the snapshot shows it, only while the row still has
 * its entry's key.
 */

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
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
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
  // 1. Insert 2,000 rows: the tree's bytes are more than none.
  // 2. A tree of a table with no rows has its empty root alone.
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
  REQUIRE(sm3::tbl_idx_size(t, f.by_state, disk, mem) == 0);
  CHECK(disk + mem > empty);
  CHECK(disk + mem > 2000u * 10);
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
