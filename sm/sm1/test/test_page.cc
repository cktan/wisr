/*
 * test_page.cc -- pages (../BTREE.md §2): keys of class 0x04, written
 * outside every transaction and read through the block cache, apart
 * from every version of the store.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

/* Write one page, failing the test on an error. */
void write_page(sm1::session_t *s, uint64_t store, std::string_view key,
                std::string_view bytes) {
  std::vector<sm1::page_put_t> puts{{key, bytes, false, false}};
  REQUIRE_MESSAGE(sm1::page_write(s, store, puts) == 0, sm1::errmsg());
}

/* The page at key as s reads it, or "<none>"; fails on an error. */
std::string read_page(sm1::session_t *s, uint64_t store, std::string_view key) {
  sm1::page_t *page = nullptr;
  std::string_view bytes;
  int r = sm1::page_read(s, store, key, false, page, bytes);
  REQUIRE_MESSAGE(r >= 0, sm1::errmsg());
  if (r == 1) {
    return "<none>";
  }
  std::string out(bytes);
  sm1::page_release(page);
  return out;
}

} // namespace

TEST_CASE("page: written outside the transaction, read through the cache") {
  // Verify a page's write commits when it is made, and is read back.
  // 1. A page never written reads as none.
  // 2. Write two pages in one call, in a transaction, and abort it:
  //    both are still there, from this session and from another.
  // 3. Rewrite one, erase the other in one call: the reads follow.
  // 4. A pin taken before the rewrite keeps the old bytes.
  // 5. Reattached, the store holds the page as last written.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK("<none>" == read_page(s, store, "a"));
  std::vector<sm1::page_put_t> puts{{"a", "one", false, true},
                                    {"b", "two", false, false}};
  REQUIRE(sm1::page_write(s, store, puts) == 0);
  REQUIRE(sm1::abort(s) == 0);

  sm1::session_t *t = opendb(home.env(), "db");
  REQUIRE(t);
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK("one" == read_page(t, store, "a"));
  CHECK("two" == read_page(t, store, "b"));

  sm1::page_t *held = nullptr;
  std::string_view held_bytes;
  REQUIRE(sm1::page_read(t, store, "a", true, held, held_bytes) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::vector<sm1::page_put_t> change{{"a", "uno", false, true},
                                      {"b", {}, true, false}};
  REQUIRE(sm1::page_write(s, store, change) == 0);
  CHECK("uno" == read_page(t, store, "a"));
  CHECK("<none>" == read_page(t, store, "b"));
  CHECK("one" == held_bytes);
  sm1::page_release(held);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::commit(t) == 0);
  REQUIRE(sm1::closedb(t) == 0);
  REQUIRE(sm1::closedb(s) == 0);

  home.reattach();
  s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK("uno" == read_page(s, store, "a"));
  CHECK("<none>" == read_page(s, store, "b"));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: a read that misses never puts back a page replaced") {
  // Verify the fill latch: a miss's read and its insert are not split
  // by a write, so the cache never holds a page older than RocksDB's.
  // 1. Attach with a cache far smaller than the pages, so reads miss.
  // 2. Four readers read the pages at random, missing and filling.
  // 3. A writer rewrites each page with a count that only rises, and
  //    reads it back at once: always what it wrote, never older.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  sm1::env_options_t opt;
  opt.block_cache_bytes = 64 << 10;
  sm1::env_t *env = nullptr;
  REQUIRE_MESSAGE(sm1::attach(tmp.path(), env, &opt) == 0, sm1::errmsg());
  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *w = opendb(env, "db");
  REQUIRE(w);
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(w);
  REQUIRE(sm1::commit(w) == 0);

  constexpr int pages = 64;
  const std::string pad(4000, 'p');
  auto bytes_of = [&](int n) { return std::to_string(n) + "|" + pad; };
  auto count_of = [](std::string_view b) {
    return std::stoi(std::string(b.substr(0, b.find('|'))));
  };
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  for (int p = 0; p < pages; p++) {
    write_page(w, store, "k" + std::to_string(p), bytes_of(0));
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&, r] {
      sm1::session_t *s = opendb(env, "db");
      sm1::begin(s, sm1::xctlevel_t::read_committed);
      unsigned x = 12345 + r;
      while (!stop) {
        x = x * 1103515245 + 12345;
        sm1::page_t *page = nullptr;
        std::string_view b;
        std::string key = "k" + std::to_string((x >> 8) % pages);
        if (sm1::page_read(s, store, key, false, page, b) == 0) {
          sm1::page_release(page);
        }
      }
      sm1::commit(s);
      sm1::closedb(s);
    });
  }
  std::vector<int> counts(pages, 0);
  int stale = 0;
  for (int i = 0; i < 4000; i++) {
    int p = (i * 7) % pages;
    std::string key = "k" + std::to_string(p);
    write_page(w, store, key, bytes_of(++counts[p]));
    sm1::page_t *page = nullptr;
    std::string_view b;
    REQUIRE(sm1::page_read(w, store, key, false, page, b) == 0);
    if (count_of(b) != counts[p]) {
      stale++;
    }
    sm1::page_release(page);
  }
  stop = true;
  for (auto &th : readers) {
    th.join();
  }
  CHECK(0 == stale);
  REQUIRE(sm1::commit(w) == 0);
  REQUIRE(sm1::closedb(w) == 0);
  REQUIRE(sm1::detach(env, 1) == 0);
}

TEST_CASE("page: dropped by prefix, with their cache entries") {
  // Verify sm1::page_drop removes the pages under a prefix alone.
  // 1. Write pages under "i1", "i10" and "i2", and read each into
  //    the cache.
  // 2. Drop "i1": its pages, "i10"'s included, read as none, though
  //    they were cached; "i2"'s are still there.
  // 3. A page made again under "i1" reads as the new one.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  for (const char *k : {"i1/0", "i1/1", "i10/0", "i2/0", "i2/1"}) {
    write_page(s, store, k, std::string("v") + k);
    CHECK(std::string("v") + k == read_page(s, store, k));
  }
  REQUIRE(sm1::page_drop(s, store, "i1") == 0);
  CHECK("<none>" == read_page(s, store, "i1/0"));
  CHECK("<none>" == read_page(s, store, "i1/1"));
  CHECK("<none>" == read_page(s, store, "i10/0"));
  CHECK("vi2/0" == read_page(s, store, "i2/0"));
  CHECK("vi2/1" == read_page(s, store, "i2/1"));
  write_page(s, store, "i1/0", "again");
  CHECK("again" == read_page(s, store, "i1/0"));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: no version meets a page") {
  // Verify pages and versions of one store keep apart.
  // 1. In a chained store, write a version and pages under the same
  //    user key; update the version so an old one is chained.
  // 2. A scan of the store and a read of the key give the version;
  //    the page reads as written.
  // 3. Compaction, through the GC filter, keeps the page.
  // 4. With an old version compaction has not seen, reattached: the
  //    chain's next old id is past it, not read from a page.
  // 5. A store holding only pages is empty to a load, and a rollback
  //    of the load to a savepoint before it leaves the pages.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = 0;
  REQUIRE(sm1::mkstore(s, store, false, true) == 0);
  uint64_t bare = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::chain_insert(s, store, "k", "v1") == 0);
  write_page(s, store, "k", "page");
  write_page(s, store, std::string(8, '\xff'), "last page");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::chain_update(s, store, "k", "v2") == 0);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::string_view data;
  REQUIRE(sm1::chain_read(s, store, "k", data) == 0);
  CHECK("v2" == data);
  sm1::scan_t *c = nullptr;
  REQUIRE(sm1::chain_scan(s, store, nullptr, 0, 0, nullptr, 0, 0, c) == 0);
  std::string_view key;
  int rows = 0;
  while (sm1::scannxt(c, key, data) == 0) {
    CHECK("k" == key);
    rows++;
  }
  sm1::scanend(c);
  CHECK(1 == rows);
  CHECK("page" == read_page(s, store, "k"));
  REQUIRE(sm1::commit(s) == 0);

  rocksdb::CompactRangeOptions copt;
  copt.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  REQUIRE(s->db->rdb
              ->CompactRange(copt, wisr::db_store(s->db, store).cf, nullptr,
                             nullptr)
              .ok());
  // An old version compaction has not seen, so the next id is past it.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::chain_update(s, store, "k", "v3") == 0);
  REQUIRE(sm1::commit(s) == 0);
  uint64_t next_old = wisr::db_store(s->db, store).next_old->load();
  REQUIRE(next_old > 1);
  REQUIRE(sm1::closedb(s) == 0);

  home.reattach();
  s = opendb(home.env(), "db");
  REQUIRE(s);
  CHECK(next_old == wisr::db_store(s->db, store).next_old->load());
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK("page" == read_page(s, store, "k"));
  CHECK("last page" == read_page(s, store, std::string(8, '\xff')));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::chain_read(s, store, "k", data) == 0);
  CHECK("v3" == data);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  write_page(s, bare, "meta", "m");
  bool empty = false;
  REQUIRE(sm1::store_empty(s, bare, empty) == 0);
  CHECK(empty);
  REQUIRE(sm1::savepoint(s) == 1);
  sm1::loader_t *l = nullptr;
  REQUIRE_MESSAGE(sm1::load_open(s, bare, l) == 0, sm1::errmsg());
  size_t stream = 0;
  REQUIRE(sm1::load_stream(l, true, stream) == 0);
  REQUIRE(sm1::load_put(l, stream, "row", "r") == 0);
  REQUIRE(sm1::load_close(l) == 0);
  CHECK("r" == get(s, bare, "row"));
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK("<none>" == get(s, bare, "row"));
  CHECK("m" == read_page(s, bare, "meta"));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}
