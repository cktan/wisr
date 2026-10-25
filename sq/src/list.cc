/*
 * list.cc -- PostgreSQL's list functions over sq_list_t.
 */

#include "list.h"

#include "tree.h"

#include <string.h>

#include <algorithm>

namespace sq {

/* A list of one node. */
list_ref_t list_make1(sq_node_t *a) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items.push_back(a);
  return {l};
}

/* A list of two nodes. */
list_ref_t list_make2(sq_node_t *a, sq_node_t *b) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items.push_back(a);
  l->items.push_back(b);
  return {l};
}

/* A list of three nodes. */
list_ref_t list_make3(sq_node_t *a, sq_node_t *b, sq_node_t *c) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items.push_back(a);
  l->items.push_back(b);
  l->items.push_back(c);
  return {l};
}

/* A list of four nodes. */
list_ref_t list_make4(sq_node_t *a, sq_node_t *b, sq_node_t *c, sq_node_t *d) {
  sq_list_t *l = make_node<sq_list_t>();
  l->items.push_back(a);
  l->items.push_back(b);
  l->items.push_back(c);
  l->items.push_back(d);
  return {l};
}

/* A list holding a copy of items, or NIL. */
list_ref_t list_from(const sq_items_t &items) {
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
  l.list->items.push_front(node);
  return l;
}

/* PostgreSQL's list_concat. */
list_ref_t list_concat(list_ref_t a, list_ref_t b) {
  if (a == NIL) {
    return b == NIL ? NIL : list_from(b.list->items);
  }
  if (b != NIL) {
    // a and b may be the same list, which append allows.
    a.list->items.append(b.list->items);
  }
  return a;
}

/* PostgreSQL's list_concat, onto a node's own list. */
void append_list(sq_items_t &items, list_ref_t l) {
  if (l != NIL) {
    items.append(l.list->items);
  }
}

/* PostgreSQL's list_truncate. */
list_ref_t list_truncate(list_ref_t l, int n) {
  if (n <= 0) {
    return NIL;
  }
  if (n < list_length(l)) {
    l.list->items.truncate(n);
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
  for (int i = nskip; i < list_length(l); i++) {
    tail->items.push_back(l.list->items[i]);
  }
  return {tail};
}

} // namespace sq

/* Room for n nodes, in an array cut anew from the current tree's arena. */
void sq_items_t::reserve(size_t n) {
  if (n <= m_cap) {
    return;
  }
  // Twice the room, at least four, as PostgreSQL's List grows; the old
  // array stays in the arena until the tree goes.
  size_t cap = std::max<size_t>(n, m_cap ? 2 * size_t(m_cap) : 4);
  auto **items = static_cast<sq_node_t **>(sq::current_arena().alloc(
      cap * sizeof(sq_node_t *), alignof(sq_node_t *)));
  if (m_size > 0) {
    memcpy(items, m_items, m_size * sizeof(sq_node_t *));
  }
  m_items = items;
  m_cap = static_cast<uint32_t>(cap);
}

/* Add node at the end. */
void sq_items_t::push_back(sq_node_t *node) {
  reserve(size_t(m_size) + 1);
  m_items[m_size++] = node;
}

/* Add node at the front, moving the rest up. */
void sq_items_t::push_front(sq_node_t *node) {
  reserve(size_t(m_size) + 1);
  memmove(m_items + 1, m_items, m_size * sizeof(sq_node_t *));
  m_items[0] = node;
  m_size++;
}

/* Add other's nodes at the end; read before any growth, as other may be this.
 */
void sq_items_t::append(const sq_items_t &other) {
  size_t n = other.m_size;
  if (n == 0) {
    return;
  }
  sq_node_t *const *from = other.m_items;
  reserve(size_t(m_size) + n);
  memcpy(m_items + m_size, from, n * sizeof(sq_node_t *));
  m_size += static_cast<uint32_t>(n);
}
