/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES process array code.
 *
 *
 * This module maintains arrays of PGPROC substructures, as well as associated
 * arrays in ProcGlobal, for all active backends.  Although there are several
 * uses for this, the principal one is as a means of determining the set of
 * currently running transactions.
 *
 * Because of various subtle race conditions it is critical that a backend
 * hold the correct locks while setting or clearing its xid (in
 * ProcGlobal->xids[]/MyProc->xid).  See notes in
 * src/backend/access/transam/README.
 *
 * The process arrays now also include structures representing prepared
 * transactions.  The xid and subxids fields of these are valid, as are the
 * myProcLocks lists.  They can be distinguished from regular backend PGPROCs
 * at need by checking for pid == 0.
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
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

/* Our shared memory area */
typedef struct ProcArrayStruct {
  int numProcs; /* number of valid procs entries */
  int maxProcs; /* allocated size of procs array */

  /* indexes into allProcs[], has PROCARRAY_MAXPROCS entries */
  int pgprocnos[FLEXIBLE_ARRAY_MEMBER];
} ProcArrayStruct;

static thread_local /*SHARED*/ ProcArrayStruct *procArray;

static thread_local /*SHARED*/ PGPROC *allProcs;

/*
 * Cache to reduce overhead of repeated calls to TransactionIdIsInProgress()
 */
static thread_local /*LOCAL*/ TransactionId cachedXidIsNotInProgress =
    InvalidTransactionId;

#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_known_xact = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_by_known_assigned = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_known_xact_inc()		(xc_by_known_xact++)
#define xc_by_my_xact_inc()			(xc_by_my_xact++)
#define xc_by_latest_xid_inc()		(xc_by_latest_xid++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_by_known_assigned_inc()	(xc_by_known_assigned++)
#define xc_no_overflow_inc()		(xc_no_overflow++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);
#else /* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_known_xact_inc()		((void) 0)
#define xc_by_my_xact_inc()			((void) 0)
#define xc_by_latest_xid_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
#define xc_by_known_assigned_inc()	((void) 0)
#define xc_no_overflow_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif /* XIDCACHE_DEBUG */

static void MaintainLatestCompletedXid(TransactionId latestXid);

static inline FullTransactionId FullXidRelativeTo(FullTransactionId rel,
                                                  TransactionId xid);

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
  bool found;

  /* Create or attach to the ProcArray shared structure */
  procArray = (ProcArrayStruct *)ShmemInitStruct(
      "Proc Array",
      add_size(offsetof(ProcArrayStruct, pgprocnos),
               mul_size(sizeof(int), PROCARRAY_MAXPROCS)),
      &found);

  if (!found) {
    /*
     * We're the first - initialize.
     */
    procArray->numProcs = 0;
    procArray->maxProcs = PROCARRAY_MAXPROCS;
    TransamVariables->xactCompletionCount = 1;
  }

  allProcs = ProcGlobal->allProcs;
}

/*
 * Add the specified PGPROC to the shared array.
 */
void ProcArrayAdd(PGPROC *proc) {
  int pgprocno = GetNumberFromPGProc(proc);
  ProcArrayStruct *arrayP = procArray;
  int index;
  int movecount;

  /* See ProcGlobal comment: ProcArrayLock guards the dense arrays */
  LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

  if (arrayP->numProcs >= arrayP->maxProcs) {
    /*
     * Oops, no room.  (This really shouldn't happen, since there is a
     * fixed supply of PGPROC structs too, and so we should have failed
     * earlier.)
     */
    ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                    errmsg("sorry, too many clients already")));
  }

  /*
   * Keep the procs array sorted by (PGPROC *) so that we can utilize
   * locality of references much better. This is useful while traversing the
   * ProcArray because there is an increased likelihood of finding the next
   * PGPROC structure in the cache.
   *
   * Since the occurrence of adding/removing a proc is much lower than the
   * access to the ProcArray itself, the overhead should be marginal
   */
  for (index = 0; index < arrayP->numProcs; index++) {
    int this_procno = arrayP->pgprocnos[index];

    Assert(this_procno >= 0 && this_procno < arrayP->maxProcs);
    Assert(allProcs[this_procno].pgxactoff == index);

    /* If we have found our right position in the array, break */
    if (this_procno > pgprocno)
      break;
  }

  movecount = arrayP->numProcs - index;
  memmove(&arrayP->pgprocnos[index + 1], &arrayP->pgprocnos[index],
          movecount * sizeof(*arrayP->pgprocnos));
  memmove(&ProcGlobal->xids[index + 1], &ProcGlobal->xids[index],
          movecount * sizeof(*ProcGlobal->xids));
  memmove(&ProcGlobal->subxidStates[index + 1],
          &ProcGlobal->subxidStates[index],
          movecount * sizeof(*ProcGlobal->subxidStates));
  memmove(&ProcGlobal->statusFlags[index + 1], &ProcGlobal->statusFlags[index],
          movecount * sizeof(*ProcGlobal->statusFlags));

  arrayP->pgprocnos[index] = GetNumberFromPGProc(proc);
  proc->pgxactoff = index;
  ProcGlobal->xids[index] = proc->xid;
  ProcGlobal->subxidStates[index] = proc->subxidStatus;
  ProcGlobal->statusFlags[index] = proc->statusFlags;

  arrayP->numProcs++;

  /* adjust pgxactoff for all following PGPROCs */
  index++;
  for (; index < arrayP->numProcs; index++) {
    int procno = arrayP->pgprocnos[index];

    Assert(procno >= 0 && procno < arrayP->maxProcs);
    Assert(allProcs[procno].pgxactoff == index - 1);

    allProcs[procno].pgxactoff = index;
  }

  LWLockRelease(ProcArrayLock);
}

/*
 * Remove the specified PGPROC from the shared array.
 *
 * When latestXid is a valid XID, we are removing a live 2PC gxact from the
 * array, and thus causing it to appear as "not running" anymore.  In this
 * case we must advance latestCompletedXid.  (This is essentially the same
 * as ProcArrayEndTransaction followed by removal of the PGPROC, but we take
 * the ProcArrayLock only once, and don't damage the content of the PGPROC;
 * twophase.c depends on the latter.)
 */
void ProcArrayRemove(PGPROC *proc, TransactionId latestXid) {
  ProcArrayStruct *arrayP = procArray;
  int myoff;
  int movecount;

#ifdef XIDCACHE_DEBUG
  /* dump stats at backend shutdown, but not prepared-xact end */
  if (proc->pid != 0)
    DisplayXidCache();
#endif

  /* See ProcGlobal comment: ProcArrayLock guards the dense arrays */
  LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

  myoff = proc->pgxactoff;

  Assert(myoff >= 0 && myoff < arrayP->numProcs);
  Assert(ProcGlobal->allProcs[arrayP->pgprocnos[myoff]].pgxactoff == myoff);

  if (TransactionIdIsValid(latestXid)) {
    Assert(TransactionIdIsValid(ProcGlobal->xids[myoff]));

    /* Advance global latestCompletedXid while holding the lock */
    MaintainLatestCompletedXid(latestXid);

    /* Same with xactCompletionCount  */
    TransamVariables->xactCompletionCount++;

    ProcGlobal->xids[myoff] = InvalidTransactionId;
    ProcGlobal->subxidStates[myoff].overflowed = false;
    ProcGlobal->subxidStates[myoff].count = 0;
  } else {
    /* Shouldn't be trying to remove a live transaction here */
    Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
  }

  Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
  Assert(ProcGlobal->subxidStates[myoff].count == 0);
  Assert(ProcGlobal->subxidStates[myoff].overflowed == false);

  ProcGlobal->statusFlags[myoff] = 0;

  /* Keep the PGPROC array sorted. See notes above */
  movecount = arrayP->numProcs - myoff - 1;
  memmove(&arrayP->pgprocnos[myoff], &arrayP->pgprocnos[myoff + 1],
          movecount * sizeof(*arrayP->pgprocnos));
  memmove(&ProcGlobal->xids[myoff], &ProcGlobal->xids[myoff + 1],
          movecount * sizeof(*ProcGlobal->xids));
  memmove(&ProcGlobal->subxidStates[myoff],
          &ProcGlobal->subxidStates[myoff + 1],
          movecount * sizeof(*ProcGlobal->subxidStates));
  memmove(&ProcGlobal->statusFlags[myoff], &ProcGlobal->statusFlags[myoff + 1],
          movecount * sizeof(*ProcGlobal->statusFlags));

  arrayP->pgprocnos[arrayP->numProcs - 1] = -1; /* for debugging */
  arrayP->numProcs--;

  /*
   * Adjust pgxactoff of following procs for removed PGPROC (note that
   * numProcs already has been decremented).
   */
  for (int index = myoff; index < arrayP->numProcs; index++) {
    int procno = arrayP->pgprocnos[index];

    Assert(procno >= 0 && procno < arrayP->maxProcs);
    Assert(allProcs[procno].pgxactoff - 1 == index);

    allProcs[procno].pgxactoff = index;
  }

  LWLockRelease(ProcArrayLock);
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
   * is cleared: its clearing, under ProcArrayLock or by the group, is gone.
   */
  Assert(!TransactionIdIsValid(latestXid));
  /*
   * If we have no XID, we don't need to lock, since we won't affect
   * anyone else's calculation of a snapshot.  We might change their
   * estimate of global xmin, but that's OK.
   */
  Assert(!TransactionIdIsValid(proc->xid));
  Assert(proc->subxidStatus.count == 0);
  Assert(!proc->subxidStatus.overflowed);

  proc->vxid.lxid = InvalidLocalTransactionId;
  proc->xmin = InvalidTransactionId;

  /* must be cleared with xid/xmin: */
  /* avoid unnecessarily dirtying shared cachelines */
  if (proc->statusFlags & PROC_VACUUM_STATE_MASK) {
    Assert(!LWLockHeldByMe(ProcArrayLock));
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    Assert(proc->statusFlags == ProcGlobal->statusFlags[proc->pgxactoff]);
    proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
    ProcGlobal->statusFlags[proc->pgxactoff] = proc->statusFlags;
    LWLockRelease(ProcArrayLock);
  }
}

/*
 * Update TransamVariables->latestCompletedXid to point to latestXid if
 * currently older.
 */
static void MaintainLatestCompletedXid(TransactionId latestXid) {
  FullTransactionId cur_latest = TransamVariables->latestCompletedXid;

  Assert(FullTransactionIdIsValid(cur_latest));
  Assert(LWLockHeldByMe(ProcArrayLock));

  if (TransactionIdPrecedes(XidFromFullTransactionId(cur_latest), latestXid)) {
    TransamVariables->latestCompletedXid =
        FullXidRelativeTo(cur_latest, latestXid);
  }

  Assert(IsBootstrapProcessingMode() ||
         FullTransactionIdIsNormal(TransamVariables->latestCompletedXid));
}

/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * Aside from some shortcuts such as checking RecentXmin and our own Xid,
 * there are four possibilities for finding a running transaction:
 *
 * 1. The given Xid is a main transaction Id.  We will find this out cheaply
 * by looking at ProcGlobal->xids.
 *
 * 2. The given Xid is one of the cached subxact Xids in the PGPROC array.
 * We can find this out cheaply too.
 *
 * 3. In Hot Standby mode, we must search the KnownAssignedXids list to see
 * if the Xid is running on the primary.
 *
 * 4. Search the SubTrans tree to find the Xid's topmost parent, and then see
 * if that is running according to ProcGlobal->xids[] or KnownAssignedXids.
 * This is the slowest way, but sadly it has to be done always if the others
 * failed, unless we see that the cached subxact sets are complete (none have
 * overflowed).
 *
 * ProcArrayLock has to be held while we do 1, 2, 3.  If we save the top Xids
 * while doing 1 and 3, we can release the ProcArrayLock while we do 4.
 * This buys back some concurrency (and we can't retrieve the main Xids from
 * ProcGlobal->xids[] again anyway; see GetNewTransactionId).
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
int GetMaxSnapshotXidCount(void) { return procArray->maxProcs; }

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
  bool result = false;
  ProcArrayStruct *arrayP = procArray;
  int index;

  Assert(TransactionIdIsNormal(xmin));
  if (!sourcevxid)
    return false;

  /* Get lock so source xact can't end while we're doing this */
  LWLockAcquire(ProcArrayLock, LW_SHARED);

  /*
   * Find the PGPROC entry of the source transaction. (This could use
   * GetPGProcByNumber(), unless it's a prepared xact.  But this isn't
   * performance critical.)
   */
  for (index = 0; index < arrayP->numProcs; index++) {
    int pgprocno = arrayP->pgprocnos[index];
    PGPROC *proc = &allProcs[pgprocno];
    int statusFlags = ProcGlobal->statusFlags[index];
    TransactionId xid;

    /* Ignore procs running LAZY VACUUM */
    if (statusFlags & PROC_IN_VACUUM)
      continue;

    /* We are only interested in the specific virtual transaction. */
    if (proc->vxid.procNumber != sourcevxid->procNumber)
      continue;
    if (proc->vxid.lxid != sourcevxid->localTransactionId)
      continue;

    /*
     * We check the transaction's database ID for paranoia's sake: if it's
     * in another DB then its xmin does not cover us.  Caller should have
     * detected this already, so we just treat any funny cases as
     * "transaction not found".
     */
    if (proc->databaseId != MyDatabaseId)
      continue;

    /*
     * Likewise, let's just make real sure its xmin does cover us.
     */
    xid = UINT32_ACCESS_ONCE(proc->xmin);
    if (!TransactionIdIsNormal(xid) ||
        !TransactionIdPrecedesOrEquals(xid, xmin))
      continue;

    /*
     * We're good.  Install the new xmin.  As in GetSnapshotData, set
     * TransactionXmin too.  (Note that because snapmgr.c called
     * GetSnapshotData first, we'll be overwriting a valid xmin here, so
     * we don't check that.)
     */
    MyProc->xmin = TransactionXmin = xmin;

    result = true;
    break;
  }

  LWLockRelease(ProcArrayLock);

  return result;
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
  bool result = false;
  TransactionId xid;

  Assert(TransactionIdIsNormal(xmin));
  Assert(proc != NULL);

  /*
   * Get an exclusive lock so that we can copy statusFlags from source proc.
   */
  LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

  /*
   * Be certain that the referenced PGPROC has an advertised xmin which is
   * no later than the one we're installing, so that the system-wide xmin
   * can't go backwards.  Also, make sure it's running in the same database,
   * so that the per-database xmin cannot go backwards.
   */
  xid = UINT32_ACCESS_ONCE(proc->xmin);
  if (proc->databaseId == MyDatabaseId && TransactionIdIsNormal(xid) &&
      TransactionIdPrecedesOrEquals(xid, xmin)) {
    /*
     * Install xmin and propagate the statusFlags that affect how the
     * value is interpreted by vacuum.
     */
    MyProc->xmin = TransactionXmin = xmin;
    MyProc->statusFlags = (MyProc->statusFlags & ~PROC_XMIN_FLAGS) |
                          (proc->statusFlags & PROC_XMIN_FLAGS);
    ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

    result = true;
  }

  LWLockRelease(ProcArrayLock);

  return result;
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
  PGPROC *result;

  if (pid == 0) /* never match dummy PGPROCs */
    return NULL;

  LWLockAcquire(ProcArrayLock, LW_SHARED);

  result = BackendPidGetProcWithLock(pid);

  LWLockRelease(ProcArrayLock);

  return result;
}

/*
 * BackendPidGetProcWithLock -- get a backend's PGPROC given its PID
 *
 * Same as above, except caller must be holding ProcArrayLock.  The found
 * entry, if any, can be assumed to be valid as long as the lock remains held.
 */
PGPROC *BackendPidGetProcWithLock(int pid) {
  PGPROC *result = NULL;
  ProcArrayStruct *arrayP = procArray;
  int index;

  if (pid == 0) /* never match dummy PGPROCs */
    return NULL;

  for (index = 0; index < arrayP->numProcs; index++) {
    PGPROC *proc = &allProcs[arrayP->pgprocnos[index]];

    if (proc->pid == pid) {
      result = proc;
      break;
    }
  }

  return result;
}

/*
 * GetCurrentVirtualXIDs -- returns an array of currently active VXIDs.
 *
 * The array is palloc'd. The number of valid entries is returned into *nvxids.
 *
 * The arguments allow filtering the set of VXIDs returned.  Our own process
 * is always skipped.  In addition:
 *	If limitXmin is not InvalidTransactionId, skip processes with
 *		xmin > limitXmin.
 *	If excludeXmin0 is true, skip processes with xmin = 0.
 *	If allDbs is false, skip processes attached to other databases.
 *	If excludeVacuum isn't zero, skip processes for which
 *		(statusFlags & excludeVacuum) is not zero.
 *
 * Note: the purpose of the limitXmin and excludeXmin0 parameters is to
 * allow skipping backends whose oldest live snapshot is no older than
 * some snapshot we have.  Since we examine the procarray with only shared
 * lock, there are race conditions: a backend could set its xmin just after
 * we look.  Indeed, on multiprocessors with weak memory ordering, the
 * other backend could have set its xmin *before* we look.  We know however
 * that such a backend must have held shared ProcArrayLock overlapping our
 * own hold of ProcArrayLock, else we would see its xmin update.  Therefore,
 * any snapshot the other backend is taking concurrently with our scan cannot
 * consider any transactions as still running that we think are committed
 * (since backends must hold ProcArrayLock exclusive to commit).
 */
VirtualTransactionId *GetCurrentVirtualXIDs(TransactionId limitXmin,
                                            bool excludeXmin0, bool allDbs,
                                            int excludeVacuum, int *nvxids) {
  VirtualTransactionId *vxids;
  ProcArrayStruct *arrayP = procArray;
  int count = 0;
  int index;

  /* allocate what's certainly enough result space */
  vxids = (VirtualTransactionId *)palloc(sizeof(VirtualTransactionId) *
                                         arrayP->maxProcs);

  LWLockAcquire(ProcArrayLock, LW_SHARED);

  for (index = 0; index < arrayP->numProcs; index++) {
    int pgprocno = arrayP->pgprocnos[index];
    PGPROC *proc = &allProcs[pgprocno];
    uint8 statusFlags = ProcGlobal->statusFlags[index];

    if (proc == MyProc)
      continue;

    if (excludeVacuum & statusFlags)
      continue;

    if (allDbs || proc->databaseId == MyDatabaseId) {
      /* Fetch xmin just once - might change on us */
      TransactionId pxmin = UINT32_ACCESS_ONCE(proc->xmin);

      if (excludeXmin0 && !TransactionIdIsValid(pxmin))
        continue;

      /*
       * InvalidTransactionId precedes all other XIDs, so a proc that
       * hasn't set xmin yet will not be rejected by this test.
       */
      if (!TransactionIdIsValid(limitXmin) ||
          TransactionIdPrecedesOrEquals(pxmin, limitXmin)) {
        VirtualTransactionId vxid;

        GET_VXID_FROM_PGPROC(vxid, *proc);
        if (VirtualTransactionIdIsValid(vxid))
          vxids[count++] = vxid;
      }
    }
  }

  LWLockRelease(ProcArrayLock);

  *nvxids = count;
  return vxids;
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
  ProcArrayStruct *arrayP = procArray;
  List *pids = NIL;
  int nprepared = 0;
  int i;

  LWLockAcquire(ProcArrayLock, LW_SHARED);

  for (i = 0; i < procArray->numProcs; i++) {
    int pgprocno = arrayP->pgprocnos[i];
    PGPROC *proc = &allProcs[pgprocno];

    if (proc->databaseId != databaseId)
      continue;
    if (proc == MyProc)
      continue;

    if (proc->pid != 0)
      pids = lappend_int(pids, proc->pid);
    else
      nprepared++;
  }

  LWLockRelease(ProcArrayLock);

  if (nprepared > 0)
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
             errmsg("database \"%s\" is being used by prepared transactions",
                    get_database_name(databaseId)),
             errdetail_plural(
                 "There is %d prepared transaction using the database.",
                 "There are %d prepared transactions using the database.",
                 nprepared, nprepared)));

  if (pids) {
    ListCell *lc;

    /*
     * Permissions checks relax the pg_terminate_backend checks in two
     * ways, both by omitting the !OidIsValid(proc->roleId) check:
     *
     * - Accept terminating autovacuum workers, since DROP DATABASE
     * without FORCE terminates them.
     *
     * Unlike pg_terminate_backend, we don't raise some warnings - like
     * "PID %d is not a PostgreSQL server process", because for us already
     * finished session is not a problem.
     */
    foreach (lc, pids) {
      int pid = lfirst_int(lc);
      PGPROC *proc = BackendPidGetProc(pid);

      if (proc != NULL) {
        if (superuser_arg(proc->roleId) && !superuser())
          ereport(ERROR,
                  (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                   errmsg("permission denied to terminate process"),
                   errdetail("Only roles with the %s attribute may terminate "
                             "processes of roles with the %s attribute.",
                             "SUPERUSER", "SUPERUSER")));

        if (!has_privs_of_role(GetUserId(), proc->roleId) &&
            !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
          ereport(ERROR,
                  (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                   errmsg("permission denied to terminate process"),
                   errdetail("Only roles with privileges of the role whose "
                             "process is being terminated or with privileges "
                             "of the \"%s\" role may terminate this process.",
                             "pg_signal_backend")));
      }
    }

    /*
     * There's a race condition here: once we release the ProcArrayLock,
     * it's possible for the session to exit before we issue kill.  That
     * race condition possibility seems too unlikely to worry about.  See
     * pg_signal_backend.
     */
    foreach (lc, pids) {
      int pid = lfirst_int(lc);
      PGPROC *proc = BackendPidGetProc(pid);

      if (proc != NULL)
        InterruptProc(proc, INTERRUPT_DIE);
    }
  }
}

#ifdef XIDCACHE_DEBUG

/*
 * Print stats about effectiveness of XID cache
 */
static void DisplayXidCache(void) {
  fprintf(stderr,
          "XidCache: xmin: %ld, known: %ld, myxact: %ld, latest: %ld, mainxid: "
          "%ld, childxid: %ld, knownassigned: %ld, nooflo: %ld, slow: %ld\n",
          xc_by_recent_xmin, xc_by_known_xact, xc_by_my_xact, xc_by_latest_xid,
          xc_by_main_xid, xc_by_child_xid, xc_by_known_assigned, xc_no_overflow,
          xc_slow_answer);
}
#endif /* XIDCACHE_DEBUG */

/*
 * Convert a 32 bit transaction id into 64 bit transaction id, by assuming it
 * is within MaxTransactionId / 2 of XidFromFullTransactionId(rel).
 *
 * Be very careful about when to use this function. It can only safely be used
 * when there is a guarantee that xid is within MaxTransactionId / 2 xids of
 * rel. That e.g. can be guaranteed if the caller assures a snapshot is
 * held by the backend and xid is from a table (where vacuum/freezing ensures
 * the xid has to be within that range), or if xid is from the procarray and
 * prevents xid wraparound that way.
 */
static inline FullTransactionId FullXidRelativeTo(FullTransactionId rel,
                                                  TransactionId xid) {
  TransactionId rel_xid = XidFromFullTransactionId(rel);

  Assert(TransactionIdIsValid(xid));
  Assert(TransactionIdIsValid(rel_xid));

  return FullTransactionIdFromU64(U64FromFullTransactionId(rel) +
                                  (int32)(xid - rel_xid));
}
