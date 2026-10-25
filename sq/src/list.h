/*
 * list.h -- PostgreSQL's List, as the grammar's actions use it.
 *
 * A node's own list is an sq_items_t (sq_items.h). While
 * the grammar is building, a list is passed about by handle, as
 * PostgreSQL passes a List *: a list_ref_t, which is NIL or names an
 * sq_list_t in the tree. Assigning a handle to a node's field copies
 * the list into the field; the handle can then go on being used.
 *
 * The functions are PostgreSQL's pg_list.h, by its names, for a list
 * of nodes: the grammar builds no other kind.
 */
#ifndef SQ_SRC_LIST_H
#define SQ_SRC_LIST_H

#include <cstddef>

#include "sq_nodes.h"

namespace sq {

/*
 * A handle on a list being built. It is trivial to make and to copy,
 * so that bison can keep one in its union.
 */
struct list_ref_t {
  sq_list_t *list; /* NULL for the empty list, NIL */

  list_ref_t() = default;

  /* The handle on a list node, or NIL for NULL. */
  constexpr list_ref_t(sq_list_t *l) : list(l) {}

  /* The handle on a node known to be a list: PostgreSQL's (List *). */
  explicit list_ref_t(sq_node_t *node) : list(static_cast<sq_list_t *>(node)) {}

  /* A copy of the list's nodes, for a node's field. */
  operator sq_items_t() const { return list ? list->items : sq_items_t(); }

  /* The list as a node, for where any node may go; NULL for NIL. */
  operator sq_node_t *() const { return list; }

  /* Whether the list has anything in it. */
  explicit operator bool() const { return list != nullptr; }
};

/* The empty list. */
inline constexpr list_ref_t NIL = list_ref_t(static_cast<sq_list_t *>(nullptr));

/* Whether two handles name the same list. */
inline bool operator==(list_ref_t a, list_ref_t b) { return a.list == b.list; }
inline bool operator!=(list_ref_t a, list_ref_t b) { return a.list != b.list; }

/* A place in a list: PostgreSQL's ListCell *. */
typedef sq_node_t **list_cell_t;

/* The node at a place in a list. */
#define lfirst(cell) (*(cell))
#define lfirst_node(type, cell) (static_cast<type *>(lfirst(cell)))

/* How many nodes a list holds. */
inline int list_length(list_ref_t l) {
  return l.list ? static_cast<int>(l.list->items.size()) : 0;
}

/* The place of the nth node of l, from 0, or NULL past its end. */
inline list_cell_t list_nth_cell(list_ref_t l, int n) {
  return n < list_length(l) ? &l.list->items[n] : nullptr;
}

/* The place after cell in l, or NULL at its end. */
inline list_cell_t lnext(list_ref_t l, list_cell_t cell) {
  return list_nth_cell(l, static_cast<int>(cell - l.list->items.data()) + 1);
}

/*
 * Walk a list: cell is each place in turn. The list is asked for its
 * length each time round, so it may be appended to on the way.
 */
#define foreach(cell, lst)                                                     \
  for (int cell##__i = 0;                                                      \
       ((cell) = sq::list_nth_cell((lst), cell##__i)) != nullptr; cell##__i++)

/* The first, second, third and last nodes of a list that has them. */
inline sq_node_t *&linitial(list_ref_t l) { return l.list->items[0]; }
inline sq_node_t *&lsecond(list_ref_t l) { return l.list->items[1]; }
inline sq_node_t *&lthird(list_ref_t l) { return l.list->items[2]; }
inline sq_node_t *&llast(list_ref_t l) { return l.list->items.back(); }
#define linitial_node(type, l) (static_cast<type *>(sq::linitial(l)))
#define lsecond_node(type, l) (static_cast<type *>(sq::lsecond(l)))
#define llast_node(type, l) (static_cast<type *>(sq::llast(l)))

/* A new list of the nodes given. */
list_ref_t list_make1(sq_node_t *a);
list_ref_t list_make2(sq_node_t *a, sq_node_t *b);
list_ref_t list_make3(sq_node_t *a, sq_node_t *b, sq_node_t *c);
list_ref_t list_make4(sq_node_t *a, sq_node_t *b, sq_node_t *c, sq_node_t *d);

/* A new list holding a copy of a node's own list; NIL if it is empty. */
list_ref_t list_from(const sq_items_t &items);

/* Add node at the end of l, which may be NIL. Returns the list. */
list_ref_t lappend(list_ref_t l, sq_node_t *node);

/* Add node at the front of l, which may be NIL. Returns the list. */
list_ref_t lcons(sq_node_t *node, list_ref_t l);

/*
 * Add b's nodes at the end of a. Returns a, or a copy of b where a is
 * NIL: the result never shares b's storage.
 */
list_ref_t list_concat(list_ref_t a, list_ref_t b);

/* Add l's nodes at the end of a node's own list. */
void append_list(sq_items_t &items, list_ref_t l);

/* Cut l to its first n nodes. Returns l, or NIL when n is 0. */
list_ref_t list_truncate(list_ref_t l, int n);

/* A new list of l's nodes from the nskip'th on; NIL if there are none. */
list_ref_t list_copy_tail(list_ref_t l, int nskip);

} // namespace sq

#endif /* SQ_SRC_LIST_H */
