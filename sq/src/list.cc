/*
 * list.cc -- PostgreSQL's list functions over sq_list_t.
 */

#include "list.h"

#include "tree.h"

namespace sq {

/* A list of one node. */
list_ref_t list_make1(sq_node_t *a) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items = {a};
  return {l};
}

/* A list of two nodes. */
list_ref_t list_make2(sq_node_t *a, sq_node_t *b) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items = {a, b};
  return {l};
}

/* A list of three nodes. */
list_ref_t list_make3(sq_node_t *a, sq_node_t *b, sq_node_t *c) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items = {a, b, c};
  return {l};
}

/* A list of four nodes. */
list_ref_t list_make4(sq_node_t *a, sq_node_t *b, sq_node_t *c, sq_node_t *d) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items = {a, b, c, d};
  return {l};
}

/* A list holding a copy of items, or NIL. */
list_ref_t list_from(const std::vector<sq_node_t *> &items) {
  if (items.empty()) {
    return NIL;
  }
  sq_list_t *l = make_node<sq_list_t>();
  l->items = items;
  return {l};
}

/* PostgreSQL's lappend. */
list_ref_t lappend(list_ref_t l, sq_node_t *node) {
  if (l == NIL) {
    return list_make1(node);
  }
  l.list->items.push_back(node);
  return l;
}

/* PostgreSQL's lcons. */
list_ref_t lcons(sq_node_t *node, list_ref_t l) {
  if (l == NIL) {
    return list_make1(node);
  }
  l.list->items.insert(l.list->items.begin(), node);
  return l;
}

/* PostgreSQL's list_concat. */
list_ref_t list_concat(list_ref_t a, list_ref_t b) {
  if (a == NIL) {
    return b == NIL ? NIL : list_from(b.list->items);
  }
  if (b != NIL) {
    // Copied first: a and b may be the same list.
    std::vector<sq_node_t *> more = b.list->items;
    a.list->items.insert(a.list->items.end(), more.begin(), more.end());
  }
  return a;
}

/* PostgreSQL's list_concat, onto a node's own list. */
void append_list(std::vector<sq_node_t *> &items, list_ref_t l) {
  if (l != NIL) {
    items.insert(items.end(), l.list->items.begin(), l.list->items.end());
  }
}

/* PostgreSQL's list_truncate. */
list_ref_t list_truncate(list_ref_t l, int n) {
  if (n <= 0) {
    return NIL;
  }
  if (n < list_length(l)) {
    l.list->items.resize(n);
  }
  return l;
}

/* PostgreSQL's list_copy_tail. */
list_ref_t list_copy_tail(list_ref_t l, int nskip) {
  if (nskip < 0) {
    nskip = 0;
  }
  if (nskip >= list_length(l)) {
    return NIL;
  }
  sq_list_t *tail = make_node<sq_list_t>();
  tail->items.assign(l.list->items.begin() + nskip, l.list->items.end());
  return {tail};
}

} // namespace sq
