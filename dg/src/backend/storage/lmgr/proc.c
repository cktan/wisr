/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  routines to manage per-process shared memory data structure
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/proc.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * ProcReleaseLocks -- frees the locks associated with current transaction
 *
 * ProcKill -- destroys the shared memory state (and locks)
 * associated with the process.
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/advisory_lock.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/spin.h"
#include "utils/control_block.h"

/* GUC variables */
thread_local /*LOCAL*/ int StatementTimeout = 0;
thread_local /*LOCAL*/ int LockTimeout = 0;
thread_local /*LOCAL*/ int IdleInTransactionSessionTimeout = 0;
thread_local /*LOCAL*/ int TransactionTimeout = 0;
thread_local /*LOCAL*/ int IdleSessionTimeout = 0;

/* Pointer to this process's PGPROC struct, if any */
thread_local /*LOCAL*/ PGPROC *MyProc = NULL;

/*
 * pcb->proc_struct_mutex protects the freelist of recycled PGPROC
 * structures.  It cannot be an LWLock, because the LWLock manager depends on
 * already having a PGPROC and a wait semaphore; and these structures are
 * touched only at a session's start and end, not for long.
 */

/* Pointers to shared-memory structures */
thread_local /*SHARED*/ PROC_HDR *ProcGlobal = NULL;

static void RemoveProcFromArray(int code, Datum arg);
static void ProcKill(int code, Datum arg);

/*
 * Report shared-memory space needed by PGPROC.
 */
static Size PGProcShmemSize(void) {
  Size size = 0;
  Size TotalProcs = MaxBackends;

  size = add_size(size, mul_size(TotalProcs, sizeof(PGPROC)));

  return size;
}

/*
 * Report number of semaphores needed by InitProcGlobal.
 */
int ProcGlobalSemas(void) {
  /* We need a sema per backend. */
  return MaxBackends;
}

/*
 * InitProcGlobal -
 *	  Initialize the global process table during postmaster or standalone
 *	  backend startup.
 *
 *	  We also create all the per-process semaphores we will need to support
 *	  the requested number of backends.  We used to allocate semaphores
 *	  only when backends were actually started up, but that is bad because
 *	  it lets Postgres fail under load --- a lot of Unix systems are
 *	  (mis)configured with small limits on the number of semaphores, and
 *	  running out when trying to start another backend is a common failure.
 *	  So, now we grab enough semaphores to support the desired max number
 *	  of backends immediately at initialization --- if the sysadmin has set
 *	  MaxConnections or max_wal_senders
 *	  higher than his kernel will support, he'll
 *	  find out sooner rather than later.
 *
 *	  Another reason for creating semaphores here is that the semaphore
 *	  implementation typically requires us to create semaphores in the
 *	  postmaster, not in backends.
 *
 * Note: this is NOT called by individual backends under a postmaster,
 * not even in the EXEC_BACKEND case.  The ProcGlobal pointer must be
 * propagated specially for EXEC_BACKEND operation.
 */
void InitProcGlobal(void) {
  PGPROC *procs;
  int i;
  bool found;
  uint32 TotalProcs = MaxBackends;
  Size requestSize;
  char *ptr;

  /* Create the ProcGlobal shared structure */
  ProcGlobal =
      (PROC_HDR *)ShmemInitStruct("Proc Header", sizeof(PROC_HDR), &found);
  Assert(!found);

  /*
   * Initialize the data structures.
   */
  ProcGlobal->spins_per_delay = DEFAULT_SPINS_PER_DELAY;
  dlist_init(&ProcGlobal->freeProcs);

  /*
   * Create and initialize all the PGPROC structures we'll need.  There are
   * six separate consumers: (1) normal backends, (2) special workers,
   * (3) background workers, (4) walsenders, (5) auxiliary processes, and
   * (6) prepared transactions.  Each PGPROC structure is dedicated to
   * exactly one of these purposes, and they do not move between groups.
   */
  requestSize = PGProcShmemSize();

  ptr = (char *)ShmemInitStruct("PGPROC structures", requestSize, &found);

  MemSet(ptr, 0, requestSize);

  procs = (PGPROC *)ptr;
  ptr = (char *)ptr + TotalProcs * sizeof(PGPROC);

  ProcGlobal->allProcs = procs;
  /* XXX allProcCount isn't really all of them; it excludes prepared xacts */
  ProcGlobal->allProcCount = MaxBackends;

  /* make sure wer didn't overflow */
  Assert((ptr > (char *)procs) && (ptr <= (char *)procs + requestSize));

  for (i = 0; i < TotalProcs; i++) {
    PGPROC *proc = &procs[i];

    /* Set up per-PGPROC semaphore and latch. */
    proc->sem = PGSemaphoreCreate();
    InitSharedLatch(&(proc->procLatch));

    /* Each is a session's: queue it up on the free list. */
    dlist_push_tail(&ProcGlobal->freeProcs, &proc->links);
    proc->procgloballist = &ProcGlobal->freeProcs;
  }
}

/*
 * InitProcess -- initialize a per-process PGPROC entry for this backend
 */
/*
 * Give this session's PGPROC its cancel key, which a cancel request for its
 * pid must match (SendCancelRequest()).
 */
void ProcSetCancelKey(const uint8 *key, int len) {
  Assert(len >= 0 && len <= MAX_CANCEL_KEY_LENGTH);
  std::lock_guard<std::mutex> lock(pcb->cancel_key_mutex);

  if (len > 0)
    memcpy(MyProc->cancelKey, key, len);
  MyProc->cancelKeyLength = len;
}

void InitProcess(void) {
  dlist_head *procgloballist;

  /*
   * ProcGlobal should be set up already (if we are a backend, we inherit
   * this by fork() or EXEC_BACKEND mechanism from the postmaster).
   */
  if (ProcGlobal == NULL)
    elog(PANIC, "proc header uninitialized");

  if (MyProc != NULL)
    elog(ERROR, "you already exist");

  /* The free list supplies our PGPROC (InitProcGlobal()). */
  procgloballist = &ProcGlobal->freeProcs;

  /*
   * Try to get a proc struct from the appropriate free list.  If this
   * fails, we must be out of PGPROC structures (not to mention semaphores).
   *
   * While we are holding the free list's lock, also copy the current shared
   * estimate of spins_per_delay to local storage.
   */
  {
    /* Just long enough to take a PGPROC off the list. */
    std::lock_guard<std::mutex> lock(pcb->proc_struct_mutex);

    set_spins_per_delay(ProcGlobal->spins_per_delay);
    if (!dlist_is_empty(procgloballist))
      MyProc =
          dlist_container(PGPROC, links, dlist_pop_head_node(procgloballist));
  }
  if (MyProc == NULL) {
    /*
     * If we reach here, all the PGPROCs are in use.  This is one of the
     * possible places to detect "too many backends", so give the standard
     * error message.
     */
    ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                    errmsg("sorry, too many clients already")));
  }
  MyProcNumber = GetNumberFromPGProc(MyProc);

  /*
   * Cross-check that the PGPROC is of the type we expect; if this were not
   * the case, it would get returned to the wrong list.
   */
  Assert(MyProc->procgloballist == procgloballist);

  /*
   * Initialize all fields of MyProc, except for those previously
   * initialized by InitProcGlobal.
   */
  dlist_node_init(&MyProc->links);
  MyProc->xid = InvalidTransactionId;
  MyProc->xmin = InvalidTransactionId;
  MyProc->pid = MyProcPid;
  MyProc->vxid.procNumber = MyProcNumber;
  MyProc->vxid.lxid = InvalidLocalTransactionId;
  /* databaseId and roleId will be filled in later */
  MyProc->databaseId = InvalidOid;
  MyProc->roleId = InvalidOid;
  MyProc->tempNamespaceId = InvalidOid;
  MyProc->isRegularBackend = AmRegularBackendProcess();
  MyProc->statusFlags = 0;
  MyProc->lwWaiting = LW_WS_NOT_WAITING;
  MyProc->lwWaitMode = 0;

  /*
   * Acquire ownership of the PGPROC's latch, so that we can use WaitLatch
   * on it.  That allows us to repoint the process latch, which so far
   * points to process local one, to the shared one.
   */
  OwnLatch(&MyProc->procLatch);
  SwitchToSharedLatch();

  /*
   * We might be reusing a semaphore that belonged to a failed process. So
   * be careful and reinitialize its value here.  (This is not strictly
   * necessary anymore, but seems like a good idea for cleanliness.)
   */
  PGSemaphoreReset(MyProc->sem);

  /*
   * Arrange to clean up at backend exit.
   */
  on_shmem_exit(ProcKill, 0);

  /*
   * Now that we have a PGPROC, we could try to acquire LWLocks, so
   * initialize the local state they need.
   */
  InitLWLockAccess();

  /*
   * Initialize backend-local pointers to all the shared data structures.
   * (We couldn't do this until now because it needs LWLocks.)  A forked
   * child inherited them; a session thread, like an EXEC_BACKEND child,
   * did not, or not all.
   */
  if (IsSessionThread)
    AttachSharedMemoryStructs();
}

/*
 * InitProcessPhase2 -- make MyProc visible in the shared ProcArray.
 *
 * This is separate from InitProcess because we can't acquire LWLocks until
 * we've created a PGPROC, but in the EXEC_BACKEND case ProcArrayAdd won't
 * work until after we've done AttachSharedMemoryStructs.
 */
void InitProcessPhase2(void) {
  Assert(MyProc != NULL);

  /*
   * Add our PGPROC to the PGPROC array in shared memory.
   */
  ProcArrayAdd(MyProc);

  /*
   * Arrange to clean that up at backend exit.
   */
  on_shmem_exit(RemoveProcFromArray, 0);
}

/*
 * Check whether there are at least N free PGPROC objects.  If false is
 * returned, *nfree will be set to the number of free PGPROC objects.
 * Otherwise, *nfree will be set to n.
 *
 * Note: this is designed on the assumption that N will generally be small.
 */
bool HaveNFreeProcs(int n, int *nfree) {
  dlist_iter iter;

  Assert(n > 0);
  Assert(nfree);

  /* The free list's lock, while it is counted. */
  std::lock_guard<std::mutex> lock(pcb->proc_struct_mutex);

  *nfree = 0;
  dlist_foreach(iter, &ProcGlobal->freeProcs) {
    (*nfree)++;
    if (*nfree == n)
      break;
  }

  return (*nfree == n);
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
  if (!MyProc)
    return;
  /* Release transaction-level advisory locks */
  advisory_xact_end();
}

/*
 * RemoveProcFromArray() -- Remove this process from the shared ProcArray.
 */
static void RemoveProcFromArray(int code, Datum arg) {
  Assert(MyProc != NULL);
  ProcArrayRemove(MyProc, InvalidTransactionId);
}

/*
 * ProcKill() -- Destroy the per-proc data structure for
 *		this process. Release any of its held LW locks.
 */
static void ProcKill(int code, Datum arg) {
  PGPROC *proc;
  dlist_head *procgloballist;

  Assert(MyProc != NULL);

  /* not safe if forked by system(), etc. */
  if (MyProc->pid != CurrentProcPid())
    elog(PANIC, "ProcKill() called in child process");

  /*
   * Release any LW locks I am holding.  There really shouldn't be any, but
   * it's cheap to check again before we cut the knees off the LWLock
   * facility by releasing our PGPROC ...
   */
  LWLockReleaseAll();

  /*
   * Reset MyLatch to the process local one and disown the shared latch, so
   * that signal handlers et al can continue using the latch after the
   * shared latch isn't ours anymore.
   *
   * DisownLatch() must happen before our PGPROC can appear on a freelist: a
   * newly-forked backend that pops our slot and calls OwnLatch() would
   * PANIC on a still-owned latch.
   */
  SwitchBackToLocalLatch();
  DisownLatch(&MyProc->procLatch);

  proc = MyProc;
  procgloballist = proc->procgloballist;

  MyProc = NULL;
  MyProcNumber = INVALID_PROC_NUMBER;

  /* Mark the proc no longer in use: no cancel request finds it now. */
  {
    std::lock_guard<std::mutex> lock(pcb->cancel_key_mutex);

    proc->cancelKeyLength = 0;
    proc->pid = 0;
  }
  proc->vxid.procNumber = INVALID_PROC_NUMBER;
  proc->vxid.lxid = InvalidTransactionId;

  /* The free list's lock, while our PGPROC goes back on it. */
  std::lock_guard<std::mutex> lock(pcb->proc_struct_mutex);
  /* Return PGPROC structure (and semaphore) to appropriate freelist */
  dlist_push_tail(procgloballist, &proc->links);

  /* Update shared estimate of spins_per_delay */
  ProcGlobal->spins_per_delay =
      update_spins_per_delay(ProcGlobal->spins_per_delay);
}

/*
 * The PGPROC of the backend whose pid is pid, or NULL if it has none yet.
 * For the postmaster, which takes no lock: the scan reads each pid without
 * one, so the answer may be stale, as a pid given to kill() may be.
 */
PGPROC *PostmasterPidGetProc(int pid) {
  for (int i = 0; i < MaxBackends; i++) {
    PGPROC *proc = GetPGProcByNumber(i);

    if (proc->pid == pid)
      return proc;
  }
  return NULL;
}

/*
 * ProcWaitForSignal - wait for a signal from another backend.
 *
 * As this uses the generic process latch the caller has to be robust against
 * unrelated wakeups: Always check that the desired state has occurred, and
 * wait again if not.
 */
void ProcWaitForSignal(uint32 wait_event_info) {
  (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
                  wait_event_info);
  ResetLatch(MyLatch);
  CHECK_FOR_INTERRUPTS();
}

/*
 * ProcSendSignal - set the latch of a backend identified by ProcNumber
 */
void ProcSendSignal(ProcNumber procNumber) {
  if (procNumber < 0 || procNumber >= ProcGlobal->allProcCount)
    elog(ERROR, "procNumber out of range");

  SetLatch(&ProcGlobal->allProcs[procNumber].procLatch);
}
