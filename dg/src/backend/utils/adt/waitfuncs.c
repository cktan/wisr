/*-------------------------------------------------------------------------
 *
 * waitfuncs.c
 *		Functions for SQL access to syntheses of multiple contention
 *types.
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/utils/adt/waitfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/session_registry.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/wait_event.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

/*
 * pg_isolation_test_session_is_blocked - support function for isolationtester
 *
 * Check if specified PID is blocked by any of the PIDs listed in the second
 * argument.  Currently, this looks for blocking caused by waiting for
 * injection points, heavyweight locks, or safe snapshots.  We ignore blockage
 * caused by PIDs not directly under the isolationtester's control, eg
 * autovacuum.
 *
 * This is an undocumented function intended for use by the isolation tester,
 * and may change in future releases as required for testing purposes.
 */
Datum pg_isolation_test_session_is_blocked(PG_FUNCTION_ARGS) {
  int blocked_pid = PG_GETARG_INT32(0);
  ArrayType *interesting_pids_a = PG_GETARG_ARRAYTYPE_P(1);
  ArrayType *blocking_pids_a;
  int32 *interesting_pids;
  int32 *blocking_pids;
  int num_interesting_pids;
  int num_blocking_pids;
  int i, j;

  if (session_by_pid(blocked_pid) == nullptr)
    PG_RETURN_BOOL(false); /* session gone: definitely unblocked */

  /* Validate the passed-in array */
  Assert(ARR_ELEMTYPE(interesting_pids_a) == INT4OID);
  if (array_contains_nulls(interesting_pids_a))
    elog(ERROR, "array must not contain nulls");
  interesting_pids = (int32 *)ARR_DATA_PTR(interesting_pids_a);
  num_interesting_pids = ArrayGetNItems(ARR_NDIM(interesting_pids_a),
                                        ARR_DIMS(interesting_pids_a));

  /*
   * Get the PIDs of all sessions blocking the given session's attempt to
   * acquire heavyweight locks.
   */
  blocking_pids_a =
      DatumGetArrayTypeP(DirectFunctionCall1(pg_blocking_pids, blocked_pid));

  Assert(ARR_ELEMTYPE(blocking_pids_a) == INT4OID);
  Assert(!array_contains_nulls(blocking_pids_a));
  blocking_pids = (int32 *)ARR_DATA_PTR(blocking_pids_a);
  num_blocking_pids =
      ArrayGetNItems(ARR_NDIM(blocking_pids_a), ARR_DIMS(blocking_pids_a));

  /*
   * Check if any of these are in the list of interesting PIDs, that being
   * the sessions that the isolation tester is running.  We don't use
   * "arrayoverlaps" here, because it would lead to cache lookups and one of
   * our goals is to run quickly with debug_discard_caches > 0.  We expect
   * blocking_pids to be usually empty and otherwise a very small number in
   * isolation tester cases, so make that the outer loop of a naive search
   * for a match.
   */
  for (i = 0; i < num_blocking_pids; i++)
    for (j = 0; j < num_interesting_pids; j++) {
      if (blocking_pids[i] == interesting_pids[j])
        PG_RETURN_BOOL(true);
    }

  PG_RETURN_BOOL(false);
}
