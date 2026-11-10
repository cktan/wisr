/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to set up and tear down a session (dg/WORKNOTE.md step 159)
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/proc.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * A session had a PGPROC, a slot in shared memory, which the next session
 * took over.  What other sessions read of it is in its record
 * (storage/session_registry.h), and the rest is its own: its number is its
 * thread's id, and its transaction's local id and xmin are its tcb's.
 *
 * ProcReleaseLocks -- frees the locks associated with current transaction
 *
 * ProcKill -- forgets the session's number as it ends.
 */
#include "postgres.h"


#include "miscadmin.h"
#include "storage/advisory_lock.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/session_registry.h"

/* GUC variables */
thread_local /*LOCAL*/ int LockTimeout = 0;
/* How long a wait in sm lasts before it looks for a deadlock (smstore.c). */
thread_local /*LOCAL*/ int DeadlockTimeout = 50;
thread_local /*LOCAL*/ int IdleInTransactionSessionTimeout = 0;
thread_local /*LOCAL*/ int TransactionTimeout = 0;
thread_local /*LOCAL*/ int IdleSessionTimeout = 0;

static void RemoveProcFromArray(int code, Datum arg);
static void ProcKill(int code, Datum arg);

/*
 * InitProcess -- initialize this session's identity, before its record
 */
void InitProcess(void) {
  if (MyProcNumber != INVALID_PROC_NUMBER)
    elog(ERROR, "you already exist");

  /* A session's number is its thread's id, unique while it runs. */
  MyProcNumber = MyProcPid;

  /*
   * Arrange to clean up at backend exit.
   */
  on_shmem_exit(ProcKill, 0);
}

/*
 * InitProcessPhase2 -- register this session, for other sessions to find.
 */
void InitProcessPhase2(void) {
  Assert(MyProcNumber != INVALID_PROC_NUMBER);

  /*
   * Add our record to the session registry: other sessions find this one
   * by it (session_registry.c).
   */
  session_register();

  /*
   * Arrange to clean that up at backend exit.
   */
  on_shmem_exit(RemoveProcFromArray, 0);
}

/*
 * ProcReleaseLocks() -- release locks associated with current transaction
 *			at main transaction commit or abort
 *
 * The only locks digres holds here are advisory locks
 * (storage/advisory_lock.h): a table's are sm's, which its transaction's
 * end releases.  Advisory locks are released only if they are
 * transaction-level; session-level holds remain, whether this is a commit
 * or not.  A subtransaction's are released or handed to its parent as it
 * ends (advisory_subxact_end()).
 */
void ProcReleaseLocks(bool isCommit) {
  /* Release transaction-level advisory locks */
  advisory_xact_end();
}

/*
 * RemoveProcFromArray() -- Remove this session from the session registry.
 */
static void RemoveProcFromArray(int code, Datum arg) { session_unregister(); }

/*
 * ProcKill() -- Forget this session's number, as it ends.
 */
static void ProcKill(int code, Datum arg) {
  /* not safe if forked by system(), etc. */
  if (MyProcPid != CurrentProcPid())
    elog(PANIC, "ProcKill() called in child process");

  MyProcNumber = INVALID_PROC_NUMBER;
}
