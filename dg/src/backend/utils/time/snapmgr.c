/*-------------------------------------------------------------------------
 *
 * snapmgr.c
 *		PostgreSQL snapshot manager, as digres keeps it
 *
 * What a session sees is sm's snapshot, not PostgreSQL's: sm's statement
 * reads at one (access/smstore.h), and digres assigns no transaction id and
 * has no heap.  So every MVCC snapshot is the same, its xmin and xmax the
 * first normal xid and no xid in progress, and this module hands out that
 * one (WORKNOTE.md step 235).
 *
 * What is kept is the bookkeeping callers still ask about:
 *
 * - The active snapshot stack: each push remembers its snapshot and the
 *   transaction nesting level that owns it, so a subtransaction's abort
 *   pops what it pushed (AtSubAbort_Snapshot()), and ActiveSnapshotSet()
 *   tells a query in progress.
 * - Registered snapshots: a count, each registration tracked by a resource
 *   owner, so an abort releases it, ThereAreNoPriorRegisteredSnapshots()
 *   answers COPY FREEZE and HaveRegisteredOrActiveSnapshot() answers TOAST.
 * - FirstSnapshotSet: whether a query of the transaction has run, which SET
 *   TRANSACTION ISOLATION LEVEL checks.
 *
 * No snapshot is copied, freed, or ordered by xmin, and there is no catalog
 * snapshot to invalidate.
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

#include <utility>
#include <vector>

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

/* The one MVCC snapshot, filled by mvcc_snapshot(). */
static thread_local /*LOCAL*/ SnapshotData MVCCSnapshotData = {SNAPSHOT_MVCC};
thread_local /*LOCAL*/ SnapshotData SnapshotSelfData = {SNAPSHOT_SELF};
thread_local /*LOCAL*/ SnapshotData SnapshotAnyData = {SNAPSHOT_ANY};

/*
 * The active snapshot stack, its top last: each snapshot pushed and the
 * transaction nesting level that owns it, levels never decreasing.
 */
static thread_local /*LOCAL*/ std::vector<std::pair<Snapshot, int>>
    active_snapshots;

/* How many registrations are held, each by a resource owner. */
static thread_local /*LOCAL*/ int registered_snapshots = 0;

/* first GetTransactionSnapshot call in a transaction? */
thread_local /*LOCAL*/ bool FirstSnapshotSet = false;

/* ResourceOwner callbacks to track snapshot references */
static void ResOwnerReleaseSnapshot(Datum res);

static const ResourceOwnerDesc snapshot_resowner_desc = {
    .name = "snapshot reference",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = RELEASE_PRIO_SNAPSHOT_REFS,
    .ReleaseResource = ResOwnerReleaseSnapshot,
    .DebugPrint = NULL /* the default message is fine */
};

/*
 * The MVCC snapshot: no xid is in progress, as digres assigns none, and its
 * xmin and xmax are the first normal xid, after the last completed, which
 * none ever is.  Filled at each call, so a caller's change does not last.
 */
static Snapshot mvcc_snapshot(void) {
  Snapshot snapshot = &MVCCSnapshotData;

  snapshot->snapshot_type = SNAPSHOT_MVCC;
  snapshot->xmin = FirstNormalTransactionId;
  snapshot->xmax = FirstNormalTransactionId;
  snapshot->xip = NULL;
  snapshot->xcnt = 0;
  snapshot->subxip = NULL;
  snapshot->subxcnt = 0;
  snapshot->suboverflowed = false;
  return snapshot;
}

/*
 * GetTransactionSnapshot
 * The snapshot for a new query in a transaction: the MVCC one.
 */
Snapshot GetTransactionSnapshot(void) {
  FirstSnapshotSet = true;
  return mvcc_snapshot();
}

/*
 * GetLatestSnapshot
 * A snapshot as of now, the one MVCC snapshot.
 */
Snapshot GetLatestSnapshot(void) { return GetTransactionSnapshot(); }

/*
 * GetCatalogSnapshot
 * A snapshot for a scan of a system catalog: the MVCC one.
 */
Snapshot GetCatalogSnapshot(Oid relid) {
  return GetNonHistoricCatalogSnapshot(relid);
}

/*
 * GetNonHistoricCatalogSnapshot
 * As GetCatalogSnapshot(): digres has no historic snapshot.
 */
Snapshot GetNonHistoricCatalogSnapshot(Oid relid) { return mvcc_snapshot(); }

/* No catalog snapshot is kept, so there is none to invalidate. */
void InvalidateCatalogSnapshot(void) {}

/* As InvalidateCatalogSnapshot(): there is none to drop. */
void InvalidateCatalogSnapshotConditionally(void) {}

/*
 * PushActiveSnapshot
 * Make the given snapshot the active one, owned by the current
 * transaction nesting level.
 */
void PushActiveSnapshot(Snapshot snapshot) {
  PushActiveSnapshotWithLevel(snapshot, GetCurrentTransactionNestLevel());
}

/*
 * PushActiveSnapshotWithLevel
 * As PushActiveSnapshot(), owned by the given nesting level, which
 * must not be deeper than the current top's.
 */
void PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level) {
  Assert(snapshot != InvalidSnapshot);
  Assert(active_snapshots.empty() ||
         snap_level >= active_snapshots.back().second);
  active_snapshots.emplace_back(snapshot, snap_level);
}

/*
 * PushCopiedSnapshot
 * As PushActiveSnapshot(): no snapshot changes, so none is copied.
 */
void PushCopiedSnapshot(Snapshot snapshot) { PushActiveSnapshot(snapshot); }

/* Remove the topmost snapshot from the active snapshot stack. */
void PopActiveSnapshot(void) {
  Assert(!active_snapshots.empty());
  active_snapshots.pop_back();
}

/* The topmost snapshot of the active snapshot stack. */
Snapshot GetActiveSnapshot(void) {
  Assert(!active_snapshots.empty());
  return active_snapshots.back().first;
}

/* Whether there is at least one snapshot on the active snapshot stack. */
bool ActiveSnapshotSet(void) { return !active_snapshots.empty(); }

/*
 * RegisterSnapshot
 * Register a snapshot as being in use by the current resource owner.
 * InvalidSnapshot is not registered.
 */
Snapshot RegisterSnapshot(Snapshot snapshot) {
  return RegisterSnapshotOnOwner(snapshot, CurrentResourceOwner);
}

/*
 * RegisterSnapshotOnOwner
 * As RegisterSnapshot(), by the given resource owner.
 */
Snapshot RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner) {
  if (snapshot == InvalidSnapshot)
    return InvalidSnapshot;
  /* The owner releases the registration if the transaction aborts. */
  ResourceOwnerEnlarge(owner);
  ResourceOwnerRemember(owner, PointerGetDatum(snapshot),
                        &snapshot_resowner_desc);
  registered_snapshots++;
  return snapshot;
}

/* Release a registration the current resource owner holds. */
void UnregisterSnapshot(Snapshot snapshot) {
  UnregisterSnapshotFromOwner(snapshot, CurrentResourceOwner);
}

/* As UnregisterSnapshot(), one the given resource owner holds. */
void UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner) {
  if (snapshot == NULL)
    return;
  ResourceOwnerForget(owner, PointerGetDatum(snapshot),
                      &snapshot_resowner_desc);
  Assert(registered_snapshots > 0);
  registered_snapshots--;
}

/*
 * AtSubCommit_Snapshot
 * Hand the active snapshots a committed subtransaction pushed to its
 * parent.
 */
void AtSubCommit_Snapshot(int level) {
  for (auto it = active_snapshots.rbegin();
       it != active_snapshots.rend() && it->second >= level; ++it)
    it->second = level - 1;
}

/*
 * AtSubAbort_Snapshot
 * Pop the active snapshots an aborted subtransaction pushed; its
 * resource owners release its registrations.
 */
void AtSubAbort_Snapshot(int level) {
  while (!active_snapshots.empty() && active_snapshots.back().second >= level)
    active_snapshots.pop_back();
}

/*
 * AtEOXact_Snapshot
 * At the end of a transaction, complain on commit of a snapshot still
 * active or registered, and forget them all.
 */
void AtEOXact_Snapshot(bool isCommit) {
  if (isCommit) {
    if (registered_snapshots > 0)
      elog(WARNING, "registered snapshots seem to remain after cleanup");
    for (size_t i = 0; i < active_snapshots.size(); i++)
      elog(WARNING, "snapshot %p still active",
           (void *)active_snapshots[i].first);
  }
  active_snapshots.clear();
  registered_snapshots = 0;
  FirstSnapshotSet = false;
}

/*
 * pg_export_snapshot
 * Refused: a session sees sm's snapshot, which none exports.
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
 * Refuse "SET TRANSACTION SNAPSHOT": no snapshot is exported
 * (pg_export_snapshot()), so none is imported (WORKNOTE.md step 149).
 */
void ImportSnapshot(const char *idstr) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("SET TRANSACTION SNAPSHOT is not supported yet")));
}

/*
 * ThereAreNoPriorRegisteredSnapshots
 * Is at most one snapshot registered?
 */
bool ThereAreNoPriorRegisteredSnapshots(void) {
  return registered_snapshots <= 1;
}

/*
 * HaveRegisteredOrActiveSnapshot
 * Is any snapshot registered or active?
 */
bool HaveRegisteredOrActiveSnapshot(void) {
  return !active_snapshots.empty() || registered_snapshots > 0;
}

/*
 * XidInMVCCSnapshot
 * Is the given XID still in progress according to the snapshot?
 */
bool XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot) {
  /* Any xid < xmin is not in progress */
  if (TransactionIdPrecedes(xid, snapshot->xmin))
    return false;
  /* Any xid >= xmax is in progress */
  if (TransactionIdFollowsOrEquals(xid, snapshot->xmax))
    return true;
  /* digres assigns no subtransaction an xid: xid is its own topmost. */
  if (pg_lfind32(xid, snapshot->subxip, snapshot->subxcnt))
    return true;
  return pg_lfind32(xid, snapshot->xip, snapshot->xcnt);
}

/* A resource owner releases a registration it holds. */
static void ResOwnerReleaseSnapshot(Datum res) {
  Assert(registered_snapshots > 0);
  registered_snapshots--;
}
