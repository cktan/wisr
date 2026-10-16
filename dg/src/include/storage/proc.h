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

/* configurable options */
extern thread_local /*LOCAL*/ PGDLLIMPORT int StatementTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int LockTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int DeadlockTimeout;
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
