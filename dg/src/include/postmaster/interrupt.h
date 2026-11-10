/*-------------------------------------------------------------------------
 *
 * interrupt.h
 *	  Interrupt handling routines.
 *
 * Responses to interrupts are fairly varied and many types of backends
 * have their own implementations, but we provide a few generic things
 * here to facilitate code reuse.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/include/postmaster/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <signal.h>

extern thread_local /*LOCAL*/ PGDLLIMPORT bool
    ConfigReloadPending;

extern void SignalHandlerForConfigReload(void);
extern void SignalHandlerForCrashExit(void);

#endif
