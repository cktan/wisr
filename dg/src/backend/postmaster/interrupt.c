/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  Interrupt handling routines.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"

thread_local /*LOCAL*/ bool ConfigReloadPending = false;

/*
 * Simple signal handler for triggering a configuration reload.
 *
 * Normally, this handler would be used for SIGHUP. The idea is that code
 * which uses it would arrange to check the ConfigReloadPending flag at
 * convenient places inside main loops, or else call ProcessMainLoopInterrupts.
 */
void SignalHandlerForConfigReload(void) {
  ConfigReloadPending = true;
  SetLatch(MyLatch);
}

/*
 * Simple signal handler for exiting quickly as if due to a crash.
 *
 * Normally, this would be used for handling SIGQUIT.
 */
void SignalHandlerForCrashExit(void) {
  /*
   * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
   * because shared memory may be corrupted, so we don't want to try to
   * clean up our transaction.  Just nail the windows shut and get out of
   * town.  The callbacks wouldn't be safe to run from a signal handler,
   * anyway.
   *
   * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
   * a system reset cycle if someone sends a manual SIGQUIT to a random
   * backend.  This is necessary precisely because we don't clean up our
   * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
   * should ensure the postmaster sees this as a crash, too, but no harm in
   * being doubly sure.)
   *
   * A session thread ends the thread, not the server, as quickdie() does.
   */
  if (IsSessionThread)
    proc_exit(2);
  _exit(2);
}
