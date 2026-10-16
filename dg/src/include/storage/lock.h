/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#ifdef FRONTEND
#error "lock.h may not be included from frontend code"
#endif

#include "lib/ilist.h"
#include "storage/lockdefs.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* struct PGPROC is declared in proc.h, but must forward-reference it */
typedef struct PGPROC PGPROC;

/*
 * Top-level transactions are identified by VirtualTransactionIDs comprising
 * PGPROC fields procNumber and lxid.  For recovered prepared transactions, the
 * LocalTransactionId is an ordinary XID; LOCKTAG_VIRTUALTRANSACTION never
 * refers to that kind.  These are guaranteed unique over the short term, but
 * will be reused after a database restart or XID wraparound; hence they
 * should never be stored on disk.
 *
 * Note that struct VirtualTransactionId can not be assumed to be atomically
 * assignable as a whole.  However, type LocalTransactionId is assumed to
 * be atomically assignable, and the proc number doesn't change often enough
 * to be a problem, so we can fetch or assign the two fields separately.
 * We deliberately refrain from using the struct within PGPROC, to prevent
 * coding errors from trying to use struct assignment with it.
 */
typedef struct {
  ProcNumber procNumber;                 /* proc number of the PGPROC */
  LocalTransactionId localTransactionId; /* lxid from PGPROC */
} VirtualTransactionId;

#define InvalidLocalTransactionId		0
#define LocalTransactionIdIsValid(lxid) ((lxid) != InvalidLocalTransactionId)
#define VirtualTransactionIdIsValid(vxid) \
	(LocalTransactionIdIsValid((vxid).localTransactionId))

/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */
#define MAX_LOCKMODES		10

#define LOCKBIT_ON(lockmode) (1 << (lockmode))

/*
 * Lock methods are identified by LOCKMETHODID.  (Despite the declaration as
 * uint16, we are constrained to 256 lockmethods by the layout of LOCKTAG.)
 */
typedef uint16 LOCKMETHODID;

/* These identify the known lock methods */
#define DEFAULT_LOCKMETHOD	1

/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.  A LOCKTAG value uniquely identifies a lockable object.
 *
 * The LockTagType enum defines the different kinds of objects we can lock.
 * We can handle up to 256 different LockTagTypes.
 */
typedef enum LockTagType {
  LOCKTAG_RELATION,            /* whole relation */
  LOCKTAG_RELATION_EXTEND,     /* the right to extend a relation */
  LOCKTAG_DATABASE_FROZEN_IDS, /* pg_database.datfrozenxid */
  LOCKTAG_PAGE,                /* one page of a relation */
  LOCKTAG_TUPLE,               /* one physical tuple */
  LOCKTAG_TRANSACTION,         /* transaction (for waiting for xact done) */
  LOCKTAG_VIRTUALTRANSACTION,  /* virtual transaction (ditto) */
  LOCKTAG_SPECULATIVE_TOKEN,   /* speculative insertion Xid and token */
  LOCKTAG_OBJECT,              /* non-relation database object */
  LOCKTAG_USERLOCK,            /* reserved for old contrib/userlock code */
  LOCKTAG_ADVISORY,            /* advisory user locks */
  LOCKTAG_APPLY_TRANSACTION,   /* transaction being applied on a logical
                                * replication subscriber */
} LockTagType;

/*
 * The LOCKTAG struct is defined with malice aforethought to fit into 16
 * bytes with no padding.  Note that this would need adjustment if we were
 * to widen Oid, BlockNumber, or TransactionId to more than 32 bits.
 *
 * We include lockmethodid in the locktag so that a single hash table in
 * shared memory can store locks of different lockmethods.
 */
typedef struct LOCKTAG {
  uint32 locktag_field1;      /* a 32-bit ID field */
  uint32 locktag_field2;      /* a 32-bit ID field */
  uint32 locktag_field3;      /* a 32-bit ID field */
  uint16 locktag_field4;      /* a 16-bit ID field */
  uint8 locktag_type;         /* see enum LockTagType */
  uint8 locktag_lockmethodid; /* lockmethod indicator */
} LOCKTAG;

/*
 * These macros define how we map logical IDs of lockable objects into
 * the physical fields of LOCKTAG.  Use these to set up LOCKTAG values,
 * rather than accessing the fields directly.  Note multiple eval of target!
 */

/* ID info for a relation is DB OID + REL OID; DB OID = 0 if shared */
#define SET_LOCKTAG_RELATION(locktag, dboid, reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* Whether a lock in mode1 conflicts with one in mode2 (lmgr.c). */
extern bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2);

#endif /* LOCK_H_ */
