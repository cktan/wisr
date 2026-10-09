/*
 * parse.cc -- sq_parse, and the tree it fills.
 *
 * The parser is not here yet (PLAN.md step 1): sq_parse refuses
 * every input. What is here is the shape of a call: the tree is
 * emptied first, and no exception leaves.
 */

#include "sq.h"

#include <new>
#include <utility>

#include "error.h"

/* An empty tree. */
sq_tree_t::sq_tree_t() = default;

/* Destroy the tree and its nodes; there are no nodes to own yet. */
sq_tree_t::~sq_tree_t() = default;

/* Take other's statements, leaving it empty. */
sq_tree_t::sq_tree_t(sq_tree_t &&other) noexcept
    : stmts(std::move(other.stmts)) {
  other.stmts.clear();
}

/* Replace this tree with other's statements, leaving other empty. */
sq_tree_t &sq_tree_t::operator=(sq_tree_t &&other) noexcept {
  if (this != &other) {
    stmts = std::move(other.stmts);
    other.stmts.clear();
  }
  return *this;
}

/* Parse sql into out: refused, until there is a parser. */
int sq_parse(std::string_view sql, sq_tree_t &out) {
  try {
    // A parse is all or nothing, so what out held goes first.
    out = sq_tree_t();
    (void)sql;
    return sq::set_error(SQ_ERR_PARSE, sq::sqlstate_feature_not_supported, 0,
                         "sq_parse is not implemented");
  } catch (const std::bad_alloc &) {
    return sq::set_error(SQ_ERR_NOMEM, sq::sqlstate_out_of_memory, 0,
                         "out of memory");
  } catch (...) {
    return sq::set_error(SQ_ERR_EXCEPTION, sq::sqlstate_internal_error, 0,
                         "unexpected exception in sq_parse");
  }
}
