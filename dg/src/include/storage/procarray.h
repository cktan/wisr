/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procarray.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

extern Size ProcArrayShmemSize(void);
extern void ProcArrayShmemInit(void);
extern void ProcArrayAdd(PGPROC *proc);
extern void ProcArrayRemove(PGPROC *proc, TransactionId latestXid);

extern void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid);

extern int GetMaxSnapshotXidCount(void);
extern int GetMaxSnapshotSubxidCount(void);

extern Snapshot GetSnapshotData(Snapshot snapshot);

extern bool ProcArrayInstallImportedXmin(TransactionId xmin,
                                         VirtualTransactionId *sourcevxid);
extern bool ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc);

extern bool TransactionIdIsInProgress(TransactionId xid);
extern bool TransactionIdIsActive(TransactionId xid);
extern TransactionId GetOldestNonRemovableTransactionId(Relation rel);
extern TransactionId GetOldestTransactionIdConsideredRunning(void);
extern void GetReplicationHorizons(TransactionId *xmin,
                                   TransactionId *catalog_xmin);

extern PGPROC *ProcNumberGetProc(int procNumber);
extern void ProcNumberGetTransactionIds(int procNumber, TransactionId *xid,
                                        TransactionId *xmin, int *nsubxid,
                                        bool *overflowed);
extern PGPROC *BackendPidGetProc(int pid);
extern PGPROC *BackendPidGetProcWithLock(int pid);
extern int BackendXidGetPid(TransactionId xid);
extern bool IsBackendPid(int pid);

extern VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool excludeXmin0, bool allDbs,
                      int excludeVacuum, int *nvxids);

extern int CountDBBackends(Oid databaseid);
extern int CountDBConnections(Oid databaseid);
extern int CountUserBackends(Oid roleid);
extern bool CountOtherDBBackends(Oid databaseId, int *nbackends,
                                 int *nprepared);
extern void TerminateOtherDBBackends(Oid databaseId);

extern void XidCacheRemoveRunningXids(TransactionId xid, int nxids,
                                      const TransactionId *xids,
                                      TransactionId latestXid);

extern void ProcArraySetReplicationSlotXmin(TransactionId xmin,
                                            TransactionId catalog_xmin,
                                            bool already_locked);

extern void ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
                                            TransactionId *catalog_xmin);

#endif /* PROCARRAY_H */
