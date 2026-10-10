/*-------------------------------------------------------------------------
 *
 * launch_backend.c
 *	  The names of the kinds of backend.
 *
 * Every backend is a thread of the postmaster's process
 * (backend_thread.c); what is left here is the name of each kind.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/launch_backend.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/postmaster.h"

/* The names of the kinds of backend. */
static const char *const child_process_names[] = {
    [B_INVALID] = "invalid",
    [B_BACKEND] = "backend",
    [B_DEAD_END_BACKEND] = "dead-end backend",
    [B_WAL_SENDER] = "wal sender",
    [B_STANDALONE_BACKEND] = "standalone backend",
};

/* The name of a kind of backend, for messages. */
const char *PostmasterChildName(BackendType child_type) {
  return child_process_names[child_type];
}
