/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_blist.cc -- the block list, blist_t (include/sm0_blist.h).
 */

#include "doctest.h"

#include "sm0_blist.h"

#include <vector>

namespace {

/* An entry with a value and a default, as a lock reference has. */
struct item_t {
  int v = -1;
  int w = 7;
};

/* The values in l, newest first. */
template <typename L> std::vector<int> values(const L &l) {
  std::vector<int> out;
  l.each([&](const item_t &x) { out.push_back(x.v); });
  return out;
}

/* The values in l after m, newest first. */
template <typename L, typename M> std::vector<int> after(const L &l, M m) {
  std::vector<int> out;
  l.each_from(m, [&](const item_t &x) { out.push_back(x.v); });
  return out;
}

/* The values from n - 1 down to 0. */
std::vector<int> down(int n) {
  std::vector<int> out;
  for (int i = n - 1; i >= 0; i--) {
    out.push_back(i);
  }
  return out;
}

} // namespace

TEST_CASE("blist entries never move as the list grows") {
  // Verify pointer stability across blocks, and order.
  // 1. Emplace ten entries into blocks of four, keeping each
  //    pointer.
  // 2. Every pointer still reads its own value, and the list
  //    iterates 9 down to 0.
  // 3. An emplace with no arguments value-initializes; one with
  //    arguments constructs from them.
  sm0::blist_t<item_t, 4> l;
  std::vector<item_t *> p;
  for (int i = 0; i < 10; i++) {
    p.push_back(l.emplace(i, 100 + i));
  }
  for (int i = 0; i < 10; i++) {
    CHECK(p[i]->v == i);
    CHECK(p[i]->w == 100 + i);
  }
  CHECK(l.size() == 10);
  CHECK(values(l) == down(10));
  CHECK(l.find([](const item_t &) { return true; }) == p[9]);

  item_t *d = l.emplace();
  CHECK(d->v == -1);
  CHECK(d->w == 7);
}

TEST_CASE("blist cut drops what followed a mark, at every boundary") {
  // Verify cut at the start, mid-block, and a full block's end.
  // 1. A mark on an empty list, cut to after emplaces, leaves it
  //    empty.
  // 2. A mark mid-block keeps the entries before it, and from()
  //    lists those after.
  // 3. A mark taken when a block is exactly full keeps that
  //    block's entries and drops the next block's.
  // 4. Emplaces after each cut continue from the mark.
  sm0::blist_t<item_t, 4> l;
  auto m0 = l.mark();
  for (int i = 0; i < 6; i++) {
    l.emplace(i);
  }
  CHECK(after(l, m0) == down(6));
  l.cut(m0);
  CHECK(l.empty());
  CHECK(nullptr == l.find([](const item_t &) { return true; }));

  for (int i = 0; i < 2; i++) {
    l.emplace(i);
  }
  auto m2 = l.mark();
  for (int i = 2; i < 9; i++) {
    l.emplace(i);
  }
  CHECK(after(l, m2) == std::vector<int>{8, 7, 6, 5, 4, 3, 2});
  l.cut(m2);
  CHECK(values(l) == down(2));
  CHECK(l.size() == 2);

  l.emplace(2);
  l.emplace(3);
  auto m4 = l.mark(); // the first block is full
  for (int i = 4; i < 7; i++) {
    l.emplace(i);
  }
  CHECK(after(l, m4) == std::vector<int>{6, 5, 4});
  l.cut(m4);
  CHECK(values(l) == down(4));
  CHECK(after(l, m4).empty());
  l.emplace(4);
  CHECK(values(l) == down(5));
  CHECK(l.size() == 5);
}

TEST_CASE("blist reuses the blocks a cut gives up") {
  // Verify spares: a cut keeps them, an append takes them, up
  // to four.
  // 1. Fill two blocks, cut back to one: one spare.
  // 2. Emplace past the full block: it takes that spare's
  //    storage, the same address the cut entry had.
  // 3. Fill eight blocks and clear: four spares kept.
  // 4. reserve() on an empty list makes a spare, which an append
  //    consumes; with room in the last block it makes none.
  // 5. reserve(n) makes a spare only when the last block lacks
  //    room for n.
  sm0::blist_t<item_t, 4> l;
  for (int i = 0; i < 4; i++) {
    l.emplace(i);
  }
  auto m = l.mark();
  item_t *gone = l.emplace(4);
  l.cut(m);
  CHECK(l.spares() == 1);
  CHECK(l.emplace(5) == gone);
  CHECK(l.spares() == 0);

  for (int i = 0; i < 32; i++) {
    l.emplace(i);
  }
  l.clear();
  CHECK(l.empty());
  CHECK(l.spares() == 4);

  sm0::blist_t<item_t, 4> r;
  r.reserve();
  CHECK(r.spares() == 1);
  r.reserve();
  CHECK(r.spares() == 1);
  r.emplace(0);
  CHECK(r.spares() == 0);
  r.reserve();
  CHECK(r.spares() == 0);

  sm0::blist_t<item_t, 8> k;
  for (int i = 0; i < 5; i++) {
    k.emplace(i);
  }
  k.reserve(3);
  CHECK(k.spares() == 0);
  k.reserve(4);
  CHECK(k.spares() == 1);
}

TEST_CASE("blist swap exchanges two lists") {
  // Verify swap, as a prepare moves a session's locks.
  // 1. Two lists of different lengths swap entries and sizes.
  // 2. Each goes on appending to what it now holds.
  sm0::blist_t<item_t, 4> a;
  sm0::blist_t<item_t, 4> b;
  for (int i = 0; i < 6; i++) {
    a.emplace(i);
  }
  b.emplace(0);
  a.swap(b);
  CHECK(values(a) == down(1));
  CHECK(values(b) == down(6));
  CHECK(a.size() == 1);
  CHECK(b.size() == 6);
  a.emplace(1);
  b.emplace(6);
  CHECK(values(a) == down(2));
  CHECK(values(b) == down(7));
}

TEST_CASE("blist find returns the first match, from a mark") {
  // Verify find and find_from: newest first, across blocks, down to
  // the mark, and the entry itself, to change in place.
  // 1. Ten entries in blocks of four, two of each value mod 5: find
  //    of v % 5 == 3 returns the newer, 8, in the third block.
  // 2. find_from a mark after six entries returns 6, the oldest
  //    after it, mid-block, and none of the entries before it; one
  //    at a full block's end looks at none of that block.
  // 3. No match is nullptr; a change through the pointer is seen by
  //    each.
  sm0::blist_t<item_t, 4> l;
  sm0::blist_t<item_t, 4>::mark_t m4, m6;
  for (int i = 0; i < 10; i++) {
    if (4 == i) {
      m4 = l.mark();
    }
    if (6 == i) {
      m6 = l.mark();
    }
    l.emplace(i);
  }
  auto is3 = [](const item_t &x) { return 3 == x.v % 5; };
  item_t *f = l.find(is3);
  REQUIRE(nullptr != f);
  CHECK(f->v == 8);
  item_t *g = l.find_from(m6, [](const item_t &x) { return 1 == x.v % 5; });
  REQUIRE(nullptr != g);
  CHECK(g->v == 6);
  CHECK(nullptr ==
        l.find_from(m6, [](const item_t &x) { return 0 == x.v % 5; }));
  const item_t *h = l.find_from(m4, [](const item_t &x) { return x.v < 4; });
  CHECK(nullptr == h);

  CHECK(nullptr == l.find([](const item_t &x) { return 10 == x.v; }));
  f->v = 80;
  CHECK(values(l) == std::vector<int>{9, 80, 7, 6, 5, 4, 3, 2, 1, 0});
}
