/*----------------------------------------------------------------------
 *
 * tableam.c
 *		Table access method routines too big to be inline functions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/table/tableam.c
 *
 * NOTES
 *	  Note that most function in here are documented in tableam.h, rather
 *than here. That's because there's a lot of inline functions in tableam.h and
 *	  it'd be harder to understand if one constantly had to switch between
 *files.
 *
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"

/*
 * Constants to control the behavior of block allocation to parallel workers
 * during a parallel seqscan.  Technically these values do not need to be
 * powers of 2, but having them as powers of 2 makes the math more optimal
 * and makes the ramp-down stepping more even.
 */

/* The number of I/O chunks we try to break a parallel seqscan down into */
#define PARALLEL_SEQSCAN_NCHUNKS			2048
/* Ramp down size of allocations when we've only this number of chunks left */
#define PARALLEL_SEQSCAN_RAMPDOWN_CHUNKS	64
/* Cap the size of parallel I/O chunks to this number of blocks */
#define PARALLEL_SEQSCAN_MAX_CHUNK_SIZE		8192

/* GUC variables */
thread_local /*LOCAL*/ char *default_table_access_method =
    DEFAULT_TABLE_ACCESS_METHOD;
thread_local /*LOCAL*/ bool synchronize_seqscans = true;

/* ----------------------------------------------------------------------------
 * Slot functions.
 * ----------------------------------------------------------------------------
 */

const TupleTableSlotOps *table_slot_callbacks(Relation relation) {
  const TupleTableSlotOps *tts_cb;

  if (relation->rd_tableam)
    tts_cb = relation->rd_tableam->slot_callbacks(relation);
  else if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE) {
    /*
     * Historically FDWs expect to store heap tuples in slots. Continue
     * handing them one, to make it less painful to adapt FDWs to new
     * versions. The cost of a heap slot over a virtual slot is pretty
     * small.
     */
    tts_cb = &TTSOpsHeapTuple;
  } else {
    /*
     * These need to be supported, as some parts of the code (like COPY)
     * need to create slots for such relations too. It seems better to
     * centralize the knowledge that a heap slot is the right thing in
     * that case here.
     */
    Assert(relation->rd_rel->relkind == RELKIND_VIEW ||
           relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
    tts_cb = &TTSOpsVirtual;
  }

  return tts_cb;
}

TupleTableSlot *table_slot_create(Relation relation, List **reglist) {
  const TupleTableSlotOps *tts_cb;
  TupleTableSlot *slot;

  tts_cb = table_slot_callbacks(relation);
  slot = MakeSingleTupleTableSlot(RelationGetDescr(relation), tts_cb);

  if (reglist)
    *reglist = lappend(*reglist, slot);

  return slot;
}

/* ----------------------------------------------------------------------------
 * Table scan functions.
 * ----------------------------------------------------------------------------
 */

TableScanDesc table_beginscan_catalog(Relation relation, int nkeys,
                                      struct ScanKeyData *key) {
  uint32 flags = SO_TYPE_SEQSCAN | SO_ALLOW_STRAT | SO_ALLOW_SYNC |
                 SO_ALLOW_PAGEMODE | SO_TEMP_SNAPSHOT;
  Oid relid = RelationGetRelid(relation);
  Snapshot snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));

  return relation->rd_tableam->scan_begin(relation, snapshot, nkeys, key,
                                          flags);
}

/* ----------------------------------------------------------------------------
 * Parallel table scan related functions.
 * ----------------------------------------------------------------------------
 */

/* ----------------------------------------------------------------------------
 * Index scan related functions.
 * ----------------------------------------------------------------------------
 */

/*
 * To perform that check simply start an index scan, create the necessary
 * slot, do the heap lookup, and shut everything down again. This could be
 * optimized, but is unlikely to matter from a performance POV. If there
 * frequently are live index pointers also matching a unique index key, the
 * CPU overhead of this routine is unlikely to matter.
 *
 * Note that *tid may be modified when we return true if the AM supports
 * storing multiple row versions reachable via a single index entry (like
 * heap's HOT).
 */
bool table_index_fetch_tuple_check(Relation rel, ItemPointer tid,
                                   Snapshot snapshot, bool *all_dead) {
  IndexFetchTableData *scan;
  TupleTableSlot *slot;
  bool call_again = false;
  bool found;

  slot = table_slot_create(rel, NULL);
  scan = table_index_fetch_begin(rel);
  found =
      table_index_fetch_tuple(scan, tid, snapshot, slot, &call_again, all_dead);
  table_index_fetch_end(scan);
  ExecDropSingleTupleTableSlot(slot);

  return found;
}

/* ------------------------------------------------------------------------
 * Functions for non-modifying operations on individual tuples
 * ------------------------------------------------------------------------
 */

void table_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid) {
  Relation rel = scan->rs_rd;
  const TableAmRoutine *tableam = rel->rd_tableam;

  /*
   * We don't expect direct calls to table_tuple_get_latest_tid with valid
   * CheckXidAlive for catalog or regular tables.  See detailed comments in
   * xact.c where these variables are declared.
   */
  if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
    elog(ERROR,
         "unexpected table_tuple_get_latest_tid call during logical decoding");

  /*
   * Since this can be called with user-supplied TID, don't trust the input
   * too much.
   */
  if (!tableam->tuple_tid_valid(scan, tid))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("tid (%u, %u) is not valid for relation \"%s\"",
                           ItemPointerGetBlockNumberNoCheck(tid),
                           ItemPointerGetOffsetNumberNoCheck(tid),
                           RelationGetRelationName(rel))));

  tableam->tuple_get_latest_tid(scan, tid);
}

/* ----------------------------------------------------------------------------
 * Functions to make modifications a bit simpler.
 * ----------------------------------------------------------------------------
 */

/*
 * simple_table_tuple_insert - insert a tuple
 *
 * Currently, this routine differs from table_tuple_insert only in supplying a
 * default command ID and not allowing access to the speedup options.
 */
void simple_table_tuple_insert(Relation rel, TupleTableSlot *slot) {
  table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);
}

/*
 * simple_table_tuple_delete - delete a tuple
 *
 * This routine may be used to delete a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void simple_table_tuple_delete(Relation rel, ItemPointer tid,
                               Snapshot snapshot) {
  TM_Result result;
  TM_FailureData tmfd;

  result = table_tuple_delete(rel, tid, GetCurrentCommandId(true), snapshot,
                              InvalidSnapshot, true /* wait for commit */,
                              &tmfd, false /* changingPart */);

  switch (result) {
  case TM_SelfModified:
    /* Tuple was already updated in current command? */
    elog(ERROR, "tuple already updated by self");
    break;

  case TM_Ok:
    /* done successfully */
    break;

  case TM_Updated:
    elog(ERROR, "tuple concurrently updated");
    break;

  case TM_Deleted:
    elog(ERROR, "tuple concurrently deleted");
    break;

  default:
    elog(ERROR, "unrecognized table_tuple_delete status: %u", result);
    break;
  }
}

/*
 * simple_table_tuple_update - replace a tuple
 *
 * This routine may be used to update a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void simple_table_tuple_update(Relation rel, ItemPointer otid,
                               TupleTableSlot *slot, Snapshot snapshot,
                               TU_UpdateIndexes *update_indexes) {
  TM_Result result;
  TM_FailureData tmfd;
  LockTupleMode lockmode;

  result = table_tuple_update(
      rel, otid, slot, GetCurrentCommandId(true), snapshot, InvalidSnapshot,
      true /* wait for commit */, &tmfd, &lockmode, update_indexes);

  switch (result) {
  case TM_SelfModified:
    /* Tuple was already updated in current command? */
    elog(ERROR, "tuple already updated by self");
    break;

  case TM_Ok:
    /* done successfully */
    break;

  case TM_Updated:
    elog(ERROR, "tuple concurrently updated");
    break;

  case TM_Deleted:
    elog(ERROR, "tuple concurrently deleted");
    break;

  default:
    elog(ERROR, "unrecognized table_tuple_update status: %u", result);
    break;
  }
}

/* ----------------------------------------------------------------------------
 * Helper functions to implement parallel scans for block oriented AMs.
 * ----------------------------------------------------------------------------
 */

/* ----------------------------------------------------------------------------
 * Helper functions to implement relation sizing for block oriented AMs.
 * ----------------------------------------------------------------------------
 */

/*
 * table_block_relation_size
 *
 * If a table AM uses the various relation forks as the sole place where data
 * is stored, and if it uses them in the expected manner (e.g. the actual data
 * is in the main fork rather than some other), it can use this implementation
 * of the relation_size callback rather than implementing its own.
 */
uint64 table_block_relation_size(Relation rel, ForkNumber forkNumber) {
  uint64 nblocks = 0;

  /* InvalidForkNumber indicates returning the size for all forks */
  if (forkNumber == InvalidForkNumber) {
    for (int i = 0; i < MAX_FORKNUM; i++)
      nblocks += smgrnblocks(RelationGetSmgr(rel), (ForkNumber)i);
  } else
    nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);

  return nblocks * BLCKSZ;
}

/*
 * table_block_relation_estimate_size
 *
 * This function can't be directly used as the implementation of the
 * relation_estimate_size callback, because it has a few additional parameters.
 * Instead, it is intended to be used as a helper function; the caller can
 * pass through the arguments to its relation_estimate_size function plus the
 * additional values required here.
 *
 * overhead_bytes_per_tuple should contain the approximate number of bytes
 * of storage required to store a tuple above and beyond what is required for
 * the tuple data proper. Typically, this would include things like the
 * size of the tuple header and item pointer. This is only used for query
 * planning, so a table AM where the value is not constant could choose to
 * pass a "best guess".
 *
 * usable_bytes_per_page should contain the approximate number of bytes per
 * page usable for tuple data, excluding the page header and any anticipated
 * special space.
 */
void table_block_relation_estimate_size(Relation rel, int32 *attr_widths,
                                        BlockNumber *pages, double *tuples,
                                        double *allvisfrac,
                                        Size overhead_bytes_per_tuple,
                                        Size usable_bytes_per_page) {
  BlockNumber curpages;
  BlockNumber relpages;
  double reltuples;
  BlockNumber relallvisible;
  double density;

  /* it should have storage, so we can call the smgr */
  curpages = RelationGetNumberOfBlocks(rel);

  /* coerce values in pg_class to more desirable types */
  relpages = (BlockNumber)rel->rd_rel->relpages;
  reltuples = (double)rel->rd_rel->reltuples;
  relallvisible = (BlockNumber)rel->rd_rel->relallvisible;

  /*
   * HACK: if the relation has never yet been vacuumed, use a minimum size
   * estimate of 10 pages.  The idea here is to avoid assuming a
   * newly-created table is really small, even if it currently is, because
   * that may not be true once some data gets loaded into it.  Once a vacuum
   * or analyze cycle has been done on it, it's more reasonable to believe
   * the size is somewhat stable.
   *
   * (Note that this is only an issue if the plan gets cached and used again
   * after the table has been filled.  What we're trying to avoid is using a
   * nestloop-type plan on a table that has grown substantially since the
   * plan was made.  Normally, autovacuum/autoanalyze will occur once enough
   * inserts have happened and cause cached-plan invalidation; but that
   * doesn't happen instantaneously, and it won't happen at all for cases
   * such as temporary tables.)
   *
   * We test "never vacuumed" by seeing whether reltuples < 0.
   *
   * If the table has inheritance children, we don't apply this heuristic.
   * Totally empty parent tables are quite common, so we should be willing
   * to believe that they are empty.
   */
  if (curpages < 10 && reltuples < 0 && !rel->rd_rel->relhassubclass)
    curpages = 10;

  /* report estimated # pages */
  *pages = curpages;
  /* quick exit if rel is clearly empty */
  if (curpages == 0) {
    *tuples = 0;
    *allvisfrac = 0;
    return;
  }

  /* estimate number of tuples from previous tuple density */
  if (reltuples >= 0 && relpages > 0)
    density = reltuples / (double)relpages;
  else {
    /*
     * When we have no data because the relation was never yet vacuumed,
     * estimate tuple width from attribute datatypes.  We assume here that
     * the pages are completely full, which is OK for tables but is
     * probably an overestimate for indexes.  Fortunately
     * get_relation_info() can clamp the overestimate to the parent
     * table's size.
     *
     * Note: this code intentionally disregards alignment considerations,
     * because (a) that would be gilding the lily considering how crude
     * the estimate is, (b) it creates platform dependencies in the
     * default plans which are kind of a headache for regression testing,
     * and (c) different table AMs might use different padding schemes.
     */
    int32 tuple_width;
    int fillfactor;

    /*
     * Without reltuples/relpages, we also need to consider fillfactor.
     * The other branch considers it implicitly by calculating density
     * from actual relpages/reltuples statistics.
     */
    fillfactor = RelationGetFillFactor(rel, HEAP_DEFAULT_FILLFACTOR);

    tuple_width = get_rel_data_width(rel, attr_widths);
    tuple_width += overhead_bytes_per_tuple;
    /* note: integer division is intentional here */
    density = (usable_bytes_per_page * fillfactor / 100) / tuple_width;
    /* There's at least one row on the page, even with low fillfactor. */
    density = clamp_row_est(density);
  }
  *tuples = rint(density * (double)curpages);

  /*
   * We use relallvisible as-is, rather than scaling it up like we do for
   * the pages and tuples counts, on the theory that any pages added since
   * the last VACUUM are most likely not marked all-visible.  But costsize.c
   * wants it converted to a fraction.
   */
  if (relallvisible == 0 || curpages <= 0)
    *allvisfrac = 0;
  else if ((double)relallvisible >= curpages)
    *allvisfrac = 1;
  else
    *allvisfrac = (double)relallvisible / curpages;
}
