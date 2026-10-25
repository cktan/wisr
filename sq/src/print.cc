/*
 * print.cc -- sq_to_string: a node as text, as PostgreSQL's
 * nodeToString() writes it.
 *
 * This file is PostgreSQL's outfuncs.c, as far as a parse tree needs
 * it: the token escapes, lists, and the nodes it prints by hand. The
 * rest are in the generated print_nodes.cc.
 */

#include "print.h"

#include <cctype>
#include <new>

#include "error.h"
#include "sq.h"

namespace {

/* Whether the calling thread's print is to show locations. */
thread_local bool t_write_locations = false;

} // namespace

namespace sq {

/* Write a location, or -1 in its place. */
void out_location(std::string &s, int location) {
  out_int(s, t_write_locations ? location : -1);
}

/* Write text as a token that PostgreSQL's node reader would read back. */
void out_string(std::string &s, const char *text) {
  if (text == nullptr) {
    s += "<>";
    return;
  }
  if (*text == '\0') {
    s += "\"\"";
    return;
  }
  // These need a backslash only at the start of a token.
  unsigned char c0 = static_cast<unsigned char>(text[0]);
  unsigned char c1 = static_cast<unsigned char>(text[1]);
  if (c0 == '<' || c0 == '"' || isdigit(c0) ||
      ((c0 == '+' || c0 == '-') && (isdigit(c1) || c1 == '.'))) {
    s += '\\';
  }
  // These need one anywhere.
  for (const char *p = text; *p; p++) {
    if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '(' || *p == ')' ||
        *p == '{' || *p == '}' || *p == '\\') {
      s += '\\';
    }
    s += *p;
  }
}

/* Write one character as a token. */
void out_char(std::string &s, char c) {
  if (c == '\0') {
    s += "<>";
    return;
  }
  const char text[2] = {c, '\0'};
  out_string(s, text);
}

/* Write a boolean as PostgreSQL spells it. */
void out_bool(std::string &s, bool b) { s += b ? "true" : "false"; }

/* Write an int. */
void out_int(std::string &s, int v) { s += std::to_string(v); }

/* Write an unsigned 32-bit number. */
void out_uint(std::string &s, uint32_t v) { s += std::to_string(v); }

/* Write a 64-bit number. */
void out_int64(std::string &s, int64_t v) { s += std::to_string(v); }

/* Write a long. */
void out_long(std::string &s, long v) { s += std::to_string(v); }

/* Write a list: its nodes, one space apart, in parentheses. */
void out_list(std::string &s, const sq_items_t &items) {
  // PostgreSQL's empty list is NIL, a null pointer.
  if (items.empty()) {
    s += "<>";
    return;
  }
  s += '(';
  for (size_t i = 0; i < items.size(); i++) {
    if (i > 0) {
      s += ' ';
    }
    out_node(s, items[i]);
  }
  s += ')';
}

namespace {

/* Write an A_Const: NULL, or its value node. */
void out_a_const(std::string &s, const sq_a_const_t &n) {
  s += "A_CONST";
  if (n.isnull) {
    s += " NULL";
  } else {
    s += " :val ";
    out_node(s, n.val);
  }
  s += " :location ";
  out_location(s, n.location);
}

/* The word PostgreSQL prints before an A_Expr's name, for its kind. */
const char *a_expr_kind_word(sq_a_expr_kind_t kind) {
  switch (kind) {
  case SQ_AEXPR_OP:
    return "";
  case SQ_AEXPR_OP_ANY:
    return " ANY";
  case SQ_AEXPR_OP_ALL:
    return " ALL";
  case SQ_AEXPR_DISTINCT:
    return " DISTINCT";
  case SQ_AEXPR_NOT_DISTINCT:
    return " NOT_DISTINCT";
  case SQ_AEXPR_NULLIF:
    return " NULLIF";
  case SQ_AEXPR_IN:
    return " IN";
  case SQ_AEXPR_LIKE:
    return " LIKE";
  case SQ_AEXPR_ILIKE:
    return " ILIKE";
  case SQ_AEXPR_SIMILAR:
    return " SIMILAR";
  case SQ_AEXPR_BETWEEN:
    return " BETWEEN";
  case SQ_AEXPR_NOT_BETWEEN:
    return " NOT_BETWEEN";
  case SQ_AEXPR_BETWEEN_SYM:
    return " BETWEEN_SYM";
  case SQ_AEXPR_NOT_BETWEEN_SYM:
    return " NOT_BETWEEN_SYM";
  }
  return "";
}

/* Write an A_Expr: its kind as a word, not a number. */
void out_a_expr(std::string &s, const sq_a_expr_t &n) {
  s += "A_EXPR";
  s += a_expr_kind_word(n.kind);
  s += " :name ";
  out_list(s, n.name);
  s += " :lexpr ";
  out_node(s, n.lexpr);
  s += " :rexpr ";
  out_node(s, n.rexpr);
  s += " :rexpr_list_start ";
  out_location(s, n.rexpr_list_start);
  s += " :rexpr_list_end ";
  out_location(s, n.rexpr_list_end);
  s += " :location ";
  out_location(s, n.location);
}

/* Write a BoolExpr: its operator as a word, not a number. */
void out_bool_expr(std::string &s, const sq_bool_expr_t &n) {
  s += "BOOLEXPR :boolop ";
  switch (n.boolop) {
  case SQ_AND_EXPR:
    s += "and";
    break;
  case SQ_OR_EXPR:
    s += "or";
    break;
  case SQ_NOT_EXPR:
    s += "not";
    break;
  }
  s += " :args ";
  out_list(s, n.args);
  s += " :location ";
  out_location(s, n.location);
}

} // namespace

/* Write any node: a list, a bare value, or a labelled node in braces. */
void out_node(std::string &s, const sq_node_t *node) {
  if (node == nullptr) {
    s += "<>";
    return;
  }
  switch (node->tag) {
  case SQ_T_LIST:
    out_list(s, static_cast<const sq_list_t *>(node)->items);
    return;
  case SQ_T_INTEGER:
    out_int(s, static_cast<const sq_integer_t *>(node)->ival);
    return;
  case SQ_T_FLOAT:
    // A float is kept as the text it was written in.
    s += static_cast<const sq_float_t *>(node)->fval;
    return;
  case SQ_T_BOOLEAN:
    out_bool(s, static_cast<const sq_boolean_t *>(node)->boolval);
    return;
  case SQ_T_STRING: {
    const char *sval = static_cast<const sq_string_t *>(node)->sval;
    s += '"';
    if (sval[0] != '\0') {
      out_string(s, sval);
    }
    s += '"';
    return;
  }
  case SQ_T_BIT_STRING:
    out_string(s, static_cast<const sq_bit_string_t *>(node)->bsval);
    return;
  default:
    break;
  }
  s += '{';
  switch (node->tag) {
  case SQ_T_A_CONST:
    out_a_const(s, *static_cast<const sq_a_const_t *>(node));
    break;
  case SQ_T_A_EXPR:
    out_a_expr(s, *static_cast<const sq_a_expr_t *>(node));
    break;
  case SQ_T_BOOL_EXPR:
    out_bool_expr(s, *static_cast<const sq_bool_expr_t *>(node));
    break;
  default:
    if (!out_generated(s, node)) {
      s += "UNKNOWN";
    }
    break;
  }
  s += '}';
}

} // namespace sq

/* Write node into out, with or without its locations. */
static int to_string(const sq_node_t *node, std::string &out, bool locations) {
  try {
    out.clear();
    t_write_locations = locations;
    sq::out_node(out, node);
    t_write_locations = false;
    return 0;
  } catch (const std::bad_alloc &) {
    t_write_locations = false;
    out.clear();
    return sq::set_error(SQ_ERR_NOMEM, sq::sqlstate_out_of_memory, 0,
                         "out of memory");
  } catch (...) {
    t_write_locations = false;
    out.clear();
    return sq::set_error(SQ_ERR_EXCEPTION, sq::sqlstate_internal_error, 0,
                         "unexpected exception in sq_to_string");
  }
}

/* Write node into out as PostgreSQL's nodeToString() would. */
int sq_to_string(const sq_node_t *node, std::string &out) {
  return to_string(node, out, false);
}

/* Write node into out as PostgreSQL's nodeToStringWithLocations() would. */
int sq_to_string_with_locations(const sq_node_t *node, std::string &out) {
  return to_string(node, out, true);
}
