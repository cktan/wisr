/*
 * test_catkv.cc -- the catalog key-value store over sm1's store 0
 * (sm3/CATALOG.md §1).
 */

#include "db.h"
#include "doctest.h"

#include "sm1.h"
#include "sm2.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kvs_t = std::vector<std::pair<std::string, std::string>>;

/* Every key and value under prefix, in order. */
kvs_t scan_all(sm2::catkv_t *kv, std::string_view prefix) {
  sm2::catkv_scan_t *c = nullptr;
  REQUIRE_MESSAGE(sm2::catkv_scan(kv, prefix, c) == 0, sm1::errmsg());
  kvs_t out;
  std::string_view k, v;
  while (sm2::catkv_scannxt(c, k, v) == 0) {
    out.emplace_back(std::string(k), std::string(v));
  }
  sm2::catkv_scanend(c);
  return out;
}

/* A catalog handle on s; it must open. */
sm2::catkv_t *open_kv(sm1::session_t *s) {
  sm2::catkv_t *kv = nullptr;
  REQUIRE(sm2::catkv_open(s, kv) == 0);
  return kv;
}

} // namespace

TEST_CASE("the catalog store reads and writes as a transaction does") {
  // Verify catkv's calls, and that they follow the transaction.
  // 1. Insert, get, update and erase keys, which are the caller's
  //    own, the prefix never showing.
  // 2. A scan yields exactly the keys under its prefix, in order,
  //    including prefixes ending in 0xFF bytes and the empty one.
  // 3. An abort undoes a write; so does a rollback to a savepoint
  //    taken before it, while writes before the savepoint stay.
  // 4. A taken key is SM_ERR_DUPKEY; a missing one to update or
  //    erase is SM_ERR_NOTFOUND.
  // 5. Committed keys are there after a restart.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::catkv_t *kv = open_kv(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::catkv_insert(kv, "t/1", "one") == 0);
  REQUIRE(sm2::catkv_insert(kv, "t/2", "two") == 0);
  REQUIRE(sm2::catkv_insert(kv, "n/a", "1") == 0);
  REQUIRE(sm2::catkv_insert(kv, std::string("x\xff", 2), "ff") == 0);
  REQUIRE(sm2::catkv_insert(kv, "y", "why") == 0);
  REQUIRE(sm2::catkv_update(kv, "t/2", "TWO") == 0);
  std::string_view v;
  REQUIRE(sm2::catkv_get(kv, "t/2", v) == 0);
  CHECK(v == "TWO");
  CHECK(sm2::catkv_get(kv, "t/3", v) == 1);
  CHECK(scan_all(kv, "t/") == kvs_t{{"t/1", "one"}, {"t/2", "TWO"}});
  CHECK(scan_all(kv, std::string("x\xff", 2)) ==
        kvs_t{{std::string("x\xff", 2), "ff"}});
  CHECK(scan_all(kv, "").size() == 5);
  REQUIRE(sm2::catkv_erase(kv, "y") == 0);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::catkv_insert(kv, "t/3", "three") == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm2::catkv_get(kv, "t/3", v) == 1);
  REQUIRE(sm2::catkv_insert(kv, "t/4", "four") == 0);
  int sp = sm1::savepoint(s);
  REQUIRE(sp > 0);
  REQUIRE(sm2::catkv_insert(kv, "t/5", "five") == 0);
  REQUIRE(sm1::rollback(s, sp) == 0);
  CHECK(sm2::catkv_get(kv, "t/5", v) == 1);
  CHECK(sm2::catkv_get(kv, "t/4", v) == 0);

  CHECK(sm2::catkv_insert(kv, "t/1", "again") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm2::catkv_update(kv, "t/9", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm2::catkv_erase(kv, "t/9") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm2::catkv_close(kv);

  db.restart();
  s = db.open();
  kv = open_kv(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(scan_all(kv, "t/") ==
        kvs_t{{"t/1", "one"}, {"t/2", "TWO"}, {"t/4", "four"}});
  REQUIRE(sm1::commit(s) == 0);
  sm2::catkv_close(kv);
}

TEST_CASE("the catalog store orders concurrent writers") {
  // Verify catkv under two sessions, as DDL uses it.
  // 1. Two REPEATABLE READ transactions insert one key; the first
  //    to commit wins and the other's insert is SM_ERR_CONFLICT.
  // 2. A shared lock on a key lets another shared lock through,
  //    but blocks an update of the key until its holder commits;
  //    the update then goes through.
  db_t db;
  sm1::session_t *a = db.open(), *b = db.open();
  sm2::catkv_t *ka = open_kv(a), *kb = open_kv(b);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm2::catkv_insert(ka, "name", "a") == 0);
  REQUIRE(sm1::commit(a) == 0);
  CHECK(sm2::catkv_insert(kb, "name", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm1::abort(b);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::catkv_lock(ka, "name", sm1::lockmode_t::shared) == 0);
  REQUIRE(sm2::catkv_lock(kb, "name", sm1::lockmode_t::shared) == 0);
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  std::atomic<bool> done{false};
  int rc = -2;
  std::thread writer([&] {
    rc = sm2::catkv_update(kb, "name", "b");
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK_FALSE(done.load());
  REQUIRE(sm1::commit(a) == 0);
  writer.join();
  CHECK(rc == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm2::catkv_close(ka);
  sm2::catkv_close(kb);
}
