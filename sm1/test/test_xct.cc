/*
 * test_xct.cc -- the commit sequencer, the active-transaction
 * table and commit cache, and the fail-stop state (SM1_DESIGN.md
 * §3.4, §4.3, §8.2).
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A database "db" with store 1, opened with opt. */
class xct_fixture_t {
public:
  /* Create "db" and store 1; the first session opens "db" with opt. */
  explicit xct_fixture_t(const sm_options_t *opt = nullptr) {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_first = sm_opendb(m_home.env(), "db", opt);
    REQUIRE(m_first);
    REQUIRE(sm_begin(m_first, SM_XCT_REPEATABLE_READ) == 0);
    m_store = mkstore(m_first);
    REQUIRE(sm_commit(m_first) == 0);
  }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* The session the fixture opened. */
  sm_session_t *first() const { return m_first; }

  /* Another session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* The shared database object behind the sessions. */
  wisr::db_t *db() const { return m_first->db; }

  /* The home directory. */
  home_t &home() { return m_home; }

private:
  home_t m_home;
  sm_session_t *m_first = nullptr;
  uint64_t m_store = 0;
};

/* Insert key -> "v" into store in one committed transaction. */
int commit_one(sm_session_t *s, uint64_t store, const std::string &key) {
  if (sm_begin(s, SM_XCT_REPEATABLE_READ) != 0 ||
      sm_insert(s, store, key.data(), key.size(), "v", 1) != 0) {
    return -1;
  }
  return sm_commit(s);
}

} // namespace

TEST_CASE("a stalled commit holds back every later commit's visibility") {
  // Verify the sequencer publishes in commit_ts order (§4.3).
  // 1. Session A commits "a" but stalls after claiming its
  //    commit_ts, before its status record is written.
  // 2. Session B commits "b" meanwhile; its commit returns.
  // 3. A new snapshot sees neither "a" nor "b": B's later
  //    commit_ts must stay hidden while A's is unpublished.
  // 4. Release A; a new snapshot sees both.
  xct_fixture_t f;
  sm_session_t *a = f.first();
  sm_session_t *b = f.open();
  sm_session_t *c = f.open();

  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> calls{0};
  f.db()->test_before_commit = [&](uint64_t) {
    if (calls++ == 0) {
      entered.set_value();
      released.wait();
    }
    return rocksdb::Status::OK();
  };

  std::thread ta([&] { CHECK(commit_one(a, f.store(), "a") == 0); });
  entered.get_future().wait();
  REQUIRE(commit_one(b, f.store(), "b") == 0);

  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(c, f.store(), "a") == "<none>");
  CHECK(get(c, f.store(), "b") == "<none>");
  REQUIRE(sm_commit(c) == 0);

  release.set_value();
  ta.join();
  f.db()->test_before_commit = nullptr;

  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(c, f.store(), "a") == "v");
  CHECK(get(c, f.store(), "b") == "v");
  REQUIRE(sm_commit(c) == 0);
  sm_closedb(b);
  sm_closedb(c);
}

TEST_CASE("a failed status write fails the database stop") {
  // Verify the §8.2 fail-stop state and recovery by reopening.
  // 1. Session B begins and reads committed "old".
  // 2. Session A inserts "x"; its status write fails, and
  //    sm_commit returns SM_ERR_IO.
  // 3. B's existing snapshot still reads "old".
  // 4. A new transaction can begin, read, and commit with no
  //    writes, but insert and sm_mkstore fail with SM_ERR_IO.
  // 5. Close every session and reopen: "x" never committed,
  //    and writes work again.
  xct_fixture_t f;
  sm_session_t *a = f.first();
  sm_session_t *b = f.open();
  REQUIRE(commit_one(a, f.store(), "old") == 0);
  REQUIRE(sm_begin(b, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(b, f.store(), "old") == "v");

  f.db()->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(commit_one(a, f.store(), "x") == -1);
  CHECK(sm_errno() == SM_ERR_IO);
  f.db()->test_before_commit = nullptr;

  CHECK(get(b, f.store(), "old") == "v");
  REQUIRE(sm_commit(b) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(a, f.store(), "old") == "v");
  CHECK(get(a, f.store(), "x") == "<none>");
  CHECK(sm_insert(a, f.store(), "y", 1, "v", 1) == -1);
  CHECK(sm_errno() == SM_ERR_IO);
  uint64_t newstore = 0;
  CHECK(sm_mkstore(a, &newstore) == -1);
  CHECK(sm_errno() == SM_ERR_IO);
  REQUIRE(sm_commit(a) == 0); // read-only: nothing to publish

  REQUIRE(sm_closedb(a) == 0);
  REQUIRE(sm_closedb(b) == 0);
  sm_session_t *s = sm_opendb(f.home().env(), "db", nullptr);
  REQUIRE(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, f.store(), "old") == "v");
  CHECK(get(s, f.store(), "x") == "<none>");
  put(s, f.store(), "y", "v");
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_closedb(s) == 0);
}

TEST_CASE("a one-entry commit cache gives the same answers") {
  // Verify eviction never changes visibility (§3.4, §12.5).
  // 1. Open "db" with commit_cache_entries = 1.
  // 2. Session A takes an RR snapshot.
  // 3. Session B commits 50 keys, one per transaction.
  // 4. A still sees none of them; a new snapshot sees all 50,
  //    by point read and by scan; the cache never exceeds one
  //    entry.
  sm_options_t opt;
  sm_options_init(&opt);
  opt.commit_cache_entries = 1;
  xct_fixture_t f(&opt);
  sm_session_t *a = f.first();
  sm_session_t *b = f.open();

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 50; i++) {
    REQUIRE(commit_one(b, f.store(), "k" + std::to_string(100 + i)) == 0);
    CHECK(f.db()->cache.size() <= 1);
  }
  CHECK(get(a, f.store(), "k100") == "<none>");
  CHECK(get(a, f.store(), "k149") == "<none>");
  REQUIRE(sm_commit(a) == 0);

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 50; i++) {
    CHECK(get(a, f.store(), "k" + std::to_string(100 + i)) == "v");
  }
  sm_scan_t *c = sm_scan(a, f.store(), nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  int n = 0;
  const char *k, *d;
  size_t kl, dl;
  while (sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
    n++;
  }
  sm_scanend(c);
  CHECK(n == 50);
  REQUIRE(sm_commit(a) == 0);
  sm_closedb(b);

  opt.commit_cache_entries = 0;
  REQUIRE(sm_mkdb(f.home().env(), "db2", nullptr) == 0);
  CHECK(sm_opendb(f.home().env(), "db2", &opt) == nullptr);
  CHECK(sm_errno() == SM_ERR_INVALID);
}

TEST_CASE("concurrent commits become visible atomically and monotonically") {
  // Verify snapshot correctness under concurrent committers.
  // 1. Four writer threads each commit 200 transactions, each
  //    inserting a pair of keys "<t>.<i>.a" and "<t>.<i>.b".
  // 2. A reader thread repeatedly scans at a fresh RR snapshot
  //    and checks every pair is whole (both keys or neither)
  //    and the row count never shrinks.
  // 3. At the end every pair is visible.
  xct_fixture_t f;
  const int writers = 4, per_writer = 200;
  std::vector<sm_session_t *> sessions;
  for (int t = 0; t < writers + 1; t++) {
    sessions.push_back(f.open());
  }
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < writers; t++) {
    threads.emplace_back([&, t] {
      sm_session_t *s = sessions[t];
      for (int i = 0; i < per_writer; i++) {
        std::string base = std::to_string(t) + "." + std::to_string(i);
        if (sm_begin(s, SM_XCT_REPEATABLE_READ) != 0 ||
            sm_insert(s, f.store(), (base + ".a").data(), base.size() + 2, "v",
                      1) != 0 ||
            sm_insert(s, f.store(), (base + ".b").data(), base.size() + 2, "v",
                      1) != 0 ||
            sm_commit(s) != 0) {
          failures++;
        }
      }
    });
  }

  size_t last = 0;
  int torn = 0, shrank = 0, scans = 0;
  std::thread reader([&] {
    sm_session_t *s = sessions[writers];
    bool final_pass = false;
    while (!final_pass) {
      final_pass = done;
      if (sm_begin(s, SM_XCT_REPEATABLE_READ) != 0) {
        failures++;
        return;
      }
      sm_scan_t *c = sm_scan(s, f.store(), nullptr, 0, 0, nullptr, 0, 0);
      std::vector<std::string> keys;
      const char *k, *d;
      size_t kl, dl;
      while (c && sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
        keys.emplace_back(k, kl);
      }
      sm_scanend(c);
      sm_commit(s);
      // Keys sort as "<t>.<i>.a" then "<t>.<i>.b": pairs are adjacent.
      if (keys.size() % 2 != 0) {
        torn++;
      }
      for (size_t j = 0; j + 1 < keys.size(); j += 2) {
        std::string ka = keys[j], kb = keys[j + 1];
        if (ka.back() != 'a' || kb.back() != 'b' ||
            ka.substr(0, ka.size() - 1) != kb.substr(0, kb.size() - 1)) {
          torn++;
        }
      }
      if (keys.size() < last) {
        shrank++;
      }
      last = keys.size();
      scans++;
    }
  });

  for (std::thread &t : threads) {
    t.join();
  }
  done = true;
  reader.join();
  CHECK(failures == 0);
  CHECK(torn == 0);
  CHECK(shrank == 0);
  CHECK(last == size_t(2 * writers * per_writer));
  MESSAGE("reader scans: " << scans);
  for (sm_session_t *s : sessions) {
    sm_closedb(s);
  }
}
