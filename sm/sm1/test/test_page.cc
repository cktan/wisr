/*
 * test_page.cc -- pages (../BTREE.md §2): keys of class 0x04, written
 * outside every transaction and read through the buffer pool, apart
 * from every version of the store.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <rocksdb/advanced_cache.h>

namespace {

/* A page to write, by write_pages. */
struct put_t {
  std::string_view key;
  std::string_view bytes;
  bool erase = false;
  unsigned flags = 0;
};

/* Pin pages exclusive, write them at once, and unpin them. */
void write_pages(sm1::session_t *s, uint64_t store,
                 const std::vector<put_t> &pages) {
  std::vector<sm1::page_put_t> puts;
  for (const put_t &p : pages) {
    sm1::page_t *pin = nullptr;
    std::string_view old;
    int r = sm1::pin_page(s, store, p.key, sm1::page_mode_t::ex,
                          p.flags | sm1::page_no_read, pin, old);
    REQUIRE_MESSAGE(r == 0, sm1::errmsg());
    puts.push_back({pin, p.key, p.bytes, p.erase});
  }
  REQUIRE_MESSAGE(sm1::page_write(s, store, puts) == 0, sm1::errmsg());
  for (const sm1::page_put_t &p : puts) {
    sm1::unpin_page(s, p.page);
  }
}

/* Write one page, failing the test on an error. */
void write_page(sm1::session_t *s, uint64_t store, std::string_view key,
                std::string_view bytes) {
  write_pages(s, store, {{key, bytes}});
}

/* The page at key as s reads it, or "<none>"; fails on an error. */
std::string read_page(sm1::session_t *s, uint64_t store, std::string_view key) {
  sm1::page_t *page = nullptr;
  std::string_view bytes;
  int r = sm1::pin_page(s, store, key, sm1::page_mode_t::sh, 0, page, bytes);
  REQUIRE_MESSAGE(r >= 0, sm1::errmsg());
  if (r == 1) {
    return "<none>";
  }
  std::string out(bytes);
  sm1::unpin_page(s, page);
  return out;
}

} // namespace

TEST_CASE("page: written outside the transaction, read through the pool") {
  // Verify a page's write commits when it is made, and is read back.
  // 1. A page never written reads as none.
  // 2. Write two pages in one call, in a transaction, and abort it:
  //    both are still there, from this session and from another.
  // 3. Rewrite one, erase the other in one call: the reads follow.
  // 4. Reattached, the store holds the page as last written.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK("<none>" == read_page(s, store, "a"));
  write_pages(s, store, {{"a", "one"}, {"b", "two"}});
  REQUIRE(sm1::abort(s) == 0);

  sm1::session_t *t = opendb(home.env(), "db");
  REQUIRE(t);
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK("one" == read_page(t, store, "a"));
  CHECK("two" == read_page(t, store, "b"));

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  write_pages(s, store, {{"a", "uno"}, {"b", {}, true}});
  CHECK("uno" == read_page(t, store, "a"));
  CHECK("<none>" == read_page(t, store, "b"));
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

TEST_CASE("page: a pin of one page shuts out a writer of it") {
  // Verify a pin is a latch, through sm1.h.
  // 1. A reader pins a page shared.
  // 2. A writer's exclusive pin of it waits until the reader unpins.
  // 3. A page's key past a frame's tag, and bytes past a frame, fail.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  write_page(s, store, "a", "one");
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(sm1::pin_page(s, store, "a", sm1::page_mode_t::sh, 0, pin, bytes) ==
          0);
  std::atomic<bool> pinned{false};
  std::thread writer([&] {
    sm1::session_t *w = opendb(home.env(), "db");
    sm1::begin(w, sm1::xctlevel_t::read_committed);
    sm1::page_t *p = nullptr;
    std::string_view b;
    if (sm1::pin_page(w, store, "a", sm1::page_mode_t::ex, 0, p, b) == 0) {
      pinned = true;
      sm1::unpin_page(w, p);
    }
    sm1::commit(w);
    sm1::closedb(w);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(pinned);
  sm1::unpin_page(s, pin);
  writer.join();
  CHECK(pinned);

  std::string long_key(sm1::page_key_max + 1, 'k');
  CHECK(sm1::pin_page(s, store, long_key, sm1::page_mode_t::sh, 0, pin,
                      bytes) == -1);
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  REQUIRE(sm1::pin_page(s, store, "a", sm1::page_mode_t::ex, 0, pin, bytes) ==
          0);
  std::string big(sm1::page_size + 1, 'b');
  std::vector<sm1::page_put_t> puts{{pin, "a", big}};
  CHECK(sm1::page_write(s, store, puts) == -1);
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  sm1::unpin_page(s, pin);
  CHECK("one" == read_page(s, store, "a"));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: a read that misses never puts back a page replaced") {
  // Verify a miss holds its frame through its read, so a write cannot
  // land between, and no frame holds a page older than RocksDB's.
  // 1. Attach so the pages far outnumber the frames: reads miss.
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

  constexpr int pages = 256;
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
        if (sm1::pin_page(s, store, key, sm1::page_mode_t::sh, 0, page, b) ==
            0) {
          sm1::unpin_page(s, page);
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
    REQUIRE(sm1::pin_page(w, store, key, sm1::page_mode_t::sh, 0, page, b) ==
            0);
    if (count_of(b) != counts[p]) {
      stale++;
    }
    sm1::unpin_page(w, page);
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

TEST_CASE("page: dropped by prefix, with their frames") {
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

TEST_CASE("page: a session's copy is good until its page changes") {
  // Verify a copy of a page reads as it was until the page is changed
  // or dropped, and is the session's own.
  // 1. Pin a page, keep a copy, unpin: the copy reads as the page.
  // 2. A read of the page by another session leaves the copy good.
  // 3. The page written again: the copy is no longer good; a new
  //    copy is.
  // 4. Another key, or another store, has no copy.
  // 5. The page dropped: the copy is no longer good.
  // 6. One copy past the most a session keeps drops the others.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  sm1::session_t *s2 = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(s2);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(s2, sm1::xctlevel_t::read_committed) == 0);
  write_page(s, store, "r/0", "one");
  auto keep = [&](std::string_view key) {
    sm1::page_t *page = nullptr;
    std::string_view bytes;
    REQUIRE(0 ==
            sm1::pin_page(s, store, key, sm1::page_mode_t::sh, 0, page, bytes));
    sm1::keep_page_copy(s, store, key, page, bytes);
    sm1::unpin_page(s, page);
  };
  auto copy = [&](uint64_t st, std::string_view key) {
    std::string_view bytes;
    return sm1::page_copy(s, st, key, bytes) ? std::string(bytes)
                                             : std::string("<none>");
  };
  keep("r/0");
  CHECK("one" == copy(store, "r/0"));

  CHECK("one" == read_page(s2, store, "r/0"));
  CHECK("one" == copy(store, "r/0"));

  write_page(s2, store, "r/0", "two");
  CHECK("<none>" == copy(store, "r/0"));
  keep("r/0");
  CHECK("two" == copy(store, "r/0"));

  CHECK("<none>" == copy(store, "r/1"));
  CHECK("<none>" == copy(store + 1, "r/0"));

  REQUIRE(0 == sm1::page_drop(s2, store, "r"));
  CHECK("<none>" == copy(store, "r/0"));

  for (size_t i = 0; i <= sm1::page_copies_max; i++) {
    std::string key = "c/" + std::to_string(i);
    write_page(s, store, key, key);
    keep(key);
  }
  CHECK("<none>" == copy(store, "c/0"));
  std::string last = "c/" + std::to_string(sm1::page_copies_max);
  CHECK(last == copy(store, last));
  REQUIRE(sm1::commit(s2) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s2) == 0);
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

TEST_CASE("page: the bytes of a prefix's pages") {
  // Verify sm1::page_approxsize counts a prefix's pages alone.
  // 1. Write 200 pages of 4 KB under "a", one under "b".
  // 2. Before a flush they are in memtables: "a"'s more than "b"'s,
  //    "c"'s none.
  // 3. Compacted, they are in files: "a"'s over 400 KB, "c"'s none.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  // Bytes that do not compress, as a page's keys mostly do not.
  std::string bytes(4096, '\0');
  uint64_t x = 88172645463325252ULL;
  auto fill = [&] {
    for (char &c : bytes) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c = static_cast<char>(x);
    }
  };
  for (int i = 0; i < 200; i++) {
    fill();
    write_page(s, store, "a" + std::to_string(1000 + i), bytes);
  }
  write_page(s, store, "b", bytes);
  uint64_t ad = 0, am = 0, bd = 0, bm = 0, cd = 0, cm = 0;
  REQUIRE(sm1::page_approxsize(s, store, "a", ad, am) == 0);
  REQUIRE(sm1::page_approxsize(s, store, "b", bd, bm) == 0);
  REQUIRE(sm1::page_approxsize(s, store, "c", cd, cm) == 0);
  CHECK(am > bm);
  CHECK(0u == cd + cm);
  REQUIRE(sm1::compact(s, store) == 0);
  REQUIRE(sm1::page_approxsize(s, store, "a", ad, am) == 0);
  REQUIRE(sm1::page_approxsize(s, store, "c", cd, cm) == 0);
  CHECK(ad > 400u * 1000);
  CHECK(0u == cd + cm);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: a page not inner is evicted before an inner page") {
  // Verify every page written is in the pool, and which outlast which.
  // 1. Attach with the fewest frames a pool has.
  // 2. Write a page "once" and an inner page.
  // 3. Write as many other pages as there are frames, and two more.
  // 4. "once" is gone; the inner page stayed, and the last page
  //    written, not inner, is in the pool.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  sm1::env_options_t opt;
  opt.block_cache_bytes = 256 << 10;
  sm1::env_t *env = nullptr;
  REQUIRE_MESSAGE(sm1::attach(tmp.path(), env, &opt) == 0, sm1::errmsg());
  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *s = opendb(env, "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  write_page(s, store, "once", "a");
  write_pages(s, store, {{"inner", "c", false, sm1::page_inner}});
  const std::string pad(4000, 'p');
  int others = static_cast<int>(wisr::bufpool_min_frames);
  for (int i = 0; i < others; i++) {
    write_page(s, store, "k" + std::to_string(i), pad);
  }
  CHECK_FALSE(sm1::page_cached(s, store, "once"));
  CHECK(sm1::page_cached(s, store, "inner"));
  CHECK(sm1::page_cached(s, store, "k" + std::to_string(others - 1)));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
  REQUIRE(sm1::detach(env, 1) == 0);
}

TEST_CASE("page: dropped outside a transaction, after a commit") {
  // Verify sm1::page_drop with no transaction active, as an
  // on_commit function makes it.
  // 1. Write pages under "t"; commit with a function that drops them.
  // 2. After the commit they are gone, read in a new transaction.
  // 3. A store dropped is no error, outside a transaction.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  write_page(s, store, "t1", "a");
  write_page(s, store, "t2", "b");
  write_page(s, store, "u1", "c");
  int rc = -2;
  REQUIRE(sm1::on_commit(s, [&] { rc = sm1::page_drop(s, store, "t"); }) == 0);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(0 == rc);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK("<none>" == read_page(s, store, "t1"));
  CHECK("<none>" == read_page(s, store, "t2"));
  CHECK("c" == read_page(s, store, "u1"));
  REQUIRE(sm1::rmstore(s, store) == 0);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(0 == sm1::page_drop(s, store, "u"));
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: a store's checks made once, for many pins") {
  // Verify page_enter checks a call on a store, and a pin by its handle
  // makes none of its own.
  // 1. With no transaction, store 0, and a store never made, it fails
  //    as a pin does.
  // 2. Its handle pins pages, of either mode, as a pin by the store's
  //    id does, and a page there is none of is 1.
  // 3. A store dropped in the transaction is gone to page_enter, and
  //    stays good to a handle taken before, until the commit.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  const sm1::page_store_t *h = nullptr;
  CHECK(sm1::page_enter(s, 1, h) == -1);
  CHECK(sm1::SM_ERR_NOTRANS == sm1::errnum());
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::page_enter(s, 0, h) == -1);
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  CHECK(sm1::page_enter(s, store + 1000, h) == -1);
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());

  write_page(s, store, "a", "one");
  REQUIRE(sm1::page_enter(s, store, h) == 0);
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(sm1::pin_page(s, store, h, "a", sm1::page_mode_t::sh, 0, pin,
                        bytes) == 0);
  CHECK("one" == bytes);
  sm1::unpin_page(s, pin);
  REQUIRE(sm1::pin_page(s, store, h, "a", sm1::page_mode_t::ex, 0, pin,
                        bytes) == 0);
  sm1::unpin_page(s, pin);
  CHECK(sm1::pin_page(s, store, h, "none", sm1::page_mode_t::sh, 0, pin,
                      bytes) == 1);

  REQUIRE(sm1::rmstore(s, store) == 0);
  const sm1::page_store_t *gone = nullptr;
  CHECK(sm1::page_enter(s, store, gone) == -1);
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  REQUIRE(sm1::pin_page(s, store, h, "a", sm1::page_mode_t::sh, 0, pin,
                        bytes) == 0);
  CHECK("one" == bytes);
  sm1::unpin_page(s, pin);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("page: a miss leaves no second copy in the block cache") {
  // Verify a page read into a frame is not cached by RocksDB as well.
  // 1. Write 200 pages that do not compress, into a sorted file.
  // 2. Reattached, read one, so RocksDB's index blocks are loaded.
  // 3. Read the rest: the block cache has grown by no page's worth of
  //    its blocks, though they were all read from it.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::string bytes(4000, '\0');
  uint64_t x = 88172645463325252ULL;
  auto key_of = [](int i) { return "p" + std::to_string(1000 + i); };
  for (int i = 0; i < 200; i++) {
    for (char &c : bytes) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c = static_cast<char>(x);
    }
    write_page(s, store, key_of(i), bytes);
  }
  REQUIRE(sm1::compact(s, store) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);

  home.reattach();
  s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(4000u == read_page(s, store, key_of(0)).size());
  size_t before = home.env()->block_cache->GetUsage();
  for (int i = 1; i < 60; i++) {
    CHECK(4000u == read_page(s, store, key_of(i)).size());
  }
  CHECK(home.env()->block_cache->GetUsage() < before + 4000);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}
