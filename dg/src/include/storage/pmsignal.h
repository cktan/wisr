/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  routines for signaling between the postmaster and its child processes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pmsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PMSIGNAL_H
#define PMSIGNAL_H

#include <signal.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif

/*
 * Reasons why the postmaster would send SIGQUIT to its children.
 */
typedef enum {
  PMQUIT_NOT_SENT = 0, /* postmaster hasn't sent SIGQUIT */
  PMQUIT_FOR_CRASH,    /* some other backend bought the farm */
  PMQUIT_FOR_STOP,     /* immediate stop was commanded */
} QuitSignalReason;

/* PMSignalData is an opaque struct, details known only within pmsignal.c */
typedef struct PMSignalData PMSignalData;

/*
 * prototypes for functions in pmsignal.c
 */
extern Size PMSignalShmemSize(void);
extern void PMSignalShmemInit(void);
extern void SetQuitSignalReason(QuitSignalReason reason);
extern QuitSignalReason GetQuitSignalReason(void);
extern void MarkPostmasterChildSlotAssigned(int slot);
extern bool MarkPostmasterChildSlotUnassigned(int slot);
extern bool IsPostmasterChildWalSender(int slot);
extern void RegisterPostmasterChildActive(void);
extern void MarkPostmasterChildWalSender(void);
extern bool PostmasterIsAliveInternal(void);
extern void PostmasterDeathSignalInit(void);

/*
 * Do we have a way to ask for a signal on parent death?
 *
 * If we do, pmsignal.c will set up a signal handler, that sets a flag when
 * the parent dies.  Checking the flag first makes PostmasterIsAlive() a lot
 * cheaper in usual case that the postmaster is alive.
 */
#if (defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_PDEATHSIG)) ||                \
    (defined(HAVE_SYS_PROCCTL_H) && defined(PROC_PDEATHSIG_CTL))
#define USE_POSTMASTER_DEATH_SIGNAL
#endif

#ifdef USE_POSTMASTER_DEATH_SIGNAL
extern thread_local /*LOCAL*/ PGDLLIMPORT volatile sig_atomic_t
    postmaster_possibly_dead;

static inline bool PostmasterIsAlive(void) {
  if (likely(!postmaster_possibly_dead))
    return true;
  return PostmasterIsAliveInternal();
}
#else
#define PostmasterIsAlive() PostmasterIsAliveInternal()
#endif

#endif /* PMSIGNAL_H */
