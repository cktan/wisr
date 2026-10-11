/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		Two-phase commit support functions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/access/transam/twophase.c
 *
 * NOTES
 *		Each global transaction is associated with a global transaction
 *		identifier (GID). The client assigns a GID to a postgres
 *		transaction with the PREPARE TRANSACTION command.
 *
 *		digres refuses PREPARE TRANSACTION: a prepared transaction's
 *state was kept in WAL, and there is none.  What is left is the shared memory
 *array of global transactions, which stays empty, and the lookups that other
 *modules make in it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* GUC variable, can't be changed after startup */
thread_local /*LOCAL*/ int max_prepared_xacts = 0;

/*
 * This struct describes one global transaction that is in prepared state
 * or attempting to become prepared.
 *
 * The lifecycle of a global transaction is:
 *
 * 1. After checking that the requested GID is not in use, set up an entry in
 * the TwoPhaseState->prepXacts array with the correct GID and valid = false,
 * and mark it as locked by my backend.
 *
 * 2. After successfully completing prepare, set valid = true and enter the
 * referenced PGPROC into the global ProcArray.
 *
 * 3. To begin COMMIT PREPARED or ROLLBACK PREPARED, check that the entry is
 * valid and not locked, then mark the entry as locked by storing my current
 * proc number into locking_backend.  This prevents concurrent attempts to
 * commit or rollback the same prepared xact.
 *
 * 4. On completion of COMMIT PREPARED or ROLLBACK PREPARED, remove the entry
 * from the ProcArray and the TwoPhaseState->prepXacts array and return it to
 * the freelist.
 *
 * Note that if the preparing transaction fails between steps 1 and 2, the
 * entry must be removed so that the GID and the GlobalTransaction struct
 * can be reused.  See AtAbort_Twophase().
 *
 * typedef struct GlobalTransactionData *GlobalTransaction appears in
 * twophase.h
 */

typedef struct GlobalTransactionData {
  GlobalTransaction next;  /* list link for free list */
  int pgprocno;            /* ID of associated dummy PGPROC */
  TimestampTz prepared_at; /* time of preparation */

  /*
   * Note that we need to keep track of two LSNs for each GXACT. We keep
   * track of the start LSN because this is the address we must use to read
   * state data back from WAL when committing a prepared GXACT. We keep
   * track of the end LSN because that is the LSN we need to wait for prior
   * to commit.
   */
  XLogRecPtr prepare_start_lsn; /* XLOG offset of prepare record start */
  XLogRecPtr prepare_end_lsn;   /* XLOG offset of prepare record end */
  TransactionId xid;            /* The GXACT id */

  Oid owner;                  /* ID of user that executed the xact */
  ProcNumber locking_backend; /* backend currently working on the xact */
  bool valid;                 /* true if PGPROC entry is in proc array */
  bool ondisk;                /* true if prepare state file is on disk */
  bool inredo;                /* true if entry was added via xlog_redo */
  char gid[GIDSIZE];          /* The GID assigned to the prepared xact */
} GlobalTransactionData;

/*
 * Two Phase Commit shared state.  Access to this struct is protected
 * by TwoPhaseStateLock.
 */
typedef struct TwoPhaseStateData {
  /* Head of linked list of free GlobalTransactionData structs */
  GlobalTransaction freeGXacts;

  /* Number of valid prepXacts entries. */
  int numPrepXacts;

  /* There are max_prepared_xacts items in this array */
  GlobalTransaction prepXacts[FLEXIBLE_ARRAY_MEMBER];
} TwoPhaseStateData;

static thread_local /*SHARED*/ TwoPhaseStateData *TwoPhaseState;

/*
 * Global transaction entry currently locked by us, if any.  Note that any
 * access to the entry pointed to by this variable must be protected by
 * TwoPhaseStateLock, though obviously the pointer itself doesn't need to be
 * (since it's just local memory).
 */
static thread_local /*LOCAL*/ GlobalTransaction MyLockedGxact = NULL;

static void RemoveGXact(GlobalTransaction gxact);

/*
 * Initialization of shared memory
 */
Size TwoPhaseShmemSize(void) {
  Size size;

  /* Need the fixed struct, the array of pointers, and the GTD structs */
  size = offsetof(TwoPhaseStateData, prepXacts);
  size =
      add_size(size, mul_size(max_prepared_xacts, sizeof(GlobalTransaction)));
  size = MAXALIGN(size);
  size = add_size(size,
                  mul_size(max_prepared_xacts, sizeof(GlobalTransactionData)));

  return size;
}

void TwoPhaseShmemInit(void) {
  bool found;

  TwoPhaseState = (TwoPhaseStateData *)ShmemInitStruct(
      "Prepared Transaction Table", TwoPhaseShmemSize(), &found);
  if (!IsUnderPostmaster) {
    GlobalTransaction gxacts;
    int i;

    Assert(!found);
    TwoPhaseState->freeGXacts = NULL;
    TwoPhaseState->numPrepXacts = 0;

    /*
     * Initialize the linked list of free GlobalTransactionData structs
     */
    gxacts =
        (GlobalTransaction)((char *)TwoPhaseState +
                            MAXALIGN(offsetof(TwoPhaseStateData, prepXacts) +
                                     sizeof(GlobalTransaction) *
                                         max_prepared_xacts));
    for (i = 0; i < max_prepared_xacts; i++) {
      /* insert into linked list */
      gxacts[i].next = TwoPhaseState->freeGXacts;
      TwoPhaseState->freeGXacts = &gxacts[i];

      /* associate it with a PGPROC assigned by InitProcGlobal */
      gxacts[i].pgprocno = GetNumberFromPGProc(&PreparedXactProcs[i]);
    }
  } else
    Assert(found);
}

/*
 * Abort hook to unlock the global transaction entry we're working on.
 */
void AtAbort_Twophase(void) {
  if (MyLockedGxact == NULL)
    return;

  /*
   * What to do with the locked global transaction entry?  If we were in the
   * process of preparing the transaction, but haven't written the WAL
   * record and state file yet, the transaction must not be considered as
   * prepared.  Likewise, if we are in the process of finishing an
   * already-prepared transaction, and fail after having already written the
   * 2nd phase commit or rollback record to the WAL, the transaction should
   * not be considered as prepared anymore.  In those cases, just remove the
   * entry from shared memory.
   *
   * Otherwise, the entry must be left in place so that the transaction can
   * be finished later, so just unlock it.
   *
   * If we abort during prepare, after having written the WAL record, we
   * might not have transferred all locks and other state to the prepared
   * transaction yet.  Likewise, if we abort during commit or rollback,
   * after having written the WAL record, we might not have released all the
   * resources held by the transaction yet.  In those cases, the in-memory
   * state can be wrong, but it's too late to back out.
   */
  LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
  if (!MyLockedGxact->valid)
    RemoveGXact(MyLockedGxact);
  else
    MyLockedGxact->locking_backend = INVALID_PROC_NUMBER;
  LWLockRelease(TwoPhaseStateLock);

  MyLockedGxact = NULL;
}

/*
 * RemoveGXact
 *		Remove the prepared transaction from the shared memory array.
 *
 * NB: caller should have already removed it from ProcArray
 */
static void RemoveGXact(GlobalTransaction gxact) {
  int i;

  Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

  for (i = 0; i < TwoPhaseState->numPrepXacts; i++) {
    if (gxact == TwoPhaseState->prepXacts[i]) {
      /* remove from the active array */
      TwoPhaseState->numPrepXacts--;
      TwoPhaseState->prepXacts[i] =
          TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts];

      /* and put it back in the freelist */
      gxact->next = TwoPhaseState->freeGXacts;
      TwoPhaseState->freeGXacts = gxact;

      return;
    }
  }

  elog(ERROR, "failed to find %p in GlobalTransaction array", gxact);
}

/*
 * Returns an array of all prepared transactions for the user-level
 * function pg_prepared_xact.
 *
 * The returned array and all its elements are copies of internal data
 * structures, to minimize the time we need to hold the TwoPhaseStateLock.
 *
 * WARNING -- we return even those transactions that are not fully prepared
 * yet.  The caller should filter them out if he doesn't want them.
 *
 * The returned array is palloc'd.
 */
static int GetPreparedTransactionList(GlobalTransaction *gxacts) {
  GlobalTransaction array;
  int num;
  int i;

  LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

  if (TwoPhaseState->numPrepXacts == 0) {
    LWLockRelease(TwoPhaseStateLock);

    *gxacts = NULL;
    return 0;
  }

  num = TwoPhaseState->numPrepXacts;
  array = (GlobalTransaction)palloc(sizeof(GlobalTransactionData) * num);
  *gxacts = array;
  for (i = 0; i < num; i++)
    memcpy(array + i, TwoPhaseState->prepXacts[i],
           sizeof(GlobalTransactionData));

  LWLockRelease(TwoPhaseStateLock);

  return num;
}

/* Working status for pg_prepared_xact */
typedef struct {
  GlobalTransaction array;
  int ngxacts;
  int currIdx;
} Working_State;

/*
 * pg_prepared_xact
 *		Produce a view with one row per prepared transaction.
 *
 * This function is here so we don't have to export the
 * GlobalTransactionData struct definition.
 */
Datum pg_prepared_xact(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;
  Working_State *status;

  if (SRF_IS_FIRSTCALL()) {
    TupleDesc tupdesc;
    MemoryContext oldcontext;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /*
     * Switch to memory context appropriate for multiple function calls
     */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* build tupdesc for result tuples */
    /* this had better match pg_prepared_xacts view in system_views.sql */
    tupdesc = CreateTemplateTupleDesc(5);
    TupleDescInitEntry(tupdesc, (AttrNumber)1, "transaction", XIDOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)2, "gid", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)3, "prepared", TIMESTAMPTZOID, -1,
                       0);
    TupleDescInitEntry(tupdesc, (AttrNumber)4, "ownerid", OIDOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)5, "dbid", OIDOID, -1, 0);

    funcctx->tuple_desc = BlessTupleDesc(tupdesc);

    /*
     * Collect all the 2PC status information that we will format and send
     * out as a result set.
     */
    status = (Working_State *)palloc(sizeof(Working_State));
    funcctx->user_fctx = status;

    status->ngxacts = GetPreparedTransactionList(&status->array);
    status->currIdx = 0;

    MemoryContextSwitchTo(oldcontext);
  }

  funcctx = SRF_PERCALL_SETUP();
  status = (Working_State *)funcctx->user_fctx;

  while (status->array != NULL && status->currIdx < status->ngxacts) {
    GlobalTransaction gxact = &status->array[status->currIdx++];
    PGPROC *proc = GetPGProcByNumber(gxact->pgprocno);
    Datum values[5] = {0};
    bool nulls[5] = {0};
    HeapTuple tuple;
    Datum result;

    if (!gxact->valid)
      continue;

    /*
     * Form tuple with appropriate data.
     */

    values[0] = TransactionIdGetDatum(proc->xid);
    values[1] = CStringGetTextDatum(gxact->gid);
    values[2] = TimestampTzGetDatum(gxact->prepared_at);
    values[3] = ObjectIdGetDatum(gxact->owner);
    values[4] = ObjectIdGetDatum(proc->databaseId);

    tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
    result = HeapTupleGetDatum(tuple);
    SRF_RETURN_NEXT(funcctx, result);
  }

  SRF_RETURN_DONE(funcctx);
}

/*
 * TwoPhaseGetXidByVirtualXID
 *		Lookup VXID among xacts prepared since last startup.
 *
 * (This won't find recovered xacts.)  If more than one matches, return any
 * and set "have_more" to true.  To witness multiple matches, a single
 * proc number must consume 2^32 LXIDs, with no intervening database restart.
 */
TransactionId TwoPhaseGetXidByVirtualXID(VirtualTransactionId vxid,
                                         bool *have_more) {
  int i;
  TransactionId result = InvalidTransactionId;

  Assert(VirtualTransactionIdIsValid(vxid));
  LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

  for (i = 0; i < TwoPhaseState->numPrepXacts; i++) {
    GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
    PGPROC *proc;
    VirtualTransactionId proc_vxid;

    if (!gxact->valid)
      continue;
    proc = GetPGProcByNumber(gxact->pgprocno);
    GET_VXID_FROM_PGPROC(proc_vxid, *proc);
    if (VirtualTransactionIdEquals(vxid, proc_vxid)) {
      /*
       * Startup process sets proc->vxid.procNumber to
       * INVALID_PROC_NUMBER.
       */
      Assert(!gxact->inredo);

      if (result != InvalidTransactionId) {
        *have_more = true;
        break;
      }
      result = gxact->xid;
    }
  }

  LWLockRelease(TwoPhaseStateLock);

  return result;
}

/*
 * FinishPreparedTransaction: execute COMMIT PREPARED or ROLLBACK PREPARED.
 * Nothing can be prepared (see PrepareTransactionBlock()), so there is never
 * a transaction to finish.
 */
void FinishPreparedTransaction(const char *gid, bool isCommit) {
  ereport(ERROR,
          (errcode(ERRCODE_UNDEFINED_OBJECT),
           errmsg("prepared transaction with identifier \"%s\" does not exist",
                  gid)));
}

/*
 * TwoPhaseTransactionGid
 *		Form the prepared transaction GID for two_phase transactions.
 *
 * Return the GID in the supplied buffer.
 */
void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid_res,
                            int szgid) {
  Assert(OidIsValid(subid));

  if (!TransactionIdIsValid(xid))
    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                    errmsg_internal("invalid two-phase transaction ID")));

  snprintf(gid_res, szgid, "pg_gid_%u_%u", subid, xid);
}

/*
 * IsTwoPhaseTransactionGidForSubid
 *		Check whether the given GID (as formed by
 *TwoPhaseTransactionGid) is for the specified 'subid'.
 */
static bool IsTwoPhaseTransactionGidForSubid(Oid subid, char *gid) {
  int ret;
  Oid subid_from_gid;
  TransactionId xid_from_gid;
  char gid_tmp[GIDSIZE];

  /* Extract the subid and xid from the given GID */
  ret = sscanf(gid, "pg_gid_%u_%u", &subid_from_gid, &xid_from_gid);

  /*
   * Check that the given GID has expected format, and at least the subid
   * matches.
   */
  if (ret != 2 || subid != subid_from_gid)
    return false;

  /*
   * Reconstruct a temporary GID based on the subid and xid extracted from
   * the given GID and check whether the temporary GID and the given GID
   * match.
   */
  TwoPhaseTransactionGid(subid, xid_from_gid, gid_tmp, sizeof(gid_tmp));

  return strcmp(gid, gid_tmp) == 0;
}

/*
 * LookupGXactBySubid
 *		Check if the prepared transaction done by apply worker exists.
 */
bool LookupGXactBySubid(Oid subid) {
  bool found = false;

  LWLockAcquire(TwoPhaseStateLock, LW_SHARED);
  for (int i = 0; i < TwoPhaseState->numPrepXacts; i++) {
    GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

    /* Ignore not-yet-valid GIDs. */
    if (gxact->valid && IsTwoPhaseTransactionGidForSubid(subid, gxact->gid)) {
      found = true;
      break;
    }
  }
  LWLockRelease(TwoPhaseStateLock);

  return found;
}
