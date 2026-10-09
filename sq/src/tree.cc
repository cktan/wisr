/*
 * tree.cc -- sq_tree_t and the store behind it.
 */

#include "tree.h"

#include <string.h>

namespace sq {

/* Delete every node, each as the type it was made. */
store_t::~store_t() {
  for (auto &[node, destroy] : nodes) {
    destroy(node);
  }
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
  auto copy = std::make_unique<char[]>(text.size() + 1);
  memcpy(copy.get(), text.data(), text.size());
  copy[text.size()] = '\0';
  store.strings.push_back(std::move(copy));
  return store.strings.back().get();
}

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
