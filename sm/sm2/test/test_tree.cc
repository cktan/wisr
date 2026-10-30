/*
 * test_tree.cc -- a B+tree over sm1's pages (../BTREE.md §13 step 5):
 * inserts against a sorted map, cursors over the whole and ranges,
 * a split's post cut short and finished later, and writers on many
 * threads with cursors running.
 */

#include "doctest.h"

#include "db.h"

#include "../src/tree/tree.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace sm2::tree;

namespace {

/* A key of random bytes, 1 to max_len long. */
std::string random_key(std::mt19937_64 &rng, size_t max_len) {
  std::uniform_int_distribution<size_t> len(1, max_len);
  std::uniform_int_distribution<int> byte(0, 255);
  std::string key(len(rng), '\0');
  for (char &c : key) {
    c = static_cast<char>(byte(rng));
  }
  return key;
}

/* Every key and value of a cursor, in order; REQUIREs no error. */
std::vector<std::pair<std::string, std::string>> drain(cursor_t &c) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string_view k, v;
  int rc;
  while (0 == (rc = cursor_next(c, k, v))) {
    out.emplace_back(std::string(k), std::string(v));
  }
  REQUIRE_MESSAGE(1 == rc, sm1::errmsg());
  return out;
}

/* A session in a transaction on d, its tree of relid opened. */
struct open_tree_t {
  sm1::session_t *s;
  tree_t tree;

  open_tree_t(db_t &d, uint16_t relid, size_t limit) : s(d.open()) {
    REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
    tree_open(tree, s, d.store(), relid, limit);
  }
  ~open_tree_t() {
    sm1::commit(s);
    sm1::closedb(s);
  }
};

} // namespace

TEST_CASE("tree: inserts at random, read back in order") {
  // Verify the tree against a sorted map, through many splits.
  // 1. Make a tree with a small page limit, so it grows tall.
  // 2. Insert 20,000 random keys: a key twice is 1, unchanged.
  // 3. The tree checks whole; it is three levels or more.
  // 4. A cursor over the whole gives the map; ranges, inclusive and
  //    not, give the map's ranges.
  // 5. After a restart, another session reads the same.
  db_t d;
  std::map<std::string, std::string> expected;
  {
    open_tree_t t(d, 7, 512);
    REQUIRE(0 == tree_create(t.tree));
    std::mt19937_64 rng(5);
    for (int i = 0; i < 20000; i++) {
      std::string key = random_key(rng, 24);
      std::string value = std::to_string(i);
      int rc = tree_insert(t.tree, key, value);
      REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
      if (expected.count(key) > 0) {
        CHECK(1 == rc);
      } else {
        CHECK(0 == rc);
        expected[key] = value;
      }
    }
    REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
    size_t height = 0;
    REQUIRE(0 == tree_height(t.tree, height));
    CHECK(height >= 3);
  }
  d.restart();
  open_tree_t t(d, 7, 512);
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  auto all = drain(c);
  REQUIRE(expected.size() == all.size());
  CHECK(std::equal(all.begin(), all.end(), expected.begin(),
                   [](const auto &a, const auto &b) {
                     return a.first == b.first && a.second == b.second;
                   }));
  std::mt19937_64 rng(6);
  for (int i = 0; i < 200; i++) {
    std::string lo = random_key(rng, 3);
    std::string hi = lo + random_key(rng, 2);
    bool lo_in = 0 == i % 2;
    bool hi_in = 0 == i % 3;
    cursor_open(t.tree, c, &lo, lo_in, &hi, hi_in);
    auto got = drain(c);
    std::vector<std::pair<std::string, std::string>> want;
    for (auto it = lo_in ? expected.lower_bound(lo) : expected.upper_bound(lo);
         it != expected.end(); ++it) {
      if (hi_in ? it->first > hi : it->first >= hi) {
        break;
      }
      want.push_back(*it);
    }
    CHECK(want == got);
  }
}

TEST_CASE("tree: a cursor moves on past splits made under it") {
  // Verify a cursor holds no page, and misses no key a split moves.
  // 1. Fill a tree with even numbers.
  // 2. Read half of them by a cursor; insert every odd number.
  // 3. The cursor reads on: the rest of the leaf it copied, as it
  //    was, then every key past it, even and odd, once, in order.
  //    A seek moves it forward.
  db_t d;
  open_tree_t t(d, 3, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) {
    char buf[16];
    snprintf(buf, sizeof buf, "%08d", n);
    return std::string(buf);
  };
  for (int n = 0; n < 4000; n += 2) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "e"));
  }
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  std::string_view k, v;
  for (int n = 0; n < 2000; n += 2) {
    REQUIRE(0 == cursor_next(c, k, v));
    CHECK(key_of(n) == k);
  }
  REQUIRE(c.end > c.at);
  int copied = std::stoi(std::string(c.view.key(c.end - 1)));
  for (int n = 1; n < 4000; n += 2) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "o"));
  }
  for (int n = 2000; n <= copied; n += 2) {
    REQUIRE(0 == cursor_next(c, k, v));
    CHECK(key_of(n) == k);
  }
  for (int n = copied + 1; n < 3000; n++) {
    REQUIRE(0 == cursor_next(c, k, v));
    CHECK(key_of(n) == k);
  }
  cursor_seek(c, key_of(3500), false);
  REQUIRE(0 == cursor_next(c, k, v));
  CHECK(key_of(3501) == k);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a split's post cut short is finished by the next insert") {
  // Verify a crash between a split's two writes leaves a tree readers
  // walk whole, and the next writer to latch the page finishes it.
  // 1. Fill a tree until its root leaf splits; then fill the left
  //    leaf, with keys below the first, until it splits, its post
  //    made to fail, as a crash would cut it short.
  // 2. The tree does not check (the left leaf is flagged
  //    incomplete), but a cursor reads every key, the new one too,
  //    by the right link.
  // 3. Restart; insert a key below every other, into the flagged
  //    leaf: the tree checks whole again, and every key is there.
  db_t d;
  std::set<std::string> keys;
  {
    open_tree_t t(d, 9, 512);
    REQUIRE(0 == tree_create(t.tree));
    size_t height = 1;
    for (int n = 0; 1 == height; n++) {
      std::string key = "k" + std::to_string(1000 + n);
      REQUIRE(0 == tree_insert(t.tree, key, "v"));
      keys.insert(key);
      REQUIRE(0 == tree_height(t.tree, height));
    }
    detail::fail_posts = 1;
    int rc = 0;
    for (int n = 0; 0 == rc; n++) {
      std::string key = "k0" + std::to_string(1000 + n);
      rc = tree_insert(t.tree, key, "v");
      keys.insert(key);
    }
    CHECK(-1 == rc);
    CHECK(0 == detail::fail_posts);
    CHECK(-1 == tree_check(t.tree));
    cursor_t c;
    cursor_open(t.tree, c, nullptr, true, nullptr, true);
    auto all = drain(c);
    REQUIRE(keys.size() == all.size());
  }
  d.restart();
  open_tree_t t(d, 9, 512);
  REQUIRE(0 == tree_insert(t.tree, "a", "v"));
  keys.insert("a");
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  auto all = drain(c);
  REQUIRE(keys.size() == all.size());
  auto it = keys.begin();
  for (const auto &e : all) {
    CHECK(*it++ == e.first);
  }
}

TEST_CASE("tree: entries too long for an inner page are refused") {
  // Verify a key's limit counts it as an inner entry, with a page id.
  // 1. A key whose leaf entry, with a page id's 8 bytes as its value,
  //    is just entry_max is taken; one byte longer is refused.
  // 2. Many of the longest keys split and post whole.
  db_t d;
  open_tree_t t(d, 4, 512);
  REQUIRE(0 == tree_create(t.tree));
  size_t key_len = entry_max(512) - entry_size(0, 8);
  REQUIRE(0 == tree_insert(t.tree, std::string(key_len, 'a'), ""));
  CHECK(-1 == tree_insert(t.tree, std::string(key_len + 1, 'b'), ""));
  CHECK(sm1::SM_ERR_KEYTOOLONG == sm1::errnum());
  for (int i = 0; i < 200; i++) {
    std::string key(key_len, 'c');
    key.replace(0, 4, std::to_string(1000 + i));
    REQUIRE_MESSAGE(0 == tree_insert(t.tree, key, ""), sm1::errmsg());
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: writers on many threads, cursors running") {
  // Verify concurrent inserts lose nothing and readers see order.
  // 1. Four writers insert 3,000 keys each, of their own, at random,
  //    into one tree with a small limit, so pages split under them.
  // 2. Two readers scan the whole tree meanwhile: each scan's keys
  //    are in order, no key twice.
  // 3. At the end the tree checks whole and holds every key.
  db_t d;
  {
    open_tree_t t(d, 11, 512);
    REQUIRE(0 == tree_create(t.tree));
  }
  std::atomic<bool> stop{false};
  std::atomic<int> disorder{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 4; w++) {
    threads.emplace_back([&, w] {
      open_tree_t t(d, 11, 512);
      std::mt19937_64 rng(100 + w);
      for (int i = 0; i < 3000; i++) {
        std::string key = std::to_string(rng() % 1000000) + "/" +
                          std::to_string(w) + "/" + std::to_string(i);
        if (0 != tree_insert(t.tree, key, "v")) {
          errors++;
        }
      }
    });
  }
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    readers.emplace_back([&] {
      open_tree_t t(d, 11, 512);
      while (!stop) {
        cursor_t c;
        cursor_open(t.tree, c, nullptr, true, nullptr, true);
        std::string last;
        bool first = true;
        std::string_view k, v;
        int rc;
        while (0 == (rc = cursor_next(c, k, v))) {
          if (!first && !(last < k)) {
            disorder++;
          }
          last = k;
          first = false;
        }
        if (rc < 0) {
          errors++;
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  stop = true;
  for (auto &th : readers) {
    th.join();
  }
  CHECK(0 == errors.load());
  CHECK(0 == disorder.load());
  open_tree_t t(d, 11, 512);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(4u * 3000 == drain(c).size());
}

TEST_CASE("tree: made again under a relid, it starts empty") {
  // Verify tree_create drops what an earlier tree under its relid left,
  // in the store and in the cache, and tree_drop removes a tree.
  // 1. Fill a tree and read it, so its pages are cached.
  // 2. Make it again: a cursor reads nothing, and it checks whole.
  // 3. Drop it: its meta page is gone.
  db_t d;
  open_tree_t t(d, 5, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int i = 0; i < 500; i++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(i), "v"));
  }
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(500u == drain(c).size());
  REQUIRE(0 == tree_create(t.tree));
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(drain(c).empty());
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  REQUIRE(0 == tree_drop(t.s, d.store(), 5));
  size_t height = 0;
  CHECK(-1 == tree_height(t.tree, height));
  CHECK(sm1::SM_ERR_CORRUPT == sm1::errnum());
}

TEST_CASE("tree: prune removes the entries it is told to, leaf by leaf") {
  // Verify tree_prune walks every leaf and writes what it removes.
  // 1. Fill a tree of several leaves with 3,000 keys.
  // 2. Prune every key whose number is odd: 1,500 removed.
  // 3. The tree checks whole and holds the even keys alone; a prune
  //    that fails stops with its error.
  db_t d;
  open_tree_t t(d, 13, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 3000; n++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(100000 + n), "v"));
  }
  size_t removed = 0;
  REQUIRE(0 == tree_prune(
                   t.tree,
                   [](std::string_view key, std::string_view) {
                     return (key.back() - '0') % 2;
                   },
                   removed));
  CHECK(1500u == removed);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  auto all = drain(c);
  REQUIRE(1500u == all.size());
  for (size_t i = 0; i < all.size(); i++) {
    CHECK(std::to_string(100000 + 2 * i) == all[i].first);
  }
  CHECK(-1 == tree_prune(
                  t.tree,
                  [](std::string_view, std::string_view) {
                    return sm1::seterr(sm1::SM_ERR_IO, "stop");
                  },
                  removed));
  CHECK(sm1::SM_ERR_IO == sm1::errnum());
}
