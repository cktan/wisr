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

extern PGPROC *ProcNumberGetProc(int procNumber);
extern PGPROC *BackendPidGetProc(int pid);

extern VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool excludeXmin0, bool allDbs,
                      int excludeVacuum, int *nvxids);

extern bool CountOtherDBBackends(Oid databaseId, int *nbackends,
                                 int *nprepared);
extern void TerminateOtherDBBackends(Oid databaseId);

#endif /* PROCARRAY_H */
