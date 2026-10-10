/*-------------------------------------------------------------------------
 *
 * startup.c
 *
 * The Startup process initialises the server.  There is no WAL to recover
 * from, so it gives the transaction counters their first values and ends.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/*
 * Time at which the most recent startup operation started.
 */
static thread_local /*LOCAL*/ TimestampTz startup_progress_phase_start_time;

/*
 * Indicates whether the startup progress interval mentioned by the user is
 * elapsed or not. TRUE if timeout occurred, FALSE otherwise.
 */
static thread_local /*LOCAL*/ volatile sig_atomic_t
    startup_progress_timer_expired = false;

/*
 * Time between progress updates for long-running startup operations.
 */
thread_local /*LOCAL*/ int log_startup_progress_interval = 10000; /* 10 sec */

/* ----------------------------------
 *	Startup Process main entry point
 * ----------------------------------
 */
void StartupProcessMain(const void *startup_data, size_t startup_data_len) {
  Assert(startup_data_len == 0);

  MyBackendType = B_STARTUP;
  AuxiliaryProcessMainCommon();

  /*
   * There is no WAL to replay, and StartupXLOG() returns at once, so the
   * signals the postmaster might send us can wait: ignore them.
   */
  pqsignal(SIGHUP, SIG_IGN);
  pqsignal(SIGINT, SIG_IGN);
  pqsignal(SIGTERM, SIG_IGN);
  /* SIGQUIT handler was already set up by InitPostmasterChild */
  InitializeTimeouts(); /* establishes SIGALRM handler */
  pqsignal(SIGPIPE, SIG_IGN);
  pqsignal(SIGUSR1, procsignal_sigusr1_handler);
  pqsignal(SIGUSR2, SIG_IGN);

  /*
   * Reset some signals that are accepted by postmaster but not here
   */
  pqsignal(SIGCHLD, SIG_DFL);

  /*
   * Unblock signals (they were blocked when the postmaster forked us)
   */
  sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

  /*
   * Do what we came for.
   */
  StartupXLOG();

  /*
   * Exit normally. Exit code 0 tells postmaster that we completed recovery
   * successfully.
   */
  proc_exit(0);
}

/*
 * Set a flag indicating that it's time to log a progress report.
 */
void startup_progress_timeout_handler(void) {
  startup_progress_timer_expired = true;
}

void disable_startup_progress_timeout(void) {
  /* Feature is disabled. */
  if (log_startup_progress_interval == 0)
    return;

  disable_timeout(STARTUP_PROGRESS_TIMEOUT, false);
  startup_progress_timer_expired = false;
}

/*
 * Set the start timestamp of the current operation and enable the timeout.
 */
void enable_startup_progress_timeout(void) {
  TimestampTz fin_time;

  /* Feature is disabled. */
  if (log_startup_progress_interval == 0)
    return;

  startup_progress_phase_start_time = GetCurrentTimestamp();
  fin_time = TimestampTzPlusMilliseconds(startup_progress_phase_start_time,
                                         log_startup_progress_interval);
  enable_timeout_every(STARTUP_PROGRESS_TIMEOUT, fin_time,
                       log_startup_progress_interval);
}

/*
 * A thin wrapper to first disable and then enable the startup progress
 * timeout.
 */
void begin_startup_progress_phase(void) {
  /* Feature is disabled. */
  if (log_startup_progress_interval == 0)
    return;

  disable_startup_progress_timeout();
  enable_startup_progress_timeout();
}

/*
 * Report whether startup progress timeout has occurred. Reset the timer flag
 * if it did, set the elapsed time to the out parameters and return true,
 * otherwise return false.
 */
bool has_startup_progress_timeout_expired(long *secs, int *usecs) {
  long seconds;
  int useconds;
  TimestampTz now;

  /* No timeout has occurred. */
  if (!startup_progress_timer_expired)
    return false;

  /* Calculate the elapsed time. */
  now = GetCurrentTimestamp();
  TimestampDifference(startup_progress_phase_start_time, now, &seconds,
                      &useconds);

  *secs = seconds;
  *usecs = useconds;
  startup_progress_timer_expired = false;

  return true;
}
