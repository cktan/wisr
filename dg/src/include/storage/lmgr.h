/*-------------------------------------------------------------------------
 *
 * lmgr.h
 *	  POSTGRES lock manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/rel.h"

extern void RelationInitLockInfo(Relation relation);

/* Lock a relation */
extern void LockRelationOid(Oid relid, LOCKMODE lockmode);
extern void LockRelationId(LockRelId *relid, LOCKMODE lockmode);
extern bool ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode);

extern void LockRelation(Relation relation, LOCKMODE lockmode);
extern bool CheckRelationLockedByMe(Relation relation, LOCKMODE lockmode,
                                    bool orstronger);
extern bool CheckRelationOidLockedByMe(Oid relid, LOCKMODE lockmode,
                                       bool orstronger);

/* A token for a speculative insertion, which no one waits for */

/* Lock a general object (other than a relation) of the current database */
extern void LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
                               LOCKMODE lockmode);

/* Lock a shared-across-databases object (other than a relation) */
extern void LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                             LOCKMODE lockmode);
extern bool ConditionalLockSharedObject(Oid classid, Oid objid, uint16 objsubid,
                                        LOCKMODE lockmode);

#endif /* LMGR_H */
