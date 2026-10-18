/*-------------------------------------------------------------------------
 *
 * latch.c
 *	  Routines for inter-process latches
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable.  See latch.h for more information
 * on how to use them.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/latch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "storage/waiteventset.h"
#include "utils/resowner.h"

/* A common WaitEventSet used to implement WaitLatch() */
static thread_local /*LOCAL*/ WaitEventSet *LatchWaitSet;

/* Free the WaitEventSet WaitLatch() uses: a session thread, as it ends. */
void ShutdownLatchWaitSet(void) {
  if (LatchWaitSet != NULL) {
    FreeWaitEventSet(LatchWaitSet);
    LatchWaitSet = NULL;
  }
}

/* The position of the latch event in LatchWaitSet */
#define LatchWaitSetLatchPos 0

void InitializeLatchWaitSet(void) {
  int latch_pos PG_USED_FOR_ASSERTS_ONLY;

  Assert(LatchWaitSet == NULL);

  /* Set up the WaitEventSet used by WaitLatch(). */
  LatchWaitSet = CreateWaitEventSet(NULL, 1);
  latch_pos = AddWaitEventToSet(LatchWaitSet, WL_LATCH_SET, PGINVALID_SOCKET,
                                MyLatch, NULL);
  Assert(latch_pos == LatchWaitSetLatchPos);
}

/*
 * Initialize a process-local latch.
 */
void InitLatch(Latch *latch) {
  /* Not set: a latch bit left from before is taken. */
  (void)sm_take(INTERRUPT_LATCH);
}

/*
 * Wait for a given latch to be set, or until timeout is exceeded. 'wakeEvents'
 * is a bitmask that specifies which of those events to wait for. If the latch
 * is already set (and WL_LATCH_SET is given), the function returns immediately.
 *
 * The "timeout" is given in milliseconds. It must be >= 0 if WL_TIMEOUT flag
 * is given.  Although it is declared as "long", we don't actually support
 * timeouts longer than INT_MAX milliseconds.  Note that some extra overhead
 * is incurred when WL_TIMEOUT is given, so avoid using a timeout if possible.
 *
 * The latch must be owned by the current thread, ie. it must be a latch
 * it initialized with InitLatch.
 *
 * Returns bit mask indicating which condition(s) caused the wake-up. Note
 * that if multiple wake-up conditions are true, there is no guarantee that
 * we return all of them in one call, but we will return at least one.
 */
int WaitLatch(Latch *latch, int wakeEvents, long timeout,
              uint32 wait_event_info) {
  WaitEvent event;

  /*
   * Some callers may have a latch other than MyLatch, or no latch at all.
   * It's cheap to assign, so just do it every time.
   */
  if (!(wakeEvents & WL_LATCH_SET))
    latch = NULL;
  ModifyWaitEvent(LatchWaitSet, LatchWaitSetLatchPos, WL_LATCH_SET, latch);

  if (WaitEventSetWait(LatchWaitSet, (wakeEvents & WL_TIMEOUT) ? timeout : -1,
                       &event, 1, wait_event_info) == 0)
    return WL_TIMEOUT;
  else
    return event.events;
}

/*
 * Like WaitLatch, but with an extra socket argument for WL_SOCKET_*
 * conditions.
 *
 * When waiting on a socket, EOF and error conditions always cause the socket
 * to be reported as readable/writable/connected, so that the caller can deal
 * with the condition.
 *
 * NB: These days this is just a wrapper around the WaitEventSet API. When
 * using a latch very frequently, consider creating a longer living
 * WaitEventSet instead; that's more efficient.
 */
int WaitLatchOrSocket(Latch *latch, int wakeEvents, pgsocket sock, long timeout,
                      uint32 wait_event_info) {
  int ret = 0;
  int rc;
  WaitEvent event;
  WaitEventSet *set = CreateWaitEventSet(CurrentResourceOwner, 2);

  if (wakeEvents & WL_TIMEOUT)
    Assert(timeout >= 0);
  else
    timeout = -1;

  if (wakeEvents & WL_LATCH_SET)
    AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, latch, NULL);

  if (wakeEvents & WL_SOCKET_MASK) {
    int ev;

    ev = wakeEvents & WL_SOCKET_MASK;
    AddWaitEventToSet(set, ev, sock, NULL, NULL);
  }

  rc = WaitEventSetWait(set, timeout, &event, 1, wait_event_info);

  if (rc == 0)
    ret |= WL_TIMEOUT;
  else {
    ret |= event.events & (WL_LATCH_SET | WL_SOCKET_MASK);
  }

  FreeWaitEventSet(set);

  return ret;
}

/*
 * Sets a latch and wakes up anyone waiting on it.
 *
 * This is cheap if the latch is already set, otherwise not so much.
 *
 * NB: when calling this in a signal handler, be sure to save and restore
 * errno around it.  (That's standard practice in most signal handlers, of
 * course, but we used to omit it in handlers that only set a flag.)
 *
 * NB: this function is called from critical sections and signal handlers so
 * throwing an error is not a good idea.
 */
void SetLatch(Latch *latch) {
  /*
   * Raise this thread's INTERRUPT_LATCH, which also writes its wake fd, so a
   * WaitEventSet it sleeps in returns; safe in a signal handler.  A latch is
   * only ever its own thread's to set: another thread raises the bit through
   * the thread's handle (sm_interrupt()).
   */
  Assert(LatchIsMine(latch));
  sm_interrupt_self(INTERRUPT_LATCH);
}

/*
 * Clear the latch. Calling WaitLatch after this will sleep, unless
 * the latch is set again before the WaitLatch call.
 */
void ResetLatch(Latch *latch) {
  /* Only the owner should reset the latch */
  Assert(LatchIsMine(latch));

  /*
   * Taken with acquire: what a setter wrote before its set is seen by the
   * look the caller makes next.
   */
  (void)sm_take(INTERRUPT_LATCH);
}

/* Whether the latch, the calling thread's own, is set. */
bool LatchIsSet(Latch *latch) {
  Assert(LatchIsMine(latch));
  return (sm_pending_word()->load(std::memory_order_acquire) &
          INTERRUPT_LATCH) != 0;
}

/* Whether the latch is the calling thread's own, MyLatch. */
bool LatchIsMine(Latch *latch) { return latch != NULL && latch == MyLatch; }
