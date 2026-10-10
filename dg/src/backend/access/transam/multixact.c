/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL multi-transaction-log manager
 *
 * The pg_multixact manager is a pg_xact-like manager that stores an array of
 * MultiXactMember for each MultiXactId.  It is a fundamental part of the
 * shared-row-lock implementation.  Each MultiXactMember is comprised of a
 * TransactionId and a set of flag bits.  The name is a bit historical:
 * originally, a MultiXactId consisted of more than one TransactionId (except
 * in rare corner cases), hence "multi".  Nowadays, however, it's perfectly
 * legitimate to have MultiXactIds that only include a single Xid.
 *
 * The meaning of the flag bits is opaque to this module, but they are mostly
 * used in heapam.c to identify lock modes that each of the member transactions
 * is holding on any given tuple.  This module just contains support to store
 * and retrieve the arrays.
 *
 * We use two SLRU areas, one for storing the offsets at which the data
 * starts for each MultiXactId in the other one.  This trick allows us to
 * store variable length arrays of TransactionIds.  (We could alternatively
 * use one area containing counts and TransactionIds, with valid MultiXactId
 * values pointing at slots containing counts; but that way seems less robust
 * since it would get completely confused if someone inquired about a bogus
 * MultiXactId that pointed to an intermediate slot containing an XID.)
 *
 * Like clog.c, and unlike subtrans.c, we have to preserve state across
 * crashes and ensure that MXID and offset numbering increases monotonically
 * across a crash.  We do this in the same way as it's done for transaction
 * IDs: the WAL record is guaranteed to contain evidence of every MXID we
 * could need to worry about, and we just make sure that at the end of
 * replay, the next-MXID and next-offset counters are at least as large as
 * anything we saw during replay.
 *
 * We are able to remove segments no longer necessary by carefully tracking
 * each table's used values: during vacuum, any multixact older than a certain
 * value is removed; the cutoff value is stored in pg_class.  The minimum value
 * across all tables in each database is stored in pg_database, and the global
 * minimum across all databases is part of pg_control and is kept in shared
 * memory.  Whenever that minimum is advanced, the SLRUs are truncated.
 *
 * When new multixactid values are to be created, care is taken that the
 * counter does not fall within the wraparound horizon considering the global
 * minimum value.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/multixact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

/*
 * Defines for MultiXactOffset page sizes.  A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 *
 * Note: because MultiXactOffsets are 32 bits and wrap around at 0xFFFFFFFF,
 * MultiXact page numbering also wraps around at
 * 0xFFFFFFFF/MULTIXACT_OFFSETS_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/MULTIXACT_OFFSETS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in TruncateMultiXact (see
 * MultiXactOffsetPagePrecedes).
 */

/* We need four bytes per offset */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

static inline int64 MultiXactIdToOffsetPage(MultiXactId multi) {
  return multi / MULTIXACT_OFFSETS_PER_PAGE;
}

static inline int MultiXactIdToOffsetEntry(MultiXactId multi) {
  return multi % MULTIXACT_OFFSETS_PER_PAGE;
}

/*
 * The situation for members is a bit more complex: we store one byte of
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags, and then the
 * corresponding 4 Xids.  Each such 5-word (20-byte) set we call a "group", and
 * are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep 409 groups
 * per page.  This wastes 12 bytes per page, but that's OK -- simplicity (and
 * performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need eight bits per xact, so one xact fits in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/*
 * Because the number of items per page is not a divisor of the last item
 * number (member 0xFFFFFFFF), the last segment does not use the maximum number
 * of pages, and moreover the last used page therein does not use the same
 * number of items as previous pages.  (Another way to say it is that the
 * 0xFFFFFFFF member is somewhere in the middle of the last page, so the page
 * has some empty space after that item.)
 *
 * This constant is the number of members in the last page of the last segment.
 */
#define MAX_MEMBERS_IN_LAST_MEMBERS_PAGE \
		((uint32) ((0xFFFFFFFF % MULTIXACT_MEMBERS_PER_PAGE) + 1))

/* page in which a member is to be found */
static inline int64 MXOffsetToMemberPage(MultiXactOffset offset) {
  return offset / MULTIXACT_MEMBERS_PER_PAGE;
}

/* Location (byte offset within page) of flag word for a given member */
static inline int MXOffsetToFlagsOffset(MultiXactOffset offset) {
  MultiXactOffset group = offset / MULTIXACT_MEMBERS_PER_MEMBERGROUP;
  int grouponpg = group % MULTIXACT_MEMBERGROUPS_PER_PAGE;
  int byteoff = grouponpg * MULTIXACT_MEMBERGROUP_SIZE;

  return byteoff;
}

static inline int MXOffsetToFlagsBitShift(MultiXactOffset offset) {
  int member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;
  int bshift = member_in_group * MXACT_MEMBER_BITS_PER_XACT;

  return bshift;
}

/* Location (byte offset within page) of TransactionId of given member */
static inline int MXOffsetToMemberOffset(MultiXactOffset offset) {
  int member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;

  return MXOffsetToFlagsOffset(offset) + MULTIXACT_FLAGBYTES_PER_GROUP +
         member_in_group * sizeof(TransactionId);
}

/*
 * Links to shared-memory data structures for MultiXact control
 */
static thread_local /*SHARED*/ SlruCtlData MultiXactOffsetCtlData;
static thread_local /*SHARED*/ SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * MultiXact state shared across all backends.  All this state is protected
 * by MultiXactGenLock.  (We also use SLRU bank's lock of MultiXactOffset and
 * MultiXactMember to guard accesses to the two sets of SLRU buffers.  For
 * concurrency's sake, we avoid holding more than one of these locks at a
 * time.)
 */
typedef struct MultiXactStateData {
  /* next-to-be-assigned MultiXactId */
  MultiXactId nextMXact;

  /* next-to-be-assigned offset */
  MultiXactOffset nextOffset;

  /* Have we completed multixact startup? */
  bool finishedStartup;

  /*
   * Oldest multixact that is still potentially referenced by a relation.
   * Anything older than this should not be consulted.  These values are
   * updated by vacuum.
   */
  MultiXactId oldestMultiXactId;
  Oid oldestMultiXactDB;

  /*
   * Oldest multixact offset that is potentially referenced by a multixact
   * referenced by a relation.  We don't always know this value, so there's
   * a flag here to indicate whether or not we currently do.
   */
  MultiXactOffset oldestOffset;
  bool oldestOffsetKnown;

  /* support for anti-wraparound measures */
  MultiXactId multiStopLimit;
  MultiXactId multiWrapLimit;

  /* support for members anti-wraparound measures */
  MultiXactOffset offsetStopLimit; /* known if oldestOffsetKnown */

  /*
   * Per-backend data starts here.  We have two arrays stored in the area
   * immediately following the MultiXactStateData struct:
   *
   * OldestMemberMXactId[k] is the oldest MultiXactId each backend's current
   * transaction(s) could possibly be a member of, or InvalidMultiXactId
   * when the backend has no live transaction that could possibly be a
   * member of a MultiXact.  Each backend sets its entry to the current
   * nextMXact counter just before first acquiring a shared lock in a given
   * transaction, and clears it at transaction end. (This works because only
   * during or after acquiring a shared lock could an XID possibly become a
   * member of a MultiXact, and that MultiXact would have to be created
   * during or after the lock acquisition.)
   *
   * In the OldestMemberMXactId array, there's a slot for all normal
   * backends (0..MaxBackends-1) followed by a slot for max_prepared_xacts
   * prepared transactions.
   *
   * OldestVisibleMXactId[k] is the oldest MultiXactId each backend's
   * current transaction(s) think is potentially live, or InvalidMultiXactId
   * when not in a transaction or not in a transaction that's paid any
   * attention to MultiXacts yet.  This is computed when first needed in a
   * given transaction, and cleared at transaction end.  We can compute it
   * as the minimum of the valid OldestMemberMXactId[] entries at the time
   * we compute it (using nextMXact if none are valid).  Each backend is
   * required not to attempt to access any SLRU data for MultiXactIds older
   * than its own OldestVisibleMXactId[] setting; this is necessary because
   * the relevant SLRU data can be concurrently truncated away.
   *
   * In the OldestVisibleMXactId array, there's a slot for all normal
   * backends (0..MaxBackends-1) only. No slots for prepared transactions.
   *
   * The oldest valid value among all of the OldestMemberMXactId[] and
   * OldestVisibleMXactId[] entries is considered by vacuum as the earliest
   * possible value still having any live member transaction -- OldestMxact.
   * Any value older than that is typically removed from tuple headers, or
   * "frozen" via being replaced with a new xmax.  VACUUM can sometimes even
   * remove an individual MultiXact xmax whose value is >= its OldestMxact
   * cutoff, though typically only when no individual member XID is still
   * running.  See FreezeMultiXactId for full details.
   *
   * Whenever VACUUM advances relminmxid, then either its OldestMxact cutoff
   * or the oldest extant Multi remaining in the table is used as the new
   * pg_class.relminmxid value (whichever is earlier).  The minimum of all
   * relminmxid values in each database is stored in pg_database.datminmxid.
   * In turn, the minimum of all of those values is stored in pg_control.
   * This is used as the truncation point for pg_multixact when unneeded
   * segments get removed by vac_truncate_clog() during vacuuming.
   */
  MultiXactId perBackendXactIds[FLEXIBLE_ARRAY_MEMBER];
} MultiXactStateData;

/*
 * Sizes of OldestMemberMXactId and OldestVisibleMXactId arrays.
 */
#define NumMemberSlots		(MaxBackends + max_prepared_xacts)
#define NumVisibleSlots		MaxBackends

/* Pointers to the state data in shared memory */
static thread_local /*SHARED*/ MultiXactStateData *MultiXactState;
static thread_local /*SHARED*/ MultiXactId *OldestMemberMXactId;
static thread_local /*SHARED*/ MultiXactId *OldestVisibleMXactId;

static inline MultiXactId *MyOldestMemberMXactIdSlot(void) {
  /*
   * The first MaxBackends entries in the OldestMemberMXactId array are
   * reserved for regular backends.  MyProcNumber should index into one of
   * them.
   */
  Assert(MyProcNumber >= 0 && MyProcNumber < MaxBackends);
  return &OldestMemberMXactId[MyProcNumber];
}

static inline MultiXactId *
PreparedXactOldestMemberMXactIdSlot(ProcNumber procno) {
  int prepared_xact_idx;

  Assert(procno >= FIRST_PREPARED_XACT_PROC_NUMBER);
  prepared_xact_idx = procno - FIRST_PREPARED_XACT_PROC_NUMBER;

  /*
   * The first MaxBackends entries in the OldestMemberMXactId array are
   * reserved for regular backends.  Prepared xacts come after them.
   */
  Assert(MaxBackends + prepared_xact_idx < NumMemberSlots);
  return &OldestMemberMXactId[MaxBackends + prepared_xact_idx];
}

static inline MultiXactId *MyOldestVisibleMXactIdSlot(void) {
  Assert(MyProcNumber >= 0 && MyProcNumber < NumVisibleSlots);
  return &OldestVisibleMXactId[MyProcNumber];
}

/*
 * Definitions for the backend-local MultiXactId cache.
 *
 * We use this cache to store known MultiXacts, so we don't need to go to
 * SLRU areas every time.
 *
 * The cache lasts for the duration of a single transaction, the rationale
 * for this being that most entries will contain our own TransactionId and
 * so they will be uninteresting by the time our next transaction starts.
 * (XXX not clear that this is correct --- other members of the MultiXact
 * could hang around longer than we did.  However, it's not clear what a
 * better policy for flushing old cache entries would be.)	FIXME actually
 * this is plain wrong now that multixact's may contain update Xids.
 *
 * We allocate the cache entries in a memory context that is deleted at
 * transaction end, so we don't need to do retail freeing of entries.
 */
typedef struct mXactCacheEnt {
  MultiXactId multi;
  int nmembers;
  dlist_node node;
  MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} mXactCacheEnt;

#define MAX_CACHE_ENTRIES	256
static thread_local /*LOCAL*/ dclist_head MXactCache =
    DCLIST_STATIC_INIT(MXactCache);
static thread_local /*LOCAL*/ MemoryContext MXactContext = NULL;

#ifdef MULTIXACT_DEBUG
#define debug_elog2(a, b) elog(a,b)
#define debug_elog3(a, b, c) elog(a,b,c)
#define debug_elog4(a, b, c, d) elog(a,b,c,d)
#define debug_elog5(a, b, c, d, e) elog(a,b,c,d,e)
#define debug_elog6(a, b, c, d, e, f) elog(a,b,c,d,e,f)
#else
#define debug_elog2(a, b)
#define debug_elog3(a, b, c)
#define debug_elog4(a, b, c, d)
#define debug_elog5(a, b, c, d, e)
#define debug_elog6(a, b, c, d, e, f)
#endif

/* internal MultiXactId management */
static void MultiXactIdSetOldestVisible(void);
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
                               int nmembers, MultiXactMember *members);
static MultiXactId GetNewMultiXactId(int nmembers, MultiXactOffset *offset);

/* MultiXact cache management */
static int mxactMemberComparator(const void *arg1, const void *arg2);
static MultiXactId mXactCacheGetBySet(int nmembers, MultiXactMember *members);
static int mXactCacheGetById(MultiXactId multi, MultiXactMember **members);
static void mXactCachePut(MultiXactId multi, int nmembers,
                          MultiXactMember *members);

static char *mxstatus_to_string(MultiXactStatus status);

/* management of SLRU infrastructure */
static int ZeroMultiXactOffsetPage(int64 pageno);
static int ZeroMultiXactMemberPage(int64 pageno);
static bool MultiXactOffsetPagePrecedes(int64 page1, int64 page2);
static bool MultiXactMemberPagePrecedes(int64 page1, int64 page2);
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
                                    MultiXactOffset offset2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static bool MultiXactOffsetWouldWrap(MultiXactOffset boundary,
                                     MultiXactOffset start, uint32 distance);
static void SetOffsetVacuumLimit(bool is_startup);
static bool find_multixact_start(MultiXactId multi, MultiXactOffset *result);

/*
 * MultiXactIdCreate
 *		Construct a MultiXactId representing two TransactionIds.
 *
 * The two XIDs must be different, or be requesting different statuses.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId MultiXactIdCreate(TransactionId xid1, MultiXactStatus status1,
                              TransactionId xid2, MultiXactStatus status2) {
  MultiXactId newMulti;
  MultiXactMember members[2];

  Assert(TransactionIdIsValid(xid1));
  Assert(TransactionIdIsValid(xid2));

  Assert(!TransactionIdEquals(xid1, xid2) || (status1 != status2));

  /* MultiXactIdSetOldestMember() must have been called already. */
  Assert(MultiXactIdIsValid(*MyOldestMemberMXactIdSlot()));

  /*
   * Note: unlike MultiXactIdExpand, we don't bother to check that both XIDs
   * are still running.  In typical usage, xid2 will be our own XID and the
   * caller just did a check on xid1, so it'd be wasted effort.
   */

  members[0].xid = xid1;
  members[0].status = status1;
  members[1].xid = xid2;
  members[1].status = status2;

  newMulti = MultiXactIdCreateFromMembers(2, members);

  debug_elog3(DEBUG2, "Create: %s", mxid_to_string(newMulti, 2, members));

  return newMulti;
}

/*
 * MultiXactIdExpand
 *		Add a TransactionId to a pre-existing MultiXactId.
 *
 * If the TransactionId is already a member of the passed MultiXactId with the
 * same status, just return it as-is.
 *
 * Note that we do NOT actually modify the membership of a pre-existing
 * MultiXactId; instead we create a new one.  This is necessary to avoid
 * a race condition against code trying to wait for one MultiXactId to finish;
 * see notes in heapam.c.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 *
 * Note: It is critical that MultiXactIds that come from an old cluster (i.e.
 * one upgraded by pg_upgrade from a cluster older than this feature) are not
 * passed in.
 */
MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
                              MultiXactStatus status) {
  MultiXactId newMulti;
  MultiXactMember *members;
  MultiXactMember *newMembers;
  int nmembers;
  int i;
  int j;

  Assert(MultiXactIdIsValid(multi));
  Assert(TransactionIdIsValid(xid));

  /* MultiXactIdSetOldestMember() must have been called already. */
  Assert(MultiXactIdIsValid(*MyOldestMemberMXactIdSlot()));

  debug_elog5(DEBUG2, "Expand: received multi %u, xid %u status %s", multi, xid,
              mxstatus_to_string(status));

  /*
   * Note: we don't allow for old multis here.  The reason is that the only
   * caller of this function does a check that the multixact is no longer
   * running.
   */
  nmembers = GetMultiXactIdMembers(multi, &members, false, false);

  if (nmembers < 0) {
    MultiXactMember member;

    /*
     * The MultiXactId is obsolete.  This can only happen if all the
     * MultiXactId members stop running between the caller checking and
     * passing it to us.  It would be better to return that fact to the
     * caller, but it would complicate the API and it's unlikely to happen
     * too often, so just deal with it by creating a singleton MultiXact.
     */
    member.xid = xid;
    member.status = status;
    newMulti = MultiXactIdCreateFromMembers(1, &member);

    debug_elog4(DEBUG2, "Expand: %u has no members, create singleton %u", multi,
                newMulti);
    return newMulti;
  }

  /*
   * If the TransactionId is already a member of the MultiXactId with the
   * same status, just return the existing MultiXactId.
   */
  for (i = 0; i < nmembers; i++) {
    if (TransactionIdEquals(members[i].xid, xid) &&
        (members[i].status == status)) {
      debug_elog4(DEBUG2, "Expand: %u is already a member of %u", xid, multi);
      pfree(members);
      return multi;
    }
  }

  /*
   * Determine which of the members of the MultiXactId are still of
   * interest. This is any running transaction, and also any transaction
   * that grabbed something stronger than just a lock and was committed. (An
   * update that aborted is of no interest here; and having more than one
   * update Xid in a multixact would cause errors elsewhere.)
   *
   * Removing dead members is not just an optimization: freezing of tuples
   * whose Xmax are multis depends on this behavior.
   *
   * Note we have the same race condition here as above: j could be 0 at the
   * end of the loop.
   */
  newMembers =
      (MultiXactMember *)palloc(sizeof(MultiXactMember) * (nmembers + 1));

  for (i = 0, j = 0; i < nmembers; i++) {
    if (TransactionIdIsInProgress(members[i].xid) ||
        (ISUPDATE_from_mxstatus(members[i].status) &&
         TransactionIdDidCommit(members[i].xid))) {
      newMembers[j].xid = members[i].xid;
      newMembers[j++].status = members[i].status;
    }
  }

  newMembers[j].xid = xid;
  newMembers[j++].status = status;
  newMulti = MultiXactIdCreateFromMembers(j, newMembers);

  pfree(members);
  pfree(newMembers);

  debug_elog3(DEBUG2, "Expand: returning new multi %u", newMulti);

  return newMulti;
}

/*
 * MultiXactIdIsRunning
 *		Returns whether a MultiXactId is "running".
 *
 * We return true if at least one member of the given MultiXactId is still
 * running.  Note that a "false" result is certain not to change,
 * because it is not legal to add members to an existing MultiXactId.
 *
 * Caller is expected to have verified that the multixact does not come from
 * a pg_upgraded share-locked tuple.
 */
bool MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly) {
  MultiXactMember *members;
  int nmembers;
  int i;

  debug_elog3(DEBUG2, "IsRunning %u?", multi);

  /*
   * "false" here means we assume our callers have checked that the given
   * multi cannot possibly come from a pg_upgraded database.
   */
  nmembers = GetMultiXactIdMembers(multi, &members, false, isLockOnly);

  if (nmembers <= 0) {
    debug_elog2(DEBUG2, "IsRunning: no members");
    return false;
  }

  /*
   * Checking for myself is cheap compared to looking in shared memory;
   * return true if any live subtransaction of the current top-level
   * transaction is a member.
   *
   * This is not needed for correctness, it's just a fast path.
   */
  for (i = 0; i < nmembers; i++) {
    if (TransactionIdIsCurrentTransactionId(members[i].xid)) {
      debug_elog3(DEBUG2, "IsRunning: I (%d) am running!", i);
      pfree(members);
      return true;
    }
  }

  /*
   * This could be made faster by having another entry point in procarray.c,
   * walking the PGPROC array only once for all the members.  But in most
   * cases nmembers should be small enough that it doesn't much matter.
   */
  for (i = 0; i < nmembers; i++) {
    if (TransactionIdIsInProgress(members[i].xid)) {
      debug_elog4(DEBUG2, "IsRunning: member %d (%u) is running", i,
                  members[i].xid);
      pfree(members);
      return true;
    }
  }

  pfree(members);

  debug_elog3(DEBUG2, "IsRunning: %u is not running", multi);

  return false;
}

/*
 * MultiXactIdSetOldestMember
 *		Save the oldest MultiXactId this transaction could be a member
 *of.
 *
 * We set the OldestMemberMXactId for a given transaction the first time it's
 * going to do some operation that might require a MultiXactId (tuple lock,
 * update or delete).  We need to do this even if we end up using a
 * TransactionId instead of a MultiXactId, because there is a chance that
 * another transaction would add our XID to a MultiXactId.
 *
 * The value to set is the next-to-be-assigned MultiXactId, so this is meant to
 * be called just before doing any such possibly-MultiXactId-able operation.
 */
void MultiXactIdSetOldestMember(void) {
  if (!MultiXactIdIsValid(*MyOldestMemberMXactIdSlot())) {
    MultiXactId nextMXact;

    /*
     * You might think we don't need to acquire a lock here, since
     * fetching and storing of TransactionIds is probably atomic, but in
     * fact we do: suppose we pick up nextMXact and then lose the CPU for
     * a long time.  Someone else could advance nextMXact, and then
     * another someone else could compute an OldestVisibleMXactId that
     * would be after the value we are going to store when we get control
     * back.  Which would be wrong.
     *
     * Note that a shared lock is sufficient, because it's enough to stop
     * someone from advancing nextMXact; and nobody else could be trying
     * to write to our OldestMember entry, only reading (and we assume
     * storing it is atomic.)
     */
    LWLockAcquire(MultiXactGenLock, LW_SHARED);

    /*
     * We have to beware of the possibility that nextMXact is in the
     * wrapped-around state.  We don't fix the counter itself here, but we
     * must be sure to store a valid value in our array entry.
     */
    nextMXact = MultiXactState->nextMXact;
    if (nextMXact < FirstMultiXactId)
      nextMXact = FirstMultiXactId;

    *MyOldestMemberMXactIdSlot() = nextMXact;

    LWLockRelease(MultiXactGenLock);

    debug_elog4(DEBUG2, "MultiXact: setting OldestMember[%d] = %u",
                MyProcNumber, nextMXact);
  }
}

/*
 * MultiXactIdSetOldestVisible
 *		Save the oldest MultiXactId this transaction considers possibly
 *live.
 *
 * We set the OldestVisibleMXactId for a given transaction the first time
 * it's going to inspect any MultiXactId.  Once we have set this, we are
 * guaranteed that SLRU data for MultiXactIds >= our own OldestVisibleMXactId
 * won't be truncated away.
 *
 * The value to set is the oldest of nextMXact and all the valid per-backend
 * OldestMemberMXactId[] entries.  Because of the locking we do, we can be
 * certain that no subsequent call to MultiXactIdSetOldestMember can set
 * an OldestMemberMXactId[] entry older than what we compute here.  Therefore
 * there is no live transaction, now or later, that can be a member of any
 * MultiXactId older than the OldestVisibleMXactId we compute here.
 */
static void MultiXactIdSetOldestVisible(void) {
  if (!MultiXactIdIsValid(*MyOldestVisibleMXactIdSlot())) {
    MultiXactId oldestMXact;
    int i;

    LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

    /*
     * We have to beware of the possibility that nextMXact is in the
     * wrapped-around state.  We don't fix the counter itself here, but we
     * must be sure to store a valid value in our array entry.
     */
    oldestMXact = MultiXactState->nextMXact;
    if (oldestMXact < FirstMultiXactId)
      oldestMXact = FirstMultiXactId;

    for (i = 0; i < NumMemberSlots; i++) {
      MultiXactId thisoldest = OldestMemberMXactId[i];

      if (MultiXactIdIsValid(thisoldest) &&
          MultiXactIdPrecedes(thisoldest, oldestMXact))
        oldestMXact = thisoldest;
    }

    *MyOldestVisibleMXactIdSlot() = oldestMXact;

    LWLockRelease(MultiXactGenLock);

    debug_elog4(DEBUG2, "MultiXact: setting OldestVisible[%d] = %u",
                MyProcNumber, oldestMXact);
  }
}

/*
 * ReadNextMultiXactId
 *		Return the next MultiXactId to be assigned, but don't allocate
 *it
 */
MultiXactId ReadNextMultiXactId(void) {
  MultiXactId mxid;

  /* XXX we could presumably do this without a lock. */
  LWLockAcquire(MultiXactGenLock, LW_SHARED);
  mxid = MultiXactState->nextMXact;
  LWLockRelease(MultiXactGenLock);

  if (mxid < FirstMultiXactId)
    mxid = FirstMultiXactId;

  return mxid;
}

/*
 * ReadMultiXactIdRange
 *		Get the range of IDs that may still be referenced by a relation.
 */
void ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next) {
  LWLockAcquire(MultiXactGenLock, LW_SHARED);
  *oldest = MultiXactState->oldestMultiXactId;
  *next = MultiXactState->nextMXact;
  LWLockRelease(MultiXactGenLock);

  if (*oldest < FirstMultiXactId)
    *oldest = FirstMultiXactId;
  if (*next < FirstMultiXactId)
    *next = FirstMultiXactId;
}

/*
 * MultiXactIdCreateFromMembers
 *		Make a new MultiXactId from the specified set of members
 *
 * Make SLRU and cache entries for a new MultiXactId, recording the
 * given TransactionIds as members.  Returns the newly created MultiXactId.
 *
 * NB: the passed members[] array will be sorted in-place.
 */
MultiXactId MultiXactIdCreateFromMembers(int nmembers,
                                         MultiXactMember *members) {
  MultiXactId multi;
  MultiXactOffset offset;

  debug_elog3(DEBUG2, "Create: %s",
              mxid_to_string(InvalidMultiXactId, nmembers, members));

  /*
   * See if the same set of members already exists in our cache; if so, just
   * re-use that MultiXactId.  (Note: it might seem that looking in our
   * cache is insufficient, and we ought to search disk to see if a
   * duplicate definition already exists.  But since we only ever create
   * MultiXacts containing our own XID, in most cases any such MultiXacts
   * were in fact created by us, and so will be in our cache.  There are
   * corner cases where someone else added us to a MultiXact without our
   * knowledge, but it's not worth checking for.)
   */
  multi = mXactCacheGetBySet(nmembers, members);
  if (MultiXactIdIsValid(multi)) {
    debug_elog2(DEBUG2, "Create: in cache!");
    return multi;
  }

  /* Verify that there is a single update Xid among the given members. */
  {
    int i;
    bool has_update = false;

    for (i = 0; i < nmembers; i++) {
      if (ISUPDATE_from_mxstatus(members[i].status)) {
        if (has_update)
          elog(ERROR, "new multixact has more than one updating member: %s",
               mxid_to_string(InvalidMultiXactId, nmembers, members));
        has_update = true;
      }
    }
  }

  /* Load the injection point before entering the critical section */
  INJECTION_POINT_LOAD("multixact-create-from-members");

  /*
   * Assign the MXID and offsets range to use, and make sure there is space
   * in the OFFSETs and MEMBERs files.  NB: this routine does
   * START_CRIT_SECTION().
   *
   * Note: unlike MultiXactIdCreate and MultiXactIdExpand, we do not check
   * that we've called MultiXactIdSetOldestMember here.  This is because
   * this routine is used in some places to create new MultiXactIds of which
   * the current backend is not a member, notably during freezing of multis
   * in vacuum.  During vacuum, in particular, it would be unacceptable to
   * keep OldestMulti set, in case it runs for long.
   */
  multi = GetNewMultiXactId(nmembers, &offset);

  INJECTION_POINT_CACHED("multixact-create-from-members", NULL);

  /* Now enter the information into the OFFSETs and MEMBERs logs */
  RecordNewMultiXact(multi, offset, nmembers, members);

  /* Done with critical section */
  END_CRIT_SECTION();

  /* Store the new MultiXactId in the local cache, too */
  mXactCachePut(multi, nmembers, members);

  debug_elog2(DEBUG2, "Create: all done");

  return multi;
}

/*
 * RecordNewMultiXact
 *		Write info about a new multixact into the offsets and members
 *files
 *
 * This is broken out of MultiXactIdCreateFromMembers so that xlog replay can
 * use it.
 */
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
                               int nmembers, MultiXactMember *members) {
  int64 pageno;
  int64 prev_pageno;
  int entryno;
  int slotno;
  MultiXactOffset *offptr;
  MultiXactId next;
  int64 next_pageno;
  int next_entryno;
  MultiXactOffset *next_offptr;
  MultiXactOffset next_offset;
  LWLock *lock;
  LWLock *prevlock = NULL;

  /* position of this multixid in the offsets SLRU area  */
  pageno = MultiXactIdToOffsetPage(multi);
  entryno = MultiXactIdToOffsetEntry(multi);

  /* position of the next multixid */
  next = multi + 1;
  if (next < FirstMultiXactId)
    next = FirstMultiXactId;
  next_pageno = MultiXactIdToOffsetPage(next);
  next_entryno = MultiXactIdToOffsetEntry(next);

  /*
   * Set the starting offset of this multixid's members.
   *
   * In the common case, it was already be set by the previous
   * RecordNewMultiXact call, as this was the next multixid of the previous
   * multixid.  But if multiple backends are generating multixids
   * concurrently, we might race ahead and get called before the previous
   * multixid.
   */
  lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
  LWLockAcquire(lock, LW_EXCLUSIVE);

  /*
   * Note: we pass the MultiXactId to SimpleLruReadPage as the "transaction"
   * to complain about if there's any I/O error.  This is kinda bogus, but
   * since the errors will always give the full pathname, it should be clear
   * enough that a MultiXactId is really involved.  Perhaps someday we'll
   * take the trouble to generalize the slru.c error reporting code.
   */
  slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
  offptr = (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
  offptr += entryno;

  if (*offptr != offset) {
    /* should already be set to the correct value, or not at all */
    Assert(*offptr == 0);
    *offptr = offset;
    MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
  }

  /*
   * Set the next multixid's offset to the end of this multixid's members.
   */
  if (next_pageno == pageno) {
    next_offptr = offptr + 1;
  } else {
    /* must be the first entry on the page */
    Assert(next_entryno == 0 || next == FirstMultiXactId);

    /* Swap the lock for a lock on the next page */
    LWLockRelease(lock);
    lock = SimpleLruGetBankLock(MultiXactOffsetCtl, next_pageno);
    LWLockAcquire(lock, LW_EXCLUSIVE);

    slotno = SimpleLruReadPage(MultiXactOffsetCtl, next_pageno, true, next);
    next_offptr =
        (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
    next_offptr += next_entryno;
  }

  /* Like in GetNewMultiXactId(), skip over offset 0 */
  next_offset = offset + nmembers;
  if (next_offset == 0)
    next_offset = 1;
  if (*next_offptr != next_offset) {
    /* should already be set to the correct value, or not at all */
    Assert(*next_offptr == 0);
    *next_offptr = next_offset;
    MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
  }

  /* Release MultiXactOffset SLRU lock. */
  LWLockRelease(lock);

  prev_pageno = -1;

  for (int i = 0; i < nmembers; i++, offset++) {
    TransactionId *memberptr;
    uint32 *flagsptr;
    uint32 flagsval;
    int bshift;
    int flagsoff;
    int memberoff;

    Assert(members[i].status <= MultiXactStatusUpdate);

    pageno = MXOffsetToMemberPage(offset);
    memberoff = MXOffsetToMemberOffset(offset);
    flagsoff = MXOffsetToFlagsOffset(offset);
    bshift = MXOffsetToFlagsBitShift(offset);

    if (pageno != prev_pageno) {
      /*
       * MultiXactMember SLRU page is changed so check if this new page
       * fall into the different SLRU bank then release the old bank's
       * lock and acquire lock on the new bank.
       */
      lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);
      if (lock != prevlock) {
        if (prevlock != NULL)
          LWLockRelease(prevlock);

        LWLockAcquire(lock, LW_EXCLUSIVE);
        prevlock = lock;
      }
      slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
      prev_pageno = pageno;
    }

    memberptr =
        (TransactionId *)(MultiXactMemberCtl->shared->page_buffer[slotno] +
                          memberoff);

    *memberptr = members[i].xid;

    flagsptr =
        (uint32 *)(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

    flagsval = *flagsptr;
    flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
    flagsval |= (members[i].status << bshift);
    *flagsptr = flagsval;

    MultiXactMemberCtl->shared->page_dirty[slotno] = true;
  }

  if (prevlock != NULL)
    LWLockRelease(prevlock);
}

/*
 * GetNewMultiXactId
 *		Get the next MultiXactId.
 *
 * Also, reserve the needed amount of space in the "members" area.  The
 * starting offset of the reserved space is returned in *offset.
 *
 * This may extend the offsets and/or members files.  We have to do that
 * while holding MultiXactGenLock, so that a page is zeroed before any
 * backend can possibly try to store data in it.
 *
 * We start a critical section before advancing the shared counters.  The
 * caller must end the critical section after writing SLRU data.
 */
static MultiXactId GetNewMultiXactId(int nmembers, MultiXactOffset *offset) {
  MultiXactId result;
  MultiXactOffset nextOffset;

  debug_elog3(DEBUG2, "GetNew: for %d xids", nmembers);

  LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

  /* Handle wraparound of the nextMXact counter */
  if (MultiXactState->nextMXact < FirstMultiXactId)
    MultiXactState->nextMXact = FirstMultiXactId;

  /* Assign the MXID */
  result = MultiXactState->nextMXact;

  /*----------
   * Check to see if it's safe to assign another MultiXactId.  This protects
   * against catastrophic data loss due to multixact wraparound.  The basic
   * rules are:
   *
   * If we're past multiStopLimit, refuse to create new MultiXactIds.
   *
   * Note these are pretty much the same protections in GetNewTransactionId.
   *----------
   */
  if (!MultiXactIdPrecedes(result, MultiXactState->multiStopLimit)) {
    /*
     * For safety's sake, we release MultiXactGenLock while reporting the
     * error.  This is not so much because we care about
     * preserving concurrency in this situation, as to avoid any
     * possibility of deadlock while doing get_database_name(). First,
     * copy all the shared values we'll need in this path.
     */
    MultiXactId multiStopLimit = MultiXactState->multiStopLimit;
    Oid oldest_datoid = MultiXactState->oldestMultiXactDB;

    LWLockRelease(MultiXactGenLock);

    if (IsUnderPostmaster && !MultiXactIdPrecedes(result, multiStopLimit)) {
      char *oldest_datname = get_database_name(oldest_datoid);

      /* complain even if that DB has disappeared */
      if (oldest_datname)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("database is not accepting commands that assign new "
                        "MultiXactIds to avoid wraparound data loss in "
                        "database \"%s\"",
                        oldest_datname)));
      else
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("database is not accepting commands that assign new "
                        "MultiXactIds to avoid wraparound data loss in "
                        "database with OID %u",
                        oldest_datoid)));
    }

    /* Re-acquire lock and start over */
    LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
    result = MultiXactState->nextMXact;
    if (result < FirstMultiXactId)
      result = FirstMultiXactId;
  }

  /*
   * Make sure there is room for the next MXID in the file.  Assigning this
   * MXID sets the next MXID's offset already.
   */
  ExtendMultiXactOffset(result + 1);

  /*
   * Reserve the members space, similarly to above.  Also, be careful not to
   * return zero as the starting offset for any multixact. See
   * GetMultiXactIdMembers() for motivation.
   */
  nextOffset = MultiXactState->nextOffset;
  if (nextOffset == 0) {
    *offset = 1;
    nmembers++; /* allocate member slot 0 too */
  } else
    *offset = nextOffset;

  /*----------
   * Protect against overrun of the members space as well, with the
   * following rules:
   *
   * If we're past offsetStopLimit, refuse to generate more multis.
   *
   * Note we haven't updated the shared state yet, so if we fail at this
   * point, the multixact ID we grabbed can still be used by the next guy.
   *----------
   */
  if (MultiXactState->oldestOffsetKnown &&
      MultiXactOffsetWouldWrap(MultiXactState->offsetStopLimit, nextOffset,
                               nmembers)) {
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("multixact \"members\" limit exceeded"),
             errdetail_plural(
                 "This command would create a multixact with %u members, but "
                 "the remaining space is only enough for %u member.",
                 "This command would create a multixact with %u members, but "
                 "the remaining space is only enough for %u members.",
                 MultiXactState->offsetStopLimit - nextOffset - 1, nmembers,
                 MultiXactState->offsetStopLimit - nextOffset - 1)));
  }

  ExtendMultiXactMember(nextOffset, nmembers);

  /*
   * Critical section from here until caller has written the data into the
   * just-reserved SLRU space; we don't want to error out with a partly
   * written MultiXact structure.  (In particular, failing to write our
   * start offset after advancing nextMXact would effectively corrupt the
   * previous MultiXact.)
   */
  START_CRIT_SECTION();

  /*
   * Advance counters.  As in GetNewTransactionId(), this must not happen
   * until after file extension has succeeded!
   *
   * We don't care about MultiXactId wraparound here; it will be handled by
   * the next iteration.  But note that nextMXact may be InvalidMultiXactId
   * or the first value on a segment-beginning page after this routine
   * exits, so anyone else looking at the variable must be prepared to deal
   * with either case.  Similarly, nextOffset may be zero, but we won't use
   * that as the actual start offset of the next multixact.
   */
  (MultiXactState->nextMXact)++;

  MultiXactState->nextOffset += nmembers;

  LWLockRelease(MultiXactGenLock);

  debug_elog4(DEBUG2, "GetNew: returning %u offset %u", result, *offset);
  return result;
}

/*
 * GetMultiXactIdMembers
 *		Return the set of MultiXactMembers that make up a MultiXactId
 *
 * Return value is the number of members found, or -1 if there are none,
 * and *members is set to a newly palloc'ed array of members.  It's the
 * caller's responsibility to free it when done with it.
 *
 * from_pgupgrade must be passed as true if and only if only the multixact
 * corresponds to a value from a tuple that was locked in a 9.2-or-older
 * installation and later pg_upgrade'd (that is, the infomask is
 * HEAP_LOCKED_UPGRADED).  In this case, we know for certain that no members
 * can still be running, so we return -1 just like for an empty multixact
 * without any further checking.  It would be wrong to try to resolve such a
 * multixact: either the multixact is within the current valid multixact
 * range, in which case the returned result would be bogus, or outside that
 * range, in which case an error would be raised.
 *
 * In all other cases, the passed multixact must be within the known valid
 * range, that is, greater than or equal to oldestMultiXactId, and less than
 * nextMXact.  Otherwise, an error is raised.
 *
 * isLockOnly must be set to true if caller is certain that the given multi
 * is used only to lock tuples; can be false without loss of correctness,
 * but passing a true means we can return quickly without checking for
 * old updates.
 */
int GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
                          bool from_pgupgrade, bool isLockOnly) {
  int64 pageno;
  int64 prev_pageno;
  int entryno;
  int slotno;
  MultiXactOffset *offptr;
  MultiXactOffset offset;
  int length;
  int truelength;
  MultiXactId oldestMXact;
  MultiXactId nextMXact;
  MultiXactId tmpMXact;
  MultiXactOffset nextOffset;
  MultiXactMember *ptr;
  LWLock *lock;

  debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

  if (!MultiXactIdIsValid(multi) || from_pgupgrade) {
    *members = NULL;
    return -1;
  }

  /* See if the MultiXactId is in the local cache */
  length = mXactCacheGetById(multi, members);
  if (length >= 0) {
    debug_elog3(DEBUG2, "GetMembers: found %s in the cache",
                mxid_to_string(multi, length, *members));
    return length;
  }

  /* Set our OldestVisibleMXactId[] entry if we didn't already */
  MultiXactIdSetOldestVisible();

  /*
   * If we know the multi is used only for locking and not for updates, then
   * we can skip checking if the value is older than our oldest visible
   * multi.  It cannot possibly still be running.
   */
  if (isLockOnly && MultiXactIdPrecedes(multi, *MyOldestVisibleMXactIdSlot())) {
    debug_elog2(DEBUG2, "GetMembers: a locker-only multi is too old");
    *members = NULL;
    return -1;
  }

  /*
   * We check known limits on MultiXact before resorting to the SLRU area.
   *
   * An ID older than MultiXactState->oldestMultiXactId cannot possibly be
   * useful; it has already been removed, or will be removed shortly, by
   * truncation.  If one is passed, an error is raised.
   *
   * Also, an ID >= nextMXact shouldn't ever be seen here; if it is seen, it
   * implies undetected ID wraparound has occurred.  This raises a hard
   * error.
   *
   * Shared lock is enough here since we aren't modifying any global state.
   * Acquire it just long enough to grab the current counter values.  We may
   * need both nextMXact and nextOffset; see below.
   */
  LWLockAcquire(MultiXactGenLock, LW_SHARED);

  oldestMXact = MultiXactState->oldestMultiXactId;
  nextMXact = MultiXactState->nextMXact;
  nextOffset = MultiXactState->nextOffset;

  LWLockRelease(MultiXactGenLock);

  if (MultiXactIdPrecedes(multi, oldestMXact))
    ereport(
        ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("MultiXactId %u does no longer exist -- apparent wraparound",
                multi)));

  if (!MultiXactIdPrecedes(multi, nextMXact))
    ereport(
        ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg(
             "MultiXactId %u has not been created yet -- apparent wraparound",
             multi)));

  /*
   * Find out the offset at which we need to start reading MultiXactMembers
   * and the number of members in the multixact.  We determine the latter as
   * the difference between this multixact's starting offset and the next
   * one's.  However, there are some corner cases to worry about:
   *
   * 1. This multixact may be the latest one created, in which case there is
   * no next one to look at.  The next multixact's offset should be set
   * already, as we set it in RecordNewMultiXact(), but we used to not do
   * that in older minor versions.  To cope with that case, if this
   * multixact is the latest one created, use the nextOffset value we read
   * above as the endpoint.
   *
   * 2. Because GetNewMultiXactId skips over offset zero, to reserve zero
   * for to mean "unset", there is an ambiguity near the point of offset
   * wraparound.  If we see next multixact's offset is one, is that our
   * multixact's actual endpoint, or did it end at zero with a subsequent
   * increment?  We handle this using the knowledge that if the zero'th
   * member slot wasn't filled, it'll contain zero, and zero isn't a valid
   * transaction ID so it can't be a multixact member.  Therefore, if we
   * read a zero from the members array, just ignore it.
   *
   * This is all pretty messy, but the mess occurs only in infrequent corner
   * cases, so it seems better than holding the MultiXactGenLock for a long
   * time on every multixact creation.
   */
  pageno = MultiXactIdToOffsetPage(multi);
  entryno = MultiXactIdToOffsetEntry(multi);

  /* Acquire the bank lock for the page we need. */
  lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
  LWLockAcquire(lock, LW_EXCLUSIVE);

  slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
  offptr = (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
  offptr += entryno;
  offset = *offptr;

  Assert(offset != 0);

  /*
   * Use the same increment rule as GetNewMultiXactId(), that is, don't
   * handle wraparound explicitly until needed.
   */
  tmpMXact = multi + 1;

  if (nextMXact == tmpMXact) {
    /* Corner case 1: there is no next multixact */
    length = nextOffset - offset;
  } else {
    MultiXactOffset nextMXOffset;

    /* handle wraparound if needed */
    if (tmpMXact < FirstMultiXactId)
      tmpMXact = FirstMultiXactId;

    prev_pageno = pageno;

    pageno = MultiXactIdToOffsetPage(tmpMXact);
    entryno = MultiXactIdToOffsetEntry(tmpMXact);

    if (pageno != prev_pageno) {
      LWLock *newlock;

      /*
       * Since we're going to access a different SLRU page, if this page
       * falls under a different bank, release the old bank's lock and
       * acquire the lock of the new bank.
       */
      newlock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
      if (newlock != lock) {
        LWLockRelease(lock);
        LWLockAcquire(newlock, LW_EXCLUSIVE);
        lock = newlock;
      }
      slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, tmpMXact);
    }

    offptr = (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
    offptr += entryno;
    nextMXOffset = *offptr;

    if (nextMXOffset == 0)
      ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                      errmsg("MultiXact %u has invalid next offset", multi)));

    length = nextMXOffset - offset;
  }

  LWLockRelease(lock);
  lock = NULL;

  ptr = (MultiXactMember *)palloc(length * sizeof(MultiXactMember));

  truelength = 0;
  prev_pageno = -1;
  for (int i = 0; i < length; i++, offset++) {
    TransactionId *xactptr;
    uint32 *flagsptr;
    int flagsoff;
    int bshift;
    int memberoff;

    pageno = MXOffsetToMemberPage(offset);
    memberoff = MXOffsetToMemberOffset(offset);

    if (pageno != prev_pageno) {
      LWLock *newlock;

      /*
       * Since we're going to access a different SLRU page, if this page
       * falls under a different bank, release the old bank's lock and
       * acquire the lock of the new bank.
       */
      newlock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);
      if (newlock != lock) {
        if (lock)
          LWLockRelease(lock);
        LWLockAcquire(newlock, LW_EXCLUSIVE);
        lock = newlock;
      }

      slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
      prev_pageno = pageno;
    }

    xactptr =
        (TransactionId *)(MultiXactMemberCtl->shared->page_buffer[slotno] +
                          memberoff);

    if (!TransactionIdIsValid(*xactptr)) {
      /*
       * Corner case 2: offset must have wrapped around to unused slot
       * zero.
       */
      Assert(offset == 0);
      continue;
    }

    flagsoff = MXOffsetToFlagsOffset(offset);
    bshift = MXOffsetToFlagsBitShift(offset);
    flagsptr =
        (uint32 *)(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

    ptr[truelength].xid = *xactptr;
    ptr[truelength].status =
        (MultiXactStatus)((*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK);
    truelength++;
  }

  LWLockRelease(lock);

  /* A multixid with zero members should not happen */
  Assert(truelength > 0);

  /*
   * Copy the result into the local cache.
   */
  mXactCachePut(multi, truelength, ptr);

  debug_elog3(DEBUG2, "GetMembers: no cache for %s",
              mxid_to_string(multi, truelength, ptr));
  *members = ptr;
  return truelength;
}

/*
 * mxactMemberComparator
 *		qsort comparison function for MultiXactMember
 *
 * We can't use wraparound comparison for XIDs because that does not respect
 * the triangle inequality!  Any old sort order will do.
 */
static int mxactMemberComparator(const void *arg1, const void *arg2) {
  MultiXactMember member1 = *(const MultiXactMember *)arg1;
  MultiXactMember member2 = *(const MultiXactMember *)arg2;

  if (member1.xid > member2.xid)
    return 1;
  if (member1.xid < member2.xid)
    return -1;
  if (member1.status > member2.status)
    return 1;
  if (member1.status < member2.status)
    return -1;
  return 0;
}

/*
 * mXactCacheGetBySet
 *		returns a MultiXactId from the cache based on the set of
 *		TransactionIds that compose it, or InvalidMultiXactId if
 *		none matches.
 *
 * This is helpful, for example, if two transactions want to lock a huge
 * table.  By using the cache, the second will use the same MultiXactId
 * for the majority of tuples, thus keeping MultiXactId usage low (saving
 * both I/O and wraparound issues).
 *
 * NB: the passed members array will be sorted in-place.
 */
static MultiXactId mXactCacheGetBySet(int nmembers, MultiXactMember *members) {
  dlist_iter iter;

  debug_elog3(DEBUG2, "CacheGet: looking for %s",
              mxid_to_string(InvalidMultiXactId, nmembers, members));

  /* sort the array so comparison is easy */
  qsort(members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

  dclist_foreach(iter, &MXactCache) {
    mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node, iter.cur);

    if (entry->nmembers != nmembers)
      continue;

    /*
     * We assume the cache entries are sorted, and that the unused bits in
     * "status" are zeroed.
     */
    if (memcmp(members, entry->members, nmembers * sizeof(MultiXactMember)) ==
        0) {
      debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
      dclist_move_head(&MXactCache, iter.cur);
      return entry->multi;
    }
  }

  debug_elog2(DEBUG2, "CacheGet: not found :-(");
  return InvalidMultiXactId;
}

/*
 * mXactCacheGetById
 *		returns the composing MultiXactMember set from the cache for a
 *		given MultiXactId, if present.
 *
 * If successful, *xids is set to the address of a palloc'd copy of the
 * MultiXactMember set.  Return value is number of members, or -1 on failure.
 */
static int mXactCacheGetById(MultiXactId multi, MultiXactMember **members) {
  dlist_iter iter;

  debug_elog3(DEBUG2, "CacheGet: looking for %u", multi);

  dclist_foreach(iter, &MXactCache) {
    mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node, iter.cur);

    if (entry->multi == multi) {
      MultiXactMember *ptr;
      Size size;

      size = sizeof(MultiXactMember) * entry->nmembers;
      ptr = (MultiXactMember *)palloc(size);

      memcpy(ptr, entry->members, size);

      debug_elog3(DEBUG2, "CacheGet: found %s",
                  mxid_to_string(multi, entry->nmembers, entry->members));

      /*
       * Note we modify the list while not using a modifiable iterator.
       * This is acceptable only because we exit the iteration
       * immediately afterwards.
       */
      dclist_move_head(&MXactCache, iter.cur);

      *members = ptr;
      return entry->nmembers;
    }
  }

  debug_elog2(DEBUG2, "CacheGet: not found");
  return -1;
}

/*
 * mXactCachePut
 *		Add a new MultiXactId and its composing set into the local
 *cache.
 */
static void mXactCachePut(MultiXactId multi, int nmembers,
                          MultiXactMember *members) {
  mXactCacheEnt *entry;

  debug_elog3(DEBUG2, "CachePut: storing %s",
              mxid_to_string(multi, nmembers, members));

  if (MXactContext == NULL) {
    /* The cache only lives as long as the current transaction */
    debug_elog2(DEBUG2, "CachePut: initializing memory context");
    MXactContext = AllocSetContextCreate(
        TopTransactionContext, "MultiXact cache context", ALLOCSET_SMALL_SIZES);
  }

  entry = (mXactCacheEnt *)MemoryContextAlloc(
      MXactContext,
      offsetof(mXactCacheEnt, members) + nmembers * sizeof(MultiXactMember));

  entry->multi = multi;
  entry->nmembers = nmembers;
  memcpy(entry->members, members, nmembers * sizeof(MultiXactMember));

  /* mXactCacheGetBySet assumes the entries are sorted, so sort them */
  qsort(entry->members, nmembers, sizeof(MultiXactMember),
        mxactMemberComparator);

  dclist_push_head(&MXactCache, &entry->node);
  if (dclist_count(&MXactCache) > MAX_CACHE_ENTRIES) {
    dlist_node *node;

    node = dclist_tail_node(&MXactCache);
    dclist_delete_from(&MXactCache, node);

    entry = dclist_container(mXactCacheEnt, node, node);
    debug_elog3(DEBUG2, "CachePut: pruning cached multi %u", entry->multi);

    pfree(entry);
  }
}

static char *mxstatus_to_string(MultiXactStatus status) {
  switch (status) {
  case MultiXactStatusForKeyShare:
    return "keysh";
  case MultiXactStatusForShare:
    return "sh";
  case MultiXactStatusForNoKeyUpdate:
    return "fornokeyupd";
  case MultiXactStatusForUpdate:
    return "forupd";
  case MultiXactStatusNoKeyUpdate:
    return "nokeyupd";
  case MultiXactStatusUpdate:
    return "upd";
  default:
    elog(ERROR, "unrecognized multixact status %d", status);
    return "";
  }
}

char *mxid_to_string(MultiXactId multi, int nmembers,
                     MultiXactMember *members) {
  static thread_local /*LOCAL*/ char *str = NULL;
  StringInfoData buf;
  int i;

  if (str != NULL)
    pfree(str);

  initStringInfo(&buf);

  appendStringInfo(&buf, "%u %d[%u (%s)", multi, nmembers, members[0].xid,
                   mxstatus_to_string(members[0].status));

  for (i = 1; i < nmembers; i++)
    appendStringInfo(&buf, ", %u (%s)", members[i].xid,
                     mxstatus_to_string(members[i].status));

  appendStringInfoChar(&buf, ']');
  str = MemoryContextStrdup(TopMemoryContext, buf.data);
  pfree(buf.data);
  return str;
}

/*
 * AtEOXact_MultiXact
 *		Handle transaction end for MultiXact
 *
 * This is called at top transaction commit or abort (we don't care which).
 */
void AtEOXact_MultiXact(void) {
  /*
   * Reset our OldestMemberMXactId and OldestVisibleMXactId values, both of
   * which should only be valid while within a transaction.
   *
   * We assume that storing a MultiXactId is atomic and so we need not take
   * MultiXactGenLock to do this.
   */
  *MyOldestMemberMXactIdSlot() = InvalidMultiXactId;
  *MyOldestVisibleMXactIdSlot() = InvalidMultiXactId;

  /*
   * Discard the local MultiXactId cache.  Since MXactContext was created as
   * a child of TopTransactionContext, we needn't delete it explicitly.
   */
  MXactContext = NULL;
  dclist_init(&MXactCache);
}

/*
 * Initialization of shared memory for MultiXact.
 *
 * MultiXactSharedStateShmemSize() calculates the size of the MultiXactState
 * struct, and the two per-backend MultiXactId arrays.  They are carved out of
 * the same allocation.  MultiXactShmemSize() additionally includes the memory
 * needed for the two SLRU areas.
 */
static Size MultiXactSharedStateShmemSize(void) {
  Size size;

  size = offsetof(MultiXactStateData, perBackendXactIds);
  size = add_size(size, mul_size(sizeof(MultiXactId), NumMemberSlots));
  size = add_size(size, mul_size(sizeof(MultiXactId), NumVisibleSlots));
  return size;
}

void MultiXactShmemInit(void) {
  bool found;

  debug_elog2(DEBUG2, "Shared Memory Init for MultiXact");

  MultiXactOffsetCtl->PagePrecedes = MultiXactOffsetPagePrecedes;
  MultiXactMemberCtl->PagePrecedes = MultiXactMemberPagePrecedes;

  SimpleLruInit(MultiXactOffsetCtl, "multixact_offset",
                multixact_offset_buffers, "pg_multixact/offsets",
                LWTRANCHE_MULTIXACTOFFSET_BUFFER,
                LWTRANCHE_MULTIXACTOFFSET_SLRU, false);
  SlruPagePrecedesUnitTests(MultiXactOffsetCtl, MULTIXACT_OFFSETS_PER_PAGE);
  SimpleLruInit(MultiXactMemberCtl, "multixact_member",
                multixact_member_buffers, "pg_multixact/members",
                LWTRANCHE_MULTIXACTMEMBER_BUFFER,
                LWTRANCHE_MULTIXACTMEMBER_SLRU, false);
  /* doesn't call SimpleLruTruncate() or meet criteria for unit tests */

  /* Initialize our shared state struct */
  MultiXactState = (MultiXactStateData *)ShmemInitStruct(
      "Shared MultiXact State", MultiXactSharedStateShmemSize(), &found);
  if (!IsUnderPostmaster) {
    Assert(!found);

    /* Make sure we zero out the per-backend state */
    MemSet(MultiXactState, 0, MultiXactSharedStateShmemSize());
  } else
    Assert(found);

  /*
   * Set up array pointers.
   */
  OldestMemberMXactId = MultiXactState->perBackendXactIds;
  OldestVisibleMXactId = OldestMemberMXactId + NumMemberSlots;
}

/*
 * GUC check_hook for multixact_offset_buffers
 */
bool check_multixact_offset_buffers(int *newval, void **extra,
                                    GucSource source) {
  return check_slru_buffers("multixact_offset_buffers", newval);
}

/*
 * GUC check_hook for multixact_member_buffers
 */
bool check_multixact_member_buffers(int *newval, void **extra,
                                    GucSource source) {
  return check_slru_buffers("multixact_member_buffers", newval);
}

/*
 * Initialize (or reinitialize) a page of MultiXactOffset to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int ZeroMultiXactOffsetPage(int64 pageno) {
  int slotno;

  slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);

  return slotno;
}

/*
 * Ditto, for MultiXactMember
 */
static int ZeroMultiXactMemberPage(int64 pageno) {
  int slotno;

  slotno = SimpleLruZeroPage(MultiXactMemberCtl, pageno);

  return slotno;
}

/*
 * This must be called ONCE at the end of startup/recovery.
 */
void TrimMultiXact(void) {
  MultiXactId nextMXact;
  MultiXactOffset offset;
  MultiXactId oldestMXact;
  Oid oldestMXactDB;
  int64 pageno;
  int entryno;
  int flagsoff;

  LWLockAcquire(MultiXactGenLock, LW_SHARED);
  nextMXact = MultiXactState->nextMXact;
  offset = MultiXactState->nextOffset;
  oldestMXact = MultiXactState->oldestMultiXactId;
  oldestMXactDB = MultiXactState->oldestMultiXactDB;
  LWLockRelease(MultiXactGenLock);

  /* Clean up offsets state */

  /*
   * (Re-)Initialize our idea of the latest page number for offsets.
   */
  pageno = MultiXactIdToOffsetPage(nextMXact);
  pg_atomic_write_u64(&MultiXactOffsetCtl->shared->latest_page_number, pageno);

  /*
   * Set the offset of nextMXact on the offsets page.  This is normally done
   * in RecordNewMultiXact() of the previous multixact, but we used to not
   * do that in older minor versions.  To ensure that the next offset is set
   * if the binary was just upgraded from an older minor version, do it now.
   *
   * Zero out the remainder of the page.  See notes in TrimCLOG() for
   * background.  Unlike CLOG, some WAL record covers every pg_multixact
   * SLRU mutation.  Since, also unlike CLOG, we ignore the WAL rule "write
   * xlog before data," nextMXact successors may carry obsolete, nonzero
   * offset values.
   */
  entryno = MultiXactIdToOffsetEntry(nextMXact);
  {
    int slotno;
    MultiXactOffset *offptr;
    LWLock *lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);

    LWLockAcquire(lock, LW_EXCLUSIVE);
    if (entryno == 0)
      slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);
    else
      slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, nextMXact);
    offptr = (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
    offptr += entryno;

    *offptr = offset;
    if (entryno != 0 && (entryno + 1) * sizeof(MultiXactOffset) != BLCKSZ)
      MemSet(offptr + 1, 0, BLCKSZ - (entryno + 1) * sizeof(MultiXactOffset));

    MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
    LWLockRelease(lock);
  }

  /*
   * And the same for members.
   *
   * (Re-)Initialize our idea of the latest page number for members.
   */
  pageno = MXOffsetToMemberPage(offset);
  pg_atomic_write_u64(&MultiXactMemberCtl->shared->latest_page_number, pageno);

  /*
   * Zero out the remainder of the current members page.  See notes in
   * TrimCLOG() for motivation.
   */
  flagsoff = MXOffsetToFlagsOffset(offset);
  if (flagsoff != 0) {
    int slotno;
    TransactionId *xidptr;
    int memberoff;
    LWLock *lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);

    LWLockAcquire(lock, LW_EXCLUSIVE);
    memberoff = MXOffsetToMemberOffset(offset);
    slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, offset);
    xidptr = (TransactionId *)(MultiXactMemberCtl->shared->page_buffer[slotno] +
                               memberoff);

    MemSet(xidptr, 0, BLCKSZ - memberoff);

    /*
     * Note: we don't need to zero out the flag bits in the remaining
     * members of the current group, because they are always reset before
     * writing.
     */

    MultiXactMemberCtl->shared->page_dirty[slotno] = true;
    LWLockRelease(lock);
  }

  /* signal that we're officially up */
  LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
  MultiXactState->finishedStartup = true;
  LWLockRelease(MultiXactGenLock);

  /* Now compute how far away the next members wraparound is. */
  SetMultiXactIdLimit(oldestMXact, oldestMXactDB, true);
}

/*
 * Get the MultiXact data to save in a checkpoint record
 */
void MultiXactGetCheckptMulti(bool is_shutdown, MultiXactId *nextMulti,
                              MultiXactOffset *nextMultiOffset,
                              MultiXactId *oldestMulti, Oid *oldestMultiDB) {
  LWLockAcquire(MultiXactGenLock, LW_SHARED);
  *nextMulti = MultiXactState->nextMXact;
  *nextMultiOffset = MultiXactState->nextOffset;
  *oldestMulti = MultiXactState->oldestMultiXactId;
  *oldestMultiDB = MultiXactState->oldestMultiXactDB;
  LWLockRelease(MultiXactGenLock);

  debug_elog6(DEBUG2,
              "MultiXact: checkpoint is nextMulti %u, nextOffset %u, "
              "oldestMulti %u in DB %u",
              *nextMulti, *nextMultiOffset, *oldestMulti, *oldestMultiDB);
}

/*
 * Determine the last safe MultiXactId to allocate given the currently oldest
 * datminmxid (ie, the oldest MultiXactId that might exist in any database
 * of our cluster), and the OID of the (or a) database with that value.
 *
 * is_startup is true when we are just starting the cluster, false when we
 * are updating state in a running cluster.  This only affects log messages.
 */
void SetMultiXactIdLimit(MultiXactId oldest_datminmxid, Oid oldest_datoid,
                         bool is_startup) {
  MultiXactId multiStopLimit;
  MultiXactId multiWrapLimit;

  Assert(MultiXactIdIsValid(oldest_datminmxid));

  /*
   * We pretend that a wrap will happen halfway through the multixact ID
   * space, but that's not really true, because multixacts wrap differently
   * from transaction IDs.  Note that, separately from any concern about
   * multixact IDs wrapping, we must ensure that multixact members do not
   * wrap.  Limits for that are set in SetOffsetVacuumLimit, not here.
   */
  multiWrapLimit = oldest_datminmxid + (MaxMultiXactId >> 1);
  if (multiWrapLimit < FirstMultiXactId)
    multiWrapLimit += FirstMultiXactId;

  /*
   * We'll refuse to continue assigning MultiXactIds once we get within 3M
   * multi of data loss.  See SetTransactionIdLimit.
   */
  multiStopLimit = multiWrapLimit - 3000000;
  if (multiStopLimit < FirstMultiXactId)
    multiStopLimit -= FirstMultiXactId;

  /* Grab lock for just long enough to set the new limit values */
  LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
  MultiXactState->oldestMultiXactId = oldest_datminmxid;
  MultiXactState->oldestMultiXactDB = oldest_datoid;
  MultiXactState->multiStopLimit = multiStopLimit;
  MultiXactState->multiWrapLimit = multiWrapLimit;
  LWLockRelease(MultiXactGenLock);

  /* Log the info */
  ereport(DEBUG1,
          (errmsg_internal(
              "MultiXactId wrap limit is %u, limited by database with OID %u",
              multiWrapLimit, oldest_datoid)));

  /*
   * Computing the actual limits is only possible once the data directory is
   * in a consistent state. There's no need to compute the limits while
   * still replaying WAL - no decisions about new multis are made even
   * though multixact creations might be replayed. So we'll only do further
   * checks after TrimMultiXact() has been called.
   */
  if (!MultiXactState->finishedStartup)
    return;

  /* Set limits for offset vacuum. */
  SetOffsetVacuumLimit(is_startup);
}

/*
 * Make sure that MultiXactOffset has room for a newly-allocated MultiXactId.
 *
 * NB: this is called while holding MultiXactGenLock.  We want it to be very
 * fast most of the time; even when it's not so fast, no actual I/O need
 * happen unless we're forced to write out a dirty log or xlog page to make
 * room in shared memory.
 */
static void ExtendMultiXactOffset(MultiXactId multi) {
  int64 pageno;
  LWLock *lock;

  /*
   * No work except at first MultiXactId of a page.  But beware: just after
   * wraparound, the first MultiXactId of page zero is FirstMultiXactId.
   */
  if (MultiXactIdToOffsetEntry(multi) != 0 && multi != FirstMultiXactId)
    return;

  pageno = MultiXactIdToOffsetPage(multi);
  lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);

  LWLockAcquire(lock, LW_EXCLUSIVE);

  /* Zero the page */
  ZeroMultiXactOffsetPage(pageno);

  LWLockRelease(lock);
}

/*
 * Make sure that MultiXactMember has room for the members of a newly-
 * allocated MultiXactId.
 *
 * Like the above routine, this is called while holding MultiXactGenLock;
 * same comments apply.
 */
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers) {
  /*
   * It's possible that the members span more than one page of the members
   * file, so we loop to ensure we consider each page.  The coding is not
   * optimal if the members span several pages, but that seems unusual
   * enough to not worry much about.
   */
  while (nmembers > 0) {
    int flagsoff;
    int flagsbit;
    uint32 difference;

    /*
     * Only zero when at first entry of a page.
     */
    flagsoff = MXOffsetToFlagsOffset(offset);
    flagsbit = MXOffsetToFlagsBitShift(offset);
    if (flagsoff == 0 && flagsbit == 0) {
      int64 pageno;
      LWLock *lock;

      pageno = MXOffsetToMemberPage(offset);
      lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);

      LWLockAcquire(lock, LW_EXCLUSIVE);

      /* Zero the page */
      ZeroMultiXactMemberPage(pageno);

      LWLockRelease(lock);
    }

    /*
     * Compute the number of items till end of current page.  Careful: if
     * addition of unsigned ints wraps around, we're at the last page of
     * the last segment; since that page holds a different number of items
     * than other pages, we need to do it differently.
     */
    if (offset + MAX_MEMBERS_IN_LAST_MEMBERS_PAGE < offset) {
      /*
       * This is the last page of the last segment; we can compute the
       * number of items left to allocate in it without modulo
       * arithmetic.
       */
      difference = MaxMultiXactOffset - offset + 1;
    } else
      difference =
          MULTIXACT_MEMBERS_PER_PAGE - offset % MULTIXACT_MEMBERS_PER_PAGE;

    /*
     * Advance to next page, taking care to properly handle the wraparound
     * case.  OK if nmembers goes negative.
     */
    nmembers -= difference;
    offset += difference;
  }
}

/*
 * GetOldestMultiXactId
 *
 * Return the oldest MultiXactId that's still possibly still seen as live by
 * any running transaction.  Older ones might still exist on disk, but they no
 * longer have any running member transaction.
 *
 * It's not safe to truncate MultiXact SLRU segments on the value returned by
 * this function; however, it can be set as the new relminmxid for any table
 * that VACUUM knows has no remaining MXIDs < the same value.  It is only safe
 * to truncate SLRUs when no table can possibly still have a referencing MXID.
 */
MultiXactId GetOldestMultiXactId(void) {
  MultiXactId oldestMXact;
  MultiXactId nextMXact;

  /*
   * This is the oldest valid value among all the OldestMemberMXactId[] and
   * OldestVisibleMXactId[] entries, or nextMXact if none are valid.
   */
  LWLockAcquire(MultiXactGenLock, LW_SHARED);

  /*
   * We have to beware of the possibility that nextMXact is in the
   * wrapped-around state.  We don't fix the counter itself here, but we
   * must be sure to use a valid value in our calculation.
   */
  nextMXact = MultiXactState->nextMXact;
  if (nextMXact < FirstMultiXactId)
    nextMXact = FirstMultiXactId;

  oldestMXact = nextMXact;
  for (int i = 0; i < NumMemberSlots; i++) {
    MultiXactId thisoldest;

    thisoldest = OldestMemberMXactId[i];
    if (MultiXactIdIsValid(thisoldest) &&
        MultiXactIdPrecedes(thisoldest, oldestMXact))
      oldestMXact = thisoldest;
  }
  for (int i = 0; i < NumVisibleSlots; i++) {
    MultiXactId thisoldest;

    thisoldest = OldestVisibleMXactId[i];
    if (MultiXactIdIsValid(thisoldest) &&
        MultiXactIdPrecedes(thisoldest, oldestMXact))
      oldestMXact = thisoldest;
  }

  LWLockRelease(MultiXactGenLock);

  return oldestMXact;
}

/*
 * Determine how aggressively we need to vacuum in order to prevent member
 * wraparound.
 *
 * To do so determine what's the oldest member offset and install the limit
 * info in MultiXactState, where it can be used to prevent overrun of old data
 * in the members SLRU area.
 */
static void SetOffsetVacuumLimit(bool is_startup) {
  MultiXactId oldestMultiXactId;
  MultiXactId nextMXact;
  MultiXactOffset oldestOffset = 0; /* placate compiler */
  MultiXactOffset prevOldestOffset;
  MultiXactOffset nextOffset;
  bool oldestOffsetKnown = false;
  bool prevOldestOffsetKnown;
  MultiXactOffset offsetStopLimit = 0;
  MultiXactOffset prevOffsetStopLimit;

  /*
   * NB: Have to prevent concurrent truncation, we might otherwise try to
   * lookup an oldestMulti that's concurrently getting truncated away.
   */
  LWLockAcquire(MultiXactTruncationLock, LW_SHARED);

  /* Read relevant fields from shared memory. */
  LWLockAcquire(MultiXactGenLock, LW_SHARED);
  oldestMultiXactId = MultiXactState->oldestMultiXactId;
  nextMXact = MultiXactState->nextMXact;
  nextOffset = MultiXactState->nextOffset;
  prevOldestOffsetKnown = MultiXactState->oldestOffsetKnown;
  prevOldestOffset = MultiXactState->oldestOffset;
  prevOffsetStopLimit = MultiXactState->offsetStopLimit;
  Assert(MultiXactState->finishedStartup);
  LWLockRelease(MultiXactGenLock);

  /*
   * Determine the offset of the oldest multixact.  Normally, we can read
   * the offset from the multixact itself, but there's an important special
   * case: if there are no multixacts in existence at all, oldestMXact
   * obviously can't point to one.  It will instead point to the multixact
   * ID that will be assigned the next time one is needed.
   */
  if (oldestMultiXactId == nextMXact) {
    /*
     * When the next multixact gets created, it will be stored at the next
     * offset.
     */
    oldestOffset = nextOffset;
    oldestOffsetKnown = true;
  } else {
    /*
     * Figure out where the oldest existing multixact's offsets are
     * stored. Due to bugs in early release of PostgreSQL 9.3.X and 9.4.X,
     * the supposedly-earliest multixact might not really exist.  We are
     * careful not to fail in that case.
     */
    oldestOffsetKnown = find_multixact_start(oldestMultiXactId, &oldestOffset);

    if (oldestOffsetKnown)
      ereport(DEBUG1,
              (errmsg_internal("oldest MultiXactId member is at offset %u",
                               oldestOffset)));
    else
      ereport(
          LOG,
          (errmsg(
              "MultiXact member wraparound protections are disabled because "
              "oldest checkpointed MultiXact %u does not exist on disk",
              oldestMultiXactId)));
  }

  LWLockRelease(MultiXactTruncationLock);

  /*
   * If we can, compute limits (and install them MultiXactState) to prevent
   * overrun of old data in the members SLRU area. We can only do so if the
   * oldest offset is known though.
   */
  if (oldestOffsetKnown) {
    /* move back to start of the corresponding segment */
    offsetStopLimit =
        oldestOffset -
        (oldestOffset % (MULTIXACT_MEMBERS_PER_PAGE * SLRU_PAGES_PER_SEGMENT));

    /* always leave one segment before the wraparound point */
    offsetStopLimit -= (MULTIXACT_MEMBERS_PER_PAGE * SLRU_PAGES_PER_SEGMENT);

    if (!prevOldestOffsetKnown && !is_startup)
      ereport(
          LOG,
          (errmsg("MultiXact member wraparound protections are now enabled")));

    ereport(DEBUG1,
            (errmsg_internal(
                "MultiXact member stop limit is now %u based on MultiXact %u",
                offsetStopLimit, oldestMultiXactId)));
  } else if (prevOldestOffsetKnown) {
    /*
     * If we failed to get the oldest offset this time, but we have a
     * value from a previous pass through this function, use the old
     * values rather than leaving the stop limit unknown.
     */
    oldestOffset = prevOldestOffset;
    oldestOffsetKnown = true;
    offsetStopLimit = prevOffsetStopLimit;
  }

  /* Install the computed values */
  LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
  MultiXactState->oldestOffset = oldestOffset;
  MultiXactState->oldestOffsetKnown = oldestOffsetKnown;
  MultiXactState->offsetStopLimit = offsetStopLimit;
  LWLockRelease(MultiXactGenLock);
}

/*
 * Return whether adding "distance" to "start" would move past "boundary".
 *
 * We use this to determine whether the addition is "wrapping around" the
 * boundary point, hence the name.  The reason we don't want to use the regular
 * 2^31-modulo arithmetic here is that we want to be able to use the whole of
 * the 2^32-1 space here, allowing for more multixacts than would fit
 * otherwise.
 */
static bool MultiXactOffsetWouldWrap(MultiXactOffset boundary,
                                     MultiXactOffset start, uint32 distance) {
  MultiXactOffset finish;

  /*
   * Note that offset number 0 is not used (see GetMultiXactIdMembers), so
   * if the addition wraps around the UINT_MAX boundary, skip that value.
   */
  finish = start + distance;
  if (finish < start)
    finish++;

  /*-----------------------------------------------------------------------
   * When the boundary is numerically greater than the starting point, any
   * value numerically between the two is not wrapped:
   *
   *	<----S----B---->
   *	[---)			 = F wrapped past B (and UINT_MAX)
   *		 [---)		 = F not wrapped
   *			  [----] = F wrapped past B
   *
   * When the boundary is numerically less than the starting point (i.e. the
   * UINT_MAX wraparound occurs somewhere in between) then all values in
   * between are wrapped:
   *
   *	<----B----S---->
   *	[---)			 = F not wrapped past B (but wrapped past
   *UINT_MAX)
   *		 [---)		 = F wrapped past B (and UINT_MAX)
   *			  [----] = F not wrapped
   *-----------------------------------------------------------------------
   */
  if (start < boundary)
    return finish >= boundary || finish < start;
  else
    return finish >= boundary && finish < start;
}

/*
 * Find the starting offset of the given MultiXactId.
 *
 * Returns false if the file containing the multi does not exist on disk.
 * Otherwise, returns true and sets *result to the starting member offset.
 *
 * This function does not prevent concurrent truncation, so if that's
 * required, the caller has to protect against that.
 */
static bool find_multixact_start(MultiXactId multi, MultiXactOffset *result) {
  MultiXactOffset offset;
  int64 pageno;
  int entryno;
  int slotno;
  MultiXactOffset *offptr;

  Assert(MultiXactState->finishedStartup);

  pageno = MultiXactIdToOffsetPage(multi);
  entryno = MultiXactIdToOffsetEntry(multi);

  /*
   * Write out dirty data, so PhysicalPageExists can work correctly.
   */
  SimpleLruWriteAll(MultiXactOffsetCtl, true);
  SimpleLruWriteAll(MultiXactMemberCtl, true);

  if (!SimpleLruDoesPhysicalPageExist(MultiXactOffsetCtl, pageno))
    return false;

  /* lock is acquired by SimpleLruReadPage_ReadOnly */
  slotno = SimpleLruReadPage_ReadOnly(MultiXactOffsetCtl, pageno, multi);
  offptr = (MultiXactOffset *)MultiXactOffsetCtl->shared->page_buffer[slotno];
  offptr += entryno;
  offset = *offptr;
  LWLockRelease(SimpleLruGetBankLock(MultiXactOffsetCtl, pageno));

  *result = offset;
  return true;
}

typedef struct mxtruncinfo {
  int64 earliestExistingPage;
} mxtruncinfo;

/*
 * Decide whether a MultiXactOffset page number is "older" for truncation
 * purposes.  Analogous to CLOGPagePrecedes().
 *
 * Offsetting the values is optional, because MultiXactIdPrecedes() has
 * translational symmetry.
 */
static bool MultiXactOffsetPagePrecedes(int64 page1, int64 page2) {
  MultiXactId multi1;
  MultiXactId multi2;

  multi1 = ((MultiXactId)page1) * MULTIXACT_OFFSETS_PER_PAGE;
  multi1 += FirstMultiXactId + 1;
  multi2 = ((MultiXactId)page2) * MULTIXACT_OFFSETS_PER_PAGE;
  multi2 += FirstMultiXactId + 1;

  return (MultiXactIdPrecedes(multi1, multi2) &&
          MultiXactIdPrecedes(multi1, multi2 + MULTIXACT_OFFSETS_PER_PAGE - 1));
}

/*
 * Decide whether a MultiXactMember page number is "older" for truncation
 * purposes.  There is no "invalid offset number" so use the numbers verbatim.
 */
static bool MultiXactMemberPagePrecedes(int64 page1, int64 page2) {
  MultiXactOffset offset1;
  MultiXactOffset offset2;

  offset1 = ((MultiXactOffset)page1) * MULTIXACT_MEMBERS_PER_PAGE;
  offset2 = ((MultiXactOffset)page2) * MULTIXACT_MEMBERS_PER_PAGE;

  return (MultiXactOffsetPrecedes(offset1, offset2) &&
          MultiXactOffsetPrecedes(offset1,
                                  offset2 + MULTIXACT_MEMBERS_PER_PAGE - 1));
}

/*
 * Decide which of two MultiXactIds is earlier.
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2) {
  int32 diff = (int32)(multi1 - multi2);

  return (diff < 0);
}

/*
 * MultiXactIdPrecedesOrEquals -- is multi1 logically <= multi2?
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool MultiXactIdPrecedesOrEquals(MultiXactId multi1, MultiXactId multi2) {
  int32 diff = (int32)(multi1 - multi2);

  return (diff <= 0);
}

/*
 * Decide which of two offsets is earlier.
 */
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
                                    MultiXactOffset offset2) {
  int32 diff = (int32)(offset1 - offset2);

  return (diff < 0);
}

Datum pg_get_multixact_members(PG_FUNCTION_ARGS) {
  typedef struct {
    MultiXactMember *members;
    int nmembers;
    int iter;
  } mxact;
  MultiXactId mxid = PG_GETARG_TRANSACTIONID(0);
  mxact *multi;
  FuncCallContext *funccxt;

  if (mxid < FirstMultiXactId)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid MultiXactId: %u", mxid)));

  if (SRF_IS_FIRSTCALL()) {
    MemoryContext oldcxt;
    TupleDesc tupdesc;

    funccxt = SRF_FIRSTCALL_INIT();
    oldcxt = MemoryContextSwitchTo(funccxt->multi_call_memory_ctx);

    multi = (mxact *)palloc(sizeof(mxact));
    /* no need to allow for old values here */
    multi->nmembers =
        GetMultiXactIdMembers(mxid, &multi->members, false, false);
    multi->iter = 0;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      elog(ERROR, "return type must be a row type");
    funccxt->tuple_desc = tupdesc;
    funccxt->attinmeta = TupleDescGetAttInMetadata(tupdesc);
    funccxt->user_fctx = multi;

    MemoryContextSwitchTo(oldcxt);
  }

  funccxt = SRF_PERCALL_SETUP();
  multi = (mxact *)funccxt->user_fctx;

  while (multi->iter < multi->nmembers) {
    HeapTuple tuple;
    char *values[2];

    values[0] = psprintf("%u", multi->members[multi->iter].xid);
    values[1] = mxstatus_to_string(multi->members[multi->iter].status);

    tuple = BuildTupleFromCStrings(funccxt->attinmeta, values);

    multi->iter++;
    pfree(values[0]);
    SRF_RETURN_NEXT(funccxt, HeapTupleGetDatum(tuple));
  }

  SRF_RETURN_DONE(funccxt);
}
