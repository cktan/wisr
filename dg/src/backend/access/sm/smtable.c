/*-------------------------------------------------------------------------
 *
 * smtable.c
 *	  The table access method of a table in sm (access/tableam.h).
 *
 * A table in sm is scanned and inserted into by the executor through
 * smstore.c, not through a table access method; the heap, which was one, is
 * gone.  But a relation still carries a TableAmRoutine, and code that only
 * asks it questions -- what slot holds a row, whether a bitmap scan is
 * possible -- reads this one: a row is a virtual tuple.  UPDATE and DELETE
 * do come this way: nodeModifyTable.c fetches a row by its tid, updates or
 * deletes it, and reads its newest version when another transaction changed
 * it first, in many places, and each is a callback here over smstore.c.  So
 * does COPY, which scans a table and inserts into one through the access
 * method, and TABLESAMPLE, whose blocks are a tid's, SMSTORE_ROWS_PER_BLOCK
 * rownos each.  Every other callback raises an error: nothing should reach
 * it.
 *
 * IDENTIFICATION
 *	  src/backend/access/sm/smtable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/smstore.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/snapmgr.h"
#include "utils/fmgrprotos.h"

/* A callback of the access method that a table in sm never takes. */
template <typename R, typename... A> static R refuse(A...) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("a table in sm is read and written through sm "
                         "(access/smstore.h), not a table access method")));
  pg_unreachable();
}

/* The slot a row of a table in sm is put in: a virtual one. */
static const TupleTableSlotOps *smtable_slot_callbacks(Relation rel) {
  return &TTSOpsVirtual;
}

/*
 * A scan of a table in sm by the access method: sm's cursor behind it.  A
 * sample scan's blocks are a tid's, SMSTORE_ROWS_PER_BLOCK rownos each: the
 * table's count of them, as its highest rowno makes it, and the one read.
 */
typedef struct smtable_scan_t {
  TableScanDescData base; /* what tableam.h reads */
  smstore_scan_t *scan;
  BlockNumber nblocks;
  BlockNumber block;
  bool started;
} smtable_scan_t;

/* A sample scan's blocks: as many as hold the table's rownos. */
static BlockNumber sample_blocks(Relation rel) {
  uint64 limit = smstore_rowno_limit(rel);

  if (limit == 0)
    return 0;
  return (BlockNumber)Min(limit / SMSTORE_ROWS_PER_BLOCK + 1,
                          (uint64)MaxBlockNumber);
}

/*
 * Begin a scan of every row of rel, in rowno order.  sm reads at its own
 * transaction's snapshot, and takes no scan keys.
 */
static TableScanDesc smtable_scan_begin(Relation rel, Snapshot snapshot,
                                        int nkeys, struct ScanKeyData *key,
                                        uint32 flags) {
  smtable_scan_t *scan;

  if (nkeys != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a scan of table \"%s\" takes no scan keys",
                           RelationGetRelationName(rel))));
  scan = (smtable_scan_t *)palloc0(sizeof(smtable_scan_t));
  scan->base.rs_rd = rel;
  scan->base.rs_snapshot = snapshot;
  scan->base.rs_flags = flags;
  scan->scan = smstore_beginscan(rel, RelationGetNumberOfAttributes(rel));
  if (flags & SO_TYPE_SAMPLESCAN)
    scan->nblocks = sample_blocks(rel);
  return &scan->base;
}

/*
 * Start a scan again: a TID scan's, as a nested loop rescans one for each
 * outer row, has nothing to start, as it fetches each row by its tid and
 * never reads its cursor; a sample scan's starts its cursor and its blocks
 * again, as the table may have grown. No other scan of a table in sm is
 * rescanned here.
 */
static void smtable_scan_rescan(TableScanDesc sscan, struct ScanKeyData *key,
                                bool set_params, bool allow_strat,
                                bool allow_pagemode) {
  smtable_scan_t *scan = (smtable_scan_t *)sscan;

  if (sscan->rs_flags & SO_TYPE_TIDSCAN)
    return;
  if (!(sscan->rs_flags & SO_TYPE_SAMPLESCAN))
    refuse<void>();
  smstore_endscan(scan->scan);
  scan->scan = smstore_beginscan(sscan->rs_rd,
                                 RelationGetNumberOfAttributes(sscan->rs_rd));
  scan->nblocks = sample_blocks(sscan->rs_rd);
  scan->started = false;
}

/*
 * A sample scan's next block: the one the sampling method picks, as SYSTEM
 * does, or else the next; false when there are no more.  The cursor moves to
 * its first rowno.
 */
static bool smtable_scan_sample_next_block(TableScanDesc sscan,
                                           SampleScanState *scanstate) {
  smtable_scan_t *scan = (smtable_scan_t *)sscan;
  TsmRoutine *tsm = scanstate->tsmroutine;
  BlockNumber block;

  if (scan->nblocks == 0)
    return false;
  if (tsm->NextSampleBlock)
    block = tsm->NextSampleBlock(scanstate, scan->nblocks);
  else if (!scan->started)
    block = 0;
  else if (scan->block + 1 < scan->nblocks)
    block = scan->block + 1;
  else
    block = InvalidBlockNumber;
  if (!BlockNumberIsValid(block))
    return false;
  scan->block = block;
  scan->started = true;
  smstore_scan_seek(scan->scan, (uint64)block * SMSTORE_ROWS_PER_BLOCK);
  return true;
}

/*
 * A sample scan's next row of its block into slot: at the next offset the
 * sampling method picks that has a row, a tid's offset, one above the
 * rowno's place in the block; false when the block has no more.
 */
static bool smtable_scan_sample_next_tuple(TableScanDesc sscan,
                                           SampleScanState *scanstate,
                                           TupleTableSlot *slot) {
  smtable_scan_t *scan = (smtable_scan_t *)sscan;
  TsmRoutine *tsm = scanstate->tsmroutine;

  for (;;) {
    OffsetNumber offset;

    CHECK_FOR_INTERRUPTS();
    offset =
        tsm->NextSampleTuple(scanstate, scan->block, SMSTORE_ROWS_PER_BLOCK);
    if (!OffsetNumberIsValid(offset)) {
      ExecClearTuple(slot);
      return false;
    }
    if (smstore_scan_row(
            scan->scan,
            (uint64)scan->block * SMSTORE_ROWS_PER_BLOCK + offset - 1, slot))
      return true;
  }
}

/* End a scan, and let go of a snapshot taken for it alone. */
static void smtable_scan_end(TableScanDesc sscan) {
  smtable_scan_t *scan = (smtable_scan_t *)sscan;

  smstore_endscan(scan->scan);
  if (sscan->rs_flags & SO_TEMP_SNAPSHOT)
    UnregisterSnapshot(sscan->rs_snapshot);
  pfree(scan);
}

/* The scan's next row into slot; false at the end.  Forward only. */
static bool smtable_scan_getnextslot(TableScanDesc sscan,
                                     ScanDirection direction,
                                     TupleTableSlot *slot) {
  smtable_scan_t *scan = (smtable_scan_t *)sscan;

  if (!ScanDirectionIsForward(direction))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a scan of table \"%s\" runs forwards only",
                           RelationGetRelationName(sscan->rs_rd))));
  return smstore_getnext(scan->scan, slot);
}

/* Insert the slot's row: sm writes the table's indexes with it. */
static void smtable_tuple_insert(Relation rel, TupleTableSlot *slot,
                                 int options,
                                 struct BulkInsertStateData *bistate) {
  smstore_insert(rel, slot);
  slot->tts_tableOid = RelationGetRelid(rel);
}

/*
 * Hand the slots' rows to sm's bulk load of the table, which this begins:
 * COPY's batches come this way, and CREATE TABLE AS's rows.
 */
static void smtable_multi_insert(Relation rel, TupleTableSlot **slots,
                                 int nslots, int options,
                                 struct BulkInsertStateData *bistate) {
  smstore_load(rel, slots, nslots);
}

/* End the bulk load multi_insert began: COPY has no more rows. */
static void smtable_finish_bulk_insert(Relation rel, int options) {
  smstore_load_end(rel);
}

/*
 * Whether the row in slot is one snapshot sees: always.  A row trigger on
 * a table in sm is given its row from a tuple store, without its tid
 * (commands/trigger.c), and fires at its statement's end, as no deferred
 * one is made (smstore.c); RI_FKey_check() asks, of a row it is to check.
 */
static bool smtable_tuple_satisfies_snapshot(Relation, TupleTableSlot *,
                                             Snapshot) {
  return true;
}

/* The row at tid, as the statement sees it, or as it kept it (smstore.c). */
static bool smtable_fetch_row_version(Relation rel, ItemPointer tid,
                                      Snapshot snapshot, TupleTableSlot *slot) {
  return smstore_fetch_row(rel, tid, slot->tts_tupleDescriptor->natts, slot);
}

/*
 * Whether tid may be a row's: any may, and fetching it says whether the
 * statement sees one.  A TID scan asks, for WHERE CURRENT OF.
 */
static bool smtable_tuple_tid_valid(TableScanDesc, ItemPointer) {
  return true;
}

/* A row's newest version has its tid: sm updates a row at its rowno. */
static void smtable_tuple_get_latest_tid(TableScanDesc, ItemPointer) {}

/* Delete the row at tid: sm's snapshot and its waits are its own. */
static TM_Result smtable_tuple_delete(Relation rel, ItemPointer tid,
                                      Snapshot snapshot, Snapshot crosscheck,
                                      bool wait, TM_FailureData *tmfd,
                                      bool changingPart) {
  return smstore_delete(rel, tid, tmfd);
}

/*
 * Replace the row at otid with the slot's.  sm writes the table's indexes
 * itself, so the executor is told to write none.
 */
static TM_Result smtable_tuple_update(Relation rel, ItemPointer otid,
                                      TupleTableSlot *slot, Snapshot snapshot,
                                      Snapshot crosscheck, bool wait,
                                      TM_FailureData *tmfd,
                                      LockTupleMode *lockmode,
                                      TU_UpdateIndexes *update_indexes) {
  *lockmode = LockTupleExclusive;
  *update_indexes = TU_None;
  return smstore_update(rel, otid, slot, tmfd);
}

/*
 * Lock the row at tid and read its newest version: SELECT ... FOR UPDATE's
 * and FOR SHARE's, and an UPDATE's or DELETE's when a write found the row
 * changed.
 */
static TM_Result smtable_tuple_lock(Relation rel, ItemPointer tid,
                                    Snapshot snapshot, TupleTableSlot *slot,
                                    LockTupleMode mode,
                                    LockWaitPolicy wait_policy, uint8 flags,
                                    TM_FailureData *tmfd) {
  return smstore_lock_row(rel, tid, slot, mode, wait_policy, tmfd);
}

/* Every callback set, as GetTableAmRoutine() requires. */
static const TableAmRoutine smtable_methods = [] {
  TableAmRoutine r = {};

  r.type = T_TableAmRoutine;
  r.slot_callbacks = smtable_slot_callbacks;
  r.scan_begin = smtable_scan_begin;
  r.scan_end = smtable_scan_end;
  r.scan_rescan = smtable_scan_rescan;
  r.scan_getnextslot = smtable_scan_getnextslot;
  r.scan_set_tidrange = refuse;
  r.scan_getnextslot_tidrange = refuse;
  r.index_fetch_begin = refuse;
  r.index_fetch_reset = refuse;
  r.index_fetch_end = refuse;
  r.index_fetch_tuple = refuse;
  r.tuple_fetch_row_version = smtable_fetch_row_version;
  r.tuple_tid_valid = smtable_tuple_tid_valid;
  r.tuple_get_latest_tid = smtable_tuple_get_latest_tid;
  r.tuple_satisfies_snapshot = smtable_tuple_satisfies_snapshot;
  r.index_delete_tuples = refuse;
  r.tuple_insert = smtable_tuple_insert;
  r.tuple_insert_speculative = refuse;
  r.tuple_complete_speculative = refuse;
  r.multi_insert = smtable_multi_insert;
  r.finish_bulk_insert = smtable_finish_bulk_insert;
  r.tuple_delete = smtable_tuple_delete;
  r.tuple_update = smtable_tuple_update;
  r.tuple_lock = smtable_tuple_lock;
  r.relation_set_new_filelocator = refuse;
  r.relation_nontransactional_truncate = refuse;
  r.relation_copy_data = refuse;
  r.relation_copy_for_cluster = refuse;
  r.index_build_range_scan = refuse;
  r.index_validate_scan = refuse;
  r.relation_size = refuse;
  r.relation_needs_toast_table = refuse;
  r.relation_toast_am = refuse;
  r.relation_estimate_size = refuse;
  /* Set, so the planner plans bitmap scans; nodeBitmapHeapscan.c runs them. */
  r.scan_bitmap_next_tuple = refuse;
  r.scan_sample_next_block = smtable_scan_sample_next_block;
  r.scan_sample_next_tuple = smtable_scan_sample_next_tuple;
  return r;
}();

/* The table access method of a table in sm. */
const TableAmRoutine *smtable_routine(void) { return &smtable_methods; }

/*
 * The handler of pg_am's heap, the one table access method there is: a
 * table is a table in sm.
 */
Datum heap_tableam_handler(PG_FUNCTION_ARGS) {
  PG_RETURN_POINTER(&smtable_methods);
}
