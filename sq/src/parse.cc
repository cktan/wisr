/*
 * parse.cc -- sq_parse.
 *
 * The parser is not here yet (PLAN.md step 1): sq_parse refuses
 * every input. What is here is the shape of a call: the tree is
 * emptied first, and no exception leaves.
 */

#include "sq.h"

#include <new>

#include "error.h"

/* Parse sql into out: refused, until there is a parser. */
int sq_parse(std::string_view sql, const sq_options_t *opt, sq_tree_t &out) {
  try {
    // A parse is all or nothing, so what out held goes first.
    out = sq_tree_t();
    (void)sql;
    (void)opt;
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
