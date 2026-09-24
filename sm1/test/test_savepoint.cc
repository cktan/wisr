/*
 * test_savepoint.cc -- sm1_savepoint and sm1_rollback (SM1_DESIGN.md
 * §6.5, §12.5 "Own writes").
 *
 * The lock rule these calls obey is a timeline between two
 * sessions, so it lives in mvcc/in/b25_savepoint_releases_lock.t
 * and b31_savepoint_and_explicit_locks.t. What stays here is
 * the single-session part: values restored, the stack and its
 * bounds, cursors closed, and DDL undone.
 */

#include "doctest.h"
#include "util.h"

#include "session.h"
#include "sm1.h"

#include <string>
#include <utility>
#include <vector>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "a" -> "0". */
class sp_fixture_t {
public:
  /* Create "db", the store, and the row. */
  sp_fixture_t() {
    REQUIRE(sm1_mkdb(m_home.env(), "db", nullptr) == 0);
    sm1_session_t *s = open();
    REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
    g_store = mkstore(s);
    put(s, g_store, "a", "0");
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_closedb(s) == 0);
  }

  /* A new session on "db". */
  sm1_session_t *open() const {
    sm1_session_t *s = sm1_opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

private:
  home_t m_home;
};

/* Set key in store 1 to val; the result of sm1_update. */
int set(sm1_session_t *s, const std::string &key, const std::string &val) {
  return sm1_update(s, g_store, key.data(), key.size(), val.data(), val.size(),
                    nullptr, nullptr);
}

/* Delete key from store 1; the result of sm1_delete. */
int del(sm1_session_t *s, const std::string &key) {
  return sm1_delete(s, g_store, key.data(), key.size(), nullptr, nullptr);
}

/* Drain a cursor into its keys and close it. */
std::vector<std::string> keys_of(sm1_scan_t *c) {
  REQUIRE_MESSAGE(c, sm1_errmsg());
  std::vector<std::string> out;
  const char *k, *d;
  size_t kl, dl;
  while (sm1_scannxt(c, &k, &kl, &d, &dl) == 0) {
    out.emplace_back(k, kl);
  }
  sm1_scanend(c);
  return out;
}

} // namespace

TEST_CASE("rollback restores the values as of the savepoint, at every level") {
  // Verify rollback undoes inserts, updates, and deletes after the mark.
  // 1. Before the mark: update "a" to "1", insert "b".
  // 2. Set the savepoint.
  // 3. After it: update "a" twice, delete "b", insert "c",
  //    re-insert "b".
  // 4. Roll back: "a" is "1", "b" is "1", "c" is absent.
  // 5. Roll back again to the same mark; still the same.
  // 6. Write after the rollback and commit; a new transaction
  //    sees the post-rollback writes and nothing undone.
  for (sm1_xctlevel_t level : {SM1_XCT_READ_COMMITTED, SM1_XCT_REPEATABLE_READ,
                               SM1_XCT_SERIALIZABLE}) {
    CAPTURE(level);
    sp_fixture_t f;
    sm1_session_t *s = f.open();
    REQUIRE(sm1_begin(s, level) == 0);
    REQUIRE(set(s, "a", "1") == 0);
    put(s, g_store, "b", "1");
    REQUIRE(sm1_savepoint(s) == 1);

    REQUIRE(set(s, "a", "2") == 0);
    REQUIRE(set(s, "a", "3") == 0);
    REQUIRE(del(s, "b") == 0);
    put(s, g_store, "c", "2");
    put(s, g_store, "b", "2");
    CHECK(get(s, g_store, "a") == "3");

    for (int round = 0; round < 2; round++) {
      CAPTURE(round);
      REQUIRE(sm1_rollback(s, 1) == 0);
      CHECK(get(s, g_store, "a") == "1");
      CHECK(get(s, g_store, "b") == "1");
      CHECK(get(s, g_store, "c") == "<none>");
      CHECK(keys_of(sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
            std::vector<std::string>{"a", "b"});
    }

    REQUIRE(set(s, "a", "4") == 0);
    put(s, g_store, "d", "4");
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
    CHECK(get(s, g_store, "a") == "4");
    CHECK(get(s, g_store, "b") == "1");
    CHECK(get(s, g_store, "c") == "<none>");
    CHECK(get(s, g_store, "d") == "4");
    REQUIRE(sm1_commit(s) == 0);
    sm1_closedb(s);
  }
}

TEST_CASE("rollback needs a savepoint, and a savepoint lives only in its "
          "transaction") {
  // Verify SM_ERR_NOSAVEPOINT and the savepoint's lifetime.
  // 1. Rollback without a savepoint fails with SM_ERR_NOSAVEPOINT.
  // 2. Savepoint, commit; in the next transaction rollback
  //    fails again: commit discarded the savepoint.
  // 3. A depth outside the stack fails the same way.
  // 4. Savepoint right after begin rolls back to the start.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  CHECK(sm1_savepoint(s) == -1);
  CHECK(sm1_errno() == SM_ERR_NOTRANS);

  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(sm1_rollback(s, 1) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);
  REQUIRE(sm1_savepoint(s) == 1);
  REQUIRE(sm1_commit(s) == 0);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(sm1_rollback(s, 1) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);

  REQUIRE(sm1_savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  CHECK(sm1_rollback(s, 2) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);
  CHECK(sm1_rollback(s, 0) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);
  CHECK(sm1_forget(s, 2) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);
  REQUIRE(sm1_rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(sm1_abort(s) == 0);

  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm1_savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  put(s, g_store, "b", "1");
  REQUIRE(sm1_rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  CHECK(get(s, g_store, "b") == "<none>");
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
}

TEST_CASE("savepoints nest, and rolling back to one drops those inside it") {
  // Verify the stack: depths, partial undo, and reuse of a mark.
  // 1. Push three savepoints, writing a distinct value before
  //    each; the depths returned are 1, 2, 3.
  // 2. Roll back to depth 2: writes after it are gone, writes
  //    before it stay, and depth 3 is gone with them.
  // 3. Depth 2 survives, so a new write and a second rollback
  //    to it work; a fresh push lands at depth 3 again.
  // 4. Rolling back to depth 1 undoes the lot.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_savepoint(s) == 1);
  REQUIRE(set(s, "a", "2") == 0);
  REQUIRE(sm1_savepoint(s) == 2);
  REQUIRE(set(s, "a", "3") == 0);
  REQUIRE(sm1_savepoint(s) == 3);
  REQUIRE(set(s, "a", "4") == 0);
  put(s, g_store, "b", "4");

  REQUIRE(sm1_rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  CHECK(get(s, g_store, "b") == "<none>");
  CHECK(sm1_rollback(s, 3) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);

  REQUIRE(set(s, "a", "5") == 0);
  REQUIRE(sm1_rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  REQUIRE(sm1_savepoint(s) == 3);

  REQUIRE(sm1_rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1_commit(s) == 0);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
}

TEST_CASE("sm1_forget drops a mark and those inside it, keeping the writes") {
  // Verify RELEASE SAVEPOINT semantics and the depth reuse it causes.
  // 1. Push two savepoints with a write after each.
  // 2. Forget depth 2: both writes stay, and depth 2 can no
  //    longer be rolled back to.
  // 3. A new savepoint takes depth 2 again, so a stale depth
  //    now names a different mark -- the documented hazard.
  // 4. Rolling back to depth 1 still undoes everything after it.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm1_savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_savepoint(s) == 2);
  REQUIRE(set(s, "a", "2") == 0);

  REQUIRE(sm1_forget(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  CHECK(sm1_rollback(s, 2) == -1);
  CHECK(sm1_errno() == SM_ERR_NOSAVEPOINT);

  REQUIRE(sm1_savepoint(s) == 2);
  REQUIRE(set(s, "a", "3") == 0);
  REQUIRE(sm1_rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");

  REQUIRE(sm1_rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
}

TEST_CASE("the savepoint stack is bounded, and forgetting makes room") {
  // Verify SM1_MAX_SAVEPOINTS.
  // 1. Push SM1_MAX_SAVEPOINTS savepoints; each returns its depth.
  // 2. The next push fails with SM_ERR_INVALID.
  // 3. Forgetting the innermost mark makes room for one more.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  for (int depth = 1; depth <= SM1_MAX_SAVEPOINTS; depth++) {
    REQUIRE(sm1_savepoint(s) == depth);
  }
  CHECK(sm1_savepoint(s) == -1);
  CHECK(sm1_errno() == SM_ERR_INVALID);

  REQUIRE(sm1_forget(s, SM1_MAX_SAVEPOINTS) == 0);
  CHECK(sm1_savepoint(s) == SM1_MAX_SAVEPOINTS);
  REQUIRE(sm1_abort(s) == 0);
  sm1_closedb(s);
}

TEST_CASE(
    "cursors before the mark survive rollback; cursors after it are closed") {
  // Verify §6.5's cursor rule and the seq rewind.
  // 1. Insert "b"; open cursor C1; set the savepoint.
  // 2. Insert "c"; open cursor C2.
  // 3. Roll back: C2 is closed, C1 stays open.
  // 4. Insert "d" (it reuses the undone seq number).
  // 5. C1 returns exactly "a" and "b": no row written after
  //    the mark, and nothing written after C1 opened.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  put(s, g_store, "b", "1");
  sm1_scan_t *c1 = sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c1);
  REQUIRE(sm1_savepoint(s) == 1);
  put(s, g_store, "c", "2");
  sm1_scan_t *c2 = sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c2);
  CHECK(s->scans.size() == 2);

  REQUIRE(sm1_rollback(s, 1) == 0);
  CHECK(s->scans.size() == 1);
  CHECK(s->scans.count(c1) == 1);
  put(s, g_store, "d", "3");
  CHECK(keys_of(c1) == std::vector<std::string>{"a", "b"});
  CHECK(keys_of(sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a", "b", "d"});
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
}

TEST_CASE("rollback undoes sm1_mkstore and sm1_rmstore made after the mark") {
  // Verify DDL follows the savepoint (§6.5, §11).
  // 1. Create store P before the mark; set the savepoint.
  // 2. Create store Q and write to it; drop the fixture's.
  // 3. Roll back: P exists, Q does not, and the fixture's
  //    store is back with its rows.
  // 4. Commit; a new transaction sees the same. A later
  //    sm1_mkstore hands out a storeid of its own.
  sp_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  uint64_t p = mkstore(s);
  REQUIRE(sm1_savepoint(s) == 1);
  uint64_t q = mkstore(s);
  put(s, q, "k", "v");
  REQUIRE(sm1_rmstore(s, g_store) == 0);
  REQUIRE(sm1_rollback(s, 1) == 0);

  for (int pass = 0; pass < 2; pass++) {
    CAPTURE(pass);
    const char *d;
    size_t dl;
    CHECK(sm1_read(s, p, "k", 1, &d, &dl) == 1);
    CHECK(sm1_read(s, q, "k", 1, &d, &dl) == -1);
    CHECK(sm1_errno() == SM_ERR_NOTFOUND);
    CHECK(get(s, g_store, "a") == "0");
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  }
  CHECK(mkstore(s) > q); // a fresh storeid, never q again
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
}
