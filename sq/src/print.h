/*
 * print.h -- the pieces a node is printed with.
 *
 * The text is PostgreSQL's nodeToString(): its names, its escapes,
 * and "<>" for whatever is absent. src/print.cc has the pieces and
 * the nodes PostgreSQL prints by hand; the generated print_nodes.cc
 * has every other node.
 */
#ifndef SQ_SRC_PRINT_H
#define SQ_SRC_PRINT_H

#include <cstdint>
#include <string>
#include <vector>

#include "sq_nodes.h"

namespace sq {

/* Write a node of any kind, or "<>" for NULL. */
void out_node(std::string &s, const sq_node_t *node);

/* Write a node through a pointer of its own type. */
inline void out_typed(std::string &s, const sq_node_t *node) {
  out_node(s, node);
}

/* Write a list's nodes in parentheses, or "<>" for an empty list. */
void out_list(std::string &s, const std::vector<sq_node_t *> &items);

/* Write text as a token, escaped; "<>" for NULL and "" for empty. */
void out_string(std::string &s, const char *text);

/* Write one character as a token, or "<>" for NUL. */
void out_char(std::string &s, char c);

/* Write true or false. */
void out_bool(std::string &s, bool b);

/* Write a number in decimal. */
void out_int(std::string &s, int v);
void out_uint(std::string &s, uint32_t v);
void out_int64(std::string &s, int64_t v);
void out_long(std::string &s, long v);

/*
 * Write the label and fields of a node, if it is one the generated
 * printer knows. Returns whether it was.
 */
bool out_generated(std::string &s, const sq_node_t *node);

} // namespace sq

#endif /* SQ_SRC_PRINT_H */
