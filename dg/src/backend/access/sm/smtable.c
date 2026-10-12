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
 * it first, in many places, and each is a callback here over smstore.c.
 * Every other callback raises an error: nothing should reach it.
 *
 * IDENTIFICATION
 *	  src/backend/access/sm/smtable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/smstore.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
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
  r.scan_begin = refuse;
  r.scan_end = refuse;
  r.scan_rescan = refuse;
  r.scan_getnextslot = refuse;
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
  r.tuple_insert = refuse;
  r.tuple_insert_speculative = refuse;
  r.tuple_complete_speculative = refuse;
  r.multi_insert = refuse;
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
