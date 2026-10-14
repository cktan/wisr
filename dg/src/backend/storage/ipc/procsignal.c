/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include <mutex>

#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinval.h"
#include "utils/control_block.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/*
 * The SIGUSR1 signal is multiplexed to support signaling multiple event
 * types. The specific reason is communicated via flags in shared memory.
 * We keep a boolean flag for each possible "reason", so that different
 * reasons can be signaled to a process concurrently.  (However, if the same
 * reason is signaled more than once nearly simultaneously, the process may
 * observe it only once.)
 *
 * Each process that wants to receive signals registers its process ID
 * in the pcb->procsignal array. The array is indexed by ProcNumber to make
 * slot allocation simple, and to avoid having to search the array when you
 * know the ProcNumber of the process you're signaling.  (We do support
 * signaling without ProcNumber, but it's a bit less efficient.)
 *
 * The fields in each slot are protected by its std::mutex, pss_mutex.
 * pss_pid can also be read without holding it, as a quick preliminary check
 * when searching for a particular PID in the array.
 *
 * pss_signalFlags are set where we don't need to keep track of whether or
 * not the target process has handled the signal.
 */
typedef struct ProcSignalSlot {
  pg_atomic_uint32 pss_pid;
  int pss_cancel_key_len; /* 0 means no cancellation is possible */
  uint8 pss_cancel_key[MAX_CANCEL_KEY_LENGTH];
  volatile sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];
  std::mutex pss_mutex; /* protects the above fields */
} ProcSignalSlot;

/* We reserve a slot for each possible ProcNumber. */
#define NumProcSignalSlots	(MaxBackends)

static thread_local /*LOCAL*/ ProcSignalSlot *MyProcSignalSlot = NULL;

static bool CheckProcSignal(ProcSignalReason reason);
static void CleanupProcSignalState(int status, Datum arg);

/*
 * ProcSignalShmemInit
 *		Make and initialize the ProcSignal slots, once
 *
 * The postmaster makes them before any session thread starts (CODE.md
 * section 3); a session thread's call finds them made.
 */
void ProcSignalShmemInit(void) {
  if (pcb->procsignal != NULL)
    return;
  /* Kept for the process's whole life, as the pcb is: never freed. */
  pcb->procsignal = new ProcSignalSlot[NumProcSignalSlots]();
  for (int i = 0; i < NumProcSignalSlots; ++i)
    pg_atomic_init_u32(&pcb->procsignal[i].pss_pid, 0);
}

/*
 * ProcSignalInit
 *		Register the current process in the ProcSignal array
 */
void ProcSignalInit(const uint8 *cancel_key, int cancel_key_len) {
  ProcSignalSlot *slot;
  uint32 old_pss_pid;

  Assert(cancel_key_len >= 0 && cancel_key_len <= MAX_CANCEL_KEY_LENGTH);
  if (MyProcNumber < 0)
    elog(ERROR, "MyProcNumber not set");
  if (MyProcNumber >= NumProcSignalSlots)
    elog(ERROR, "unexpected MyProcNumber %d in ProcSignalInit (max %d)",
         MyProcNumber, NumProcSignalSlots);
  slot = &pcb->procsignal[MyProcNumber];

  std::unique_lock<std::mutex> lock(slot->pss_mutex);

  /* Value used for sanity check below */
  old_pss_pid = pg_atomic_read_u32(&slot->pss_pid);

  /* Clear out any leftover signal reasons */
  MemSet(slot->pss_signalFlags, 0, NUM_PROCSIGNALS * sizeof(sig_atomic_t));

  /* Publish the PID, which marks the slot taken. */
  pg_atomic_write_membarrier_u32(&slot->pss_pid, MyProcPid);

  if (cancel_key_len > 0)
    memcpy(slot->pss_cancel_key, cancel_key, cancel_key_len);
  slot->pss_cancel_key_len = cancel_key_len;

  lock.unlock();

  /* The lock is released, do the check */
  if (old_pss_pid != 0)
    elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
         MyProcPid, MyProcNumber);

  /* Remember slot location for CheckProcSignal */
  MyProcSignalSlot = slot;

  /* Set up to release the slot on process exit */
  on_shmem_exit(CleanupProcSignalState, (Datum)0);
}

/*
 * CleanupProcSignalState
 *		Remove current process from ProcSignal mechanism
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 */
static void CleanupProcSignalState(int status, Datum arg) {
  pid_t old_pid;
  ProcSignalSlot *slot = MyProcSignalSlot;

  /*
   * Clear MyProcSignalSlot, so that a SIGUSR1 received after this point
   * won't try to access it after it's no longer ours (and perhaps even
   * after we've unmapped the shared memory segment).
   */
  Assert(MyProcSignalSlot != NULL);
  MyProcSignalSlot = NULL;

  /* sanity check */
  std::unique_lock<std::mutex> lock(slot->pss_mutex);
  old_pid = pg_atomic_read_u32(&slot->pss_pid);
  if (old_pid != MyProcPid) {
    /*
     * don't ERROR here. We're exiting anyway, and don't want to get into
     * infinite loop trying to exit
     */
    lock.unlock();
    elog(LOG, "process %d releasing ProcSignal slot %d, but it contains %d",
         MyProcPid, (int)(slot - pcb->procsignal), (int)old_pid);
    return; /* XXX better to zero the slot anyway? */
  }

  /* Mark the slot as unused */
  pg_atomic_write_u32(&slot->pss_pid, 0);
  slot->pss_cancel_key_len = 0;

  lock.unlock();
}

/*
 * SendProcSignal
 *		Send a signal to a Postgres process
 *
 * Providing procNumber is optional, but it will speed up the operation.
 *
 * On success (a signal was sent), zero is returned.
 * On error, -1 is returned, and errno is set (typically to ESRCH or EPERM).
 *
 * Not to be confused with ProcSendSignal
 */
int SendProcSignal(pid_t pid, ProcSignalReason reason, ProcNumber procNumber) {
  ProcSignalSlot *slot;

  if (procNumber != INVALID_PROC_NUMBER) {
    Assert(procNumber < NumProcSignalSlots);
    slot = &pcb->procsignal[procNumber];

    std::unique_lock<std::mutex> lock(slot->pss_mutex);
    if (pg_atomic_read_u32(&slot->pss_pid) == pid) {
      /* Atomically set the proper flag */
      slot->pss_signalFlags[reason] = true;
      lock.unlock();
      /* Send signal */
      InterruptProc(GetPGProcByNumber(procNumber), INTERRUPT_PROCSIG);
      return 0;
    }
    lock.unlock();
  } else {
    /*
     * procNumber not provided, so search the array using pid.  We search
     * the array back to front so as to reduce search overhead.  Passing
     * INVALID_PROC_NUMBER means that the target is most likely an
     * auxiliary process, which will have a slot near the end of the
     * array.
     */
    int i;

    for (i = NumProcSignalSlots - 1; i >= 0; i--) {
      slot = &pcb->procsignal[i];

      if (pg_atomic_read_u32(&slot->pss_pid) == pid) {
        std::unique_lock<std::mutex> lock(slot->pss_mutex);
        if (pg_atomic_read_u32(&slot->pss_pid) == pid) {
          /* Atomically set the proper flag */
          slot->pss_signalFlags[reason] = true;
          lock.unlock();
          /* Send signal */
          InterruptProc(GetPGProcByNumber(i), INTERRUPT_PROCSIG);
          return 0;
        }
        lock.unlock();
      }
    }
  }

  errno = ESRCH;
  return -1;
}

/*
 * CheckProcSignal - check to see if a particular reason has been
 * signaled, and clear the signal flag.  Should be called after receiving
 * SIGUSR1.
 */
static bool CheckProcSignal(ProcSignalReason reason) {
  volatile ProcSignalSlot *slot = MyProcSignalSlot;

  if (slot != NULL) {
    /*
     * Careful here --- don't clear flag if we haven't seen it set.
     * pss_signalFlags is of type "volatile sig_atomic_t" to allow us to
     * read it here safely, without holding the spinlock.
     */
    if (slot->pss_signalFlags[reason]) {
      slot->pss_signalFlags[reason] = false;
      return true;
    }
  }

  return false;
}

/*
 * procsignal_sigusr1_handler - handle INTERRUPT_PROCSIG, once SIGUSR1.
 */
void procsignal_sigusr1_handler(void) {
  if (CheckProcSignal(PROCSIG_CATCHUP_INTERRUPT))
    HandleCatchupInterrupt();

  if (CheckProcSignal(PROCSIG_LOG_MEMORY_CONTEXT))
    HandleLogMemoryContextInterrupt();

  SetLatch(MyLatch);
}

/*
 * Send a query cancellation signal to backend.
 *
 * Note: This is called from a backend process before authentication.  We
 * cannot take LWLocks yet, but that's OK; we rely on atomic reads of the
 * fields in the ProcSignal slots.
 */
void SendCancelRequest(int backendPID, const uint8 *cancel_key,
                       int cancel_key_len) {
  if (backendPID == 0) {
    ereport(LOG, (errmsg("invalid cancel request with PID 0")));
    return;
  }

  /*
   * See if we have a matching backend. Reading the pss_pid and
   * pss_cancel_key fields is racy, a backend might die and remove itself
   * from the array at any time.  The probability of the cancellation key
   * matching wrong process is miniscule, however, so we can live with that.
   * PIDs are reused too, so sending the signal based on PID is inherently
   * racy anyway, although OS's avoid reusing PIDs too soon.
   */
  for (int i = 0; i < NumProcSignalSlots; i++) {
    ProcSignalSlot *slot = &pcb->procsignal[i];
    bool match;

    if (pg_atomic_read_u32(&slot->pss_pid) != backendPID)
      continue;

    /* Acquire the spinlock and re-check */
    std::unique_lock<std::mutex> lock(slot->pss_mutex);
    if (pg_atomic_read_u32(&slot->pss_pid) != backendPID) {
      lock.unlock();
      continue;
    } else {
      match = slot->pss_cancel_key_len == cancel_key_len &&
              timingsafe_bcmp(slot->pss_cancel_key, cancel_key,
                              cancel_key_len) == 0;

      lock.unlock();

      if (match) {
        /* Found a match; signal that backend to cancel current op */
        ereport(DEBUG2,
                (errmsg_internal(
                    "processing cancel request: sending SIGINT to process %d",
                    backendPID)));

        InterruptProc(GetPGProcByNumber(i), INTERRUPT_CANCEL);
      } else {
        /* Right PID, wrong key: no way, Jose */
        ereport(LOG, (errmsg("wrong key in cancel request for process %d",
                             backendPID)));
      }
      return;
    }
  }

  /* No matching backend */
  ereport(LOG, (errmsg("PID %d in cancel request did not match any process",
                       backendPID)));
}

/* This thread's handler of each interrupt bit, NULL for none. */
static thread_local /*LOCAL*/ interrupt_handler_t interrupt_handlers[32];

/* Make handler the one this thread runs for bit. */
void SetInterruptHandler(uint32 bit, interrupt_handler_t handler) {
  Assert(bit != 0 && (bit & (bit - 1)) == 0);
  interrupt_handlers[pg_rightmost_one_pos32(bit)] = handler;
}

/*
 * Interrupt the thread that owns proc, which runs the handlers of bits at
 * its next CHECK_FOR_INTERRUPTS() or wait.  sm_sys.h wakes it: a wait it is in
 * returns, and runs them.  A proc that no thread owns is left alone.
 */
void InterruptProc(PGPROC *proc, uint32 bits) {
  sm_thread_ref_t owner = LatchOwner(&proc->procLatch);

  if (owner)
    sm_interrupt(owner, bits);
}

/* Run this thread's handlers of bits now. */
void InterruptSelf(uint32 bits) {
  while (bits != 0) {
    interrupt_handler_t handler =
        interrupt_handlers[pg_rightmost_one_pos32(bits)];

    bits &= bits - 1;
    if (handler != NULL)
      handler();
  }
}

/*
 * Leave bits pending for this thread, to run at its next wait or
 * CHECK_FOR_INTERRUPTS(): for a handler whose work may not be done where
 * it runs.
 */
void InterruptSelfLater(uint32 bits) { sm_interrupt(sm_thread_self(), bits); }

/* Run the handler of each bit pending for this thread, taking them. */
void DeliverInterrupts(void) {
  if (MyInterrupts->load(std::memory_order_relaxed) == 0)
    return;
  InterruptSelf(sm_take(INTERRUPT_ALL));
}

/*
 * The interrupt bits of what a signal asked of a process, for the
 * postmaster, which still speaks to its children in signals, and the SQL
 * functions that send them.  0 for a signal that has no bit.
 */
uint32 InterruptsOfSignal(int sig) {
  switch (sig) {
  case SIGINT:
    return INTERRUPT_CANCEL;
  case SIGTERM:
    return INTERRUPT_DIE;
  case SIGQUIT:
    return INTERRUPT_QUIT;
  case SIGHUP:
    return INTERRUPT_RELOAD;
  case SIGUSR1:
    return INTERRUPT_PROCSIG;
  default:
    return 0;
  }
}
