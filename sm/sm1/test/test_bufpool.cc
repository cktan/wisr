/*
 * test_bufpool.cc -- the buffer pool alone (src/bufpool.h): tags,
 * bytes and pins, a miss reading through a loader of the test's, with
 * no session, store or RocksDB. test_page.cc tests pages on top of it
 * through sm1.h.
 */

#include "doctest.h"

#include "bufpool.h"

#include "sm1.h"
#include "sm_sys.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using wisr::bufpool_inner;
using wisr::bufpool_no_read;
using wisr::bufpool_page_t;
using wisr::bufpool_t;

namespace {

/* A store of pages, as the test's loader reads them. */
struct store_t {
  std::vector<std::string> pages;
  std::atomic<int> loads{0};
  std::atomic<int> sleep_ms{0};
  std::atomic<bool> fail{false};
};

/* A page's key: its number. */
std::string key_of(size_t n) { return "k" + std::to_string(n); }

/* The loader of store_t: a key's number's page, none if it is empty. */
int load_page(void *arg, char *buf, size_t &len) {
  auto *c = static_cast<std::pair<store_t *, size_t> *>(arg);
  store_t &st = *c->first;
  st.loads++;
  if (0 != st.sleep_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(st.sleep_ms.load()));
  }
  if (st.fail) {
    return sm1::seterr(sm1::SM_ERR_IO, "a failed read, for a test");
  }
  const std::string &p = st.pages[c->second];
  if (p.empty()) {
    return 1;
  }
  memcpy(buf, p.data(), p.size());
  len = p.size();
  return 0;
}

/* Pin page n of st in pool, shared or exclusive, with flags. */
int pin(bufpool_t &pool, store_t &st, size_t n, bool ex, unsigned flags,
        bufpool_t::frame_t *&f, std::string_view &bytes) {
  std::pair<store_t *, size_t> c(&st, n);
  std::string key = key_of(n);
  return pool.pin(bufpool_page_t{1, 2, key}, ex, flags, load_page, &c, f,
                  bytes);
}

/* Whether page n has a frame. */
bool cached(const bufpool_t &pool, size_t n) {
  std::string key = key_of(n);
  return pool.cached(bufpool_page_t{1, 2, key});
}

/* A store of n pages, each named by its number. */
void fill(store_t &st, size_t n) {
  st.pages.clear();
  for (size_t i = 0; i < n; i++) {
    st.pages.push_back("page" + std::to_string(i));
  }
}

/* Wait until done() is true, for at most ten seconds. */
template <typename F> bool eventually(F done) {
  for (int i = 0; i < 10000 && !done(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

} // namespace

TEST_CASE("bufpool: a miss reads once, a hit not at all") {
  // Verify a page is read into a frame once, and read from it after.
  // 1. Pin a page: a miss, one load, its bytes.
  // 2. Unpin and pin again: a hit, no load, the same bytes.
  // 3. A page there is none of: 1, no pin, and no frame left for it.
  // 4. A key past a tag's room is refused.
  bufpool_t pool(8);
  store_t st;
  fill(st, 4);
  st.pages[3].clear();
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  CHECK("page1" == b);
  CHECK(1 == st.loads);
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  CHECK("page1" == b);
  CHECK(1 == st.loads);
  pool.unpin(f);
  CHECK(1 == pin(pool, st, 3, false, 0, f, b));
  CHECK_FALSE(cached(pool, 3));
  std::string long_key(wisr::bufpool_key_max + 1, 'x');
  CHECK(-1 == pool.pin(bufpool_page_t{1, 2, long_key}, false, 0, load_page,
                       nullptr, f, b));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
}

TEST_CASE("bufpool: pages of two stores or databases are apart") {
  // Verify a tag names a page by its database, its store and its key.
  // 1. Pin one key in two stores and two databases: three loads.
  // 2. Each frame holds its own bytes.
  bufpool_t pool(8);
  store_t st;
  st.pages = {"a", "b", "c"};
  std::string key = "same";
  std::pair<store_t *, size_t> ca(&st, 0), cb(&st, 1), cc(&st, 2);
  bufpool_t::frame_t *fa = nullptr, *fb = nullptr, *fc = nullptr;
  std::string_view a, b, c;
  REQUIRE(0 == pool.pin({1, 2, key}, false, 0, load_page, &ca, fa, a));
  REQUIRE(0 == pool.pin({1, 3, key}, false, 0, load_page, &cb, fb, b));
  REQUIRE(0 == pool.pin({2, 2, key}, false, 0, load_page, &cc, fc, c));
  CHECK(3 == st.loads);
  CHECK("a" == a);
  CHECK("b" == b);
  CHECK("c" == c);
  pool.unpin(fa);
  pool.unpin(fb);
  pool.unpin(fc);
}

TEST_CASE("bufpool: shared pins share, an exclusive pin excludes") {
  // Verify a pin is a latch.
  // 1. Two shared pins of one page are held at once.
  // 2. An exclusive pin waits for both.
  // 3. While it holds, a shared pin waits.
  // 4. A thread holding a page shared may pin it shared again, though
  //    an exclusive pin waits.
  bufpool_t pool(8);
  store_t st;
  fill(st, 2);
  bufpool_t::frame_t *f1 = nullptr, *f2 = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, 0, f1, b));
  REQUIRE(0 == pin(pool, st, 0, false, 0, f2, b));
  CHECK(f1 == f2);

  std::atomic<int> stage{0};
  std::thread writer([&] {
    bufpool_t::frame_t *x = nullptr;
    std::string_view w;
    stage = 1;
    REQUIRE(0 == pin(pool, st, 0, true, 0, x, w));
    stage = 2;
    while (3 != stage) {
      std::this_thread::yield();
    }
    pool.unpin(x);
  });
  REQUIRE(eventually([&] { return 1 == stage; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(1 == stage);
  bufpool_t::frame_t *f3 = nullptr;
  REQUIRE(0 == pin(pool, st, 0, false, 0, f3, b));
  pool.unpin(f3);
  pool.unpin(f1);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(1 == stage);
  pool.unpin(f2);
  REQUIRE(eventually([&] { return 2 == stage; }));

  std::atomic<bool> read{false};
  std::thread reader([&] {
    bufpool_t::frame_t *r = nullptr;
    std::string_view v;
    REQUIRE(0 == pin(pool, st, 0, false, 0, r, v));
    read = true;
    pool.unpin(r);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_FALSE(read);
  stage = 3;
  writer.join();
  reader.join();
  CHECK(read);
}

TEST_CASE("bufpool: a pin taken after a sleep leaves no wait bit behind") {
  // Verify the wait bit lasts only while a thread sleeps on the frame:
  // a sleeper that wakes and takes its pin clears it, so the page's
  // next readers pin on the fast path, not under the wait queue's mutex.
  // 1. Pin a page exclusive.
  // 2. A reader pins it shared, and sleeps: the wait bit is set.
  // 3. Unpin; the reader takes its pin and holds it.
  // 4. While it holds, the wait bit is clear.
  bufpool_t pool(8);
  store_t st;
  fill(st, 1);
  std::string key = key_of(0);
  bufpool_page_t page{1, 2, key};
  bufpool_t::frame_t *x = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, true, 0, x, b));

  // 2.
  std::atomic<int> stage{0};
  std::thread reader([&] {
    bufpool_t::frame_t *r = nullptr;
    std::string_view v;
    REQUIRE(0 == pin(pool, st, 0, false, 0, r, v));
    stage = 1;
    while (2 != stage) {
      std::this_thread::yield();
    }
    pool.unpin(r);
  });
  REQUIRE(eventually([&] { return pool.waiting(page); }));

  // 3.
  pool.unpin(x);
  REQUIRE(eventually([&] { return 1 == stage; }));

  // 4.
  CHECK_FALSE(pool.waiting(page));
  stage = 2;
  reader.join();
}

TEST_CASE("bufpool: a page's pins wait out its miss, and it is read once") {
  // Verify a miss holds its frame exclusive through its read.
  // 1. Eight threads pin one page, its load slow.
  // 2. It is read once, and each sees its bytes.
  // 3. A writer's exclusive pin during a slow miss comes after it: it
  //    sees the page loaded, and installs over it.
  bufpool_t pool(8);
  store_t st;
  fill(st, 2);
  st.sleep_ms = 50;
  std::atomic<int> right{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&] {
      bufpool_t::frame_t *f = nullptr;
      std::string_view b;
      if (0 == pin(pool, st, 0, false, 0, f, b)) {
        right += "page0" == b ? 1 : 0;
        pool.unpin(f);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  CHECK(8 == right);
  CHECK(1 == st.loads);

  std::thread reader([&] {
    bufpool_t::frame_t *f = nullptr;
    std::string_view b;
    REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
    pool.unpin(f);
  });
  REQUIRE(eventually([&] { return 2 == st.loads; }));
  bufpool_t::frame_t *x = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 1, true, 0, x, b));
  CHECK("page1" == b);
  pool.install(x, "new");
  pool.unpin(x);
  reader.join();
  REQUIRE(0 == pin(pool, st, 1, false, 0, x, b));
  CHECK("new" == b);
  CHECK(2 == st.loads);
  pool.unpin(x);
}

TEST_CASE("bufpool: eviction leaves pinned pages, and pages come back") {
  // Verify eviction skips a pinned frame, and a page evicted reads again.
  // 1. In a pool of 4, pin 3 pages.
  // 2. Read 64 other pages through the one frame left.
  // 3. The 3 are still cached, with their bytes.
  // 4. An evicted page reads back from the store.
  bufpool_t pool(4);
  store_t st;
  fill(st, 70);
  bufpool_t::frame_t *held[3];
  std::string_view bytes[3];
  for (size_t i = 0; i < 3; i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, held[i], bytes[i]));
  }
  for (size_t i = 3; i < 67; i++) {
    bufpool_t::frame_t *f = nullptr;
    std::string_view b;
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    CHECK(st.pages[i] == b);
    pool.unpin(f);
  }
  for (size_t i = 0; i < 3; i++) {
    CHECK(cached(pool, i));
    CHECK(st.pages[i] == bytes[i]);
    pool.unpin(held[i]);
  }
  CHECK_FALSE(cached(pool, 10));
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 10, false, 0, f, b));
  CHECK("page10" == b);
  pool.unpin(f);
}

TEST_CASE("bufpool: an inner page outlasts a scan of leaves") {
  // Verify leaves age twice as fast as inner pages.
  // 1. In a pool of 8, pin an inner page once.
  // 2. Read as many leaves as the pool holds, each once, unpinned.
  // 3. The inner page is still there; the first leaf is not.
  // 4. A leaf pinned twice is no older than one pinned once: it goes
  //    in a scan too.
  bufpool_t pool(8);
  store_t st;
  fill(st, 40);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f, b));
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  pool.unpin(f);
  for (size_t i = 2; i < 9; i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
  }
  CHECK(cached(pool, 0));
  for (size_t i = 9; i < 12; i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
  }
  CHECK_FALSE(cached(pool, 1));
  CHECK(cached(pool, 0));
}

TEST_CASE("bufpool: the hand shifts an inner page's age by one, a leaf's by "
          "two") {
  // Verify a pin ORs 128 into a page's age, the hand halves an inner
  // page's age and quarters a leaf's, and takes a page at 0.
  // 1. In a pool of 2, pin inner page 0 and leaf 1: each is 128.
  // 2. Miss leaf 2: the hand alternates, shifting page 0 five times,
  //    to 4, and leaf 1 four times, to 0, taking its frame.
  // 3. Pin page 0 again: 4 | 128, 132.
  // 4. Miss leaf 3: page 0's lower bits shift out with the rest, to 4
  //    again, and leaf 2 is taken.
  // 5. Miss leaf 4: page 0 reaches 0 in three more turns and is taken,
  //    eight shifts after its last pin.
  bufpool_t pool(2);
  store_t st;
  fill(st, 8);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  auto age = [&](size_t n) {
    std::string key = key_of(n);
    return pool.age(bufpool_page_t{1, 2, key});
  };
  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f, b));
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  pool.unpin(f);
  CHECK(128 == age(0));
  CHECK(128 == age(1));

  REQUIRE(0 == pin(pool, st, 2, false, 0, f, b));
  pool.unpin(f);
  CHECK(4 == age(0));
  CHECK_FALSE(cached(pool, 1));
  CHECK(128 == age(2));

  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f, b));
  pool.unpin(f);
  CHECK(132 == age(0));

  REQUIRE(0 == pin(pool, st, 3, false, 0, f, b));
  pool.unpin(f);
  CHECK(4 == age(0));
  CHECK_FALSE(cached(pool, 2));

  REQUIRE(0 == pin(pool, st, 4, false, 0, f, b));
  pool.unpin(f);
  CHECK_FALSE(cached(pool, 0));
  CHECK(2 == age(3));
  CHECK(128 == age(4));
}

TEST_CASE("bufpool: a pin waits for a frame while every one is pinned") {
  // Verify a miss with no frame free waits for one.
  // 1. In a pool of 2, pin 2 pages.
  // 2. A third thread's pin of a third page waits.
  // 3. Unpin one: it goes on.
  bufpool_t pool(2);
  store_t st;
  fill(st, 3);
  bufpool_t::frame_t *f0 = nullptr, *f1 = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, 0, f0, b));
  REQUIRE(0 == pin(pool, st, 1, false, 0, f1, b));
  std::atomic<bool> done{false};
  std::thread third([&] {
    bufpool_t::frame_t *f = nullptr;
    std::string_view v;
    REQUIRE(0 == pin(pool, st, 2, false, 0, f, v));
    done = true;
    pool.unpin(f);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_FALSE(done);
  pool.unpin(f0);
  third.join();
  CHECK(done);
  pool.unpin(f1);
}

TEST_CASE("bufpool: what fails leaves a frame as it was") {
  // Verify a frame changes only by install.
  // 1. An exclusive pin given up with no install, as a failed write's
  //    is, leaves the page's old bytes, with no load.
  // 2. A failed read fails the pin, leaves no frame, and a later read
  //    succeeds.
  // 3. A new page pinned no_read and given up with no install is gone,
  //    and a pin that waited on it reads the store.
  // 4. A new page installed reads back, with no load.
  bufpool_t pool(8);
  store_t st;
  fill(st, 4);
  st.pages[2].clear();
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  CHECK("page0" == b);
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 0, false, 0, f, b));
  CHECK("page0" == b);
  CHECK(1 == st.loads);
  pool.unpin(f);

  st.fail = true;
  CHECK(-1 == pin(pool, st, 1, false, 0, f, b));
  CHECK(sm1::SM_ERR_IO == sm1::errnum());
  CHECK_FALSE(cached(pool, 1));
  st.fail = false;
  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  CHECK("page1" == b);
  pool.unpin(f);

  int before = st.loads;
  REQUIRE(0 == pin(pool, st, 2, true, bufpool_no_read, f, b));
  CHECK(b.empty());
  CHECK(cached(pool, 2));
  CHECK(before == st.loads);
  std::atomic<int> got{-2};
  std::thread reader([&] {
    bufpool_t::frame_t *r = nullptr;
    std::string_view v;
    got = pin(pool, st, 2, false, 0, r, v);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(-2 == got);
  pool.unpin(f);
  reader.join();
  CHECK(1 == got);
  CHECK_FALSE(cached(pool, 2));

  REQUIRE(0 == pin(pool, st, 2, true, bufpool_no_read, f, b));
  pool.install(f, "made");
  pool.unpin(f);
  before = st.loads;
  REQUIRE(0 == pin(pool, st, 2, false, 0, f, b));
  CHECK("made" == b);
  CHECK(before == st.loads);
  pool.unpin(f);
}

TEST_CASE("bufpool: a page's frame is discarded, and a prefix's purged") {
  // Verify frames leave for a page erased and for a prefix dropped.
  // 1. An exclusive pin discards its page: the frame goes.
  // 2. Purge a prefix: its pages go; another prefix's, another store's
  //    and another database's stay.
  // 3. A purge waits out a pin of a page it takes.
  bufpool_t pool(16);
  store_t st;
  fill(st, 8);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  pool.discard(f);
  pool.unpin(f);
  CHECK_FALSE(cached(pool, 0));

  std::pair<store_t *, size_t> c(&st, 1);
  for (std::string key : {"a1", "a2", "b1"}) {
    REQUIRE(0 == pool.pin({1, 2, key}, false, 0, load_page, &c, f, b));
    pool.unpin(f);
  }
  std::string a1 = "a1", b1 = "b1";
  REQUIRE(0 == pool.pin({1, 3, a1}, false, 0, load_page, &c, f, b));
  pool.unpin(f);
  REQUIRE(0 == pool.pin({9, 2, a1}, false, 0, load_page, &c, f, b));
  pool.unpin(f);
  pool.purge(1, 2, "a");
  CHECK_FALSE(pool.cached({1, 2, a1}));
  CHECK_FALSE(pool.cached({1, 2, std::string_view("a2")}));
  CHECK(pool.cached({1, 2, b1}));
  CHECK(pool.cached({1, 3, a1}));
  CHECK(pool.cached({9, 2, a1}));

  REQUIRE(0 == pool.pin({1, 2, b1}, false, 0, load_page, &c, f, b));
  std::atomic<bool> purged{false};
  std::thread dropper([&] {
    pool.purge(1, 2, "b");
    purged = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_FALSE(purged);
  pool.unpin(f);
  dropper.join();
  CHECK_FALSE(pool.cached({1, 2, b1}));
}

TEST_CASE("bufpool: a frame's version moves with each change of its page") {
  // Verify the version says, read with no pin, whether a frame still
  // holds its page as it was.
  // 1. A shared pin and its unpin, again, leave the version as it was.
  // 2. An exclusive pin that installs no bytes leaves it too.
  // 3. An install moves it.
  // 4. Eviction moves it: the frame's page goes for another.
  // 5. A discard and a purge move it.
  bufpool_t pool(2);
  store_t st;
  fill(st, 8);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, 0, f, b));
  bufpool_t::frame_t *f0 = f;
  uint64_t v = pool.version(f0);
  pool.unpin(f);
  REQUIRE(0 == pin(pool, st, 0, false, 0, f, b));
  CHECK(f0 == f);
  CHECK(v == pool.version(f0));
  pool.unpin(f);

  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  pool.unpin(f);
  CHECK(v == pool.version(f0));

  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  pool.install(f, "new bytes");
  pool.unpin(f);
  CHECK(v != pool.version(f0));
  v = pool.version(f0);

  for (size_t i = 1; i < 8 && cached(pool, 0); i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
  }
  REQUIRE_FALSE(cached(pool, 0));
  CHECK(v != pool.version(f0));

  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  v = pool.version(f);
  pool.discard(f);
  CHECK(v != pool.version(f));
  pool.unpin(f);

  REQUIRE(0 == pin(pool, st, 1, false, 0, f, b));
  v = pool.version(f);
  pool.unpin(f);
  pool.purge(1, 2, "k");
  CHECK(v != pool.version(f));
}

TEST_CASE("bufpool: a touch ages a frame as a pin does, with no pin") {
  // Verify a page touched, not pinned, is kept as a page pinned is.
  // 1. In a pool of 8, pin an inner page once, and unpin it.
  // 2. Read 64 leaves, each once, touching the inner page's frame
  //    after each: the page stays, its age's top bit set.
  // 3. Read 64 more with no touch: the page goes.
  bufpool_t pool(8);
  store_t st;
  fill(st, 140);
  bufpool_t::frame_t *f0 = nullptr;
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f0, b));
  pool.unpin(f0);
  for (size_t i = 1; i <= 64; i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
    pool.touch(f0);
  }
  CHECK(cached(pool, 0));
  std::string k0 = key_of(0);
  CHECK(128 <= pool.age(bufpool_page_t{1, 2, k0}));
  for (size_t i = 65; i <= 128; i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
  }
  CHECK_FALSE(cached(pool, 0));
}

TEST_CASE("bufpool: readers share one copy of a page at a version") {
  // Verify share() hands every reader of a page at one version one
  // copy, and lets a copy go once its page changes or leaves.
  // 1. Two readers of a page share: one copy, its bytes the page's.
  // 2. The page installed anew: the old copy's holder still reads
  //    the old bytes; once its holders let it go it is freed, as the
  //    frame no longer keeps it.
  // 3. A share is a new copy, of the new bytes.
  // 4. The page evicted: its copy, held by no reader, is freed.
  bufpool_t pool(4);
  store_t st;
  fill(st, 20);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f, b));
  std::shared_ptr<const wisr::bufpool_copy_t> c1 = pool.share(f, b);
  std::shared_ptr<const wisr::bufpool_copy_t> c2 = pool.share(f, b);
  CHECK(c1.get() == c2.get());
  CHECK("page0" == c1->bytes);
  pool.unpin(f);

  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  pool.install(f, "page0 again");
  pool.unpin(f);
  CHECK("page0" == c1->bytes);
  std::weak_ptr<const wisr::bufpool_copy_t> w1 = c1;
  c1.reset();
  c2.reset();
  CHECK(w1.expired());

  REQUIRE(0 == pin(pool, st, 0, false, bufpool_inner, f, b));
  std::shared_ptr<const wisr::bufpool_copy_t> c3 = pool.share(f, b);
  pool.unpin(f);
  CHECK("page0 again" == c3->bytes);

  std::weak_ptr<const wisr::bufpool_copy_t> w3 = c3;
  c3.reset();
  CHECK_FALSE(w3.expired());
  for (size_t i = 1; i < 20 && cached(pool, 0); i++) {
    REQUIRE(0 == pin(pool, st, i, false, 0, f, b));
    pool.unpin(f);
  }
  REQUIRE_FALSE(cached(pool, 0));
  CHECK(w3.expired());
}

TEST_CASE("bufpool: a pin's waits can be interrupted") {
  // Verify a wait on a frame and a wait for a frame end on an interrupt.
  // 1. Pin a page exclusive; a thread's pin of it waits.
  // 2. Interrupt that thread: the pin fails, SM_ERR_INTERRUPTED.
  // 3. In a pool of 1, its one frame pinned, another page's pin waits;
  //    interrupted, it fails the same way.
  bufpool_t pool(1);
  store_t st;
  fill(st, 2);
  bufpool_t::frame_t *f = nullptr;
  std::string_view b;
  REQUIRE(0 == pin(pool, st, 0, true, 0, f, b));
  for (size_t page : {size_t(0), size_t(1)}) {
    std::atomic<bool> started{false};
    std::atomic<int> rc{-2};
    std::atomic<int> err{0};
    sm_thread_ref_t waiter;
    std::thread t([&] {
      waiter = sm_thread_self();
      started = true;
      bufpool_t::frame_t *r = nullptr;
      std::string_view v;
      err = 0;
      int got = pin(pool, st, page, false, 0, r, v);
      err = sm1::errnum();
      rc = got;
      sm_take(1);
    });
    REQUIRE(eventually([&] { return started.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(-2 == rc);
    sm_interrupt(waiter, 1);
    t.join();
    CHECK(-1 == rc);
    CHECK(sm1::SM_ERR_INTERRUPTED == err);
  }
  pool.unpin(f);
}

TEST_CASE("bufpool: under writers and readers a pin always sees the store") {
  // Verify no frame is ever stale, torn or another page's, under
  // eviction, misses and writes at once.
  // 1. A store of 64 pages in a pool of 16: reads miss constantly.
  // 2. Two writers pin a random page exclusive, count it up in the
  //    store, and install it.
  // 3. Four readers pin pages shared: each page is its own, whole, and
  //    equal to the store's count, as no writer can run under a pin.
  bufpool_t pool(16);
  constexpr size_t pages = 64;
  std::vector<std::atomic<uint64_t>> counts(pages);
  auto bytes_of = [](size_t n, uint64_t c) {
    std::string s(2000, '\0');
    uint64_t words[2] = {n, c};
    memcpy(s.data(), words, sizeof(words));
    for (size_t i = sizeof(words); i < s.size(); i++) {
      s[i] = static_cast<char>(n + c + i);
    }
    return s;
  };
  struct ctx_t {
    std::vector<std::atomic<uint64_t>> *counts;
    decltype(bytes_of) *make;
    size_t n;
  };
  auto load = +[](void *arg, char *buf, size_t &len) {
    auto *c = static_cast<ctx_t *>(arg);
    std::string s = (*c->make)(c->n, (*c->counts)[c->n].load());
    memcpy(buf, s.data(), s.size());
    len = s.size();
    return 0;
  };
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::atomic<uint64_t> reads{0};
  std::vector<std::thread> threads;
  for (unsigned r = 0; r < 4; r++) {
    threads.emplace_back([&, r] {
      uint64_t x = 88172645463325252ULL + r;
      while (!stop) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        size_t n = x % pages;
        std::string key = key_of(n);
        ctx_t c{&counts, &bytes_of, n};
        bufpool_t::frame_t *f = nullptr;
        std::string_view b;
        if (0 != pool.pin({1, 2, key}, false, 0, load, &c, f, b)) {
          wrong++;
          continue;
        }
        if (b != bytes_of(n, counts[n].load())) {
          wrong++;
        }
        pool.unpin(f);
        reads++;
      }
    });
  }
  for (unsigned w = 0; w < 2; w++) {
    threads.emplace_back([&, w] {
      uint64_t x = 2685821657736338717ULL + w;
      for (int i = 0; i < 20000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        size_t n = x % pages;
        std::string key = key_of(n);
        ctx_t c{&counts, &bytes_of, n};
        bufpool_t::frame_t *f = nullptr;
        std::string_view b;
        if (0 != pool.pin({1, 2, key}, true, 0, load, &c, f, b)) {
          wrong++;
          continue;
        }
        if (b != bytes_of(n, counts[n].load())) {
          wrong++;
        }
        uint64_t next = counts[n].load() + 1;
        counts[n].store(next);
        pool.install(f, bytes_of(n, next));
        pool.unpin(f);
      }
    });
  }
  threads[4].join();
  threads[5].join();
  stop = true;
  for (size_t i = 0; i < 4; i++) {
    threads[i].join();
  }
  CHECK(0 == wrong);
  CHECK(reads > 0);
}
