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
 *   - A tree owns its nodes and their text. Both are valid for as
 *     long as the tree is, and nothing is freed by hand.
 *   - Anything else handed out is the caller's own: bytes are a
 *     std::string, filled by the call.
 *   - No C++ exception crosses this boundary. Every call catches
 *     everything: std::bad_alloc becomes SQ_ERR_NOMEM, anything
 *     else SQ_ERR_EXCEPTION.
 *
 * Threading: any number of threads may parse at once, each into
 * its own tree. The error state is per thread.
 */
#ifndef SQ_H
#define SQ_H

#include <string>
#include <string_view>
#include <vector>

#include "sq_error.h"
#include "sq_nodes.h"

namespace sq {
struct store_t;
struct tree_access_t;
} // namespace sq

/*
 * The result of one parse, which owns every node in it and every
 * piece of text a node points at. It can be moved and not copied;
 * destroying it destroys them all. The nodes are in sq_nodes.h.
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

private:
    friend struct sq::tree_access_t;

    /* What the tree owns; NULL until something is made for it. */
    sq::store_t* m_store = nullptr;
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

/* ---- printing ---- */

/*
 * Write node, and everything under it, into out as PostgreSQL's
 * nodeToString() writes its own: "{SELECTSTMT :distinctClause <>
 * ...}". The names in the text are PostgreSQL's, and every location
 * is written as -1, so that two parses of the same statement print
 * the same. A NULL node is "<>".
 */
int sq_to_string(const sq_node_t* node, std::string& out);

#endif /* SQ_H */
