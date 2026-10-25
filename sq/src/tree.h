/*
 * tree.h -- making the nodes and text a tree owns.
 *
 * A tree keeps a store of everything made for it and frees it all in
 * its destructor (sq.h). The parser makes every node with make_node
 * and copies every piece of text with dup_string, so that a parse
 * which fails halfway leaves nothing behind but the tree. Both are
 * cut from the store's arena, as PostgreSQL's palloc cuts from a
 * memory context, and go with it at once.
 */
#ifndef SQ_SRC_TREE_H
#define SQ_SRC_TREE_H

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <type_traits>

#include "sq.h"

namespace sq {

/*
 * Memory handed out in pieces and freed all at once: blocks, the first
 * held inline, each next twice the last's size up to a limit.
 */
class arena_t {
public:
  arena_t() = default;
  ~arena_t();
  arena_t(const arena_t &) = delete;
  arena_t &operator=(const arena_t &) = delete;

  /* size bytes aligned to align, a power of two; throws bad_alloc. */
  void *alloc(size_t size, size_t align) {
    char *p = reinterpret_cast<char *>(
        (reinterpret_cast<uintptr_t>(m_next) + align - 1) & ~(align - 1));
    if (p > m_end || size > static_cast<size_t>(m_end - p)) {
      return grow(size, align);
    }
    m_next = p + size;
    return p;
  }

private:
  /* A block past the first, its bytes after it. */
  struct block_t {
    block_t *next;
    size_t size;
  };

  /* A new block holding size bytes aligned to align; returns them. */
  void *grow(size_t size, size_t align);

  alignas(std::max_align_t) char m_first[4096];
  char *m_next = m_first;
  char *m_end = m_first + sizeof(m_first);
  block_t *m_blocks = nullptr;
  size_t m_block_size = 8192; /* the next block's size */
};

/*
 * What a tree owns: an arena with its nodes and text in it, and, for
 * each node whose type holds a list, how to destroy it: a list's
 * vector has storage of its own.
 */
struct store_t {
  /* A node to destroy when the tree goes; the newest first. */
  struct destroy_t {
    destroy_t *next;
    void *node;
    void (*destroy)(void *);
  };

  arena_t arena;
  destroy_t *destroys = nullptr;

  store_t() = default;
  ~store_t();
  store_t(const store_t &) = delete;
  store_t &operator=(const store_t &) = delete;
};

/* The one way into a tree's store from outside sq_tree_t. */
struct tree_access_t {
  /* The store of tree, made on first use. */
  static store_t &store(sq_tree_t &tree);
};

/* Make a node of type T, zeroed but for its tag, owned by tree. */
template <typename T> T *make_node(sq_tree_t &tree) {
  store_t &store = tree_access_t::store(tree);
  void *at = store.arena.alloc(sizeof(T), alignof(T));
  if constexpr (std::is_trivially_destructible_v<T>) {
    return new (at) T();
  } else {
    // Room for the record first, so that nothing can throw between
    // making the node and recording it.
    auto *d = static_cast<store_t::destroy_t *>(store.arena.alloc(
        sizeof(store_t::destroy_t), alignof(store_t::destroy_t)));
    T *node = new (at) T();
    *d = {store.destroys, node, [](void *p) { static_cast<T *>(p)->~T(); }};
    store.destroys = d;
    return node;
  }
}

/*
 * Copy text into tree, NUL-terminated. The copy is the caller's to
 * change until it is put in a node.
 */
char *dup_string(sq_tree_t &tree, std::string_view text);

/*
 * The tree the calling thread is building. The grammar's actions make
 * nodes far from anything that could hand them the tree, as
 * PostgreSQL's allocate in its current memory context, so a parse
 * names its tree here for as long as it runs.
 */
sq_tree_t *current_tree();

/* Names a tree as the one being built, for as long as this lives. */
class current_tree_t {
public:
  /* Make tree the calling thread's current tree. */
  explicit current_tree_t(sq_tree_t &tree);

  /* Put back whatever was current before. */
  ~current_tree_t();

  current_tree_t(const current_tree_t &) = delete;
  current_tree_t &operator=(const current_tree_t &) = delete;

private:
  sq_tree_t *m_previous;
};

/* Make a node of type T in the tree being built. */
template <typename T> T *make_node() { return make_node<T>(*current_tree()); }

/* Copy text into the tree being built: PostgreSQL's pstrdup. */
inline char *tree_strdup(std::string_view text) {
  return dup_string(*current_tree(), text);
}

} // namespace sq

#endif /* SQ_SRC_TREE_H */
