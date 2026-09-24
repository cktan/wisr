/*
 * test_xct.cc -- the commit sequencer, the active-transaction
 * table and commit cache, and the fail-stop state (SM1_DESIGN.md
 * §3.4, §4.3, §8.2).
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <atomic>
#include <future>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A database "db" with store 1, opened with opt. */
class xct_fixture_t {
public:
  /* Create "db" and store 1; the first session opens "db" with opt. */
  explicit xct_fixture_t(const sm1::options_t *opt = nullptr) {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_first = opendb(m_home.env(), "db", opt);
    REQUIRE(m_first);
    REQUIRE(sm1::begin(m_first, sm1::xctlevel_t::repeatable_read) == 0);
    m_store = mkstore(m_first);
    REQUIRE(sm1::commit(m_first) == 0);
  }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* The session the fixture opened. */
  sm1::session_t *first() const { return m_first; }

  /* Another session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* The shared database object behind the sessions. */
  wisr::db_t *db() const { return m_first->db; }

  /* The home directory. */
  home_t &home() { return m_home; }

private:
  home_t m_home;
  sm1::session_t *m_first = nullptr;
  uint64_t m_store = 0;
};

/* Insert key -> "v" into store in one committed transaction. */
int commit_one(sm1::session_t *s, uint64_t store, const std::string &key) {
  if (sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
      sm1::insert(s, store, key, "v") != 0) {
    return -1;
  }
  return sm1::commit(s);
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
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();
  sm1::session_t *c = f.open();

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

  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(c, f.store(), "a") == "<none>");
  CHECK(get(c, f.store(), "b") == "<none>");
  REQUIRE(sm1::commit(c) == 0);

  release.set_value();
  ta.join();
  f.db()->test_before_commit = nullptr;

  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(c, f.store(), "a") == "v");
  CHECK(get(c, f.store(), "b") == "v");
  REQUIRE(sm1::commit(c) == 0);
  sm1::closedb(b);
  sm1::closedb(c);
}

TEST_CASE("running out of memory after the commit point still commits") {
  // Verify a commit whose status record is written reports
  // success, even if what follows it cannot allocate (§8.2).
  // 1. Session A creates a store and inserts "k" into it.
  // 2. Make the commit cache insert throw std::bad_alloc, then
  //    commit: sm1::commit returns 0, since the record is written.
  // 3. A is left with no transaction: a new one begins on it.
  // 4. Session B sees "k" in the new store, so visible_ts moved
  //    past the commit and the store was not dropped.
  // 5. B updates "k", so A's row lock was released.
  xct_fixture_t f;
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(a);
  REQUIRE(sm1::insert(a, store, "k", "v") == 0);

  f.db()->test_cache_insert = [] { throw std::bad_alloc(); };
  CHECK_MESSAGE(sm1::commit(a) == 0, sm1::errmsg());
  f.db()->test_cache_insert = nullptr;

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, store, "k") == "v");
  CHECK(sm1::update(b, store, "k", "w") == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(b);
}

TEST_CASE(
    "a commit that runs out of memory before its record frees its commit_ts") {
  // Verify every allocation failure between claiming a commit_ts
  // and writing the status record hands the commit_ts back, so
  // later commits still become visible (§4.3, §8.2).
  // 1. For n = 0, 1, 2, ...: A reads "x" and inserts a key at
  //    SERIALIZABLE, and every allocation from the n-th on fails,
  //    from the start of sm1::commit up to the status write.
  // 2. sm1::commit throws std::bad_alloc, and A aborts its
  //    transaction.
  // 3. B commits a key of its own, and a fresh snapshot sees it:
  //    no stranded commit_ts is holding visible_ts back.
  // 4. Once n passes every allocation, A's commit succeeds, and a
  //    fresh snapshot sees A's key.
  xct_fixture_t f;
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();
  f.db()->test_before_commit = [](uint64_t) {
    t_alloc_budget = -1;
    return rocksdb::Status::OK();
  };

  long n = 0;
  for (;; n++) {
    REQUIRE(n < 10000);
    REQUIRE(sm1::begin(a, sm1::xctlevel_t::serializable) == 0);
    CHECK(get(a, f.store(), "x") == "<none>");
    REQUIRE(sm1::insert(a, f.store(), "a", "v") == 0);
    bool threw;
    t_alloc_budget = n;
    int rc = catch_oom([&] { return sm1::commit(a); }, threw);
    if (rc == 0) {
      break;
    }
    INFO("allocation ", n, " failed");
    CHECK(threw);
    REQUIRE_MESSAGE(sm1::abort(a) == 0, sm1::errmsg());
    std::string key = "b" + std::to_string(n);
    REQUIRE(commit_one(b, f.store(), key) == 0);
    REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(get(b, f.store(), key) == "v");
    REQUIRE(sm1::commit(b) == 0);
  }
  f.db()->test_before_commit = nullptr;
  CHECK(n > 0);

  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "a") == "v");
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(b);
}

TEST_CASE("a failed status write fails the database stop") {
  // Verify the §8.2 fail-stop state and recovery by reopening.
  // 1. Session B begins and reads committed "old".
  // 2. Session A inserts "x"; its status write fails, and
  //    sm1::commit returns SM_ERR_IO.
  // 3. B's existing snapshot still reads "old".
  // 4. A new transaction can begin, read, and commit with no
  //    writes, but insert and sm1::mkstore fail with SM_ERR_IO.
  // 5. Close every session and reopen: "x" never committed,
  //    and writes work again.
  xct_fixture_t f;
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();
  REQUIRE(commit_one(a, f.store(), "old") == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "old") == "v");

  f.db()->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(commit_one(a, f.store(), "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
  f.db()->test_before_commit = nullptr;

  CHECK(get(b, f.store(), "old") == "v");
  REQUIRE(sm1::commit(b) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(a, f.store(), "old") == "v");
  CHECK(get(a, f.store(), "x") == "<none>");
  CHECK(sm1::insert(a, f.store(), "y", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
  uint64_t newstore = 0;
  CHECK(sm1::mkstore(a, newstore) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
  REQUIRE(sm1::commit(a) == 0); // read-only: nothing to publish

  REQUIRE(sm1::closedb(a) == 0);
  REQUIRE(sm1::closedb(b) == 0);
  sm1::session_t *s = opendb(f.home().env(), "db", nullptr);
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, f.store(), "old") == "v");
  CHECK(get(s, f.store(), "x") == "<none>");
  put(s, f.store(), "y", "v");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a one-entry commit cache gives the same answers") {
  // Verify eviction never changes visibility (§3.4, §12.5).
  // 1. Open "db" with commit_cache_entries = 1.
  // 2. Session A takes an RR snapshot.
  // 3. Session B commits 50 keys, one per transaction.
  // 4. A still sees none of them; a new snapshot sees all 50,
  //    by point read and by scan; the cache never exceeds one
  //    entry.
  sm1::options_t opt;
  opt.commit_cache_entries = 1;
  xct_fixture_t f(&opt);
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 0; i < 50; i++) {
    REQUIRE(commit_one(b, f.store(), "k" + std::to_string(100 + i)) == 0);
    CHECK(f.db()->cache.size() <= 1);
  }
  CHECK(get(a, f.store(), "k100") == "<none>");
  CHECK(get(a, f.store(), "k149") == "<none>");
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 0; i < 50; i++) {
    CHECK(get(a, f.store(), "k" + std::to_string(100 + i)) == "v");
  }
  sm1::scan_t *c = scan(a, f.store(), nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  int n = 0;
  std::string_view k, d;
  while (sm1::scannxt(c, k, d) == 0) {
    n++;
  }
  sm1::scanend(c);
  CHECK(n == 50);
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(b);

  opt.commit_cache_entries = 0;
  REQUIRE(sm1::mkdb(f.home().env(), "db2", nullptr) == 0);
  CHECK(opendb(f.home().env(), "db2", &opt) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
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
  std::vector<sm1::session_t *> sessions;
  for (int t = 0; t < writers + 1; t++) {
    sessions.push_back(f.open());
  }
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < writers; t++) {
    threads.emplace_back([&, t] {
      sm1::session_t *s = sessions[t];
      for (int i = 0; i < per_writer; i++) {
        std::string base = std::to_string(t) + "." + std::to_string(i);
        if (sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
            sm1::insert(s, f.store(),
                        std::string_view((base + ".a").data(), base.size() + 2),
                        "v") != 0 ||
            sm1::insert(s, f.store(),
                        std::string_view((base + ".b").data(), base.size() + 2),
                        "v") != 0 ||
            sm1::commit(s) != 0) {
          failures++;
        }
      }
    });
  }

  size_t last = 0;
  int torn = 0, shrank = 0, scans = 0;
  std::thread reader([&] {
    sm1::session_t *s = sessions[writers];
    bool final_pass = false;
    while (!final_pass) {
      final_pass = done;
      if (sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
        failures++;
        return;
      }
      sm1::scan_t *c = scan(s, f.store(), nullptr, 0, 0, nullptr, 0, 0);
      std::vector<std::string> keys;
      std::string_view k, d;
      while (c && sm1::scannxt(c, k, d) == 0) {
        keys.emplace_back(k);
      }
      sm1::scanend(c);
      sm1::commit(s);
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
  for (sm1::session_t *s : sessions) {
    sm1::closedb(s);
  }
}
