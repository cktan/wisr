/*
 * test_read_latest.cc -- sm_read_latest and SM_ERR_UPDATED: the
 * argument rules and the engine state a scenario script cannot
 * see (DESIGN.md §5.4). The timelines themselves are the f
 * scripts in mvcc/in/.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

#include <string>

namespace {

/* The fixture's relation, set when the fixture is created. */
uint64_t g_rel = 0;

/* A database "db" with one relation holding "x" -> "0", "y" -> "0". */
class latest_fixture_t {
public:
  /* Create "db", the relation, and the two rows. */
  latest_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    sm_session_t *s = open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    g_rel = mkrel(s);
    put(s, g_rel, "x", "0");
    put(s, g_rel, "y", "0");
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_closedb(s) == 0);
  }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

private:
  home_t m_home;
};

/* Set key in relation 1 to val; the result of sm_update. */
int set(sm_session_t *s, const std::string &key, const std::string &val) {
  return sm_update(s, g_rel, key.data(), key.size(), val.data(), val.size(),
                   nullptr, nullptr);
}

/* sm_read_latest of key: "<none>" if absent; -1 as "<error>". */
std::string latest(sm_session_t *s, const std::string &key) {
  const char *data = nullptr;
  size_t len = 0;
  int r = sm_read_latest(s, g_rel, key.data(), key.size(), &data, &len);
  if (r < 0) {
    return "<error>";
  }
  return r == 1 ? "<none>" : std::string(data, len);
}

/* Commit a new value of key from a fresh repeatable read session. */
void commit_from_other(latest_fixture_t &f, const std::string &key,
                       const std::string &val) {
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(u, key, val) == 0);
  REQUIRE(sm_commit(u) == 0);
  REQUIRE(sm_closedb(u) == 0);
}

} // namespace

TEST_CASE(
    "sm_read_latest needs read committed and a lock this transaction holds") {
  // Verify the argument rules of sm_read_latest.
  // 1. At repeatable read, with the lock held: SM_ERR_INVALID.
  // 2. At read committed, with no lock held: SM_ERR_INVALID.
  // 3. At read committed under an exclusive lock: it reads.
  latest_fixture_t f;
  sm_session_t *t = f.open();

  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(t, g_rel, "x", 1, SM_LOCK_EXCLUSIVE) == 0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(sm_abort(t) == 0);

  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(sm_lock(t, g_rel, "x", 1, SM_LOCK_EXCLUSIVE) == 0);
  CHECK(latest(t, "x") == "0");
  REQUIRE(sm_commit(t) == 0);
  sm_closedb(t);
}

TEST_CASE("SM_ERR_UPDATED is not retryable and leaves the transaction active") {
  // Verify the failed write neither aborts nor releases the lock.
  // 1. T (read committed) begins; U commits a new "x".
  // 2. T's update of "x" fails with SM_ERR_UPDATED, not retryable.
  // 3. T is still active: it reads the newest "x", writes, commits.
  latest_fixture_t f;
  sm_session_t *t = f.open();
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  commit_from_other(f, "x", "u");

  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_UPDATED);
  CHECK(sm_retryable() == 0);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == 0);
  REQUIRE(sm_commit(t) == 0);
  sm_closedb(t);
}

TEST_CASE("a successful write consumes the mark") {
  // Verify a write after a savepoint rollback must re-read.
  // 1. T begins; U commits a new "x"; T locks "x" exclusively.
  // 2. T sets a savepoint, reads the latest "x" and writes it.
  // 3. T rolls back to the savepoint, undoing the write but
  //    keeping the lock.
  // 4. T's next write of "x" fails with SM_ERR_UPDATED again.
  latest_fixture_t f;
  sm_session_t *t = f.open();
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  commit_from_other(f, "x", "u");
  REQUIRE(sm_lock(t, g_rel, "x", 1, SM_LOCK_EXCLUSIVE) == 0);

  REQUIRE(sm_savepoint(t) == 1);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == 0);
  REQUIRE(sm_rollback(t, 1) == 0);
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_UPDATED);
  REQUIRE(sm_abort(t) == 0);
  sm_closedb(t);
}

TEST_CASE("under a shared lock sm_read_latest reads but sets no mark") {
  // Verify a shared holder sees the newest row but cannot write.
  // 1. T (read committed) takes a shared lock on "x"; a commit
  //    to "x" landed after T began.
  // 2. sm_read_latest returns the newest value.
  // 3. T's update, an upgrade, still fails with SM_ERR_UPDATED.
  latest_fixture_t f;
  sm_session_t *t = f.open();
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  commit_from_other(f, "x", "u");

  REQUIRE(sm_lock(t, g_rel, "x", 1, SM_LOCK_SHARED) == 0);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_UPDATED);
  REQUIRE(sm_abort(t) == 0);
  sm_closedb(t);
}
