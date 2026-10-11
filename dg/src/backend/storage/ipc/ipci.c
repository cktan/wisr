/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipci.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "utils/guc.h"
#include "utils/injection_point.h"

thread_local /*SHARED*/ shmem_startup_hook_type shmem_startup_hook = NULL;

static void CreateOrAttachShmemStructs(void);

/*
 * AttachSharedMemoryStructs
 *		Initialize a postmaster child process's access to shared memory
 *      structures.
 *
 * In !EXEC_BACKEND mode, a child process inherits everything through the
 * fork, and this isn't needed.  A session thread is given its parent's
 * SHARED variables, but some pointers into shared memory sit in LOCAL
 * structures (pgStatLocal's, say): it needs this too.
 */
void AttachSharedMemoryStructs(void) {
  /* InitProcess must've been called already */
  Assert(MyProc != NULL);
  Assert(IsUnderPostmaster);

  /*
   * In EXEC_BACKEND mode, backends don't inherit the number of fast-path
   * groups we calculated before setting the shmem up, so recalculate it.
   */
  InitializeFastPathLocks();

  CreateOrAttachShmemStructs();

  /*
   * Now give loadable modules a chance to set up their shmem allocations
   */
  if (shmem_startup_hook)
    shmem_startup_hook();
}

/*
 * CreateSharedMemoryAndSemaphores
 *		Creates and initializes the structures the threads share, and
 *the semaphores.
 */
void CreateSharedMemoryAndSemaphores(void) {
  Assert(!IsUnderPostmaster);

  /* The index the structures are found by, before any is made. */
  InitShmemIndex();

  /*
   * Create semaphores.  (This is done here for historical reasons.  We used
   * to support emulating spinlocks with semaphores, which required
   * initializing semaphores early.)
   */
  PGReserveSemaphores(ProcGlobalSemas());

  /* Initialize subsystems */
  CreateOrAttachShmemStructs();

  /*
   * Now give loadable modules a chance to set up their shared structures
   */
  if (shmem_startup_hook)
    shmem_startup_hook();
}

/*
 * Initialize various subsystems, setting up the data structures the threads
 * share.
 *
 * This is called by the postmaster or by a standalone backend, which make
 * the structures, and by a session thread, which finds them: some pointers
 * to them sit in its LOCAL variables.  The called routines check
 * IsUnderPostmaster to tell the two apart.
 */
static void CreateOrAttachShmemStructs(void) {
  /* Now initialize LWLocks */
  CreateLWLocks();

  /*
   * Set up xlog, clog, and buffers
   */
  VarsupShmemInit();
  MultiXactShmemInit();
  BufferManagerShmemInit();

  /*
   * Set up lock manager
   */
  LockManagerShmemInit();

  /*
   * Set up predicate lock manager
   */
  PredicateLockShmemInit();

  /*
   * Set up process table
   */
  if (!IsUnderPostmaster)
    InitProcGlobal();
  ProcArrayShmemInit();
  TwoPhaseShmemInit();

  /*
   * Set up shared-inval messaging
   */
  SharedInvalShmemInit();

  /*
   * Set up interprocess signaling mechanisms
   */
  PMSignalShmemInit();
  ProcSignalShmemInit();

  /*
   * Set up other modules that need some shared memory space
   */
  BTreeShmemInit();
  InjectionPointShmemInit();
}

/*
 * InitializeShmemGUCs
 *
 * This function initializes runtime-computed GUCs: the number of semaphores
 * the current configuration needs.
 */
void InitializeShmemGUCs(void) {
  char buf[64];

  sprintf(buf, "%d", ProcGlobalSemas());
  SetConfigOption("num_os_semaphores", buf, PGC_INTERNAL,
                  PGC_S_DYNAMIC_DEFAULT);
}
