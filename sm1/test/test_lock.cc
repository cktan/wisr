/*
 * test_lock.cc -- row locks, first-committer-wins, retryable
 * errors, and deadlock detection (SM1_DESIGN.md §5, §6.2, §12.1).
 *
 * The timelines two sessions play out against each other live
 * in mvcc/in/ as scenario scripts, where blocking is one word
 * rather than a thread and a sleep. What stays here is what a
 * script cannot say: error codes and argument handling, engine
 * state a script cannot see, contention run under load, and
 * the index-build protocol, which needs a write to a second
 * store the driver's language has no verb for.
 */

#include "doctest.h"
#include "util.h"

#include "session.h"
#include "sm1.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "x" -> "0", "y" -> "0". */
class lock_fixture_t {
public:
  /* Create "db", the store, and the two rows. */
  lock_fixture_t() {
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

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("first-committer-wins prevents lost updates at every level") {
  // Verify a write loses to a commit made after its snapshot.
  // 1. A and B begin and both read "x".
  // 2. A updates "x" and commits.
  // 3. B's update of "x" fails at once. At repeatable read and
  //    serializable that is SM_ERR_CONFLICT, retryable; at read
  //    committed it is SM_ERR_UPDATED, which is not (SM1_DESIGN.md §5.4).
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(level);
    lock_fixture_t f;
    sm1::session_t *a = f.open();
    sm1::session_t *b = f.open();
    REQUIRE(sm1::begin(a, level) == 0);
    REQUIRE(sm1::begin(b, level) == 0);
    CHECK(get(a, g_store, "x") == "0");
    CHECK(get(b, g_store, "x") == "0");
    REQUIRE(set(a, "x", "a") == 0);
    REQUIRE(sm1::commit(a) == 0);
    CHECK(set(b, "x", "b") == -1);
    bool rc = level == sm1::xctlevel_t::read_committed;
    CHECK(sm1::errnum() == (rc ? sm1::SM_ERR_UPDATED : sm1::SM_ERR_CONFLICT));
    CHECK(sm1::retryable() == (rc ? 0 : 1));
    sm1::closedb(a);
    sm1::closedb(b);
  }
}

TEST_CASE("read committed sees a newer commit and re-reads it to overwrite") {
  // Verify §6.2: RC reads at a statement snapshot; a write over a newer
  // commit needs sm1::read_latest first.
  // 1. T (read committed) writes "y", so it has an xid.
  // 2. U commits a new value of "x".
  // 3. T reads "x" and sees U's value (§10.1).
  // 4. T's update of "x" fails with SM_ERR_UPDATED, and T stays active.
  // 5. After sm1::read_latest of "x", the update goes ahead.
  lock_fixture_t f;
  sm1::session_t *t = f.open();
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(t, "y", "t") == 0);
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm1::commit(u) == 0);

  CHECK(get(t, g_store, "x") == "u");
  CHECK(get(t, g_store, "y") == "t");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  std::string_view d;
  REQUIRE(sm1::read_latest(t, g_store, "x", d) == 0);
  CHECK(std::string(d) == "u");
  CHECK(set(t, "x", "t") == 0);
  REQUIRE(sm1::commit(t) == 0);
  sm1::closedb(t);
  sm1::closedb(u);
}

TEST_CASE("a retryable error aborts the transaction and releases everything") {
  // Verify the engine-side abort on SM_ERR_CONFLICT (§12.1).
  // 1. T creates a store, inserts into it, and locks "y".
  // 2. U commits a change to "x"; T's update of "x" conflicts.
  // 3. T's store is gone, and another session can lock "y"
  //    at once and create a store itself.
  lock_fixture_t f;
  sm1::session_t *t = f.open();
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t tstore = mkstore(t);
  put(t, tstore, "k", "v");
  REQUIRE(set(t, "y", "t") == 0);
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm1::commit(u) == 0);
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(set(u, "y", "u") == 0); // would block if T still held it
  std::string_view d;
  CHECK(sm1::read(u, tstore, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(mkstore(u) > tstore);
  REQUIRE(sm1::commit(u) == 0);
  sm1::closedb(t);
  sm1::closedb(u);
}

TEST_CASE("concurrent sm1_mkstore hands out a storeid of its own to each") {
  // Verify two transactions creating stores do not collide;
  // the storeids themselves are what no scenario script can see.
  // 1. A and B each create a store in their own transaction.
  // 2. The storeids differ, and both commit, since each writes a
  //    catalog row of its own.
  // The blocked half -- two transactions dropping ONE store
  // -- is mvcc/in/b32_concurrent_drop_of_one_relation.t.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t astore = mkstore(a);
  uint64_t bstore = mkstore(b);
  CHECK(astore != bstore);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a doomed transaction fails its next call and changes nothing") {
  // Verify the doomed flag stops a transaction that is not waiting (§5.1).
  // 1. T updates "x", then is marked doomed as a deadlock victim
  //    would be while running.
  // 2. T's next call, a read, fails with SM_ERR_DEADLOCK.
  // 3. T's update was discarded and its lock released.
  lock_fixture_t f;
  sm1::session_t *t = f.open();
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(t, "x", "t") == 0);
  t->doomed = sm1::SM_ERR_DEADLOCK;
  std::string_view d;
  CHECK(sm1::read(t, g_store, "x", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DEADLOCK);
  CHECK(sm1::retryable() == 1);

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(u, g_store, "x") == "0");
  CHECK(set(u, "x", "u") == 0);
  REQUIRE(sm1::commit(u) == 0);
  sm1::closedb(t);
  sm1::closedb(u);
}

TEST_CASE("concurrent increments with retry never lose an update") {
  // Verify lost-update freedom under contention, at RC and RR.
  // 1. Four threads each add 1 to "x" fifty times, reading,
  //    then updating, and retrying on any retryable error.
  // 2. The final value is exactly 200. At read committed a write
  //    that fails with SM_ERR_UPDATED re-reads the newest value
  //    with sm1::read_latest and writes it plus one; it needs no
  //    retry.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read}) {
    CAPTURE(level);
    lock_fixture_t f;
    std::vector<sm1::session_t *> sessions;
    for (int t = 0; t < 4; t++) {
      sessions.push_back(f.open());
    }
    std::atomic<int> errors{0}, retries{0};
    std::vector<std::thread> threads;
    for (sm1::session_t *s : sessions) {
      threads.emplace_back([&, s] {
        for (int i = 0; i < 50;) {
          std::string_view d;
          int ok =
              sm1::begin(s, level) == 0 && sm1::read(s, g_store, "x", d) == 0;
          if (ok) {
            std::string v = std::to_string(std::stoi(std::string(d)) + 1);
            ok = set(s, "x", v) == 0;
            // Read committed: re-read the newest value, add one again.
            if (!ok && sm1::errnum() == sm1::SM_ERR_UPDATED) {
              ok = sm1::read_latest(s, g_store, "x", d) == 0;
              v = std::to_string(std::stoi(std::string(d)) + 1);
              ok = ok && set(s, "x", v) == 0;
            }
            ok = ok && sm1::commit(s) == 0;
          }
          if (ok) {
            i++;
          } else if (sm1::retryable()) {
            retries++;
          } else {
            errors++;
            sm1::abort(s);
            return;
          }
        }
      });
    }
    for (std::thread &t : threads) {
      t.join();
    }
    CHECK(errors == 0);
    sm1::session_t *s = sessions[0];
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(get(s, g_store, "x") == "200");
    REQUIRE(sm1::commit(s) == 0);
    MESSAGE("retries: " << retries.load());
    for (sm1::session_t *x : sessions) {
      sm1::closedb(x);
    }
  }
}

TEST_CASE("two upgraders on one row deadlock, and the younger one loses") {
  // Verify the cycle a shared lock makes possible (SM1_DESIGN.md §5.1).
  // 1. A (older) and B (younger) both hold "x" shared.
  // 2. Both try to write "x"; each waits for the other.
  // 3. The younger, B, is doomed with SM_ERR_DEADLOCK, and A
  //    completes its write.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock(a, g_store, "x", sm1::lockmode_t::shared) == 0);
  REQUIRE(sm1::lock(b, g_store, "x", sm1::lockmode_t::shared) == 0);

  int rc_a = -2, rc_b = -2, err_b = 0;
  std::thread ta([&] { rc_a = set(a, "x", "a"); });
  std::thread tb([&] {
    rc_b = set(b, "x", "b");
    err_b = sm1::errnum();
  });
  ta.join();
  tb.join();
  CHECK(rc_a == 0);
  CHECK(rc_b == -1);
  CHECK(err_b == sm1::SM_ERR_DEADLOCK);
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE(
    "sm1_lock fails with CONFLICT on a row committed after the snapshot") {
  // Verify §5.2's check runs once the lock is held, so what the
  // caller reads under it is current.
  // 1. A begins and takes its snapshot.
  // 2. B updates "x" and commits.
  // 3. A's sm1::lock on "x" fails with SM_ERR_CONFLICT, in either
  //    mode, and its transaction is aborted.
  for (sm1::lockmode_t mode :
       {sm1::lockmode_t::shared, sm1::lockmode_t::exclusive}) {
    CAPTURE(mode);
    lock_fixture_t f;
    sm1::session_t *a = f.open();
    sm1::session_t *b = f.open();
    REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(get(a, g_store, "x") == "0");

    REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(set(b, "x", "b") == 0);
    REQUIRE(sm1::commit(b) == 0);

    CHECK(sm1::lock(a, g_store, "x", mode) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
    CHECK(sm1::retryable() == 1);
    CHECK(sm1::abort(a) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
    sm1::closedb(a);
    sm1::closedb(b);
  }
}

TEST_CASE(
    "the index-build protocol blocks writers, then shows them the new schema") {
  // Verify sm3/SM3_DESIGN.md §3's build protocol end to end: a builder
  // cannot miss a concurrent writer's rows.
  // 1. A catalog store holds a schema row saying "no index".
  // 2. Writer W takes it shared, reads it, writes a base row,
  //    and commits.
  // 3. Builder B takes it exclusive by updating it. Writer X,
  //    beginning after B, blocks on the shared lock, so no
  //    writer can run while B scans the base store.
  // 4. B sees W's row, publishes the index, and commits.
  // 5. X's shared lock is then granted at read committed, with no
  //    error; sm1::read_latest gives it the new schema, so it
  //    maintains the index. The schema-row lock is what kept X
  //    out, not a conflict check.
  lock_fixture_t f;
  sm1::session_t *w = f.open();
  sm1::session_t *b = f.open();
  sm1::session_t *x = f.open();

  // The layer's catalog: one schema row per base store.
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t cat = mkstore(w);
  put(w, cat, "S", "noindex");
  REQUIRE(sm1::commit(w) == 0);

  // A writer reads the schema under a shared lock, then writes.
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock(w, cat, "S", sm1::lockmode_t::shared) == 0);
  CHECK(get(w, cat, "S") == "noindex");
  put(w, g_store, "w-row", "1");
  REQUIRE(sm1::commit(w) == 0);

  // The builder takes the schema row exclusive.
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::update(b, cat, "S", "indexed") == 0);

  // A writer arriving now waits for the build to finish.
  REQUIRE(sm1::begin(x, sm1::xctlevel_t::read_committed) == 0);
  std::atomic<bool> returned{false};
  int rc = -2, err = 0;
  std::thread tx([&] {
    rc = sm1::lock(x, cat, "S", sm1::lockmode_t::shared);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);

  // The builder's scan sees the committed writer's row.
  CHECK(get(b, g_store, "w-row") == "1");
  REQUIRE(sm1::commit(b) == 0);

  // The blocked writer gets the lock, and reads the new schema.
  tx.join();
  CHECK(rc == 0);
  std::string_view sd;
  REQUIRE(sm1::read_latest(x, cat, "S", sd) == 0);
  CHECK(std::string(sd) == "indexed");
  REQUIRE(sm1::commit(x) == 0);
  sm1::closedb(w);
  sm1::closedb(b);
  sm1::closedb(x);
}

TEST_CASE("sm1_lock rejects bad arguments and needs a transaction") {
  // Verify the call's contract.
  // 1. A NULL session, an unknown mode, and storeid 0 are
  //    SM_ERR_INVALID; an unknown store is SM_ERR_NOTFOUND.
  // 2. Without a transaction it fails with SM_ERR_NOTRANS.
  // 3. A key that does not exist is lockable.
  lock_fixture_t f;
  sm1::session_t *s = f.open();
  CHECK(sm1::lock(nullptr, g_store, "x", sm1::lockmode_t::shared) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock(s, g_store, "x", sm1::lockmode_t::shared) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::lock(s, g_store, "x", (sm1::lockmode_t)7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock(s, 0, "x", sm1::lockmode_t::shared) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock(s, g_store + 999, "x", sm1::lockmode_t::shared) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::lock(s, g_store, "absent", sm1::lockmode_t::exclusive) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}
