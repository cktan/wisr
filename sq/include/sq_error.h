/*
 * sq_error.h -- sq's error codes, in C.
 *
 * A code says what kind of thing went wrong, and there are few.
 * What was wrong with the SQL is in the SQLSTATE, read with
 * sq_sqlstate(): PostgreSQL's five characters, which a server
 * sends to its client as they are.
 *
 * Values are explicit and stable: new codes are appended, and
 * existing values never change.
 */
#ifndef SQ_ERROR_H
#define SQ_ERROR_H

typedef enum sq_errnum_t {
    SQ_OK            = 0,

    SQ_ERR_PARSE     = 1,   /* the SQL was refused; see sq_sqlstate() */
    SQ_ERR_NOMEM     = 2,   /* out of memory */

    /*
     * An exception other than std::bad_alloc reached a call's
     * catch-all, which means a bug.
     */
    SQ_ERR_EXCEPTION = 3,

    /* The highest code; move it when one is appended. */
    SQ_ERR_LAST      = SQ_ERR_EXCEPTION
} sq_errnum_t;

#endif /* SQ_ERROR_H */
