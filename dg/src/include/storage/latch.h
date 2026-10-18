/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Routines for interprocess latches
 *
 * A latch is a boolean variable, with operations that let processes sleep
 * until it is set. A latch can be set from another process, or a signal
 * handler within the same process.
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable. Because on some platforms an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before
 * entering the sleep, the common pattern must periodically wake up and
 * poll the flag variable. The pselect() system call was invented to solve
 * this problem, but it is not portable enough. Latches are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring quick response to signals from other processes.
 *
 * A latch is a thread's, initialized by InitLatch: only the thread owning
 * it can wait on it, but any thread can set it, and a signal handler in the
 * owning thread.  PostgreSQL's shared latches, a PGPROC's, are gone
 * (dg/WORKNOTE.md step 158): a session is reached by its thread.
 *
 * There are three basic operations on a latch:
 *
 * SetLatch		- Sets the latch
 * ResetLatch	- Clears the latch, allowing it to be set again
 * WaitLatch	- Waits for the latch to become set
 *
 * WaitLatch includes a provision for timeouts (which should be avoided
 * when possible, as they incur extra overhead) and a provision for
 * postmaster child processes to wake up immediately on postmaster death.
 * See latch.c for detailed specifications for the exported functions.
 *
 * The correct pattern to wait for event(s) is:
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * It's important to reset the latch *before* checking if there's work to
 * do. Otherwise, if someone sets the latch between the check and the
 * ResetLatch call, you will miss it and Wait will incorrectly block.
 *
 * Another valid coding pattern looks like:
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition
 *satisfied WaitLatch(); ResetLatch();
 * }
 *
 * This is useful to reduce latch traffic if it's expected that the loop's
 * termination condition will often be satisfied in the first iteration;
 * the cost is an extra loop iteration before blocking when it is not.
 * What must be avoided is placing any checks for asynchronous events after
 * WaitLatch and before ResetLatch, as that creates a race condition.
 *
 * To wake up the waiter, you must first set a global flag or something
 * else that the wait loop tests in the "if (work to do)" part, and call
 * SetLatch *after* that. SetLatch is designed to return quickly if the
 * latch is already set.
 *
 * On some platforms, signals will not interrupt the latch wait primitive
 * by themselves.  Therefore, it is critical that any signal handler that
 * is meant to terminate a WaitLatch wait calls SetLatch.
 *
 * Note that use of the thread's latch (MyLatch) is generally better than an
 * ad-hoc latch for signaling it.  This is because generic signal handlers
 * will call SetLatch on MyLatch only, so using any other latch effectively
 * precludes use of any generic handler.
 *
 *
 * See also WaitEventSets in waiteventset.h. They allow to wait for latches
 * being set and additional events - postmaster dying and socket readiness of
 * several sockets currently - at the same time.  On many platforms using a
 * long lived event set is more efficient than using WaitLatch or
 * WaitLatchOrSocket.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

#include "storage/waiteventset.h" /* for WL_* arguments to WaitLatch */
#include "sm_sys.h"

/*
 * Latch structure should be treated as opaque and only accessed through
 * the public functions. It is defined here to allow embedding Latches as
 * part of bigger structs.
 */
/*
 * A latch is a thread's own: its flag is the thread's INTERRUPT_LATCH bit
 * (sm_sys.h), which SetLatch() raises and ResetLatch() takes.  The Latch
 * holds nothing: it is a token, MyLatch, a thread's LocalLatchData, that
 * names the calling thread's bit to PostgreSQL's API.  Another thread
 * raises the bit through the thread's handle (sm_interrupt()), not a Latch.
 */
typedef struct Latch {
} Latch;

/*
 * prototypes for functions in latch.c
 */
extern void InitLatch(Latch *latch);
extern void SetLatch(Latch *latch);
extern void ResetLatch(Latch *latch);
extern bool LatchIsSet(Latch *latch);
extern bool LatchIsMine(Latch *latch);

extern int WaitLatch(Latch *latch, int wakeEvents, long timeout,
                     uint32 wait_event_info);
extern int WaitLatchOrSocket(Latch *latch, int wakeEvents, pgsocket sock,
                             long timeout, uint32 wait_event_info);
extern void InitializeLatchWaitSet(void);
extern void ShutdownLatchWaitSet(void);

#endif /* LATCH_H */
