/*
 * tree.cc -- sq_tree_t and the store behind it.
 */

#include "tree.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <stdexcept>

namespace sq {

/* Free every block past the first. */
arena_t::~arena_t() {
  while (m_blocks != nullptr) {
    block_t *next = m_blocks->next;
    free(m_blocks);
    m_blocks = next;
  }
}

/* Start a block big enough for size bytes at align, and cut them from it. */
void *arena_t::grow(size_t size, size_t align) {
  // A block twice the last up to 1 MB, or as big as the piece asked.
  size_t need = sizeof(block_t) + align + size;
  size_t bytes = std::max(m_block_size, need);
  m_block_size = std::min(m_block_size * 2, size_t(1) << 20);
  auto *b = static_cast<block_t *>(malloc(bytes));
  if (b == nullptr) {
    throw std::bad_alloc();
  }
  *b = {m_blocks, bytes};
  m_blocks = b;
  m_next = reinterpret_cast<char *>(b + 1);
  m_end = reinterpret_cast<char *>(b) + bytes;
  return alloc(size, align);
}

/* Grow ptr by the caller's realloc, or by a copy in the arena. */
void *store_t::realloc(void *ptr, size_t old, size_t size) {
  if (allocator != nullptr) {
    void *p = allocator->realloc(allocator->arg, ptr, size);
    if (p == nullptr) {
      throw std::bad_alloc();
    }
    return p;
  }
  // The arena gives nothing back: the old piece stays until the tree goes.
  void *p = arena.alloc(size, alignof(std::max_align_t));
  memcpy(p, ptr, std::min(old, size));
  return p;
}

/* The calling thread's current tree (tree.h). */
constinit thread_local sq_tree_t *t_current_tree = nullptr;

/* A list changed with no tree being built is a bug. */
void no_current_tree() {
  throw std::logic_error("sq: a list changed with no tree being built");
}

/* Remember what was current and make tree current. */
current_tree_t::current_tree_t(sq_tree_t &tree) : m_previous(t_current_tree) {
  t_current_tree = &tree;
}

/* Restore what was current. */
current_tree_t::~current_tree_t() { t_current_tree = m_previous; }

} // namespace sq

/* An empty tree, with no store until something is made for it. */
sq_tree_t::sq_tree_t() = default;

/* Destroy the tree, and with it every node and all the text it owns. */
sq_tree_t::~sq_tree_t() { delete m_store; }

/* Take other's statements and store, leaving it empty. */
sq_tree_t::sq_tree_t(sq_tree_t &&other) noexcept
    : stmts(std::move(other.stmts)), m_store(other.m_store) {
  other.stmts.clear();
  other.m_store = nullptr;
}

/* Replace this tree with other's, freeing what this one owned. */
sq_tree_t &sq_tree_t::operator=(sq_tree_t &&other) noexcept {
  if (this != &other) {
    delete m_store;
    stmts = std::move(other.stmts);
    m_store = other.m_store;
    other.stmts.clear();
    other.m_store = nullptr;
  }
  return *this;
}
