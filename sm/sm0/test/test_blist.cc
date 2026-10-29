/*
 * test_blist.cc -- the block list, blist_t (include/sm0_blist.h).
 */

#include "doctest.h"

#include "sm0_blist.h"

#include <cstring>
#include <vector>

namespace {

/* An entry with a value and a default, as a lock reference has. */
struct item_t {
  int v = -1;
  int w = 7;
};

/* The values in l, oldest first. */
template <typename L> std::vector<int> values(const L &l) {
  std::vector<int> out;
  for (const item_t &x : l) {
    out.push_back(x.v);
  }
  return out;
}

/* The values in l after m, oldest first. */
template <typename L, typename M> std::vector<int> after(const L &l, M m) {
  std::vector<int> out;
  for (auto it = l.from(m); it != l.end(); ++it) {
    out.push_back(it->v);
  }
  return out;
}

/* The values from 0 to n - 1. */
std::vector<int> upto(int n) {
  std::vector<int> out;
  for (int i = 0; i < n; i++) {
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
  //    iterates 0 to 9.
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
  CHECK(values(l) == upto(10));
  CHECK(&*l.last() == p[9]);
  CHECK(l.last()->v == 9);

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
  CHECK(after(l, m0) == upto(6));
  l.cut(m0);
  CHECK(l.empty());
  CHECK(l.begin() == l.end());

  for (int i = 0; i < 2; i++) {
    l.emplace(i);
  }
  auto m2 = l.mark();
  for (int i = 2; i < 9; i++) {
    l.emplace(i);
  }
  CHECK(after(l, m2) == std::vector<int>{2, 3, 4, 5, 6, 7, 8});
  l.cut(m2);
  CHECK(values(l) == upto(2));
  CHECK(l.size() == 2);

  l.emplace(2);
  l.emplace(3);
  auto m4 = l.mark(); // the first block is full
  for (int i = 4; i < 7; i++) {
    l.emplace(i);
  }
  CHECK(after(l, m4) == std::vector<int>{4, 5, 6});
  l.cut(m4);
  CHECK(values(l) == upto(4));
  CHECK(after(l, m4).empty());
  CHECK(l.last()->v == 3);
  l.emplace(4);
  CHECK(values(l) == upto(5));
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
  // 5. reserve(n) of bytes makes a spare only when the last block
  //    lacks n.
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

  sm0::blist_t<char, 8> k;
  k.alloc(5);
  k.reserve(3);
  CHECK(k.spares() == 0);
  k.reserve(4);
  CHECK(k.spares() == 1);
}

TEST_CASE("blist alloc never splits a key across blocks") {
  // Verify the byte arena: contiguous runs, and large ones apart.
  // 1. Two runs of five in blocks of eight: the second starts a
  //    new block, and both read back whole.
  // 2. A run of twenty, above a block, gets a block of its own
  //    and reads back whole.
  // 3. A cut past the large block frees it, keeping only the
  //    block of eight as a spare.
  sm0::blist_t<char, 8> k;
  char *a = k.alloc(5);
  memcpy(a, "abcde", 5);
  char *b = k.alloc(5);
  memcpy(b, "fghij", 5);
  CHECK(b != a + 5);
  CHECK(0 == memcmp(a, "abcde", 5));
  CHECK(0 == memcmp(b, "fghij", 5));
  CHECK(k.size() == 10);

  auto m = k.mark();
  char *big = k.alloc(20);
  memcpy(big, "0123456789abcdefghij", 20);
  char *tail = k.alloc(3);
  memcpy(tail, "xyz", 3);
  CHECK(0 == memcmp(big, "0123456789abcdefghij", 20));
  CHECK(0 == memcmp(tail, "xyz", 3));
  CHECK(k.size() == 33);

  k.cut(m);
  CHECK(k.size() == 10);
  CHECK(k.spares() == 1);
  CHECK(0 == memcmp(b, "fghij", 5));
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
  CHECK(values(a) == upto(1));
  CHECK(values(b) == upto(6));
  CHECK(a.size() == 1);
  CHECK(b.size() == 6);
  a.emplace(1);
  b.emplace(6);
  CHECK(values(a) == upto(2));
  CHECK(values(b) == upto(7));
}
