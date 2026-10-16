/*-------------------------------------------------------------------------
 *
 * lmgr.c
 *	  POSTGRES lock manager code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/smstore.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/inval.h"

/*
 * Per-backend counter for generating speculative insertion tokens.
 *
 * This may wrap around, but that's OK as it's only used for the short
 * duration between inserting a tuple and checking that there are no (unique)
 * constraint violations.  It's theoretically possible that a backend sees a
 * tuple that was speculatively inserted by another backend, but before it has
 * started waiting on the token, the other backend completes its insertion,
 * and then performs 2^32 unrelated insertions.  And after all that, the
 * first backend finally calls SpeculativeInsertionLockAcquire(), with the
 * intention of waiting for the first insertion to complete, but ends up
 * waiting for the latest unrelated insertion instead.  Even then, nothing
 * particularly bad happens: in the worst case they deadlock, causing one of
 * the transactions to abort.
 */
static thread_local /*LOCAL*/ uint32 speculativeInsertionToken = 0;

/*
 * RelationInitLockInfo
 *		Initializes the lock information in a relation descriptor.
 *
 *		relcache.c must call this during creation of any reldesc.
 */
void RelationInitLockInfo(Relation relation) {
  Assert(RelationIsValid(relation));
  Assert(OidIsValid(RelationGetRelid(relation)));

  relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

  if (relation->rd_rel->relisshared)
    relation->rd_lockInfo.lockRelId.dbId = InvalidOid;
  else
    relation->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
}

/*
 * Data structures defining the semantics of the standard lock methods.
 *
 * The conflict table defines the semantics of the various lock modes.
 */
static const LOCKMASK LockConflicts[] = {
    0,

    /* AccessShareLock */
    LOCKBIT_ON(AccessExclusiveLock),

    /* RowShareLock */
    LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

    /* RowExclusiveLock */
    LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
        LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

    /* ShareUpdateExclusiveLock */
    LOCKBIT_ON(ShareUpdateExclusiveLock) | LOCKBIT_ON(ShareLock) |
        LOCKBIT_ON(ShareRowExclusiveLock) | LOCKBIT_ON(ExclusiveLock) |
        LOCKBIT_ON(AccessExclusiveLock),

    /* ShareLock */
    LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
        LOCKBIT_ON(ShareRowExclusiveLock) | LOCKBIT_ON(ExclusiveLock) |
        LOCKBIT_ON(AccessExclusiveLock),

    /* ShareRowExclusiveLock */
    LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
        LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
        LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

    /* ExclusiveLock */
    LOCKBIT_ON(RowShareLock) | LOCKBIT_ON(RowExclusiveLock) |
        LOCKBIT_ON(ShareUpdateExclusiveLock) | LOCKBIT_ON(ShareLock) |
        LOCKBIT_ON(ShareRowExclusiveLock) | LOCKBIT_ON(ExclusiveLock) |
        LOCKBIT_ON(AccessExclusiveLock),

    /* AccessExclusiveLock */
    LOCKBIT_ON(AccessShareLock) | LOCKBIT_ON(RowShareLock) |
        LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
        LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
        LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock)

};

/*
 * Given two lock modes, return whether they would conflict.
 */
bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2) {
  if (LockConflicts[mode1] & LOCKBIT_ON(mode2))
    return true;

  return false;
}

/*
 * Whether relid is locked by sm: a relation in sm, any a user made
 * (access/smstore.h).  sm locks it as it is opened, and its records as they
 * are read and written, so a lock asked of this lock manager is held as far
 * as anyone asks.  A lock request takes sm's lock on the table
 * (smstore_lock_relation()), then the invalidation messages waiting, as
 * acquiring a lock does: the definition is read after any DDL on it ends.
 *
 * Any other relation is a catalog, compiled in, whose rows never change
 * (catalog/indexing.c refuses each write): no lock guards anything of it, so
 * none is taken, and one asked for is held.  The request still takes the
 * invalidation messages waiting.
 */
static inline bool locked_by_sm(Oid relid) {
  return relid >= FirstNormalObjectId;
}

/*
 *		LockRelationOid
 *
 * Lock a relation given only its OID.  This should generally be used
 * before attempting to open the relation's relcache entry.
 */
void LockRelationOid(Oid relid, LOCKMODE lockmode) {
  /* sm's lock, for a table of sm's: a catalog takes none. */
  if (locked_by_sm(relid))
    smstore_lock_relation(relid, lockmode);
  AcceptInvalidationMessages();
}

/*
 *		ConditionalLockRelationOid
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns true iff the lock was acquired.
 *
 * NOTE: we do not currently need conditional versions of all the
 * LockXXX routines in this file, but they could easily be added if needed.
 */
bool ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode) {
  /* sm's lock, if it is free now: else nothing is taken.  A catalog's is. */
  if (locked_by_sm(relid) && !smstore_trylock_relation(relid, lockmode))
    return false;
  AcceptInvalidationMessages();
  return true;
}

/*
 *		LockRelationId
 *
 * Lock, given a LockRelId.  Same as LockRelationOid but take LockRelId as an
 * input.
 */
void LockRelationId(LockRelId *relid, LOCKMODE lockmode) {
  /* sm's lock, for a table of sm's: a catalog takes none. */
  if (locked_by_sm(relid->relId))
    smstore_lock_relation(relid->relId, lockmode);
  AcceptInvalidationMessages();
}

/*
 *		UnlockRelationId
 *
 * Unlock, given a LockRelId.  This is preferred over UnlockRelationOid
 * for speed reasons.
 */
void UnlockRelationId(LockRelId *relid, LOCKMODE lockmode) {
  /* sm's lock is held to the transaction's end; a catalog has none. */
}

/*
 *		UnlockRelationOid
 *
 * Unlock, given only a relation Oid.  Use UnlockRelationId if you can.
 */
void UnlockRelationOid(Oid relid, LOCKMODE lockmode) {
  /* sm's lock is held to the transaction's end; a catalog has none. */
}

/*
 *		LockRelation
 *
 * This is a convenience routine for acquiring an additional lock on an
 * already-open relation.  Never try to do "relation_open(foo, NoLock)"
 * and then lock with this.
 */
void LockRelation(Relation relation, LOCKMODE lockmode) {
  /* sm's lock, for a table of sm's: a catalog takes none. */
  if (locked_by_sm(RelationGetRelid(relation)))
    smstore_lock_relation(RelationGetRelid(relation), lockmode);
  AcceptInvalidationMessages();
}

/*
 *		CheckRelationLockedByMe
 *
 * Returns true if current transaction holds a lock on 'relation' of mode
 * 'lockmode'.  If 'orstronger' is true, a stronger lockmode is also OK.
 * ("Stronger" is defined as "numerically higher", which is a bit
 * semantically dubious but is OK for the purposes we use this for.)
 */
bool CheckRelationLockedByMe(Relation relation, LOCKMODE lockmode,
                             bool orstronger) {
  /* sm's lock, in a mode covering lockmode's; a catalog's is held. */
  return !locked_by_sm(RelationGetRelid(relation)) ||
         smstore_relation_held(RelationGetRelid(relation), lockmode);
}

/*
 *		CheckRelationOidLockedByMe
 *
 * Like the above, but takes an OID as argument.
 */
bool CheckRelationOidLockedByMe(Oid relid, LOCKMODE lockmode, bool orstronger) {
  /* sm's lock, in a mode covering lockmode's; a catalog's is held. */
  return !locked_by_sm(relid) || smstore_relation_held(relid, lockmode);
}

/*
 *		LockRelationIdForSession
 *
 * This routine grabs a session-level lock on the target relation.  The
 * session lock persists across transaction boundaries.  It will be removed
 * when UnlockRelationIdForSession() is called, or if an ereport(ERROR) occurs,
 * or if the backend exits.
 *
 * Note that one should also grab a transaction-level lock on the rel
 * in any transaction that actually uses the rel, to ensure that the
 * relcache entry is up to date.
 */
void LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode) {
  /* sm holds no lock for a session; a catalog needs none. */
}

/*
 *		UnlockRelationIdForSession
 */
void UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode) {}

/*
 *		LockTuple
 *
 * Obtain a tuple-level lock.  This is used in a less-than-intuitive fashion
 * because we can't afford to keep a separate lock in shared memory for every
 * tuple.  See heap_lock_tuple before using this!
 */
void LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode) {
  /* Only a catalog's row is locked so, for an inplace update: none is. */
}

/*
 *		ConditionalLockTuple
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns true iff the lock was acquired.
 */
bool ConditionalLockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode,
                          bool logLockFailure) {
  /* As LockTuple: no catalog row changes, so the lock is had. */
  return true;
}

/*
 *		UnlockTuple
 */
void UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode) {}

/*
 *		XactLockTableWait
 *
 * Wait for the specified transaction to commit or abort.  If an operation
 * is specified, an error context callback is set up.  If 'oper' is passed as
 * None, no error context callback is set up.
 *
 * Note that this does the right thing for subtransactions: if we wait on a
 * subtransaction, we will exit as soon as it aborts or its top parent commits.
 * It takes some extra work to ensure this, because to save on shared memory
 * the XID lock of a subtransaction is released when it ends, whether
 * successfully or unsuccessfully.  So we have to check if it's "still running"
 * and if so wait for its parent.
 */
void XactLockTableWait(TransactionId xid, Relation rel, ItemPointer ctid,
                       XLTW_Oper oper) {
  /*
   * Nothing to wait for: no transaction id is locked, and no transaction
   * but the session's own is in progress to it (TransactionIdIsInProgress()),
   * which a caller never waits for.  A row's writers are sm's to wait for.
   */
}

/*
 *		SpeculativeInsertionLockAcquire
 *
 * Insert a lock showing that the given transaction ID is inserting a tuple,
 * but hasn't yet decided whether it's going to keep it.  The lock can then be
 * used to wait for the decision to go ahead with the insertion, or aborting
 * it.
 *
 * The token is used to distinguish multiple insertions by the same
 * transaction.  It is returned to caller.
 */
uint32 SpeculativeInsertionLockAcquire(TransactionId xid) {
  speculativeInsertionToken++;

  /*
   * Check for wrap-around. Zero means no token is held, so don't use that.
   */
  if (speculativeInsertionToken == 0)
    speculativeInsertionToken = 1;

  /*
   * No lock: only a catalog's insert is speculative, as sm's is sm's
   * (ExecInsert()), and a catalog's insert is refused, so no other session
   * would wait for it.
   */
  return speculativeInsertionToken;
}

/*
 *		SpeculativeInsertionLockRelease
 *
 * Delete the lock showing that the given transaction is speculatively
 * inserting a tuple.
 */
void SpeculativeInsertionLockRelease(TransactionId xid) {}

/*
 *		SpeculativeInsertionWait
 *
 * Wait for the specified transaction to finish or abort the insertion of a
 * tuple.
 */
void SpeculativeInsertionWait(TransactionId xid, uint32 token) {
  /* No speculative insertion holds a lock to wait for. */
}

/*
 * WaitForLockersMultiple
 *		Wait until no transaction holds locks that conflict with the
 *given locktags at the given lockmode.
 *
 * To do this, obtain the current list of lockers, and wait on their VXIDs
 * until they are finished.
 *
 * Note we don't try to acquire the locks on the given locktags, only the
 * VXIDs and XIDs of their lock holders; if somebody grabs a conflicting lock
 * on the objects after we obtained our initial list of lockers, we will not
 * wait for them.
 */
void WaitForLockersMultiple(List *locktags, LOCKMODE lockmode) {
  /*
   * No locker to wait for: no session holds a lock of this lock manager on
   * a relation, as a table of sm's is locked by sm and a catalog by none.
   */
}

/*
 * WaitForLockers
 *
 * Same as WaitForLockersMultiple, for a single lock tag.
 */
void WaitForLockers(LOCKTAG heaplocktag, LOCKMODE lockmode) {
  WaitForLockersMultiple(list_make1(&heaplocktag), lockmode);
}

/*
 * Object locks, of a database's objects and of shared ones, are no-ops: no
 * object in digres needs one here.  A built-in is compiled in and never
 * changes, and every other object -- a namespace, a table, the database a
 * session opens -- is sm's, which locks it itself (PLAN.md section 7).
 */

/*
 *		LockDatabaseObject
 *
 * Obtain a lock on a general object of the current database.  Don't use
 * this for shared objects (such as tablespaces).  It's unwise to apply it
 * to relations, also, since a lock taken this way will NOT conflict with
 * locks taken via LockRelation and friends.
 */
void LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
                        LOCKMODE lockmode) {
  /* Taking a lock still takes the invalidation messages waiting. */
  AcceptInvalidationMessages();
}

/*
 *		UnlockDatabaseObject
 */
void UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
                          LOCKMODE lockmode) {}

/*
 *		LockSharedObject
 *
 * Obtain a lock on a shared-across-databases object.
 */
void LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                      LOCKMODE lockmode) {
  /* Taking a lock still takes the invalidation messages waiting. */
  AcceptInvalidationMessages();
}

/*
 *		ConditionalLockSharedObject
 *
 * As above, but only lock if we can get the lock without blocking.
 * Returns true iff the lock was acquired.
 */
bool ConditionalLockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                                 LOCKMODE lockmode) {
  /* Taking a lock still takes the invalidation messages waiting. */
  AcceptInvalidationMessages();
  return true;
}

/*
 *		UnlockSharedObject
 */
void UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                        LOCKMODE lockmode) {}

/*
 *		LockSharedObjectForSession
 *
 * Obtain a session-level lock on a shared-across-databases object.
 * See LockRelationIdForSession for notes about session-level locks.
 */
void LockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
                                LOCKMODE lockmode) {}

/*
 *		UnlockSharedObjectForSession
 */
void UnlockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
                                  LOCKMODE lockmode) {}
