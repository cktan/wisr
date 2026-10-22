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

#include "access/smstore.h"
#include "catalog/pg_namespace_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/advisory_lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datadir.h"
#include "utils/lsyscache.h"

/* The columns of pg_lock_status(), in order (pg_proc.dat). */
enum {
  LOCKFUNCS_LOCKTYPE,
  LOCKFUNCS_DATABASE,
  LOCKFUNCS_RELATION,
  LOCKFUNCS_PAGE,
  LOCKFUNCS_TUPLE,
  LOCKFUNCS_VIRTUALXID,
  LOCKFUNCS_TRANSACTIONID,
  LOCKFUNCS_CLASSID,
  LOCKFUNCS_OBJID,
  LOCKFUNCS_OBJSUBID,
  LOCKFUNCS_VIRTUALTRANSACTION,
  LOCKFUNCS_PID,
  LOCKFUNCS_MODE,
  LOCKFUNCS_GRANTED,
  LOCKFUNCS_FASTPATH,
  LOCKFUNCS_WAITSTART,
  LOCKFUNCS_NCOLUMNS
};

/* Add a row of pg_lock_status(): every column NULL but those given. */
static void lockfuncs_put_row(ReturnSetInfo *rsinfo, Datum *values,
                              bool *nulls) {
  values[LOCKFUNCS_FASTPATH] = BoolGetDatum(false);
  nulls[LOCKFUNCS_FASTPATH] = false;
  tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * pg_lock_status - produce a view with one row per held or awaited lock mode
 *
 * PostgreSQL's lock manager is gone (dg/WORKNOTE.md step 156).  A table's
 * locks, a row's and a catalog key's are sm1's lock table's
 * (sm_lock_status()), and an advisory lock is digres's own
 * (storage/advisory_lock.h).  A mode is named as sm names it (IS, IX, S,
 * SIX, X), and an advisory lock's S or X.  A writer waiting behind another's
 * uncommitted row takes no lock and is not listed: pg_blocking_pids() names
 * whom it waits for.  No virtual xid or xid is locked, and nothing waits for
 * one.
 */
Datum pg_lock_status(PG_FUNCTION_ARGS) {
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

  /* Every row is made now, so no lock table is held across calls. */
  InitMaterializedSRF(fcinfo, 0);

  for (const datadir_lock_t &l : datadir_sm_locks()) {
    Datum values[LOCKFUNCS_NCOLUMNS] = {0};
    bool nulls[LOCKFUNCS_NCOLUMNS];
    const char *locktype = "object";

    memset(nulls, true, sizeof(nulls));
    values[LOCKFUNCS_DATABASE] = ObjectIdGetDatum(l.database);
    nulls[LOCKFUNCS_DATABASE] = false;
    if (l.name == DATADIR_LOCK_OBJECT) {
      /* A schema of this database is an object; any other oid a relation. */
      if (l.database == MyDatabaseId && get_namespace_name(l.oid) != NULL) {
        values[LOCKFUNCS_CLASSID] = ObjectIdGetDatum(NamespaceRelationId);
        values[LOCKFUNCS_OBJID] = ObjectIdGetDatum(l.oid);
        values[LOCKFUNCS_OBJSUBID] = Int16GetDatum(0);
        nulls[LOCKFUNCS_CLASSID] = nulls[LOCKFUNCS_OBJID] =
            nulls[LOCKFUNCS_OBJSUBID] = false;
      } else {
        locktype = "relation";
        values[LOCKFUNCS_RELATION] = ObjectIdGetDatum(l.oid);
        nulls[LOCKFUNCS_RELATION] = false;
      }
    } else if (l.name == DATADIR_LOCK_ROW) {
      ItemPointerData tid;

      /*
       * The row's tid, as a scan gives it (smstore_rowno_to_tid()); none for
       * a row of a table with a primary key, whose lock names its key
       * (../sm/CLUSTER.md section 10).
       */
      locktype = "tuple";
      if (0 != l.rowno) {
        smstore_rowno_to_tid(l.rowno, &tid);
        values[LOCKFUNCS_PAGE] =
            Int32GetDatum((int32)ItemPointerGetBlockNumber(&tid));
        values[LOCKFUNCS_TUPLE] =
            Int16GetDatum((int16)ItemPointerGetOffsetNumber(&tid));
        nulls[LOCKFUNCS_PAGE] = nulls[LOCKFUNCS_TUPLE] = false;
      }
      if (l.oid != 0) {
        values[LOCKFUNCS_RELATION] = ObjectIdGetDatum(l.oid);
        nulls[LOCKFUNCS_RELATION] = false;
      }
    }
    values[LOCKFUNCS_LOCKTYPE] = CStringGetTextDatum(locktype);
    nulls[LOCKFUNCS_LOCKTYPE] = false;
    if (l.pid != 0) {
      values[LOCKFUNCS_PID] = Int32GetDatum(l.pid);
      nulls[LOCKFUNCS_PID] = false;
    }
    values[LOCKFUNCS_MODE] = CStringGetTextDatum(l.mode);
    values[LOCKFUNCS_GRANTED] = BoolGetDatum(l.granted);
    nulls[LOCKFUNCS_MODE] = nulls[LOCKFUNCS_GRANTED] = false;
    lockfuncs_put_row(rsinfo, values, nulls);
  }

  for (const advisory_status_t &a : advisory_status()) {
    Datum values[LOCKFUNCS_NCOLUMNS] = {0};
    bool nulls[LOCKFUNCS_NCOLUMNS];

    /* Its key as PostgreSQL's pg_locks gives an advisory lock's. */
    memset(nulls, true, sizeof(nulls));
    values[LOCKFUNCS_LOCKTYPE] = CStringGetTextDatum("advisory");
    values[LOCKFUNCS_DATABASE] = ObjectIdGetDatum(a.key.database);
    values[LOCKFUNCS_CLASSID] = ObjectIdGetDatum(a.key.key1);
    values[LOCKFUNCS_OBJID] = ObjectIdGetDatum(a.key.key2);
    values[LOCKFUNCS_OBJSUBID] = Int16GetDatum((int16)a.key.form);
    values[LOCKFUNCS_PID] = Int32GetDatum(a.pid);
    values[LOCKFUNCS_MODE] = CStringGetTextDatum(a.exclusive ? "X" : "S");
    values[LOCKFUNCS_GRANTED] = BoolGetDatum(a.granted);
    for (int col : {LOCKFUNCS_LOCKTYPE, LOCKFUNCS_DATABASE, LOCKFUNCS_CLASSID,
                    LOCKFUNCS_OBJID, LOCKFUNCS_OBJSUBID, LOCKFUNCS_PID,
                    LOCKFUNCS_MODE, LOCKFUNCS_GRANTED})
      nulls[col] = false;
    lockfuncs_put_row(rsinfo, values, nulls);
  }
  return (Datum)0;
}

/*
 * pg_blocking_pids - produce an array of the PIDs blocking given PID
 *
 * The reported PIDs are those that hold an advisory lock conflicting with
 * blocked_pid's wait for one (storage/advisory_lock.h), and those it waits
 * for in sm: a table lock's holder, a row's writer (datadir_sm_blockers()).
 */
Datum pg_blocking_pids(PG_FUNCTION_ARGS) {
  int blocked_pid = PG_GETARG_INT32(0);
  std::vector<int> advisory = advisory_blockers(blocked_pid);
  /* And whom it waits for in sm: a table's lock, a row's, a row's writer */
  std::vector<int> in_sm = datadir_sm_blockers(blocked_pid);
  Datum *arrayelems =
      (Datum *)palloc((advisory.size() + in_sm.size() + 1) * sizeof(Datum));
  int narrayelems = 0;

  for (int pid : advisory)
    arrayelems[narrayelems++] = Int32GetDatum(pid);
  for (int pid : in_sm)
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
