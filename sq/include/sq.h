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
 *     sq_errpos(), and PostgreSQL's detail and hint, where it has
 *     them, with sq_errdetail() and sq_errhint(). A successful
 *     call leaves them untouched.
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

/* Its detail, or "" when it has none. */
const char* sq_errdetail(void);

/* Its hint, or "" when it has none. */
const char* sq_errhint(void);

/* ---- options, and notices ---- */

/* PostgreSQL's backslash_quote: whether \' may stand for a quote. */
enum sq_backslash_quote_t {
    SQ_BACKSLASH_QUOTE_OFF           = 0,
    SQ_BACKSLASH_QUOTE_ON            = 1,
    SQ_BACKSLASH_QUOTE_SAFE_ENCODING = 2   /* on: the text is UTF-8 */
};

/* How serious a notice is: PostgreSQL's NOTICE and WARNING. */
enum sq_notice_level_t {
    SQ_NOTICE  = 0,
    SQ_WARNING = 1
};

/*
 * Something a parse has to say that is not an error, such as an
 * identifier cut to PostgreSQL's 63 bytes. The strings are valid
 * only during the call that hands them over.
 */
struct sq_notice_t {
    sq_notice_level_t level;
    const char*       sqlstate;   /* five characters */
    const char*       message;
    const char*       hint;       /* or NULL */
    int               pos;        /* in characters from 1, or 0 */
};

/*
 * A notice hook: the arg given in the options, and the notice. It
 * runs inside the parse, on the calling thread: it must return
 * normally, and must not throw or longjmp.
 */
typedef void (*sq_notice_fn)(void* arg, const sq_notice_t* notice);

/*
 * How to parse. The first three are PostgreSQL's settings of the
 * same names, at PostgreSQL's defaults.
 */
struct sq_options_t {
    bool                 standard_conforming_strings = true;
    sq_backslash_quote_t backslash_quote = SQ_BACKSLASH_QUOTE_SAFE_ENCODING;
    bool                 escape_string_warning = true;

    /* Called for each notice, or NULL to drop them. */
    sq_notice_fn         notice = nullptr;
    void*                notice_arg = nullptr;
};

/* ---- parsing ---- */

/*
 * Parse sql, a list of statements separated by semicolons, into
 * out, replacing what out held. opt may be NULL, for the defaults.
 * The text must be UTF-8. A parse is all or nothing: on -1, out is
 * empty.
 */
int sq_parse(std::string_view sql, const sq_options_t* opt, sq_tree_t& out);

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
