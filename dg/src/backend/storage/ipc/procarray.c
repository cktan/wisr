/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES process array code.
 *
 *
 * PostgreSQL's process array, which told which transactions were running
 * by scanning every backend's PGPROC.  In digres no PostgreSQL xid is ever
 * assigned, so a snapshot is a constant (GetSnapshotData()), and a session
 * is found by its record in the session registry (session_registry.c,
 * dg/SHMEM.md section 5.2), not by a scan of PGPROC slots under a lock.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procarray.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_authid.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "storage/advisory_lock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/session_registry.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))


/*
 * Cache to reduce overhead of repeated calls to TransactionIdIsInProgress()
 */
static thread_local /*LOCAL*/ TransactionId cachedXidIsNotInProgress =
    InvalidTransactionId;

/* Size of the ProcArray structure itself */
#define PROCARRAY_MAXPROCS	(MaxBackends)

/*
 * Snapshots are sized to hold every xid and cached subxid of every proc;
 * see GetMaxSnapshotSubxidCount().
 */
#define TOTAL_MAX_CACHED_SUBXIDS \
	((PGPROC_MAX_CACHED_SUBXIDS + 1) * PROCARRAY_MAXPROCS)

/*
 * Initialize the shared PGPROC array during postmaster startup.
 */
void ProcArrayShmemInit(void) {
  /* A snapshot's count of completed transactions starts at 1, and stays. */
  if (!IsUnderPostmaster)
    TransamVariables->xactCompletionCount = 1;
  /* The sessions are in a registry of the pcb's (SHMEM.md section 5.2). */
  session_registry_init();
  /* Advisory locks are in a table of the pcb's, by the sessions' records. */
  advisory_init();
}

/*
 * Add the specified PGPROC to the shared array.
 */
void ProcArrayAdd(PGPROC *proc) {
  /* Other sessions find this one by its record (session_registry.c). */
  session_register(proc);
}

/*
 * Remove the specified PGPROC from the shared array: the session leaves the
 * registry.  latestXid is never valid, as no xid is assigned and there is no
 * two-phase commit.
 */
void ProcArrayRemove(PGPROC *proc, TransactionId latestXid) {
  /* No PostgreSQL xid is ever assigned: none completes here. */
  Assert(!TransactionIdIsValid(latestXid));
  session_unregister();
}

/*
 * ProcArrayEndTransaction -- mark a transaction as no longer running
 *
 * This is used interchangeably for commit and abort cases.  The transaction
 * commit/abort must already be reported to WAL and pg_xact.
 *
 * proc is currently always MyProc, but we pass it explicitly for flexibility.
 * latestXid is the latest Xid among the transaction's main XID and
 * subtransactions, or InvalidTransactionId if it has no XID.  (We must ask
 * the caller to pass latestXid, instead of computing it from the PGPROC's
 * contents, because the subxid information in the PGPROC might be
 * incomplete.)
 */
void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid) {
  /*
   * No PostgreSQL xid is ever assigned (WORKNOTE.md steps 45, 137), so none
   * is cleared.  No other session reads these: they are the session's own.
   */
  Assert(!TransactionIdIsValid(latestXid));
  Assert(!TransactionIdIsValid(proc->xid));

  proc->vxid.lxid = InvalidLocalTransactionId;
  proc->xmin = InvalidTransactionId;
  proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
}

/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 */
bool TransactionIdIsInProgress(TransactionId xid) {
  /*
   * No other session holds a PostgreSQL xid (GetSnapshotData()): only this
   * one's own can be in progress.
   */
  return TransactionIdIsCurrentTransactionId(xid);
}

/*
 * GetMaxSnapshotXidCount -- get max size for snapshot XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int GetMaxSnapshotXidCount(void) { return PROCARRAY_MAXPROCS; }

/*
 * GetMaxSnapshotSubxidCount -- get max size for snapshot sub-XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int GetMaxSnapshotSubxidCount(void) { return TOTAL_MAX_CACHED_SUBXIDS; }

/*
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (highest completed xact ID + 1), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *		All xact IDs < xmin are considered finished.
 *		All xact IDs >= xmax are considered still running.
 *		For an xact ID xmin <= xid < xmax, consult list to see whether
 *		it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * In digres no xid is ever running, so the list is empty and every snapshot
 * is the same (see below).
 *
 * We also update the following backend-global variables:
 *		TransactionXmin: the oldest xmin of any snapshot in use in the
 *			current transaction (this is the same as MyProc->xmin).
 *		RecentXmin: the xmin computed for the most recent snapshot. XIDs
 *			older than this are known not running any more.
 *
 * No horizon is kept: what one serves, heap pruning and VACUUM, does not
 * run.
 *
 * Note: this function should probably not be called with an argument that's
 * not statically allocated (see xip allocation below).
 */
Snapshot GetSnapshotData(Snapshot snapshot) {
  TransactionId xmax;

  Assert(snapshot != NULL);

  /* The arrays, as before, though no xid is ever put in them. */
  if (snapshot->xip == NULL) {
    snapshot->xip = (TransactionId *)malloc(GetMaxSnapshotXidCount() *
                                            sizeof(TransactionId));
    if (snapshot->xip == NULL)
      ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    Assert(snapshot->subxip == NULL);
    snapshot->subxip = (TransactionId *)malloc(GetMaxSnapshotSubxidCount() *
                                               sizeof(TransactionId));
    if (snapshot->subxip == NULL)
      ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
  }

  /*
   * No PostgreSQL transaction id is ever assigned (WORKNOTE.md steps 45 and
   * 137), and what a session sees is sm's snapshot, not this one: so no xid
   * is in progress, and every snapshot is the same, its xmin and xmax the xid
   * after the last completed, which is set before the first session starts
   * and never moves.  It is made with no lock and no scan of the proc array
   * (SHMEM.md section 7, step 3).  The command id is the caller's, as before.
   */
  xmax = XidFromFullTransactionId(TransamVariables->latestCompletedXid);
  TransactionIdAdvance(xmax);
  Assert(TransactionIdIsNormal(xmax));

  if (!TransactionIdIsValid(MyProc->xmin))
    MyProc->xmin = TransactionXmin = xmax;
  RecentXmin = xmax;

  snapshot->xmin = xmax;
  snapshot->xmax = xmax;
  snapshot->xcnt = 0;
  snapshot->subxcnt = 0;
  snapshot->suboverflowed = false;
  snapshot->snapXactCompletionCount = TransamVariables->xactCompletionCount;
  snapshot->curcid = GetCurrentCommandId(false);
  snapshot->active_count = 0;
  snapshot->regd_count = 0;
  snapshot->copied = false;
  return snapshot;
}

/*
 * ProcArrayInstallImportedXmin -- install imported xmin into MyProc->xmin
 *
 * This is called when installing a snapshot imported from another
 * transaction.  To ensure that OldestXmin doesn't go backwards, we must
 * check that the source transaction is still running, and we'd better do
 * that atomically with installing the new xmin.
 *
 * Returns true if successful, false if source xact is no longer running.
 */
bool ProcArrayInstallImportedXmin(TransactionId xmin,
                                  VirtualTransactionId *sourcevxid) {
  /* No snapshot is imported (ImportSnapshot() refuses): none installs. */
  return false;
}

/*
 * ProcArrayInstallRestoredXmin -- install restored xmin into MyProc->xmin
 *
 * This is like ProcArrayInstallImportedXmin, but we have a pointer to the
 * PGPROC of the transaction from which we imported the snapshot, rather than
 * an XID.
 *
 * Note that this function also copies statusFlags from the source `proc` in
 * order to avoid the case where MyProc's xmin needs to be skipped for
 * computing xid horizon.
 *
 * Returns true if successful, false if source xact is no longer running.
 */
bool ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc) {
  /*
   * Every snapshot is the same constant (GetSnapshotData()), so the one
   * restored is this session's too, and nothing of proc need be read.
   */
  Assert(TransactionIdIsNormal(xmin));
  MyProc->xmin = TransactionXmin = xmin;
  return true;
}

/*
 * ProcNumberGetProc -- get a backend's PGPROC given its proc number
 *
 * The result may be out of date arbitrarily quickly, so the caller
 * must be careful about how this information is used.  NULL is
 * returned if the backend is not active.
 */
PGPROC *ProcNumberGetProc(ProcNumber procNumber) {
  PGPROC *result;

  if (procNumber < 0 || procNumber >= ProcGlobal->allProcCount)
    return NULL;
  result = GetPGProcByNumber(procNumber);

  if (result->pid == 0)
    return NULL;

  return result;
}

/*
 * BackendPidGetProc -- get a backend's PGPROC given its PID
 *
 * Returns NULL if not found.  Note that it is up to the caller to be
 * sure that the question remains meaningful for long enough for the
 * answer to be used ...
 */
PGPROC *BackendPidGetProc(int pid) {
  session_ref_t record;

  if (pid == 0) /* never match dummy PGPROCs */
    return NULL;
  /* A PGPROC is never freed: the pointer stays good after the record. */
  record = session_by_pid(pid);
  return record != nullptr ? record->proc : NULL;
}

/*
 * Terminate existing connections to the specified database. This routine
 * is used by the DROP DATABASE command when user has asked to forcefully
 * drop the database.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * If the target database has a prepared transaction or permissions checks
 * fail for a connection, this fails without terminating anything.
 */
void TerminateOtherDBBackends(Oid databaseId) {
  std::vector<session_ref_t> others;

  /* The records of the database's other sessions (SHMEM.md section 5.2). */
  for (session_ref_t &record : sessions_all())
    if (record->database.load(std::memory_order_acquire) == databaseId &&
        record->proc != MyProc)
      others.push_back(std::move(record));

  /*
   * Permissions checks relax the pg_terminate_backend checks in two ways,
   * both by omitting the !OidIsValid(role) check:
   *
   * - Accept terminating autovacuum workers, since DROP DATABASE without
   * FORCE terminates them.
   *
   * Unlike pg_terminate_backend, we don't raise some warnings - like "PID
   * %d is not a PostgreSQL server process", because for us already finished
   * session is not a problem.
   */
  for (const session_ref_t &record : others) {
    Oid role = record->role.load(std::memory_order_acquire);

    if (superuser_arg(role) && !superuser())
      ereport(ERROR,
              (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
               errmsg("permission denied to terminate process"),
               errdetail("Only roles with the %s attribute may terminate "
                         "processes of roles with the %s attribute.",
                         "SUPERUSER", "SUPERUSER")));

    if (!has_privs_of_role(GetUserId(), role) &&
        !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
      ereport(ERROR,
              (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
               errmsg("permission denied to terminate process"),
               errdetail("Only roles with privileges of the role whose "
                         "process is being terminated or with privileges "
                         "of the \"%s\" role may terminate this process.",
                         "pg_signal_backend")));
  }

  /* Each in its own thread: one that has ended since is not another's. */
  for (const session_ref_t &record : others)
    session_interrupt(record, INTERRUPT_DIE);
}


