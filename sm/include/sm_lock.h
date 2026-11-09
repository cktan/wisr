/*
 * sm_lock.h -- the lock modes of the whole wisr stack, in C.
 *
 * One set of modes serves every level, as sm_error.h's codes do
 * (DESIGN.md section 5): sm1 takes them, sm2 and sm3 pass them
 * through, and the C wrapper hands them to its callers. sm1.h
 * includes this file and names the enum sm1::lockmode_t; sm.h
 * includes it for C callers. It belongs to no level, so it
 * lives at the root, and the two never need keeping in step.
 *
 * Values are explicit and stable: they are persisted in a
 * prepared transaction's lock set, so they never change.
 *
 * Gray's five, for locking at several granularities. The
 * intention modes lock a name that stands for a larger object
 * -- a table, say -- to announce locks on the rows below it;
 * sm1 keeps no hierarchy itself, only this table on each name.
 *
 *              IS   IX   S    SIX  X
 *      IS      ok   ok   ok   ok   --
 *      IX      ok   ok   --   --   --
 *      S       ok   --   ok   --   --
 *      SIX     ok   --   --   --   --
 *      X       --   --   --   --   --
 *
 * And two modes of a row alone, PostgreSQL's FOR KEY SHARE and FOR NO
 * KEY UPDATE (LOCKMGR.md section 1.13): a row is held KS, S, NKU or X.
 *
 *              KS   S    NKU  X
 *      KS      ok   ok   ok   --
 *      S       ok   ok   --   --
 *      NKU     ok   --   --   --
 *      X       --   --   --   --
 */
#ifndef SM_LOCK_H
#define SM_LOCK_H

typedef enum sm_lockmode_t {
    SM_LOCKMODE_SH  = 0, /* S:   reads it, and all below it */
    SM_LOCKMODE_EX  = 1, /* X:   no one else holds it in any mode */
    SM_LOCKMODE_IS  = 2, /* IS:  will read below it */
    SM_LOCKMODE_IX  = 3, /* IX:  will write below it */
    SM_LOCKMODE_SIX = 4, /* SIX: S, and will write below it */
    SM_LOCKMODE_KS  = 5, /* KS:  a row's key stays: FOR KEY SHARE */
    SM_LOCKMODE_NKU = 6  /* NKU: a row changes, its key does not */
} sm_lockmode_t;

#ifdef __cplusplus
/*
 * Notify the host of a lock wait. Called with start=true once a
 * conflict requires a wait. Return true to request a paired call
 * with start=false when the wait ends; its return is ignored.
 * Both calls hold no lock mutex. Timers in sm_sys.h are safe,
 * but do not reenter the storage engine. Neither call may throw.
 */
typedef bool (*sm_lock_wait_fn)(void *arg, bool start) noexcept;
#endif

#endif /* SM_LOCK_H */
