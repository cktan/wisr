/*
 * sm0_blist.h -- a list of fixed blocks, appended to and cut back
 * to a mark.
 *
 *   sm0::blist_t<lockref_t, 1024> locks;
 *   lockref_t *l = locks.emplace();          // value-initialized
 *   auto m = locks.mark();                   // a savepoint
 *   ...
 *   locks.cut(m);                            // drop what followed
 *
 * An entry never moves: a pointer to one stays good until a cut
 * drops it, unlike std::vector's, which a growth moves. Blocks of
 * N entries are linked oldest first; a cut gives the blocks past
 * its mark to a list of spares, keeping up to four, and the
 * appends that follow take them before allocating, so a list
 * that is filled and emptied again and again settles into
 * allocating nothing. reserve() makes sure of a spare before a
 * caller takes a lock it must not allocate under.
 *
 * T must be trivially destructible: a cut only moves the top, and
 * runs no destructor. With T = char the list is an arena of
 * bytes as well: alloc(n) takes n contiguous bytes, which never
 * straddle two blocks, and n above N takes a block of its own,
 * freed at the cut rather than kept.
 *
 * A mark stays good until a cut to an earlier one; a cut to a
 * mark no longer good is undefined.
 */
#ifndef SM0_BLIST_H
#define SM0_BLIST_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace sm0 {

template <typename T, uint32_t N> class blist_t {
  static_assert(std::is_trivially_destructible_v<T>);
  static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  static_assert(0 < N);

  /* A block: N entries, or more for one alloc() above N. */
  struct block_t {
    block_t *next;
    uint32_t top; /* entries in use */
    uint32_t cap; /* entries it has room for */
    alignas(T) char ch[N * sizeof(T)];
  };

public:
  /* A point in the list to cut back to: a block and its top. */
  struct mark_t {
    block_t *b = nullptr; /* nullptr: the list's start */
    uint32_t top = 0;
  };

  /* A position in the list, from oldest entry to newest. */
  class iterator_t {
  public:
    /* The entry here. */
    T &operator*() const { return blist_t::at(m_b, m_i); }

    /* The entry here, for a member's access. */
    T *operator->() const { return &blist_t::at(m_b, m_i); }

    /* Step to the next entry, across a block's end. */
    iterator_t &operator++() {
      m_i++;
      skip();
      return *this;
    }

    /* Whether two positions are the same. */
    bool operator==(const iterator_t &o) const {
      return m_b == o.m_b && m_i == o.m_i;
    }

  private:
    friend class blist_t;

    /* The position at entry i of b, or past it if b has no more. */
    iterator_t(block_t *b, uint32_t i) : m_b(b), m_i(i) { skip(); }

    /* Leave the blocks with nothing at or past m_i. */
    void skip() {
      while (nullptr != m_b && m_i == m_b->top) {
        m_b = m_b->next;
        m_i = 0;
      }
    }

    block_t *m_b;
    uint32_t m_i;
  };

  blist_t() = default;

  /* Free every block, the spares too. */
  ~blist_t() {
    release(m_head, true);
    release(m_spare, true);
  }

  /* A list is never copied: two would own its blocks. */
  blist_t(const blist_t &) = delete;
  blist_t &operator=(const blist_t &) = delete;

  /* Construct an entry past the last, and return it. */
  template <class... A> T *emplace(A &&...a) {
    if (nullptr == m_tail || m_tail->top == m_tail->cap) {
      append(take());
    }
    T *p = new (&at(m_tail, m_tail->top)) T(std::forward<A>(a)...);
    m_tail->top++;
    m_size++;
    return p;
  }

  /* Take n contiguous bytes past the last, uninitialized. */
  char *alloc(size_t n)
    requires std::is_same_v<T, char>
  {
    assert(n <= UINT32_MAX);
    if (nullptr == m_tail || m_tail->cap - m_tail->top < n) {
      append(n > N ? make(static_cast<uint32_t>(n)) : take());
    }
    char *p = &at(m_tail, m_tail->top);
    m_tail->top += static_cast<uint32_t>(n);
    m_size += n;
    return p;
  }

  /* The point past the last entry, to cut back to. */
  mark_t mark() const { return {m_tail, nullptr == m_tail ? 0 : m_tail->top}; }

  /* Drop the entries after m, keeping their blocks as spares. */
  void cut(mark_t m) {
    block_t *rest = m_head;
    if (nullptr != m.b) {
      assert(m.top <= m.b->top);
      rest = m.b->next;
      m_size -= m.b->top - m.top;
      m.b->top = m.top;
      m.b->next = nullptr;
    } else {
      m_head = nullptr;
    }
    m_tail = m.b;
    for (block_t *b = rest; nullptr != b; b = b->next) {
      m_size -= b->top;
    }
    release(rest, false);
  }

  /* Drop every entry, keeping the blocks as spares. */
  void clear() { cut(mark_t{}); }

  /* Make sure of a spare, so the next block an append needs is
   * allocated now. */
  void reserve() {
    if (nullptr == m_spare) {
      block_t *b = make(N);
      b->next = nullptr;
      m_spare = b;
      m_nspare = 1;
    }
  }

  /* Exchange two lists' entries and spares. */
  void swap(blist_t &o) {
    std::swap(m_head, o.m_head);
    std::swap(m_tail, o.m_tail);
    std::swap(m_spare, o.m_spare);
    std::swap(m_nspare, o.m_nspare);
    std::swap(m_size, o.m_size);
  }

  /* The entries in the list; with T = char, the bytes taken. */
  size_t size() const { return m_size; }

  /* Whether the list has no entry. */
  bool empty() const { return 0 == m_size; }

  /* The spares kept, for the tests. */
  int spares() const { return m_nspare; }

  /* The oldest entry's position. */
  iterator_t begin() const { return iterator_t(m_head, 0); }

  /* The position past the newest entry. */
  iterator_t end() const { return iterator_t(nullptr, 0); }

  /* The position of the first entry after m. */
  iterator_t from(mark_t m) const {
    return nullptr == m.b ? begin() : iterator_t(m.b, m.top);
  }

private:
  /* The most spares a cut keeps. */
  static constexpr int keep = 4;

  /* Entry i of block b. */
  static T &at(block_t *b, uint32_t i) {
    // A block made for a large alloc() runs past ch's declared
    // size, so the entries are addressed from the block's start.
    char *base = reinterpret_cast<char *>(b) + offsetof(block_t, ch);
    return reinterpret_cast<T *>(base)[i];
  }

  /* A new, empty block with room for cap entries. */
  static block_t *make(uint32_t cap) {
    size_t data = static_cast<size_t>(cap > N ? cap : N) * sizeof(T);
    // Out of memory aborts the process, by the new-handler.
    void *raw = ::operator new(offsetof(block_t, ch) + data);
    block_t *b = static_cast<block_t *>(raw);
    b->next = nullptr;
    b->top = 0;
    b->cap = cap > N ? cap : N;
    return b;
  }

  /* A spare if there is one, else a new block of N entries. */
  block_t *take() {
    if (nullptr == m_spare) {
      return make(N);
    }
    block_t *b = m_spare;
    m_spare = b->next;
    m_nspare--;
    b->next = nullptr;
    b->top = 0;
    return b;
  }

  /* Link b past the last block. */
  void append(block_t *b) {
    if (nullptr == m_tail) {
      m_head = b;
    } else {
      m_tail->next = b;
    }
    m_tail = b;
  }

  /* Give up the blocks from b on: as spares, up to keep of those
   * of N entries, unless all is set; the rest freed. */
  void release(block_t *b, bool all) {
    while (nullptr != b) {
      block_t *next = b->next;
      if (!all && N == b->cap && m_nspare < keep) {
        b->next = m_spare;
        m_spare = b;
        m_nspare++;
      } else {
        ::operator delete(b);
      }
      b = next;
    }
  }

  block_t *m_head = nullptr;
  block_t *m_tail = nullptr;
  block_t *m_spare = nullptr;
  int m_nspare = 0;
  size_t m_size = 0;
};

} // namespace sm0

#endif /* SM0_BLIST_H */
