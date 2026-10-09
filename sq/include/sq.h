/*
 * sq.h -- sq's API: PostgreSQL 18's SQL in, a tree of nodes out.
 * It is C in style and compiled as C++: std::string, std::vector
 * and std::string_view may cross it, and no other C++ type does.
 *
 * Conventions that hold throughout:
 *
 *   - A function returning int returns 0 on success and -1 on
 *     error; the code, message, SQLSTATE and position are then
 *     read with sq_errnum(), sq_errmsg(), sq_sqlstate() and
 *     sq_errpos(). A successful call leaves them untouched.
 *   - Text is a std::string_view, read during the call and never
 *     retained: a tree holds copies of what it needs.
 *   - A tree owns its nodes. A node is valid for as long as its
 *     tree is, and nothing is freed by hand.
 *   - No C++ exception crosses this boundary. Every call catches
 *     everything: std::bad_alloc becomes SQ_ERR_NOMEM, anything
 *     else SQ_ERR_EXCEPTION.
 *
 * Threading: any number of threads may parse at once, each into
 * its own tree. The error state is per thread.
 */
#ifndef SQ_H
#define SQ_H

#include <string_view>
#include <vector>

#include "sq_error.h"

/* One node of a parse tree. */
struct sq_node_t;

/*
 * The result of one parse, which owns every node in it. It can be
 * moved and not copied; destroying it destroys the nodes.
 */
struct sq_tree_t {
    /* The statements, in the order written. */
    std::vector<sq_node_t*> stmts;

    sq_tree_t();
    ~sq_tree_t();
    sq_tree_t(sq_tree_t&& other) noexcept;
    sq_tree_t& operator=(sq_tree_t&& other) noexcept;
    sq_tree_t(const sq_tree_t&) = delete;
    sq_tree_t& operator=(const sq_tree_t&) = delete;
};

/* ---- errors: the calling thread's error state ---- */

/* The last error raised on the calling thread. */
sq_errnum_t sq_errnum(void);

/* Its message; never NULL, valid until the thread's next error. */
const char* sq_errmsg(void);

/* Its SQLSTATE, five characters; "00000" when there is no error. */
const char* sq_sqlstate(void);

/*
 * Where in the text it was found, counted in characters from 1,
 * or 0 when the error has no position.
 */
int sq_errpos(void);

/* ---- parsing ---- */

/*
 * Parse sql, a list of statements separated by semicolons, into
 * out, replacing what out held. A parse is all or nothing: on -1,
 * out is empty.
 */
int sq_parse(std::string_view sql, sq_tree_t& out);

#endif /* SQ_H */
