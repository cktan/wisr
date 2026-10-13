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
 * method.  Every other callback raises an error: nothing should reach it.
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
#include "executor/tuptable.h"
#include "fmgr.h"
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

/* A scan of a table in sm by the access method: sm's cursor behind it. */
typedef struct smtable_scan_t {
  TableScanDescData base; /* what tableam.h reads */
  smstore_scan_t *scan;
} smtable_scan_t;

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
  scan->scan = smstore_beginscan(rel);
  return &scan->base;
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
                                 CommandId cid, int options,
                                 struct BulkInsertStateData *bistate) {
  smstore_insert(rel, slot);
  slot->tts_tableOid = RelationGetRelid(rel);
}

/*
 * Hand the slots' rows to sm's bulk load of the table, which this begins:
 * COPY's batches come this way, and nothing else does.
 */
static void smtable_multi_insert(Relation rel, TupleTableSlot **slots,
                                 int nslots, CommandId cid, int options,
                                 struct BulkInsertStateData *bistate) {
  smstore_load(rel, slots, nslots);
}

/* End the bulk load multi_insert began: COPY has no more rows. */
static void smtable_finish_bulk_insert(Relation rel, int options) {
  smstore_load_end(rel);
}

/* The row at tid, as the statement sees it, or as it kept it (smstore.c). */
static bool smtable_fetch_row_version(Relation rel, ItemPointer tid,
                                      Snapshot snapshot, TupleTableSlot *slot) {
  return smstore_fetch_row(rel, tid, slot);
}

/* Delete the row at tid: sm's snapshot and its waits are its own. */
static TM_Result smtable_tuple_delete(Relation rel, ItemPointer tid,
                                      CommandId cid, Snapshot snapshot,
                                      Snapshot crosscheck, bool wait,
                                      TM_FailureData *tmfd, bool changingPart) {
  return smstore_delete(rel, tid, cid, tmfd);
}

/*
 * Replace the row at otid with the slot's.  sm writes the table's indexes
 * itself, so the executor is told to write none.
 */
static TM_Result smtable_tuple_update(Relation rel, ItemPointer otid,
                                      TupleTableSlot *slot, CommandId cid,
                                      Snapshot snapshot, Snapshot crosscheck,
                                      bool wait, TM_FailureData *tmfd,
                                      LockTupleMode *lockmode,
                                      TU_UpdateIndexes *update_indexes) {
  *lockmode = LockTupleExclusive;
  *update_indexes = TU_None;
  return smstore_update(rel, otid, slot, cid, tmfd);
}

/* The newest version of the row at tid, which a write found changed. */
static TM_Result smtable_tuple_lock(Relation rel, ItemPointer tid,
                                    Snapshot snapshot, TupleTableSlot *slot,
                                    CommandId cid, LockTupleMode mode,
                                    LockWaitPolicy wait_policy, uint8 flags,
                                    TM_FailureData *tmfd) {
  return smstore_lock_latest(rel, tid, slot, tmfd);
}

/* Every callback set, as GetTableAmRoutine() requires. */
static const TableAmRoutine smtable_methods = [] {
  TableAmRoutine r = {};

  r.type = T_TableAmRoutine;
  r.slot_callbacks = smtable_slot_callbacks;
  r.scan_begin = smtable_scan_begin;
  r.scan_end = smtable_scan_end;
  r.scan_rescan = refuse;
  r.scan_getnextslot = smtable_scan_getnextslot;
  r.scan_set_tidrange = refuse;
  r.scan_getnextslot_tidrange = refuse;
  r.index_fetch_begin = refuse;
  r.index_fetch_reset = refuse;
  r.index_fetch_end = refuse;
  r.index_fetch_tuple = refuse;
  r.tuple_fetch_row_version = smtable_fetch_row_version;
  r.tuple_tid_valid = refuse;
  r.tuple_get_latest_tid = refuse;
  r.tuple_satisfies_snapshot = refuse;
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
  r.relation_fetch_toast_slice = refuse;
  r.relation_estimate_size = refuse;
  /* Set, so the planner plans bitmap scans; nodeBitmapHeapscan.c runs them. */
  r.scan_bitmap_next_tuple = refuse;
  r.scan_sample_next_block = refuse;
  r.scan_sample_next_tuple = refuse;
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
