/*
 * sm.h -- the wisr storage manager's public C API.
 *
 * API.md is the interface contract; DESIGN.md is the design
 * behind it.
 */
#ifndef SM_H
#define SM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A counted, immutable list of names returned by a listing
 * call. Elements are NUL-terminated and owned by the list;
 * release the whole list with sm_strlist_free().
 *
 * A list holds no engine state -- no lock, no snapshot, no
 * session reference -- so failing to free one leaks memory
 * and nothing else. This is unlike sm_scan_t, where a
 * missed close pins the GC watermark.
 *
 * Names only. Keys and values may contain NUL and must not
 * travel in one.
 */
typedef struct sm_strlist_t sm_strlist_t;

/* The number of names in the list. */
size_t sm_strlist_count(const sm_strlist_t* sl);

/*
 * Name i, NUL-terminated and owned by the list, or NULL if
 * i is past the end. Valid until sm_strlist_free().
 */
const char* sm_strlist_at(const sm_strlist_t* sl, size_t i);

/* Release a list. Passing NULL is a no-op. */
void sm_strlist_free(sm_strlist_t* sl);

#ifdef __cplusplus
}
#endif

#endif /* SM_H */
