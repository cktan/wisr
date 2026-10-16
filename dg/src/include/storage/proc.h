/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"

/*
 * A snapshot's array of subtransaction XIDs is sized by this for each proc,
 * as PostgreSQL sized each PGPROC's cache of them; digres keeps no cache, as
 * no XID is ever assigned, and a snapshot's array stays empty.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64

/* configurable options */
extern thread_local /*LOCAL*/ PGDLLIMPORT int StatementTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int LockTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int TransactionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleSessionTimeout;

/*
 * Function Prototypes
 */
extern void InitProcess(void);
extern void InitProcessPhase2(void);

extern void ProcReleaseLocks(bool isCommit);

#endif /* _PROC_H_ */
