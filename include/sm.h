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
 * An environment: one process's handle on a home directory,
 * through which every database and session call goes.
 */
typedef struct sm_env_t sm_env_t;

/*
 * Make homedir -- which must already exist -- a wisr home
 * directory: create global/ and base/, the root db inside
 * global/, and the format version inside that.
 *
 * Call it once in a home directory's life, not once per
 * process. It takes no lock and opens nothing. Returns 0, or
 * -1 with SM_ERR_EXISTS if homedir is already a wisr home.
 */
int sm_init(const char* homedir);

/*
 * Create an environment on a home directory sm_init() has
 * already prepared, taking the pidlock on it for as long as
 * the environment lives. Opens no database.
 *
 * Creates nothing: a homedir that was never initialized is an
 * error, not a new home directory. Returns NULL on error.
 */
sm_env_t* sm_attach(const char* homedir);

/*
 * Release an environment and its pidlock, leaving the home
 * directory intact. This undoes sm_attach(), not sm_init().
 *
 * Returns -1 with SM_ERR_INUSE if any session opened through
 * env is still open, and env stays valid. NULL is a no-op
 * returning 0.
 */
int sm_detach(sm_env_t* env);

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
