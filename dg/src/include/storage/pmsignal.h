/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  routines for signaling between the postmaster and its child processes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/storage/pmsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PMSIGNAL_H
#define PMSIGNAL_H

#include <signal.h>

/*
 * Reasons why the postmaster would send SIGQUIT to its children.
 */
typedef enum {
  PMQUIT_NOT_SENT = 0, /* postmaster hasn't sent SIGQUIT */
  PMQUIT_FOR_CRASH,    /* some other backend bought the farm */
  PMQUIT_FOR_STOP,     /* immediate stop was commanded */
} QuitSignalReason;

/* PMSignalData is an opaque struct, details known only within pmsignal.c */

/*
 * prototypes for functions in pmsignal.c
 */
extern void SetQuitSignalReason(QuitSignalReason reason);
extern QuitSignalReason GetQuitSignalReason(void);

#endif /* PMSIGNAL_H */
