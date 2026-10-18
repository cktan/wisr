/*-------------------------------------------------------------------------
 *
 * snapmgr.c
 *		PostgreSQL snapshot manager
 *
 * The following functions return an MVCC snapshot that can be used in tuple
 * visibility checks:
 *
 * - GetTransactionSnapshot
 * - GetLatestSnapshot
 * - GetCatalogSnapshot
 * - GetNonHistoricCatalogSnapshot
 *
 * Each of these functions returns a reference to a statically allocated
 * snapshot.  The statically allocated snapshot is subject to change on any
 * snapshot-related function call, and should not be used directly.  Instead,
 * call PushActiveSnapshot() or RegisterSnapshot() to create a longer-lived
 * copy and use that.
 *
 * We keep track of snapshots in two ways: those "registered" by resowner.c,
 * and the "active snapshot" stack.  All snapshots in either of them live in
 * persistent memory.  When a snapshot is no longer in any of these lists
 * (tracked by separate refcounts on each snapshot), its memory can be freed.
 *
 * In addition to the above-mentioned MVCC snapshots, there are some special
 * snapshots like SnapshotSelf, SnapshotAny, and "dirty" snapshots.  They can
 * only be used in limited contexts and cannot be registered or pushed to the
 * active stack.
 *
 * ActiveSnapshot stack
 * --------------------
 *
 * Most visibility checks use the current "active snapshot" returned by
 * GetActiveSnapshot().  When running normal queries, the active snapshot is
 * set when query execution begins based on the transaction isolation level.
 *
 * The active snapshot is tracked in a stack so that the currently active one
 * is at the top of the stack.  It mirrors the process call stack: whenever we
 * recurse or switch context to fetch rows from a different portal for
 * example, the appropriate snapshot is pushed to become the active snapshot,
 * and popped on return.  Once upon a time, ActiveSnapshot was just a global
 * variable that was saved and restored similar to CurrentMemoryContext, but
 * nowadays it's managed as a separate data structure so that we can keep
 * track of which snapshots are in use and reset the session's xmin when
 * there is no active snapshot.
 *
 * However, there are a couple of exceptions where the active snapshot stack
 * does not strictly mirror the call stack:
 *
 * - VACUUM and a few other utility commands manage their own transactions,
 *   which take their own snapshots.  They are called with an active snapshot
 *   set, like most utility commands, but they pop the active snapshot that
 *   was pushed by the caller.  PortalRunUtility knows about the possibility
 *   that the snapshot it pushed is no longer active on return.
 *
 * - When COMMIT or ROLLBACK is executed within a procedure or DO-block, the
 *   active snapshot stack is destroyed, and re-established later when
 *   subsequent statements in the procedure are executed.  There are many
 *   limitations on when in-procedure COMMIT/ROLLBACK is allowed; one such
 *   limitation is that all the snapshots on the active snapshot stack are
 *   known to portals that are being executed, which makes it safe to reset
 *   the stack.  See EnsurePortalSnapshotExists().
 *
 * Registered snapshots
 * --------------------
 *
 * In addition to snapshots pushed to the active snapshot stack, a snapshot
 * can be registered with a resource owner.
 *
 * The FirstXactSnapshot, if any, is treated a bit specially: we increment its
 * regd_count and list it in RegisteredSnapshots, but this reference is not
 * tracked by a resource owner. We used to use the TopTransactionResourceOwner
 * to track this snapshot reference, but that introduces logical circularity
 * and thus makes it impossible to clean up in a sane fashion.  It's better to
 * handle this reference as an internally-tracked registration, so that this
 * module is entirely lower-level than ResourceOwners.
 *
 * Likewise, the CatalogSnapshot is listed in RegisteredSnapshots when it
 * is valid, but is not tracked by any resource owner.
 *
 * The same is true for historic snapshots used during logical decoding,
 * their lifetime is managed separately (as they live longer than one xact.c
 * transaction).
 *
 * These arrangements let us reset the session's xmin when there are no
 * snapshots referenced by this transaction, and advance it when the one with
 * oldest Xmin is no longer referenced.  For simplicity however, only registered
 * snapshots not active snapshots participate in tracking which one is oldest;
 * we don't try to change the session's xmin except when the active-snapshot
 * stack is empty.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/snapmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "datatype/timestamp.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * CurrentSnapshot points to the only snapshot taken in transaction-snapshot
 * mode, and to the latest one taken in a read-committed transaction.
 * SecondarySnapshot is a snapshot that's always up-to-date as of the current
 * instant, even in transaction-snapshot mode.  It should only be used for
 * special-purpose code (say, RI checking.)  CatalogSnapshot points to an
 * MVCC snapshot intended to be used for catalog scans; we must invalidate it
 * whenever a system catalog change occurs.
 *
 * These SnapshotData structs are static to simplify memory allocation
 * (see the hack in GetSnapshotData to avoid repeated malloc/free).
 */
static thread_local /*LOCAL*/ SnapshotData CurrentSnapshotData = {
    SNAPSHOT_MVCC};
static thread_local /*LOCAL*/ SnapshotData SecondarySnapshotData = {
    SNAPSHOT_MVCC};
static thread_local /*LOCAL*/ SnapshotData CatalogSnapshotData = {
    SNAPSHOT_MVCC};
thread_local /*LOCAL*/ SnapshotData SnapshotSelfData = {SNAPSHOT_SELF};
thread_local /*LOCAL*/ SnapshotData SnapshotAnyData = {SNAPSHOT_ANY};
thread_local /*LOCAL*/ SnapshotData SnapshotToastData = {SNAPSHOT_TOAST};

/* Pointers to valid snapshots */
static thread_local /*LOCAL*/ Snapshot CurrentSnapshot = NULL;
static thread_local /*LOCAL*/ Snapshot SecondarySnapshot = NULL;
static thread_local /*LOCAL*/ Snapshot CatalogSnapshot = NULL;

/*
 * Elements of the active snapshot stack.
 *
 * Each element here accounts for exactly one active_count on SnapshotData.
 *
 * NB: the code assumes that elements in this list are in non-increasing
 * order of as_level; also, the list must be NULL-terminated.
 */
typedef struct ActiveSnapshotElt {
  Snapshot as_snap;
  int as_level;
  struct ActiveSnapshotElt *as_next;
} ActiveSnapshotElt;

/* Top of the stack of active snapshots */
static thread_local /*LOCAL*/ ActiveSnapshotElt *ActiveSnapshot = NULL;

/*
 * Currently registered Snapshots.  Ordered in a heap by xmin, so that we can
 * quickly find the one with lowest xmin, to advance the session's xmin.
 */
static int xmin_cmp(const pairingheap_node *a, const pairingheap_node *b,
                    void *arg);

static thread_local /*LOCAL*/ pairingheap RegisteredSnapshots = {&xmin_cmp,
                                                                 NULL, NULL};

/* first GetTransactionSnapshot call in a transaction? */
thread_local /*LOCAL*/ bool FirstSnapshotSet = false;

/*
 * Remember the serializable transaction snapshot, if any.  We cannot trust
 * FirstSnapshotSet in combination with IsolationUsesXactSnapshot(), because
 * GUC may be reset before us, changing the value of IsolationUsesXactSnapshot.
 */
static thread_local /*LOCAL*/ Snapshot FirstXactSnapshot = NULL;

/* Prototypes for local functions */
static Snapshot CopySnapshot(Snapshot snapshot);
static void UnregisterSnapshotNoOwner(Snapshot snapshot);
static void FreeSnapshot(Snapshot snapshot);
static Snapshot GetSnapshotData(Snapshot snapshot);

/* ResourceOwner callbacks to track snapshot references */
static void ResOwnerReleaseSnapshot(Datum res);

static const ResourceOwnerDesc snapshot_resowner_desc = {
    .name = "snapshot reference",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = RELEASE_PRIO_SNAPSHOT_REFS,
    .ReleaseResource = ResOwnerReleaseSnapshot,
    .DebugPrint = NULL /* the default message is fine */
};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void ResourceOwnerRememberSnapshot(ResourceOwner owner,
                                                 Snapshot snap) {
  ResourceOwnerRemember(owner, PointerGetDatum(snap), &snapshot_resowner_desc);
}
static inline void ResourceOwnerForgetSnapshot(ResourceOwner owner,
                                               Snapshot snap) {
  ResourceOwnerForget(owner, PointerGetDatum(snap), &snapshot_resowner_desc);
}

/*
 * GetTransactionSnapshot
 *		Get the appropriate snapshot for a new query in a transaction.
 *
 * Note that the return value points at static storage that will be modified
 * by future calls and by CommandCounterIncrement().  Callers must call
 * RegisterSnapshot or PushActiveSnapshot on the returned snap before doing
 * any other non-trivial work that could invalidate it.
 */
Snapshot GetTransactionSnapshot(void) {
  /* First call in transaction? */
  if (!FirstSnapshotSet) {
    /*
     * Don't allow catalog snapshot to be older than xact snapshot.  Must
     * do this first to allow the empty-heap Assert to succeed.
     */
    InvalidateCatalogSnapshot();

    Assert(pairingheap_is_empty(&RegisteredSnapshots));
    Assert(FirstXactSnapshot == NULL);

    /*
     * In transaction-snapshot mode, the first snapshot must live until
     * end of xact regardless of what the caller does with it, so we must
     * make a copy of it rather than returning CurrentSnapshotData
     * directly.  Furthermore, if we're running in serializable mode,
     * predicate.c needs to wrap the snapshot fetch in its own processing.
     */
    if (IsolationUsesXactSnapshot()) {
      /* First, create the snapshot in CurrentSnapshotData */
      CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);
      /* Make a saved copy */
      CurrentSnapshot = CopySnapshot(CurrentSnapshot);
      FirstXactSnapshot = CurrentSnapshot;
      /* Mark it as "registered" in FirstXactSnapshot */
      FirstXactSnapshot->regd_count++;
      pairingheap_add(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
    } else
      CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

    FirstSnapshotSet = true;
    return CurrentSnapshot;
  }

  if (IsolationUsesXactSnapshot())
    return CurrentSnapshot;

  /* Don't allow catalog snapshot to be older than xact snapshot. */
  InvalidateCatalogSnapshot();

  CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

  return CurrentSnapshot;
}

/*
 * GetLatestSnapshot
 *		Get a snapshot that is up-to-date as of the current instant,
 *		even if we are executing in transaction-snapshot mode.
 */
Snapshot GetLatestSnapshot(void) {
  /* If first call in transaction, go ahead and set the xact snapshot */
  if (!FirstSnapshotSet)
    return GetTransactionSnapshot();

  SecondarySnapshot = GetSnapshotData(&SecondarySnapshotData);

  return SecondarySnapshot;
}

/*
 * GetCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the
 *		system catalog with the specified OID.
 */
Snapshot GetCatalogSnapshot(Oid relid) {
  return GetNonHistoricCatalogSnapshot(relid);
}

/*
 * GetNonHistoricCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the
 *system catalog with the specified OID, even while historic snapshots are set
 *		up.
 */
Snapshot GetNonHistoricCatalogSnapshot(Oid relid) {
  /*
   * If the caller is trying to scan a relation that has no syscache, no
   * catcache invalidations will be sent when it is updated.  For a few key
   * relations, snapshot invalidations are sent instead.  If we're trying to
   * scan a relation for which neither catcache nor snapshot invalidations
   * are sent, we must refresh the snapshot every time.
   */
  if (CatalogSnapshot && !RelationInvalidatesSnapshotsOnly(relid) &&
      !RelationHasSysCache(relid))
    InvalidateCatalogSnapshot();

  if (CatalogSnapshot == NULL) {
    /* Get new snapshot. */
    CatalogSnapshot = GetSnapshotData(&CatalogSnapshotData);

    /*
     * Make sure the catalog snapshot will be accounted for in decisions
     * about advancing the session's xmin.  We could apply RegisterSnapshot, but
     * that would result in making a physical copy, which is overkill; and
     * it would also create a dependency on some resource owner, which we
     * do not want for reasons explained at the head of this file. Instead
     * just shove the CatalogSnapshot into the pairing heap manually. This
     * has to be reversed in InvalidateCatalogSnapshot, of course.
     *
     * NB: it had better be impossible for this to throw error, since the
     * CatalogSnapshot pointer is already valid.
     */
    pairingheap_add(&RegisteredSnapshots, &CatalogSnapshot->ph_node);
  }

  return CatalogSnapshot;
}

/*
 * InvalidateCatalogSnapshot
 *		Mark the current catalog snapshot, if any, as invalid
 *
 * We could change this API to allow the caller to provide more fine-grained
 * invalidation details, so that a change to relation A wouldn't prevent us
 * from using our cached snapshot to scan relation B, but so far there's no
 * evidence that the CPU cycles we spent tracking such fine details would be
 * well-spent.
 */
void InvalidateCatalogSnapshot(void) {
  if (CatalogSnapshot) {
    pairingheap_remove(&RegisteredSnapshots, &CatalogSnapshot->ph_node);
    CatalogSnapshot = NULL;
  }
}

/*
 * InvalidateCatalogSnapshotConditionally
 *		Drop catalog snapshot if it's the only one we have
 *
 * This is called when we are about to wait for client input, so we don't
 * want to continue holding the catalog snapshot if it might mean that the
 * global xmin horizon can't advance.  However, if there are other snapshots
 * still active or registered, the catalog snapshot isn't likely to be the
 * oldest one, so we might as well keep it.
 */
void InvalidateCatalogSnapshotConditionally(void) {
  if (CatalogSnapshot && ActiveSnapshot == NULL &&
      pairingheap_is_singular(&RegisteredSnapshots))
    InvalidateCatalogSnapshot();
}

/*
 * SnapshotSetCommandId
 *		Propagate CommandCounterIncrement into the static snapshots, if
 *set
 */
void SnapshotSetCommandId(CommandId curcid) {
  if (!FirstSnapshotSet)
    return;

  if (CurrentSnapshot)
    CurrentSnapshot->curcid = curcid;
  if (SecondarySnapshot)
    SecondarySnapshot->curcid = curcid;
  /* Should we do the same with CatalogSnapshot? */
}

/*
 * CopySnapshot
 *		Copy the given snapshot.
 *
 * The copy is palloc'd in TopTransactionContext and has initial refcounts set
 * to 0.  The returned snapshot has the copied flag set.
 */
static Snapshot CopySnapshot(Snapshot snapshot) {
  Snapshot newsnap;
  Size subxipoff;
  Size size;

  Assert(snapshot != InvalidSnapshot);

  /* We allocate any XID arrays needed in the same palloc block. */
  size = subxipoff =
      sizeof(SnapshotData) + snapshot->xcnt * sizeof(TransactionId);
  if (snapshot->subxcnt > 0)
    size += snapshot->subxcnt * sizeof(TransactionId);

  newsnap = (Snapshot)MemoryContextAlloc(TopTransactionContext, size);
  memcpy(newsnap, snapshot, sizeof(SnapshotData));

  newsnap->regd_count = 0;
  newsnap->active_count = 0;
  newsnap->copied = true;
  newsnap->snapXactCompletionCount = 0;

  /* setup XID array */
  if (snapshot->xcnt > 0) {
    newsnap->xip = (TransactionId *)(newsnap + 1);
    memcpy(newsnap->xip, snapshot->xip, snapshot->xcnt * sizeof(TransactionId));
  } else
    newsnap->xip = NULL;

  /*
   * Setup subXID array. Don't bother to copy it if it had overflowed,
   * though, because it's not used anywhere in that case.
   */
  if (snapshot->subxcnt > 0 && !snapshot->suboverflowed) {
    newsnap->subxip = (TransactionId *)((char *)newsnap + subxipoff);
    memcpy(newsnap->subxip, snapshot->subxip,
           snapshot->subxcnt * sizeof(TransactionId));
  } else
    newsnap->subxip = NULL;

  return newsnap;
}

/*
 * FreeSnapshot
 *		Free the memory associated with a snapshot.
 */
static void FreeSnapshot(Snapshot snapshot) {
  Assert(snapshot->regd_count == 0);
  Assert(snapshot->active_count == 0);
  Assert(snapshot->copied);

  pfree(snapshot);
}

/*
 * PushActiveSnapshot
 *		Set the given snapshot as the current active snapshot
 *
 * If the passed snapshot is a statically-allocated one, or it is possibly
 * subject to a future command counter update, create a new long-lived copy
 * with active refcount=1.  Otherwise, only increment the refcount.
 */
void PushActiveSnapshot(Snapshot snapshot) {
  PushActiveSnapshotWithLevel(snapshot, GetCurrentTransactionNestLevel());
}

/*
 * PushActiveSnapshotWithLevel
 *		Set the given snapshot as the current active snapshot
 *
 * Same as PushActiveSnapshot except that caller can specify the
 * transaction nesting level that "owns" the snapshot.  This level
 * must not be deeper than the current top of the snapshot stack.
 */
void PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level) {
  ActiveSnapshotElt *newactive;

  Assert(snapshot != InvalidSnapshot);
  Assert(ActiveSnapshot == NULL || snap_level >= ActiveSnapshot->as_level);

  newactive = (ActiveSnapshotElt *)MemoryContextAlloc(
      TopTransactionContext, sizeof(ActiveSnapshotElt));

  /*
   * Checking SecondarySnapshot is probably useless here, but it seems
   * better to be sure.
   */
  if (snapshot == CurrentSnapshot || snapshot == SecondarySnapshot ||
      !snapshot->copied)
    newactive->as_snap = CopySnapshot(snapshot);
  else
    newactive->as_snap = snapshot;

  newactive->as_next = ActiveSnapshot;
  newactive->as_level = snap_level;

  newactive->as_snap->active_count++;

  ActiveSnapshot = newactive;
}

/*
 * PushCopiedSnapshot
 *		As above, except forcibly copy the presented snapshot.
 *
 * This should be used when the ActiveSnapshot has to be modifiable, for
 * example if the caller intends to call UpdateActiveSnapshotCommandId.
 * The new snapshot will be released when popped from the stack.
 */
void PushCopiedSnapshot(Snapshot snapshot) {
  PushActiveSnapshot(CopySnapshot(snapshot));
}

/*
 * UpdateActiveSnapshotCommandId
 *
 * Update the current CID of the active snapshot.  This can only be applied
 * to a snapshot that is not referenced elsewhere.
 */
void UpdateActiveSnapshotCommandId(void) {
  CommandId curcid;

  Assert(ActiveSnapshot != NULL);
  Assert(ActiveSnapshot->as_snap->active_count == 1);
  Assert(ActiveSnapshot->as_snap->regd_count == 0);

  curcid = GetCurrentCommandId(false);
  ActiveSnapshot->as_snap->curcid = curcid;
}

/*
 * PopActiveSnapshot
 *
 * Remove the topmost snapshot from the active snapshot stack, decrementing the
 * reference count, and free it if this was the last reference.
 */
void PopActiveSnapshot(void) {
  ActiveSnapshotElt *newstack;

  newstack = ActiveSnapshot->as_next;

  Assert(ActiveSnapshot->as_snap->active_count > 0);

  ActiveSnapshot->as_snap->active_count--;

  if (ActiveSnapshot->as_snap->active_count == 0 &&
      ActiveSnapshot->as_snap->regd_count == 0)
    FreeSnapshot(ActiveSnapshot->as_snap);

  pfree(ActiveSnapshot);
  ActiveSnapshot = newstack;
}

/*
 * GetActiveSnapshot
 *		Return the topmost snapshot in the Active stack.
 */
Snapshot GetActiveSnapshot(void) {
  Assert(ActiveSnapshot != NULL);

  return ActiveSnapshot->as_snap;
}

/*
 * ActiveSnapshotSet
 *		Return whether there is at least one snapshot in the Active
 *stack
 */
bool ActiveSnapshotSet(void) { return ActiveSnapshot != NULL; }

/*
 * RegisterSnapshot
 *		Register a snapshot as being in use by the current resource
 *owner
 *
 * If InvalidSnapshot is passed, it is not registered.
 */
Snapshot RegisterSnapshot(Snapshot snapshot) {
  if (snapshot == InvalidSnapshot)
    return InvalidSnapshot;

  return RegisterSnapshotOnOwner(snapshot, CurrentResourceOwner);
}

/*
 * RegisterSnapshotOnOwner
 *		As above, but use the specified resource owner
 */
Snapshot RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner) {
  Snapshot snap;

  if (snapshot == InvalidSnapshot)
    return InvalidSnapshot;

  /* Static snapshot?  Create a persistent copy */
  snap = snapshot->copied ? snapshot : CopySnapshot(snapshot);

  /* and tell resowner.c about it */
  ResourceOwnerEnlarge(owner);
  snap->regd_count++;
  ResourceOwnerRememberSnapshot(owner, snap);

  if (snap->regd_count == 1)
    pairingheap_add(&RegisteredSnapshots, &snap->ph_node);

  return snap;
}

/*
 * UnregisterSnapshot
 *
 * Decrement the reference count of a snapshot, remove the corresponding
 * reference from CurrentResourceOwner, and free the snapshot if no more
 * references remain.
 */
void UnregisterSnapshot(Snapshot snapshot) {
  if (snapshot == NULL)
    return;

  UnregisterSnapshotFromOwner(snapshot, CurrentResourceOwner);
}

/*
 * UnregisterSnapshotFromOwner
 *		As above, but use the specified resource owner
 */
void UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner) {
  if (snapshot == NULL)
    return;

  ResourceOwnerForgetSnapshot(owner, snapshot);
  UnregisterSnapshotNoOwner(snapshot);
}

static void UnregisterSnapshotNoOwner(Snapshot snapshot) {
  Assert(snapshot->regd_count > 0);
  Assert(!pairingheap_is_empty(&RegisteredSnapshots));

  snapshot->regd_count--;
  if (snapshot->regd_count == 0)
    pairingheap_remove(&RegisteredSnapshots, &snapshot->ph_node);

  if (snapshot->regd_count == 0 && snapshot->active_count == 0) {
    FreeSnapshot(snapshot);
  }
}

/*
 * Comparison function for RegisteredSnapshots heap.  Snapshots are ordered
 * by xmin, so that the snapshot with smallest xmin is at the top.
 */
static int xmin_cmp(const pairingheap_node *a, const pairingheap_node *b,
                    void *arg) {
  const SnapshotData *asnap =
      pairingheap_const_container(SnapshotData, ph_node, a);
  const SnapshotData *bsnap =
      pairingheap_const_container(SnapshotData, ph_node, b);

  if (TransactionIdPrecedes(asnap->xmin, bsnap->xmin))
    return 1;
  else if (TransactionIdFollows(asnap->xmin, bsnap->xmin))
    return -1;
  else
    return 0;
}

/*
 * AtSubCommit_Snapshot
 */
void AtSubCommit_Snapshot(int level) {
  ActiveSnapshotElt *active;

  /*
   * Relabel the active snapshots set in this subtransaction as though they
   * are owned by the parent subxact.
   */
  for (active = ActiveSnapshot; active != NULL; active = active->as_next) {
    if (active->as_level < level)
      break;
    active->as_level = level - 1;
  }
}

/*
 * AtSubAbort_Snapshot
 *		Clean up snapshots after a subtransaction abort
 */
void AtSubAbort_Snapshot(int level) {
  /* Forget the active snapshots set by this subtransaction */
  while (ActiveSnapshot && ActiveSnapshot->as_level >= level) {
    ActiveSnapshotElt *next;

    next = ActiveSnapshot->as_next;

    /*
     * Decrement the snapshot's active count.  If it's still registered or
     * marked as active by an outer subtransaction, we can't free it yet.
     */
    Assert(ActiveSnapshot->as_snap->active_count >= 1);
    ActiveSnapshot->as_snap->active_count -= 1;

    if (ActiveSnapshot->as_snap->active_count == 0 &&
        ActiveSnapshot->as_snap->regd_count == 0)
      FreeSnapshot(ActiveSnapshot->as_snap);

    /* and free the stack element */
    pfree(ActiveSnapshot);

    ActiveSnapshot = next;
  }
}

/*
 * AtEOXact_Snapshot
 *		Snapshot manager's cleanup function for end of transaction
 */
void AtEOXact_Snapshot(bool isCommit) {
  /*
   * In transaction-snapshot mode we must release our privately-managed
   * reference to the transaction snapshot.  We must remove it from
   * RegisteredSnapshots to keep the check below happy.  But we don't bother
   * to do FreeSnapshot, for two reasons: the memory will go away with
   * TopTransactionContext anyway, and if someone has left the snapshot
   * stacked as active, we don't want the code below to be chasing through a
   * dangling pointer.
   */
  if (FirstXactSnapshot != NULL) {
    Assert(FirstXactSnapshot->regd_count > 0);
    Assert(!pairingheap_is_empty(&RegisteredSnapshots));
    pairingheap_remove(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
  }
  FirstXactSnapshot = NULL;

  /* Drop catalog snapshot if any */
  InvalidateCatalogSnapshot();

  /* On commit, complain about leftover snapshots */
  if (isCommit) {
    ActiveSnapshotElt *active;

    if (!pairingheap_is_empty(&RegisteredSnapshots))
      elog(WARNING, "registered snapshots seem to remain after cleanup");

    /* complain about unpopped active snapshots */
    for (active = ActiveSnapshot; active != NULL; active = active->as_next)
      elog(WARNING, "snapshot %p still active", active);
  }

  /*
   * And reset our state.  We don't need to free the memory explicitly --
   * it'll go away with TopTransactionContext.
   */
  ActiveSnapshot = NULL;
  pairingheap_reset(&RegisteredSnapshots);

  CurrentSnapshot = NULL;
  SecondarySnapshot = NULL;

  FirstSnapshotSet = false;
}

/*
 * pg_export_snapshot
 *		Refused: a session sees sm's snapshot, which none exports.
 */
Datum pg_export_snapshot(PG_FUNCTION_ARGS) {
  /*
   * What a session sees is sm's snapshot, not this one: another session
   * importing it would not see what this one does (WORKNOTE.md step 149).
   */
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("exporting a snapshot is not supported yet")));
  PG_RETURN_NULL(); /* not reached */
}

/*
 * ImportSnapshot
 *		Refuse "SET TRANSACTION SNAPSHOT": no snapshot is exported
 *		(pg_export_snapshot()), so none is imported (WORKNOTE.md step
 *149).
 */
void ImportSnapshot(const char *idstr) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("SET TRANSACTION SNAPSHOT is not supported yet")));
}

/*
 * ThereAreNoPriorRegisteredSnapshots
 *		Is the registered snapshot count less than or equal to one?
 *
 * Don't use this to settle important decisions.  While zero registrations and
 * no ActiveSnapshot would confirm a certain idleness, the system makes no
 * guarantees about the significance of one registered snapshot.
 */
bool ThereAreNoPriorRegisteredSnapshots(void) {
  if (pairingheap_is_empty(&RegisteredSnapshots) ||
      pairingheap_is_singular(&RegisteredSnapshots))
    return true;

  return false;
}

/*
 * HaveRegisteredOrActiveSnapshot
 *		Is there any registered or active snapshot?
 *
 * NB: Unless pushed or active, the cached catalog snapshot will not cause
 * this function to return true. That allows this function to be used in
 * checks enforcing a longer-lived snapshot.
 */
bool HaveRegisteredOrActiveSnapshot(void) {
  if (ActiveSnapshot != NULL)
    return true;

  /*
   * The catalog snapshot is in RegisteredSnapshots when valid, but can be
   * removed at any time due to invalidation processing. If explicitly
   * registered more than one snapshot has to be in RegisteredSnapshots.
   */
  if (CatalogSnapshot != NULL && pairingheap_is_singular(&RegisteredSnapshots))
    return false;

  return !pairingheap_is_empty(&RegisteredSnapshots);
}

/*
 * XidInMVCCSnapshot
 *		Is the given XID still-in-progress according to the snapshot?
 *
 * Note: GetSnapshotData never stores either top xid or subxids of our own
 * backend into a snapshot, so these xids will not be reported as "running"
 * by this function.  This is OK for current uses, because we always check
 * TransactionIdIsCurrentTransactionId first, except when it's known the
 * XID could not be ours anyway.
 */
bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot) {
  /*
   * Make a quick range check to eliminate most XIDs without looking at the
   * xip arrays.  Note that this is OK even if we convert a subxact XID to
   * its parent below, because a subxact with XID < xmin has surely also got
   * a parent with XID < xmin, while one with XID >= xmax must belong to a
   * parent that was not yet committed at the time of this snapshot.
   */

  /* Any xid < xmin is not in-progress */
  if (TransactionIdPrecedes(xid, snapshot->xmin))
    return false;
  /* Any xid >= xmax is in-progress */
  if (TransactionIdFollowsOrEquals(xid, snapshot->xmax))
    return true;

  /*
   * If the snapshot contains full subxact data, the fastest way to
   * check things is just to compare the given XID against both subxact
   * XIDs and top-level XIDs.  If the snapshot overflowed, we have to
   * use pg_subtrans to convert a subxact XID to its parent XID, but
   * then we need only look at top-level XIDs not subxacts.
   */
  if (!snapshot->suboverflowed) {
    /* we have full data, so search subxip */
    if (pg_lfind32(xid, snapshot->subxip, snapshot->subxcnt))
      return true;

    /* not there, fall through to search xip[] */
  } else {
    /*
     * Snapshot overflowed, so convert xid to top-level.  This is safe
     * because we eliminated too-old XIDs above.
     */
    /* digres assigns no subtransaction an xid: xid is its own topmost. */

    /*
     * If xid was indeed a subxact, we might now have an xid < xmin,
     * so recheck to avoid an array scan.  No point in rechecking
     * xmax.
     */
    if (TransactionIdPrecedes(xid, snapshot->xmin))
      return false;
  }

  if (pg_lfind32(xid, snapshot->xip, snapshot->xcnt))
    return true;

  return false;
}

/* ResourceOwner callbacks */

static void ResOwnerReleaseSnapshot(Datum res) {
  UnregisterSnapshotNoOwner((Snapshot)DatumGetPointer(res));
}

/*
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (highest completed xact ID + 1), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *		All xact IDs < xmin are considered finished.
 *		All xact IDs >= xmax are considered still running.
 *		For an xact ID xmin <= xid < xmax, consult list to see whether
 *		it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * In digres no xid is ever running, so the list is empty and every snapshot
 * is the same (see below).
 *
 * No horizon is kept: what one serves, heap pruning and VACUUM, does not
 * run.  A snapshot's xid arrays are not allocated: it holds none.
 */
static Snapshot GetSnapshotData(Snapshot snapshot) {
  TransactionId xmax;

  Assert(snapshot != NULL);

  /*
   * No PostgreSQL transaction id is ever assigned (WORKNOTE.md steps 45 and
   * 137), and what a session sees is sm's snapshot, not this one: so no xid
   * is in progress, and every snapshot is the same, its xmin and xmax the
   * first normal xid, after the last completed, which none ever is.  It is
   * made with no lock and no scan of the proc array (SHMEM.md section 7,
   * step 3).  The command id is the caller's, as before.
   */
  xmax = FirstNormalTransactionId;

  snapshot->xmin = xmax;
  snapshot->xmax = xmax;
  snapshot->xcnt = 0;
  snapshot->subxcnt = 0;
  snapshot->suboverflowed = false;
  /* No transaction with an xid completes: the count stays at 1. */
  snapshot->snapXactCompletionCount = 1;
  snapshot->curcid = GetCurrentCommandId(false);
  snapshot->active_count = 0;
  snapshot->regd_count = 0;
  snapshot->copied = false;
  return snapshot;
}
