/*-------------------------------------------------------------------------
 *
 * signalfuncs.c
 *	  Functions for signaling backends
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/signalfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/session_registry.h"
#include "utils/acl.h"
#include "utils/control_block.h"
#include "utils/fmgrprotos.h"
#include "utils/logfile.h"
#include "utils/wait_event.h"

/*
 * Send a signal to another backend.
 *
 * The signal is delivered if the user is either a superuser or the same
 * role as the backend being signaled. For "dangerous" signals, an explicit
 * check for superuser needs to be done prior to calling this function.
 *
 * Returns 0 on success, 1 on general failure, 2 on normal permission error,
 * 3 if the caller needs to be a superuser, and 4 if the caller needs to have
 * privileges of pg_signal_autovacuum_worker.
 *
 * In the event of a general failure (return code 1), a warning message will
 * be emitted. For permission errors, doing that is the responsibility of
 * the caller.
 */
#define SIGNAL_BACKEND_SUCCESS 0
#define SIGNAL_BACKEND_ERROR 1
#define SIGNAL_BACKEND_NOPERMISSION 2
#define SIGNAL_BACKEND_NOSUPERUSER 3
static int pg_signal_backend(int pid, int sig) {
  /* The session's record: its role, and the thread to interrupt. */
  session_ref_t record = pid != 0 ? session_by_pid(pid) : nullptr;
  Oid role;

  /*
   * No record if the pid is no session's, the postmaster's included.  A
   * session found here may end before it is interrupted; as every caller
   * asks for it to end anyway, that is no problem.
   */
  if (record == nullptr) {
    /*
     * This is just a warning so a loop-through-resultset will not abort
     * if one backend terminated on its own during the run.
     */
    ereport(WARNING,
            (errmsg("PID %d is not a PostgreSQL backend process", pid)));

    return SIGNAL_BACKEND_ERROR;
  }

  /*
   * Only allow superusers to signal superuser-owned backends.  Any process
   * not advertising a role might have the importance of a superuser-owned
   * backend, so treat it that way.
   *
   * Otherwise, users can signal backends for roles they have privileges of.
   */
  role = record->role.load(std::memory_order_acquire);
  if (!OidIsValid(role) || superuser_arg(role)) {
    if (!superuser())
      return SIGNAL_BACKEND_NOSUPERUSER;
  } else if (!has_privs_of_role(GetUserId(), role) &&
             !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
    return SIGNAL_BACKEND_NOPERMISSION;

  /* Its own thread: a session ended since is interrupted harmlessly. */
  session_interrupt(record, InterruptsOfSignal(sig));
  return SIGNAL_BACKEND_SUCCESS;
}

/*
 * Signal to cancel a backend process.  This is allowed if you are a member of
 * the role whose process is being canceled.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum pg_cancel_backend(PG_FUNCTION_ARGS) {
  int r = pg_signal_backend(PG_GETARG_INT32(0), SIGINT);

  if (r == SIGNAL_BACKEND_NOSUPERUSER)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to cancel query"),
                    errdetail("Only roles with the %s attribute may cancel "
                              "queries of roles with the %s attribute.",
                              "SUPERUSER", "SUPERUSER")));

  if (r == SIGNAL_BACKEND_NOPERMISSION)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to cancel query"),
                    errdetail("Only roles with privileges of the role whose "
                              "query is being canceled or with privileges of "
                              "the \"%s\" role may cancel this query.",
                              "pg_signal_backend")));

  PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Wait until there is no backend process with the given PID and return true.
 * On timeout, a warning is emitted and false is returned.
 */
static bool pg_wait_until_termination(int pid, int64 timeout) {
  /*
   * Wait in steps of waittime milliseconds until this function exits or
   * timeout.
   */
  int64 waittime = 100;

  /*
   * Initially remaining time is the entire timeout specified by the user.
   */
  int64 remainingtime = timeout;

  /*
   * Check existence of the backend. If the backend still exists, then wait
   * for waittime milliseconds, again check for the existence. Repeat this
   * until timeout or an error occurs or a pending interrupt such as query
   * cancel gets processed.
   */
  do {
    if (remainingtime < waittime)
      waittime = remainingtime;

    if (kill(pid, 0) == -1) {
      if (errno == ESRCH)
        return true;
      else
        ereport(
            ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg(
                 "could not check the existence of the backend with PID %d: %m",
                 pid)));
    }

    /* Process interrupts, if any, before waiting */
    CHECK_FOR_INTERRUPTS();

    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT, waittime,
                    WAIT_EVENT_BACKEND_TERMINATION);

    ResetLatch(MyLatch);

    remainingtime -= waittime;
  } while (remainingtime > 0);

  ereport(WARNING,
          (errmsg_plural("backend with PID %d did not terminate within %" PRId64
                         " millisecond",
                         "backend with PID %d did not terminate within %" PRId64
                         " milliseconds",
                         timeout, pid, timeout)));

  return false;
}

/*
 * Send a signal to terminate a backend process. This is allowed if you are a
 * member of the role whose process is being terminated. If the timeout input
 * argument is 0, then this function just signals the backend and returns
 * true.  If timeout is nonzero, then it waits until no process has the given
 * PID; if the process ends within the timeout, true is returned, and if the
 * timeout is exceeded, a warning is emitted and false is returned.
 *
 * Note that only superusers can signal superuser-owned processes.
 */
Datum pg_terminate_backend(PG_FUNCTION_ARGS) {
  int pid;
  int r;
  int timeout; /* milliseconds */

  pid = PG_GETARG_INT32(0);
  timeout = PG_GETARG_INT64(1);

  if (timeout < 0)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("\"timeout\" must not be negative")));

  r = pg_signal_backend(pid, SIGTERM);

  if (r == SIGNAL_BACKEND_NOSUPERUSER)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to terminate process"),
                    errdetail("Only roles with the %s attribute may terminate "
                              "processes of roles with the %s attribute.",
                              "SUPERUSER", "SUPERUSER")));

  if (r == SIGNAL_BACKEND_NOPERMISSION)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to terminate process"),
                    errdetail("Only roles with privileges of the role whose "
                              "process is being terminated or with privileges "
                              "of the \"%s\" role may terminate this process.",
                              "pg_signal_backend")));

  /* Wait only on success and if actually requested */
  if (r == SIGNAL_BACKEND_SUCCESS && timeout > 0)
    PG_RETURN_BOOL(pg_wait_until_termination(pid, timeout));
  else
    PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * Signal to reload the database configuration
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
Datum pg_reload_conf(PG_FUNCTION_ARGS) {
  if (kill(pcb->PostmasterPid, SIGHUP)) {
    ereport(WARNING, (errmsg("failed to send signal to postmaster: %m")));
    PG_RETURN_BOOL(false);
  }

  PG_RETURN_BOOL(true);
}

/*
 * Rotate log file
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
Datum pg_rotate_logfile(PG_FUNCTION_ARGS) {
  if (!pcb->Logging_collector) {
    ereport(
        WARNING,
        (errmsg("rotation not possible because log collection not active")));
    PG_RETURN_BOOL(false);
  }

  PG_RETURN_BOOL(logfile_rotate());
}
