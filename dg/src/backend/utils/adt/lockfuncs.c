/*-------------------------------------------------------------------------
 *
 * lockfuncs.c
 *		Functions for SQL access to various lock-manager capabilities.
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/utils/adt/lockfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "storage/advisory_lock.h"
#include "utils/array.h"
#include "utils/builtins.h"

/*
 * pg_lock_status - produce a view with one row per held or awaited lock mode
 *
 * No row: PostgreSQL's lock manager is gone (dg/WORKNOTE.md step 156).  A
 * table's locks are sm's and an advisory lock is digres's own
 * (storage/advisory_lock.h), and neither is listed yet.
 */
Datum pg_lock_status(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;

  if (SRF_IS_FIRSTCALL())
    (void)SRF_FIRSTCALL_INIT();
  funcctx = SRF_PERCALL_SETUP();
  SRF_RETURN_DONE(funcctx);
}

/*
 * pg_blocking_pids - produce an array of the PIDs blocking given PID
 *
 * The reported PIDs are those that hold an advisory lock conflicting with
 * blocked_pid's wait for one (storage/advisory_lock.h).  A wait for a lock
 * of sm's is not reported yet: sm1 lists no waits.
 */
Datum pg_blocking_pids(PG_FUNCTION_ARGS) {
  int blocked_pid = PG_GETARG_INT32(0);
  std::vector<int> advisory = advisory_blockers(blocked_pid);
  Datum *arrayelems = (Datum *)palloc((advisory.size() + 1) * sizeof(Datum));
  int narrayelems = 0;

  for (int pid : advisory)
    arrayelems[narrayelems++] = Int32GetDatum(pid);
  PG_RETURN_ARRAYTYPE_P(
      construct_array_builtin(arrayelems, narrayelems, INT4OID));
}

/*
 * Functions for manipulating advisory locks, which are digres's own
 * (storage/advisory_lock.h): a key is the locktag's fields PostgreSQL used.
 *
 *	database: MyDatabaseId ... ensures locks are local to each database
 *	key1: first of 2 int4 keys, or high-order half of an int8 key
 *	key2: second of 2 int4 keys, or low-order half of an int8 key
 *	form: 1 if using an int8 key, 2 if using 2 int4 keys
 */
#define ADVISORY_KEY_INT64(key64) \
	(advisory_key_t{MyDatabaseId, (uint32) ((key64) >> 32), (uint32) (key64), 1})
#define ADVISORY_KEY_INT32(key1, key2) \
	(advisory_key_t{MyDatabaseId, (uint32) (key1), (uint32) (key2), 2})

/*
 * pg_advisory_lock(int8) - acquire exclusive lock on an int8 key
 */
Datum pg_advisory_lock_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);

  (void)advisory_lock(tag, true, true, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock(int8) - acquire xact scoped
 * exclusive lock on an int8 key
 */
Datum pg_advisory_xact_lock_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);

  (void)advisory_lock(tag, true, false, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int8) - acquire share lock on an int8 key
 */
Datum pg_advisory_lock_shared_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);

  (void)advisory_lock(tag, false, true, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock_shared(int8) - acquire xact scoped
 * share lock on an int8 key
 */
Datum pg_advisory_xact_lock_shared_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);

  (void)advisory_lock(tag, false, false, false);

  PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int8) - acquire exclusive lock on an int8 key, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_lock_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_lock(tag, true, true, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_xact_lock(int8) - acquire xact scoped
 * exclusive lock on an int8 key, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_xact_lock_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_lock(tag, true, false, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_lock_shared(int8) - acquire share lock on an int8 key, no
 * wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_lock_shared_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_lock(tag, false, true, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_xact_lock_shared(int8) - acquire xact scoped
 * share lock on an int8 key, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_xact_lock_shared_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_lock(tag, false, false, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock(int8) - release exclusive lock on an int8 key
 *
 * Returns true if successful, false if lock was not held
 */
Datum pg_advisory_unlock_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_unlock(tag, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int8) - release share lock on an int8 key
 *
 * Returns true if successful, false if lock was not held
 */
Datum pg_advisory_unlock_shared_int8(PG_FUNCTION_ARGS) {
  int64 key = PG_GETARG_INT64(0);
  advisory_key_t tag = ADVISORY_KEY_INT64(key);
  bool res;

  res = advisory_unlock(tag, false);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_lock(int4, int4) - acquire exclusive lock on 2 int4 keys
 */
Datum pg_advisory_lock_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);

  (void)advisory_lock(tag, true, true, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock(int4, int4) - acquire xact scoped
 * exclusive lock on 2 int4 keys
 */
Datum pg_advisory_xact_lock_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);

  (void)advisory_lock(tag, true, false, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int4, int4) - acquire share lock on 2 int4 keys
 */
Datum pg_advisory_lock_shared_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);

  (void)advisory_lock(tag, false, true, false);

  PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock_shared(int4, int4) - acquire xact scoped
 * share lock on 2 int4 keys
 */
Datum pg_advisory_xact_lock_shared_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);

  (void)advisory_lock(tag, false, false, false);

  PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int4, int4) - acquire exclusive lock on 2 int4 keys, no
 * wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_lock_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_lock(tag, true, true, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_xact_lock(int4, int4) - acquire xact scoped
 * exclusive lock on 2 int4 keys, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_xact_lock_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_lock(tag, true, false, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_lock_shared(int4, int4) - acquire share lock on 2 int4 keys,
 * no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_lock_shared_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_lock(tag, false, true, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_try_advisory_xact_lock_shared(int4, int4) - acquire xact scoped
 * share lock on 2 int4 keys, no wait
 *
 * Returns true if successful, false if lock not available
 */
Datum pg_try_advisory_xact_lock_shared_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_lock(tag, false, false, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock(int4, int4) - release exclusive lock on 2 int4 keys
 *
 * Returns true if successful, false if lock was not held
 */
Datum pg_advisory_unlock_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_unlock(tag, true);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int4, int4) - release share lock on 2 int4 keys
 *
 * Returns true if successful, false if lock was not held
 */
Datum pg_advisory_unlock_shared_int4(PG_FUNCTION_ARGS) {
  int32 key1 = PG_GETARG_INT32(0);
  int32 key2 = PG_GETARG_INT32(1);
  advisory_key_t tag = ADVISORY_KEY_INT32(key1, key2);
  bool res;

  res = advisory_unlock(tag, false);

  PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_all() - release all advisory locks
 */
Datum pg_advisory_unlock_all(PG_FUNCTION_ARGS) {
  advisory_unlock_all();

  PG_RETURN_VOID();
}
