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

/*
 * CreateSharedMemoryAndSemaphores
 *		Make the structures the threads share, before any thread starts.
 *
 * They are the pcb's (utils/control_block.h): there is no shared memory,
 * and no registry of named blocks a thread would find them in
 * (dg/WORKNOTE.md step 173).  A session thread has nothing to attach.
 */
void CreateSharedMemoryAndSemaphores(void) {
  Assert(!IsUnderPostmaster);

  /*
   * Set up the session registry
   */
  ProcArrayShmemInit();

  /*
   * Set up interprocess signaling mechanisms
   */
  /* A rebuild after a crash clears why the postmaster sent SIGQUIT. */
  SetQuitSignalReason(PMQUIT_NOT_SENT);

  /*
   * Set up other modules that need structures of their own
   */
  InjectionPointShmemInit();
}
