/*
 * test_parse.cc -- sq_parse, as far as it goes: it refuses every
 * input (PLAN.md step 1).
 */

#include "doctest.h"

#include "sq.h"

#include <string>

TEST_CASE("sq_parse refuses its input and leaves the tree empty") {
  // Verify a failed parse returns -1, reports why, and empties its tree.
  // 1. Give a tree one statement slot, as an earlier parse would have.
  // 2. Parse a statement into it.
  // 3. Confirm -1, the error, and that the tree is empty.
  sq_tree_t tree;
  tree.stmts.push_back(nullptr);
  CHECK(sq_parse("SELECT 1;", tree) == -1);
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_sqlstate()) == "0A000");
  CHECK(std::string(sq_errmsg()) == "sq_parse is not implemented");
  CHECK(sq_errpos() == 0);
  CHECK(tree.stmts.empty());
}
