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

#include "access/nbtree.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
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
  /*
   * Set up xlog, clog, and buffers
   */
  VarsupShmemInit();

  /*
   * Set up process table
   */
  if (!IsUnderPostmaster)
    InitProcGlobal();
  ProcArrayShmemInit();

  /*
   * Set up interprocess signaling mechanisms
   */
  /* A rebuild after a crash clears why the postmaster sent SIGQUIT. */
  if (!IsUnderPostmaster)
    SetQuitSignalReason(PMQUIT_NOT_SENT);

  /*
   * Set up other modules that need some shared memory space
   */
  InjectionPointShmemInit();
}
