/*
 * test_arena.cc -- the memory area, arena_t (include/sm0_arena.h).
 */

#include "doctest.h"

#include "sm0_arena.h"

#include <cstring>
#include <vector>

using sm0::arena_t;

TEST_CASE("arena pieces are exact bytes in a block") {
  // Verify a piece takes only its bytes, up to a block's room.
  // 1. Two pieces of odd sizes follow each other with no padding.
  // 2. A piece that fills the block stays in it.
  // 3. One more byte starts a new block.
  arena_t a;
  char *p = a.alloc(3);
  char *q = a.alloc(5);
  CHECK(q == p + 3);
  char *r = a.alloc(arena_t::block_room - 8);
  CHECK(r == q + 5);
  char *s = a.alloc(1);
  CHECK(s != r + arena_t::block_room - 8);
}

TEST_CASE("arena block_room is the largest piece in a usual block") {
  // Verify the edge between a usual piece and a large one.
  // 1. A piece of block_room is not large: it ends the block before
  //    it, so the next piece follows neither of them.
  // 2. A piece of block_room + 1 is large: a piece taken before it
  //    and one taken after it are neighbours.
  arena_t a;
  char *first = a.alloc(8);
  char *p = a.alloc(arena_t::block_room);
  memset(p, 'x', arena_t::block_room);
  char *q = a.alloc(8);
  CHECK(q != first + 8);
  CHECK(q != p + arena_t::block_room);
  char *large = a.alloc(arena_t::block_room + 1);
  memset(large, 'y', arena_t::block_room + 1);
  char *r = a.alloc(8);
  CHECK(r == q + 8);
}

TEST_CASE("arena large piece leaves the newest block its room") {
  // Verify a large piece does not end the block small pieces use.
  // 1. Take a small piece, then large pieces between small ones.
  // 2. Confirm the small pieces stay neighbours in one block.
  // 3. Confirm every piece reads back whole.
  arena_t a;
  char *p = a.alloc(5);
  memcpy(p, "abcde", 5);
  char *big = a.alloc(9000);
  memset(big, 'x', 9000);
  char *q = a.alloc(5);
  memcpy(q, "fghij", 5);
  char *big2 = a.alloc(20000);
  memset(big2, 'y', 20000);
  char *r = a.alloc(5);
  memcpy(r, "klmno", 5);
  CHECK(q == p + 5);
  CHECK(r == q + 5);
  CHECK(0 == memcmp(p, "abcdefghijklmno", 15));
  for (int i = 0; i < 9000; i++) {
    REQUIRE(big[i] == 'x');
  }
  for (int i = 0; i < 20000; i++) {
    REQUIRE(big2[i] == 'y');
  }
}

TEST_CASE("arena large piece first of all") {
  // Verify an arena whose first block is large.
  // 1. Take a large piece, and a second, with no block before them.
  // 2. Take two small pieces: they share a new block.
  // 3. The destructor frees all of it, as ASan's leak check sees.
  arena_t a;
  char *big = a.alloc(9000);
  memset(big, 'x', 9000);
  char *big2 = a.alloc(9000);
  memset(big2, 'y', 9000);
  char *p = a.alloc(8);
  char *q = a.alloc(8);
  CHECK(q == p + 8);
  CHECK(big[8999] == 'x');
  CHECK(big2[8999] == 'y');
}

TEST_CASE("arena piece of no bytes is a pointer") {
  // Verify alloc(0) returns memory an empty arena has to make.
  // 1. Ask an empty arena for no bytes: the pointer is not null.
  // 2. The next piece follows it.
  arena_t a;
  char *p = a.alloc(0);
  CHECK(nullptr != p);
  CHECK(a.alloc(8) == p);
}

TEST_CASE("arena pieces never move or overlap") {
  // Verify many pieces across many blocks all keep their bytes.
  // 1. Take pieces of many sizes, large ones among them, each filled
  //    with a value of its own.
  // 2. Read every piece back.
  arena_t a;
  struct piece_t {
    char *p;
    size_t n;
    char v;
  };
  std::vector<piece_t> pieces;
  for (size_t i = 0; i < 5000; i++) {
    size_t n = 0 == i % 97 ? 5000 + i : 1 + i % 300;
    char v = static_cast<char>(i);
    char *p = a.alloc(n);
    memset(p, v, n);
    pieces.push_back({p, n, v});
  }
  for (const piece_t &x : pieces) {
    for (size_t i = 0; i < x.n; i++) {
      REQUIRE(x.p[i] == x.v);
    }
  }
}
