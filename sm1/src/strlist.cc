/*
 * strlist.cc -- sm1_strlist_t, the counted immutable list of
 * names returned by the library's listing calls.
 *
 * One allocation holds the whole list:
 *
 *   sm1_strlist_t     the header, holding the count
 *   char* ptr[count]  one per name, pointing into the tail
 *   char  bytes[]     the names, each NUL-terminated
 *
 * so a list is one malloc and one free, and every name sits
 * beside the pointer that names it. The caller releases a list
 * with free() itself; the library has no release call.
 *
 * strlist_make() is the only producer; the accessors below
 * are the only readers.
 */

#include "strlist.h"

#include <stdlib.h>
#include <string.h>

struct sm1_strlist_t {
  size_t count;
  /* char* ptr[count], then char bytes[] -- see above. */
};

/*
 * The pointer array that follows the header. The header is
 * one size_t, so the array is already aligned for char*.
 */
static char *const *strlist_ptr(const sm1_strlist_t *sl) {
  return reinterpret_cast<char *const *>(sl + 1);
}

/* The pointer array, writable, for strlist_make(). */
static char **strlist_ptr_mut(sm1_strlist_t *sl) {
  return reinterpret_cast<char **>(sl + 1);
}

namespace wisr {

/* Pack names into one allocation laid out as described above. */
sm1_strlist_t *strlist_make(const std::vector<std::string> &names) {
  // Size the header, the pointer array, and every name with its NUL.
  size_t bytes = sizeof(sm1_strlist_t) + names.size() * sizeof(char *);
  for (const std::string &n : names) {
    bytes += n.size() + 1;
  }
  // Allocate the whole list as one block.
  sm1_strlist_t *sl = static_cast<sm1_strlist_t *>(malloc(bytes));
  // Fail if the allocation failed.
  if (!sl) {
    return nullptr;
  }
  // Record the count in the header.
  sl->count = names.size();
  // Locate the pointer array and the byte tail after it.
  char **ptr = strlist_ptr_mut(sl);
  char *tail = reinterpret_cast<char *>(ptr + names.size());
  // Copy each name into the tail and point its slot at it.
  for (size_t i = 0; i < names.size(); i++) {
    // Point this name's slot at its place in the tail.
    ptr[i] = tail;
    // Copy the name's bytes.
    memcpy(tail, names[i].data(), names[i].size());
    // NUL-terminate the copy.
    tail[names[i].size()] = '\0';
    // Advance the tail past it.
    tail += names[i].size() + 1;
  }
  return sl;
}

} // namespace wisr

/* The number of names in the list. */
size_t sm1_strlist_count(const sm1_strlist_t *sl) { return sl->count; }

/* Name i, or NULL if i is past the end. */
const char *sm1_strlist_at(const sm1_strlist_t *sl, size_t i) {
  // Reject an index past the end.
  if (i >= sl->count) {
    return NULL;
  }
  return strlist_ptr(sl)[i];
}
