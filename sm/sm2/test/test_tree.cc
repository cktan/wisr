/*
 * test_tree.cc -- a B+tree over sm1's pages (../BTREE.md §13 step 5):
 * inserts against a sorted map, cursors over the whole and ranges,
 * a split's post cut short and finished later, and writers on many
 * threads with cursors running.
 */

#include "doctest.h"

#include "db.h"

#include "../src/tree/tree.h"

#include "sm0_codec.h"

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
    tree_close(tree);
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
  // The batch's last key, the copy's last entry: 8 digits, no value.
  int copied = std::stoi(c.bytes.substr(c.bytes.size() - 2 - 1 - 8, 8));
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

TEST_CASE("tree: inner pages held stale still route every probe") {
  // Verify a tree's held inner pages, gone stale, lead to every key.
  // 1. A reader's tree reads every key once, so it holds the inner
  //    pages as they were.
  // 2. A writer's tree inserts 20,000 keys more, splitting the inner
  //    pages the reader holds and the root above them.
  // 3. The reader, its pages unchanged, finds each key by a probe of
  //    its own; closed and opened again, it holds pages anew.
  db_t d;
  open_tree_t r(d, 15, 512);
  REQUIRE(0 == tree_create(r.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n * 7 % 30011); };
  for (int n = 0; n < 2000; n++) {
    REQUIRE(0 == tree_insert(r.tree, key_of(n), "v"));
  }
  for (int n = 0; n < 2000; n++) {
    cursor_t c;
    std::string k = key_of(n);
    cursor_open(r.tree, c, &k, true, &k, true);
    REQUIRE(1u == drain(c).size());
  }
  CHECK_FALSE(r.tree.held.empty());
  {
    open_tree_t w(d, 15, 512);
    for (int n = 2000; n < 22000; n++) {
      REQUIRE(0 == tree_insert(w.tree, key_of(n), "v"));
    }
  }
  for (int n = 0; n < 22000; n += 3) {
    cursor_t c;
    std::string k = key_of(n);
    cursor_open(r.tree, c, &k, true, &k, true);
    CHECK(1u == drain(c).size());
  }
  tree_close(r.tree);
  CHECK(r.tree.held.empty());
  REQUIRE_MESSAGE(0 == tree_check(r.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a leaf a probe reads is evicted before the inner pages") {
  // Verify a descent's miss of a leaf caches it as a leaf, not inner.
  // 1. Attach with a cache of 256 KB, and make a tree over many leaves.
  // 2. Write other pages, more than the cache holds, so its leaves go.
  // 3. A tree opened anew probes the first key, missing its leaf.
  // 4. Write as many other pages again.
  // 5. The leaf is gone; the root and the meta page stayed.
  sm1::env_options_t opt;
  opt.block_cache_bytes = 256 << 10;
  db_t d(&opt);
  open_tree_t t(d, 21, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  for (int n = 0; n < 500; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
  }
  const std::string pad(4000, 'p');
  auto write_others = [&](int from) {
    for (int n = from; n < from + 128; n++) {
      std::string k = "other" + std::to_string(n);
      std::vector<sm1::page_put_t> puts{{k, pad, false, false}};
      REQUIRE(0 == sm1::page_write(t.s, d.store(), puts));
    }
  };
  write_others(0);
  tree_t probe;
  tree_open(probe, t.s, d.store(), 21, 512);
  cursor_t c;
  std::string k = key_of(0);
  cursor_open(probe, c, &k, true, &k, true);
  REQUIRE(1u == drain(c).size());
  uint64_t root = probe.root;
  REQUIRE(1 != root);
  // Let the pinned inner pages go, so only their priority keeps them.
  tree_close(probe);
  write_others(1000);
  auto cached = [&](uint64_t pageid) {
    std::string name{static_cast<char>(0), static_cast<char>(21)};
    sm0::put_be64(name, pageid);
    return sm1::page_cached(t.s, d.store(), name);
  };
  CHECK_FALSE(cached(1));
  CHECK(cached(root));
  CHECK(cached(0));
}

TEST_CASE("tree: a unique insert judges every entry of its prefix") {
  // Verify tree_insert_unique judges each entry of the prefix, on
  // the leaf that covers it and the ones to its right.
  // 1. Insert 300 entries of prefix "k|", dead to the check, then one
  //    whose row is live: with pages of 512 bytes they span leaves.
  // 2. A key of the prefix is refused, as the live one is judged a
  //    duplicate; with none live, it goes in, whatever leaf it lands
  //    on; inserted again it is there already.
  // 3. A check that answers wait gets 2, and no change is made.
  db_t d;
  open_tree_t t(d, 17, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) {
    char buf[32];
    snprintf(buf, sizeof buf, "k|%06d", n);
    return std::string(buf);
  };
  for (int n = 0; n < 300; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n * 2), "v"));
  }
  REQUIRE(0 == tree_insert(t.tree, key_of(1001), "live"));
  REQUIRE(0 == tree_insert(t.tree, "a", "v"));
  REQUIRE(0 == tree_insert(t.tree, "z", "v"));
  size_t judged = 0;
  auto dup_if_live = [&](std::string_view, std::string_view v) {
    judged++;
    return v == "live" ? sm1::seterr(sm1::SM_ERR_DUPKEY, "dup"), 1 : 0;
  };
  CHECK(-1 == tree_insert_unique(t.tree, "k|", key_of(3), "v", dup_if_live));
  CHECK(sm1::SM_ERR_DUPKEY == sm1::errnum());
  CHECK(judged >= 300);
  auto none = [](std::string_view, std::string_view) { return 0; };
  for (int n : {3, 599, 1500}) {
    CHECK(0 == tree_insert_unique(t.tree, "k|", key_of(n), "v", none));
    CHECK(1 == tree_insert_unique(t.tree, "k|", key_of(n), "v", none));
  }
  auto wait = [](std::string_view, std::string_view) { return 2; };
  CHECK(2 == tree_insert_unique(t.tree, "k|", key_of(5), "v", wait));
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  std::string lo = "k|", hi = "k}";
  cursor_open(t.tree, c, &lo, true, &hi, false);
  CHECK(304u == drain(c).size());
}

TEST_CASE("tree: a root's split cut short is finished by its right page's") {
  // Verify a crash between a root's split's two writes, after which
  // the meta page still names the old root, does not stop a split of
  // the page to its right from posting.
  // 1. Fill the root leaf until the next key splits it; make that
  //    post fail: the root is flagged, its right page unposted.
  // 2. Insert keys past every other, into the right page, until it
  //    splits too: its post finds no level above it, so it finishes
  //    the root's split first, then its own.
  // 3. The tree checks whole, and holds every key.
  db_t d;
  open_tree_t t(d, 19, 512);
  REQUIRE(0 == tree_create(t.tree));
  std::set<std::string> keys;
  int n = 0;
  size_t height = 1;
  detail::fail_posts = 1;
  while (1 == height) {
    std::string key = "k" + std::to_string(10000 + n++);
    int rc = tree_insert(t.tree, key, "v");
    keys.insert(key);
    if (rc < 0) {
      break;
    }
    REQUIRE(0 == tree_height(t.tree, height));
  }
  REQUIRE(0 == detail::fail_posts);
  REQUIRE(0 == tree_height(t.tree, height));
  CHECK(1u == height);
  for (int i = 0; i < 200; i++) {
    std::string key = "k" + std::to_string(10000 + n++);
    REQUIRE_MESSAGE(0 == tree_insert(t.tree, key, "v"),
                    std::string(sm1::errmsg()));
    keys.insert(key);
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  REQUIRE(0 == tree_height(t.tree, height));
  CHECK(height >= 2u);
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(keys.size() == drain(c).size());
}
