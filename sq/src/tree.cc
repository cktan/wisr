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

/* Cut size bytes from the caller's allocator, or from the arena. */
void *store_t::alloc(size_t size, size_t align) {
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

/* Return tree's store, making it if this is the first use. */
store_t &tree_access_t::store(sq_tree_t &tree) {
  if (tree.m_store == nullptr) {
    tree.m_store = new store_t();
  }
  return *tree.m_store;
}

/* Copy text into tree's store and return the copy. */
char *dup_string(sq_tree_t &tree, std::string_view text) {
  store_t &store = tree_access_t::store(tree);
  char *copy = static_cast<char *>(store.alloc(text.size() + 1, 1));
  memcpy(copy, text.data(), text.size());
  copy[text.size()] = '\0';
  return copy;
}

namespace {
thread_local sq_tree_t *t_current_tree = nullptr;
} // namespace

/* The tree the calling thread is building, or NULL outside a parse. */
sq_tree_t *current_tree() { return t_current_tree; }

/* The current tree's store; a list changed with no tree is a bug. */
store_t &current_store() {
  if (t_current_tree == nullptr) {
    throw std::logic_error("sq: a list changed with no tree being built");
  }
  return tree_access_t::store(*t_current_tree);
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
