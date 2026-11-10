/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_tree.cc -- a tree owns what is made for it (sq.h, src/tree.h).
 * The test binary is linked with a leak checker, so a node or a
 * string that a tree failed to free fails the run.
 */

#include "doctest.h"

#include "sq.h"

#include "list.h"
#include "tree.h"

#include <string>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

TEST_CASE("a node is made zeroed, with its own tag") {
  // Verify make_node gives what PostgreSQL's makeNode gives, and a list
  // what PostgreSQL's list_make1 gives.
  // 1. Make a SelectStmt, and a list of it.
  // 2. Confirm each one's tag, that every field of the SelectStmt is
  //    empty, and that the list holds it in place.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  sq_select_stmt_t *sel = sq::make_node<sq_select_stmt_t>(t);
  CHECK(sel->tag == SQ_T_SELECT_STMT);
  CHECK(sel->target_list == nullptr);
  CHECK(sel->where_clause == nullptr);
  CHECK(sel->all == false);
  CHECK(sel->op == SQ_SETOP_NONE);
  sq_list_t *l = sq::list_make1(sel);
  CHECK(l->tag == SQ_T_LIST);
  CHECK(l->length == 1);
  CHECK(l->elements == l->initial_elements);
  CHECK(l->elements[0].ptr_value == sel);
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
  sq::current_tree_t current(a); // lists grow in the tree being built
  sq_column_ref_t *col = sq::make_node<sq_column_ref_t>(a);
  sq_string_t *name = sq::make_node<sq_string_t>(a);
  name->sval = sq::dup_string(a, "x");
  col->fields = sq::lappend(col->fields, name);
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

TEST_CASE("a tree past its first block keeps every node and string whole") {
  // Verify the arena under a tree: many blocks, and a piece bigger
  // than any block, each node aligned and each string intact.
  // 1. Make 5000 column refs, each holding a string of its own number,
  //    and one string of 2 MB, past the largest block.
  // 2. Confirm each node is aligned for its type, keeps its tag and
  //    its list, and each string reads back as written.
  // 3. Let the tree go; the leak checker sees its blocks and lists.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  std::vector<sq_column_ref_t *> cols;
  for (int i = 0; i < 5000; i++) {
    sq_column_ref_t *col = sq::make_node<sq_column_ref_t>(t);
    sq_string_t *name = sq::make_node<sq_string_t>(t);
    name->sval = sq::dup_string(t, std::to_string(i));
    col->fields = sq::lappend(col->fields, name);
    cols.push_back(col);
  }
  std::string big(2 << 20, 'b');
  const char *copy = sq::dup_string(t, big);

  for (int i = 0; i < 5000; i++) {
    sq_column_ref_t *col = cols[i];
    CHECK(reinterpret_cast<uintptr_t>(col) % alignof(sq_column_ref_t) == 0);
    CHECK(col->tag == SQ_T_COLUMN_REF);
    REQUIRE(sq::list_length(col->fields) == 1);
    auto *name = static_cast<sq_string_t *>(col->fields->elements[0].ptr_value);
    CHECK(name->tag == SQ_T_STRING);
    CHECK(std::string(name->sval) == std::to_string(i));
  }
  CHECK(std::string(copy) == big);
}

TEST_CASE(
    "a list grows as PostgreSQL's does: in place, then an array of its own") {
  // Verify list.c's growth: cells after the header until they run out,
  // then an array apart that grows; and the functions that move cells.
  // 1. Append 40 nodes one by one: the first fill the cells in place,
  //    the next move to an array apart, which grows after.
  // 2. Put one more at the front; concatenate the list with itself.
  // 3. Confirm every node is where it should be.
  sq_tree_t t;
  sq::current_tree_t current(t); // lists grow in the tree being built
  std::vector<sq_node_t *> nodes;
  sq::list_ref_t l = sq::NIL;
  for (int i = 0; i < 40; i++) {
    nodes.push_back(sq::make_node<sq_integer_t>(t));
    l = sq::lappend(l, nodes.back());
    if (i == 0) {
      CHECK(l.list->elements == l.list->initial_elements);
    }
  }
  CHECK(l.list->elements != l.list->initial_elements);
  CHECK(l.list->max_length >= 40);
  sq_node_t *front = sq::make_node<sq_integer_t>(t);
  l = sq::lcons(front, l);
  l = sq::list_concat(l, l);

  REQUIRE(sq::list_length(l) == 82);
  for (int half = 0; half < 2; half++) {
    CHECK(l.list->elements[half * 41].ptr_value == front);
    for (int i = 0; i < 40; i++) {
      CHECK(l.list->elements[half * 41 + 1 + i].ptr_value == nodes[i]);
    }
  }
}

namespace {

/* An allocator that keeps what it hands out, for a test to free. */
struct kept_t {
  std::vector<void *> pieces;
  int reallocs = 0;
};

/* sq_allocator_t's alloc, from malloc, kept. */
void *kept_alloc(void *arg, size_t size) {
  void *p = malloc(size);
  static_cast<kept_t *>(arg)->pieces.push_back(p);
  return p;
}

/* sq_allocator_t's realloc, kept in place of what it grew. */
void *kept_realloc(void *arg, void *ptr, size_t size) {
  kept_t *k = static_cast<kept_t *>(arg);
  void *p = realloc(ptr, size);
  for (void *&piece : k->pieces) {
    if (piece == ptr) {
      piece = p;
    }
  }
  k->reallocs++;
  return p;
}

} // namespace

TEST_CASE(
    "a parse with an allocator makes its tree there, and frees none of it") {
  // Verify sq_options_t's allocator: a server's memory context holds
  // the tree, and the server frees it.
  // 1. Parse a SELECT of 40 columns with an allocator that keeps what
  //    it hands out.
  // 2. Confirm the statement and its select list came from it, and
  //    that the list grew by its realloc; and that the tree's store
  //    came from it too, so that the parse made none of its own.
  // 3. Destroy the tree: everything kept is still the test's to free,
  //    which it does; the leak checker sees anything else.
  kept_t kept;
  sq_allocator_t allocator{kept_alloc, kept_realloc, &kept};
  sq_options_t opt;
  opt.allocator = &allocator;
  std::string sql = "select 0";
  for (int i = 1; i < 40; i++) {
    sql += ", " + std::to_string(i);
  }
  {
    sq_tree_t tree;
    REQUIRE(sq_parse(sql, &opt, tree) == 0);
    REQUIRE(tree.stmts.size() == 1);
    auto *sel = static_cast<sq_select_stmt_t *>(tree.stmts[0]->stmt);
    auto kept_piece = [&](const void *p) {
      return std::find(kept.pieces.begin(), kept.pieces.end(), p) !=
             kept.pieces.end();
    };
    CHECK(kept_piece(tree.stmts[0]));
    CHECK(kept_piece(sel));
    CHECK(kept_piece(sel->target_list));
    CHECK(kept_piece(sel->target_list->elements));
    CHECK(sq::list_length(sel->target_list) == 40);
    CHECK(kept.reallocs > 0);
    CHECK(kept_piece(&sq::tree_access_t::store(tree)));
  }
  for (void *p : kept.pieces) {
    free(p);
  }
}

TEST_CASE("a failed parse with an allocator leaves its store to the caller") {
  // Verify a tree whose store is the caller's frees none of it, when a
  // parse fails as when one succeeds.
  // 1. Parse with an allocator that keeps what it hands out, then parse
  //    text that fails into the same tree.
  // 2. Confirm the tree is empty.
  // 3. Let the tree go, then free what was kept; the leak checker sees
  //    a store sq freed itself, or one it never freed.
  kept_t kept;
  sq_allocator_t allocator{kept_alloc, kept_realloc, &kept};
  sq_options_t opt;
  opt.allocator = &allocator;
  {
    sq_tree_t tree;
    REQUIRE(sq_parse("select 1", &opt, tree) == 0);
    CHECK(sq_parse("select from where", &opt, tree) == -1);
    CHECK(tree.stmts.empty());
  }
  for (void *p : kept.pieces) {
    free(p);
  }
}
