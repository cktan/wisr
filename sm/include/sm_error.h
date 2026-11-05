/*
 * sm_error.h -- the error codes of the whole wisr stack, in C.
 *
 * One errno namespace serves every level (DESIGN.md section 5):
 * every level and the C wrapper report through sm0's
 * thread-local error state, and every code any of them uses is
 * defined here. sm0.h and sm1.h include this file and name the
 * enum sm0::errnum_t and sm1::errnum_t; sm.h includes it for C
 * callers. It belongs to no level, so it lives at the root.
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

    /* Not an abort: the transaction stays active (../LOCKMGR.md §1.9). */
    SM_ERR_UPDATED       = 19,  /* newer commit; sm1::read_latest, then retry */

    /* sm2's: types and tuples (../LSMINDEX.md §2.4, ../sm2/TUPLE.md §10, ../ROWSTORE.md §8) */
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
     * holds, met by a session set qa_nowait (../LOCKMGR.md §3.2).
     */
    SM_ERR_BLOCKED       = 23,  /* would wait for a lock; repeat the call */

    /* sm3's: constraints (../INDEXBUILD.md §2) */
    SM_ERR_NOTNULL       = 24,  /* a NULL in a NOT NULL column */

    /*
     * sm1's: a rollback to a savepoint whose undo list was
     * dropped at savepoint_undo_bytes (../XCTMGR.md §6). The
     * transaction can only be aborted.
     */
    SM_ERR_UNDOLIMIT     = 25,  /* the savepoint can no longer be rolled back to */

    /*
     * sm1's, and not an abort: an update or delete of a row the
     * running statement has already changed (../XCTMGR.md §5).
     * Nothing is written; the caller skips the row.
     */
    SM_ERR_SELFMODIFIED  = 26,  /* the statement already changed this row */

    /*
     * sm1's, and not an abort: a write in a transaction begun
     * read-only (../XCTMGR.md §7.5). Nothing is written.
     */
    SM_ERR_READONLY      = 27,  /* the transaction was declared read-only */

    /*
     * sm1's, and not an abort: a wait for another transaction that
     * an interrupt to the calling thread ended (sm_sys.h, through its
     * wait mask; ../LOCKMGR.md §3.2). The transaction stays active
     * and holds what it held.
     */
    SM_ERR_INTERRUPTED   = 28,  /* the wait was interrupted */

    /*
     * sm0's: no file descriptor to be had -- the process's ceiling,
     * a cache's share, or the system's limit (EMFILE, ENFILE) is
     * reached, and nothing could be closed to make room (sm_vfd.h).
     */
    SM_ERR_NOFDS         = 29,  /* out of file descriptors */

    /*
     * sm1's: a rollback to a savepoint failed, leaving the writes
     * it was to undo (../XCTMGR.md §6). Every call on the
     * transaction but an abort then fails with it.
     */
    SM_ERR_DOOMED        = 30,  /* the transaction can only be aborted */

    /*
     * sm1's, and not an abort: a read or seek on a cursor a
     * rollback to a savepoint set before it opened has closed
     * (../XCTMGR.md §6). Its old view cannot be recreated.
     */
    SM_ERR_SCANCLOSED    = 31,  /* a rollback closed the cursor */

    /*
     * sm1's, and not an abort: an update, a delete or a newest
     * read of a key whose newest version is a move's tombstone
     * (sm1::erase_moved), the row living on under another key
     * (../ROWSTORE.md §2.5).
     */
    SM_ERR_MOVED         = 32,  /* the row moved to another key */

    /* The highest code, for code that walks them all. */
    SM_ERR_LAST          = SM_ERR_MOVED
} sm_errnum_t;

#endif /* SM_ERROR_H */
