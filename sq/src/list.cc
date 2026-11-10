/*
 * list.cc -- PostgreSQL's list functions over sq_list_t, growing as
 * PostgreSQL's list.c grows a List.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for sq,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 */

#include "list.h"

#include "tree.h"

#include <string.h>

#include <algorithm>
#include <bit>

namespace sq {

namespace {

/* Cells a list's header takes the room of: list.c's LIST_HEADER_OVERHEAD. */
constexpr int header_cells =
    static_cast<int>((sizeof(sq_list_t) - 1) / sizeof(sq_list_cell_t) + 1);

/* The least power of two not below n: pg_nextpower2_32. */
int next_power2(int n) { return static_cast<int>(std::bit_ceil(unsigned(n))); }

/*
 * A new list of length cells, whose nodes the caller sets, with room
 * in place for more: list.c's new_list.
 */
sq_list_t *new_list(int length) {
  int max_length =
      next_power2(std::max(8, length + header_cells)) - header_cells;
  void *at = current_store().alloc(sizeof(sq_list_t) +
                                       max_length * sizeof(sq_list_cell_t),
                                   alignof(sq_list_t));
  sq_list_t *l = new (at) sq_list_t;
  l->tag = SQ_T_LIST;
  l->length = length;
  l->max_length = max_length;
  l->elements = l->initial_elements;
  return l;
}

/*
 * Room in l for min_length cells: list.c's enlarge_list. The cells in
 * place cannot be given back, so a list that outgrows them takes an
 * array of its own, which grows by realloc after.
 */
void enlarge_list(sq_list_t *l, int min_length) {
  int max_length = next_power2(std::max(16, min_length));
  store_t &store = current_store();
  if (l->elements == l->initial_elements) {
    auto *cells = static_cast<sq_list_cell_t *>(store.alloc(
        max_length * sizeof(sq_list_cell_t), alignof(sq_list_cell_t)));
    memcpy(cells, l->elements, l->length * sizeof(sq_list_cell_t));
    l->elements = cells;
  } else {
    l->elements = static_cast<sq_list_cell_t *>(
        store.realloc(l->elements, l->max_length * sizeof(sq_list_cell_t),
                      max_length * sizeof(sq_list_cell_t)));
  }
  l->max_length = max_length;
}

} // namespace

/* A list of one node. */
list_ref_t list_make1(sq_node_t *a) {
  sq_list_t *l = new_list(1);
  l->elements[0].ptr_value = a;
  return {l};
}

/* A list of two nodes. */
list_ref_t list_make2(sq_node_t *a, sq_node_t *b) {
  sq_list_t *l = new_list(2);
  l->elements[0].ptr_value = a;
  l->elements[1].ptr_value = b;
  return {l};
}

/* A list of three nodes. */
list_ref_t list_make3(sq_node_t *a, sq_node_t *b, sq_node_t *c) {
  sq_list_t *l = new_list(3);
  l->elements[0].ptr_value = a;
  l->elements[1].ptr_value = b;
  l->elements[2].ptr_value = c;
  return {l};
}

/* A list of four nodes. */
list_ref_t list_make4(sq_node_t *a, sq_node_t *b, sq_node_t *c, sq_node_t *d) {
  sq_list_t *l = new_list(4);
  l->elements[0].ptr_value = a;
  l->elements[1].ptr_value = b;
  l->elements[2].ptr_value = c;
  l->elements[3].ptr_value = d;
  return {l};
}

/* PostgreSQL's lappend. */
list_ref_t lappend(list_ref_t l, sq_node_t *node) {
  if (l == NIL) {
    return list_make1(node);
  }
  if (l.list->length >= l.list->max_length) {
    enlarge_list(l.list, l.list->length + 1);
  }
  l.list->elements[l.list->length++].ptr_value = node;
  return l;
}

/* PostgreSQL's lcons. */
list_ref_t lcons(sq_node_t *node, list_ref_t l) {
  if (l == NIL) {
    return list_make1(node);
  }
  if (l.list->length >= l.list->max_length) {
    enlarge_list(l.list, l.list->length + 1);
  }
  memmove(&l.list->elements[1], &l.list->elements[0],
          l.list->length * sizeof(sq_list_cell_t));
  l.list->elements[0].ptr_value = node;
  l.list->length++;
  return l;
}

/* PostgreSQL's list_concat. */
list_ref_t list_concat(list_ref_t a, list_ref_t b) {
  if (a == NIL) {
    return list_copy(b);
  }
  if (b == NIL) {
    return a;
  }
  // b's length and cells read after a grows: a may be b, whose cells
  // grow with it, and the copy's two ranges do not meet.
  int length = a.list->length + b.list->length;
  if (length > a.list->max_length) {
    enlarge_list(a.list, length);
  }
  memcpy(&a.list->elements[a.list->length], &b.list->elements[0],
         b.list->length * sizeof(sq_list_cell_t));
  a.list->length = length;
  return a;
}

/* PostgreSQL's list_copy. */
list_ref_t list_copy(list_ref_t l) {
  if (l == NIL) {
    return NIL;
  }
  sq_list_t *copy = new_list(l.list->length);
  memcpy(copy->elements, l.list->elements,
         l.list->length * sizeof(sq_list_cell_t));
  return {copy};
}

/* PostgreSQL's list_truncate. */
list_ref_t list_truncate(list_ref_t l, int n) {
  if (n <= 0) {
    return NIL;
  }
  if (n < list_length(l)) {
    l.list->length = n;
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
  sq_list_t *tail = new_list(l.list->length - nskip);
  memcpy(tail->elements, &l.list->elements[nskip],
         tail->length * sizeof(sq_list_cell_t));
  return {tail};
}

} // namespace sq
