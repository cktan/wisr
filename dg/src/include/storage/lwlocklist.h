/*-------------------------------------------------------------------------
 *
 * lwlocklist.h
 *
 * The predefined LWLock list is kept in its own source file for use by
 * automatic tools.  The exact representation of a keyword is determined by
 * the PG_LWLOCK macro, which is not defined in this file; it can be
 * defined by the caller for special purposes.
 *
 * Also, generate-lwlocknames.pl processes this file to create lwlocknames.h.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/include/storage/lwlocklist.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Some commonly-used locks have predefined positions within MainLWLockArray;
 * these are defined here.  If you add a lock, add it to the end to avoid
 * renumbering the existing locks; if you remove a lock, consider leaving a gap
 * in the numbering sequence for the benefit of DTrace and other external
 * debugging scripts.  Also, do not forget to update the section
 * WaitEventLWLock of src/backend/utils/activity/wait_event_names.txt.
 *
 * Note that the names here don't include the Lock suffix, to appease the
 * C preprocessor; it's added elsewhere.
 */

/* 0 is available; was formerly BufFreelistLock */
/* 1 was ShmemIndexLock */
PG_LWLOCK(4, ProcArray)
PG_LWLOCK(5, SInvalRead)
PG_LWLOCK(6, SInvalWrite)
/* 7 was WALBufMappingLock */
/* 8 was WALWriteLock */
/* 9 was ControlFileLock */
/* 10 was CheckpointLock */
/* 11 was XactSLRULock */
/* 12 was SubtransSLRULock */
/* 13 was MultiXactGenLock */
/* 14 was MultiXactOffsetSLRULock */
/* 15 was MultiXactMemberSLRULock */
/* 17 was CheckpointerComm */
/* 18 was TwoPhaseState */
/* 19 was TablespaceCreateLock */
/* 20 was BtreeVacuumLock */
/* 21 was AddinShmemInitLock */
/* 22 was AutovacuumLock */
/* 23 was AutovacuumScheduleLock */
/* 24 was SyncScan */
/* 26 was NotifySLRULock */
/* 27 was NotifyQueue */
/* 28 was SerializableXactHashLock */
/* 29 was SerializableFinishedListLock */
/* 30 was SerializablePredicateListLock */
/* 31 was SerialSLRULock */
/* 32 was SyncRepLock */
/* 33 was BackgroundWorker */
/* 34 was DynamicSharedMemoryControlLock */
/* 35 was AutoFileLock */
/* 36 was ReplicationSlotAllocationLock */
/* 37 was ReplicationSlotControlLock */
/* 38 was CommitTsSLRULock */
/* 39 was CommitTs */
/* 40 was ReplicationOriginLock */
/* 41 was MultiXactTruncationLock */
/* 42 was OldSnapshotTimeMapLock */
/* 43 was LogicalRepWorkerLock */
/* 45 was XactTruncationLock until removal of BackendRandomLock */
/* 46 was WrapLimitsVacuumLock */
/* 47 was NotifyQueueTail */
/* 48 was WaitEventCustomLock */
/* 49 was WALSummarizerLock */
/* 50 was DSMRegistryLock */
PG_LWLOCK(51, InjectionPoint)
/* 52 was SerialControlLock */
