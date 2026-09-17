/*
 * strlist.cc -- sm_strlist_t, the counted immutable list of
 * names returned by the library's listing calls.
 *
 * One allocation holds the whole list (PLAN.md §3):
 *
 *   sm_strlist_t      the header, holding the count
 *   char* ptr[count]  one per name, pointing into the tail
 *   char  bytes[]     the names, each NUL-terminated
 *
 * so a list is one malloc and one free, and every name sits
 * beside the pointer that names it.
 *
 * Construction is internal and deliberately not written yet.
 * The layout described here is the contract a producer must
 * build to, and these accessors are its only readers.
 */

#include "sm.h"

#include <stdlib.h>

struct sm_strlist_t {
    size_t count;
    /* char* ptr[count], then char bytes[] -- see above. */
};

/*
 * The pointer array that follows the header. The header is
 * one size_t, so the array is already aligned for char*.
 */
static char* const* strlist_ptr(const sm_strlist_t* sl)
{
    return reinterpret_cast<char* const*>(sl + 1);
}

/* The number of names in the list. */
size_t sm_strlist_count(const sm_strlist_t* sl)
{
    return sl->count;
}

/* Name i, or NULL if i is past the end. */
const char* sm_strlist_at(const sm_strlist_t* sl, size_t i)
{
    if (i >= sl->count) {
        return NULL;
    }
    return strlist_ptr(sl)[i];
}

/* Release a list: one free, since it is one allocation. */
void sm_strlist_free(sm_strlist_t* sl)
{
    free(sl);
}
