/*-------------------------------------------------------------------------
 *
 * pmsignal.c
 *	  routines for signaling between the postmaster and its child processes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/pmsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "utils/control_block.h"
#include "utils/memutils.h"

/*
 * Children no longer signal the postmaster with SIGUSR1 and a reason: the
 * postmaster ignores SIGUSR1.  What was PMSignalState is gone or elsewhere.
 * Its flags told the postmaster of a child that ended without cleaning up
 * after itself, which in one process cannot be: whatever skips a session's
 * exit callbacks ends the process.  Why the postmaster sent SIGQUIT is
 * pcb->quit_reason.
 */

/*
 * SetQuitSignalReason - broadcast the reason for a system shutdown.
 * Should be called by postmaster before sending SIGQUIT to children.
 *
 * Note: in a crash-and-restart scenario, the "reason" field gets cleared
 * as a part of rebuilding shared memory; the postmaster need not do it
 * explicitly.
 */
void SetQuitSignalReason(QuitSignalReason reason) {
  pcb->quit_reason.store(reason, std::memory_order_release);
}

/*
 * GetQuitSignalReason - obtain the reason for a system shutdown.
 * Called by child processes when they receive SIGQUIT.
 * If the postmaster hasn't actually sent SIGQUIT, will return PMQUIT_NOT_SENT.
 */
QuitSignalReason GetQuitSignalReason(void) {
  if (!IsUnderPostmaster)
    return PMQUIT_NOT_SENT;
  return (QuitSignalReason)pcb->quit_reason.load(std::memory_order_acquire);
}
