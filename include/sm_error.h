/*
 * sm_error.h -- the error codes of the whole wisr stack, in C.
 *
 * One errno namespace serves every level (DESIGN.md section 5):
 * sm1, sm2, sm3 and the C wrapper all report through sm1's
 * thread-local error state, and every code any of them uses is
 * defined here. sm1.h includes this file and names the enum
 * sm1::errnum_t; sm.h includes it for C callers. It belongs to
 * no level, so it lives at the root.
 *
 * Values are explicit and stable: new codes are appended,
 * grouped under a comment naming the level that produces them,
 * and existing values never change -- a number matters once
 * PostgreSQL sees it.
 */
#ifndef SM_ERROR_H
#define SM_ERROR_H

typedef enum sm_errnum_t {
    SM_OK                = 0,

    /* caller/programming errors */
    SM_ERR_INVALID       = 1,   /* bad argument */
    SM_ERR_NOTRANS       = 2,   /* op requires an active transaction */
    SM_ERR_INTRANS       = 3,   /* op forbidden inside a transaction */
    SM_ERR_PREPARED      = 4,   /* op forbidden on a prepared transaction */
    SM_ERR_NOSAVEPOINT   = 5,   /* rollback with no savepoint set */

    /* data errors */
    SM_ERR_NOTFOUND      = 6,   /* store or key does not exist */
    SM_ERR_DUPKEY        = 7,   /* insert onto a live key */
    SM_ERR_EXISTS        = 8,   /* home dir, database, or store exists */
    SM_ERR_INUSE         = 9,   /* database or sessions still open */

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
    SM_ERR_VERSION       = 18,  /* home dir format not supported */

    /* Not an abort: the transaction stays active (SM1_DESIGN.md §5.4). */
    SM_ERR_UPDATED       = 19,  /* newer commit; sm1::read_latest, then retry */

    /* sm2's: types and tuples (sm2/SM2_DESIGN.md §3.2, §4, §5) */
    SM_ERR_KEYTOOLONG    = 20,  /* a key that may not be cut is too long */
    SM_ERR_ROWTOOLONG    = 22,  /* a tuple past sm2::max_tup_len */

    /*
     * The C wrapper's: an exception other than std::bad_alloc
     * reached its catch-all, which means a bug; the session's
     * transaction has been aborted (DESIGN.md section 5).
     */
    SM_ERR_EXCEPTION     = 21,

    /*
     * sm1's, and not an abort either: a lock another transaction
     * holds, met by a session set qa_nowait (SM1_DESIGN.md §5.1).
     */
    SM_ERR_BLOCKED       = 23,  /* would wait for a lock; repeat the call */

    /* sm3's: constraints (sm3/SM3_DESIGN.md §3) */
    SM_ERR_NOTNULL       = 24,  /* a NULL in a NOT NULL column */

    /*
     * sm1's: a rollback to a savepoint whose undo list was
     * dropped at savepoint_undo_bytes (SM1_DESIGN.md §6.5). The
     * transaction can only be aborted.
     */
    SM_ERR_UNDOLIMIT     = 25,  /* the savepoint can no longer be rolled back to */

    /* The highest code, for code that walks them all. */
    SM_ERR_LAST          = SM_ERR_UNDOLIMIT
} sm_errnum_t;

#endif /* SM_ERROR_H */
