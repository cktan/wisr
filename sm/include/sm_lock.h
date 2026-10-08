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
 */
#ifndef SM_LOCK_H
#define SM_LOCK_H

typedef enum sm_lockmode_t {
    SM_LOCKMODE_SH  = 0, /* S:   reads it, and all below it */
    SM_LOCKMODE_EX  = 1, /* X:   no one else holds it in any mode */
    SM_LOCKMODE_IS  = 2, /* IS:  will read below it */
    SM_LOCKMODE_IX  = 3, /* IX:  will write below it */
    SM_LOCKMODE_SIX = 4  /* SIX: S, and will write below it */
} sm_lockmode_t;

#endif /* SM_LOCK_H */
