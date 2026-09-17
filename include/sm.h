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
 * Error codes. Values are explicit and stable: new codes are
 * appended, existing values never change.
 */
typedef enum {
    SM_OK                = 0,

    /* caller/programming errors */
    SM_ERR_INVALID       = 1,   /* bad argument */
    SM_ERR_NOTRANS       = 2,   /* op requires an active transaction */
    SM_ERR_INTRANS       = 3,   /* op forbidden inside a transaction */
    SM_ERR_PREPARED      = 4,   /* op forbidden on a prepared transaction */
    SM_ERR_NOSAVEPOINT   = 5,   /* rollback with no savepoint set */

    /* data errors */
    SM_ERR_NOTFOUND      = 6,   /* relation or key does not exist */
    SM_ERR_DUPKEY        = 7,   /* insert onto a live key */
    SM_ERR_EXISTS        = 8,   /* home dir, database, or relation exists */
    SM_ERR_INUSE         = 9,   /* database open, or homedir locked */

    /* concurrency errors -- RETRYABLE */
    SM_ERR_CONFLICT      = 10,  /* write-write conflict */
    SM_ERR_SERIALIZATION = 11,  /* SSI validation failure */
    SM_ERR_DEADLOCK      = 12,  /* chosen as deadlock victim */

    /* environment errors */
    SM_ERR_CORRUPT       = 13,
    SM_ERR_IO            = 14,
    SM_ERR_NOMEM         = 15,
    SM_ERR_DISKFULL      = 16,
    SM_ERR_PERMISSION    = 17,
    SM_ERR_VERSION       = 18   /* home dir format not supported */
} sm_errno_t;

/*
 * The last error raised on the calling thread. Meaningful
 * only immediately after a call that returned an error
 * indication: a successful call leaves it untouched, so it
 * cannot be used to test for success.
 */
sm_errno_t sm_errno(void);

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
