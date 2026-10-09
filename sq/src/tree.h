/*
 * tree.h -- making the nodes and text a tree owns.
 *
 * A tree keeps a store of everything made for it and frees it all in
 * its destructor (sq.h). The parser makes every node with make_node
 * and copies every piece of text with dup_string, so that a parse
 * which fails halfway leaves nothing behind but the tree.
 */
#ifndef SQ_SRC_TREE_H
#define SQ_SRC_TREE_H

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "sq.h"

namespace sq {

/* What a tree owns: its nodes, each with how to delete it, and text. */
struct store_t {
  std::vector<std::pair<void *, void (*)(void *)>> nodes;
  std::vector<std::unique_ptr<char[]>> strings;

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
  // Room for the record first, so that nothing can throw between
  // making the node and recording it.
  store.nodes.reserve(store.nodes.size() + 1);
  T *node = new T();
  store.nodes.emplace_back(node, [](void *p) { delete static_cast<T *>(p); });
  return node;
}

/* Copy text into tree, NUL-terminated. */
const char *dup_string(sq_tree_t &tree, std::string_view text);

} // namespace sq

#endif /* SQ_SRC_TREE_H */
