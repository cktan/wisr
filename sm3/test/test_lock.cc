/*
 * test_lock.cc -- schema locks (SM3_DESIGN.md §2, CATALOG.md §5):
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
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_init(s) == 0);
    REQUIRE(sm3::cat_open(s, cat) == 0);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    REQUIRE(sm3::session_open(cat, open(), c) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"x", sm2::type_int4.code, -1, false}};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_create_table(a, sm3::nsp_public, "t", cols, t) == 0);
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

/* A column y int4, nullable, for cat_add_column. */
const sm3::column_spec_t col_y = {"y", sm2::type_int4.code, -1, false};

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("readers and writers of a table hold its schema lock together") {
  // Verify IS and IX on one table coexist, and each gets its definition.
  // 1. A locks t IS, B locks t IX, C locks t IS: none waits.
  // 2. Each sees t with its one column.
  locks_t f;
  def_ptr da, db, dc;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_lock_table(f.a, f.t, sm1::lockmode_t::intent_shared, da) ==
          0);
  REQUIRE(sm3::cat_lock_table(f.b, f.t, sm1::lockmode_t::intent_exclusive,
                              db) == 0);
  REQUIRE(sm3::cat_lock_table(f.c, f.t, sm1::lockmode_t::intent_shared, dc) ==
          0);
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
  REQUIRE(sm3::cat_lock_table(f.a, f.t, sm1::lockmode_t::intent_shared, d) ==
          0);
  REQUIRE(sm3::cat_lock_table(f.b, f.t, sm1::lockmode_t::intent_exclusive, d) ==
          0);

  std::atomic<bool> returned{false};
  int rc = -1;
  REQUIRE(sm3::begin(f.c, sm1::xctlevel_t::read_committed) == 0);
  std::thread tc([&] {
    rc = sm3::cat_add_column(f.c, f.t, col_y, std::nullopt);
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
    REQUIRE(sm3::cat_add_column(f.b, f.t, col_y, std::nullopt) == 0);

    std::atomic<bool> returned{false};
    int rc = -1;
    def_ptr d;
    std::thread ta([&] {
      rc = sm3::cat_lock_table(f.a, f.t, sm1::lockmode_t::intent_shared, d);
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
  REQUIRE(sm3::cat_drop_table(f.b, f.t) == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  def_ptr d;
  std::thread ta([&] {
    rc = sm3::cat_lock_table(f.a, f.t, sm1::lockmode_t::intent_shared, d);
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

TEST_CASE("two DDL on one table run one after the other") {
  // Verify the second DDL waits for the first, then reads its change.
  // 1. A adds column y to t.
  // 2. B adds column z to t, in a thread: it waits.
  // 3. A commits: B's DDL returns, and t has x, y and z.
  locks_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_add_column(f.a, f.t, col_y, std::nullopt) == 0);

  std::atomic<bool> returned{false};
  int rc = -1;
  sm3::column_spec_t z = {"z", sm2::type_int4.code, -1, false};
  std::thread tb([&] {
    rc = sm3::cat_add_column(f.b, f.t, z, std::nullopt);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  REQUIRE(sm3::commit(f.a) == 0);
  tb.join();
  REQUIRE(rc == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  def_ptr d = sm3::cat_table(f.cat, f.t);
  REQUIRE(d != nullptr);
  CHECK(d->columns.size() == 3);
}
