/*----------------------------------------------------------------------
 *
 * tableam.c
 *		Table access method routines too big to be inline functions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
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

#include "access/tableam.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "port/pg_bitutils.h"

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
  uint32 flags =
      SO_TYPE_SEQSCAN | SO_ALLOW_STRAT | SO_ALLOW_PAGEMODE | SO_TEMP_SNAPSHOT;
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
  table_tuple_insert(rel, slot, 0, NULL);
}

/*
 * simple_table_tuple_delete - delete a tuple
 *
 * This routine may be used to delete a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void simple_table_tuple_delete(Relation rel, const struct RowKey *key,
                               Snapshot snapshot) {
  TM_Result result;
  TM_FailureData tmfd;

  result = table_tuple_delete(rel, key, snapshot, InvalidSnapshot,
                              true /* wait for commit */, &tmfd,
                              false /* changingPart */);

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
void simple_table_tuple_update(Relation rel, const struct RowKey *key,
                               TupleTableSlot *slot, Snapshot snapshot,
                               TU_UpdateIndexes *update_indexes) {
  TM_Result result;
  TM_FailureData tmfd;
  LockTupleMode lockmode;

  result = table_tuple_update(rel, key, slot, snapshot, InvalidSnapshot,
                              true /* wait for commit */, &tmfd, &lockmode,
                              update_indexes);

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
