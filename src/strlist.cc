/*
 * strlist.cc -- sm_strlist_t, the counted immutable list of
 * names returned by the library's listing calls.
 *
 * One allocation holds the whole list:
 *
 *   sm_strlist_t      the header, holding the count
 *   char* ptr[count]  one per name, pointing into the tail
 *   char  bytes[]     the names, each NUL-terminated
 *
 * so a list is one malloc and one free, and every name sits
 * beside the pointer that names it.
 *
 * strlist_make() is the only producer; the accessors below
 * are the only readers.
 */

#include "strlist.h"

#include <stdlib.h>
#include <string.h>

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

/* The pointer array, writable, for strlist_make(). */
static char** strlist_ptr_mut(sm_strlist_t* sl)
{
    return reinterpret_cast<char**>(sl + 1);
}

namespace wisr
{

/* Pack names into one allocation laid out as described above. */
sm_strlist_t* strlist_make(const std::vector<std::string>& names)
{
    size_t bytes = sizeof(sm_strlist_t) + names.size() * sizeof(char*);
    for (const std::string& n : names) {
        bytes += n.size() + 1;
    }
    sm_strlist_t* sl = static_cast<sm_strlist_t*>(malloc(bytes));
    if (!sl) {
        return nullptr;
    }
    sl->count = names.size();
    char** ptr = strlist_ptr_mut(sl);
    char* tail = reinterpret_cast<char*>(ptr + names.size());
    for (size_t i = 0; i < names.size(); i++) {
        ptr[i] = tail;
        memcpy(tail, names[i].data(), names[i].size());
        tail[names[i].size()] = '\0';
        tail += names[i].size() + 1;
    }
    return sl;
}

} // namespace wisr

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
