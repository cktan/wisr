/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Implementation of Lehman and Yao's btree management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "commands/vacuum.h"
#include "nodes/execnodes.h"
#include "storage/bulk_write.h"
#include "storage/condition_variable.h"
#include "storage/indexfsm.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"

static void btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                         IndexBulkDeleteCallback callback, void *callback_state,
                         BTCycleId cycleid);
static BlockNumber btvacuumpage(BTVacState *vstate, Buffer buf);
static BTVacuumPosting btreevacuumposting(BTVacState *vstate,
                                          IndexTuple posting,
                                          OffsetNumber updatedoffset,
                                          int *nremaining);

/*
 * Btree handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum bthandler(PG_FUNCTION_ARGS) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  amroutine->amstrategies = BTMaxStrategyNumber;
  amroutine->amsupport = BTNProcs;
  amroutine->amoptsprocnum = BTOPTIONS_PROC;
  amroutine->amcanorder = true;
  amroutine->amcanorderbyop = false;
  amroutine->amcanhash = false;
  amroutine->amconsistentequality = true;
  amroutine->amconsistentordering = true;
  amroutine->amcanbackward = true;
  amroutine->amcanunique = true;
  amroutine->amcanmulticol = true;
  amroutine->amoptionalkey = true;
  amroutine->amsearcharray = true;
  amroutine->amsearchnulls = true;
  amroutine->amstorage = false;
  amroutine->amclusterable = true;
  amroutine->ampredlocks = true;
  amroutine->amcaninclude = true;
  amroutine->amusemaintenanceworkmem = false;
  amroutine->amsummarizing = false;
  amroutine->amkeytype = InvalidOid;

  amroutine->ambuild = btbuild;
  amroutine->ambuildempty = btbuildempty;
  amroutine->aminsert = btinsert;
  amroutine->aminsertcleanup = NULL;
  amroutine->ambulkdelete = btbulkdelete;
  amroutine->amvacuumcleanup = btvacuumcleanup;
  amroutine->amcanreturn = btcanreturn;
  amroutine->amcostestimate = btcostestimate;
  amroutine->amgettreeheight = btgettreeheight;
  amroutine->amoptions = btoptions;
  amroutine->amproperty = btproperty;
  amroutine->ambuildphasename = btbuildphasename;
  amroutine->amvalidate = btvalidate;
  amroutine->amadjustmembers = btadjustmembers;
  amroutine->ambeginscan = btbeginscan;
  amroutine->amrescan = btrescan;
  amroutine->amgettuple = btgettuple;
  amroutine->amgetbitmap = btgetbitmap;
  amroutine->amendscan = btendscan;
  amroutine->ammarkpos = btmarkpos;
  amroutine->amrestrpos = btrestrpos;
  amroutine->amtranslatestrategy = bttranslatestrategy;
  amroutine->amtranslatecmptype = bttranslatecmptype;

  PG_RETURN_POINTER(amroutine);
}

/*
 *	btbuildempty() -- build an empty btree index in the initialization fork
 */
void btbuildempty(Relation index) {
  bool allequalimage = _bt_allequalimage(index, false);
  BulkWriteState *bulkstate;
  BulkWriteBuffer metabuf;

  bulkstate = smgr_bulk_start_rel(index, INIT_FORKNUM);

  /* Construct metapage. */
  metabuf = smgr_bulk_get_buf(bulkstate);
  _bt_initmetapage((Page)metabuf, P_NONE, 0, allequalimage);
  smgr_bulk_write(bulkstate, BTREE_METAPAGE, metabuf, true);

  smgr_bulk_finish(bulkstate);
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for
 *our new tuple, and put it there.
 */
bool btinsert(Relation rel, Datum *values, bool *isnull, ItemPointer ht_ctid,
              Relation heapRel, IndexUniqueCheck checkUnique,
              bool indexUnchanged, IndexInfo *indexInfo) {
  bool result;
  IndexTuple itup;

  /* generate an index tuple */
  itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
  itup->t_tid = *ht_ctid;

  result = _bt_doinsert(rel, itup, checkUnique, indexUnchanged, heapRel);

  pfree(itup);

  return result;
}

/*
 *	btgettuple() -- Get the next tuple in the scan.
 */
bool btgettuple(IndexScanDesc scan, ScanDirection dir) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;
  bool res;

  Assert(scan->heapRelation != NULL);

  /* btree indexes are never lossy */
  scan->xs_recheck = false;

  /* Each loop iteration performs another primitive index scan */
  do {
    /*
     * If we've already initialized this scan, we can just advance it in
     * the appropriate direction.  If we haven't done so yet, we call
     * _bt_first() to get the first item in the scan.
     */
    if (!BTScanPosIsValid(so->currPos))
      res = _bt_first(scan, dir);
    else {
      /*
       * Check to see if we should kill the previously-fetched tuple.
       */
      if (scan->kill_prior_tuple) {
        /*
         * Yes, remember it for later. (We'll deal with all such
         * tuples at once right before leaving the index page.)  The
         * test for numKilled overrun is not just paranoia: if the
         * caller reverses direction in the indexscan then the same
         * item might get entered multiple times. It's not worth
         * trying to optimize that, so we don't detect it, but instead
         * just forget any excess entries.
         */
        if (so->killedItems == NULL)
          so->killedItems = (int *)palloc(MaxTIDsPerBTreePage * sizeof(int));
        if (so->numKilled < MaxTIDsPerBTreePage)
          so->killedItems[so->numKilled++] = so->currPos.itemIndex;
      }

      /*
       * Now continue the scan.
       */
      res = _bt_next(scan, dir);
    }

    /* If we have a tuple, return it ... */
    if (res)
      break;
    /* ... otherwise see if we need another primitive index scan */
  } while (so->numArrayKeys && _bt_start_prim_scan(scan, dir));

  return res;
}

/*
 * btgetbitmap() -- gets all matching tuples, and adds them to a bitmap
 */
int64 btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;
  int64 ntids = 0;
  ItemPointer heapTid;

  Assert(scan->heapRelation == NULL);

  /* Each loop iteration performs another primitive index scan */
  do {
    /* Fetch the first page & tuple */
    if (_bt_first(scan, ForwardScanDirection)) {
      /* Save tuple ID, and continue scanning */
      heapTid = &scan->xs_heaptid;
      tbm_add_tuples(tbm, heapTid, 1, false);
      ntids++;

      for (;;) {
        /*
         * Advance to next tuple within page.  This is the same as the
         * easy case in _bt_next().
         */
        if (++so->currPos.itemIndex > so->currPos.lastItem) {
          /* let _bt_next do the heavy lifting */
          if (!_bt_next(scan, ForwardScanDirection))
            break;
        }

        /* Save tuple ID, and continue scanning */
        heapTid = &so->currPos.items[so->currPos.itemIndex].heapTid;
        tbm_add_tuples(tbm, heapTid, 1, false);
        ntids++;
      }
    }
    /* Now see if we need another primitive index scan */
  } while (so->numArrayKeys && _bt_start_prim_scan(scan, ForwardScanDirection));

  return ntids;
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
IndexScanDesc btbeginscan(Relation rel, int nkeys, int norderbys) {
  IndexScanDesc scan;
  BTScanOpaque so;

  /* no order by operators allowed */
  Assert(norderbys == 0);

  /* get the scan */
  scan = RelationGetIndexScan(rel, nkeys, norderbys);

  /* allocate private workspace */
  so = (BTScanOpaque)palloc(sizeof(BTScanOpaqueData));
  BTScanPosInvalidate(so->currPos);
  BTScanPosInvalidate(so->markPos);
  if (scan->numberOfKeys > 0)
    so->keyData = (ScanKey)palloc(scan->numberOfKeys * sizeof(ScanKeyData));
  else
    so->keyData = NULL;

  so->skipScan = false;
  so->needPrimScan = false;
  so->scanBehind = false;
  so->oppositeDirCheck = false;
  so->arrayKeys = NULL;
  so->orderProcs = NULL;
  so->arrayContext = NULL;

  so->killedItems = NULL; /* until needed */
  so->numKilled = 0;

  /*
   * We don't know yet whether the scan will be index-only, so we do not
   * allocate the tuple workspace arrays until btrescan.  However, we set up
   * scan->xs_itupdesc whether we'll need it or not, since that's so cheap.
   */
  so->currTuples = so->markTuples = NULL;

  scan->xs_itupdesc = RelationGetDescr(rel);

  scan->opaque = so;

  return scan;
}

/*
 *	btrescan() -- rescan an index relation
 */
void btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
              ScanKey orderbys, int norderbys) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;

  /* we aren't holding any read locks, but gotta drop the pins */
  if (BTScanPosIsValid(so->currPos)) {
    /* Before leaving current page, deal with any killed items */
    if (so->numKilled > 0)
      _bt_killitems(scan);
    BTScanPosUnpinIfPinned(so->currPos);
    BTScanPosInvalidate(so->currPos);
  }

  /*
   * We prefer to eagerly drop leaf page pins before btgettuple returns.
   * This avoids making VACUUM wait to acquire a cleanup lock on the page.
   *
   * We cannot safely drop leaf page pins during index-only scans due to a
   * race condition involving VACUUM setting pages all-visible in the VM.
   * It's also unsafe for plain index scans that use a non-MVCC snapshot.
   *
   * When we drop pins eagerly, the mechanism that marks so->killedItems[]
   * index tuples LP_DEAD has to deal with concurrent TID recycling races.
   * The scheme used to detect unsafe TID recycling won't work without WAL
   * (since it involves saving an affected page's LSN), and digres has none:
   * never drop pins eagerly (this is preferable to opting out of
   * kill_prior_tuple LP_DEAD setting).
   *
   * Also opt out of dropping leaf page pins eagerly during bitmap scans.
   * Pins cannot be held for more than an instant during bitmap scans either
   * way, so we might as well avoid wasting cycles on acquiring page LSNs.
   *
   * See nbtree/README section on making concurrent TID recycling safe.
   *
   * Note: so->dropPin should never change across rescans.
   */
  so->dropPin = false;

  so->markItemIndex = -1;
  so->needPrimScan = false;
  so->scanBehind = false;
  so->oppositeDirCheck = false;
  BTScanPosUnpinIfPinned(so->markPos);
  BTScanPosInvalidate(so->markPos);

  /*
   * Allocate tuple workspace arrays, if needed for an index-only scan and
   * not already done in a previous rescan call.  To save on palloc
   * overhead, both workspaces are allocated as one palloc block; only this
   * function and btendscan know that.
   *
   * NOTE: this data structure also makes it safe to return data from a
   * "name" column, even though btree name_ops uses an underlying storage
   * datatype of cstring.  The risk there is that "name" is supposed to be
   * padded to NAMEDATALEN, but the actual index tuple is probably shorter.
   * However, since we only return data out of tuples sitting in the
   * currTuples array, a fetch of NAMEDATALEN bytes can at worst pull some
   * data out of the markTuples array --- running off the end of memory for
   * a SIGSEGV is not possible.  Yeah, this is ugly as sin, but it beats
   * adding special-case treatment for name_ops elsewhere.
   */
  if (scan->xs_want_itup && so->currTuples == NULL) {
    so->currTuples = (char *)palloc(BLCKSZ * 2);
    so->markTuples = so->currTuples + BLCKSZ;
  }

  /*
   * Reset the scan keys
   */
  if (scankey && scan->numberOfKeys > 0)
    memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));
  so->numberOfKeys = 0; /* until _bt_preprocess_keys sets it */
  so->numArrayKeys = 0; /* ditto */
}

/*
 *	btendscan() -- close down a scan
 */
void btendscan(IndexScanDesc scan) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;

  /* we aren't holding any read locks, but gotta drop the pins */
  if (BTScanPosIsValid(so->currPos)) {
    /* Before leaving current page, deal with any killed items */
    if (so->numKilled > 0)
      _bt_killitems(scan);
    BTScanPosUnpinIfPinned(so->currPos);
  }

  so->markItemIndex = -1;
  BTScanPosUnpinIfPinned(so->markPos);

  /* No need to invalidate positions, the RAM is about to be freed. */

  /* Release storage */
  if (so->keyData != NULL)
    pfree(so->keyData);
  /* so->arrayKeys and so->orderProcs are in arrayContext */
  if (so->arrayContext != NULL)
    MemoryContextDelete(so->arrayContext);
  if (so->killedItems != NULL)
    pfree(so->killedItems);
  if (so->currTuples != NULL)
    pfree(so->currTuples);
  /* so->markTuples should not be pfree'd, see btrescan */
  pfree(so);
}

/*
 *	btmarkpos() -- save current scan position
 */
void btmarkpos(IndexScanDesc scan) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;

  /* There may be an old mark with a pin (but no lock). */
  BTScanPosUnpinIfPinned(so->markPos);

  /*
   * Just record the current itemIndex.  If we later step to next page
   * before releasing the marked position, _bt_steppage makes a full copy of
   * the currPos struct in markPos.  If (as often happens) the mark is moved
   * before we leave the page, we don't have to do that work.
   */
  if (BTScanPosIsValid(so->currPos))
    so->markItemIndex = so->currPos.itemIndex;
  else {
    BTScanPosInvalidate(so->markPos);
    so->markItemIndex = -1;
  }
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
void btrestrpos(IndexScanDesc scan) {
  BTScanOpaque so = (BTScanOpaque)scan->opaque;

  if (so->markItemIndex >= 0) {
    /*
     * The scan has never moved to a new page since the last mark.  Just
     * restore the itemIndex.
     *
     * NB: In this case we can't count on anything in so->markPos to be
     * accurate.
     */
    so->currPos.itemIndex = so->markItemIndex;
  } else {
    /*
     * The scan moved to a new page after last mark or restore, and we are
     * now restoring to the marked page.  We aren't holding any read
     * locks, but if we're still holding the pin for the current position,
     * we must drop it.
     */
    if (BTScanPosIsValid(so->currPos)) {
      /* Before leaving current page, deal with any killed items */
      if (so->numKilled > 0)
        _bt_killitems(scan);
      BTScanPosUnpinIfPinned(so->currPos);
    }

    if (BTScanPosIsValid(so->markPos)) {
      /* bump pin on mark buffer for assignment to current buffer */
      if (BTScanPosIsPinned(so->markPos))
        IncrBufferRefCount(so->markPos.buf);
      memcpy(&so->currPos, &so->markPos,
             offsetof(BTScanPosData, items[1]) +
                 so->markPos.lastItem * sizeof(BTScanPosItem));
      if (so->currTuples)
        memcpy(so->currTuples, so->markTuples, so->markPos.nextTupleOffset);
      /* Reset the scan's array keys (see _bt_steppage for why) */
      if (so->numArrayKeys) {
        _bt_start_array_keys(scan, so->currPos.dir);
        so->needPrimScan = false;
      }
    } else
      BTScanPosInvalidate(so->currPos);
  }
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *btbulkdelete(IndexVacuumInfo *info,
                                    IndexBulkDeleteResult *stats,
                                    IndexBulkDeleteCallback callback,
                                    void *callback_state) {
  Relation rel = info->index;
  BTCycleId cycleid;

  /* allocate stats if first time through, else re-use existing struct */
  if (stats == NULL)
    stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));

  /* Establish the vacuum cycle ID to use for this scan */
  /* The ENSURE stuff ensures we clean up shared memory on failure */
  PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
  {
    cycleid = _bt_start_vacuum(rel);

    btvacuumscan(info, stats, callback, callback_state, cycleid);
  }
  PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
  _bt_end_vacuum(rel);

  return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *btvacuumcleanup(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats) {
  BlockNumber num_delpages;

  /* No-op in ANALYZE ONLY mode */
  if (info->analyze_only)
    return stats;

  /*
   * If btbulkdelete was called, we need not do anything (we just maintain
   * the information used within _bt_vacuum_needs_cleanup() by calling
   * _bt_set_cleanup_info() below).
   *
   * If btbulkdelete was _not_ called, then we have a choice to make: we
   * must decide whether or not a btvacuumscan() call is needed now (i.e.
   * whether the ongoing VACUUM operation can entirely avoid a physical scan
   * of the index).  A call to _bt_vacuum_needs_cleanup() decides it for us
   * now.
   */
  if (stats == NULL) {
    /* Check if VACUUM operation can entirely avoid btvacuumscan() call */
    if (!_bt_vacuum_needs_cleanup(info->index))
      return NULL;

    /*
     * Since we aren't going to actually delete any leaf items, there's no
     * need to go through all the vacuum-cycle-ID pushups here.
     *
     * Posting list tuples are a source of inaccuracy for cleanup-only
     * scans.  btvacuumscan() will assume that the number of index tuples
     * from each page can be used as num_index_tuples, even though
     * num_index_tuples is supposed to represent the number of TIDs in the
     * index.  This naive approach can underestimate the number of tuples
     * in the index significantly.
     *
     * We handle the problem by making num_index_tuples an estimate in
     * cleanup-only case.
     */
    stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    btvacuumscan(info, stats, NULL, NULL, 0);
    stats->estimated_count = true;
  }

  /*
   * Maintain num_delpages value in metapage for _bt_vacuum_needs_cleanup().
   *
   * num_delpages is the number of deleted pages now in the index that were
   * not safe to place in the FSM to be recycled just yet.  num_delpages is
   * greater than 0 only when _bt_pagedel() actually deleted pages during
   * our call to btvacuumscan().  Even then, _bt_pendingfsm_finalize() must
   * have failed to place any newly deleted pages in the FSM just moments
   * ago.  (Actually, there are edge cases where recycling of the current
   * VACUUM's newly deleted pages does not even become safe by the time the
   * next VACUUM comes around.  See nbtree/README.)
   */
  Assert(stats->pages_deleted >= stats->pages_free);
  num_delpages = stats->pages_deleted - stats->pages_free;
  _bt_set_cleanup_info(info->index, num_delpages);

  /*
   * It's quite possible for us to be fooled by concurrent page splits into
   * double-counting some index tuples, so disbelieve any total that exceeds
   * the underlying heap's count ... if we know that accurately.  Otherwise
   * this might just make matters worse.
   */
  if (!info->estimated_count) {
    if (stats->num_index_tuples > info->num_heap_tuples)
      stats->num_index_tuples = info->num_heap_tuples;
  }

  return stats;
}

/*
 * btvacuumscan --- scan the index for VACUUMing purposes
 *
 * This combines the functions of looking for leaf tuples that are deletable
 * according to the vacuum callback, looking for empty pages that can be
 * deleted, and looking for old deleted pages that can be recycled.  Both
 * btbulkdelete and btvacuumcleanup invoke this (the latter only if no
 * btbulkdelete call occurred and _bt_vacuum_needs_cleanup returned true).
 *
 * The caller is responsible for initially allocating/zeroing a stats struct
 * and for obtaining a vacuum cycle ID if necessary.
 */
static void btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                         IndexBulkDeleteCallback callback, void *callback_state,
                         BTCycleId cycleid) {
  Relation rel = info->index;
  BTVacState vstate;
  BlockNumber num_pages;
  bool needLock;
  BlockRangeReadStreamPrivate p;
  ReadStream *stream = NULL;

  /*
   * Reset fields that track information about the entire index now.  This
   * avoids double-counting in the case where a single VACUUM command
   * requires multiple scans of the index.
   *
   * Avoid resetting the tuples_removed and pages_newly_deleted fields here,
   * since they track information about the VACUUM command, and so must last
   * across each call to btvacuumscan().
   *
   * (Note that pages_free is treated as state about the whole index, not
   * the current VACUUM.  This is appropriate because RecordFreeIndexPage()
   * calls are idempotent, and get repeated for the same deleted pages in
   * some scenarios.  The point for us is to track the number of recyclable
   * pages in the index at the end of the VACUUM command.)
   */
  stats->num_pages = 0;
  stats->num_index_tuples = 0;
  stats->pages_deleted = 0;
  stats->pages_free = 0;

  /* Set up info to pass down to btvacuumpage */
  vstate.info = info;
  vstate.stats = stats;
  vstate.callback = callback;
  vstate.callback_state = callback_state;
  vstate.cycleid = cycleid;

  /* Create a temporary memory context to run _bt_pagedel in */
  vstate.pagedelcontext = AllocSetContextCreate(
      CurrentMemoryContext, "_bt_pagedel", ALLOCSET_DEFAULT_SIZES);

  /* Initialize vstate fields used by _bt_pendingfsm_finalize */
  vstate.bufsize = 0;
  vstate.maxbufsize = 0;
  vstate.pendingpages = NULL;
  vstate.npendingpages = 0;
  /* Consider applying _bt_pendingfsm_finalize optimization */
  _bt_pendingfsm_init(rel, &vstate, (callback == NULL));

  /*
   * The outer loop iterates over all index pages except the metapage, in
   * physical order (we hope the kernel will cooperate in providing
   * read-ahead for speed).  It is critical that we visit all leaf pages,
   * including ones added after we start the scan, else we might fail to
   * delete some deletable tuples.  Hence, we must repeatedly check the
   * relation length.  We must acquire the relation-extension lock while
   * doing so to avoid a race condition: if someone else is extending the
   * relation, there is a window where bufmgr/smgr have created a new
   * all-zero page but it hasn't yet been write-locked by _bt_getbuf(). If
   * we manage to scan such a page here, we'll improperly assume it can be
   * recycled.  Taking the lock synchronizes things enough to prevent a
   * problem: either num_pages won't include the new page, or _bt_getbuf
   * already has write lock on the buffer and it will be fully initialized
   * before we can examine it.  Also, we need not worry if a page is added
   * immediately after we look; the page splitting code already has
   * write-lock on the left page before it adds a right page, so we must
   * already have processed any tuples due to be moved into such a page.
   *
   * XXX: Now that new pages are locked with RBM_ZERO_AND_LOCK, I don't
   * think the use of the extension lock is still required.
   *
   * We can skip locking for new or temp relations, however, since no one
   * else could be accessing them.
   */
  needLock = !RELATION_IS_LOCAL(rel);

  p.current_blocknum = BTREE_METAPAGE + 1;

  /*
   * It is safe to use batchmode as block_range_read_stream_cb takes no
   * locks.
   */
  stream = read_stream_begin_relation(
      READ_STREAM_MAINTENANCE | READ_STREAM_FULL, info->strategy, rel,
      MAIN_FORKNUM, block_range_read_stream_cb, &p, 0);
  for (;;) {
    /* Get the current relation length */
    if (needLock)
      LockRelationForExtension(rel, ExclusiveLock);
    num_pages = RelationGetNumberOfBlocks(rel);
    if (needLock)
      UnlockRelationForExtension(rel, ExclusiveLock);

    /* Quit if we've scanned the whole relation */
    if (p.current_blocknum >= num_pages)
      break;

    p.last_exclusive = num_pages;

    /* Iterate over pages, then loop back to recheck relation length */
    while (true) {
      Buffer buf;

      /* call vacuum_delay_point while not holding any buffer lock */
      vacuum_delay_point(false);

      buf = read_stream_next_buffer(stream, NULL);

      if (!BufferIsValid(buf))
        break;

      btvacuumpage(&vstate, buf);
    }

    /*
     * We have to reset the read stream to use it again. After returning
     * InvalidBuffer, the read stream API won't invoke our callback again
     * until the stream has been reset.
     */
    read_stream_reset(stream);
  }

  read_stream_end(stream);

  /* Set statistics num_pages field to final size of index */
  stats->num_pages = num_pages;

  MemoryContextDelete(vstate.pagedelcontext);

  /*
   * If there were any calls to _bt_pagedel() during scan of the index then
   * see if any of the resulting pages can be placed in the FSM now.  When
   * it's not safe we'll have to leave it up to a future VACUUM operation.
   *
   * Finally, if we placed any pages in the FSM (either just now or during
   * the scan), forcibly update the upper-level FSM pages to ensure that
   * searchers can find them.
   */
  _bt_pendingfsm_finalize(rel, &vstate);
  if (stats->pages_free > 0)
    IndexFreeSpaceMapVacuum(rel);
}

/*
 * btvacuumpage --- VACUUM one page
 *
 * This processes a single page for btvacuumscan().  In some cases we must
 * backtrack to re-examine and VACUUM pages that were on buf's page during
 * a previous call here.  This is how we handle page splits (that happened
 * after our cycleid was acquired) whose right half page happened to reuse
 * a block that we might have processed at some point before it was
 * recycled (i.e. before the page split).
 *
 * Returns BlockNumber of a scanned page (not backtracked).
 */
static BlockNumber btvacuumpage(BTVacState *vstate, Buffer buf) {
  IndexVacuumInfo *info = vstate->info;
  IndexBulkDeleteResult *stats = vstate->stats;
  IndexBulkDeleteCallback callback = vstate->callback;
  void *callback_state = vstate->callback_state;
  Relation rel = info->index;
  Relation heaprel = info->heaprel;
  bool attempt_pagedel;
  BlockNumber blkno, backtrack_to;
  BlockNumber scanblkno = BufferGetBlockNumber(buf);
  Page page;
  BTPageOpaque opaque;

  blkno = scanblkno;

backtrack:

  attempt_pagedel = false;
  backtrack_to = P_NONE;

  _bt_lockbuf(rel, buf, BT_READ);
  page = BufferGetPage(buf);
  opaque = NULL;
  if (!PageIsNew(page)) {
    _bt_checkpage(rel, buf);
    opaque = BTPageGetOpaque(page);
  }

  Assert(blkno <= scanblkno);
  if (blkno != scanblkno) {
    /*
     * We're backtracking.
     *
     * We followed a right link to a sibling leaf page (a page that
     * happens to be from a block located before scanblkno).  The only
     * case we want to do anything with is a live leaf page having the
     * current vacuum cycle ID.
     *
     * The page had better be in a state that's consistent with what we
     * expect.  Check for conditions that imply corruption in passing.  It
     * can't be half-dead because only an interrupted VACUUM process can
     * leave pages in that state, so we'd definitely have dealt with it
     * back when the page was the scanblkno page (half-dead pages are
     * always marked fully deleted by _bt_pagedel(), barring corruption).
     */
    if (!opaque || !P_ISLEAF(opaque) || P_ISHALFDEAD(opaque)) {
      Assert(false);
      ereport(LOG, (errcode(ERRCODE_INDEX_CORRUPTED),
                    errmsg_internal(
                        "right sibling %u of scanblkno %u unexpectedly in an "
                        "inconsistent state in index \"%s\"",
                        blkno, scanblkno, RelationGetRelationName(rel))));
      _bt_relbuf(rel, buf);
      return scanblkno;
    }

    /*
     * We may have already processed the page in an earlier call, when the
     * page was scanblkno.  This happens when the leaf page split occurred
     * after the scan began, but before the right sibling page became the
     * scanblkno.
     *
     * Page may also have been deleted by current btvacuumpage() call,
     * since _bt_pagedel() sometimes deletes the right sibling page of
     * scanblkno in passing (it does so after we decided where to
     * backtrack to).  We don't need to process this page as a deleted
     * page a second time now (in fact, it would be wrong to count it as a
     * deleted page in the bulk delete statistics a second time).
     */
    if (opaque->btpo_cycleid != vstate->cycleid || P_ISDELETED(opaque)) {
      /* Done with current scanblkno (and all lower split pages) */
      _bt_relbuf(rel, buf);
      return scanblkno;
    }
  }

  if (!opaque || BTPageIsRecyclable(page, heaprel)) {
    /* Okay to recycle this page (which could be leaf or internal) */
    RecordFreeIndexPage(rel, blkno);
    stats->pages_deleted++;
    stats->pages_free++;
  } else if (P_ISDELETED(opaque)) {
    /*
     * Already deleted page (which could be leaf or internal).  Can't
     * recycle yet.
     */
    stats->pages_deleted++;
  } else if (P_ISHALFDEAD(opaque)) {
    /* Half-dead leaf page (from interrupted VACUUM) -- finish deleting */
    attempt_pagedel = true;

    /*
     * _bt_pagedel() will increment both pages_newly_deleted and
     * pages_deleted stats in all cases (barring corruption)
     */
  } else if (P_ISLEAF(opaque)) {
    OffsetNumber deletable[MaxIndexTuplesPerPage];
    int ndeletable;
    BTVacuumPosting updatable[MaxIndexTuplesPerPage];
    int nupdatable;
    OffsetNumber offnum, minoff, maxoff;
    int nhtidsdead, nhtidslive;

    /*
     * Trade in the initial read lock for a full cleanup lock on this
     * page.  We must get such a lock on every leaf page over the course
     * of the vacuum scan, whether or not it actually contains any
     * deletable tuples --- see nbtree/README.
     */
    _bt_upgradelockbufcleanup(rel, buf);

    /*
     * Check whether we need to backtrack to earlier pages.  What we are
     * concerned about is a page split that happened since we started the
     * vacuum scan.  If the split moved tuples on the right half of the
     * split (i.e. the tuples that sort high) to a block that we already
     * passed over, then we might have missed the tuples.  We need to
     * backtrack now.  (Must do this before possibly clearing btpo_cycleid
     * or deleting scanblkno page below!)
     */
    if (vstate->cycleid != 0 && opaque->btpo_cycleid == vstate->cycleid &&
        !(opaque->btpo_flags & BTP_SPLIT_END) && !P_RIGHTMOST(opaque) &&
        opaque->btpo_next < scanblkno)
      backtrack_to = opaque->btpo_next;

    ndeletable = 0;
    nupdatable = 0;
    minoff = P_FIRSTDATAKEY(opaque);
    maxoff = PageGetMaxOffsetNumber(page);
    nhtidsdead = 0;
    nhtidslive = 0;
    if (callback) {
      /* btbulkdelete callback tells us what to delete (or update) */
      for (offnum = minoff; offnum <= maxoff;
           offnum = OffsetNumberNext(offnum)) {
        IndexTuple itup;

        itup = (IndexTuple)PageGetItem(page, PageGetItemId(page, offnum));

        Assert(!BTreeTupleIsPivot(itup));
        if (!BTreeTupleIsPosting(itup)) {
          /* Regular tuple, standard table TID representation */
          if (callback(&itup->t_tid, callback_state)) {
            deletable[ndeletable++] = offnum;
            nhtidsdead++;
          } else
            nhtidslive++;
        } else {
          BTVacuumPosting vacposting;
          int nremaining;

          /* Posting list tuple */
          vacposting = btreevacuumposting(vstate, itup, offnum, &nremaining);
          if (vacposting == NULL) {
            /*
             * All table TIDs from the posting tuple remain, so no
             * delete or update required
             */
            Assert(nremaining == BTreeTupleGetNPosting(itup));
          } else if (nremaining > 0) {

            /*
             * Store metadata about posting list tuple in
             * updatable array for entire page.  Existing tuple
             * will be updated during the later call to
             * _bt_delitems_vacuum().
             */
            Assert(nremaining < BTreeTupleGetNPosting(itup));
            updatable[nupdatable++] = vacposting;
            nhtidsdead += BTreeTupleGetNPosting(itup) - nremaining;
          } else {
            /*
             * All table TIDs from the posting list must be
             * deleted.  We'll delete the index tuple completely
             * (no update required).
             */
            Assert(nremaining == 0);
            deletable[ndeletable++] = offnum;
            nhtidsdead += BTreeTupleGetNPosting(itup);
            pfree(vacposting);
          }

          nhtidslive += nremaining;
        }
      }
    }

    /*
     * Apply any needed deletes or updates.  We issue just one
     * _bt_delitems_vacuum() call per page, so as to minimize WAL traffic.
     */
    if (ndeletable > 0 || nupdatable > 0) {
      Assert(nhtidsdead >= ndeletable + nupdatable);
      _bt_delitems_vacuum(rel, buf, deletable, ndeletable, updatable,
                          nupdatable);

      stats->tuples_removed += nhtidsdead;
      /* must recompute maxoff */
      maxoff = PageGetMaxOffsetNumber(page);

      /* can't leak memory here */
      for (int i = 0; i < nupdatable; i++)
        pfree(updatable[i]);
    } else {
      /*
       * If the leaf page has been split during this vacuum cycle, it
       * seems worth expending a write to clear btpo_cycleid even if we
       * don't have any deletions to do.  (If we do, _bt_delitems_vacuum
       * takes care of this.)  This ensures we won't process the page
       * again.
       *
       * We treat this like a hint-bit update because there's no need to
       * WAL-log it.
       */
      Assert(nhtidsdead == 0);
      if (vstate->cycleid != 0 && opaque->btpo_cycleid == vstate->cycleid) {
        opaque->btpo_cycleid = 0;
        MarkBufferDirtyHint(buf, true);
      }
    }

    /*
     * If the leaf page is now empty, try to delete it; else count the
     * live tuples (live table TIDs in posting lists are counted as
     * separate live tuples).  We don't delete when backtracking, though,
     * since that would require teaching _bt_pagedel() about backtracking
     * (doesn't seem worth adding more complexity to deal with that).
     *
     * We don't count the number of live TIDs during cleanup-only calls to
     * btvacuumscan (i.e. when callback is not set).  We count the number
     * of index tuples directly instead.  This avoids the expense of
     * directly examining all of the tuples on each page.  VACUUM will
     * treat num_index_tuples as an estimate in cleanup-only case, so it
     * doesn't matter that this underestimates num_index_tuples
     * significantly in some cases.
     */
    if (minoff > maxoff)
      attempt_pagedel = (blkno == scanblkno);
    else if (callback)
      stats->num_index_tuples += nhtidslive;
    else
      stats->num_index_tuples += maxoff - minoff + 1;

    Assert(!attempt_pagedel || nhtidslive == 0);
  }

  if (attempt_pagedel) {
    MemoryContext oldcontext;

    /* Run pagedel in a temp context to avoid memory leakage */
    MemoryContextReset(vstate->pagedelcontext);
    oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

    /*
     * _bt_pagedel maintains the bulk delete stats on our behalf;
     * pages_newly_deleted and pages_deleted are likely to be incremented
     * during call
     */
    Assert(blkno == scanblkno);
    _bt_pagedel(rel, buf, vstate);

    MemoryContextSwitchTo(oldcontext);
    /* pagedel released buffer, so we shouldn't */
  } else
    _bt_relbuf(rel, buf);

  if (backtrack_to != P_NONE) {
    blkno = backtrack_to;

    /* check for vacuum delay while not holding any buffer lock */
    vacuum_delay_point(false);

    /*
     * We can't use _bt_getbuf() here because it always applies
     * _bt_checkpage(), which will barf on an all-zero page. We want to
     * recycle all-zero pages, not fail.  Also, we want to use a
     * nondefault buffer access strategy.
     */
    buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                             info->strategy);
    goto backtrack;
  }

  return scanblkno;
}

/*
 * btreevacuumposting --- determine TIDs still needed in posting list
 *
 * Returns metadata describing how to build replacement tuple without the TIDs
 * that VACUUM needs to delete.  Returned value is NULL in the common case
 * where no changes are needed to caller's posting list tuple (we avoid
 * allocating memory here as an optimization).
 *
 * The number of TIDs that should remain in the posting list tuple is set for
 * caller in *nremaining.
 */
static BTVacuumPosting btreevacuumposting(BTVacState *vstate,
                                          IndexTuple posting,
                                          OffsetNumber updatedoffset,
                                          int *nremaining) {
  int live = 0;
  int nitem = BTreeTupleGetNPosting(posting);
  ItemPointer items = BTreeTupleGetPosting(posting);
  BTVacuumPosting vacposting = NULL;

  for (int i = 0; i < nitem; i++) {
    if (!vstate->callback(items + i, vstate->callback_state)) {
      /* Live table TID */
      live++;
    } else if (vacposting == NULL) {
      /*
       * First dead table TID encountered.
       *
       * It's now clear that we need to delete one or more dead table
       * TIDs, so start maintaining metadata describing how to update
       * existing posting list tuple.
       */
      vacposting = (BTVacuumPosting)palloc(
          offsetof(BTVacuumPostingData, deletetids) + nitem * sizeof(uint16));

      vacposting->itup = posting;
      vacposting->updatedoffset = updatedoffset;
      vacposting->ndeletedtids = 0;
      vacposting->deletetids[vacposting->ndeletedtids++] = i;
    } else {
      /* Second or subsequent dead table TID */
      vacposting->deletetids[vacposting->ndeletedtids++] = i;
    }
  }

  *nremaining = live;
  return vacposting;
}

/*
 *	btcanreturn() -- Check whether btree indexes support index-only scans.
 *
 * btrees always do, so this is trivial.
 */
bool btcanreturn(Relation index, int attno) { return true; }

/*
 * btgettreeheight() -- Compute tree height for use by btcostestimate().
 */
int btgettreeheight(Relation rel) { return _bt_getrootheight(rel); }

CompareType bttranslatestrategy(StrategyNumber strategy, Oid opfamily) {
  switch (strategy) {
  case BTLessStrategyNumber:
    return COMPARE_LT;
  case BTLessEqualStrategyNumber:
    return COMPARE_LE;
  case BTEqualStrategyNumber:
    return COMPARE_EQ;
  case BTGreaterEqualStrategyNumber:
    return COMPARE_GE;
  case BTGreaterStrategyNumber:
    return COMPARE_GT;
  default:
    return COMPARE_INVALID;
  }
}

StrategyNumber bttranslatecmptype(CompareType cmptype, Oid opfamily) {
  switch (cmptype) {
  case COMPARE_LT:
    return BTLessStrategyNumber;
  case COMPARE_LE:
    return BTLessEqualStrategyNumber;
  case COMPARE_EQ:
    return BTEqualStrategyNumber;
  case COMPARE_GE:
    return BTGreaterEqualStrategyNumber;
  case COMPARE_GT:
    return BTGreaterStrategyNumber;
  default:
    return InvalidStrategy;
  }
}
