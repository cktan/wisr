/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  code to create and destroy physical storage for relations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/storage.c
 *
 * NOTES
 *	  Some of this code used to be in storage/smgr/smgr.c, and the
 *	  function names still reflect that.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "storage/freespace.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * We keep a list of all relations (represented as RelFileLocator values)
 * that have been created or deleted in the current transaction.  When
 * a relation is created, we create the physical file immediately, but
 * remember it so that we can delete the file again if the current
 * transaction is aborted.  Conversely, a deletion request is NOT
 * executed immediately, but is just entered in the list.  When and if
 * the transaction commits, we can delete the physical file.
 *
 * To handle subtransactions, every entry is marked with its transaction
 * nesting level.  At subtransaction commit, we reassign the subtransaction's
 * entries to the parent nesting level.  At subtransaction abort, we can
 * immediately execute the abort-time actions for all entries of the current
 * nesting level.
 *
 * NOTE: the list is kept in TopMemoryContext to be sure it won't disappear
 * unbetimes.  It'd probably be OK to keep it in TopTransactionContext,
 * but I'm being paranoid.
 */

typedef struct PendingRelDelete {
  RelFileLocator rlocator;       /* relation that may need to be deleted */
  ProcNumber procNumber;         /* INVALID_PROC_NUMBER if not a temp rel */
  bool atCommit;                 /* T=delete at commit; F=delete at abort */
  int nestLevel;                 /* xact nesting level of request */
  struct PendingRelDelete *next; /* linked-list link */
} PendingRelDelete;

typedef struct PendingRelSync {
  RelFileLocator rlocator;
} PendingRelSync;

static thread_local /*LOCAL*/ PendingRelDelete *pendingDeletes =
    NULL; /* head of linked list */
static thread_local /*LOCAL*/ HTAB *pendingSyncHash = NULL;

/*
 * AddPendingSync
 *		Queue an at-commit fsync.
 */
static void AddPendingSync(const RelFileLocator *rlocator) {
  bool found;

  /* create the hash if not yet */
  if (!pendingSyncHash) {
    HASHCTL ctl;

    ctl.keysize = sizeof(RelFileLocator);
    ctl.entrysize = sizeof(PendingRelSync);
    ctl.hcxt = TopTransactionContext;
    pendingSyncHash = hash_create("pending sync hash", 16, &ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
  }

  (void)hash_search(pendingSyncHash, rlocator, HASH_ENTER, &found);
  Assert(!found);
}

/*
 * RelationCreateStorage
 *		Create physical storage for a relation.
 *
 * Create the underlying disk file storage for the relation. This only
 * creates the main fork; additional forks are created lazily by the
 * modules that need them.
 *
 * This function is transactional. The creation is WAL-logged, and if the
 * transaction aborts later on, the storage will be destroyed.  A caller
 * that does not want the storage to be destroyed in case of an abort may
 * pass register_delete = false.
 */
SMgrRelation RelationCreateStorage(RelFileLocator rlocator, char relpersistence,
                                   bool register_delete) {
  SMgrRelation srel;
  ProcNumber procNumber;

  switch (relpersistence) {
  case RELPERSISTENCE_TEMP:
    procNumber = ProcNumberForTempRelations();
    break;
  case RELPERSISTENCE_UNLOGGED:
  case RELPERSISTENCE_PERMANENT:
    procNumber = INVALID_PROC_NUMBER;
    break;
  default:
    elog(ERROR, "invalid relpersistence: %c", relpersistence);
    return NULL; /* placate compiler */
  }

  srel = smgropen(rlocator, procNumber);
  smgrcreate(srel, MAIN_FORKNUM, false);

  /*
   * Add the relation to the list of stuff to delete at abort, if we are
   * asked to do so.
   */
  if (register_delete) {
    PendingRelDelete *pending;

    pending = (PendingRelDelete *)MemoryContextAlloc(TopMemoryContext,
                                                     sizeof(PendingRelDelete));
    pending->rlocator = rlocator;
    pending->procNumber = procNumber;
    pending->atCommit = false; /* delete if abort */
    pending->nestLevel = GetCurrentTransactionNestLevel();
    pending->next = pendingDeletes;
    pendingDeletes = pending;
  }

  if (relpersistence == RELPERSISTENCE_PERMANENT) {
    Assert(procNumber == INVALID_PROC_NUMBER);
    AddPendingSync(&rlocator);
  }

  return srel;
}

/*
 * RelationDropStorage
 *		Schedule unlinking of physical storage at transaction commit.
 */
void RelationDropStorage(Relation rel) {
  PendingRelDelete *pending;

  /* Add the relation to the list of stuff to delete at commit */
  pending = (PendingRelDelete *)MemoryContextAlloc(TopMemoryContext,
                                                   sizeof(PendingRelDelete));
  pending->rlocator = rel->rd_locator;
  pending->procNumber = rel->rd_backend;
  pending->atCommit = true; /* delete if commit */
  pending->nestLevel = GetCurrentTransactionNestLevel();
  pending->next = pendingDeletes;
  pendingDeletes = pending;

  /*
   * NOTE: if the relation was created in this transaction, it will now be
   * present in the pending-delete list twice, once with atCommit true and
   * once with atCommit false.  Hence, it will be physically deleted at end
   * of xact in either case (and the other entry will be ignored by
   * smgrDoPendingDeletes, so no error will occur).  We could instead remove
   * the existing list entry and delete the physical file immediately, but
   * for now I'll keep the logic simple.
   */

  RelationCloseSmgr(rel);
}

/*
 * RelationPreserveStorage
 *		Mark a relation as not to be deleted after all.
 *
 * We need this function because relation mapping changes are committed
 * separately from commit of the whole transaction, so it's still possible
 * for the transaction to abort after the mapping update is done.
 * When a new physical relation is installed in the map, it would be
 * scheduled for delete-on-abort, so we'd delete it, and be in trouble.
 * The relation mapper fixes this by telling us to not delete such relations
 * after all as part of its commit.
 *
 * We also use this to reuse an old build of an index during ALTER TABLE, this
 * time removing the delete-at-commit entry.
 *
 * No-op if the relation is not among those scheduled for deletion.
 */
void RelationPreserveStorage(RelFileLocator rlocator, bool atCommit) {
  PendingRelDelete *pending;
  PendingRelDelete *prev;
  PendingRelDelete *next;

  prev = NULL;
  for (pending = pendingDeletes; pending != NULL; pending = next) {
    next = pending->next;
    if (RelFileLocatorEquals(rlocator, pending->rlocator) &&
        pending->atCommit == atCommit) {
      /* unlink and delete list entry */
      if (prev)
        prev->next = next;
      else
        pendingDeletes = next;
      pfree(pending);
      /* prev does not change */
    } else {
      /* unrelated entry, don't touch it */
      prev = pending;
    }
  }
}

/*
 * RelationTruncate
 *		Physically truncate a relation to the specified number of
 *blocks.
 *
 * This includes getting rid of any buffers for the blocks that are to be
 * dropped.
 */
void RelationTruncate(Relation rel, BlockNumber nblocks) {
  bool fsm;
  bool vm;
  bool need_fsm_vacuum = false;
  ForkNumber forks[MAX_FORKNUM];
  BlockNumber old_blocks[MAX_FORKNUM];
  BlockNumber blocks[MAX_FORKNUM];
  int nforks = 0;
  SMgrRelation reln;

  /*
   * Make sure smgr_targblock etc aren't pointing somewhere past new end.
   * (Note: don't rely on this reln pointer below this loop.)
   */
  reln = RelationGetSmgr(rel);
  reln->smgr_targblock = InvalidBlockNumber;
  for (int i = 0; i <= MAX_FORKNUM; ++i)
    reln->smgr_cached_nblocks[i] = InvalidBlockNumber;

  /* Prepare for truncation of MAIN fork of the relation */
  forks[nforks] = MAIN_FORKNUM;
  old_blocks[nforks] = smgrnblocks(reln, MAIN_FORKNUM);
  blocks[nforks] = nblocks;
  nforks++;

  /* Prepare for truncation of the FSM if it exists */
  fsm = smgrexists(RelationGetSmgr(rel), FSM_FORKNUM);
  if (fsm) {
    blocks[nforks] = FreeSpaceMapPrepareTruncateRel(rel, nblocks);
    if (BlockNumberIsValid(blocks[nforks])) {
      forks[nforks] = FSM_FORKNUM;
      old_blocks[nforks] = smgrnblocks(reln, FSM_FORKNUM);
      nforks++;
      need_fsm_vacuum = true;
    }
  }

  /* Prepare for truncation of the visibility map too if it exists */
  vm = smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM);
  if (vm) {
    blocks[nforks] = visibilitymap_prepare_truncate(rel, nblocks);
    if (BlockNumberIsValid(blocks[nforks])) {
      forks[nforks] = VISIBILITYMAP_FORKNUM;
      old_blocks[nforks] = smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
      nforks++;
    }
  }

  /*
   * The code which follows can interact with concurrent checkpoints in two
   * separate ways.
   *
   * First, the truncation operation might drop buffers that the checkpoint
   * otherwise would have flushed. If it does, then it's essential that the
   * files actually get truncated on disk before the checkpoint record is
   * written. Otherwise, if reply begins from that checkpoint, the
   * to-be-truncated blocks might still exist on disk but have older
   * contents than expected, which can cause replay to fail. It's OK for the
   * blocks to not exist on disk at all, but not for them to have the wrong
   * contents. For this reason, we need to set DELAY_CHKPT_COMPLETE while
   * this code executes.
   *
   * Second, the call to smgrtruncate() below will in turn call
   * RegisterSyncRequest(). We need the sync request created by that call to
   * be processed before the checkpoint completes. CheckPointGuts() will
   * call ProcessSyncRequests(), but if we register our sync request after
   * that happens, then the WAL record for the truncation could end up
   * preceding the checkpoint record, while the actual sync doesn't happen
   * until the next checkpoint. To prevent that, we need to set
   * DELAY_CHKPT_START here. That way, if the XLOG_SMGR_TRUNCATE precedes
   * the redo pointer of a concurrent checkpoint, we're guaranteed that the
   * corresponding sync request will be processed before the checkpoint
   * completes.
   */
  Assert((MyProc->delayChkptFlags &
          (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
  MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;

  /*
   * We WAL-log the truncation first and then truncate in a critical
   * section. Truncation drops buffers, even if dirty, and then truncates
   * disk files. All of that work needs to complete before the lock is
   * released, or else old versions of pages on disk that are missing recent
   * changes would become accessible again.  We'll try the whole operation
   * again in crash recovery if we panic, but even then we can't give up
   * because we don't want standbys' relation sizes to diverge and break
   * replay or visibility invariants downstream.  The critical section also
   * suppresses interrupts.
   *
   * (See also visibilitymap.c if changing this code.)
   */
  START_CRIT_SECTION();

  /*
   * This will first remove any buffers from the buffer pool that should no
   * longer exist after truncation is complete, and then truncate the
   * corresponding files on disk.
   */
  smgrtruncate(RelationGetSmgr(rel), forks, nforks, old_blocks, blocks);

  END_CRIT_SECTION();

  /* We've done all the critical work, so checkpoints are OK now. */
  MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);

  /*
   * Update upper-level FSM pages to account for the truncation. This is
   * important because the just-truncated pages were likely marked as
   * all-free, and would be preferentially selected.
   *
   * NB: There's no point in delaying checkpoints until this is done.
   * Because the FSM is not WAL-logged, we have to be prepared for the
   * possibility of corruption after a crash anyway.
   */
  if (need_fsm_vacuum)
    FreeSpaceMapVacuumRange(rel, nblocks, InvalidBlockNumber);
}

/*
 * Copy a fork's data, block by block.
 *
 * Note that this requires that there is no dirty data in shared buffers. If
 * it's possible that there are, callers need to flush those using
 * e.g. FlushRelationBuffers(rel).
 *
 * Also note that this is frequently called via locutions such as
 *		RelationCopyStorage(RelationGetSmgr(rel), ...);
 * That's safe only because we perform only smgr operations here.
 * If we invoked anything else, a relcache flush could cause our SMgrRelation
 * argument to become a dangling pointer.
 */
void RelationCopyStorage(SMgrRelation src, SMgrRelation dst,
                         ForkNumber forkNum) {
  BlockNumber nblocks;
  BlockNumber blkno;
  BulkWriteState *bulkstate;

  bulkstate = smgr_bulk_start_smgr(dst, forkNum);

  nblocks = smgrnblocks(src, forkNum);

  for (blkno = 0; blkno < nblocks; blkno++) {
    BulkWriteBuffer buf;
    int piv_flags;
    bool checksum_failure;
    bool verified;

    /* If we got a cancel signal during the copy of the data, quit */
    CHECK_FOR_INTERRUPTS();

    buf = smgr_bulk_get_buf(bulkstate);
    smgrread(src, forkNum, blkno, (Page)buf);

    piv_flags = PIV_LOG_WARNING;
    if (ignore_checksum_failure)
      piv_flags |= PIV_IGNORE_CHECKSUM_FAILURE;
    verified = PageIsVerified((Page)buf, blkno, piv_flags, &checksum_failure);
    if (checksum_failure) {
      RelFileLocatorBackend rloc = src->smgr_rlocator;

      pgstat_prepare_report_checksum_failure(rloc.locator.dbOid);
      pgstat_report_checksum_failures_in_db(rloc.locator.dbOid, 1);
    }

    if (!verified) {
      /*
       * For paranoia's sake, capture the file path before invoking the
       * ereport machinery.  This guards against the possibility of a
       * relcache flush caused by, e.g., an errcontext callback.
       * (errcontext callbacks shouldn't be risking any such thing, but
       * people have been known to forget that rule.)
       */
      RelPathStr relpath = relpathbackend(src->smgr_rlocator.locator,
                                          src->smgr_rlocator.backend, forkNum);

      ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                      errmsg("invalid page in block %u of relation \"%s\"",
                             blkno, relpath.str)));
    }

    /* Queue the page for writing out */
    smgr_bulk_write(bulkstate, blkno, buf, false);
  }
  smgr_bulk_finish(bulkstate);
}

/*
 * RelFileLocatorSkippingWAL
 *		Check if a BM_PERMANENT relfilelocator is using WAL.
 *
 * Changes to certain relations must not write WAL; see "Skipping WAL for
 * New RelFileLocator" in src/backend/access/transam/README.  Though it is
 * known from Relation efficiently, this function is intended for the code
 * paths not having access to Relation.
 */
bool RelFileLocatorSkippingWAL(RelFileLocator rlocator) {
  if (!pendingSyncHash ||
      hash_search(pendingSyncHash, &rlocator, HASH_FIND, NULL) == NULL)
    return false;

  return true;
}

/*
 *	smgrDoPendingDeletes() -- Take care of relation deletes at end of xact.
 *
 * This also runs when aborting a subxact; we want to clean up a failed
 * subxact immediately.
 *
 * Note: It's possible that we're being asked to remove a relation that has
 * no physical storage in any fork. In particular, it's possible that we're
 * cleaning up an old temporary relation for which RemovePgTempFiles has
 * already recovered the physical storage.
 */
void smgrDoPendingDeletes(bool isCommit) {
  int nestLevel = GetCurrentTransactionNestLevel();
  PendingRelDelete *pending;
  PendingRelDelete *prev;
  PendingRelDelete *next;
  int nrels = 0, maxrels = 0;
  SMgrRelation *srels = NULL;

  prev = NULL;
  for (pending = pendingDeletes; pending != NULL; pending = next) {
    next = pending->next;
    if (pending->nestLevel < nestLevel) {
      /* outer-level entries should not be processed yet */
      prev = pending;
    } else {
      /* unlink list entry first, so we don't retry on failure */
      if (prev)
        prev->next = next;
      else
        pendingDeletes = next;
      /* do deletion if called for */
      if (pending->atCommit == isCommit) {
        SMgrRelation srel;

        srel = smgropen(pending->rlocator, pending->procNumber);

        /* allocate the initial array, or extend it, if needed */
        if (maxrels == 0) {
          maxrels = 8;
          srels = (SMgrRelationData **)palloc(sizeof(SMgrRelation) * maxrels);
        } else if (maxrels <= nrels) {
          maxrels *= 2;
          srels = (SMgrRelationData **)repalloc(srels,
                                                sizeof(SMgrRelation) * maxrels);
        }

        srels[nrels++] = srel;
      }
      /* must explicitly free the list entry */
      pfree(pending);
      /* prev does not change */
    }
  }

  if (nrels > 0) {
    smgrdounlinkall(srels, nrels, false);

    for (int i = 0; i < nrels; i++)
      smgrclose(srels[i]);

    pfree(srels);
  }
}

/*
 *	smgrDoPendingSyncs() -- Take care of relation syncs at end of xact.
 */
void smgrDoPendingSyncs(bool isCommit) {
  PendingRelDelete *pending;
  int nrels = 0, maxrels = 0;
  SMgrRelation *srels = NULL;
  HASH_SEQ_STATUS scan;
  PendingRelSync *pendingsync;

  Assert(GetCurrentTransactionNestLevel() == 1);

  if (!pendingSyncHash)
    return; /* no relation needs sync */

  /* Abort -- just throw away all pending syncs */
  if (!isCommit) {
    pendingSyncHash = NULL;
    return;
  }

  AssertPendingSyncs_RelationCache();

  /* Skip syncing nodes that smgrDoPendingDeletes() will delete. */
  for (pending = pendingDeletes; pending != NULL; pending = pending->next)
    if (pending->atCommit)
      (void)hash_search(pendingSyncHash, &pending->rlocator, HASH_REMOVE, NULL);

  hash_seq_init(&scan, pendingSyncHash);
  while ((pendingsync = (PendingRelSync *)hash_seq_search(&scan))) {
    SMgrRelation srel;

    srel = smgropen(pendingsync->rlocator, INVALID_PROC_NUMBER);

    /*
     * Sync the file: there is no WAL to write its contents to instead.
     */

    /* allocate the initial array, or extend it, if needed */
    if (maxrels == 0) {
      maxrels = 8;
      srels = (SMgrRelationData **)palloc(sizeof(SMgrRelation) * maxrels);
    } else if (maxrels <= nrels) {
      maxrels *= 2;
      srels =
          (SMgrRelationData **)repalloc(srels, sizeof(SMgrRelation) * maxrels);
    }

    srels[nrels++] = srel;
  }

  pendingSyncHash = NULL;

  if (nrels > 0) {
    smgrdosyncall(srels, nrels);
    pfree(srels);
  }
}

/*
 * smgrGetPendingDeletes() -- Get a list of non-temp relations to be deleted.
 *
 * The return value is the number of relations scheduled for termination.
 * *ptr is set to point to a freshly-palloc'd array of RelFileLocators.
 * If there are no relations to be deleted, *ptr is set to NULL.
 *
 * Only non-temporary relations are included in the returned list.  This is OK
 * because the list is used only in contexts where temporary relations don't
 * matter: we're either writing to the two-phase state file (and transactions
 * that have touched temp tables can't be prepared) or we're writing to xlog
 * (and all temporary files will be zapped if we restart anyway, so no need
 * for redo to do it also).
 *
 * Note that the list does not include anything scheduled for termination
 * by upper-level transactions.
 */
int smgrGetPendingDeletes(bool forCommit, RelFileLocator **ptr) {
  int nestLevel = GetCurrentTransactionNestLevel();
  int nrels;
  RelFileLocator *rptr;
  PendingRelDelete *pending;

  nrels = 0;
  for (pending = pendingDeletes; pending != NULL; pending = pending->next) {
    if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit &&
        pending->procNumber == INVALID_PROC_NUMBER)
      nrels++;
  }
  if (nrels == 0) {
    *ptr = NULL;
    return 0;
  }
  rptr = (RelFileLocator *)palloc(nrels * sizeof(RelFileLocator));
  *ptr = rptr;
  for (pending = pendingDeletes; pending != NULL; pending = pending->next) {
    if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit &&
        pending->procNumber == INVALID_PROC_NUMBER) {
      *rptr = pending->rlocator;
      rptr++;
    }
  }
  return nrels;
}

/*
 * AtSubCommit_smgr() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending-deletes list to the parent transaction.
 */
void AtSubCommit_smgr(void) {
  int nestLevel = GetCurrentTransactionNestLevel();
  PendingRelDelete *pending;

  for (pending = pendingDeletes; pending != NULL; pending = pending->next) {
    if (pending->nestLevel >= nestLevel)
      pending->nestLevel = nestLevel - 1;
  }
}

/*
 * AtSubAbort_smgr() --- Take care of subtransaction abort.
 *
 * Delete created relations and forget about deleted relations.
 * We can execute these operations immediately because we know this
 * subtransaction will not commit.
 */
void AtSubAbort_smgr(void) { smgrDoPendingDeletes(false); }
