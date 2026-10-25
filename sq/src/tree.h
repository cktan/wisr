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

#include <string.h>

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
 * Where a tree's nodes, lists and text go: its arena, or the caller's
 * allocator (sq_options_t), whose memory the tree never frees.
 */
struct store_t {
  arena_t arena;
  const sq_allocator_t *allocator = nullptr;

  store_t() = default;
  store_t(const store_t &) = delete;
  store_t &operator=(const store_t &) = delete;

  /* size bytes aligned to align, at most 8; throws bad_alloc. */
  void *alloc(size_t size, size_t align) {
    if (allocator == nullptr) {
      return arena.alloc(size, align);
    }
    // The caller's memory is aligned for any node.
    void *p = allocator->alloc(allocator->arg, size);
    if (p == nullptr) {
      throw std::bad_alloc();
    }
    return p;
  }

  /* ptr, alloc'd with old bytes, grown to size; throws bad_alloc. */
  void *realloc(void *ptr, size_t old, size_t size);
};

/* The one way into a tree's store from outside sq_tree_t. */
struct tree_access_t {
  /* The store of tree, made on first use. */
  static store_t &store(sq_tree_t &tree) {
    if (tree.m_store == nullptr) {
      // Not value-initialized: the arena's first block is left as it is.
      tree.m_store = new store_t;
    }
    return *tree.m_store;
  }
};

/* Make a node of type T, zeroed but for its tag, owned by tree. */
template <typename T> T *make_node(sq_tree_t &tree) {
  // A node is never destroyed: its memory goes with the tree, or is
  // the caller's.
  static_assert(std::is_trivially_destructible_v<T>);
  void *at = tree_access_t::store(tree).alloc(sizeof(T), alignof(T));
  // Zeroed, as PostgreSQL's makeNode zeroes, padding and all.
  memset(at, 0, sizeof(T));
  return new (at) T();
}

/*
 * Copy text into tree, NUL-terminated. The copy is the caller's to
 * change until it is put in a node.
 */
inline char *dup_string(sq_tree_t &tree, std::string_view text) {
  char *copy =
      static_cast<char *>(tree_access_t::store(tree).alloc(text.size() + 1, 1));
  memcpy(copy, text.data(), text.size());
  copy[text.size()] = '\0';
  return copy;
}

/* The calling thread's current tree: current_tree_t sets it. Constant
 * initialized, so reading it from another file is a plain TLS load. */
extern constinit thread_local sq_tree_t *t_current_tree;

/* Thrown from current_store() with no tree being built. */
[[noreturn]] void no_current_tree();

/*
 * The tree the calling thread is building. The grammar's actions make
 * nodes far from anything that could hand them the tree, as
 * PostgreSQL's allocate in its current memory context, so a parse
 * names its tree here for as long as it runs.
 */
inline sq_tree_t *current_tree() { return t_current_tree; }

/*
 * The store of the tree being built, where a list grows; throws
 * logic_error if no tree is being built.
 */
inline store_t &current_store() {
  if (t_current_tree == nullptr) {
    no_current_tree();
  }
  return tree_access_t::store(*t_current_tree);
}

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
