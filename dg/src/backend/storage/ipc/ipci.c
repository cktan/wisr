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
#include "storage/advisory_lock.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/session_registry.h"
#include "utils/guc.h"
#include "utils/injection_point.h"

/*
 * CreateSharedMemoryAndSemaphores
 *		Make the structures the threads share, before any thread starts.
 *
 * They are shmem's (utils/control_block.h), a struct of the process, not
 * shared memory: there is no registry of named blocks a thread would find
 * them in (dg/WORKNOTE.md step 173).  A session thread has nothing to
 * attach.
 */
void CreateSharedMemoryAndSemaphores(void) {
  Assert(!IsUnderPostmaster);

  /*
   * Set up the session registry
   */
  /* The sessions are in a registry of shmem's (SHMEM.md section 5.2). */
  session_registry_init();
  /* Advisory locks are in a table of shmem's, by the sessions' records. */
  advisory_init();

  /*
   * Set up other modules that need structures of their own
   */
  InjectionPointShmemInit();
}
