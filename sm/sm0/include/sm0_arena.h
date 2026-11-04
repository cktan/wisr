/*
 * sm0_arena.h -- a memory area: bytes cut into pieces, freed at once.
 *
 *   sm0::arena_t a;
 *   char *p = a.alloc(n);    // n bytes, uninitialized
 *   ...                      // the destructor frees every piece
 *
 * An arena holds bytes alone. A piece has no alignment and no
 * destructor: do not put an object in one. A piece never moves, and
 * stays good until the arena goes.
 *
 * Pieces are cut from the newest block of a list. A piece above
 * block_room gets a block of exactly its size, on a list of large
 * blocks, so the newest block keeps its room for the pieces to come.
 *
 * An arena has no mutex: one owner uses it. Out of memory aborts the
 * process, by the new-handler, so alloc() has no error to return.
 */
#ifndef SM0_ARENA_H
#define SM0_ARENA_H

#include <cstddef>

namespace sm0 {

class arena_t {
  /* A block: a header, then its bytes, past data's declared size. */
  struct block_t {
    block_t *next;
    size_t top; /* bytes in use */
    char data[1];
  };

  /* What a block takes, its header included, but for a large one. */
  static constexpr size_t block_size = 4096;

public:
  /* The largest piece that is cut from a block of the usual size. */
  static constexpr size_t block_room = block_size - offsetof(block_t, data);

  arena_t() = default;
  ~arena_t();

  /* An arena is never copied: two would own its blocks. */
  arena_t(const arena_t &) = delete;
  arena_t &operator=(const arena_t &) = delete;

  /* Take n bytes, uninitialized. */
  char *alloc(size_t n) {
    if (nullptr == m_blocks || n > block_room - m_blocks->top) [[unlikely]] {
      return grow(n);
    }
    char *p = m_blocks->data + m_blocks->top;
    m_blocks->top += n;
    return p;
  }

private:
  char *grow(size_t n);

  block_t *m_blocks = nullptr; /* the newest first, cut from */
  block_t *m_large = nullptr;  /* a block for each large piece */
};

} // namespace sm0

#endif /* SM0_ARENA_H */
