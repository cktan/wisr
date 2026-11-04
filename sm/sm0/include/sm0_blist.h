/*
 * sm0_blist.h -- a list of fixed blocks, appended to and cut back
 * to a mark.
 *
 *   sm0::blist_t<lockref_t, 1024> locks;
 *   lockref_t *l = locks.emplace();          // value-initialized
 *   auto m = locks.mark();                   // a savepoint
 *   ...
 *   locks.cut(m);                            // drop what followed
 *   locks.each([](lockref_t &l) { ... });    // every entry, oldest first
 *
 * An entry never moves: a pointer to one stays good until a cut
 * drops it, unlike std::vector's, which a growth moves. Blocks of
 * N entries are linked oldest first; a cut gives the blocks past
 * its mark to a list of spares, keeping up to four, and the
 * appends that follow take them before allocating, so a list
 * that is filled and emptied again and again settles into
 * allocating nothing. reserve() makes sure of room before a
 * caller takes a lock it must not allocate under.
 *
 * T must be trivially destructible: a cut only moves the top, and
 * runs no destructor. For bytes, use arena_t (sm0_arena.h).
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

  /* A block: N entries. */
  struct block_t {
    block_t *next;
    uint32_t top; /* entries in use */
    alignas(T) char ch[N * sizeof(T)];
  };

public:
  /* A point in the list to cut back to: a block and its top. */
  struct mark_t {
    block_t *b = nullptr; /* nullptr: the list's start */
    uint32_t top = 0;
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
    if (nullptr == m_tail || N == m_tail->top) {
      append(take());
    }
    T *p = new (&at(m_tail, m_tail->top)) T(std::forward<A>(a)...);
    m_tail->top++;
    m_size++;
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

  /* Make sure of room for n more entries, at most N, allocating
   * now what they would need. */
  void reserve(size_t n = 1) {
    assert(n <= N);
    if (nullptr != m_tail && N - m_tail->top >= n) {
      return;
    }
    if (nullptr == m_spare) {
      m_spare = make();
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

  /* The entries in the list. */
  size_t size() const { return m_size; }

  /* Whether the list has no entry. */
  bool empty() const { return 0 == m_size; }

  /* The spares kept, for the tests. */
  int spares() const { return m_nspare; }

  /* The newest entry, or nullptr if there is none. */
  T *last() { return newest(); }
  const T *last() const { return newest(); }

  /* The first entry after m that pred(entry) is true of, oldest
   * first, or nullptr. */
  template <class F> T *find_from(mark_t m, F pred) { return scan(m, pred); }
  template <class F> const T *find_from(mark_t m, F pred) const {
    return scan(m, [&](T &x) { return pred(static_cast<const T &>(x)); });
  }

  /* The first entry pred(entry) is true of, oldest first, or nullptr. */
  template <class F> T *find(F pred) { return find_from(mark_t{}, pred); }
  template <class F> const T *find(F pred) const {
    return find_from(mark_t{}, pred);
  }

  /* Call fn(entry) on each entry after m, oldest first. */
  template <class F> void each_from(mark_t m, F fn) {
    find_from(m, [&](T &x) {
      fn(x);
      return false;
    });
  }
  template <class F> void each_from(mark_t m, F fn) const {
    find_from(m, [&](const T &x) {
      fn(x);
      return false;
    });
  }

  /* Call fn(entry) on each entry, oldest first. */
  template <class F> void each(F fn) { each_from(mark_t{}, fn); }
  template <class F> void each(F fn) const { each_from(mark_t{}, fn); }

private:
  /* The most spares a cut keeps. */
  static constexpr int keep = 4;

  /* The newest entry, or nullptr: the last block's last, as a
   * block is never left empty. */
  T *newest() const {
    return nullptr == m_tail || 0 == m_tail->top ? nullptr
                                                 : &at(m_tail, m_tail->top - 1);
  }

  /* The first entry after m that pred is true of, or nullptr. */
  template <class F> T *scan(mark_t m, F &&pred) const {
    block_t *b = nullptr == m.b ? m_head : m.b;
    for (uint32_t i = m.top; nullptr != b; b = b->next, i = 0) {
      for (; i < b->top; i++) {
        if (pred(at(b, i))) {
          return &at(b, i);
        }
      }
    }
    return nullptr;
  }

  /* Entry i of block b. */
  static T &at(block_t *b, uint32_t i) {
    return reinterpret_cast<T *>(b->ch)[i];
  }

  /* A new, empty block. */
  static block_t *make() {
    // Out of memory aborts the process, by the new-handler.
    void *raw = ::operator new(sizeof(block_t));
    block_t *b = static_cast<block_t *>(raw);
    b->next = nullptr;
    b->top = 0;
    return b;
  }

  /* A spare if there is one, else a new block of N entries. */
  block_t *take() {
    if (nullptr == m_spare) {
      return make();
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

  /* Give up the blocks from b on: up to keep as spares, unless
   * all is set; the rest freed. */
  void release(block_t *b, bool all) {
    while (nullptr != b) {
      block_t *next = b->next;
      if (!all && m_nspare < keep) {
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
