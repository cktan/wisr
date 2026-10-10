/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/varsup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/commit_ts.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"

/* pointer to variables struct in shared memory */
thread_local /*SHARED*/ TransamVariablesData *TransamVariables = NULL;

/*
 * Initialization of shared memory for TransamVariables.  The counters get
 * their first values here, as a bootstrap would: there is no WAL to recover
 * them from.  PostgreSQL's transactions begin afresh each time the server
 * starts, and write nothing: sm's are to replace them.
 */
Size VarsupShmemSize(void) { return sizeof(TransamVariablesData); }

void VarsupShmemInit(void) {
  bool found;

  /* Initialize our shared state struct */
  TransamVariables = (TransamVariablesData *)ShmemInitStruct(
      "TransamVariables", sizeof(TransamVariablesData), &found);
  if (!IsUnderPostmaster) {
    Assert(!found);
    memset(TransamVariables, 0, sizeof(TransamVariablesData));
    TransamVariables->nextXid = FirstNormalFullTransactionId;
    TransamVariables->nextOid = FirstNormalObjectId;
    TransamVariables->latestCompletedXid = TransamVariables->nextXid;
    FullTransactionIdRetreat(&TransamVariables->latestCompletedXid);
    SetTransactionIdLimit(FirstNormalTransactionId, InvalidOid);
  } else
    Assert(found);
}

/*
 * Allocate the next FullTransactionId for a new transaction or
 * subtransaction.
 *
 * The new XID is also stored into MyProc->xid/ProcGlobal->xids[] before
 * returning.
 *
 * Note: when this is called, we are actually already inside a valid
 * transaction, since XIDs are now not allocated until the transaction
 * does something.  So it is safe to do a database lookup if we want to
 * issue a warning about XID wrap.
 */
FullTransactionId GetNewTransactionId(bool isSubXact) {
  FullTransactionId full_xid;
  TransactionId xid;

  /*
   * During bootstrap initialization, we return the special bootstrap
   * transaction id.
   */
  if (IsBootstrapProcessingMode()) {
    Assert(!isSubXact);
    MyProc->xid = BootstrapTransactionId;
    ProcGlobal->xids[MyProc->pgxactoff] = BootstrapTransactionId;
    return FullTransactionIdFromEpochAndXid(0, BootstrapTransactionId);
  }

  LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

  full_xid = TransamVariables->nextXid;
  xid = XidFromFullTransactionId(full_xid);

  /*----------
   * Check to see if it's safe to assign another XID.  This protects against
   * catastrophic data loss due to XID wraparound.  The basic rules are:
   *
   * If we're past xidStopLimit, refuse to execute transactions, unless
   * we are running in single-user mode (which gives an escape hatch
   * to the DBA who somehow got past the earlier defenses).
   *
   * Note that this coding also appears in GetNewMultiXactId.
   *----------
   */
  if (TransactionIdFollowsOrEquals(xid, TransamVariables->xidStopLimit)) {
    /*
     * For safety's sake, we release XidGenLock while reporting the
     * error.  This is not so much because we care about
     * preserving concurrency in this situation, as to avoid any
     * possibility of deadlock while doing get_database_name(). First,
     * copy all the shared values we'll need in this path.
     */
    TransactionId xidStopLimit = TransamVariables->xidStopLimit;
    Oid oldest_datoid = TransamVariables->oldestXidDB;

    LWLockRelease(XidGenLock);

    if (IsUnderPostmaster && TransactionIdFollowsOrEquals(xid, xidStopLimit)) {
      char *oldest_datname = get_database_name(oldest_datoid);

      /* complain even if that DB has disappeared */
      if (oldest_datname)
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("database is not accepting commands that assign new "
                    "transaction IDs to avoid wraparound data loss in database "
                    "\"%s\"",
                    oldest_datname)));
      else
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("database is not accepting commands that assign new "
                    "transaction IDs to avoid wraparound data loss in database "
                    "with OID %u",
                    oldest_datoid)));
    }

    /* Re-acquire lock and start over */
    LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
    full_xid = TransamVariables->nextXid;
    xid = XidFromFullTransactionId(full_xid);
  }

  /*
   * If we are allocating the first XID of a new page of pg_subtrans or
   * pg_commit_ts, zero out that page before returning. We must do this
   * while holding XidGenLock, else another xact could acquire and commit a
   * later XID before we zero the page.
   */
  ExtendCommitTs(xid);
  ExtendSUBTRANS(xid);

  /*
   * Now advance the nextXid counter.  This must not happen until after we
   * have successfully completed ExtendSUBTRANS() --- if that routine fails,
   * we want the next incoming transaction to try it again.
   */
  FullTransactionIdAdvance(&TransamVariables->nextXid);

  /*
   * We must store the new XID into the shared ProcArray before releasing
   * XidGenLock.  This ensures that every active XID older than
   * latestCompletedXid is present in the ProcArray, which is essential for
   * correct OldestXmin tracking; see src/backend/access/transam/README.
   *
   * Note that readers of ProcGlobal->xids/PGPROC->xid should be careful to
   * fetch the value for each proc only once, rather than assume they can
   * read a value multiple times and get the same answer each time.  Note we
   * are assuming that TransactionId and int fetch/store are atomic.
   *
   * The same comments apply to the subxact xid count and overflow fields.
   *
   * Use of a write barrier prevents dangerous code rearrangement in this
   * function; other backends could otherwise e.g. be examining my subxids
   * info concurrently, and we don't want them to see an invalid
   * intermediate state, such as an incremented nxids before the array entry
   * is filled.
   *
   * Other processes that read nxids should do so before reading xids
   * elements with a pg_read_barrier() in between, so that they can be sure
   * not to read an uninitialized array element; see
   * src/backend/storage/lmgr/README.barrier.
   *
   * If there's no room to fit a subtransaction XID into PGPROC, set the
   * cache-overflowed flag instead.  This forces readers to look in
   * pg_subtrans to map subtransaction XIDs up to top-level XIDs. There is a
   * race-condition window, in that the new XID will not appear as running
   * until its parent link has been placed into pg_subtrans. However, that
   * will happen before anyone could possibly have a reason to inquire about
   * the status of the XID, so it seems OK.  (Snapshots taken during this
   * window *will* include the parent XID, so they will deliver the correct
   * answer later on when someone does have a reason to inquire.)
   */
  if (!isSubXact) {
    Assert(ProcGlobal->subxidStates[MyProc->pgxactoff].count == 0);
    Assert(!ProcGlobal->subxidStates[MyProc->pgxactoff].overflowed);
    Assert(MyProc->subxidStatus.count == 0);
    Assert(!MyProc->subxidStatus.overflowed);

    /* LWLockRelease acts as barrier */
    MyProc->xid = xid;
    ProcGlobal->xids[MyProc->pgxactoff] = xid;
  } else {
    XidCacheStatus *substat = &ProcGlobal->subxidStates[MyProc->pgxactoff];
    int nxids = MyProc->subxidStatus.count;

    Assert(substat->count == MyProc->subxidStatus.count);
    Assert(substat->overflowed == MyProc->subxidStatus.overflowed);

    if (nxids < PGPROC_MAX_CACHED_SUBXIDS) {
      MyProc->subxids.xids[nxids] = xid;
      pg_write_barrier();
      MyProc->subxidStatus.count = substat->count = nxids + 1;
    } else
      MyProc->subxidStatus.overflowed = substat->overflowed = true;
  }

  LWLockRelease(XidGenLock);

  return full_xid;
}

/*
 * Read nextXid but don't allocate it.
 */
FullTransactionId ReadNextFullTransactionId(void) {
  FullTransactionId fullXid;

  LWLockAcquire(XidGenLock, LW_SHARED);
  fullXid = TransamVariables->nextXid;
  LWLockRelease(XidGenLock);

  return fullXid;
}

/*
 * Determine the last safe XID to allocate using the currently oldest
 * datfrozenxid (ie, the oldest XID that might exist in any database
 * of our cluster), and the OID of the (or a) database with that value.
 */
void SetTransactionIdLimit(TransactionId oldest_datfrozenxid,
                           Oid oldest_datoid) {
  TransactionId xidStopLimit;
  TransactionId xidWrapLimit;

  Assert(TransactionIdIsNormal(oldest_datfrozenxid));

  /*
   * The place where we actually get into deep trouble is halfway around
   * from the oldest potentially-existing XID.  (This calculation is
   * probably off by one or two counts, because the special XIDs reduce the
   * size of the loop a little bit.  But we throw in plenty of slop below,
   * so it doesn't matter.)
   */
  xidWrapLimit = oldest_datfrozenxid + (MaxTransactionId >> 1);
  if (xidWrapLimit < FirstNormalTransactionId)
    xidWrapLimit += FirstNormalTransactionId;

  /*
   * We'll refuse to continue assigning XIDs in interactive mode once we get
   * within 3M transactions of data loss.  This leaves lots of room for the
   * DBA to fool around fixing things in a standalone backend, while not
   * being significant compared to total XID space. (VACUUM requires an XID
   * if it truncates at wal_level!=minimal.  "VACUUM (ANALYZE)", which a DBA
   * might do by reflex, assigns an XID.  Hence, we had better be sure
   * there's lots of XIDs left...)  Also, at default BLCKSZ, this leaves two
   * completely-idle segments.  In the event of edge-case bugs involving
   * page or segment arithmetic, idle segments render the bugs unreachable
   * outside of single-user mode.
   */
  xidStopLimit = xidWrapLimit - 3000000;
  if (xidStopLimit < FirstNormalTransactionId)
    xidStopLimit -= FirstNormalTransactionId;

  /* Grab lock for just long enough to set the new limit values */
  LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
  TransamVariables->oldestXid = oldest_datfrozenxid;
  TransamVariables->xidStopLimit = xidStopLimit;
  TransamVariables->xidWrapLimit = xidWrapLimit;
  TransamVariables->oldestXidDB = oldest_datoid;
  LWLockRelease(XidGenLock);

  /* Log the info */
  ereport(
      DEBUG1,
      (errmsg_internal(
          "transaction ID wrap limit is %u, limited by database with OID %u",
          xidWrapLimit, oldest_datoid)));
}

/*
 * GetNewObjectId -- allocate a new OID
 *
 * OIDs are generated by a cluster-wide counter.  Since they are only 32 bits
 * wide, counter wraparound will occur eventually, and therefore it is unwise
 * to assume they are unique unless precautions are taken to make them so.
 * Hence, this routine should generally not be used directly.  The only direct
 * callers should be GetNewOidWithIndex() and GetNewRelFileNumber() in
 * catalog/catalog.c.
 */
Oid GetNewObjectId(void) {
  Oid result;

  LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

  /*
   * Check for wraparound of the OID counter.  We *must* not return 0
   * (InvalidOid), and in normal operation we mustn't return anything below
   * FirstNormalObjectId since that range is reserved for initdb (see
   * IsCatalogRelationOid()).  Note we are relying on unsigned comparison.
   *
   * During initdb, we start the OID generator at FirstGenbkiObjectId, so we
   * only wrap if before that point when in bootstrap or standalone mode.
   * The first time through this routine after normal postmaster start, the
   * counter will be forced up to FirstNormalObjectId.  This mechanism
   * leaves the OIDs between FirstGenbkiObjectId and FirstNormalObjectId
   * available for automatic assignment during initdb, while ensuring they
   * will never conflict with user-assigned OIDs.
   */
  if (TransamVariables->nextOid < ((Oid)FirstNormalObjectId)) {
    if (IsPostmasterEnvironment) {
      /* wraparound, or first post-initdb assignment, in normal mode */
      TransamVariables->nextOid = FirstNormalObjectId;
    } else {
      /* we may be bootstrapping, so don't enforce the full range */
      if (TransamVariables->nextOid < ((Oid)FirstGenbkiObjectId)) {
        /* wraparound in standalone mode (unlikely but possible) */
        TransamVariables->nextOid = FirstNormalObjectId;
      }
    }
  }

  result = TransamVariables->nextOid;

  (TransamVariables->nextOid)++;

  LWLockRelease(OidGenLock);

  return result;
}

/*
 * SetNextObjectId
 *
 * This may only be called during initdb; it advances the OID counter
 * to the specified value.
 */
static void SetNextObjectId(Oid nextOid) {
  /* Safety check, this is only allowable during initdb */
  if (IsPostmasterEnvironment)
    elog(ERROR, "cannot advance OID counter anymore");

  /* Taking the lock is, therefore, just pro forma; but do it anyway */
  LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

  if (TransamVariables->nextOid > nextOid)
    elog(ERROR, "too late to advance OID counter to %u, it is now %u", nextOid,
         TransamVariables->nextOid);

  TransamVariables->nextOid = nextOid;

  LWLockRelease(OidGenLock);
}

/*
 * StopGeneratingPinnedObjectIds
 *
 * This is called once during initdb to force the OID counter up to
 * FirstUnpinnedObjectId.  This supports letting initdb's post-bootstrap
 * processing create some pinned objects early on.  Once it's done doing
 * so, it calls this (via pg_stop_making_pinned_objects()) so that the
 * remaining objects it makes will be considered un-pinned.
 */
void StopGeneratingPinnedObjectIds(void) {
  SetNextObjectId(FirstUnpinnedObjectId);
}

#ifdef USE_ASSERT_CHECKING

/*
 * Assert that xid is between [oldestXid, nextXid], which is the range we
 * expect XIDs coming from tables etc to be in.
 *
 * As TransamVariables->oldestXid could change just after this call without
 * further precautions, and as a wrapped-around xid could again fall within
 * the valid range, this assertion can only detect if something is definitely
 * wrong, but not establish correctness.
 *
 * This intentionally does not expose a return value, to avoid code being
 * introduced that depends on the return value.
 */
void AssertTransactionIdInAllowableRange(TransactionId xid) {
  TransactionId oldest_xid;
  TransactionId next_xid;

  Assert(TransactionIdIsValid(xid));

  /* we may see bootstrap / frozen */
  if (!TransactionIdIsNormal(xid))
    return;

  /*
   * We can't acquire XidGenLock, as this may be called with XidGenLock
   * already held (or with other locks that don't allow XidGenLock to be
   * nested). That's ok for our purposes though, since we already rely on
   * 32bit reads to be atomic. While nextXid is 64 bit, we only look at the
   * lower 32bit, so a skewed read doesn't hurt.
   *
   * There's no increased danger of falling outside [oldest, next] by
   * accessing them without a lock. xid needs to have been created with
   * GetNewTransactionId() in the originating session, and the locks there
   * pair with the memory barrier below.  We do however accept xid to be <=
   * to next_xid, instead of just <, as xid could be from the procarray,
   * before we see the updated nextXid value.
   */
  pg_memory_barrier();
  oldest_xid = TransamVariables->oldestXid;
  next_xid = XidFromFullTransactionId(TransamVariables->nextXid);

  Assert(TransactionIdFollowsOrEquals(xid, oldest_xid) ||
         TransactionIdPrecedesOrEquals(xid, next_xid));
}
#endif
