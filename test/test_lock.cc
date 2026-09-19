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

/* A database "db" with relation 1 holding "x" -> "0", "y" -> "0". */
class lock_fixture_t {
public:
  /* Create "db", relation 1, and the two rows. */
  lock_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    sm_session_t *s = open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, 1) == 0);
    put(s, 1, "x", "0");
    put(s, 1, "y", "0");
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
  return sm_update(s, 1, key.data(), key.size(), val.data(), val.size());
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
  CHECK(get(a, 1, "x") == "b");
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
    CHECK(get(a, 1, "x") == "0");
    CHECK(get(b, 1, "x") == "0");
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

  CHECK(get(t, 1, "x") == "u");
  CHECK(get(t, 1, "y") == "t");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm_errno() == SM_ERR_CONFLICT);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("a retryable error aborts the transaction and releases everything") {
  // Verify the engine-side abort on SM_ERR_CONFLICT (§12.1).
  // 1. T creates relation 2, inserts into it, and locks "y".
  // 2. U commits a change to "x"; T's update of "x" conflicts.
  // 3. Relation 2 is gone, and another session can lock "y" at once
  //    and create relation 2 itself.
  lock_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(t, 2) == 0);
  put(t, 2, "k", "v");
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
  CHECK(sm_read(u, 2, "k", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  CHECK(sm_mkrel(u, 2) == 0);
  REQUIRE(sm_commit(u) == 0);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("concurrent sm_mkrel: different relids both commit, the same relid "
          "conflicts") {
  // Verify DDL goes through the row locks (§11, §12.5 DDL).
  // 1. A and B create relation 3 and relation 4; both commit.
  // 2. A creates relation 5; B, in another thread, creates relation 5 and
  //    blocks on the catalog row.
  // 3. A commits; B fails with SM_ERR_CONFLICT.
  lock_fixture_t f;
  sm_session_t *a = f.open();
  sm_session_t *b = f.open();
  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(a, 3) == 0);
  REQUIRE(sm_mkrel(b, 4) == 0);
  REQUIRE(sm_commit(a) == 0);
  REQUIRE(sm_commit(b) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(a, 5) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = sm_mkrel(b, 5);
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
  CHECK(sm_read(t, 1, "x", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_DEADLOCK);
  CHECK(sm_retryable() == 1);

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, 1, "x") == "0");
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
          int ok =
              sm_begin(s, level) == 0 && sm_read(s, 1, "x", 1, &d, &dl) == 0;
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
    CHECK(get(s, 1, "x") == "200");
    REQUIRE(sm_commit(s) == 0);
    MESSAGE("retries: " << retries.load());
    for (sm_session_t *x : sessions) {
      sm_closedb(x);
    }
  }
}
