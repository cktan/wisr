/*
 * sq_convert.h
 *	  The parser's tree as PostgreSQL's nodes.
 *
 * digres parses with sq (../sq), which builds a tree of its own
 * nodes. The analyzer reads PostgreSQL's. sq_convert() makes the
 * second from the first; parser.c is its one caller.
 *
 * src/include/parser/sq_convert.h
 */
#ifndef SQ_CONVERT_H
#define SQ_CONVERT_H

#include "nodes/nodes.h"

struct sq_node_t;

/*
 * PostgreSQL's node for one of sq's, and for everything under it, made
 * in the current memory context. NULL for NULL; a List for a list.
 */
extern Node *sq_convert(const struct sq_node_t *node);

#endif /* SQ_CONVERT_H */
