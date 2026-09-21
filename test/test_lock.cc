/*
 * test_lock.cc -- row locks, first-committer-wins, retryable
 * errors, and deadlock detection (DESIGN.md §5, §6.2, §12.1).
 */

#include "doctest.h"
#include "util.h"

#include "session.h"
#include "sm.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

/* The fixture's relation, set when the fixture is created. */
uint64_t g_rel = 0;

/* A database "db" with one relation holding "x" -> "0", "y" -> "0". */
class lock_fixture_t {
public:
  /* Create "db", the relation, and the two rows. */
  lock_fixture_t() {
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

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("a writer blocks on a held row lock, then conflicts if the holder "
          "commits") {
  // Verify blocking and first-committer-wins after the wait.
  // 1. A updates "x" and holds its lock.
  // 2. B, in another thread, updates "x" and blocks.
  // 3. A commits; B's update fails with SM_ERR_CONFLICT, and
  //    B's transaction is already aborted (sm_abort: NOTRANS).
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(a, "x", "a") == 0);

  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = set(b, "x", "b");
    err = sm_errno();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_commit(a) == 0);
  tb.join();
  CHECK(rc == -1);
  CHECK(err == SM_ERR_CONFLICT);
  CHECK(sm_abort(b) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("a blocked writer proceeds when the holder aborts") {
  // Verify an aborted holder leaves no conflict behind.
  // 1. A updates "x"; B blocks updating "x".
  // 2. A aborts; B's update succeeds and B commits.
  // 3. A new transaction reads B's value.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(a, "x", "a") == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tb([&] {
    rc = set(b, "x", "b");
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_abort(a) == 0);
  tb.join();
  CHECK(rc == 0);
  REQUIRE(sm_commit(b) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(a, g_rel, "x") == "b");
  REQUIRE(sm_commit(a) == 0);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("first-committer-wins prevents lost updates at every level") {
  // Verify a write loses to a commit made after its snapshot.
  // 1. A and B begin and both read "x".
  // 2. A updates "x" and commits.
  // 3. B's update of "x" fails at once with SM_ERR_CONFLICT,
  //    at read committed as well as repeatable read.
  for (sm_xctlevel_t level :
       {SM_XCT_READ_COMMITTED, SM_XCT_REPEATABLE_READ, SM_XCT_SERIALIZABLE}) {
    CAPTURE(level);
    lock_fixture_t f;
    sm_session_t *a = f.open();
    sm_session_t *b = f.open();
    REQUIRE(sm_begin(a, level) == 0);
    REQUIRE(sm_begin(b, level) == 0);
    CHECK(get(a, g_rel, "x") == "0");
    CHECK(get(b, g_rel, "x") == "0");
    REQUIRE(set(a, "x", "a") == 0);
    REQUIRE(sm_commit(a) == 0);
    CHECK(set(b, "x", "b") == -1);
    CHECK(sm_errno() == SM_ERR_CONFLICT);
    CHECK(sm_retryable() == 1);
    sm_closedb(a);
    sm_closedb(b);
  }
}

TEST_CASE("read committed sees a newer commit but cannot overwrite it") {
  // Verify §6.2: RC reads at a statement snapshot, writes check the begin
  // one.
  // 1. T (read committed) writes "y", so it has an xid.
  // 2. U commits a new value of "x".
  // 3. T reads "x" and sees U's value (§10.1).
  // 4. T's update of "x" fails with SM_ERR_CONFLICT.
  lock_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(set(t, "y", "t") == 0);
  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm_commit(u) == 0);

  CHECK(get(t, g_rel, "x") == "u");
  CHECK(get(t, g_rel, "y") == "t");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_CONFLICT);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("a retryable error aborts the transaction and releases everything") {
  // Verify the engine-side abort on SM_ERR_CONFLICT (§12.1).
  // 1. T creates a relation, inserts into it, and locks "y".
  // 2. U commits a change to "x"; T's update of "x" conflicts.
  // 3. T's relation is gone, and another session can lock "y"
  //    at once and create a relation itself.
  lock_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t trel = mkrel(t);
  put(t, trel, "k", "v");
  REQUIRE(set(t, "y", "t") == 0);
  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm_commit(u) == 0);
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_CONFLICT);

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(set(u, "y", "u") == 0); // would block if T still held it
  const char *d;
  size_t dl;
  CHECK(sm_read(u, trel, "k", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  CHECK(mkrel(u) > trel);
  REQUIRE(sm_commit(u) == 0);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("concurrent DDL: separate relations commit, the same relid "
          "conflicts") {
  // Verify DDL goes through the row locks (§11, §12.5 DDL).
  // 1. A and B each create a relation; both commit, since each
  //    writes a catalog row of its own.
  // 2. A drops A's relation; B, in another thread, drops the
  //    same relation and blocks on its catalog row.
  // 3. A commits; B fails with SM_ERR_CONFLICT.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t arel = mkrel(a);
  uint64_t brel = mkrel(b);
  CHECK(arel != brel);
  REQUIRE(sm_commit(a) == 0);
  REQUIRE(sm_commit(b) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmrel(a, arel) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = sm_rmrel(b, arel);
    err = sm_errno();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_commit(a) == 0);
  tb.join();
  CHECK(rc == -1);
  CHECK(err == SM_ERR_CONFLICT);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("the youngest transaction in a deadlock cycle is the victim") {
  // Verify deadlock detection picks the youngest, whoever detects.
  // 1. A begins, then B (younger). A locks "x", B locks "y".
  // 2. Each then requests the other's row, in both orders:
  //    older-blocks-first (A detects, B is the victim) and
  //    younger-blocks-first (B detects and is the victim).
  // 3. B's blocked update fails with SM_ERR_DEADLOCK; A's
  //    proceeds and A commits.
  for (bool older_first : {true, false}) {
    CAPTURE(older_first);
    lock_fixture_t f;
    sm_session_t *a = f.open();
    sm_session_t *b = f.open();
    REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(set(a, "x", "a") == 0);
    REQUIRE(set(b, "y", "b") == 0);

    int rca = -2, rcb = -2, errb = 0;
    auto run_a = [&] { rca = set(a, "y", "a"); };
    auto run_b = [&] {
      rcb = set(b, "x", "b");
      errb = sm_errno();
    };
    std::thread first(older_first ? std::function<void()>(run_a)
                                  : std::function<void()>(run_b));
    settle();
    std::thread second(older_first ? std::function<void()>(run_b)
                                   : std::function<void()>(run_a));
    first.join();
    second.join();

    CHECK(rcb == -1);
    CHECK(errb == SM_ERR_DEADLOCK);
    CHECK(rca == 0);
    REQUIRE(sm_commit(a) == 0);
    CHECK(sm_abort(b) == -1); // already aborted by the engine
    sm_closedb(a);
    sm_closedb(b);
  }
}

TEST_CASE("a doomed transaction fails its next call and changes nothing") {
  // Verify the doomed flag stops a transaction that is not waiting (§5.1).
  // 1. T updates "x", then is marked doomed as a deadlock victim
  //    would be while running.
  // 2. T's next call, a read, fails with SM_ERR_DEADLOCK.
  // 3. T's update was discarded and its lock released.
  lock_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(t, "x", "t") == 0);
  t->doomed = SM_ERR_DEADLOCK;
  const char *d;
  size_t dl;
  CHECK(sm_read(t, g_rel, "x", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_DEADLOCK);
  CHECK(sm_retryable() == 1);

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, g_rel, "x") == "0");
  CHECK(set(u, "x", "u") == 0);
  REQUIRE(sm_commit(u) == 0);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("concurrent increments with retry never lose an update") {
  // Verify lost-update freedom under contention, at RC and RR.
  // 1. Four threads each add 1 to "x" fifty times, reading,
  //    then updating, and retrying on any retryable error.
  // 2. The final value is exactly 200.
  for (sm_xctlevel_t level : {SM_XCT_READ_COMMITTED, SM_XCT_REPEATABLE_READ}) {
    CAPTURE(level);
    lock_fixture_t f;
    std::vector<sm_session_t *> sessions;
    for (int t = 0; t < 4; t++) {
      sessions.push_back(f.open());
    }
    std::atomic<int> errors{0}, retries{0};
    std::vector<std::thread> threads;
    for (sm_session_t *s : sessions) {
      threads.emplace_back([&, s] {
        for (int i = 0; i < 50;) {
          const char *d;
          size_t dl;
          int ok = sm_begin(s, level) == 0 &&
                   sm_read(s, g_rel, "x", 1, &d, &dl) == 0;
          if (ok) {
            std::string v = std::to_string(std::stoi(std::string(d, dl)) + 1);
            ok = set(s, "x", v) == 0 && sm_commit(s) == 0;
          }
          if (ok) {
            i++;
          } else if (sm_retryable()) {
            retries++;
          } else {
            errors++;
            sm_abort(s);
            return;
          }
        }
      });
    }
    for (std::thread &t : threads) {
      t.join();
    }
    CHECK(errors == 0);
    sm_session_t *s = sessions[0];
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(s, g_rel, "x") == "200");
    REQUIRE(sm_commit(s) == 0);
    MESSAGE("retries: " << retries.load());
    for (sm_session_t *x : sessions) {
      sm_closedb(x);
    }
  }
}

TEST_CASE("shared locks coexist; an exclusive one waits for all of them") {
  // Verify S/S compatibility and S/X conflict (DESIGN.md §5.1).
  // 1. A and B both lock "x" shared; neither blocks.
  // 2. C, in another thread, locks "x" exclusive and blocks.
  // 3. A commits; C still blocks, because B holds it too.
  // 4. B commits; C gets the lock and commits.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  sm_session_t *c = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(a, g_rel, "x", 1, SM_LOCK_SHARED) == 0);
  REQUIRE(sm_lock(b, g_rel, "x", 1, SM_LOCK_SHARED) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tc([&] {
    rc = sm_lock(c, g_rel, "x", 1, SM_LOCK_EXCLUSIVE);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_commit(a) == 0);
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_commit(b) == 0);
  tc.join();
  CHECK(rc == 0);
  REQUIRE(sm_commit(c) == 0);
  sm_closedb(a);
  sm_closedb(b);
  sm_closedb(c);
}

TEST_CASE("a shared lock blocks a writer, and a writer blocks a shared lock") {
  // Verify the lock sm_lock takes is the one writes take.
  // 1. A locks "x" shared; B's update of "x" blocks until A ends.
  // 2. A updates "y", taking it exclusive; B's shared lock on
  //    "y" blocks until A ends.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(a, g_rel, "x", 1, SM_LOCK_SHARED) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tb([&] {
    rc = set(b, "x", "b");
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_abort(a) == 0);
  tb.join();
  CHECK(rc == 0);
  REQUIRE(sm_abort(b) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(a, "y", "a") == 0);
  returned = false;
  rc = -2;
  std::thread tb2([&] {
    rc = sm_lock(b, g_rel, "y", 1, SM_LOCK_SHARED);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_abort(a) == 0);
  tb2.join();
  CHECK(rc == 0);
  REQUIRE(sm_abort(b) == 0);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("a shared holder upgrades by writing, once the others are gone") {
  // Verify the S -> X upgrade (DESIGN.md §5.1).
  // 1. A and B hold "x" shared.
  // 2. A writes "x": the upgrade blocks on B.
  // 3. B commits without touching "x"; A's write completes.
  // 4. A holds "x" exclusive afterwards, so C's shared lock
  //    blocks; when A commits, C's snapshot is stale and the
  //    lock fails with SM_ERR_CONFLICT (§5.2).
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  sm_session_t *c = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(a, g_rel, "x", 1, SM_LOCK_SHARED) == 0);
  REQUIRE(sm_lock(b, g_rel, "x", 1, SM_LOCK_SHARED) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread ta([&] {
    rc = set(a, "x", "a");
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_commit(b) == 0);
  ta.join();
  CHECK(rc == 0);

  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  std::atomic<bool> c_returned{false};
  int rc_c = -2, err_c = 0;
  std::thread tc([&] {
    rc_c = sm_lock(c, g_rel, "x", 1, SM_LOCK_SHARED);
    err_c = sm_errno();
    c_returned = true;
  });
  settle();
  CHECK_FALSE(c_returned);
  REQUIRE(sm_commit(a) == 0);
  tc.join();
  CHECK(rc_c == -1);
  CHECK(err_c == SM_ERR_CONFLICT);
  sm_closedb(a);
  sm_closedb(b);
  sm_closedb(c);
}

TEST_CASE("two upgraders on one row deadlock, and the younger one loses") {
  // Verify the cycle a shared lock makes possible (DESIGN.md §5.1).
  // 1. A (older) and B (younger) both hold "x" shared.
  // 2. Both try to write "x"; each waits for the other.
  // 3. The younger, B, is doomed with SM_ERR_DEADLOCK, and A
  //    completes its write.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(a, g_rel, "x", 1, SM_LOCK_SHARED) == 0);
  REQUIRE(sm_lock(b, g_rel, "x", 1, SM_LOCK_SHARED) == 0);

  int rc_a = -2, rc_b = -2, err_b = 0;
  std::thread ta([&] { rc_a = set(a, "x", "a"); });
  std::thread tb([&] {
    rc_b = set(b, "x", "b");
    err_b = sm_errno();
  });
  ta.join();
  tb.join();
  CHECK(rc_a == 0);
  CHECK(rc_b == -1);
  CHECK(err_b == SM_ERR_DEADLOCK);
  REQUIRE(sm_commit(a) == 0);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("sm_lock fails with CONFLICT on a row committed after the snapshot") {
  // Verify §5.2's check runs once the lock is held, so what the
  // caller reads under it is current.
  // 1. A begins and takes its snapshot.
  // 2. B updates "x" and commits.
  // 3. A's sm_lock on "x" fails with SM_ERR_CONFLICT, in either
  //    mode, and its transaction is aborted.
  for (sm_lockmode_t mode : {SM_LOCK_SHARED, SM_LOCK_EXCLUSIVE}) {
    CAPTURE(mode);
    lock_fixture_t f;
    sm_session_t *a = f.open();
    sm_session_t *b = f.open();
    REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(a, g_rel, "x") == "0");

    REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(set(b, "x", "b") == 0);
    REQUIRE(sm_commit(b) == 0);

    CHECK(sm_lock(a, g_rel, "x", 1, mode) == -1);
    CHECK(sm_errno() == SM_ERR_CONFLICT);
    CHECK(sm_retryable() == 1);
    CHECK(sm_abort(a) == -1);
    CHECK(sm_errno() == SM_ERR_NOTRANS);
    sm_closedb(a);
    sm_closedb(b);
  }
}

TEST_CASE("sm_lock follows the savepoint rules, and never downgrades") {
  // Verify §6.5's lock rule for explicit locks.
  // 1. A locks "x" shared, marks, then locks "y" shared and
  //    upgrades "x" by writing it.
  // 2. A rolls back to the mark: "y" is free, "x" is still held
  //    exclusive.
  // 3. B can lock "y" at once, but blocks on "x" until A ends.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_lock(a, g_rel, "x", 1, SM_LOCK_SHARED) == 0);
  REQUIRE(sm_savepoint(a) == 1);
  REQUIRE(sm_lock(a, g_rel, "y", 1, SM_LOCK_SHARED) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  REQUIRE(sm_rollback(a, 1) == 0);

  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_lock(b, g_rel, "y", 1, SM_LOCK_SHARED) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tb([&] {
    rc = sm_lock(b, g_rel, "x", 1, SM_LOCK_SHARED);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  REQUIRE(sm_abort(a) == 0);
  tb.join();
  CHECK(rc == 0);
  REQUIRE(sm_abort(b) == 0);
  sm_closedb(a);
  sm_closedb(b);
}

TEST_CASE("the index-build protocol blocks writers, then makes them retry") {
  // Verify SECINDEX.md gap 1's protocol end to end: a builder
  // cannot miss a concurrent writer's rows.
  // 1. A catalog relation holds a schema row saying "no index".
  // 2. Writer W takes it shared, reads it, writes a base row,
  //    and commits.
  // 3. Builder B takes it exclusive by updating it. Writer X,
  //    beginning after B, blocks on the shared lock, so no
  //    writer can run while B scans the base relation.
  // 4. B sees W's row, publishes the index, and commits.
  // 5. X's shared lock then fails with SM_ERR_CONFLICT, because
  //    the schema row changed after its snapshot; on retry it
  //    reads the new schema and maintains the index.
  lock_fixture_t f;
  sm_session_t *w = f.open();
  sm_session_t *b = f.open();
  sm_session_t *x = f.open();

  // The layer's catalog: one schema row per base relation.
  REQUIRE(sm_begin(w, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t cat = mkrel(w);
  put(w, cat, "S", "noindex");
  REQUIRE(sm_commit(w) == 0);

  // A writer reads the schema under a shared lock, then writes.
  REQUIRE(sm_begin(w, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_lock(w, cat, "S", 1, SM_LOCK_SHARED) == 0);
  CHECK(get(w, cat, "S") == "noindex");
  put(w, g_rel, "w-row", "1");
  REQUIRE(sm_commit(w) == 0);

  // The builder takes the schema row exclusive.
  REQUIRE(sm_begin(b, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_update(b, cat, "S", 1, "indexed", 7, nullptr, nullptr) == 0);

  // A writer arriving now waits for the build to finish.
  REQUIRE(sm_begin(x, SM_XCT_READ_COMMITTED) == 0);
  std::atomic<bool> returned{false};
  int rc = -2, err = 0;
  std::thread tx([&] {
    rc = sm_lock(x, cat, "S", 1, SM_LOCK_SHARED);
    err = sm_errno();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  // The builder's scan sees the committed writer's row.
  CHECK(get(b, g_rel, "w-row") == "1");
  REQUIRE(sm_commit(b) == 0);

  // The blocked writer is stale, and must retry.
  tx.join();
  CHECK(rc == -1);
  CHECK(err == SM_ERR_CONFLICT);
  REQUIRE(sm_begin(x, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_lock(x, cat, "S", 1, SM_LOCK_SHARED) == 0);
  CHECK(get(x, cat, "S") == "indexed");
  REQUIRE(sm_commit(x) == 0);
  sm_closedb(w);
  sm_closedb(b);
  sm_closedb(x);
}

TEST_CASE("sm_lock rejects bad arguments and needs a transaction") {
  // Verify the call's contract.
  // 1. A NULL session, an unknown mode, and relid 0 are
  //    SM_ERR_INVALID; an unknown relation is SM_ERR_NOTFOUND.
  // 2. Without a transaction it fails with SM_ERR_NOTRANS.
  // 3. A key that does not exist is lockable.
  lock_fixture_t f;
  sm_session_t *s = f.open();
  CHECK(sm_lock(nullptr, g_rel, "x", 1, SM_LOCK_SHARED) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_lock(s, g_rel, "x", 1, SM_LOCK_SHARED) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_lock(s, g_rel, "x", 1, (sm_lockmode_t)7) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_lock(s, 0, "x", 1, SM_LOCK_SHARED) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_lock(s, g_rel + 999, "x", 1, SM_LOCK_SHARED) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  CHECK(sm_lock(s, g_rel, "absent", 6, SM_LOCK_EXCLUSIVE) == 0);
  REQUIRE(sm_commit(s) == 0);
  sm_closedb(s);
}
