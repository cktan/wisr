/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"

/*
 * A snapshot's array of subtransaction XIDs is sized by this for each proc,
 * as PostgreSQL sized each PGPROC's cache of them; digres keeps no cache, as
 * no XID is ever assigned, and a snapshot's array stays empty.
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64

/*
 * Flags for PGPROC->statusFlags and PROC_HDR->statusFlags[]
 */
#define PROC_IS_AUTOVACUUM	0x01         /* is it an autovac worker? */
#define PROC_IN_VACUUM		0x02             /* currently running lazy vacuum */
#define PROC_IN_SAFE_IC		0x04                                  /* currently running CREATE INDEX      \
                                         * CONCURRENTLY or REINDEX             \
                                         * CONCURRENTLY on non-expressional,   \
                                         * non-partial index */
#define PROC_VACUUM_FOR_WRAPAROUND	0x08 /* set by autovac only */
#define PROC_IN_LOGICAL_DECODING	0x10                                  /* currently doing logical             \
                                         * decoding outside xact */
#define PROC_AFFECTS_ALL_HORIZONS	0x20                                  /* this proc's xmin must be            \
                                         * included in vacuum horizons         \
                                         * in all databases */

/* flags reset at EOXact */
#define PROC_VACUUM_STATE_MASK \
	(PROC_IN_VACUUM | PROC_IN_SAFE_IC | PROC_VACUUM_FOR_WRAPAROUND)

/*
 * Xmin-related flags. Make sure any flags that affect how the process' Xmin
 * value is interpreted by VACUUM are included here.
 */
#define PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)

/*
 * We allow a limited number of "weak" relation locks (AccessShareLock,
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure
 * (or rather in shared memory referenced from PGPROC) rather than the main
 * lock table.  This eases contention on the lock manager LWLocks.  See
 * storage/lmgr/README for additional details.
 */
extern thread_local /*SHARED*/ PGDLLIMPORT int FastPathLockGroupsPerBackend;

/*
 * Define the maximum number of fast-path locking groups per backend.
 * This must be a power-of-two value.  The actual number of fast-path
 * lock groups is calculated in InitializeFastPathLocks() based on
 * max_locks_per_transaction.  1024 is an arbitrary upper limit (matching
 * max_locks_per_transaction = 16k).  Values over 1024 are unlikely to be
 * beneficial as there are bottlenecks we'll hit way before that.
 */
#define FP_LOCK_GROUPS_PER_BACKEND_MAX	1024
#define FP_LOCK_SLOTS_PER_GROUP		16 /* don't change */
#define FastPathLockSlotsPerBackend() \
	(FP_LOCK_SLOTS_PER_GROUP * FastPathLockGroupsPerBackend)

/*
 * Flags for PGPROC.delayChkptFlags
 *
 * These flags can be used to delay the start or completion of a checkpoint
 * for short periods. A flag is in effect if the corresponding bit is set in
 * the PGPROC of any backend.
 *
 * For our purposes here, a checkpoint has three phases: (1) determine the
 * location to which the redo pointer will be moved, (2) write all the
 * data durably to disk, and (3) WAL-log the checkpoint.
 *
 * Setting DELAY_CHKPT_START prevents the system from moving from phase 1
 * to phase 2. This is useful when we are performing a WAL-logged modification
 * of data that will be flushed to disk in phase 2. By setting this flag
 * before writing WAL and clearing it after we've both written WAL and
 * performed the corresponding modification, we ensure that if the WAL record
 * is inserted prior to the new redo point, the corresponding data changes will
 * also be flushed to disk before the checkpoint can complete. (In the
 * extremely common case where the data being modified is in shared buffers
 * and we acquire an exclusive content lock and MarkBufferDirty() on the
 * relevant buffers before writing WAL, this mechanism is not needed, because
 * phase 2 will block until we release the content lock and then flush the
 * modified data to disk.  See transam/README and SyncOneBuffer().)
 *
 * Setting DELAY_CHKPT_COMPLETE prevents the system from moving from phase 2
 * to phase 3. This is useful if we are performing a WAL-logged operation that
 * might invalidate buffers, such as relation truncation. In this case, we need
 * to ensure that any buffers which were invalidated and thus not flushed by
 * the checkpoint are actually destroyed on disk. Replay can cope with a file
 * or block that doesn't exist, but not with a block that has the wrong
 * contents.
 */
#define DELAY_CHKPT_START		(1<<0)
#define DELAY_CHKPT_COMPLETE	(1<<1)

typedef enum {
  PROC_WAIT_STATUS_OK,
  PROC_WAIT_STATUS_WAITING,
  PROC_WAIT_STATUS_ERROR,
} ProcWaitStatus;

/*
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PGPROC is in.  When waiting for a lock,
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC
 * is linked into ProcGlobal's freeProcs list.
 *
 * What another session reads of a session is in its record
 * (storage/session_registry.h), not here: a PGPROC is a slot, which the next
 * session takes over (dg/SHMEM.md section 5.2).
 */
/*
 * lwWaiting is handed from the process that wakes a waiter to the waiter
 * without a lock: a release store and an acquire load, which order the wait
 * list links around it, as the barriers did, for a thread sanitizer too.
 */
#define LWWAITING_GET(proc) __atomic_load_n(&(proc)->lwWaiting, __ATOMIC_ACQUIRE)
#define LWWAITING_SET(proc, value) \
	__atomic_store_n(&(proc)->lwWaiting, (value), __ATOMIC_RELEASE)

struct PGPROC {
  dlist_node links;           /* list link if process is in a list */
  dlist_head *procgloballist; /* procglobal list that owns this PGPROC */

  PGSemaphore sem; /* ONE semaphore to sleep on */
  ProcWaitStatus waitStatus;

  Latch procLatch; /* generic latch for process */

  TransactionId xid; /* id of top-level transaction currently being
                      * executed by this proc, if running and XID
                      * is assigned; else InvalidTransactionId. */

  TransactionId xmin; /* minimal running XID as it was when we were
                       * starting our xact, excluding LAZY VACUUM:
                       * vacuum must not remove tuples deleted by
                       * xid >= xmin ! */

  int pid; /* Backend's process ID; 0 if prepared xact */

  /*
   * The session's cancel key, which a cancel request must match, and its
   * length, 0 for none: set and read under pcb->cancel_key_mutex
   * (ProcSetCancelKey(), SendCancelRequest()).
   */
  int cancelKeyLength;
  uint8 cancelKey[MAX_CANCEL_KEY_LENGTH];

  /*
   * Currently running top-level transaction's virtual xid. Together these
   * form a VirtualTransactionId, but we don't use that struct because this
   * is not atomically assignable as whole, and we want to enforce code to
   * consider both parts separately.  See comments at VirtualTransactionId.
   */
  struct {
    ProcNumber procNumber;   /* For regular backends, equal to
                              * GetNumberFromPGProc(proc).  For prepared
                              * xacts, ID of the original backend that
                              * processed the transaction. For unused
                              * PGPROC entries, INVALID_PROC_NUMBER. */
    LocalTransactionId lxid; /* local id of top-level transaction
                              * currently * being executed by this
                              * proc, if running; else
                              * InvalidLocalTransactionId */
  } vxid;

  /* These fields are zero while a backend is still starting up: */
  Oid databaseId; /* OID of database this backend is using */
  Oid roleId;     /* OID of role using this backend */

  Oid tempNamespaceId; /* OID of temp schema this backend is
                        * using */

  bool isRegularBackend; /* true if it's a regular backend. */

  /* Info about LWLock the process is currently waiting for, if any. */
  uint8 lwWaiting;          /* see LWLockWaitState; LWWAITING_GET/SET */
  uint8 lwWaitMode;         /* lwlock mode being waited for */
  proclist_node lwWaitLink; /* position in LW lock wait list */

  /* Info about lock the process is currently waiting for, if any. */
  /* waitLock and waitProcLock are NULL if not currently waiting. */
  LOCK *waitLock;             /* Lock object we're sleeping on ... */
  PROCLOCK *waitProcLock;     /* Per-holder info for awaited lock */
  LOCKMODE waitLockMode;      /* type of lock we're waiting for */
  LOCKMASK heldLocks;         /* bitmask for lock types already held on this
                               * lock object by this backend */
  pg_atomic_uint64 waitStart; /* time at which wait for lock acquisition
                               * started */

  int delayChkptFlags; /* for DELAY_CHKPT_* flags */

  uint8 statusFlags; /* this backend's status flags, see PROC_*
                      * above */

  /*
   * All PROCLOCK objects for locks held or awaited by this backend are
   * linked into one of these lists, according to the partition number of
   * their lock.
   */
  dlist_head myProcLocks[NUM_LOCK_PARTITIONS];

  /* Lock manager data, recording fast-path locks taken by this backend. */
  LWLock fpInfoLock;  /* protects per-backend fast-path state */
  uint64 *fpLockBits; /* lock modes held for each fast-path slot */
  Oid *fpRelId;       /* slots for rel oids */
  bool fpVXIDLock;    /* are we holding a fast-path VXID lock? */
  LocalTransactionId fpLocalTransactionId; /* lxid for fast-path VXID
                                            * lock */

  /*
   * Support for lock groups.  Use LockHashPartitionLockByProc on the group
   * leader to get the LWLock protecting these fields.
   */
  PGPROC *lockGroupLeader;     /* lock group leader, if I'm a member */
  dlist_head lockGroupMembers; /* list of members, if I'm a leader */
  dlist_node lockGroupLink;    /* my member link, if I'm a member */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */

extern thread_local /*LOCAL*/ PGDLLIMPORT PGPROC *MyProc;

/*
 * There is one ProcGlobal struct for the whole database cluster.
 *
 * A session's entry in the procarray is its record in the session registry
 * (storage/session_registry.h).  PostgreSQL mirrored some PGPROC fields into
 * dense arrays here, for its snapshot's scan; digres's snapshot is a
 * constant, and they are gone.
 */
typedef struct PROC_HDR {
  /* Array of PGPROC structures (not including dummies for prepared txns) */
  PGPROC *allProcs;

  /* Length of allProcs array */
  uint32 allProcCount;
  /* Head of list of free PGPROC structures */
  dlist_head freeProcs;

  /* Current shared estimate of appropriate spins_per_delay value */
  int spins_per_delay;
} PROC_HDR;

extern thread_local /*SHARED*/ PGDLLIMPORT PROC_HDR *ProcGlobal;

/*
 * Accessors for getting PGPROC given a ProcNumber and vice versa.
 */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])
#define GetNumberFromPGProc(proc) ((proc) - &ProcGlobal->allProcs[0])

/* configurable options */
extern thread_local /*LOCAL*/ PGDLLIMPORT int DeadlockTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int StatementTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int LockTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int TransactionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleSessionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool log_lock_waits;

/*
 * Function Prototypes
 */
extern int ProcGlobalSemas(void);
extern void InitProcGlobal(void);
extern void ProcSetCancelKey(const uint8 *key, int len);
extern void InitProcess(void);
extern void InitProcessPhase2(void);

extern bool HaveNFreeProcs(int n, int *nfree);
extern void ProcReleaseLocks(bool isCommit);

extern ProcWaitStatus JoinWaitQueue(LOCALLOCK *locallock,
                                    LockMethod lockMethodTable, bool dontWait);
extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock);
extern void ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern void CheckDeadLockAlert(void);
extern void LockErrorCleanup(void);
extern void GetLockHoldersAndWaiters(LOCALLOCK *locallock,
                                     StringInfo lock_holders_sbuf,
                                     StringInfo lock_waiters_sbuf,
                                     int *lockHoldersNum);

extern void ProcWaitForSignal(uint32 wait_event_info);
extern void ProcSendSignal(ProcNumber procNumber);

extern PGPROC *PostmasterPidGetProc(int pid);

#endif /* _PROC_H_ */
