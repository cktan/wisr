/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/transam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "access/xlogdefs.h"

/* ----------------
 *		Special transaction ID values
 *
 * BootstrapTransactionId is the XID for "bootstrap" operations, and
 * FrozenTransactionId is used for very old tuples.  Both should
 * always be considered valid.
 *
 * FirstNormalTransactionId is the first "normal" transaction id.
 * Note: if you need to change it, you must change pg_class.h as well.
 * ----------------
 */
#define InvalidTransactionId		((TransactionId) 0)
/* No multixact: a row in sm never names a set of lockers (DESIGN.md §4). */
#define InvalidMultiXactId		((MultiXactId) 0)
#define BootstrapTransactionId		((TransactionId) 1)
#define FrozenTransactionId			((TransactionId) 2)
#define FirstNormalTransactionId	((TransactionId) 3)
#define MaxTransactionId			((TransactionId) 0xFFFFFFFF)

/* ----------------
 *		transaction ID manipulation macros
 * ----------------
 */
#define TransactionIdIsValid(xid)		((xid) != InvalidTransactionId)
#define TransactionIdIsNormal(xid)		((xid) >= FirstNormalTransactionId)
#define TransactionIdEquals(id1, id2)	((id1) == (id2))
#define TransactionIdStore(xid, dest)	(*(dest) = (xid))
#define StoreInvalidTransactionId(dest) (*(dest) = InvalidTransactionId)

#define EpochFromFullTransactionId(x)	((uint32) ((x).value >> 32))
#define XidFromFullTransactionId(x)		((uint32) (x).value)
#define U64FromFullTransactionId(x)		((x).value)
#define FullTransactionIdEquals(a, b)	((a).value == (b).value)
#define FullTransactionIdPrecedes(a, b)	((a).value < (b).value)
#define FullTransactionIdPrecedesOrEquals(a, b) ((a).value <= (b).value)
#define FullTransactionIdFollows(a, b) ((a).value > (b).value)
#define FullTransactionIdFollowsOrEquals(a, b) ((a).value >= (b).value)
#define FullTransactionIdIsValid(x)		TransactionIdIsValid(XidFromFullTransactionId(x))
#define InvalidFullTransactionId		FullTransactionIdFromEpochAndXid(0, InvalidTransactionId)
#define FirstNormalFullTransactionId	FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId)
#define FullTransactionIdIsNormal(x)	FullTransactionIdFollowsOrEquals(x, FirstNormalFullTransactionId)

/*
 * A 64 bit value that contains an epoch and a TransactionId.  This is
 * wrapped in a struct to prevent implicit conversion to/from TransactionId.
 * Not all values represent valid normal XIDs.
 */
typedef struct FullTransactionId {
  uint64 value;
} FullTransactionId;

static inline FullTransactionId
FullTransactionIdFromEpochAndXid(uint32 epoch, TransactionId xid) {
  FullTransactionId result;

  result.value = ((uint64)epoch) << 32 | xid;

  return result;
}

static inline FullTransactionId FullTransactionIdFromU64(uint64 value) {
  FullTransactionId result;

  result.value = value;

  return result;
}

/* advance a transaction ID variable, handling wraparound correctly */
#define TransactionIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalTransactionId) \
			(dest) = FirstNormalTransactionId; \
	} while(0)

/*
 * Retreat a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 */
static inline void FullTransactionIdRetreat(FullTransactionId *dest) {
  dest->value--;

  /*
   * In contrast to 32bit XIDs don't step over the "actual" special xids.
   * For 64bit xids these can't be reached as part of a wraparound as they
   * can in the 32bit case.
   */
  if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
    return;

  /*
   * But we do need to step over XIDs that'd appear special only for 32bit
   * XIDs.
   */
  while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
    dest->value--;
}

/*
 * Advance a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 */
static inline void FullTransactionIdAdvance(FullTransactionId *dest) {
  dest->value++;

  /* see FullTransactionIdAdvance() */
  if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
    return;

  while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
    dest->value++;
}

/* back up a transaction ID variable, handling wraparound correctly */
#define TransactionIdRetreat(dest)	\
	do { \
		(dest)--; \
	} while ((dest) < FirstNormalTransactionId)

/* compare two XIDs already known to be normal; this is a macro for speed */
#define NormalTransactionIdPrecedes(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) < 0)

/* compare two XIDs already known to be normal; this is a macro for speed */
#define NormalTransactionIdFollows(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) > 0)

/* ----------
 *		Object ID (OID) zero is InvalidOid.
 *
 *		OIDs 1-9999 are reserved for manual assignment (see .dat files
 *in src/include/catalog/).  Of these, 8000-9999 are reserved for development
 *purposes (such as in-progress patches and forks); they should not appear in
 *released versions.
 *
 *		OIDs 10000-11999 are reserved for assignment by genbki.pl, for
 *use when the .dat files in src/include/catalog/ do not specify an OID for a
 *catalog entry that requires one.  Note that genbki.pl assigns these OIDs
 *independently in each catalog, so they're not guaranteed to be globally
 *unique.  Furthermore, the bootstrap backend and initdb's post-bootstrap
 *processing can also assign OIDs in this range. The normal OID-generation logic
 *takes care of any OID conflicts that might arise from that.
 *
 *		OIDs 12000-16383 are reserved for unpinned objects created by
 *initdb's post-bootstrap processing.  initdb forces the OID generator up to
 *		12000 as soon as it's made the pinned objects it's responsible
 *for.
 *
 *		OIDs beginning at 16384 are assigned from the OID generator
 *		during normal multiuser operation.  (We force the generator up
 *to 16384 as soon as we are in normal operation.)
 *
 * The choices of 8000, 10000 and 12000 are completely arbitrary, and can be
 * moved if we run low on OIDs in any category.  Changing the macros below,
 * and updating relevant documentation (see bki.sgml and RELEASE_CHANGES),
 * should be sufficient to do this.  Moving the 16384 boundary between
 * initdb-assigned OIDs and user-defined objects would be substantially
 * more painful, however, since some user-defined OIDs will appear in
 * on-disk data; such a change would probably break pg_upgrade.
 *
 * NOTE: if the OID generator wraps around, we skip over OIDs 0-16383
 * and resume with 16384.  This minimizes the odds of OID conflict, by not
 * reassigning OIDs that might have been assigned during initdb.  Critically,
 * it also ensures that no user-created object will be considered pinned.
 * ----------
 */
#define FirstGenbkiObjectId		10000
#define FirstUnpinnedObjectId	12000
#define FirstNormalObjectId		16384

/*
 * prototypes for functions in transam/transam.c
 */
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern bool TransactionIdIsInProgress(TransactionId xid);
extern bool TransactionIdPrecedes(TransactionId id1, TransactionId id2);
extern bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollows(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2);

/*
 * Some frontend programs include this header.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide them with #ifndef FRONTEND.
 */
#ifndef FRONTEND

/* return transaction ID backed up by amount, handling wraparound correctly */
static inline TransactionId TransactionIdRetreatedBy(TransactionId xid,
                                                     uint32 amount) {
  xid -= amount;

  while (xid < FirstNormalTransactionId)
    xid--;

  return xid;
}

/* return the older of the two IDs */
static inline TransactionId TransactionIdOlder(TransactionId a,
                                               TransactionId b) {
  if (!TransactionIdIsValid(a))
    return b;

  if (!TransactionIdIsValid(b))
    return a;

  if (TransactionIdPrecedes(a, b))
    return a;
  return b;
}

/* return the older of the two IDs, assuming they're both normal */
static inline TransactionId NormalTransactionIdOlder(TransactionId a,
                                                     TransactionId b) {
  Assert(TransactionIdIsNormal(a));
  Assert(TransactionIdIsNormal(b));
  if (NormalTransactionIdPrecedes(a, b))
    return a;
  return b;
}

/* return the newer of the two IDs */
static inline FullTransactionId FullTransactionIdNewer(FullTransactionId a,
                                                       FullTransactionId b) {
  if (!FullTransactionIdIsValid(a))
    return b;

  if (!FullTransactionIdIsValid(b))
    return a;

  if (FullTransactionIdFollows(a, b))
    return a;
  return b;
}

#endif /* FRONTEND */

#endif /* TRANSAM_H */
