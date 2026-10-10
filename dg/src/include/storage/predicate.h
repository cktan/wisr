/*-------------------------------------------------------------------------
 *
 * predicate.h
 *	  POSTGRES public predicate locking definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 * GUC variables
 */
extern thread_local /*LOCAL*/ PGDLLIMPORT int max_predicate_locks_per_xact;
extern thread_local /*LOCAL*/ PGDLLIMPORT int max_predicate_locks_per_relation;
extern thread_local /*LOCAL*/ PGDLLIMPORT int max_predicate_locks_per_page;

/*
 * function prototypes
 */

/* housekeeping for shared memory predicate lock structures */
extern void PredicateLockShmemInit(void);

/* predicate lock reporting */
extern bool PageIsPredicateLocked(Relation relation, BlockNumber blkno);

/* predicate lock maintenance */
extern Snapshot GetSerializableTransactionSnapshot(Snapshot snapshot);
extern void SetSerializableTransactionSnapshot(Snapshot snapshot,
                                               VirtualTransactionId *sourcevxid,
                                               int sourcepid);
extern void RegisterPredicateLockingXid(TransactionId xid);
extern void PredicateLockRelation(Relation relation, Snapshot snapshot);
extern void PredicateLockPage(Relation relation, BlockNumber blkno,
                              Snapshot snapshot);
extern void PredicateLockTID(Relation relation, ItemPointer tid,
                             Snapshot snapshot, TransactionId tuple_xid);
extern void PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
                                   BlockNumber newblkno);
extern void PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
                                     BlockNumber newblkno);
extern void TransferPredicateLocksToHeapRelation(Relation relation);
extern void ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe);

/* conflict detection (may also trigger rollback) */
extern bool CheckForSerializableConflictOutNeeded(Relation relation,
                                                  Snapshot snapshot);
extern void CheckForSerializableConflictOut(Relation relation,
                                            TransactionId xid,
                                            Snapshot snapshot);
extern void CheckForSerializableConflictIn(Relation relation, ItemPointer tid,
                                           BlockNumber blkno);
extern void CheckTableForSerializableConflictIn(Relation relation);

/* final rollback checking */
extern void PreCommit_CheckForSerializationFailure(void);

#endif /* PREDICATE_H */
