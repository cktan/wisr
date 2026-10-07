/*
 * test_read_latest.cc -- sm1::read_latest and SM_ERR_UPDATED: the
 * argument rules and the engine state a scenario script cannot
 * see (SM1_DESIGN.md §5.4). The timelines themselves are the f
 * scripts in mvcc/in/.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <string>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "x" -> "0", "y" -> "0". */
class latest_fixture_t {
public:
  /* Create "db", the store, and the two rows. */
  latest_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    g_store = mkstore(s);
    put(s, g_store, "x", "0");
    put(s, g_store, "y", "0");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

private:
  home_t m_home;
};

/* Set key in store 1 to val; the result of sm1::update. */
int set(sm1::session_t *s, const std::string &key, const std::string &val) {
  return sm1::update(s, g_store, key, val);
}

/* sm1::read_latest of key: "<none>" if absent; -1 as "<error>". */
std::string latest(sm1::session_t *s, const std::string &key) {
  std::string_view data;
  int r = sm1::read_latest(s, g_store, key, data);
  if (r < 0) {
    return "<error>";
  }
  return r == 1 ? "<none>" : std::string(data);
}

/* Commit a new value of key from a fresh repeatable read session. */
void commit_from_other(latest_fixture_t &f, const std::string &key,
                       const std::string &val) {
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(u, key, val) == 0);
  REQUIRE(sm1::commit(u) == 0);
  REQUIRE(sm1::closedb(u) == 0);
}

} // namespace

TEST_CASE(
    "sm1_read_latest needs read committed and a lock this transaction holds") {
  // Verify the argument rules of sm1::read_latest.
  // 1. At repeatable read, with the lock held: SM_ERR_INVALID.
  // 2. At read committed, with no lock held: SM_ERR_INVALID.
  // 3. At read committed under an exclusive lock: it reads.
  latest_fixture_t f;
  sm1::session_t *t = f.open();

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock(t, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_EX) ==
          0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(t) == 0);

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::lock(t, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_EX) ==
          0);
  CHECK(latest(t, "x") == "0");
  REQUIRE(sm1::commit(t) == 0);
  sm1::closedb(t);
}

TEST_CASE("SM_ERR_UPDATED is not retryable and leaves the transaction active") {
  // Verify the failed write neither aborts nor releases the lock.
  // 1. T (read committed) begins; U commits a new "x".
  // 2. T's update of "x" fails with SM_ERR_UPDATED, not retryable.
  // 3. T is still active: it reads the newest "x", writes, commits.
  latest_fixture_t f;
  sm1::session_t *t = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  commit_from_other(f, "x", "u");

  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(sm1::retryable() == 0);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == 0);
  REQUIRE(sm1::commit(t) == 0);
  sm1::closedb(t);
}

TEST_CASE("a successful write consumes the mark") {
  // Verify a write after a savepoint rollback must re-read.
  // 1. T begins; U commits a new "x"; T locks "x" exclusively.
  // 2. T sets a savepoint, reads the latest "x" and writes it.
  // 3. T rolls back to the savepoint, undoing the write but
  //    keeping the lock.
  // 4. T's next write of "x" fails with SM_ERR_UPDATED again.
  latest_fixture_t f;
  sm1::session_t *t = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  commit_from_other(f, "x", "u");
  REQUIRE(sm1::lock(t, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_EX) ==
          0);

  REQUIRE(sm1::savepoint(t) == 1);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == 0);
  REQUIRE(sm1::rollback(t, 1) == 0);
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::abort(t) == 0);
  sm1::closedb(t);
}

TEST_CASE("under a shared lock sm1_read_latest reads but sets no mark") {
  // Verify a shared holder sees the newest row but cannot write.
  // 1. T (read committed) takes a shared lock on "x"; a commit
  //    to "x" landed after T began.
  // 2. sm1::read_latest returns the newest value.
  // 3. T's update, an upgrade, still fails with SM_ERR_UPDATED.
  latest_fixture_t f;
  sm1::session_t *t = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  commit_from_other(f, "x", "u");

  REQUIRE(sm1::lock(t, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_SH) ==
          0);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::abort(t) == 0);
  sm1::closedb(t);
}
