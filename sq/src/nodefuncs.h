/*
 * nodefuncs.h -- making and inspecting nodes, as the grammar's
 * actions do.
 *
 * These are PostgreSQL's, by sq's names: the constructors of its
 * makefuncs.c and value.c that the grammar calls, and the few
 * functions of nodeFuncs.c, define.c and namespace.c it calls beside
 * them. Each makes its nodes in the tree being built (tree.h).
 */
#ifndef SQ_SRC_NODEFUNCS_H
#define SQ_SRC_NODEFUNCS_H

#include <format>
#include <string>

#include "list.h"
#include "sq_nodes.h"
#include "tree.h"

namespace sq {

/* Whether node, which is not NULL, has the tag given. */
#define is_a(node, t) (static_cast<const sq_node_t *>(node)->tag == (t))

/* node as a pointer to the type it is known to be. */
#define cast_node(type, node) (static_cast<type *>(node))

/* What a value node holds. */
#define int_val(v) (static_cast<sq_integer_t *>(v)->ival)
#define float_val(v) (atof(static_cast<sq_float_t *>(v)->fval))
#define bool_val(v) (static_cast<sq_boolean_t *>(v)->boolval)
#define str_val(v) (static_cast<sq_string_t *>(v)->sval)

/* Text formatted with std::format, in the tree: PostgreSQL's psprintf. */
template <typename... Args>
char *tree_format(std::format_string<Args...> fmt, Args &&...args) {
  return tree_strdup(std::format(fmt, std::forward<Args>(args)...));
}

/* PostgreSQL's copyObject(): a deep copy, in the tree being built. */
sq_node_t *copy_node(const sq_node_t *node);

/* PostgreSQL's equal(): the same but for where each was written. */
bool equal_nodes(const sq_node_t *a, const sq_node_t *b);

/* ---- value.c ---- */

sq_integer_t *make_integer(int i);
sq_float_t *make_float(const char *numeric_str);
sq_boolean_t *make_boolean(bool val);
sq_string_t *make_string(const char *str);
sq_bit_string_t *make_bit_string(const char *str);

/* ---- makefuncs.c ---- */

sq_a_expr_t *make_a_expr(sq_a_expr_kind_t kind, list_ref_t name,
                         sq_node_t *lexpr, sq_node_t *rexpr, int location);
sq_a_expr_t *make_simple_a_expr(sq_a_expr_kind_t kind, const char *name,
                                sq_node_t *lexpr, sq_node_t *rexpr,
                                int location);
sq_alias_t *make_alias(const char *aliasname, list_ref_t colnames);
sq_range_var_t *make_range_var(const char *schemaname, const char *relname,
                               int location);
sq_type_name_t *make_type_name(const char *typnam);
sq_type_name_t *make_type_name_from_name_list(list_ref_t names);
sq_node_t *make_bool_expr(sq_bool_expr_type_t boolop, list_ref_t args,
                          int location);
sq_def_elem_t *make_def_elem(const char *name, sq_node_t *arg, int location);
sq_def_elem_t *make_def_elem_extended(const char *name_space, const char *name,
                                      sq_node_t *arg,
                                      sq_def_elem_action_t defaction,
                                      int location);
sq_func_call_t *make_func_call(list_ref_t name, list_ref_t args,
                               sq_coercion_form_t funcformat, int location);
sq_grouping_set_t *make_grouping_set(sq_grouping_set_kind_t kind,
                                     list_ref_t content, int location);
sq_vacuum_relation_t *make_vacuum_relation(sq_range_var_t *relation,
                                           uint32_t oid, list_ref_t va_cols);
sq_json_format_t *make_json_format(sq_json_format_type_t type,
                                   sq_json_encoding_t encoding, int location);
sq_json_value_expr_t *make_json_value_expr(sq_node_t *raw_expr,
                                           sq_node_t *formatted_expr,
                                           sq_json_format_t *format);
sq_json_behavior_t *make_json_behavior(sq_json_behavior_type_t btype,
                                       sq_node_t *expr, int location);
sq_node_t *make_json_key_value(sq_node_t *key, sq_node_t *value);
sq_node_t *make_json_is_predicate(sq_node_t *expr, sq_json_format_t *format,
                                  sq_json_value_type_t item_type,
                                  bool unique_keys, int location);
sq_node_t *make_string_const(const char *str, int location);
sq_json_table_path_spec_t *make_json_table_path_spec(const char *string,
                                                     const char *name,
                                                     int string_location,
                                                     int name_location);

/* ---- namespace.c, define.c, nodeFuncs.c ---- */

/* A dotted name list as one string, for a message. */
char *name_list_to_string(list_ref_t names);

/* The integer an option was given, or an error if it was given none. */
int32_t def_get_int32(sq_def_elem_t *def);

/*
 * Where in the text an expression, or a list of them, begins: the
 * leftmost location of anything in it, or -1.
 */
int expr_location(const sq_node_t *expr);

} // namespace sq

#endif /* SQ_SRC_NODEFUNCS_H */
