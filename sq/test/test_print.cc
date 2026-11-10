/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_print.cc -- sq_to_string: trees built by hand print as
 * PostgreSQL's nodeToString() prints the same statement. The
 * expected text in each test is what PostgreSQL's parser printed for
 * the SQL named in it.
 */

#include "doctest.h"

#include "sq.h"

#include "list.h"
#include "tree.h"

#include <string>

namespace {

/* An A_Const holding the value of the value node val, in place. */
sq_node_t *constant(sq_tree_t &t, sq_node_t *val) {
  sq_a_const_t *c = sq::make_node<sq_a_const_t>(t);
  switch (val->tag) {
  case SQ_T_INTEGER:
    c->val.ival = *static_cast<sq_integer_t *>(val);
    break;
  case SQ_T_FLOAT:
    c->val.fval = *static_cast<sq_float_t *>(val);
    break;
  case SQ_T_BOOLEAN:
    c->val.boolval = *static_cast<sq_boolean_t *>(val);
    break;
  case SQ_T_STRING:
    c->val.sval = *static_cast<sq_string_t *>(val);
    break;
  default:
    c->val.bsval = *static_cast<sq_bit_string_t *>(val);
    break;
  }
  c->location = -1;
  return c;
}

/* An integer constant. */
sq_node_t *int_const(sq_tree_t &t, int v) {
  sq_integer_t *i = sq::make_node<sq_integer_t>(t);
  i->ival = v;
  return constant(t, i);
}

/* A String node. */
sq_node_t *string_node(sq_tree_t &t, const char *text) {
  sq_string_t *s = sq::make_node<sq_string_t>(t);
  s->sval = sq::dup_string(t, text);
  return s;
}

/* A string constant. */
sq_node_t *string_const(sq_tree_t &t, const char *text) {
  return constant(t, string_node(t, text));
}

/* A reference to the column called name. */
sq_node_t *column(sq_tree_t &t, const char *name) {
  sq_column_ref_t *c = sq::make_node<sq_column_ref_t>(t);
  c->fields = sq::lappend(c->fields, string_node(t, name));
  return c;
}

/* An unnamed entry of a select list. */
sq_node_t *target(sq_tree_t &t, sq_node_t *val) {
  sq_res_target_t *r = sq::make_node<sq_res_target_t>(t);
  r->val = val;
  return r;
}

/* A list node holding the names given. */
sq_node_t *name_list(sq_tree_t &t, std::initializer_list<const char *> names) {
  sq::list_ref_t l = sq::NIL;
  for (const char *name : names) {
    l = sq::lappend(l, string_node(t, name));
  }
  return l;
}

/* What sq_to_string writes for node. */
std::string text_of(const sq_node_t *node) {
  std::string out;
  REQUIRE(sq_to_string(node, out) == 0);
  return out;
}

/* What every SelectStmt in these tests prints after its WHERE clause. */
const char *const select_tail =
    " :groupClause <> :groupDistinct false :havingClause <> :windowClause <>"
    " :valuesLists <> :sortClause <> :limitOffset <> :limitCount <>"
    " :limitOption 0 :lockingClause <> :withClause <> :op 0 :all false"
    " :larg <> :rarg <>}";

} // namespace

TEST_CASE("SELECT 1 + 1 prints as PostgreSQL prints it") {
  // Verify a generated node, a hand-printed node and a list print right.
  // 1. Build SELECT 1 + 1: a SelectStmt, a ResTarget, an A_Expr.
  // 2. Print it and compare with PostgreSQL's text.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_a_expr_t *plus = sq::make_node<sq_a_expr_t>(t);
  plus->kind = SQ_AEXPR_OP;
  plus->name = sq::lappend(plus->name, string_node(t, "+"));
  plus->lexpr = int_const(t, 1);
  plus->rexpr = int_const(t, 1);
  sq_select_stmt_t *sel = sq::make_node<sq_select_stmt_t>(t);
  sel->target_list = sq::lappend(sel->target_list, target(t, plus));

  CHECK(text_of(sel) ==
        std::string(
            "{SELECTSTMT :distinctClause <> :intoClause <> :targetList "
            "({RESTARGET :name <> :indirection <> :val {A_EXPR :name (\"+\") "
            ":lexpr {A_CONST :val 1 :location -1} :rexpr {A_CONST :val 1 "
            ":location -1} :rexpr_list_start -1 :rexpr_list_end -1 "
            ":location -1} :location -1}) :fromClause <> :whereClause <>") +
            select_tail);
}

TEST_CASE("constants of every kind, a qualified table, and AND and NOT") {
  // Verify each value node, absent and present text, and BoolExpr.
  // SELECT NULL, 'a b', 1.5, true, b'101' FROM s.t WHERE x AND NOT y;
  // 1. Build the five constants: NULL, String, Float, Boolean, BitString.
  // 2. Build s.t, with no catalog name and the default persistence.
  // 3. Build x AND NOT y.
  // 4. Print the statement and compare with PostgreSQL's text.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_select_stmt_t *sel = sq::make_node<sq_select_stmt_t>(t);

  sq_a_const_t *null_const = sq::make_node<sq_a_const_t>(t);
  null_const->isnull = true;
  sel->target_list = sq::lappend(sel->target_list, target(t, null_const));
  sel->target_list =
      sq::lappend(sel->target_list, target(t, string_const(t, "a b")));
  sq_float_t *f = sq::make_node<sq_float_t>(t);
  f->fval = sq::dup_string(t, "1.5");
  sel->target_list = sq::lappend(sel->target_list, target(t, constant(t, f)));
  sq_boolean_t *b = sq::make_node<sq_boolean_t>(t);
  b->boolval = true;
  sel->target_list = sq::lappend(sel->target_list, target(t, constant(t, b)));
  sq_bit_string_t *bits = sq::make_node<sq_bit_string_t>(t);
  bits->bsval = sq::dup_string(t, "b101");
  sel->target_list =
      sq::lappend(sel->target_list, target(t, constant(t, bits)));

  sq_range_var_t *rel = sq::make_node<sq_range_var_t>(t);
  rel->schemaname = sq::dup_string(t, "s");
  rel->relname = sq::dup_string(t, "t");
  rel->inh = true;
  rel->relpersistence = 'p';
  sel->from_clause = sq::lappend(sel->from_clause, rel);

  sq_bool_expr_t *not_y = sq::make_node<sq_bool_expr_t>(t);
  not_y->boolop = SQ_NOT_EXPR;
  not_y->args = sq::lappend(not_y->args, column(t, "y"));
  sq_bool_expr_t *both = sq::make_node<sq_bool_expr_t>(t);
  both->boolop = SQ_AND_EXPR;
  both->args = sq::lappend(both->args, column(t, "x"));
  both->args = sq::lappend(both->args, not_y);
  sel->where_clause = both;

  CHECK(text_of(sel) ==
        std::string(
            "{SELECTSTMT :distinctClause <> :intoClause <> :targetList "
            "({RESTARGET :name <> :indirection <> :val {A_CONST NULL "
            ":location -1} :location -1} "
            "{RESTARGET :name <> :indirection <> :val {A_CONST :val \"a\\ b\" "
            ":location -1} :location -1} "
            "{RESTARGET :name <> :indirection <> :val {A_CONST :val 1.5 "
            ":location -1} :location -1} "
            "{RESTARGET :name <> :indirection <> :val {A_CONST :val true "
            ":location -1} :location -1} "
            "{RESTARGET :name <> :indirection <> :val {A_CONST :val b101 "
            ":location -1} :location -1}) "
            ":fromClause ({RANGEVAR :catalogname <> :schemaname s :relname t "
            ":inh true :relpersistence p :alias <> :location -1}) "
            ":whereClause {BOOLEXPR :boolop and :args ({COLUMNREF :fields "
            "(\"x\") :location -1} {BOOLEXPR :boolop not :args ({COLUMNREF "
            ":fields (\"y\") :location -1}) :location -1}) :location -1}") +
            select_tail);
}

TEST_CASE("empty text is not absent text, and a list can stand as a node") {
  // Verify "" and NULL print differently, and a list in a node's place.
  // COMMENT ON TABLE t IS '';   and   COMMENT ON TABLE t IS NULL;
  // 1. Build the statement with the name list as its object node.
  // 2. Print it with an empty comment, then with none.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_comment_stmt_t *c = sq::make_node<sq_comment_stmt_t>(t);
  c->objtype = SQ_OBJECT_TABLE;
  c->object = name_list(t, {"t"});
  c->comment = sq::dup_string(t, "");
  CHECK(text_of(c) ==
        "{COMMENTSTMT :objtype 41 :object (\"t\") :comment \"\"}");
  c->comment = nullptr;
  CHECK(text_of(c) == "{COMMENTSTMT :objtype 41 :object (\"t\") :comment <>}");
}

TEST_CASE("a list of lists prints nested") {
  // Verify lists nest, as DROP's list of qualified names does.
  // DROP TABLE a, b.c;
  // 1. Build the statement with two name lists as its objects.
  // 2. Print it and compare with PostgreSQL's text.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_drop_stmt_t *d = sq::make_node<sq_drop_stmt_t>(t);
  d->objects = sq::lappend(d->objects, name_list(t, {"a"}));
  d->objects = sq::lappend(d->objects, name_list(t, {"b", "c"}));
  d->remove_type = SQ_OBJECT_TABLE;
  CHECK(text_of(d) ==
        "{DROPSTMT :objects ((\"a\") (\"b\" \"c\")) :removeType 41 "
        ":behavior 0 :missing_ok false :concurrent false}");
}

TEST_CASE("text is escaped as PostgreSQL's node reader needs") {
  // Verify the token escapes: anywhere, at the start only, and none.
  // SELECT 'it''s (x) "q"', '', '1abc', '<a>';
  // 1. Print a string with spaces, parentheses and quotes inside.
  // 2. Print an empty string.
  // 3. Print strings that begin with a digit and with '<'.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  CHECK(text_of(string_const(t, "it's (x) \"q\"")) ==
        "{A_CONST :val \"it's\\ \\(x\\)\\ \"q\"\" :location -1}");
  CHECK(text_of(string_const(t, "")) == "{A_CONST :val \"\" :location -1}");
  CHECK(text_of(string_const(t, "1abc")) ==
        "{A_CONST :val \"\\1abc\" :location -1}");
  CHECK(text_of(string_const(t, "<a>")) ==
        "{A_CONST :val \"\\<a>\" :location -1}");
}

TEST_CASE("locations are hidden, or shown when asked for") {
  // Verify the two printing calls differ only in the locations.
  // 1. Build a column reference inside a statement, each with a location.
  // 2. Print it the plain way: every location is -1, and so is the
  //    statement's length, which PostgreSQL counts as one.
  // 3. Print it with locations: each is the one it was given.
  // 4. Print the plain way again: asking once does not stick.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_column_ref_t *col = sq::make_node<sq_column_ref_t>(t);
  col->fields = sq::lappend(col->fields, string_node(t, "a"));
  col->location = 7;
  sq_raw_stmt_t *raw = sq::make_node<sq_raw_stmt_t>(t);
  raw->stmt = col;
  raw->stmt_location = 3;
  raw->stmt_len = 12;

  const std::string hidden = "{RAWSTMT :stmt {COLUMNREF :fields (\"a\") "
                             ":location -1} :stmt_location -1 :stmt_len -1}";
  CHECK(text_of(raw) == hidden);
  std::string shown;
  REQUIRE(sq_to_string_with_locations(raw, shown) == 0);
  CHECK(shown == "{RAWSTMT :stmt {COLUMNREF :fields (\"a\") :location 7} "
                 ":stmt_location 3 :stmt_len 12}");
  CHECK(text_of(raw) == hidden);
}

TEST_CASE("an absent node prints as <>") {
  // Verify NULL is printable, as the statement of an empty tree is.
  // 1. Print a null node.
  CHECK(text_of(nullptr) == "<>");
}
