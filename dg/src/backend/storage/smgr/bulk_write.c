/*-------------------------------------------------------------------------
 *
 * bulk_write.c
 *	  Efficiently and reliably populate a new relation
 *
 * The assumption is that no other backends access the relation while we are
 * loading it, so we can take some shortcuts.  Pages already present in the
 * indicated fork when the bulk write operation is started are not modified
 * unless explicitly written to.  Do not mix operations through the regular
 * buffer manager and the bulk loading interface!
 *
 * We bypass the buffer manager to avoid the locking overhead, and call
 * smgrextend() directly.  A downside is that the pages will need to be
 * re-read into shared buffers on first use after the build finishes.  That's
 * usually a good tradeoff for large relations, and for small relations, the
 * overhead isn't very significant compared to creating the relation in the
 * first place.
 *
 * Because we bypass the buffer manager, we need to register the relation for
 * fsyncing at the next checkpoint ourselves.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/bulk_write.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/rel.h"

/* Writes queued before they are flushed together */
#define MAX_PENDING_WRITES 32

static const PGIOAlignedBlock zero_buffer = {0}; /* worth BLCKSZ */

typedef struct PendingWrite {
  BulkWriteBuffer buf;
  BlockNumber blkno;
  bool page_std;
} PendingWrite;

/*
 * Bulk writer state for one relation fork.
 */
struct BulkWriteState {
  /* Information about the target relation we're writing */
  SMgrRelation smgr;
  ForkNumber forknum;

  /* We keep several writes queued, and write them in batches */
  int npending;
  PendingWrite pending_writes[MAX_PENDING_WRITES];

  /* Current size of the relation */
  BlockNumber relsize;

  MemoryContext memcxt;
};

static void smgr_bulk_flush(BulkWriteState *bulkstate);

/*
 * Start a bulk write operation on a relation fork.
 */
BulkWriteState *smgr_bulk_start_rel(Relation rel, ForkNumber forknum) {
  return smgr_bulk_start_smgr(RelationGetSmgr(rel), forknum);
}

/*
 * Start a bulk write operation on a relation fork.
 *
 * This is like smgr_bulk_start_rel, but can be used without a relcache entry.
 */
BulkWriteState *smgr_bulk_start_smgr(SMgrRelation smgr, ForkNumber forknum) {
  BulkWriteState *state;

  state = (BulkWriteState *)palloc(sizeof(BulkWriteState));
  state->smgr = smgr;
  state->forknum = forknum;

  state->npending = 0;
  state->relsize = smgrnblocks(smgr, forknum);

  /*
   * Remember the memory context.  We will use it to allocate all the
   * buffers later.
   */
  state->memcxt = CurrentMemoryContext;

  return state;
}

/*
 * Finish bulk write operation.
 *
 * This flushes any remaining pending writes to disk, and fsyncs the relation
 * if needed.
 */
void smgr_bulk_finish(BulkWriteState *bulkstate) {
  /* Flush any remaining pages */
  smgr_bulk_flush(bulkstate);

  /*
   * Fsync the relation, or register it for the next checkpoint, if
   * necessary.
   */
  if (SmgrIsTemp(bulkstate->smgr)) {
    /* Temporary relations don't need to be fsync'd, ever */
  } else {
    /*
     * There is no WAL to replay the pages from on a crash.  It's
     * sufficient to call smgrregistersync(), that ensures that the
     * checkpointer will flush it at the shutdown checkpoint; a new
     * permanent relation is also fsync'd at commit, by smgrDoPendingSyncs().
     */
    smgrregistersync(bulkstate->smgr, bulkstate->forknum);
  }
}

static int buffer_cmp(const void *a, const void *b) {
  const PendingWrite *bufa = (const PendingWrite *)a;
  const PendingWrite *bufb = (const PendingWrite *)b;

  /* We should not see duplicated writes for the same block */
  Assert(bufa->blkno != bufb->blkno);
  if (bufa->blkno > bufb->blkno)
    return 1;
  else
    return -1;
}

/*
 * Finish all the pending writes.
 */
static void smgr_bulk_flush(BulkWriteState *bulkstate) {
  int npending = bulkstate->npending;
  PendingWrite *pending_writes = bulkstate->pending_writes;

  if (npending == 0)
    return;

  if (npending > 1)
    qsort(pending_writes, npending, sizeof(PendingWrite), buffer_cmp);

  for (int i = 0; i < npending; i++) {
    BlockNumber blkno = pending_writes[i].blkno;
    Page page = pending_writes[i].buf->data;

    PageSetChecksumInplace(page, blkno);

    if (blkno >= bulkstate->relsize) {
      /*
       * If we have to write pages nonsequentially, fill in the space
       * with zeroes until we come back and overwrite.  This is not
       * logically necessary on standard Unix filesystems (unwritten
       * space will read as zeroes anyway), but it should help to avoid
       * fragmentation.
       */
      while (blkno > bulkstate->relsize) {
        /* don't set checksum for all-zero page */
        smgrextend(bulkstate->smgr, bulkstate->forknum, bulkstate->relsize,
                   &zero_buffer, true);
        bulkstate->relsize++;
      }

      smgrextend(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
      bulkstate->relsize++;
    } else
      smgrwrite(bulkstate->smgr, bulkstate->forknum, blkno, page, true);
    pfree(page);
  }

  bulkstate->npending = 0;
}

/*
 * Queue write of 'buf'.
 *
 * NB: this takes ownership of 'buf'!
 *
 * You are only allowed to write a given block once as part of one bulk write
 * operation.
 */
void smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum,
                     BulkWriteBuffer buf, bool page_std) {
  PendingWrite *w;

  w = &bulkstate->pending_writes[bulkstate->npending++];
  w->buf = buf;
  w->blkno = blocknum;
  w->page_std = page_std;

  if (bulkstate->npending == MAX_PENDING_WRITES)
    smgr_bulk_flush(bulkstate);
}

/*
 * Allocate a new buffer which can later be written with smgr_bulk_write().
 *
 * There is no function to free the buffer.  When you pass it to
 * smgr_bulk_write(), it takes ownership and frees it when it's no longer
 * needed.
 *
 * This is currently implemented as a simple palloc, but could be implemented
 * using a ring buffer or larger chunks in the future, so don't rely on it.
 */
BulkWriteBuffer smgr_bulk_get_buf(BulkWriteState *bulkstate) {
  return (BulkWriteBuffer)MemoryContextAllocAligned(bulkstate->memcxt, BLCKSZ,
                                                    PG_IO_ALIGN_SIZE, 0);
}
