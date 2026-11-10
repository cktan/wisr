/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_lock.cc -- locks and waits, first-committer-wins, retryable
 * errors, and deadlock detection (../LOCKMGR.md §1.7, ../XCTMGR.md §5,
 * ../DBMGR.md §11).
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
#include "sm0_defer.h"
#include "sm1.h"
#include "sm_sys.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
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

  /* The environment "db" is in. */
  sm1::env_t *env() const { return m_home.env(); }

private:
  home_t m_home;
};

/* Set key in store 1 to val; the result of sm1::update. */
int set(sm1::session_t *s, const std::string &key, const std::string &val) {
  return sm1::update(s, g_store, key, val);
}

/* Whether every strong-lock count of s's database is 0. */
bool no_strong(sm1::session_t *s) {
  for (const auto &c : s->db->fp_strong) {
    if (0 != c.load()) {
      return false;
    }
  }
  return true;
}

/* The number of entries in s's database's lock table. */
size_t table_size(sm1::session_t *s) {
  wisr::lock_all_t all(s->db);
  return s->db->lock_table.size();
}

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

/* The notifications observed by a host that interrupts each wait. */
struct wait_notice_test_t {
  int starts = 0;
  int ends = 0;
  bool paired = true;
};

/* Interrupt a notified wait and record its paired end, if requested. */
bool interrupt_notified_wait(void *arg, bool start) noexcept {
  auto *state = static_cast<wait_notice_test_t *>(arg);
  if (start) {
    ++state->starts;
    sm_interrupt_self(1);
    return state->paired;
  }
  ++state->ends;
  return false;
}

} // namespace

TEST_CASE("first-committer-wins prevents lost updates at every level") {
  // Verify a write loses to a commit made after its snapshot.
  // 1. A and B begin and both read "x".
  // 2. A updates "x" and commits.
  // 3. B's update of "x" fails at once. At repeatable read and
  //    serializable that is SM_ERR_CONFLICT, retryable; at read
  //    committed it is SM_ERR_UPDATED, which is not (../LOCKMGR.md §1.9).
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
  // Verify ../XCTMGR.md §5: RC reads at a statement snapshot; a write over a
  // newer commit needs sm1::read_latest first.
  // 1. T (read committed) writes "y", so it has an xid.
  // 2. U commits a new value of "x".
  // 3. T reads "x" and sees U's value (../ROWSTORE.md §5.2).
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
  // Verify the engine-side abort on SM_ERR_CONFLICT (../DBMGR.md §11).
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

/*
 * Whether another thread finds the latch of (store, key) held: the
 * caller's own thread may not try a mutex it holds.
 */
bool latch_held(sm1::session_t *s, uint64_t store, const std::string &key) {
  bool held = false;
  std::thread([&] {
    std::mutex &m = lock_latch(s->db, store, key);
    if (m.try_lock()) {
      m.unlock();
    } else {
      held = true;
    }
  }).join();
  return held;
}

TEST_CASE("a conflicting write aborts with no key latch held") {
  // Verify the engine-side abort of a conflicting write runs without
  // the key's write latch (../LOCKMGR.md §3.5): the abort drops
  // stores, and the latch is held only to check a key and put a version.
  // 1. T begins at repeatable read; U commits a change to "x".
  // 2. T's update of "x" conflicts. As T aborts, a hook looks at the
  //    latch of "x" from another thread.
  // 3. The latch is free.
  lock_fixture_t f;
  sm1::session_t *t = f.open();
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  mkstore(t); // the abort has a store to drop
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm1::commit(u) == 0);
  bool held = true;
  t->db->test_in_fail_retryable = [&] { held = latch_held(t, g_store, "x"); };
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  CHECK_FALSE(held);
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
  // Verify the doomed flag stops a transaction that is not waiting
  // (../LOCKMGR.md §3.2).
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
  // Verify the cycle a shared lock makes possible (../LOCKMGR.md §3.2).
  // 1. A (older) and B (younger) both hold "x" shared.
  // 2. Both try to write "x"; each waits for the other.
  // 3. The younger, B, is doomed with SM_ERR_DEADLOCK, and A
  //    completes its write.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::lock_row(b, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);

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
  // Verify ../LOCKMGR.md §1.8's check runs once the lock is held, so what the
  // caller reads under it is current.
  // 1. A begins and takes its snapshot.
  // 2. B updates "x" and commits.
  // 3. A's sm1::lock_row on "x" fails with SM_ERR_CONFLICT, in either
  //    mode, and its transaction is aborted.
  for (sm1::lockmode_t mode : {sm1::SM_LOCKMODE_SH, sm1::SM_LOCKMODE_EX}) {
    CAPTURE(mode);
    lock_fixture_t f;
    sm1::session_t *a = f.open();
    sm1::session_t *b = f.open();
    REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(get(a, g_store, "x") == "0");

    REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(set(b, "x", "b") == 0);
    REQUIRE(sm1::commit(b) == 0);

    CHECK(sm1::lock_row(a, g_store, "x", mode) == -1);
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
  // Verify ../INDEXBUILD.md §2's build protocol end to end: a builder
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
  REQUIRE(sm1::lock_row(w, cat, "S", sm1::SM_LOCKMODE_SH) == 0);
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
    rc = sm1::lock_row(x, cat, "S", sm1::SM_LOCKMODE_SH);
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

TEST_CASE(
    "reads take no locks, so readers and writers never block each other") {
  // Verify sm1 stays MVCC for plain reads and writes (../LOCKMGR.md §3.2), at
  // every isolation level.
  // 1. W writes "x" and does not commit: its uncommitted version
  //    is its lock, so it holds no lock entry on "x", nor on "y"
  //    (../LOCKMGR.md §1.7).
  // 2. R reads "x" and scans the store: neither waits on W's
  //    write, both see "x" as it was, and R holds no lock
  //    afterwards.
  // 3. With R's transaction still open, W writes "y" and commits
  //    without waiting.
  lock_fixture_t f;
  sm1::session_t *w = f.open(), *r = f.open();
  int round = 0;
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    // A value no earlier round committed, so seeing it is a dirty read.
    std::string dirty = "w" + std::to_string(++round);
    REQUIRE(sm1::begin(w, level) == 0);
    REQUIRE(set(w, "x", dirty) == 0);
    sm1::lockmode_t mode;
    CHECK_FALSE(wisr::lock_held(w, g_store, "x", &mode));
    CHECK_FALSE(wisr::lock_held(w, g_store, "y", &mode));
    CHECK(w->locks.empty());

    REQUIRE(sm1::begin(r, level) == 0);
    std::atomic<bool> returned{false};
    std::string seen;
    size_t rows = 0;
    std::thread tr([&] {
      std::string_view v;
      if (sm1::read(r, g_store, "x", v) == 0) {
        seen.assign(v);
      }
      sm1::scan_t *c = nullptr;
      if (sm1::scan(r, g_store, nullptr, 0, 1, nullptr, 0, 0, c) == 0) {
        std::string_view k;
        while (sm1::scannxt(c, k, v) == 0) {
          rows++;
        }
        sm1::scanend(c);
      }
      returned = true;
    });
    settle();
    CHECK(returned.load());
    tr.join();
    CHECK_FALSE(seen.empty());
    CHECK(seen != dirty);
    CHECK(rows == 2);
    CHECK_FALSE(wisr::lock_held(r, g_store, "x", &mode));
    CHECK_FALSE(wisr::lock_held(r, g_store, "y", &mode));

    returned = false;
    int rc = -2;
    std::thread tw([&] {
      rc = set(w, "y", "w");
      if (rc == 0) {
        rc = sm1::commit(w);
      }
      returned = true;
    });
    settle();
    CHECK(returned.load());
    tw.join();
    CHECK(rc == 0);
    REQUIRE(sm1::commit(r) == 0);
  }
  sm1::closedb(w);
  sm1::closedb(r);
}

TEST_CASE("Gray's lock modes grant and block as their matrix says") {
  // Verify the conflict table of sm1.h's lockmode_t, pair by pair.
  // 1. For each mode a and mode b, A holds "x" in a, then B asks
  //    for "x" in b on a thread of its own.
  // 2. A compatible b is granted within 150 ms; a conflicting one
  //    is still waiting then.
  // 3. A commits; B's request is granted, and B commits.
  const sm1::lockmode_t modes[] = {sm1::SM_LOCKMODE_IS, sm1::SM_LOCKMODE_IX,
                                   sm1::SM_LOCKMODE_SH, sm1::SM_LOCKMODE_SIX,
                                   sm1::SM_LOCKMODE_EX};
  // Gray's table, written out apart from the lock manager's.
  const bool ok[5][5] = {
      {true, true, true, true, false},     {true, true, false, false, false},
      {true, false, true, false, false},   {true, false, false, false, false},
      {false, false, false, false, false},
  };
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open();
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      CAPTURE(i);
      CAPTURE(j);
      REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
      REQUIRE(sm1::lock_row(a, g_store, "x", modes[i]) == 0);
      REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
      std::atomic<bool> returned{false};
      int rc = -2;
      std::thread tb([&] {
        rc = sm1::lock_row(b, g_store, "x", modes[j]);
        returned = true;
      });
      settle();
      CHECK(returned.load() == ok[i][j]);
      REQUIRE(sm1::commit(a) == 0);
      tb.join();
      CHECK(rc == 0);
      REQUIRE(sm1::commit(b) == 0);
    }
  }
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a holder asking for more converts to the mode covering both") {
  // Verify lock conversion.
  // 1. A holds "x" IX, then asks S: it now holds SIX, so B's IS is
  //    granted and C's IX waits until A commits.
  // 2. A holds "y" IS, then writes "y", which converts it to X: B's
  //    IS on "y" waits until A commits.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open(), *c = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_IX) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::lock_row(b, g_store, "x", sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::read_committed) == 0);
  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tc([&] {
    rc = sm1::lock_row(c, g_store, "x", sm1::SM_LOCKMODE_IX);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned.load());
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(b) == 0);
  tc.join();
  CHECK(rc == 0);
  REQUIRE(sm1::commit(c) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(set(a, "y", "1") == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  returned = false;
  std::thread tb([&] {
    rc = sm1::lock_row(b, g_store, "y", sm1::SM_LOCKMODE_IS);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned.load());
  REQUIRE(sm1::commit(a) == 0);
  tb.join();
  CHECK(rc == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
  sm1::closedb(c);
}

TEST_CASE("sm1_lock rejects bad arguments and needs a transaction") {
  // Verify the call's contract.
  // 1. A NULL session, an unknown mode, and storeid 0 are
  //    SM_ERR_INVALID; an unknown store is SM_ERR_NOTFOUND.
  // 2. Without a transaction it fails with SM_ERR_NOTRANS.
  // 3. A key that does not exist is lockable.
  lock_fixture_t f;
  sm1::session_t *s = f.open();
  CHECK(sm1::lock_row(nullptr, g_store, "x", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock_row(s, g_store, "x", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::lock_row(s, g_store, "x", (sm1::lockmode_t)7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock_row(s, 0, "x", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::lock_row(s, g_store + 999, "x", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::lock_row(s, g_store, "absent", sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("a wait for another transaction can be interrupted") {
  // Verify an interrupt to the waiting thread ends its wait, and only that.
  // 1. A updates "x" and does not commit. B updates "x" in a thread:
  //    it waits. The main thread interrupts that thread: B's update
  //    fails with SM_ERR_INTERRUPTED, which is not retryable.
  // 2. B's transaction is still active: it writes "y", which waits
  //    for no one.
  // 3. The bit stays pending until taken: B's next wait, on a lock A
  //    holds, fails at once, with no second interrupt.
  // 4. In a thread whose wait mask leaves bit 2 out, B updates "x"
  //    again; bit 2 raised in it does not end the wait, which lasts
  //    until A commits.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  REQUIRE(sm1::lock_row(a, g_store, "z", sm1::SM_LOCKMODE_EX) == 0);

  std::atomic<bool> returned{false};
  sm_thread_ref_t waiter;
  std::mutex waiter_mu;
  int rc = 0, err = 0, again = 0, rc_y = 0;
  int rc_z[2] = {0, 0}, err_z[2] = {0, 0};
  std::thread tb([&] {
    {
      std::lock_guard<std::mutex> guard(waiter_mu);
      waiter = sm_thread_self();
    }
    rc = set(b, "x", "b");
    err = sm1::errnum();
    again = sm1::retryable();
    returned = true;
    rc_y = set(b, "y", "b");
    for (int i = 0; i < 2; i++) {
      rc_z[i] = sm1::lock_row(b, g_store, "z", sm1::SM_LOCKMODE_SH);
      err_z[i] = sm1::errnum();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  {
    std::lock_guard<std::mutex> guard(waiter_mu);
    sm_interrupt(waiter, 1);
  }
  tb.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_INTERRUPTED);
  CHECK(again == 0);
  CHECK(rc_y == 0);
  for (int i = 0; i < 2; i++) {
    CHECK(rc_z[i] == -1);
    CHECK(err_z[i] == sm1::SM_ERR_INTERRUPTED);
  }

  REQUIRE(sm1::stmt_begin(b) == 0);
  returned = false;
  std::thread tb2([&] {
    sm_set_wait_mask(~2u);
    {
      std::lock_guard<std::mutex> guard(waiter_mu);
      waiter = sm_thread_self();
    }
    rc = set(b, "x", "b");
    err = sm1::errnum();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  {
    std::lock_guard<std::mutex> guard(waiter_mu);
    sm_interrupt(waiter, 2);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1::commit(a) == 0);
  tb2.join();
  // At READ COMMITTED the row changed under it: read it, write again.
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_UPDATED);
  std::string_view v;
  REQUIRE(sm1::read_latest(b, g_store, "x", v) == 0);
  REQUIRE(set(b, "x", "b") == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE(
    "a qa_nowait session is told a write would block, and may repeat it") {
  // Verify a write that would wait fails with SM_ERR_BLOCKED instead.
  // 1. A updates "x"; its version is uncommitted.
  // 2. B, set qa_nowait, updates "x": SM_ERR_BLOCKED, not retryable,
  //    and B's transaction is still active, for it writes "y".
  // 3. B repeats the update and is told the same.
  // 4. A aborts; B repeats the update once more and it goes through.
  // 5. B commits, and "x" holds B's value.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::set_qa_nowait(b, true) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(std::string(sm1::errname()) == "ERR_BLOCKED");
  CHECK(sm1::retryable() == 0);
  REQUIRE(set(b, "y", "b") == 0);
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  REQUIRE(sm1::abort(a) == 0);
  REQUIRE(set(b, "x", "b") == 0);
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(a, g_store, "x") == "b");
  CHECK(get(a, g_store, "y") == "b");
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE(
    "a qa_nowait session is told sm1_lock would block, and holds nothing") {
  // Verify sm1::lock_row under qa_nowait, and that a refusal takes no lock.
  // 1. A locks "x" shared.
  // 2. B, set qa_nowait, asks for it exclusive: SM_ERR_BLOCKED. Asking
  //    for it shared is compatible and goes through.
  // 3. C, set qa_nowait, asks for it exclusive: SM_ERR_BLOCKED.
  // 4. A and B commit. C's request now goes through at once, so its
  //    refused one left nothing behind and nobody waits on anybody.
  // 5. With qa_nowait off again, a compatible request still succeeds.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  sm1::session_t *c = f.open();
  REQUIRE(sm1::set_qa_nowait(b, true) == 0);
  REQUIRE(sm1::set_qa_nowait(c, true) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm1::lock_row(b, g_store, "x", sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(sm1::lock_row(b, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm1::lock_row(c, g_store, "x", sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(b) == 0);
  CHECK(sm1::lock_row(c, g_store, "x", sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::commit(c) == 0);
  REQUIRE(sm1::set_qa_nowait(c, false) == 0);
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::lock_row(c, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::commit(c) == 0);
  CHECK(sm1::set_qa_nowait(nullptr, true) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm1::closedb(a);
  sm1::closedb(b);
  sm1::closedb(c);
}

TEST_CASE("a transaction's writes leave no lock entries") {
  // Verify ../LOCKMGR.md: memory for locks does not grow with writes.
  // 1. W inserts a thousand keys and updates and deletes the two
  //    seeded rows, without committing.
  // 2. W's lock list is empty, and the shared lock table holds
  //    nothing for any of those keys.
  // 3. Another session's write of one of them still waits for W,
  //    the uncommitted version standing for the lock: under
  //    qa_nowait it fails with SM_ERR_BLOCKED.
  // 4. W commits; the other session's write then goes through.
  lock_fixture_t f;
  sm1::session_t *w = f.open(), *o = f.open();
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  for (int i = 0; i < 1000; i++) {
    put(w, g_store, "k" + std::to_string(i), "v");
  }
  REQUIRE(set(w, "x", "1") == 0);
  REQUIRE(sm1::erase(w, g_store, "y") == 0);
  CHECK(w->locks.empty());
  {
    wisr::lock_all_t all(w->db);
    CHECK(w->db->lock_table.empty());
  }

  REQUIRE(sm1::set_qa_nowait(o, true) == 0);
  REQUIRE(sm1::begin(o, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::insert(o, g_store, "k7", "o") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(set(o, "x", "2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  REQUIRE(sm1::commit(w) == 0);
  CHECK(sm1::insert(o, g_store, "k7", "o") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm1::commit(o) == 0);
  sm1::closedb(w);
  sm1::closedb(o);
}

TEST_CASE("a write refused with SM_ERR_UPDATED keeps the row locked until "
          "the re-read") {
  // Verify the one write that leaves a lock behind (../LOCKMGR.md §1.9).
  // 1. T begins at READ COMMITTED; U commits a new "x".
  // 2. T's update of "x" fails with SM_ERR_UPDATED, and T now
  //    holds "x" exclusive.
  // 3. A third session's write of "x" is blocked by that lock.
  // 4. T reads the latest "x", writes it, and commits; the third
  //    session's write then goes through.
  lock_fixture_t f;
  sm1::session_t *t = f.open(), *u = f.open(), *o = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(u, "x", "u") == 0);
  REQUIRE(sm1::commit(u) == 0);

  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  sm1::lockmode_t mode;
  REQUIRE(wisr::lock_held(t, g_store, "x", &mode));
  CHECK(mode == sm1::SM_LOCKMODE_EX);

  REQUIRE(sm1::set_qa_nowait(o, true) == 0);
  REQUIRE(sm1::begin(o, sm1::xctlevel_t::read_committed) == 0);
  CHECK(set(o, "x", "o") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  std::string_view v;
  REQUIRE(sm1::read_latest(t, g_store, "x", v) == 0);
  CHECK(v == "u");
  REQUIRE(set(t, "x", "t") == 0);
  REQUIRE(sm1::commit(t) == 0);
  CHECK(set(o, "x", "o") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::abort(o) == 0);
  sm1::closedb(t);
  sm1::closedb(u);
  sm1::closedb(o);
}

TEST_CASE("trylock takes an oid's lock only if it is free now") {
  // Verify sm1::trylock_oid never waits, and takes nothing it cannot.
  // 1. Session a takes an oid's lock shared.
  // 2. Session b's trylock of it exclusive returns 1 at once, and
  //    b holds nothing: a's exclusive try still fails only on b's
  //    later shared hold, not before.
  // 3. b's trylock of it shared returns 0: shared holders agree.
  // 4. a commits; b's trylock exclusive now converts its hold,
  //    returning 0.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *a = opendb(home.env(), "db", nullptr);
  sm1::session_t *b = opendb(home.env(), "db", nullptr);
  REQUIRE(a);
  REQUIRE(b);
  uint64_t oid = 0x0707070707070707;
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_oid(a, oid, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm1::trylock_oid(b, oid, sm1::SM_LOCKMODE_EX) == 1);
  CHECK(sm1::trylock_oid(a, oid, sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_oid(a, oid, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm1::trylock_oid(b, oid, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm1::trylock_oid(b, oid, sm1::SM_LOCKMODE_EX) == 1);
  REQUIRE(sm1::commit(a) == 0);
  CHECK(sm1::trylock_oid(b, oid, sm1::SM_LOCKMODE_EX) == 0);

  REQUIRE(sm1::commit(b) == 0);
}

TEST_CASE("trylock takes a row's lock unless it would wait") {
  // Verify sm1::trylock_row: the lock sm1::lock_row takes, and
  // never a wait.
  // 1. A row is committed. Session a locks it exclusive; b's
  //    trylock, shared or exclusive, returns 1, and b's
  //    transaction goes on.
  // 2. a commits: b's trylock exclusive returns 0, and a third
  //    try by a returns 1.
  // 3. b commits. a updates the row and does not commit: the
  //    uncommitted version is its lock, and b's trylock returns 1.
  // 4. a commits. At REPEATABLE READ, a session that began before
  //    that commit fails its trylock with SM_ERR_CONFLICT, as
  //    sm1::lock_row would.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *a = opendb(home.env(), "db", nullptr);
  sm1::session_t *b = opendb(home.env(), "db", nullptr);
  sm1::session_t *c = opendb(home.env(), "db", nullptr);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(c);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(a);
  put(a, store, "k", "v1");
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, store, "k", sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm1::trylock_row(b, store, "k", sm1::SM_LOCKMODE_SH) == 1);
  CHECK(sm1::trylock_row(b, store, "k", sm1::SM_LOCKMODE_EX) == 1);
  CHECK(get(b, store, "k") == "v1");
  REQUIRE(sm1::commit(a) == 0);
  CHECK(sm1::trylock_row(b, store, "k", sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::trylock_row(a, store, "k", sm1::SM_LOCKMODE_EX) == 1);
  REQUIRE(sm1::commit(b) == 0);

  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(c, store, "k") == "v1");
  REQUIRE(sm1::update(a, store, "k", "v2") == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::trylock_row(b, store, "k", sm1::SM_LOCKMODE_SH) == 1);
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::commit(a) == 0);
  CHECK(sm1::trylock_row(c, store, "k", sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
}

TEST_CASE("waits_for names the sessions a waiting session waits for") {
  // Verify the edges a host reads for pg_blocking_pids() (../LOCKMGR.md §3.2).
  // 1. Number A, B and C; A writes "x" and locks "y" exclusive.
  // 2. B writes "x" and waits behind A's version; C locks "y" and
  //    waits on A's lock.
  // 3. Confirm waits_for names A for B and for C, A waits for none,
  //    and an unknown number or 0 names none.
  // 4. A aborts: B and C go through, and wait for none.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  sm1::session_t *c = f.open();
  REQUIRE(sm1::set_tag(a, 101) == 0);
  REQUIRE(sm1::set_tag(b, 102) == 0);
  REQUIRE(sm1::set_tag(c, 103) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  REQUIRE(sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_EX) == 0);

  int rc_b = -2, rc_c = -2;
  std::thread tb([&] { rc_b = set(b, "x", "b"); });
  std::thread tc(
      [&] { rc_c = sm1::lock_row(c, g_store, "y", sm1::SM_LOCKMODE_SH); });
  settle();
  std::vector<uint64_t> out{7};
  REQUIRE(sm1::waits_for(f.env(), 102, &out) == 0);
  CHECK(out == std::vector<uint64_t>{101});
  REQUIRE(sm1::waits_for(f.env(), 103, &out) == 0);
  CHECK(out == std::vector<uint64_t>{101});
  REQUIRE(sm1::waits_for(f.env(), 101, &out) == 0);
  CHECK(out.empty());
  REQUIRE(sm1::waits_for(f.env(), 999, &out) == 0);
  CHECK(out.empty());
  REQUIRE(sm1::waits_for(f.env(), 0, &out) == 0);
  CHECK(out.empty());

  REQUIRE(sm1::abort(a) == 0);
  tb.join();
  tc.join();
  CHECK(rc_b == 0);
  CHECK(rc_c == 0);
  REQUIRE(sm1::waits_for(f.env(), 102, &out) == 0);
  CHECK(out.empty());
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::commit(c) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
  sm1::closedb(c);
}

TEST_CASE("lock_status lists each lock held, and each waited for in lock") {
  // Verify the rows a host reads for pg_locks (../LOCKMGR.md §3.2).
  // 1. Number A to D; A writes "x", locks oid 7 IS and "y" exclusive,
  //    of kind 5; B locks oid 7 IX.
  // 2. C waits for "y" shared, of kind 6, on A's lock; D writes "x" and
  //    waits behind A's version.
  // 3. Confirm lock_status lists A's and B's holds, granted, and C's
  //    wait, not granted, each with its database, id, kind, mode and
  //    number, a row's id its key's hash, and lists nothing for D, who
  //    waits for no lock.
  // 4. A aborts: C and D go through, and lock_status lists B's hold and
  //    C's, of C's kind, and no wait.
  lock_fixture_t f;
  sm1::session_t *s[4];
  for (int i = 0; i < 4; i++) {
    s[i] = f.open();
    REQUIRE(sm1::set_tag(s[i], 101 + i) == 0);
    REQUIRE(sm1::begin(s[i], sm1::xctlevel_t::read_committed) == 0);
  }
  uint64_t oid = 7;
  REQUIRE(set(s[0], "x", "a") == 0);
  REQUIRE(sm1::lock_oid(s[0], oid, sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::lock_row(s[0], g_store, "y", sm1::SM_LOCKMODE_EX, false, 5) ==
          0);
  REQUIRE(sm1::lock_oid(s[1], oid, sm1::SM_LOCKMODE_IX) == 0);

  int rc_c = -2, rc_d = -2;
  std::thread tc([&] {
    rc_c = sm1::lock_row(s[2], g_store, "y", sm1::SM_LOCKMODE_SH, false, 6);
  });
  std::thread td([&] { rc_d = set(s[3], "x", "d"); });
  settle();

  // Each row as a comparable string, sorted, so the order is the table's.
  auto rows = [&] {
    std::vector<sm1::lock_status_t> out(1); /* cleared first */
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    std::vector<std::string> got;
    for (const auto &r : out) {
      // A row's word is its key's hash: "y" for "y"'s.
      std::string word =
          r.type == sm1::LOCKTYPE_ROW && r.word == wisr::lock_row_hash("y")
              ? "y"
              : std::to_string(r.word);
      got.push_back(r.dbname + " " + std::to_string(r.type) + " " +
                    std::to_string(r.storeid == g_store ? 1 : r.storeid) + " " +
                    word + " " + std::to_string(r.kind) + " " +
                    std::to_string(r.mode) + " " + std::to_string(r.tag) +
                    (r.granted ? " held" : " waits"));
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  CHECK(rows() == std::vector<std::string>{
                      "db 0 1 y 5 1 101 held",  /* A: X on "y" */
                      "db 0 1 y 6 0 103 waits", /* C: S on "y" */
                      "db 1 0 7 0 2 101 held",  /* A: IS on oid 7 */
                      "db 1 0 7 0 3 102 held",  /* B: IX on oid 7 */
                  });

  REQUIRE(sm1::abort(s[0]) == 0);
  tc.join();
  td.join();
  CHECK(rc_c == 0);
  CHECK(rc_d == 0);
  CHECK(rows() == std::vector<std::string>{
                      "db 0 1 y 6 0 103 held", /* C: S on "y" */
                      "db 1 0 7 0 3 102 held", /* B: IX on oid 7 */
                  });
  for (int i = 1; i < 4; i++) {
    REQUIRE(sm1::commit(s[i]) == 0);
  }
  for (int i = 0; i < 4; i++) {
    sm1::closedb(s[i]);
  }
}

TEST_CASE("lock_status lists the locks of many sessions as the set changes") {
  // Verify lock_status reads every session's slots while the set of
  // sessions grows past several rehashes and shrinks, taking their
  // fp_mu in address order. Two calls that took two in different
  // orders are a lock-order cycle, which the thread sanitizer reports
  // (../LOCKMGR.md §3.2); this build cannot, so the rows are checked.
  // 1. Open 70 sessions; each begins and locks its own oid IS.
  // 2. After each open, lock_status lists one row for each so far.
  // 3. Close sessions from the middle; after each, it lists the rest.
  lock_fixture_t f;
  std::vector<sm1::session_t *> ss;
  auto rows = [&] {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    return out.size();
  };
  for (int i = 0; i < 70; i++) {
    sm1::session_t *s = f.open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm1::lock_oid(s, 1000 + i, sm1::SM_LOCKMODE_IS) == 0);
    ss.push_back(s);
    CHECK(rows() == ss.size());
  }
  while (ss.size() > 3) {
    size_t k = ss.size() / 2;
    sm1::closedb(ss[k]);
    ss.erase(ss.begin() + static_cast<long>(k));
    CHECK(rows() == ss.size());
  }
  for (sm1::session_t *s : ss) {
    sm1::closedb(s);
  }
}

TEST_CASE("a write waiting on a row's lock is listed with the lock's kind") {
  // Verify a write, which gives no kind, waits listed with the kind of
  // the lock it waits on, so a host still sees a row's lock (../LOCKMGR.md
  // §3.2).
  // 1. A locks "y" exclusive, of kind 5; B writes "y" and waits.
  // 2. lock_status lists A's hold and B's wait, both of kind 5.
  // 3. A commits: B's write goes through, and nothing is listed.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open();
  for (sm1::session_t *s : {a, b}) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  }
  REQUIRE(sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_EX, false, 5) == 0);
  int rc_b = -2;
  std::thread tb([&] { rc_b = set(b, "y", "b"); });
  settle();

  // Each lock as "kind mode", and whether held.
  auto rows = [&] {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    std::vector<std::string> got;
    for (const auto &r : out) {
      got.push_back(std::to_string(r.kind) + " " + std::to_string(r.mode) +
                    (r.granted ? " held" : " waits"));
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  std::string ex = std::to_string(sm1::SM_LOCKMODE_EX);
  CHECK(rows() ==
        std::vector<std::string>{"5 " + ex + " held", "5 " + ex + " waits"});

  REQUIRE(sm1::commit(a) == 0);
  tb.join();
  CHECK(rc_b == 0);
  CHECK(rows().empty());
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a lock list across blocks keeps each lock exact past a savepoint") {
  // Verify the lock list's blocks (lock_list_t): positions and a cut
  // hold across block ends.
  // 1. A locks 512 keys shared, exactly two blocks of the list, and
  //    converts k0, the list's first entry, and k300 to exclusive,
  //    which adds no entry.
  // 2. A sets a savepoint, at the second block's end, then locks
  //    300 keys more and two of max_keylen.
  // 3. lock_held finds k0 and k300 exclusive, the last, and the
  //    long keys; lock_status lists all 814.
  // 4. A rolls back to the savepoint: only the 512 are held, k300
  //    still exclusive.
  // 5. A locks one key more, past the savepoint again, then commits:
  //    the table and the list are empty.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  auto key = [](int i) { return "k" + std::to_string(i); };
  auto held = [&](const std::string &k) {
    sm1::lockmode_t mode;
    return wisr::lock_held(a, g_store, k, &mode) ? int(mode) : -1;
  };
  for (int i = 0; i < 512; i++) {
    REQUIRE(sm1::lock_row(a, g_store, key(i), sm1::SM_LOCKMODE_SH) == 0);
  }
  REQUIRE(sm1::lock_row(a, g_store, key(0), sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::lock_row(a, g_store, key(300), sm1::SM_LOCKMODE_EX) == 0);
  CHECK(a->locks.size() == 512);

  REQUIRE(sm1::savepoint(a) == 1);
  for (int i = 512; i < 812; i++) {
    REQUIRE(sm1::lock_row(a, g_store, key(i), sm1::SM_LOCKMODE_SH) == 0);
  }
  std::string big(sm1::max_keylen, 'b');
  std::string big2(sm1::max_keylen, 'c');
  REQUIRE(sm1::lock_row(a, g_store, big, sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::lock_row(a, g_store, big2, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(held(key(0)) == sm1::SM_LOCKMODE_EX);
  CHECK(held(key(811)) == sm1::SM_LOCKMODE_SH);
  CHECK(held(key(300)) == sm1::SM_LOCKMODE_EX);
  CHECK(held(big) == sm1::SM_LOCKMODE_SH);
  CHECK(held(big2) == sm1::SM_LOCKMODE_SH);
  {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    CHECK(out.size() == 814);
  }

  REQUIRE(sm1::rollback(a, 1) == 0);
  CHECK(a->locks.size() == 512);
  CHECK(held(key(511)) == sm1::SM_LOCKMODE_SH);
  CHECK(held(key(512)) == -1);
  CHECK(held(big) == -1);
  CHECK(held(big2) == -1);
  CHECK(held(key(300)) == sm1::SM_LOCKMODE_EX);

  REQUIRE(sm1::lock_row(a, g_store, key(900), sm1::SM_LOCKMODE_SH) == 0);
  CHECK(held(key(900)) == sm1::SM_LOCKMODE_SH);
  REQUIRE(sm1::commit(a) == 0);
  {
    wisr::lock_all_t all(a->db);
    CHECK(a->db->lock_table.empty());
  }
  CHECK(a->locks.empty());
  sm1::closedb(a);
}

TEST_CASE("row keys that hash alike are one lock") {
  // Verify two row keys whose ids are one (lockid_t) are one lock:
  // one table entry, one list entry, one mode (../LOCKMGR.md §3.2).
  // 1. Make every row key hash alike; A locks "x" shared, of kind 1,
  //    then "y" exclusive, of kind 2, which converts the lock "x" took
  //    and keeps its kind.
  // 2. lock_held reports "x", "y" and "z", which A never locked, all
  //    exclusive; lock_status lists the one lock, its word the hash
  //    every key has, 0, and its kind 1.
  // 3. B's lock of "z" meets it: under qa_nowait it fails with
  //    SM_ERR_BLOCKED.
  // 4. A sets a savepoint, locks "z" shared, which adds nothing, and
  //    rolls back to it: the lock is still held exclusive, its one
  //    list entry kept, and B's lock of "z" is still refused.
  // 5. A commits: the table and A's list are empty, and B locks "z".
  lock_fixture_t f;
  wisr::lock_test_rows_collide = true;
  defer { wisr::lock_test_rows_collide = false; };
  sm1::session_t *a = f.open(), *b = f.open();
  REQUIRE(sm1::set_tag(a, 101) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH, false, 1) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_EX, false, 2) == 0);
  {
    wisr::lock_all_t all(a->db);
    CHECK(a->db->lock_table.size() == 1);
  }
  CHECK(a->locks.size() == 1);

  // Each lock A holds, as "word kind mode", and whether "x", "y", "z"
  // are held.
  auto rows = [&] {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    std::vector<std::string> got;
    for (const auto &r : out) {
      CHECK(r.storeid == g_store);
      got.push_back(std::to_string(r.word) + " " + std::to_string(r.kind) +
                    " " + std::to_string(r.mode) +
                    (r.granted ? " held" : " waits"));
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  auto held = [&](const char *key) {
    sm1::lockmode_t mode;
    return wisr::lock_held(a, g_store, key, &mode) ? int(mode) : -1;
  };
  CHECK(held("x") == sm1::SM_LOCKMODE_EX);
  CHECK(held("y") == sm1::SM_LOCKMODE_EX);
  CHECK(held("z") == sm1::SM_LOCKMODE_EX);
  CHECK(rows() == std::vector<std::string>{
                      "0 1 " + std::to_string(sm1::SM_LOCKMODE_EX) + " held",
                  });

  REQUIRE(sm1::set_qa_nowait(b, true) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::lock_row(b, g_store, "z", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  REQUIRE(sm1::savepoint(a) == 1);
  REQUIRE(sm1::lock_row(a, g_store, "z", sm1::SM_LOCKMODE_SH) == 0);
  CHECK(a->locks.size() == 1);
  REQUIRE(sm1::rollback(a, 1) == 0);
  CHECK(held("z") == sm1::SM_LOCKMODE_EX);
  CHECK(held("x") == sm1::SM_LOCKMODE_EX);
  CHECK(a->locks.size() == 1);
  CHECK(sm1::lock_row(b, g_store, "z", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  REQUIRE(sm1::commit(a) == 0);
  {
    wisr::lock_all_t all(a->db);
    CHECK(a->db->lock_table.empty());
  }
  CHECK(a->locks.empty());
  CHECK(sm1::lock_row(b, g_store, "z", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a row key's lock hash is the same in every build") {
  // Verify lock_row_hash gives fixed values, as a prepared record
  // keeps them: a build that changes the hash fails here.
  // 1. The empty key, a key shorter than a word, and one of two words
  //    and a tail hash to the values written down.
  // 2. Two keys that differ only in a trailing zero byte differ.
  CHECK(wisr::lock_row_hash("") == 0x0ULL);
  CHECK(wisr::lock_row_hash("x") == 0x34558360b0535c76ULL);
  CHECK(wisr::lock_row_hash("a key of 20 bytes ok") == 0x8641b660711e5168ULL);
  CHECK(wisr::lock_row_hash(rocksdb::Slice("x\0", 2)) !=
        wisr::lock_row_hash("x"));
}

TEST_CASE("a weak table lock takes a slot, and a strong one moves it") {
  // Verify the fast path (../LOCKMGR.md §3.2): IS and IX on an oid are in the
  // session's slots, and a strong lock meets them in the table.
  // 1. A locks oid 1 IS and oid 2 IX: two slots, an empty table.
  // 2. B's trylock of oid 1 X fails: A's IS moved to the table,
  //    and the count B took is given back.
  // 3. C locks oid 1 IS: no count, so a slot, and the table holds
  //    A's alone.
  // 4. A and C commit: no slot, no entry, no count is left.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open(), *c = f.open();
  for (sm1::session_t *s : {a, b, c}) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  }
  REQUIRE(sm1::lock_oid(a, 1, sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::lock_oid(a, 2, sm1::SM_LOCKMODE_IX) == 0);
  CHECK(a->fp_used == 2);
  CHECK(table_size(a) == 0);

  CHECK(sm1::trylock_oid(b, 1, sm1::SM_LOCKMODE_EX) == 1);
  CHECK(a->fp_used == 1);
  CHECK(table_size(a) == 1);
  CHECK(no_strong(a));

  REQUIRE(sm1::lock_oid(c, 1, sm1::SM_LOCKMODE_IS) == 0);
  CHECK(c->fp_used == 1);
  CHECK(table_size(a) == 1);

  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(c) == 0);
  CHECK(a->fp_used == 0);
  CHECK(c->fp_used == 0);
  CHECK(table_size(a) == 0);
  CHECK(no_strong(a));
  REQUIRE(sm1::commit(b) == 0);
  for (sm1::session_t *s : {a, b, c}) {
    sm1::closedb(s);
  }
}

TEST_CASE("a strong table lock leaves the fast path of a table 1,024 oids "
          "away") {
  // Verify a strong lock sends to the table only the weak locks of
  // keys that hash with it, not of every oid alike modulo 1,024.
  // 1. A locks oid 5 X: a count is raised, A's lock in the table.
  // 2. B locks oid 1029 IS: a slot, and the table holds A's alone.
  // 3. Both commit: no slot, no entry, no count is left.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open();
  for (sm1::session_t *s : {a, b}) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  }
  REQUIRE(sm1::lock_oid(a, 5, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(table_size(a) == 1);
  CHECK(!no_strong(a));

  REQUIRE(sm1::lock_oid(b, 1029, sm1::SM_LOCKMODE_IS) == 0);
  CHECK(b->fp_used == 1);
  CHECK(table_size(a) == 1);

  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(b) == 0);
  CHECK(b->fp_used == 0);
  CHECK(table_size(a) == 0);
  CHECK(no_strong(a));
  for (sm1::session_t *s : {a, b}) {
    sm1::closedb(s);
  }
}

TEST_CASE("a slot's entry is its own, wherever it is in the list") {
  // Verify a fast-path slot keeps its own list entry's position:
  // a conversion in the slot, a listing and a sweep reach it, not
  // the entry before it.
  // 1. A locks row "x" shared, then oid 7 IS, in a slot, then
  //    oid 7 IX, which the slot converts.
  // 2. lock_status lists "x" shared and oid 7 IX.
  // 3. B's trylock of oid 7 S fails: A's slot is moved to the table,
  //    so no slot is used, and the table holds "x" and oid 7.
  // 4. lock_status lists the same two.
  // 5. A commits: no entry, slot or count is left.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open();
  for (sm1::session_t *s : {a, b}) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  }
  // What the database lists, as "name mode", the oid as "oid", the row
  // "x" by its key's hash.
  auto rows = [&] {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.env(), out) == 0);
    std::vector<std::string> got;
    for (const auto &r : out) {
      std::string name = sm1::LOCKTYPE_OID == r.type ? "oid"
                         : r.word == wisr::lock_row_hash("x")
                             ? "x"
                             : std::to_string(r.word);
      got.push_back(name + " " + std::to_string(r.mode));
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  const std::vector<std::string> want = {
      "oid " + std::to_string(sm1::SM_LOCKMODE_IX),
      "x " + std::to_string(sm1::SM_LOCKMODE_SH),
  };
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::lock_oid(a, 7, sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::lock_oid(a, 7, sm1::SM_LOCKMODE_IX) == 0);
  CHECK(a->fp_used == 1);
  CHECK(rows() == want);

  CHECK(sm1::trylock_oid(b, 7, sm1::SM_LOCKMODE_SH) == 1);
  CHECK(a->fp_used == 0);
  CHECK(table_size(a) == 2);
  CHECK(rows() == want);

  REQUIRE(sm1::commit(a) == 0);
  CHECK(a->fp_used == 0);
  CHECK(table_size(a) == 0);
  CHECK(no_strong(a));
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("table locks past the slots go to the table, and all are held") {
  // Verify a transaction with more table locks than slots (../LOCKMGR.md §3.2).
  // 1. A locks 40 oids IS: 32 in slots, 8 in the table.
  // 2. lock_status lists all 40, held.
  // 3. B's trylock X fails on the first oid, in a slot, and on the
  //    last, in the table.
  // 4. A commits: B's X on both is granted, and once B commits no
  //    slot, entry or count is left.
  lock_fixture_t f;
  sm1::session_t *a = f.open(), *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  for (uint64_t n = 1; n <= 40; n++) {
    REQUIRE(sm1::lock_oid(a, n, sm1::SM_LOCKMODE_IS) == 0);
  }
  CHECK(a->fp_used == sm1::session_t::fp_slots);
  CHECK(table_size(a) == 40 - sm1::session_t::fp_slots);
  std::vector<sm1::lock_status_t> out;
  REQUIRE(sm1::lock_status(f.env(), out) == 0);
  CHECK(out.size() == 40);
  for (const auto &r : out) {
    CHECK(r.granted);
    CHECK(r.mode == sm1::SM_LOCKMODE_IS);
  }

  CHECK(sm1::trylock_oid(b, 1, sm1::SM_LOCKMODE_EX) == 1);
  CHECK(sm1::trylock_oid(b, 40, sm1::SM_LOCKMODE_EX) == 1);
  REQUIRE(sm1::commit(a) == 0);
  CHECK(sm1::trylock_oid(b, 1, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm1::trylock_oid(b, 40, sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::commit(b) == 0);
  CHECK(a->fp_used == 0);
  CHECK(table_size(a) == 0);
  CHECK(no_strong(a));
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE(
    "a release wakes a lock's waiter at once, not at its deadlock check") {
  // Verify that giving back a lock in the table wakes its waiters: a
  // waiter whose next look is deadlock_ms away is granted at once.
  // 1. B waits 10 s between deadlock checks. A locks "x" and, past a
  //    savepoint, "y", both exclusive.
  // 2. B asks "y" shared and waits; A rolls back to the savepoint,
  //    and B is granted "y" well inside 10 s.
  // 3. B asks "x" shared and waits; A commits, and B is granted "x"
  //    well inside 10 s.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::set_deadlock_ms(b, 10000) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::savepoint(a) == 1);
  REQUIRE(sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_EX) == 0);
  // B's lock of key, waiting, granted after A's release: its rc, and
  // the milliseconds from the release to the grant.
  auto granted_after = [&](const char *key, auto release) {
    int rc = -2;
    std::thread tb(
        [&] { rc = sm1::lock_row(b, g_store, key, sm1::SM_LOCKMODE_SH); });
    settle();
    auto start = std::chrono::steady_clock::now();
    release();
    tb.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    CHECK(rc == 0);
    return ms;
  };
  CHECK(granted_after("y", [&] { REQUIRE(sm1::rollback(a, 1) == 0); }) < 2000);
  CHECK(granted_after("x", [&] { REQUIRE(sm1::commit(a) == 0); }) < 2000);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a cycle is found once a waiter has slept its deadlock_ms") {
  // Verify the search waits deadlock_ms, PostgreSQL's
  // deadlock_timeout, as PostgreSQL's does: a cycle of waits on two
  // locks is not found as a wait begins, but by the first waiter
  // whose time is up, and the victim is still the youngest.
  // 1. A negative deadlock_ms is refused.
  // 2. A, the older, waits 300 ms; B 10,000 ms. A holds "x" and B
  //    "y" shared. B writes "x" and waits for A.
  // 3. A writes "y", closing the cycle, and waits: its search, 300
  //    ms on, finds it. B, the younger, fails with deadlock, and A's
  //    write goes ahead, no sooner than 250 ms and well inside 10 s.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  CHECK(sm1::set_deadlock_ms(a, -1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::set_deadlock_ms(a, 300) == 0);
  REQUIRE(sm1::set_deadlock_ms(b, 10000) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::lock_row(b, g_store, "y", sm1::SM_LOCKMODE_SH) == 0);
  int rc_b = -2, err_b = 0;
  std::thread tb([&] {
    rc_b = set(b, "x", "b");
    err_b = sm1::errnum();
  });
  settle();
  auto start = std::chrono::steady_clock::now();
  int rc_a = set(a, "y", "a");
  auto ms_taken = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  tb.join();
  CHECK(rc_a == 0);
  CHECK(rc_b == -1);
  CHECK(err_b == sm1::SM_ERR_DEADLOCK);
  CHECK(ms_taken >= 250);
  CHECK(ms_taken < 5000);
  REQUIRE(sm1::commit(a) == 0);
  sm1::abort(b);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("two upgraders' cycle is found as the second begins to wait") {
  // Verify an upgrade that must wait searches at once, as PostgreSQL's
  // early check does: such a cycle needs no timer to run out, and a
  // spec of PostgreSQL's expects it found without a wait.
  // 1. A, the older, and B both wait 60 s before a search. Both hold
  //    "x" shared, and B upgrades it, and waits for A.
  // 2. A upgrades it, closing the cycle: well inside 60 s B, the
  //    younger, fails with deadlock, and A's upgrade goes ahead.
  // 3. The same with the younger second: it fails, and the older goes
  //    ahead, as the victim is still the youngest.
  // 4. An upgrade that closes no cycle only waits: C upgrades past a
  //    reader, D, which then commits.
  for (bool older_second : {true, false}) {
    CAPTURE(older_second);
    lock_fixture_t f;
    sm1::session_t *a = f.open();
    sm1::session_t *b = f.open();
    REQUIRE(sm1::set_deadlock_ms(a, 60000) == 0);
    REQUIRE(sm1::set_deadlock_ms(b, 60000) == 0);
    REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(sm1::lock_row(a, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
    REQUIRE(sm1::lock_row(b, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
    int rc_a = -2, rc_b = -2, err_b = 0;
    auto start = std::chrono::steady_clock::now();
    std::thread first([&] {
      if (older_second) {
        rc_b = set(b, "x", "b");
        err_b = sm1::errnum();
      } else {
        rc_a = set(a, "x", "a");
      }
    });
    settle();
    if (older_second) {
      rc_a = set(a, "x", "a");
    } else {
      rc_b = set(b, "x", "b");
      err_b = sm1::errnum();
    }
    if (older_second) {
      // The victim holds its lock until its transaction ends.
      first.join();
      CHECK(rc_a == 0);
    } else {
      sm1::abort(b);
      first.join();
      CHECK(rc_a == 0);
    }
    auto ms_taken = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    CHECK(rc_b == -1);
    CHECK(err_b == sm1::SM_ERR_DEADLOCK);
    CHECK(ms_taken < 5000);
    REQUIRE(sm1::commit(a) == 0);
    sm1::abort(b);
    sm1::closedb(a);
    sm1::closedb(b);
  }

  lock_fixture_t f;
  sm1::session_t *c = f.open();
  sm1::session_t *d = f.open();
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(d, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(c, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(sm1::lock_row(d, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  int rc_c = -2;
  std::thread upgrader([&] { rc_c = set(c, "x", "c"); });
  settle();
  CHECK(-2 == rc_c);
  REQUIRE(sm1::commit(d) == 0);
  upgrader.join();
  CHECK(rc_c == 0);
  REQUIRE(sm1::commit(c) == 0);
  sm1::closedb(c);
  sm1::closedb(d);
}

TEST_CASE("a cycle across two partitions of the lock table is found") {
  // Verify the search sees waits in different partitions at once,
  // holding every partition's mutex.
  // 1. Find two row keys whose locks are in different partitions.
  // 2. A locks the first X, B the second X.
  // 3. A asks the second and waits; B asks the first, closing the
  //    cycle. B, the younger, fails with deadlock; A is granted.
  lock_fixture_t f;
  std::string k1 = "k0", k2;
  size_t p1 = wisr::locktab_t::part_of(wisr::lock_row_id(g_store, k1));
  for (int i = 1; k2.empty(); i++) {
    std::string k = "k" + std::to_string(i);
    if (wisr::locktab_t::part_of(wisr::lock_row_id(g_store, k)) != p1) {
      k2 = k;
    }
  }
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::set_deadlock_ms(a, 100) == 0);
  REQUIRE(sm1::set_deadlock_ms(b, 100) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock_row(a, g_store, k1, sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::lock_row(b, g_store, k2, sm1::SM_LOCKMODE_EX) == 0);
  int rc_a = -2;
  std::thread ta(
      [&] { rc_a = sm1::lock_row(a, g_store, k2, sm1::SM_LOCKMODE_EX); });
  settle();
  CHECK(sm1::lock_row(b, g_store, k1, sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DEADLOCK);
  sm1::abort(b);
  ta.join();
  CHECK(rc_a == 0);
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("a cycle through a wait for a transaction is found") {
  // Verify the search follows a lock's wait in a partition and a
  // writer's wait for a transaction, under xact_mu, in one cycle.
  // 1. A updates "x", its version in flight; B locks "y" X.
  // 2. A asks "y" and waits on B's lock.
  // 3. B updates "x" and waits for A's transaction, closing the
  //    cycle. B, the younger, fails with deadlock; A is granted.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::set_deadlock_ms(a, 100) == 0);
  REQUIRE(sm1::set_deadlock_ms(b, 100) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  REQUIRE(sm1::lock_row(b, g_store, "y", sm1::SM_LOCKMODE_EX) == 0);
  int rc_a = -2;
  std::thread ta(
      [&] { rc_a = sm1::lock_row(a, g_store, "y", sm1::SM_LOCKMODE_EX); });
  settle();
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DEADLOCK);
  sm1::abort(b);
  ta.join();
  CHECK(rc_a == 0);
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(a);
  sm1::closedb(b);
}

TEST_CASE("host notifications cover lock and transaction waits only") {
  // Verify host timers can surround only waits, with balanced notifications.
  // 1. Hold a row lock and an uncommitted write in session A.
  // 2. Install a hook on B and confirm an uncontended write calls no hook.
  // 3. Interrupt both kinds of wait through the hook and check both ends.
  // 4. Decline a paired end and confirm no end notification is delivered.
  // 5. Disable the hook and confirm a QA refusal calls no hook.
  lock_fixture_t f;
  sm1::session_t *a = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(set(a, "x", "a") == 0);
  REQUIRE(sm1::lock_row(a, g_store, "z", sm1::SM_LOCKMODE_EX) == 0);
  wait_notice_test_t notice;
  REQUIRE(sm1::set_lock_wait_hook(b, interrupt_notified_wait, &notice) == 0);
  uint32_t old_mask = sm_wait_mask();
  // Let the hook's interrupt end these waits, restoring the host's mask later.
  sm_set_wait_mask(1);
  // Bound a failed notification test, so a missing hook cannot hang it.
  sm_timer_id_t safety =
      sm_timer_at(sm_thread_self(), sm_now_us() + 1000000, 1, 1000000);
  defer {
    sm_timer_cancel(safety);
    sm_take(1);
    sm_set_wait_mask(old_mask);
  };
  REQUIRE(set(b, "y", "b") == 0);
  CHECK(notice.starts == 0);
  CHECK(notice.ends == 0);
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTERRUPTED);
  CHECK(sm_take(1) == 1);
  CHECK(notice.starts == 1);
  CHECK(notice.ends == 1);
  CHECK(sm1::lock_row(b, g_store, "z", sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTERRUPTED);
  CHECK(sm_take(1) == 1);
  CHECK(notice.starts == 2);
  CHECK(notice.ends == 2);
  notice.paired = false;
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm_take(1) == 1);
  CHECK(notice.starts == 3);
  CHECK(notice.ends == 2);
  REQUIRE(sm1::set_lock_wait_hook(b, nullptr, nullptr) == 0);
  REQUIRE(sm1::set_qa_nowait(b, true) == 0);
  CHECK(set(b, "x", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(notice.starts == 3);
  CHECK(notice.ends == 2);
  REQUIRE(sm1::abort(b) == 0);
  REQUIRE(sm1::abort(a) == 0);
  REQUIRE(sm1::closedb(b) == 0);
  REQUIRE(sm1::closedb(a) == 0);
}
