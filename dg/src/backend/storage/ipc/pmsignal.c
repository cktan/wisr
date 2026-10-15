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

#include <signal.h>
#include <unistd.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
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
 * Signal handler to be notified if postmaster dies.
 */
#ifdef USE_POSTMASTER_DEATH_SIGNAL
thread_local /*LOCAL*/ volatile sig_atomic_t postmaster_possibly_dead = false;

static void postmaster_death_handler(SIGNAL_ARGS) {
  postmaster_possibly_dead = true;
}

/*
 * The available signals depend on the OS.  SIGUSR1 and SIGUSR2 are already
 * used for other things, so choose another one.
 *
 * Currently, we assume that we can always find a signal to use.  That
 * seems like a reasonable assumption for all platforms that are modern
 * enough to have a parent-death signaling mechanism.
 */
#if defined(SIGINFO)
#define POSTMASTER_DEATH_SIGNAL SIGINFO
#elif defined(SIGPWR)
#define POSTMASTER_DEATH_SIGNAL SIGPWR
#else
#error "cannot find a signal to use for postmaster death"
#endif

#endif /* USE_POSTMASTER_DEATH_SIGNAL */


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

/*
 * PostmasterIsAliveInternal - check whether postmaster process is still alive
 *
 * This is the slow path of PostmasterIsAlive(), where the caller has already
 * checked 'postmaster_possibly_dead'.  (On platforms that don't support
 * a signal for parent death, PostmasterIsAlive() is just an alias for this.)
 */
bool PostmasterIsAliveInternal(void) {
#ifdef USE_POSTMASTER_DEATH_SIGNAL
  /*
   * Reset the flag before checking, so that we don't miss a signal if
   * postmaster dies right after the check.  If postmaster was indeed dead,
   * we'll re-arm it before returning to caller.
   */
  postmaster_possibly_dead = false;
#endif

#ifndef WIN32
  {
    char c;
    ssize_t rc;

    rc = read(postmaster_alive_fds[POSTMASTER_FD_WATCH], &c, 1);

    /*
     * In the usual case, the postmaster is still alive, and there is no
     * data in the pipe.
     */
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;
    else {
      /*
       * Postmaster is dead, or something went wrong with the read()
       * call.
       */

#ifdef USE_POSTMASTER_DEATH_SIGNAL
      postmaster_possibly_dead = true;
#endif

      if (rc < 0)
        elog(FATAL, "read on postmaster death monitoring pipe failed: %m");
      else if (rc > 0)
        elog(FATAL, "unexpected data in postmaster death monitoring pipe");

      return false;
    }
  }

#else /* WIN32 */
  if (WaitForSingleObject(PostmasterHandle, 0) == WAIT_TIMEOUT)
    return true;
  else {
#ifdef USE_POSTMASTER_DEATH_SIGNAL
    postmaster_possibly_dead = true;
#endif
    return false;
  }
#endif /* WIN32 */
}

/*
 * PostmasterDeathSignalInit - request signal on postmaster death if possible
 */
void PostmasterDeathSignalInit(void) {
#ifdef USE_POSTMASTER_DEATH_SIGNAL
  int signum = POSTMASTER_DEATH_SIGNAL;

  /* Register our signal handler. */
  pqsignal(signum, postmaster_death_handler);

  /* Request a signal on parent exit. */
#if defined(PR_SET_PDEATHSIG)
  if (prctl(PR_SET_PDEATHSIG, signum) < 0)
    elog(ERROR, "could not request parent death signal: %m");
#elif defined(PROC_PDEATHSIG_CTL)
  if (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &signum) < 0)
    elog(ERROR, "could not request parent death signal: %m");
#else
#error                                                                         \
    "USE_POSTMASTER_DEATH_SIGNAL set, but there is no mechanism to request the signal"
#endif

  /*
   * Just in case the parent was gone already and we missed it, we'd better
   * check the slow way on the first call.
   */
  postmaster_possibly_dead = true;
#endif /* USE_POSTMASTER_DEATH_SIGNAL */
}
