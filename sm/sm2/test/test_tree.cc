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
#include <chrono>
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

/* The sm1 key of a page of relid's tree. */
std::string page_name(uint16_t relid, uint64_t pageid) {
  std::string name{static_cast<char>(relid >> 8),
                   static_cast<char>(relid & 0xFF)};
  sm0::put_be64(name, pageid);
  return name;
}

/* Page pageid of relid's tree, decoded; REQUIREs it is there. */
page_t read_decoded(sm1::session_t *s, uint64_t store, uint16_t relid,
                    uint64_t pageid) {
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(0 == sm1::pin_page(s, store, page_name(relid, pageid),
                             sm1::page_mode_t::sh, 0, pin, bytes));
  page_view_t v;
  REQUIRE(0 == v.open(bytes));
  page_t page;
  v.load(page);
  sm1::unpin_page(s, pin);
  return page;
}

/* Write a page of store directly, as no tree would. */
void write_raw(sm1::session_t *s, uint64_t store, std::string_view key,
               std::string_view bytes) {
  sm1::page_t *pin = nullptr;
  std::string_view old;
  REQUIRE(0 == sm1::pin_page(s, store, key, sm1::page_mode_t::ex,
                             sm1::page_no_read, pin, old));
  std::vector<sm1::page_put_t> puts{{pin, key, bytes}};
  REQUIRE(0 == sm1::page_write(s, store, puts));
  sm1::unpin_page(s, pin);
}

/* The tree's leftmost leaf's page id: page 0's first child, down. */
uint64_t first_leaf(sm1::session_t *s, uint64_t store, uint16_t relid) {
  uint64_t pageid = 0;
  page_t page = read_decoded(s, store, relid, pageid);
  while (0 != page.level) {
    const std::string &child = page.entries.front().second;
    REQUIRE(8u == child.size());
    pageid = 0;
    for (int i = 7; i >= 0; i--) {
      pageid = pageid << 8 | static_cast<uint8_t>(child[i]);
    }
    page = read_decoded(s, store, relid, pageid);
  }
  return pageid;
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
  //    by the next link, and each leaf's prev is the leaf before it.
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
    // The links agree already, the post being all that is unmade.
    uint64_t before = 0;
    for (uint64_t id = first_leaf(t.s, d.store(), 9); 0 != id;) {
      page_t page = read_decoded(t.s, d.store(), 9, id);
      CHECK(before == page.prev);
      before = id;
      id = page.next;
    }
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
  // 3. Drop it: its root is gone.
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

TEST_CASE("tree: prune judges with no latch, and again before it removes") {
  // Verify a leaf's entries are judged unlatched, and that what is to
  // go is judged again under the latch, where a row written meanwhile
  // shows: an insert that finds its entry there moves no version.
  // 1. Fill a tree of several leaves with 3,000 keys.
  // 2. Prune with a judge that says every odd key is dead at first
  //    asking, and counts the latches held as it is asked.
  // 3. At second asking, one odd key has its row back: it stays.
  // 4. No first asking held a latch, and every second one held one;
  //    even keys were asked once, and clean leaves took no latch.
  db_t d;
  open_tree_t t(d, 13, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 3000; n++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(100000 + n), "v"));
  }
  const std::string back = std::to_string(100000 + 1501);
  std::map<std::string, int> asked;
  bool first_latched = false, second_unlatched = false;
  size_t removed = 0;
  REQUIRE(0 == tree_prune(
                   t.tree,
                   [&](std::string_view key, std::string_view) {
                     int n = ++asked[std::string(key)];
                     size_t held = sm1::pages_exclusive(t.s);
                     first_latched = first_latched || (1 == n && 0 != held);
                     second_unlatched =
                         second_unlatched || (2 == n && 0 == held);
                     if (2 == n && key == back) {
                       return 0;
                     }
                     return (key.back() - '0') % 2;
                   },
                   removed));
  CHECK_FALSE(first_latched);
  CHECK_FALSE(second_unlatched);
  CHECK(1499u == removed);
  for (const auto &[key, n] : asked) {
    CHECK((key.back() - '0') % 2 + 1 == n);
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  auto all = drain(c);
  CHECK(1501u == all.size());
  CHECK(all.end() != std::find_if(all.begin(), all.end(), [&](const auto &e) {
          return e.first == back;
        }));
}

TEST_CASE("tree: a tree open across another's splits finds every key") {
  // Verify a tree holds nothing that another tree's splits leave stale.
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
  REQUIRE_MESSAGE(0 == tree_check(r.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a leaf a probe reads is evicted before the inner pages") {
  // Verify a descent's miss of a leaf ages it as a leaf, not inner.
  // 1. Attach with the fewest frames a pool has, and make a tree over
  //    many leaves.
  // 2. Write other pages, more than the frames, so its leaves go.
  // 3. A tree opened anew probes the first key, missing its leaf.
  // 4. Write a turn of the clock's worth of other pages.
  // 5. The leaf is gone; the root stayed.
  sm1::env_options_t opt;
  opt.block_cache_bytes = 256 << 10;
  db_t d(&opt);
  open_tree_t t(d, 21, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  for (int n = 0; n < 500; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
  }
  // The first key's leaf, found by the inner pages alone, so it stays
  // unread, and a miss.
  auto name_of = [](uint64_t pageid) {
    std::string name{static_cast<char>(0), static_cast<char>(21)};
    sm0::put_be64(name, pageid);
    return name;
  };
  size_t height = 0;
  REQUIRE(0 == tree_height(t.tree, height));
  REQUIRE(height >= 2);
  uint64_t leaf = 0;
  for (size_t level = height; level-- > 1;) {
    sm1::page_t *pin = nullptr;
    std::string_view bytes;
    REQUIRE(0 == sm1::pin_page(t.s, d.store(), name_of(leaf),
                               sm1::page_mode_t::sh, 0, pin, bytes));
    page_view_t v;
    REQUIRE(0 == v.open(bytes));
    std::string_view child = v.value(0);
    REQUIRE(8u == child.size());
    leaf = 0;
    for (int i = 7; i >= 0; i--) {
      leaf = leaf << 8 | static_cast<uint8_t>(child[i]);
    }
    sm1::unpin_page(t.s, pin);
  }
  const std::string pad(4000, 'p');
  auto write_others = [&](int from, int count) {
    for (int n = from; n < from + count; n++) {
      write_raw(t.s, d.store(), "other" + std::to_string(n), pad);
    }
  };
  write_others(0, 128);
  tree_t probe;
  tree_open(probe, t.s, d.store(), 21, 512);
  cursor_t c;
  std::string k = key_of(0);
  cursor_open(probe, c, &k, true, &k, true);
  REQUIRE(1u == drain(c).size());
  write_others(1000, 70);
  CHECK_FALSE(sm1::page_cached(t.s, d.store(), name_of(leaf)));
  CHECK(sm1::page_cached(t.s, d.store(), name_of(0)));
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

TEST_CASE("tree: a unique insert reads no leaf past its prefix's") {
  // Verify tree_insert_unique stops its walk right at a leaf whose high
  // key is past the prefix, so it pins no right sibling of no use.
  // 1. Fill leaves with keys "a|" and even numbers; pick a leaf L and
  //    its right sibling N, and a prefix, an odd number, between L's
  //    last key and its high key: L holds no entry of it.
  // 2. Remove two keys of L, so the insert finds room and splits
  //    nothing.
  // 3. Another session pins N exclusive.
  // 4. A unique insert of the prefix goes in on L without waiting for
  //    N.
  db_t d;
  open_tree_t t(d, 19, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) {
    char buf[32];
    snprintf(buf, sizeof buf, "a|%06d", n);
    return std::string(buf);
  };
  for (int n = 0; n < 600; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n * 2), "v"));
  }
  uint64_t lid = first_leaf(t.s, d.store(), 19);
  page_t leaf = read_decoded(t.s, d.store(), 19, lid);
  REQUIRE(leaf.has_high_key());
  REQUIRE(0 != leaf.next);
  const std::string last = leaf.entries.back().first;
  int n = atoi(last.c_str() + 2);
  std::string prefix = key_of(n + 1);
  REQUIRE(prefix < leaf.high);

  // 2.
  auto drop = [](std::string_view, std::string_view) { return 0; };
  REQUIRE(0 == tree_remove_if(t.tree, leaf.entries[0].first, drop));
  REQUIRE(0 == tree_remove_if(t.tree, leaf.entries[1].first, drop));

  // 3.
  open_tree_t w(d, 19, 512);
  std::string right = page_name(19, leaf.next);
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(0 == sm1::pin_page(w.s, d.store(), right, sm1::page_mode_t::ex, 0,
                             pin, bytes));

  // 4.
  std::atomic<int> rc{-2};
  std::thread inserter([&] {
    auto none = [](std::string_view, std::string_view) { return 0; };
    rc = tree_insert_unique(t.tree, prefix, prefix + "x", "v", none);
  });
  for (int i = 0; i < 2000 && -2 == rc; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(0 == rc);
  sm1::unpin_page(w.s, pin);
  inserter.join();
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a probe reads an inner root from its copy, with no pin") {
  // Verify a descent takes no pin of page 0 while its session's copy
  // of it is good, and pins it again once the page has changed.
  // 1. A writer inserts until page 0 is an inner page.
  // 2. A reader probes once, keeping a copy of page 0.
  // 3. The writer holds page 0 exclusive; the reader's probe, on a
  //    thread, ends while it does.
  // 4. The writer writes page 0 again and holds it exclusive: the
  //    reader's probe now waits for it, and ends once it is let go.
  db_t d;
  open_tree_t w(d, 31, 512);
  open_tree_t r(d, 31, 512);
  REQUIRE(0 == tree_create(w.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  for (int n = 0; n < 2000; n++) {
    REQUIRE(0 == tree_insert(w.tree, key_of(n), "v"));
  }
  size_t height = 0;
  REQUIRE(0 == tree_height(w.tree, height));
  REQUIRE(height >= 2);

  // A probe of key n by the reader, on a thread: done once it ends,
  // found if it found the key.
  std::atomic<bool> done{false};
  std::atomic<bool> found{false};
  auto probe = [&](int n) {
    cursor_t c;
    std::string k = key_of(n);
    cursor_open(r.tree, c, &k, true, &k, true);
    std::string_view key, value;
    found = 0 == cursor_next(c, key, value);
    done = true;
  };
  auto waited = [&](bool want) {
    for (int i = 0; i < 2000 && done != want; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done == want;
  };
  probe(5);
  REQUIRE(found);

  std::string root = page_name(31, 0);
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(0 == sm1::pin_page(w.s, d.store(), root, sm1::page_mode_t::ex, 0, pin,
                             bytes));
  done = found = false;
  std::thread t1([&] { probe(7); });
  CHECK(waited(true));
  sm1::unpin_page(w.s, pin);
  t1.join();
  CHECK(found);

  REQUIRE(0 == sm1::pin_page(w.s, d.store(), root, sm1::page_mode_t::ex, 0, pin,
                             bytes));
  std::string same(bytes);
  std::vector<sm1::page_put_t> puts{{pin, root, same}};
  REQUIRE(0 == sm1::page_write(w.s, d.store(), puts));
  done = found = false;
  std::thread t2([&] { probe(9); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_FALSE(done);
  sm1::unpin_page(w.s, pin);
  t2.join();
  CHECK(found);
  REQUIRE_MESSAGE(0 == tree_check(r.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a copy of the root follows a root that splits and grows") {
  // Verify a reader whose copy of page 0 goes stale, by splits and by
  // the tree growing a level, still finds every key.
  // 1. A reader probes a two-level tree, keeping a copy of page 0.
  // 2. A writer inserts until the tree is three levels high, page 0
  //    split under the copy.
  // 3. The reader finds every key, before and after.
  // 4. The tree made again under the relid, one leaf: the reader finds
  //    a key inserted and none of the old.
  db_t d;
  open_tree_t w(d, 32, 512);
  open_tree_t r(d, 32, 512);
  REQUIRE(0 == tree_create(w.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n * 7 % 30011); };
  auto finds = [&](int n) {
    cursor_t c;
    std::string k = key_of(n);
    cursor_open(r.tree, c, &k, true, &k, true);
    return drain(c).size();
  };
  int n = 0;
  size_t height = 1;
  while (height < 2) {
    REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
    REQUIRE(0 == tree_height(w.tree, height));
  }
  int first = n;
  for (int i = 0; i < first; i++) {
    CHECK(1u == finds(i));
  }
  while (height < 3) {
    REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
    REQUIRE(0 == tree_height(w.tree, height));
  }
  for (int i = 0; i < n; i++) {
    CHECK(1u == finds(i));
  }
  REQUIRE(0 == tree_create(w.tree));
  REQUIRE(0 == tree_insert(w.tree, key_of(0), "v"));
  CHECK(1u == finds(0));
  CHECK(0u == finds(1));
}

TEST_CASE("tree: a descent from a copy of the root stale by splits finds every "
          "key") {
  // Verify a descent from a copy of page 0 taken before splits under
  // it, and before the root's own split, finds every key: the window
  // between a check of the copy and the pin of the child it names.
  // 1. Insert until the tree is two levels high; save page 0's bytes.
  // 2. Insert until it is three levels high: every child page 0 named
  //    has split, and page 0 has split into two pages below it.
  // 3. Make the reader's copy of page 0 the saved bytes, with the
  //    frame's version as it is now, so the reader trusts them.
  // 4. The reader finds every key, the copy still the saved bytes: no
  //    descent pinned page 0 to replace it.
  // 5. The reader inserts keys through the stale copy; the tree
  //    checks whole, and holds every key.
  db_t d;
  open_tree_t w(d, 33, 512);
  open_tree_t r(d, 33, 512);
  REQUIRE(0 == tree_create(w.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n * 7 % 30011); };
  int n = 0;
  size_t height = 1;
  while (height < 2) {
    REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
    REQUIRE(0 == tree_height(w.tree, height));
  }
  std::string root = page_name(33, 0);
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  REQUIRE(0 == sm1::pin_page(w.s, d.store(), root, sm1::page_mode_t::sh, 0, pin,
                             bytes));
  std::string old(bytes);
  sm1::unpin_page(w.s, pin);
  page_view_t ov;
  REQUIRE(0 == ov.open(old));
  REQUIRE(1u == ov.level());

  while (height < 3) {
    REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
    REQUIRE(0 == tree_height(w.tree, height));
  }

  REQUIRE(0 == sm1::pin_page(r.s, d.store(), root, sm1::page_mode_t::sh, 0, pin,
                             bytes));
  sm1::keep_page_copy(r.s, d.store(), root, pin, old);
  sm1::unpin_page(r.s, pin);

  for (int i = 0; i < n; i++) {
    cursor_t c;
    std::string k = key_of(i);
    cursor_open(r.tree, c, &k, true, &k, true);
    CHECK(1u == drain(c).size());
  }
  std::string_view copy;
  REQUIRE(sm1::page_copy(r.s, d.store(), root, copy));
  CHECK(old == copy);

  int more = n + 500;
  for (int i = n; i < more; i++) {
    REQUIRE(0 == tree_insert(r.tree, key_of(i), "v"));
  }
  REQUIRE_MESSAGE(0 == tree_check(r.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(r.tree, c, nullptr, true, nullptr, true);
  CHECK(static_cast<size_t>(more) == drain(c).size());
}

TEST_CASE("tree: the root is page 0, at every height") {
  // Verify the root never moves, and says the tree's height itself.
  // 1. Insert until the tree is three levels high.
  // 2. At each height, page 0 is at the level below it.
  // 3. The tree checks whole, and holds every key.
  db_t d;
  open_tree_t t(d, 22, 512);
  REQUIRE(0 == tree_create(t.tree));
  size_t height = 1;
  int n = 0;
  while (height < 3) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(1000000 + n++), "v"));
    REQUIRE(0 == tree_height(t.tree, height));
    std::string name{static_cast<char>(0), static_cast<char>(22)};
    sm0::put_be64(name, 0);
    sm1::page_t *pin = nullptr;
    std::string_view bytes;
    REQUIRE(0 == sm1::pin_page(t.s, d.store(), name, sm1::page_mode_t::sh, 0,
                               pin, bytes));
    page_view_t v;
    REQUIRE(0 == v.open(bytes));
    CHECK(height == v.level() + 1u);
    CHECK_FALSE(v.has_high());
    sm1::unpin_page(t.s, pin);
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(static_cast<size_t>(n) == drain(c).size());
}

TEST_CASE("tree: a root's split has no half") {
  // Verify a root's split is one write, so no post can be cut from it.
  // 1. Arm a post to fail, and insert until the root leaf splits.
  // 2. The insert succeeds, the post unmade: no post was asked for.
  // 3. The tree checks whole.
  db_t d;
  open_tree_t t(d, 19, 512);
  REQUIRE(0 == tree_create(t.tree));
  detail::fail_posts = 1;
  size_t height = 1;
  int n = 0;
  while (1 == height) {
    REQUIRE_MESSAGE(0 ==
                        tree_insert(t.tree, std::to_string(1000000 + n++), "v"),
                    std::string(sm1::errmsg()));
    REQUIRE(0 == tree_height(t.tree, height));
  }
  CHECK(1 == detail::fail_posts);
  detail::fail_posts = 0;
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a cursor reads on past a root leaf that split") {
  // Verify a cursor on page 0 finds its place when page 0 grows.
  // 1. Fill the root leaf, and read every key of it by a cursor, which
  //    remembers page 0 as its leaf.
  // 2. Another tree inserts keys past those, until the root splits.
  // 3. The cursor reads on: the new keys alone, in order.
  db_t d;
  open_tree_t t(d, 23, 512);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  for (int n = 0; n < 5; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
  }
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  std::string_view k, v;
  for (int n = 0; n < 5; n++) {
    REQUIRE(0 == cursor_next(c, k, v));
    CHECK(key_of(n) == k);
  }
  int n = 5;
  {
    open_tree_t w(d, 23, 512);
    size_t height = 1;
    while (1 == height) {
      REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
      REQUIRE(0 == tree_height(w.tree, height));
    }
  }
  auto rest = drain(c);
  REQUIRE(static_cast<size_t>(n - 5) == rest.size());
  for (size_t i = 0; i < rest.size(); i++) {
    CHECK(key_of(5 + static_cast<int>(i)) == rest[i].first);
  }
}

TEST_CASE("tree: writers race a root as it grows") {
  // Verify a writer that found page 0 at one level, and latched it at
  // another, looks again.
  // 1. Eight trees, one after another, each from an empty root.
  // 2. Four threads insert keys of their own into it at once, so they
  //    meet at page 0 as it splits, and at each level it then grows.
  // 3. Each tree checks whole, and holds every key.
  db_t d;
  for (uint16_t relid = 30; relid < 38; relid++) {
    {
      open_tree_t t(d, relid, 256);
      REQUIRE(0 == tree_create(t.tree));
    }
    std::atomic<int> failed{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; w++) {
      threads.emplace_back([&, w] {
        sm1::session_t *s = d.open();
        sm1::begin(s, sm1::xctlevel_t::read_committed);
        tree_t tree;
        tree_open(tree, s, d.store(), relid, 256);
        for (int n = 0; n < 300; n++) {
          std::string key = std::to_string(100 + n) + "-" + std::to_string(w);
          if (0 != tree_insert(tree, key, "v")) {
            failed++;
          }
        }
        sm1::commit(s);
        sm1::closedb(s);
      });
    }
    for (auto &th : threads) {
      th.join();
    }
    CHECK(0 == failed);
    open_tree_t t(d, relid, 256);
    REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
    cursor_t c;
    cursor_open(t.tree, c, nullptr, true, nullptr, true);
    CHECK(1200u == drain(c).size());
  }
}

TEST_CASE("tree: a split links a page's old sibling back to the new page") {
  // Verify prev is kept, and that a tree with it wrong does not check.
  // 1. Insert keys in the middle of a tree, so the page split has a
  //    right sibling, and at its right edge, so it has none.
  // 2. Walking the leaves right, each prev is the leaf before; walking
  //    them left from the last, by prev, meets every leaf.
  // 3. Write a leaf with prev wrong: the tree no longer checks.
  db_t d;
  open_tree_t t(d, 27, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 1499; n++) {
    REQUIRE(0 ==
            tree_insert(t.tree, std::to_string(1000000 + n * 7 % 1499), "v"));
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  std::vector<uint64_t> leaves;
  for (uint64_t id = first_leaf(t.s, d.store(), 27); 0 != id;) {
    leaves.push_back(id);
    id = read_decoded(t.s, d.store(), 27, id).next;
  }
  REQUIRE(leaves.size() > 10);
  std::vector<uint64_t> back;
  for (uint64_t id = leaves.back(); 0 != id;) {
    back.push_back(id);
    id = read_decoded(t.s, d.store(), 27, id).prev;
  }
  std::reverse(back.begin(), back.end());
  CHECK(leaves == back);

  page_t page = read_decoded(t.s, d.store(), 27, leaves[3]);
  page.prev = 12345;
  std::string name = page_name(27, leaves[3]);
  std::string bytes = page_bytes(page);
  write_raw(t.s, d.store(), name, bytes);
  CHECK(-1 == tree_check(t.tree));
  CHECK(sm1::SM_ERR_CORRUPT == sm1::errnum());
}

TEST_CASE("tree: the longest keys, at the real page limit") {
  // Verify splits at every level with keys as long as a tree takes.
  // 1. At page_limit, keys of the most an entry may be: three fit a
  //    page at most, so nearly every insert splits one.
  // 2. Insert 2,000 out of order, so splits land all over the tree.
  // 3. The tree is at least five levels high, checks whole, and a
  //    cursor reads every key, in order.
  db_t d;
  open_tree_t t(d, 26, page_limit);
  REQUIRE(0 == tree_create(t.tree));
  const size_t key_len = entry_max(page_limit) - entry_size(0, 8);
  auto key_of = [&](int n) {
    std::string key = std::to_string(1000000 + n);
    key.resize(key_len, 'k');
    return key;
  };
  std::vector<int> order(2000);
  for (int n = 0; n < 2000; n++) {
    order[n] = n;
  }
  std::shuffle(order.begin(), order.end(), std::mt19937(26));
  for (int n : order) {
    REQUIRE_MESSAGE(0 == tree_insert(t.tree, key_of(n), "v"),
                    std::string(sm1::errmsg()));
  }
  size_t height = 0;
  REQUIRE(0 == tree_height(t.tree, height));
  CHECK(height >= 5);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  auto all = drain(c);
  REQUIRE(2000u == all.size());
  for (int n = 0; n < 2000; n++) {
    CHECK(key_of(n) == all[n].first);
  }
}

TEST_CASE("tree: a tree open while the root grows posts above it") {
  // Verify a tree open across the root's splits finds their parents.
  // 1. A tree two levels high; a writer's tree reads it, so it holds
  //    page 0 at level 1.
  // 2. Another tree inserts until the tree is at least four levels.
  // 3. The first writer, its pages unchanged, inserts keys below every
  //    other: its held page 0 leads it to the leftmost leaf with no
  //    move right, so it keeps it, and the leaf's split asks page 0 to
  //    be its parent, which it is no longer.
  // 4. It inserts enough more to split pages at every level.
  // 5. The tree checks whole, and holds every key.
  db_t d;
  open_tree_t r(d, 24, 256);
  REQUIRE(0 == tree_create(r.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n * 7 % 30011); };
  int n = 0;
  size_t height = 1;
  while (2 != height) {
    REQUIRE(0 == tree_insert(r.tree, key_of(n++), "v"));
    REQUIRE(0 == tree_height(r.tree, height));
  }
  REQUIRE(0 == tree_insert(r.tree, key_of(n++), "v"));
  {
    open_tree_t w(d, 24, 256);
    while (height < 4) {
      REQUIRE(0 == tree_insert(w.tree, key_of(n++), "v"));
      REQUIRE(0 == tree_height(w.tree, height));
    }
  }
  for (int i = 0; i < 100; i++) {
    REQUIRE_MESSAGE(
        0 == tree_insert(r.tree, "0" + std::to_string(1000 + i), "v"),
        std::string(sm1::errmsg()));
    n++;
  }
  for (int i = 0; i < 3000; i++) {
    REQUIRE_MESSAGE(0 == tree_insert(r.tree, key_of(n - 100), "v"),
                    std::string(sm1::errmsg()));
    n++;
  }
  REQUIRE_MESSAGE(0 == tree_check(r.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(r.tree, c, nullptr, true, nullptr, true);
  CHECK(static_cast<size_t>(n) == drain(c).size());
}

TEST_CASE("tree: page ids go on across a restart") {
  // Verify no page id is given twice, a restart between.
  // 1. Make a tree of many pages.
  // 2. Restart, and insert as many keys again, making pages anew.
  // 3. The tree checks whole, and holds every key: a page written over
  //    would have lost some.
  db_t d;
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  {
    open_tree_t t(d, 25, 512);
    REQUIRE(0 == tree_create(t.tree));
    for (int n = 0; n < 2000; n++) {
      REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
    }
  }
  d.restart();
  open_tree_t t(d, 25, 512);
  for (int n = 2000; n < 4000; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(4000u == drain(c).size());
}

TEST_CASE("tree: a post a crash left made finds its entry there") {
  // Verify a split whose parent has its entry, its child still flagged,
  // is finished by the next writer, and lets the child go.
  // 1. Arm a post to fail, and insert until a leaf splits: the leaf is
  //    left flagged.
  // 2. Write the parent's entry by hand, as if only the child's write
  //    was lost.
  // 3. An insert into the leaf finds the entry there, unflags the
  //    child, and goes on to insert: it does not wait on its own pin.
  // 4. The tree checks whole.
  db_t d;
  open_tree_t t(d, 28, 512);
  REQUIRE(0 == tree_create(t.tree));
  size_t height = 1;
  int n = 0;
  while (1 == height) {
    REQUIRE(0 == tree_insert(t.tree, "k" + std::to_string(1000 + n++), "v"));
    REQUIRE(0 == tree_height(t.tree, height));
  }
  detail::fail_posts = 1;
  int rc = 0;
  std::string key;
  while (0 == rc) {
    key = "k0" + std::to_string(1000 + n++);
    rc = tree_insert(t.tree, key, "v");
  }
  REQUIRE(-1 == rc);
  page_t leaf;
  uint64_t id = first_leaf(t.s, d.store(), 28);
  for (;;) {
    REQUIRE(0 != id);
    leaf = read_decoded(t.s, d.store(), 28, id);
    if (0 != (leaf.flags & page_incomplete)) {
      break;
    }
    id = leaf.next;
  }
  page_t root = read_decoded(t.s, d.store(), 28, 0);
  std::string child;
  for (int i = 0; i < 8; i++) {
    child.push_back(static_cast<char>((leaf.next >> (8 * i)) & 0xFF));
  }
  REQUIRE(0 == page_insert(root, leaf.high, child, 512));
  root.version++;
  write_raw(t.s, d.store(), page_name(28, 0), page_bytes(root));
  std::string again = leaf.entries.front().first + "x";
  REQUIRE_MESSAGE(0 == tree_insert(t.tree, again, "v"),
                  std::string(sm1::errmsg()));
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: writers, readers and a vacuum through a small pool") {
  // Verify exclusive and shared pins never wait on each other in a
  // cycle, and no reader loses a key, with pages evicted all along.
  // 1. Attach with the fewest frames, and make a tree of 5,000 keys,
  //    far more pages than frames.
  // 2. Three writers insert keys of their own, two readers scan ranges
  //    and probe, and a vacuum prunes keys the writers made to go.
  // 3. It ends: no thread failed, and no key of the first 5,000 was
  //    missing from any read.
  // 4. The tree checks whole, and holds every key not pruned.
  sm1::env_options_t opt;
  opt.block_cache_bytes = 256 << 10;
  db_t d(&opt);
  open_tree_t t(d, 29, 256);
  REQUIRE(0 == tree_create(t.tree));
  auto key_of = [](int n) { return std::to_string(1000000 + n); };
  for (int n = 0; n < 5000; n++) {
    REQUIRE(0 == tree_insert(t.tree, key_of(n), "v"));
  }
  std::atomic<int> failed{0}, missing{0}, writers{3};
  std::vector<std::thread> threads;
  auto in_tree = [&](auto body) {
    sm1::session_t *s = d.open();
    sm1::begin(s, sm1::xctlevel_t::read_committed);
    tree_t tree;
    tree_open(tree, s, d.store(), 29, 256);
    body(tree);
    sm1::commit(s);
    sm1::closedb(s);
  };
  for (int w = 0; w < 3; w++) {
    threads.emplace_back([&, w] {
      in_tree([&](tree_t &tree) {
        for (int n = 0; n < 1500; n++) {
          std::string key = std::to_string(1000000 + (n * 7 + w) % 5000) + "-" +
                            std::to_string(w) + (0 == n % 2 ? "dead" : "live");
          if (0 != tree_insert(tree, key, "v")) {
            failed++;
          }
        }
      });
      writers--;
    });
  }
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&, r] {
      in_tree([&](tree_t &tree) {
        unsigned x = 12345 + r;
        while (0 != writers) {
          x = x * 1103515245 + 12345;
          int from = (x >> 8) % 4900;
          std::string lo = key_of(from), hi = key_of(from + 50);
          cursor_t c;
          cursor_open(tree, c, &lo, true, &hi, false);
          std::string_view k, v;
          int found = 0, rc = 0;
          while (0 == (rc = cursor_next(c, k, v))) {
            found += std::string::npos == k.find('-') ? 1 : 0;
          }
          if (rc < 0) {
            failed++;
          }
          if (50 != found) {
            missing++;
          }
        }
      });
    });
  }
  threads.emplace_back([&] {
    in_tree([&](tree_t &tree) {
      while (0 != writers) {
        size_t removed = 0;
        auto dead = [](std::string_view k, std::string_view) {
          return k.size() > 4 && "dead" == k.substr(k.size() - 4) ? 1 : 0;
        };
        if (0 != tree_prune(tree, dead, removed)) {
          failed++;
        }
      }
    });
  });
  for (auto &th : threads) {
    th.join();
  }
  CHECK(0 == failed);
  CHECK(0 == missing);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  size_t live = 0;
  for (const auto &[k, v] : drain(c)) {
    live += std::string::npos == k.find("dead") ? 1 : 0;
  }
  CHECK(5000u + 3 * 750 == live);
}

TEST_CASE("tree: as many writers splitting as frames can bear") {
  // Verify a pool with frames for max_sessions never runs its sessions
  // out of frames, each waiting with all it holds.
  // 1. Attach with the least budget, and max_sessions for 18.
  // 2. Sixteen threads insert long keys of their own into one tree, so
  //    nearly every insert splits a page, parents among them.
  // 3. It ends: no insert failed, and the tree checks whole.
  sm1::env_options_t opt;
  opt.block_cache_bytes = 64 << 10;
  opt.max_sessions = 18;
  db_t d(&opt);
  open_tree_t t(d, 40, 256);
  REQUIRE(0 == tree_create(t.tree));
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 16; w++) {
    threads.emplace_back([&, w] {
      sm1::session_t *s = d.open();
      sm1::begin(s, sm1::xctlevel_t::read_committed);
      tree_t tree;
      tree_open(tree, s, d.store(), 40, 256);
      for (int n = 0; n < 150; n++) {
        std::string key = std::to_string(1000 + (n * 31) % 997) + "-" +
                          std::to_string(w) + std::string(30, 'x');
        if (0 != tree_insert(tree, key, "v")) {
          failed++;
        }
      }
      sm1::commit(s);
      sm1::closedb(s);
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  CHECK(0 == failed);
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  CHECK(16u * 150 == drain(c).size());
}

namespace {

/* Each leaf of relid's tree, left to right: its page id and its page. */
std::vector<std::pair<uint64_t, page_t>>
leaves(sm1::session_t *s, uint64_t store, uint16_t relid) {
  std::vector<std::pair<uint64_t, page_t>> out;
  uint64_t pageid = first_leaf(s, store, relid);
  for (;;) {
    page_t page = read_decoded(s, store, relid, pageid);
    uint64_t next = page.next;
    out.emplace_back(pageid, std::move(page));
    if (0 == next) {
      return out;
    }
    pageid = next;
  }
}

/* Whether page is all visible. */
bool all_visible(const page_t &page) {
  return 0 != (page.flags & page_all_visible);
}

/* The page id of the leaf of relid's tree whose range holds key. */
uint64_t leaf_of(sm1::session_t *s, uint64_t store, uint16_t relid,
                 std::string_view key) {
  for (const auto &[id, page] : leaves(s, store, relid)) {
    if (!page.has_high_key() || key < page.high) {
      return id;
    }
  }
  FAIL("no leaf holds the key");
  return 0;
}

} // namespace

TEST_CASE(
    "tree: vacuum sets leaves all visible, and each write clears its own") {
  // Verify the all-visible bit (../BTREE.md §4.1): vacuum sets it on a
  // leaf of settled entries, a cursor reports it, a set leaf is not
  // judged again, and an insert, a key found there, or a delete's
  // clear unsets only its own leaf.
  // 1. Fill a tree of several leaves with 3,000 keys; none is set.
  // 2. Prune, every entry settled: every leaf is set, nothing removed,
  //    and a cursor reads every batch as visible.
  // 3. Prune again: no entry is asked of.
  // 4. Insert a new key: its leaf alone is unset.
  // 5. Insert a key there already: its leaf is unset, no entry added.
  // 6. tree_unset_visible on a third leaf unsets it; again, on the
  //    same leaf, writes nothing: its version stays.
  // 7. The tree checks whole.
  db_t d;
  open_tree_t t(d, 13, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 3000; n++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(100000 + n), "v"));
  }
  auto before = leaves(t.s, d.store(), 13);
  REQUIRE(before.size() > 4);
  for (const auto &[id, page] : before) {
    CHECK_FALSE(all_visible(page));
  }

  size_t removed = 0;
  REQUIRE(0 == tree_prune(
                   t.tree, [](std::string_view, std::string_view) { return 2; },
                   removed));
  CHECK(0u == removed);
  auto set = leaves(t.s, d.store(), 13);
  for (const auto &[id, page] : set) {
    CHECK(all_visible(page));
  }
  cursor_t c;
  cursor_open(t.tree, c, nullptr, true, nullptr, true);
  std::string_view k, v;
  size_t n = 0;
  while (0 == cursor_next(c, k, v)) {
    CHECK(c.visible);
    n++;
  }
  CHECK(3000u == n);

  size_t asked = 0;
  REQUIRE(0 == tree_prune(
                   t.tree,
                   [&](std::string_view, std::string_view) {
                     asked++;
                     return 2;
                   },
                   removed));
  CHECK(0u == asked);

  auto unset_only = [&](uint64_t leaf) {
    for (const auto &[id, page] : leaves(t.s, d.store(), 13)) {
      CHECK(all_visible(page) == (id != leaf));
    }
  };
  // A new key between two others, so it lands on a leaf that does not
  // split: its leaf alone goes unset.
  const std::string fresh = std::to_string(100000 + 7) + "5";
  uint64_t fresh_leaf = leaf_of(t.s, d.store(), 13, fresh);
  REQUIRE(0 == tree_insert(t.tree, fresh, "v"));
  unset_only(fresh_leaf);

  REQUIRE(0 == tree_prune(
                   t.tree, [](std::string_view, std::string_view) { return 2; },
                   removed));
  const std::string there = std::to_string(100000 + 1500);
  uint64_t there_leaf = leaf_of(t.s, d.store(), 13, there);
  REQUIRE(1 == tree_insert(t.tree, there, "v"));
  unset_only(there_leaf);

  REQUIRE(0 == tree_prune(
                   t.tree, [](std::string_view, std::string_view) { return 2; },
                   removed));
  const std::string gone = std::to_string(100000 + 2900);
  uint64_t gone_leaf = leaf_of(t.s, d.store(), 13, gone);
  REQUIRE(0 == tree_unset_visible(t.tree, gone));
  unset_only(gone_leaf);
  uint64_t version = read_decoded(t.s, d.store(), 13, gone_leaf).version;
  REQUIRE(0 == tree_unset_visible(t.tree, gone));
  CHECK(version == read_decoded(t.s, d.store(), 13, gone_leaf).version);

  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE(
    "tree: a leaf is set only if every entry is settled, judged latched") {
  // Verify vacuum sets a leaf only from a judging held under its
  // latch, of every entry, so a writer's row seen then keeps it unset;
  // and that a dead entry goes on a leaf that is set.
  // 1. Fill a tree of several leaves with 3,000 keys.
  // 2. Prune: every entry settled at first asking; at second asking,
  //    one key, waiting, is not settled; another is dead both times.
  // 3. Every entry was asked twice, the second time latched, the
  //    first time not.
  // 4. The waiting key's leaf is unset, every other set; the dead key
  //    is gone.
  db_t d;
  open_tree_t t(d, 13, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 3000; n++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(100000 + n), "v"));
  }
  const std::string waiting = std::to_string(100000 + 1000);
  const std::string dead = std::to_string(100000 + 2000);
  std::map<std::string, int> asked;
  bool first_latched = false, second_unlatched = false;
  size_t removed = 0;
  REQUIRE(0 == tree_prune(
                   t.tree,
                   [&](std::string_view key, std::string_view) {
                     int n = ++asked[std::string(key)];
                     size_t held = sm1::pages_exclusive(t.s);
                     first_latched = first_latched || (1 == n && 0 != held);
                     second_unlatched =
                         second_unlatched || (2 == n && 0 == held);
                     if (key == dead) {
                       return 1;
                     }
                     return 2 == n && key == waiting ? 0 : 2;
                   },
                   removed));
  CHECK_FALSE(first_latched);
  CHECK_FALSE(second_unlatched);
  CHECK(1u == removed);
  CHECK(3000u == asked.size());
  for (const auto &[key, n] : asked) {
    CHECK(2 == n);
  }
  uint64_t waiting_leaf = leaf_of(t.s, d.store(), 13, waiting);
  for (const auto &[id, page] : leaves(t.s, d.store(), 13)) {
    CHECK(all_visible(page) == (id != waiting_leaf));
    for (const auto &[key, value] : page.entries) {
      CHECK(key != dead);
    }
  }
  REQUIRE_MESSAGE(0 == tree_check(t.tree), std::string(sm1::errmsg()));
}

TEST_CASE("tree: a unique insert that finds its key unsets its leaf") {
  // Verify a unique insert of a key the tree has, as an update A to B
  // and back to A makes, clears its leaf's bit, the row having
  // changed, and adds nothing.
  // 1. Fill a tree with keys of one prefix each, and set every leaf.
  // 2. Insert, unique, a key that is there: 1, and its leaf alone is
  //    unset, the tree's entries the same.
  db_t d;
  open_tree_t t(d, 13, 512);
  REQUIRE(0 == tree_create(t.tree));
  for (int n = 0; n < 1000; n++) {
    REQUIRE(0 == tree_insert(t.tree, std::to_string(100000 + n) + "r", "v"));
  }
  size_t removed = 0;
  REQUIRE(0 == tree_prune(
                   t.tree, [](std::string_view, std::string_view) { return 2; },
                   removed));
  const std::string prefix = std::to_string(100000 + 500);
  const std::string key = prefix + "r";
  uint64_t leaf = leaf_of(t.s, d.store(), 13, key);
  CHECK(1 == tree_insert_unique(
                 t.tree, prefix, key, "v",
                 [](std::string_view, std::string_view) { return 0; }));
  size_t entries = 0;
  for (const auto &[id, page] : leaves(t.s, d.store(), 13)) {
    CHECK(all_visible(page) == (id != leaf));
    entries += page.entries.size();
  }
  CHECK(1000u == entries);
}
