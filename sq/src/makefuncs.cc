/*
 * makefuncs.cc -- PostgreSQL's node constructors and the helpers the
 * grammar calls beside them (nodefuncs.h).
 *
 * The functions are PostgreSQL 18.6's, from its makefuncs.c, value.c,
 * namespace.c, define.c and nodeFuncs.c, with sq's names and sq's
 * nodes. What each does is what PostgreSQL's does.
 */

#include "nodefuncs.h"

#include <stdlib.h>
#include <string.h>

#include <cassert>

#include "error.h"
#include "pg_consts.h"

namespace sq {

/*
 * makeA_Expr -
 *		makes an A_Expr node
 */
sq_a_expr_t *make_a_expr(sq_a_expr_kind_t kind, list_ref_t name,
                         sq_node_t *lexpr, sq_node_t *rexpr, int location) {
  sq_a_expr_t *a = make_node<sq_a_expr_t>();

  a->kind = kind;
  a->name = name;
  a->lexpr = lexpr;
  a->rexpr = rexpr;
  a->location = location;
  return a;
}

/*
 * makeSimpleA_Expr -
 *		As above, given a simple (unqualified) operator name
 */
sq_a_expr_t *make_simple_a_expr(sq_a_expr_kind_t kind, const char *name,
                                sq_node_t *lexpr, sq_node_t *rexpr,
                                int location) {
  sq_a_expr_t *a = make_node<sq_a_expr_t>();

  a->kind = kind;
  a->name = list_make1(make_string(name));
  a->lexpr = lexpr;
  a->rexpr = rexpr;
  a->location = location;
  return a;
}

/*
 * makeAlias -
 *	  creates an Alias node
 *
 * NOTE: the given name is copied, but the colnames list (if any) isn't.
 */
sq_alias_t *make_alias(const char *aliasname, list_ref_t colnames) {
  sq_alias_t *a = make_node<sq_alias_t>();

  a->aliasname = tree_strdup(aliasname);
  a->colnames = colnames;

  return a;
}

/*
 * makeRangeVar -
 *	  creates a RangeVar node (rather oversimplified case)
 */
sq_range_var_t *make_range_var(const char *schemaname, const char *relname,
                               int location) {
  sq_range_var_t *r = make_node<sq_range_var_t>();

  r->catalogname = NULL;
  r->schemaname = schemaname;
  r->relname = relname;
  r->inh = true;
  r->relpersistence = RELPERSISTENCE_PERMANENT;
  r->alias = NULL;
  r->location = location;

  return r;
}

/*
 * makeTypeName -
 *	build a TypeName node for an unqualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
sq_type_name_t *make_type_name(const char *typnam) {
  return make_type_name_from_name_list(list_make1(make_string(typnam)));
}

/*
 * makeTypeNameFromNameList -
 *	build a TypeName node for a String list representing a qualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
sq_type_name_t *make_type_name_from_name_list(list_ref_t names) {
  sq_type_name_t *n = make_node<sq_type_name_t>();

  n->names = names;
  n->typmods = NIL;
  n->typemod = -1;
  n->location = -1;
  return n;
}

/*
 * makeBoolExpr -
 *	  creates a BoolExpr node
 */
sq_node_t *make_bool_expr(sq_bool_expr_type_t boolop, list_ref_t args,
                          int location) {
  sq_bool_expr_t *b = make_node<sq_bool_expr_t>();

  b->boolop = boolop;
  b->args = args;
  b->location = location;

  return (sq_node_t *)b;
}

/*
 * makeDefElem -
 *	build a DefElem node
 *
 * This is sufficient for the "typical" case with an unqualified option name
 * and no special action.
 */
sq_def_elem_t *make_def_elem(const char *name, sq_node_t *arg, int location) {
  sq_def_elem_t *res = make_node<sq_def_elem_t>();

  res->defnamespace = NULL;
  res->defname = name;
  res->arg = arg;
  res->defaction = SQ_DEFELEM_UNSPEC;
  res->location = location;

  return res;
}

/*
 * makeDefElemExtended -
 *	build a DefElem node with all fields available to be specified
 */
sq_def_elem_t *make_def_elem_extended(const char *name_space, const char *name,
                                      sq_node_t *arg,
                                      sq_def_elem_action_t defaction,
                                      int location) {
  sq_def_elem_t *res = make_node<sq_def_elem_t>();

  res->defnamespace = name_space;
  res->defname = name;
  res->arg = arg;
  res->defaction = defaction;
  res->location = location;

  return res;
}

/*
 * makeFuncCall -
 *
 * Initialize a FuncCall struct with the information every caller must
 * supply.  Any non-default parameters have to be inserted by the caller.
 */
sq_func_call_t *make_func_call(list_ref_t name, list_ref_t args,
                               sq_coercion_form_t funcformat, int location) {
  sq_func_call_t *n = make_node<sq_func_call_t>();

  n->funcname = name;
  n->args = args;
  n->agg_order = NIL;
  n->agg_filter = NULL;
  n->over = NULL;
  n->agg_within_group = false;
  n->agg_star = false;
  n->agg_distinct = false;
  n->func_variadic = false;
  n->funcformat = funcformat;
  n->location = location;
  return n;
}

/*
 * makeGroupingSet
 *
 */
sq_grouping_set_t *make_grouping_set(sq_grouping_set_kind_t kind,
                                     list_ref_t content, int location) {
  sq_grouping_set_t *n = make_node<sq_grouping_set_t>();

  n->kind = kind;
  n->content = content;
  n->location = location;
  return n;
}

/*
 * makeVacuumRelation -
 *	  create a VacuumRelation node
 */
sq_vacuum_relation_t *make_vacuum_relation(sq_range_var_t *relation,
                                           uint32_t oid, list_ref_t va_cols) {
  sq_vacuum_relation_t *v = make_node<sq_vacuum_relation_t>();

  v->relation = relation;
  v->oid = oid;
  v->va_cols = va_cols;
  return v;
}

/*
 * makeJsonFormat -
 *	  creates a JsonFormat node
 */
sq_json_format_t *make_json_format(sq_json_format_type_t type,
                                   sq_json_encoding_t encoding, int location) {
  sq_json_format_t *jf = make_node<sq_json_format_t>();

  jf->format_type = type;
  jf->encoding = encoding;
  jf->location = location;

  return jf;
}

/*
 * makeJsonValueExpr -
 *	  creates a JsonValueExpr node
 */
sq_json_value_expr_t *make_json_value_expr(sq_node_t *raw_expr,
                                           sq_node_t *formatted_expr,
                                           sq_json_format_t *format) {
  sq_json_value_expr_t *jve = make_node<sq_json_value_expr_t>();

  jve->raw_expr = raw_expr;
  jve->formatted_expr = formatted_expr;
  jve->format = format;

  return jve;
}

/*
 * makeJsonBehavior -
 *	  creates a JsonBehavior node
 */
sq_json_behavior_t *make_json_behavior(sq_json_behavior_type_t btype,
                                       sq_node_t *expr, int location) {
  sq_json_behavior_t *behavior = make_node<sq_json_behavior_t>();

  behavior->btype = btype;
  behavior->expr = expr;
  behavior->location = location;

  return behavior;
}

/*
 * makeJsonKeyValue -
 *	  creates a JsonKeyValue node
 */
sq_node_t *make_json_key_value(sq_node_t *key, sq_node_t *value) {
  sq_json_key_value_t *n = make_node<sq_json_key_value_t>();

  n->key = (sq_node_t *)key;
  n->value = cast_node(sq_json_value_expr_t, value);

  return (sq_node_t *)n;
}

/*
 * makeJsonIsPredicate -
 *	  creates a JsonIsPredicate node
 */
sq_node_t *make_json_is_predicate(sq_node_t *expr, sq_json_format_t *format,
                                  sq_json_value_type_t item_type,
                                  bool unique_keys, int location) {
  sq_json_is_predicate_t *n = make_node<sq_json_is_predicate_t>();

  assert(expr != NULL);

  n->expr = expr;
  n->format = format;
  n->item_type = item_type;
  n->unique_keys = unique_keys;
  n->location = location;

  return (sq_node_t *)n;
}

/*
 * makeStringConst -
 * 	build a A_Const node of type T_String for given string
 */
sq_node_t *make_string_const(const char *str, int location) {
  sq_a_const_t *n = make_node<sq_a_const_t>();

  n->val.sval.tag = SQ_T_STRING;
  n->val.sval.sval = str;
  n->location = location;

  return (sq_node_t *)n;
}

/*
 * makeJsonTablePathSpec -
 *		Make JsonTablePathSpec node from given path string and name (if
 *any)
 */
sq_json_table_path_spec_t *make_json_table_path_spec(const char *string,
                                                     const char *name,
                                                     int string_location,
                                                     int name_location) {
  sq_json_table_path_spec_t *pathspec = make_node<sq_json_table_path_spec_t>();

  assert(string != NULL);
  pathspec->string = make_string_const(string, string_location);
  if (name != NULL)
    pathspec->name = tree_strdup(name);

  pathspec->name_location = name_location;
  pathspec->location = string_location;

  return pathspec;
}

/*
 *	makeInteger
 */
sq_integer_t *make_integer(int i) {
  sq_integer_t *v = make_node<sq_integer_t>();

  v->ival = i;
  return v;
}

/*
 *	makeFloat
 *
 * Caller is responsible for passing a palloc'd string.
 */
sq_float_t *make_float(const char *numeric_str) {
  sq_float_t *v = make_node<sq_float_t>();

  v->fval = numeric_str;
  return v;
}

/*
 *	makeBoolean
 */
sq_boolean_t *make_boolean(bool val) {
  sq_boolean_t *v = make_node<sq_boolean_t>();

  v->boolval = val;
  return v;
}

/*
 *	makeString
 *
 * Caller is responsible for passing a palloc'd string.
 */
sq_string_t *make_string(const char *str) {
  sq_string_t *v = make_node<sq_string_t>();

  v->sval = str;
  return v;
}

/*
 *	makeBitString
 *
 * Caller is responsible for passing a palloc'd string.
 */
sq_bit_string_t *make_bit_string(const char *str) {
  sq_bit_string_t *v = make_node<sq_bit_string_t>();

  v->bsval = str;
  return v;
}

/*
 * NameListToString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * This is used primarily to form error messages, and so we do not quote
 * the list elements, for the sake of legibility.
 *
 * In most scenarios the list elements should always be String values,
 * but we also allow A_Star for the convenience of ColumnRef processing.
 */
char *name_list_to_string(list_ref_t names) {
  std::string string;
  list_cell_t l;

  foreach (l, names) {
    sq_node_t *name = (sq_node_t *)lfirst(l);

    if (!string.empty() || l != list_nth_cell(names, 0))
      string += '.';

    if (is_a(name, SQ_T_STRING))
      string += str_val(name);
    else if (is_a(name, SQ_T_A_STAR))
      string += '*';
    else
      raise({.sqlstate = sqlstate_internal_error},
            "unexpected node type in name list: {}", (int)(name)->tag);
  }

  return tree_strdup(string);
}

/*
 * Extract an int32 value from a DefElem.
 */
int32_t def_get_int32(sq_def_elem_t *def) {
  if (def->arg == NULL)
    raise({.sqlstate = sqlstate_syntax_error}, "{} requires an integer value",
          def->defname);
  switch ((def->arg)->tag) {
  case SQ_T_INTEGER:
    return (int32_t)int_val(def->arg);
  default:
    raise({.sqlstate = sqlstate_syntax_error}, "{} requires an integer value",
          def->defname);
  }
  return 0; /* keep compiler quiet */
}

/*
 * leftmostLoc - support for exprLocation
 *
 * Take the minimum of two parse location values, but ignore unknowns
 */
static int leftmost_loc(int loc1, int loc2) {
  if (loc1 < 0)
    return loc2;
  else if (loc2 < 0)
    return loc1;
  else
    return loc1 < loc2 ? loc1 : loc2;
}

/* The location of the first node of a list that has one, as for a List. */
static int list_location(const sq_list_t *items) {
  for (int i = 0; i < list_length(items); i++) {
    int loc = expr_location(items->elements[i].ptr_value);
    if (loc >= 0) {
      return loc;
    }
  }
  return -1;
}

/*
 *	exprLocation -
 *	  returns the parse location of an expression tree, for error reports
 *
 * -1 is returned if the location can't be determined.
 *
 * For expressions larger than a single token, the intent here is to
 * return the location of the expression's leftmost token, not necessarily
 * the topmost Node's location field.  For example, an OpExpr's location
 * field will point at the operator name, but if it is not a prefix operator
 * then we should return the location of the left-hand operand instead.
 * The reason is that we want to reference the entire expression not just
 * that operator, and pointing to its start seems to be the most natural way.
 *
 * The location is not perfect --- for example, since the grammar doesn't
 * explicitly represent parentheses in the parsetree, given something that
 * had been written "(a + b) * c" we are going to point at "a" not "(".
 * But it should be plenty good enough for error reporting purposes.
 *
 * You might think that this code is overly general, for instance why check
 * the operands of a FuncExpr node, when the function name can be expected
 * to be to the left of them?  There are a couple of reasons.  The grammar
 * sometimes builds expressions that aren't quite what the user wrote;
 * for instance x IS NOT BETWEEN ... becomes a NOT-expression whose keyword
 * pointer is to the right of its leftmost argument.  Also, nodes that were
 * inserted implicitly by parse analysis (such as FuncExprs for implicit
 * coercions) will have location -1, and so we can have odd combinations of
 * known and unknown locations in a tree.
 */
int expr_location(const sq_node_t *expr) {
  int loc;

  if (expr == NULL)
    return -1;
  switch ((expr)->tag) {
  case SQ_T_RANGE_VAR:
    loc = ((const sq_range_var_t *)expr)->location;
    break;
  case SQ_T_GROUPING_FUNC:
    loc = ((const sq_grouping_func_t *)expr)->location;
    break;
  case SQ_T_MERGE_SUPPORT_FUNC:
    loc = ((const sq_merge_support_func_t *)expr)->location;
    break;
  case SQ_T_NAMED_ARG_EXPR: {
    const sq_named_arg_expr_t *na = (const sq_named_arg_expr_t *)expr;

    /* consider both argument name and value */
    loc = leftmost_loc(na->location, expr_location((sq_node_t *)na->arg));
  } break;
  case SQ_T_BOOL_EXPR: {
    const sq_bool_expr_t *bexpr = (const sq_bool_expr_t *)expr;

    /*
     * Same as above, to handle either NOT or AND/OR.  We can't
     * special-case NOT because of the way that it's used for
     * things like IS NOT BETWEEN.
     */
    loc = leftmost_loc(bexpr->location, list_location(bexpr->args));
  } break;
  case SQ_T_SUB_LINK: {
    const sq_sub_link_t *sublink = (const sq_sub_link_t *)expr;

    /* check the testexpr, if any, and the operator/keyword */
    loc = leftmost_loc(expr_location(sublink->testexpr), sublink->location);
  } break;
  case SQ_T_CASE_EXPR:
    /* CASE keyword should always be the first thing */
    loc = ((const sq_case_expr_t *)expr)->location;
    break;
  case SQ_T_CASE_WHEN:
    /* WHEN keyword should always be the first thing */
    loc = ((const sq_case_when_t *)expr)->location;
    break;
  case SQ_T_ROW_EXPR:
    /* the location points at ROW or (, which must be leftmost */
    loc = ((const sq_row_expr_t *)expr)->location;
    break;
  case SQ_T_COALESCE_EXPR:
    /* COALESCE keyword should always be the first thing */
    loc = ((const sq_coalesce_expr_t *)expr)->location;
    break;
  case SQ_T_MIN_MAX_EXPR:
    /* GREATEST/LEAST keyword should always be the first thing */
    loc = ((const sq_min_max_expr_t *)expr)->location;
    break;
  case SQ_T_SQL_VALUE_FUNCTION:
    /* function keyword should always be the first thing */
    loc = ((const sq_sql_value_function_t *)expr)->location;
    break;
  case SQ_T_XML_EXPR: {
    const sq_xml_expr_t *xexpr = (const sq_xml_expr_t *)expr;

    /* consider both function name and leftmost arg */
    loc = leftmost_loc(xexpr->location, list_location(xexpr->args));
  } break;
  case SQ_T_JSON_FORMAT:
    loc = ((const sq_json_format_t *)expr)->location;
    break;
  case SQ_T_JSON_VALUE_EXPR:
    loc = expr_location(
        (sq_node_t *)((const sq_json_value_expr_t *)expr)->raw_expr);
    break;
  case SQ_T_JSON_IS_PREDICATE:
    loc = ((const sq_json_is_predicate_t *)expr)->location;
    break;
  case SQ_T_JSON_BEHAVIOR:
    loc = expr_location(((sq_json_behavior_t *)expr)->expr);
    break;
  case SQ_T_NULL_TEST: {
    const sq_null_test_t *nexpr = (const sq_null_test_t *)expr;

    /* Much as above */
    loc = leftmost_loc(nexpr->location, expr_location((sq_node_t *)nexpr->arg));
  } break;
  case SQ_T_BOOLEAN_TEST: {
    const sq_boolean_test_t *bexpr = (const sq_boolean_test_t *)expr;

    /* Much as above */
    loc = leftmost_loc(bexpr->location, expr_location((sq_node_t *)bexpr->arg));
  } break;
  case SQ_T_SET_TO_DEFAULT:
    loc = ((const sq_set_to_default_t *)expr)->location;
    break;
  case SQ_T_INTO_CLAUSE:
    /* use the contained RangeVar's location --- close enough */
    loc = expr_location((sq_node_t *)((const sq_into_clause_t *)expr)->rel);
    break;
  case SQ_T_LIST:
    /* report location of first list member that has a location */
    loc = list_location(static_cast<const sq_list_t *>(expr));
    break;
  case SQ_T_A_EXPR: {
    const sq_a_expr_t *aexpr = (const sq_a_expr_t *)expr;

    /* use leftmost of operator or left operand (if any) */
    /* we assume right operand can't be to left of operator */
    loc = leftmost_loc(aexpr->location, expr_location(aexpr->lexpr));
  } break;
  case SQ_T_COLUMN_REF:
    loc = ((const sq_column_ref_t *)expr)->location;
    break;
  case SQ_T_PARAM_REF:
    loc = ((const sq_param_ref_t *)expr)->location;
    break;
  case SQ_T_A_CONST:
    loc = ((const sq_a_const_t *)expr)->location;
    break;
  case SQ_T_FUNC_CALL: {
    const sq_func_call_t *fc = (const sq_func_call_t *)expr;

    /* consider both function name and leftmost arg */
    /* (we assume any ORDER BY nodes must be to right of name) */
    loc = leftmost_loc(fc->location, list_location(fc->args));
  } break;
  case SQ_T_A_ARRAY_EXPR:
    /* the location points at ARRAY or [, which must be leftmost */
    loc = ((const sq_a_array_expr_t *)expr)->location;
    break;
  case SQ_T_RES_TARGET:
    /* we need not examine the contained expression (if any) */
    loc = ((const sq_res_target_t *)expr)->location;
    break;
  case SQ_T_MULTI_ASSIGN_REF:
    loc = expr_location(((const sq_multi_assign_ref_t *)expr)->source);
    break;
  case SQ_T_TYPE_CAST: {
    const sq_type_cast_t *tc = (const sq_type_cast_t *)expr;

    /*
     * This could represent CAST(), ::, or TypeName 'literal', so
     * any of the components might be leftmost.
     */
    loc = expr_location(tc->arg);
    loc = leftmost_loc(loc, tc->type_name->location);
    loc = leftmost_loc(loc, tc->location);
  } break;
  case SQ_T_COLLATE_CLAUSE:
    /* just use argument's location */
    loc = expr_location(((const sq_collate_clause_t *)expr)->arg);
    break;
  case SQ_T_SORT_BY:
    /* just use argument's location (ignore operator, if any) */
    loc = expr_location(((const sq_sort_by_t *)expr)->node);
    break;
  case SQ_T_WINDOW_DEF:
    loc = ((const sq_window_def_t *)expr)->location;
    break;
  case SQ_T_RANGE_TABLE_SAMPLE:
    loc = ((const sq_range_table_sample_t *)expr)->location;
    break;
  case SQ_T_TYPE_NAME:
    loc = ((const sq_type_name_t *)expr)->location;
    break;
  case SQ_T_COLUMN_DEF:
    loc = ((const sq_column_def_t *)expr)->location;
    break;
  case SQ_T_CONSTRAINT:
    loc = ((const sq_constraint_t *)expr)->location;
    break;
  case SQ_T_FUNCTION_PARAMETER:
    loc = ((const sq_function_parameter_t *)expr)->location;
    break;
  case SQ_T_XML_SERIALIZE:
    /* XMLSERIALIZE keyword should always be the first thing */
    loc = ((const sq_xml_serialize_t *)expr)->location;
    break;
  case SQ_T_GROUPING_SET:
    loc = ((const sq_grouping_set_t *)expr)->location;
    break;
  case SQ_T_WITH_CLAUSE:
    loc = ((const sq_with_clause_t *)expr)->location;
    break;
  case SQ_T_INFER_CLAUSE:
    loc = ((const sq_infer_clause_t *)expr)->location;
    break;
  case SQ_T_ON_CONFLICT_CLAUSE:
    loc = ((const sq_on_conflict_clause_t *)expr)->location;
    break;
  case SQ_T_CTE_SEARCH_CLAUSE:
    loc = ((const sq_cte_search_clause_t *)expr)->location;
    break;
  case SQ_T_CTE_CYCLE_CLAUSE:
    loc = ((const sq_cte_cycle_clause_t *)expr)->location;
    break;
  case SQ_T_COMMON_TABLE_EXPR:
    loc = ((const sq_common_table_expr_t *)expr)->location;
    break;
  case SQ_T_JSON_KEY_VALUE:
    /* just use the key's location */
    loc = expr_location((sq_node_t *)((const sq_json_key_value_t *)expr)->key);
    break;
  case SQ_T_JSON_OBJECT_CONSTRUCTOR:
    loc = ((const sq_json_object_constructor_t *)expr)->location;
    break;
  case SQ_T_JSON_ARRAY_CONSTRUCTOR:
    loc = ((const sq_json_array_constructor_t *)expr)->location;
    break;
  case SQ_T_JSON_ARRAY_QUERY_CONSTRUCTOR:
    loc = ((const sq_json_array_query_constructor_t *)expr)->location;
    break;
  case SQ_T_JSON_AGG_CONSTRUCTOR:
    loc = ((const sq_json_agg_constructor_t *)expr)->location;
    break;
  case SQ_T_JSON_OBJECT_AGG:
    loc = expr_location(
        (sq_node_t *)((const sq_json_object_agg_t *)expr)->constructor);
    break;
  case SQ_T_JSON_ARRAY_AGG:
    loc = expr_location(
        (sq_node_t *)((const sq_json_array_agg_t *)expr)->constructor);
    break;
  case SQ_T_PARTITION_ELEM:
    loc = ((const sq_partition_elem_t *)expr)->location;
    break;
  case SQ_T_PARTITION_SPEC:
    loc = ((const sq_partition_spec_t *)expr)->location;
    break;
  case SQ_T_PARTITION_BOUND_SPEC:
    loc = ((const sq_partition_bound_spec_t *)expr)->location;
    break;
  default:
    /* for any other node type it's just unknown... */
    loc = -1;
    break;
  }
  return loc;
}

} // namespace sq
