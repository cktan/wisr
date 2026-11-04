/*
 * arena.cc -- the memory area, arena_t (include/sm0_arena.h).
 */

#include "sm0_arena.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <new>

namespace sm0 {

/* Free the blocks of both lists. */
arena_t::~arena_t() {
  for (block_t *list : {m_blocks, m_large}) {
    while (nullptr != list) {
      block_t *next = list->next;
      ::operator delete(list);
      list = next;
    }
  }
}

/*
 * Make a block for n bytes, the newest block having no room for them.
 * 1. Make a block of n bytes if n is above block_room, else of
 *    block_room.
 * 2. Link it first on the list of large blocks, or on the list that
 *    pieces are cut from.
 * 3. Give the piece all of a large block, or the start of the other.
 */
char *arena_t::grow(size_t n) {
  // No object is larger, so the size below cannot wrap.
  assert(n <= PTRDIFF_MAX);
  bool large = n > block_room;
  // Out of memory aborts the process, by the new-handler.
  void *raw = ::operator new(sizeof(block_t) + (large ? n : block_room));
  block_t *b = static_cast<block_t *>(raw);
  block_t *&list = large ? m_large : m_blocks;
  b->next = list;
  b->top = n;
  list = b;
  return bytes(b);
}

} // namespace sm0
