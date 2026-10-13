/*
 * test_lock.cc -- table-oid locks (SM3_DESIGN.md §2, CATALOG.md §5):
 * readers and writers of a table coexist, and DDL waits for them
 * and they for it.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using def_ptr = std::shared_ptr<const sm3::table_def_t>;

/* A catalog with table t (x int4), and sessions a, b and c over it. */
class locks_t : public db_t {
public:
  /* Initialize and load the catalog, create t, open a, b and c. */
  locks_t() {
    sm1::session_t *s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    REQUIRE(sm3::session_open(cat, open(), c) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"x", sm2::type_int4.code, -1, false}};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "t", cols, t) == 0);
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~locks_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::session_close(c);
    sm3::cat_close(cat);
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr, *c = nullptr;
  uint64_t t = 0;
};

/* A column y int4, nullable, for add_column. */
const sm3::column_spec_t col_y = {"y", sm2::type_int4.code, -1, false};

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("readers and writers of a table hold its table-oid lock together") {
  // Verify IS and IX on one table coexist, and each gets its definition.
  // 1. A locks t IS, B locks t IX, C locks t IS: none waits.
  // 2. Each sees t with its one column.
  locks_t f;
  def_ptr da, db, dc;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_lock_table(f.a, f.t, sm1::SM_LOCKMODE_IS, da) == 0);
  REQUIRE(sm3::cat_lock_table(f.b, f.t, sm1::SM_LOCKMODE_IX, db) == 0);
  REQUIRE(sm3::cat_lock_table(f.c, f.t, sm1::SM_LOCKMODE_IS, dc) == 0);
  CHECK(da->columns.size() == 1);
  CHECK(db == da);
  CHECK(dc == da);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE(sm3::commit(f.c) == 0);
}

TEST_CASE("DDL waits for the table's readers and writers") {
  // Verify DDL's X waits out an IS holder and an IX holder.
  // 1. A locks t IS and B locks t IX.
  // 2. C adds a column to t, in a thread: it waits.
  // 3. A commits: C still waits, on B.
  // 4. B commits: C's DDL returns, and succeeds.
  locks_t f;
  def_ptr d;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_lock_table(f.a, f.t, sm1::SM_LOCKMODE_IS, d) == 0);
  REQUIRE(sm3::cat_lock_table(f.b, f.t, sm1::SM_LOCKMODE_IX, d) == 0);

  std::atomic<bool> returned{false};
  int rc = -1;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  std::thread tc([&] {
    rc = sm3::add_column(f.c, f.t, col_y, std::nullopt);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.a) == 0);
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.b) == 0);
  tc.join();
  CHECK(rc == 0);
  REQUIRE(sm3::commit(f.c) == 0);
}

TEST_CASE("a reader waits for DDL, then reads the new definition") {
  // Verify a reader blocked by DDL sees its change, at every level.
  // 1. A begins; at repeatable read its snapshot predates the DDL.
  // 2. B adds column y to t, holding t X.
  // 3. A locks t IS, in a thread: it waits.
  // 4. B commits: A's lock returns, without a serialization
  //    failure, and its definition has y.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read}) {
    CAPTURE(level);
    locks_t f;
    REQUIRE(sm3::begin(f.a, level) == 0);
    REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::add_column(f.b, f.t, col_y, std::nullopt) == 0);

    std::atomic<bool> returned{false};
    int rc = -1;
    def_ptr d;
    std::thread ta([&] {
      rc = sm3::cat_lock_table(f.a, f.t, sm1::SM_LOCKMODE_IS, d);
      returned = true;
    });
    settle();
    CHECK_FALSE(returned);

    REQUIRE(sm3::commit(f.b) == 0);
    ta.join();
    REQUIRE(rc == 0);
    CHECK(d->columns.size() == 2);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("a table dropped while a reader waited is not found") {
  // Verify a lock granted after a DROP fails with SM_ERR_NOTFOUND.
  // 1. B drops t, holding t X.
  // 2. A locks t IS, in a thread: it waits.
  // 3. B commits: A's lock fails with SM_ERR_NOTFOUND.
  locks_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_table(f.b, f.t) == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  def_ptr d;
  std::thread ta([&] {
    rc = sm3::cat_lock_table(f.a, f.t, sm1::SM_LOCKMODE_IS, d);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.b) == 0);
  ta.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_NOTFOUND);
  CHECK(d == nullptr);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("an index's state is set without waiting for the table's users") {
  // Verify marking an index valid takes the table's lock IX, not X.
  // 1. Three indexes are made on t, which is committed: all pending.
  // 2. A reads t and B writes it, both in open transactions that
  //    hold its lock, and A's handle has the first index pending.
  // 3. C marks the first index valid and commits, waiting for
  //    neither: it does not need the table to itself.
  // 4. C marks the third failed and B the second valid, in
  //    transactions open at once, and both commit: each has taken
  //    effect, neither undoing the other.
  // 5. A's handle, opened before, still has the index pending, and
  //    may not scan it. A table A opens now has it valid, but A,
  //    at REPEATABLE READ with a snapshot older than the index's
  //    last entries, still may not scan it. A new transaction may.
  locks_t f;
  std::vector<sm3::index_key_spec_t> on_x = {{0, false, false, std::nullopt}};
  uint64_t x1 = 0, x2 = 0, x3 = 0;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_index(f.c, f.t, "t_x1", on_x, {}, false, x1) == 0);
  REQUIRE(sm3::create_index(f.c, f.t, "t_x2", on_x, {}, false, x2) == 0);
  REQUIRE(sm3::create_index(f.c, f.t, "t_x3", on_x, {}, false, x3) == 0);
  REQUIRE(sm3::commit(f.c) == 0);
  using sm3::index_state_t;
  CHECK(sm3::cat_get_index_state(f.cat, x1).state == index_state_t::pending);

  sm3::table_t *ta = nullptr, *tb = nullptr;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::tbl_open(f.a, f.t, sm1::SM_LOCKMODE_IS, ta) == 0);
  REQUIRE(sm3::tbl_open(f.b, f.t, sm1::SM_LOCKMODE_IX, tb) == 0);
  CHECK(sm3::tbl_index_state(ta, x1) == index_state_t::pending);
  sm3::tbl_close(tb);

  // With A and B holding the table, this returns at once.
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::set_index_state(f.c, f.t, x1, index_state_t::valid) == 0,
                  sm1::errmsg());
  CHECK(sm3::cat_get_index_state(f.c, x1).state == index_state_t::valid);
  CHECK(sm3::cat_get_index_state(f.cat, x1).state == index_state_t::pending);
  REQUIRE(sm3::commit(f.c) == 0);
  CHECK(sm3::cat_get_index_state(f.cat, x1).state == index_state_t::valid);

  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::set_index_state(f.c, f.t, x3, index_state_t::failed,
                               sm1::SM_ERR_DUPKEY, "two rows") == 0);
  REQUIRE(sm3::set_index_state(f.b, f.t, x2, index_state_t::valid) == 0);
  REQUIRE(sm3::commit(f.c) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  CHECK(sm3::cat_get_index_state(f.cat, x3).state == index_state_t::failed);
  CHECK(sm3::cat_get_index_state(f.cat, x3).reason == "two rows");
  CHECK(sm3::cat_get_index_state(f.cat, x1).state == index_state_t::valid);
  CHECK(sm3::cat_get_index_state(f.cat, x2).state == index_state_t::valid);

  CHECK(sm3::tbl_index_state(ta, x2) == index_state_t::pending);
  CHECK_FALSE(sm3::tbl_index_usable(ta, x2));
  sm3::tbl_close(ta);
  REQUIRE(sm3::tbl_open(f.a, f.t, sm1::SM_LOCKMODE_IS, ta) == 0);
  CHECK(sm3::tbl_index_state(ta, x2) == index_state_t::valid);
  CHECK_FALSE(sm3::tbl_index_usable(ta, x2));
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::tbl_open(f.a, f.t, sm1::SM_LOCKMODE_IS, ta) == 0);
  CHECK(sm3::tbl_index_usable(ta, x2));
  CHECK_FALSE(sm3::tbl_index_usable(ta, x3));
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an index's state is set once, by whoever is first") {
  // Verify two transactions cannot both set one index's state.
  // 1. An index is made on t, pending. C marks it valid and does
  //    not commit.
  // 2. B marks it failed, in a thread: it waits for C.
  // 3. C commits: B is refused with SM_ERR_INVALID, and the index
  //    is valid, as C left it.
  locks_t f;
  std::vector<sm3::index_key_spec_t> on_x = {{0, false, false, std::nullopt}};
  uint64_t x = 0;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_index(f.c, f.t, "t_x", on_x, {}, false, x) == 0);
  REQUIRE(sm3::commit(f.c) == 0);

  using sm3::index_state_t;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::set_index_state(f.c, f.t, x, index_state_t::valid) == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = sm3::set_index_state(f.b, f.t, x, index_state_t::failed,
                              sm1::SM_ERR_DUPKEY, "two rows");
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.c) == 0);
  tb.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(f.b) == 0);
  CHECK(sm3::cat_get_index_state(f.cat, x).state == index_state_t::valid);
}

TEST_CASE("DDL changes the table as it now is, not as its snapshot shows") {
  // Verify DDL works on the latest record, whatever its snapshot.
  // 1. A begins at REPEATABLE READ: its snapshot shows t with x.
  // 2. B adds y to t, and commits.
  // 3. A adds z to t: it is added after y, not in y's place, though
  //    A's snapshot is older than y.
  // 4. After A commits, t has x, y and z, here and after the
  //    catalog is loaded again.
  locks_t f;
  const sm3::column_spec_t col_z = {"z", sm2::type_int4.code, -1, false};
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::add_column(f.b, f.t, col_y, std::nullopt) == 0);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE_MESSAGE(sm3::add_column(f.a, f.t, col_z, std::nullopt) == 0,
                  sm1::errmsg());
  REQUIRE_MESSAGE(sm3::commit(f.a) == 0, sm1::errmsg());
  auto names = [](const def_ptr &d) {
    std::vector<std::string> out;
    for (const sm3::column_def_t &c : d->columns) {
      out.push_back(c.name);
    }
    return out;
  };
  using names_t = std::vector<std::string>;
  CHECK(names(sm3::cat_get_table(f.cat, f.t)) == names_t{"x", "y", "z"});

  sm3::catalog_t *again = nullptr;
  REQUIRE_MESSAGE(sm3::cat_open(f.env(), "db", again) == 0, sm1::errmsg());
  CHECK(names(sm3::cat_get_table(again, f.t)) == names_t{"x", "y", "z"});
  sm3::cat_close(again);
}

TEST_CASE("a namespace's drop waits for a create in it, and then refuses") {
  // Verify a drop cannot miss a table made in its namespace, even
  // from a snapshot that is older than the table.
  // 1. A namespace is committed. B begins at REPEATABLE READ, which
  //    takes its snapshot before the table exists.
  // 2. A creates a table in the namespace, holding it shared.
  // 3. B drops the namespace, in a thread: it waits.
  // 4. A commits: B's drop fails with SM_ERR_INUSE, though B's
  //    snapshot shows the namespace empty.
  locks_t f;
  uint64_t app = 0, oid = 0;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_namespace(f.a, "app", app) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(f.a, app, "orders", cols, oid) == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = sm3::drop_namespace(f.b, app);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.a) == 0);
  tb.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.b) == 0);
}

TEST_CASE("a create waits for its namespace's drop, and then finds none") {
  // Verify a table cannot be made in a namespace dropped under it.
  // 1. A namespace is committed. A begins at REPEATABLE READ, which
  //    takes its snapshot while the namespace exists.
  // 2. B drops the namespace, holding it exclusive.
  // 3. A creates a table in it, in a thread: it waits.
  // 4. B commits: A's create fails with SM_ERR_NOTFOUND, though A's
  //    snapshot still shows the namespace.
  // 5. Two creates in one namespace do not wait for each other.
  locks_t f;
  uint64_t app = 0, oid = 0, o2 = 0;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_namespace(f.a, "app", app) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_namespace(f.b, app) == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread ta([&] {
    rc = sm3::create_table(f.a, app, "orders", cols, oid);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.b) == 0);
  ta.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "one", cols, oid) == 0);
  REQUIRE(sm3::create_table(f.b, sm3::nsp_public, "two", cols, o2) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("two DDL on one table run one after the other") {
  // Verify the second DDL waits for the first, then reads its change.
  // 1. A adds column y to t.
  // 2. B adds column z to t, in a thread: it waits.
  // 3. A commits: B's DDL returns, and t has x, y and z.
  locks_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::add_column(f.a, f.t, col_y, std::nullopt) == 0);

  std::atomic<bool> returned{false};
  int rc = -1;
  sm3::column_spec_t z = {"z", sm2::type_int4.code, -1, false};
  std::thread tb([&] {
    rc = sm3::add_column(f.b, f.t, z, std::nullopt);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.a) == 0);
  tb.join();
  REQUIRE(rc == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  def_ptr d = sm3::cat_get_table(f.cat, f.t);
  REQUIRE(d != nullptr);
  CHECK(d->columns.size() == 3);
}

TEST_CASE("a NOT NULL column with no missing value needs t empty, free now") {
  // Verify add_column takes t X only if it is free, keeps it only
  // if t holds no row, and lets go of nothing held from before.
  // 1. B holds t IS: A's add of NOT NULL z fails at once,
  //    SM_ERR_BLOCKED.
  // 2. B commits: A's add succeeds on the empty t, and A holds t X,
  //    so C's add of NOT NULL w is SM_ERR_BLOCKED. A aborts.
  // 3. A inserts a row into t and commits.
  // 4. A's add of NOT NULL z fails, SM_ERR_NOTNULL, and lets t's X
  //    go: C's add of y returns while A is still open.
  // 5. A adds y, taking t X, then its add of NOT NULL z fails: the
  //    X from before is kept, so C's add of y waits until A aborts.
  locks_t f;
  def_ptr d;
  const sm3::column_spec_t col_z = {"z", sm2::type_int4.code, -1, true};
  const sm3::column_spec_t col_w = {"w", sm2::type_int4.code, -1, true};
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_lock_table(f.b, f.t, sm1::SM_LOCKMODE_IS, d) == 0);
  CHECK(sm3::add_column(f.a, f.t, col_z, std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE_MESSAGE(sm3::add_column(f.a, f.t, col_z, std::nullopt) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::add_column(f.c, f.t, col_w, std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  REQUIRE(sm3::abort(f.c) == 0);
  REQUIRE(sm3::abort(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *tb = nullptr;
  REQUIRE(sm3::tbl_open(f.a, f.t, sm1::SM_LOCKMODE_IX, tb) == 0);
  sm2::datum_t v = sm2::datum_from_int32(7);
  bool isnull = false;
  uint64_t rowno = 0;
  REQUIRE(sm3::tbl_insert(tb, {&v, 1}, {&isnull, 1}, rowno) == 0);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.a) == 0);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::add_column(f.a, f.t, col_z, std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::add_column(f.c, f.t, col_y, std::nullopt) == 0);
  REQUIRE(sm3::abort(f.c) == 0);

  REQUIRE(sm3::add_column(f.a, f.t, col_y, std::nullopt) == 0);
  CHECK(sm3::add_column(f.a, f.t, col_z, std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
  std::atomic<bool> returned{false};
  int rc = -1;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  std::thread tc([&] {
    rc = sm3::add_column(f.c, f.t, col_y, std::nullopt);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm3::abort(f.a) == 0);
  tc.join();
  CHECK(rc == 0);
  REQUIRE(sm3::abort(f.c) == 0);
}
