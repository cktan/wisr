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

TEST_CASE("on_commit functions run after the commit, newest first, once") {
  // Verify when sm1::on_commit's functions run, and when they do not.
  // 1. A then B are added and the transaction commits: B ran before
  //    A, each outside any transaction, since each could begin one.
  // 2. The next commit runs nothing: a function runs once.
  // 3. A transaction that aborts runs none.
  // 4. A function added after a savepoint is dropped by a rollback
  //    to it; the one added before still runs.
  // 5. Outside a transaction it is SM_ERR_NOTRANS, and an empty
  //    function SM_ERR_INVALID.
  // 6. A prepared transaction keeps its functions: one runs when
  //    the session that prepared commits, another when a second
  //    session commits by gid, and neither before.
  xct_fixture_t f;
  sm1::session_t *s = f.first();
  std::string ran;
  auto add = [&](char c) {
    return sm1::on_commit(s, [&ran, s, c] {
      ran += c;
      if (sm1::begin(s, sm1::xctlevel_t::read_committed) == 0 &&
          sm1::abort(s) == 0) {
        ran += '.';
      }
    });
  };
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(add('A') == 0);
  REQUIRE(add('B') == 0);
  CHECK(ran.empty());
  REQUIRE(sm1::commit(s) == 0);
  CHECK(ran == "B.A.");

  REQUIRE(commit_one(s, f.store(), "k1") == 0);
  CHECK(ran == "B.A.");

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(add('C') == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(commit_one(s, f.store(), "k2") == 0);
  CHECK(ran == "B.A.");

  ran.clear();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(add('D') == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(add('E') == 0);
  REQUIRE(sm1::rollback(s, 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(ran == "D.");

  CHECK(add('F') == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::on_commit(s, nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(s) == 0);

  ran.clear();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(add('G') == 0);
  REQUIRE(sm1::prepare(s, "own") == 0);
  CHECK(ran.empty());
  REQUIRE(sm1::commit(s) == 0);
  CHECK(ran == "G.");
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(add('H') == 0);
  REQUIRE(sm1::prepare(s, "other") == 0);
  sm1::session_t *t = f.open();
  REQUIRE(sm1::commit_prepared(t, "other") == 0);
  CHECK(ran == "G.H.");
  REQUIRE(sm1::closedb(t) == 0);
}

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

TEST_CASE("the smallest commit cache gives the same answers") {
  // Verify eviction never changes visibility (§3.4, §12.5).
  // 1. Open "db" with commit_cache_entries = 1: one set of two
  //    slots, the smallest there is.
  // 2. Session A takes an RR snapshot.
  // 3. Session B commits 50 keys, one per transaction.
  // 4. A still sees none of them; a new snapshot sees all 50,
  //    by point read and by scan; the cache never holds more
  //    than its two slots.
  sm1::options_t opt;
  opt.commit_cache_entries = 1;
  xct_fixture_t f(&opt);
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 0; i < 50; i++) {
    REQUIRE(commit_one(b, f.store(), "k" + std::to_string(100 + i)) == 0);
    CHECK(wisr::xct_cache_used(f.db()) <= 2);
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

TEST_CASE("a commit read from the status table is remembered") {
  // Verify a cache miss that finds a commit record keeps it, so
  // the next version of that transaction costs no disk read.
  // 1. Commit a key, noting the writer's xid, and close the
  //    database, which frees the cache: its idle time is 0, so its
  //    last session's close closes it.
  // 2. Reopen: the cache does not hold the xid.
  // 3. Read the key: it is there, and the xid is now cached.
  // 4. Read it again: the same answer, the same entry.
  home_t home;
  sm1::options_t now;
  now.idle_close_ms = 0;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db", &now);
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "k", "v");
  const uint64_t xid = s->xid;
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);

  s = opendb(home.env(), "db", &now);
  REQUIRE(s);
  CHECK_FALSE(wisr::xct_cache_has(s->db, xid));

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "k") == "v");
  CHECK(wisr::xct_cache_has(s->db, xid));

  CHECK(get(s, store, "k") == "v");
  CHECK(wisr::xct_cache_has(s->db, xid));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

/* Commit key -> "v" from s in a transaction of its own; its xid. */
uint64_t commit_key(sm1::session_t *s, uint64_t store, const std::string &key) {
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, key, "v");
  uint64_t xid = s->xid;
  REQUIRE(sm1::commit(s) == 0);
  return xid;
}

TEST_CASE("a commit cache set holds two, and a third evicts the smaller xid") {
  // Verify the two-slot set (COMMITCACHE.md §3.4).
  // 1. Open "db" with commit_cache_entries = 2: one set of two
  //    slots, which every transaction shares.
  // 2. Commit k1, then k2: both are cached.
  // 3. Commit k3: it takes the slot of k1, the smaller xid, and
  //    k2 stays.
  sm1::options_t opt;
  opt.commit_cache_entries = 2;
  xct_fixture_t f(&opt);
  sm1::session_t *b = f.open();
  const uint64_t x1 = commit_key(b, f.store(), "k1");
  const uint64_t x2 = commit_key(b, f.store(), "k2");
  CHECK(wisr::xct_cache_has(f.db(), x1));
  CHECK(wisr::xct_cache_has(f.db(), x2));

  const uint64_t x3 = commit_key(b, f.store(), "k3");
  CHECK_FALSE(wisr::xct_cache_has(f.db(), x1));
  CHECK(wisr::xct_cache_has(f.db(), x2));
  CHECK(wisr::xct_cache_has(f.db(), x3));
  CHECK(wisr::xct_cache_used(f.db()) == 2);
  sm1::closedb(b);
}

TEST_CASE("an old commit read back from disk keeps the newer of its set") {
  // Verify a miss takes the slot of the smaller xid, so a lookup of
  // an old transaction does not push out the newest commit.
  // 1. With one set of two, commit k1, k2 and k3: k2 and k3 are
  //    cached.
  // 2. Read k1: its commit is read from disk and cached in k2's
  //    slot, the smaller xid of the two; k3 stays.
  // 3. Read k2: it takes k1's slot in turn; k3 still stays.
  // 4. Every read is right.
  sm1::options_t opt;
  opt.commit_cache_entries = 2;
  xct_fixture_t f(&opt);
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();
  const uint64_t x1 = commit_key(b, f.store(), "k1");
  const uint64_t x2 = commit_key(b, f.store(), "k2");
  const uint64_t x3 = commit_key(b, f.store(), "k3");
  REQUIRE(wisr::xct_cache_has(f.db(), x3));

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(a, f.store(), "k1") == "v");
  CHECK(wisr::xct_cache_has(f.db(), x1));
  CHECK_FALSE(wisr::xct_cache_has(f.db(), x2));
  CHECK(wisr::xct_cache_has(f.db(), x3));

  CHECK(get(a, f.store(), "k2") == "v");
  CHECK(wisr::xct_cache_has(f.db(), x2));
  CHECK_FALSE(wisr::xct_cache_has(f.db(), x1));
  CHECK(wisr::xct_cache_has(f.db(), x3));
  CHECK(get(a, f.store(), "k3") == "v");
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(b);
}

TEST_CASE("a long transaction committing late is cached, and evicted first") {
  // Verify a commit always enters the cache, and the victim is the
  // smaller xid, not the earlier entry.
  // 1. With one set of two, L writes first, so its xid is the
  //    smallest, and does not commit yet.
  // 2. k2 commits, then L commits: both are cached, though L's
  //    xid is the smaller.
  // 3. k3 commits: it evicts L, the smaller xid, though L entered
  //    the cache after k2.
  // 4. An aborted writer is never cached.
  sm1::options_t opt;
  opt.commit_cache_entries = 2;
  xct_fixture_t f(&opt);
  sm1::session_t *l = f.open();
  sm1::session_t *b = f.open();
  REQUIRE(sm1::begin(l, sm1::xctlevel_t::read_committed) == 0);
  put(l, f.store(), "kl", "v");
  const uint64_t xl = l->xid;

  const uint64_t x2 = commit_key(b, f.store(), "k2");
  REQUIRE(xl < x2);
  REQUIRE(sm1::commit(l) == 0);
  CHECK(wisr::xct_cache_has(f.db(), xl));
  CHECK(wisr::xct_cache_has(f.db(), x2));

  const uint64_t x3 = commit_key(b, f.store(), "k3");
  CHECK_FALSE(wisr::xct_cache_has(f.db(), xl));
  CHECK(wisr::xct_cache_has(f.db(), x2));
  CHECK(wisr::xct_cache_has(f.db(), x3));

  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  put(b, f.store(), "k4", "v");
  const uint64_t x4 = b->xid;
  REQUIRE(sm1::abort(b) == 0);
  sm1::session_t *a = f.first();
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(a, f.store(), "k4") == "<none>");
  CHECK_FALSE(wisr::xct_cache_has(f.db(), x4));
  REQUIRE(sm1::commit(a) == 0);
  sm1::closedb(l);
  sm1::closedb(b);
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
