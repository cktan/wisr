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
#include "storage/session_registry.h"
#include "storage/sinval.h"
#include "utils/control_block.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/*
 * Send a query cancellation signal to backend.
 *
 * Note: This is called from a backend process before authentication.  The
 * session is found by its record, whose cancel key is read under
 * pcb->cancel_key_mutex (session_set_cancel_key()).  PIDs are reused, so a
 * signal sent by PID is racy anyway, although OS's avoid reusing PIDs too
 * soon; the record is the session's alone, so an ended one is interrupted
 * harmlessly.
 */
void SendCancelRequest(int backendPID, const uint8 *cancel_key,
                       int cancel_key_len) {
  session_ref_t record;
  bool match;

  if (backendPID == 0) {
    ereport(LOG, (errmsg("invalid cancel request with PID 0")));
    return;
  }

  record = session_by_pid(backendPID);
  if (record == nullptr) {
    ereport(LOG, (errmsg("PID %d in cancel request did not match any process",
                         backendPID)));
    return;
  }
  {
    /* The session sets its key under this mutex. */
    std::lock_guard<std::mutex> lock(pcb->cancel_key_mutex);

    match =
        record->cancel_key_length != 0 &&
        record->cancel_key_length == cancel_key_len &&
        timingsafe_bcmp(record->cancel_key, cancel_key, cancel_key_len) == 0;
  }
  if (match) {
    /* Found a match; signal that backend to cancel current op */
    ereport(DEBUG2,
            (errmsg_internal(
                "processing cancel request: sending SIGINT to process %d",
                backendPID)));

    session_interrupt(record, INTERRUPT_CANCEL);
  } else {
    /* Right PID, wrong key: no way, Jose */
    ereport(LOG,
            (errmsg("wrong key in cancel request for process %d", backendPID)));
  }
}

/* This thread's handler of each interrupt bit, NULL for none. */
static thread_local /*LOCAL*/ interrupt_handler_t interrupt_handlers[32];

/* Make handler the one this thread runs for bit. */
void SetInterruptHandler(uint32 bit, interrupt_handler_t handler) {
  Assert(bit != 0 && (bit & (bit - 1)) == 0);
  interrupt_handlers[pg_rightmost_one_pos32(bit)] = handler;
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
  default:
    return 0;
  }
}
