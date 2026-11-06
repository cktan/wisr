/*
 * arena.cc -- the memory area, arena_t (include/sm0_arena.h).
 */

#include "sm0_arena.h"

#include <cassert>
#include <cstdint>
#include <new>

namespace sm0 {

/* Free every block, from the tail back. */
arena_t::~arena_t() {
  while (nullptr != m_tail) {
    block_t *prev = m_tail->prev;
    ::operator delete(m_tail);
    m_tail = prev;
  }
}

/*
 * Make a block for n bytes, the tail having no room for them.
 * 1. Make a block of n bytes if n is above block_room, else of
 *    block_room.
 * 2. Link a large block behind the tail, so the tail keeps its room.
 *    With no tail, the large block is the tail, marked full.
 * 3. Link any other block as the new tail.
 * 4. Give the piece all of a large block, or the start of the other.
 */
char *arena_t::grow(size_t n) {
  // No object is larger, so the size below cannot wrap.
  assert(n <= PTRDIFF_MAX);
  bool large = n > block_room;
  // Out of memory aborts the process, by the new-handler.
  void *raw =
      ::operator new(offsetof(block_t, data) + (large ? n : block_room));
  block_t *b = static_cast<block_t *>(raw);
  if (large && nullptr != m_tail) {
    b->prev = m_tail->prev;
    m_tail->prev = b;
    b->top = n;
  } else {
    b->prev = m_tail;
    // A large tail reads as full: block_room - top must not wrap.
    b->top = large ? block_room : n;
    m_tail = b;
  }
  return b->data;
}

} // namespace sm0
