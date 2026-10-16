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

/*
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PGPROC is in: a recycled PGPROC is
 * linked into ProcGlobal's freeProcs list.
 *
 * What another session reads of a session is in its record
 * (storage/session_registry.h), not here: a PGPROC is a slot, which the next
 * session takes over (dg/SHMEM.md section 5.2).
 */
struct PGPROC {
  dlist_node links;           /* list link if process is in a list */
  dlist_head *procgloballist; /* procglobal list that owns this PGPROC */

  TransactionId xid; /* id of top-level transaction currently being
                      * executed by this proc, if running and XID
                      * is assigned; else InvalidTransactionId. */

  TransactionId xmin; /* minimal running XID as it was when we were
                       * starting our xact, excluding LAZY VACUUM:
                       * vacuum must not remove tuples deleted by
                       * xid >= xmin ! */

  int pid; /* Backend's process ID; 0 if prepared xact */

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

  int delayChkptFlags; /* for DELAY_CHKPT_* flags */

  uint8 statusFlags; /* this backend's status flags, see PROC_*
                      * above */
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
extern thread_local /*LOCAL*/ PGDLLIMPORT int StatementTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int LockTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int TransactionTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT int IdleSessionTimeout;

/*
 * Function Prototypes
 */
extern void InitProcGlobal(void);
extern void InitProcess(void);
extern void InitProcessPhase2(void);

extern bool HaveNFreeProcs(int n, int *nfree);
extern void ProcReleaseLocks(bool isCommit);

#endif /* _PROC_H_ */
