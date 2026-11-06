/*
 * test_read_latest.cc -- sm1::read_latest and SM_ERR_UPDATED: the
 * argument rules and the engine state a scenario script cannot
 * see (../LOCKMGR.md §1.9). The timelines themselves are the f
 * scripts in mvcc/in/.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <chrono>
#include <string>
#include <thread>

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
  REQUIRE(sm1::lock_row(t, g_store, "x", sm1::SM_LOCKMODE_EX) == 0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(t) == 0);

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  CHECK(latest(t, "x") == "<error>");
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::lock_row(t, g_store, "x", sm1::SM_LOCKMODE_EX) == 0);
  CHECK(latest(t, "x") == "0");
  REQUIRE(sm1::commit(t) == 0);
  sm1::closedb(t);
}

TEST_CASE("above read committed sm1_read_latest reads the transaction's own "
          "version alone") {
  // Verify that the snapshot of repeatable read and serializable,
  // which sm1_read_latest does not look past, does not stop it
  // reading a version the transaction wrote itself, its newest: an
  // insert's arbiter reads so a key its own statement took
  // (../../dg/WORKNOTE.md step 306).
  // At repeatable read and at serializable:
  // 1. In a statement, write "x" = "2": the statement reads "x" as
  //    "0", and sm1_read_latest reads "2".
  // 2. In a new transaction, under a lock on "y" and with no write
  //    of its own: SM_ERR_INVALID, as before.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::repeatable_read, sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    latest_fixture_t f;
    sm1::session_t *t = f.open();
    REQUIRE(sm1::begin(t, level) == 0);
    REQUIRE(sm1::stmt_begin(t) == 0);
    REQUIRE(set(t, "x", "2") == 0);
    std::string_view seen;
    REQUIRE(sm1::read(t, g_store, "x", seen) == 0);
    CHECK(seen == "0");
    CHECK(latest(t, "x") == "2");
    REQUIRE(sm1::abort(t) == 0);
    REQUIRE(sm1::begin(t, level) == 0);
    REQUIRE(sm1::lock_row(t, g_store, "y", sm1::SM_LOCKMODE_SH) == 0);
    CHECK(latest(t, "y") == "<error>");
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    REQUIRE(sm1::abort(t) == 0);
    sm1::closedb(t);
  }
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
  REQUIRE(sm1::lock_row(t, g_store, "x", sm1::SM_LOCKMODE_EX) == 0);

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

  REQUIRE(sm1::lock_row(t, g_store, "x", sm1::SM_LOCKMODE_SH) == 0);
  CHECK(latest(t, "x") == "u");
  CHECK(set(t, "x", "t") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::abort(t) == 0);
  sm1::closedb(t);
}

TEST_CASE("read_settled: a newest version only once every snapshot sees it") {
  // Verify sm1::read_settled, in a versioned store and a chained one.
  // 1. A key never written is settled gone.
  // 2. A writer's uncommitted insert is not settled, to it or another.
  // 3. Committed while an older snapshot is open, it is not settled;
  //    once that snapshot ends, it is, with its value.
  // 4. Deleted and committed with no snapshot open, it is settled
  //    gone.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *w = opendb(home.env(), "db");
  sm1::session_t *r = opendb(home.env(), "db");
  REQUIRE(w);
  REQUIRE(r);
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  uint64_t plain = mkstore(w);
  uint64_t chained = 0;
  REQUIRE(sm1::mkstore(w, chained, false, true) == 0);
  REQUIRE(sm1::commit(w) == 0);
  sm1::rawpin_t *pin = sm1::rawpin_new();
  for (uint64_t store : {plain, chained}) {
    CAPTURE(store);
    bool ch = store == chained;
    auto settled = [&](sm1::session_t *s, std::string &out) {
      std::string_view data;
      int rc = sm1::read_settled(s, store, "k", pin, data);
      out.assign(data);
      sm1::unpin(pin);
      return rc;
    };
    std::string v;
    REQUIRE(sm1::begin(r, sm1::xctlevel_t::read_committed) == 0);
    CHECK(1 == settled(r, v));
    REQUIRE(sm1::commit(r) == 0);

    REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE((ch ? sm1::chain_insert(w, store, "k", "v1")
                : sm1::insert(w, store, "k", "v1")) == 0);
    CHECK(2 == settled(w, v));
    REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
    std::string_view seen;
    REQUIRE((ch ? sm1::chain_read(r, store, "other", seen)
                : sm1::read(r, store, "other", seen)) == 1);
    CHECK(2 == settled(r, v));
    REQUIRE(sm1::commit(w) == 0);
    CHECK(2 == settled(r, v));
    REQUIRE(sm1::commit(r) == 0);

    REQUIRE(sm1::begin(r, sm1::xctlevel_t::read_committed) == 0);
    CHECK(0 == settled(r, v));
    CHECK("v1" == v);
    REQUIRE(sm1::commit(r) == 0);

    REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE((ch ? sm1::chain_erase(w, store, "k")
                : sm1::erase(w, store, "k")) == 0);
    REQUIRE(sm1::commit(w) == 0);
    REQUIRE(sm1::begin(r, sm1::xctlevel_t::read_committed) == 0);
    CHECK(1 == settled(r, v));
    REQUIRE(sm1::commit(r) == 0);
  }
  sm1::rawpin_free(pin);
  REQUIRE(sm1::closedb(w) == 0);
  REQUIRE(sm1::closedb(r) == 0);
}

TEST_CASE("read_newest: past the snapshot, naming a running writer") {
  // Verify sm1::read_newest and sm1::wait_writer, in both kinds of
  // store.
  // 1. A key never written is gone; one committed is read, whatever an
  //    older snapshot of the reader's sees.
  // 2. Another's uncommitted write names its writer; this
  //    transaction's own is read.
  // 3. wait_writer returns once the writer commits, on another thread;
  //    the key reads as it wrote.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *w = opendb(home.env(), "db");
  sm1::session_t *r = opendb(home.env(), "db");
  REQUIRE((w && r));
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  uint64_t plain = mkstore(w);
  uint64_t chained = 0;
  REQUIRE(sm1::mkstore(w, chained, false, true) == 0);
  REQUIRE(sm1::commit(w) == 0);
  sm1::rawpin_t *pin = sm1::rawpin_new();
  for (uint64_t store : {plain, chained}) {
    CAPTURE(store);
    bool ch = store == chained;
    auto ins = [&](sm1::session_t *s, const char *k, const char *v) {
      return ch ? sm1::chain_insert(s, store, k, v)
                : sm1::insert(s, store, k, v);
    };
    std::string_view data;
    uint64_t writer = 0;
    REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE((ch ? sm1::chain_read(r, store, "x", data)
                : sm1::read(r, store, "x", data)) == 1);
    CHECK(1 == sm1::read_newest(r, store, "k", pin, data, writer));
    REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(ins(w, "k", "v1") == 0);
    REQUIRE(sm1::commit(w) == 0);
    REQUIRE(0 == sm1::read_newest(r, store, "k", pin, data, writer));
    CHECK("v1" == data);

    REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(ins(w, "n", "w's") == 0);
    CHECK(0 == sm1::read_newest(w, store, "n", pin, data, writer));
    CHECK("w's" == data);
    CHECK(2 == sm1::read_newest(r, store, "n", pin, data, writer));
    CHECK(0 != writer);
    std::thread committer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      sm1::commit(w);
    });
    CHECK(0 == sm1::wait_writer(r, writer));
    committer.join();
    REQUIRE(0 == sm1::read_newest(r, store, "n", pin, data, writer));
    CHECK("w's" == data);
    sm1::unpin(pin);
    REQUIRE(sm1::commit(r) == 0);
  }
  sm1::rawpin_free(pin);
  REQUIRE(sm1::closedb(w) == 0);
  REQUIRE(sm1::closedb(r) == 0);
}

TEST_CASE("view_latest: reads see the newest committed state while on") {
  // Verify sm1::view_latest at repeatable read and serializable: a read
  // and a cursor see a version committed after the snapshot, and the
  // transaction's own write; off again, the snapshot's view returns.
  // 1. Begin; read x at the snapshot: "0".
  // 2. Another session commits x = "1", and inserts z.
  // 3. Write y = "own" in the first.
  // 4. Save and restore nested latest views: x reads "1", y "own",
  //    and a cursor finds z.
  // 5. View latest off: x reads "0" again, and the cursor finds no z.
  // 6. A transaction's end turns it off; with none it is SM_ERR_NOTRANS.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::repeatable_read, sm1::xctlevel_t::serializable}) {
    latest_fixture_t f;
    sm1::session_t *s = f.open();
    std::string_view v;
    REQUIRE(sm1::begin(s, level) == 0);
    REQUIRE(sm1::read(s, g_store, "x", v) == 0);
    CHECK(v == "0");
    commit_from_other(f, "x", "1");
    {
      sm1::session_t *u = f.open();
      REQUIRE(sm1::begin(u, sm1::xctlevel_t::read_committed) == 0);
      REQUIRE(sm1::insert(u, g_store, "z", "new") == 0);
      REQUIRE(sm1::commit(u) == 0);
      REQUIRE(sm1::closedb(u) == 0);
    }
    REQUIRE(set(s, "y", "own") == 0);
    // Count the keys a cursor over the whole store finds.
    auto keys = [&]() {
      sm1::scan_t *c = nullptr;
      REQUIRE(sm1::scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0, c) == 0);
      std::string_view k, d;
      std::string found;
      while (sm1::scannxt(c, k, d) == 0) {
        found += std::string(k);
      }
      sm1::scanend(c);
      return found;
    };

    bool previous = true;
    REQUIRE(sm1::view_latest(s, true, &previous) == 0);
    CHECK_FALSE(previous);
    bool nested = false;
    REQUIRE(sm1::view_latest(s, true, &nested) == 0);
    CHECK(nested);
    REQUIRE(sm1::view_latest(s, nested) == 0);
    REQUIRE(sm1::read(s, g_store, "x", v) == 0);
    CHECK(v == "1");
    REQUIRE(sm1::read(s, g_store, "y", v) == 0);
    CHECK(v == "own");
    CHECK(keys() == "xyz");

    REQUIRE(sm1::view_latest(s, previous) == 0);
    REQUIRE(sm1::read(s, g_store, "x", v) == 0);
    CHECK(v == "0");
    CHECK(keys() == "xy");
    REQUIRE(sm1::view_latest(s, true) == 0);
    REQUIRE(sm1::abort(s) == 0);
    CHECK(sm1::view_latest(s, true) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
    REQUIRE(sm1::begin(s, level) == 0);
    REQUIRE(sm1::read(s, g_store, "y", v) == 0);
    CHECK(v == "0");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }
}
