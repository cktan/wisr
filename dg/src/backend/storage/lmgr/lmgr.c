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

