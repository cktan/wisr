/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  What is left of PostgreSQL's shared memory: pg_numa_available().
 *
 * PostgreSQL laid out the structures its processes share in one shared
 * memory block, sized ahead of time, and found each by name.  digres is one
 * process, whose threads share all its memory: each structure is a field
 * of the process control block (utils/control_block.h), made before any
 * thread starts, and the registry of names is gone (dg/WORKNOTE.md step
 * 173).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "port/pg_numa.h"

/* Is NUMA support available? */
Datum pg_numa_available(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(pg_numa_init() != -1);
}
