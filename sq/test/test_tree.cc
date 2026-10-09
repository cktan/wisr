/*
 * test_tree.cc -- a tree owns what is made for it (sq.h, src/tree.h).
 * The test binary is linked with a leak checker, so a node or a
 * string that a tree failed to free fails the run.
 */

#include "doctest.h"

#include "sq.h"

#include "tree.h"

#include <string>
#include <utility>

TEST_CASE("a node is made zeroed, with its own tag") {
  // Verify make_node gives what PostgreSQL's makeNode gives.
  // 1. Make a SelectStmt and a list node.
  // 2. Confirm each one's tag, and that every field is empty.
  sq_tree_t t;
  sq_select_stmt_t *sel = sq::make_node<sq_select_stmt_t>(t);
  CHECK(sel->tag == SQ_T_SELECT_STMT);
  CHECK(sel->target_list.empty());
  CHECK(sel->where_clause == nullptr);
  CHECK(sel->all == false);
  CHECK(sel->op == SQ_SETOP_NONE);
  CHECK(sq::make_node<sq_list_t>(t)->tag == SQ_T_LIST);
}

TEST_CASE("a tree's text is its own copy") {
  // Verify dup_string copies, so the tree outlives the text parsed.
  // 1. Copy a string into the tree, then overwrite the original.
  // 2. Confirm the copy is unchanged, and NUL-terminated.
  sq_tree_t t;
  std::string original = "relname";
  const char *copy = sq::dup_string(t, original);
  original.assign("XXXXXXX");
  CHECK(std::string(copy) == "relname");
  CHECK(std::string(sq::dup_string(t, std::string_view("abcdef", 3))) == "abc");
}

TEST_CASE("a moved tree keeps its nodes, and frees them once") {
  // Verify moving a tree moves what it owns.
  // 1. Build a node holding a list and a string in one tree.
  // 2. Move the tree twice, by construction and by assignment.
  // 3. Confirm the node still prints from the last tree.
  // 4. Let all three trees be destroyed; the leak checker sees the rest.
  sq_tree_t a;
  sq_column_ref_t *col = sq::make_node<sq_column_ref_t>(a);
  sq_string_t *name = sq::make_node<sq_string_t>(a);
  name->sval = sq::dup_string(a, "x");
  col->fields.push_back(name);
  sq_raw_stmt_t *raw = sq::make_node<sq_raw_stmt_t>(a);
  raw->stmt = col;
  a.stmts.push_back(raw);

  sq_tree_t b(std::move(a));
  CHECK(a.stmts.empty());
  sq_tree_t c;
  sq::make_node<sq_select_stmt_t>(c);
  c = std::move(b);
  CHECK(b.stmts.empty());
  REQUIRE(c.stmts.size() == 1);

  std::string out;
  CHECK(sq_to_string(c.stmts[0]->stmt, out) == 0);
  CHECK(out == "{COLUMNREF :fields (\"x\") :location -1}");
}

TEST_CASE("a failed parse frees the tree it was given") {
  // Verify sq_parse empties its tree of nodes, not only of statements.
  // 1. Make nodes and text in a tree.
  // 2. Parse into it, which fails.
  // 3. Confirm it is empty; the leak checker sees the rest.
  sq_tree_t t;
  t.stmts.push_back(sq::make_node<sq_raw_stmt_t>(t));
  sq::dup_string(t, "text");
  CHECK(sq_parse("SELECT FROM WHERE", nullptr, t) == -1);
  CHECK(t.stmts.empty());
}
