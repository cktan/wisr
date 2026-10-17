/*-------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.c
 *	  Routines to support index-only scans
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeIndexonlyscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecIndexOnlyScan			scans an index
 *		IndexOnlyNext				retrieve next tuple
 *		ExecInitIndexOnlyScan		creates and initializes state
 *info. ExecReScanIndexOnlyScan		rescans the indexed relation.
 *		ExecEndIndexOnlyScan		releases all storage.
 *		ExecIndexOnlyMarkPos		marks scan position.
 *		ExecIndexOnlyRestrPos		restores scan position.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/smstore.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/rel.h"

static TupleTableSlot *IndexOnlyNext(IndexOnlyScanState *node);
static void StoreIndexTuple(IndexOnlyScanState *node, TupleTableSlot *slot,
                            IndexScanDesc scandesc);

/* ----------------------------------------------------------------
 *		IndexOnlyNext
 *
 *		Retrieve a tuple from the IndexOnlyScan node's index.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *IndexOnlyNext(IndexOnlyScanState *node) {
  EState *estate;
  ExprContext *econtext;
  ScanDirection direction;
  IndexScanDesc scandesc;
  TupleTableSlot *slot;
  ItemPointer tid;

  /*
   * extract necessary information from index scan node
   */
  estate = node->ss.ps.state;

  /*
   * Determine which direction to scan the index in based on the plan's scan
   * direction and the current direction of execution.
   */
  direction = (ScanDirection)ScanDirectionCombine(
      estate->es_direction, ((IndexOnlyScan *)node->ss.ps.plan)->indexorderdir);
  scandesc = node->ioss_ScanDesc;
  econtext = node->ss.ps.ps_ExprContext;
  slot = node->ss.ss_ScanTupleSlot;

  /* An index of a table in sm is read by sm, forwards, its rows unread. */
  if (RelationIsSm(node->ss.ss_currentRelation)) {
    if (!ScanDirectionIsForward(direction))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index scan runs forwards only")));
    if (node->ioss_SmScan == NULL)
      node->ioss_SmScan = smstore_index_beginscan(
          node->ss.ss_currentRelation, node->ioss_RelationDesc,
          node->ioss_ScanKeys, node->ioss_NumScanKeys);
    return smstore_index_getentry(node->ioss_SmScan, slot) ? slot : NULL;
  }

  if (scandesc == NULL) {
    /*
     * We reach here if the index only scan is not parallel, or if we're
     * serially executing an index only scan that was planned to be
     * parallel.
     */
    scandesc =
        index_beginscan(node->ss.ss_currentRelation, node->ioss_RelationDesc,
                        estate->es_snapshot, &node->ioss_Instrument,
                        node->ioss_NumScanKeys, node->ioss_NumOrderByKeys);

    node->ioss_ScanDesc = scandesc;

    /* Set it up for index-only scan */
    node->ioss_ScanDesc->xs_want_itup = true;

    /*
     * If no run-time keys to calculate or they are ready, go ahead and
     * pass the scankeys to the index AM.
     */
    if (node->ioss_NumRuntimeKeys == 0 || node->ioss_RuntimeKeysReady)
      index_rescan(scandesc, node->ioss_ScanKeys, node->ioss_NumScanKeys,
                   node->ioss_OrderByKeys, node->ioss_NumOrderByKeys);
  }

  /*
   * OK, now that we have what we need, fetch the next tuple.
   */
  while ((tid = index_getnext_tid(scandesc, direction)) != NULL) {
    bool tuple_from_heap = false;

    CHECK_FOR_INTERRUPTS();

    /* No page is known all-visible: visit the table to check. */
    {
      /*
       * Rats, we have to visit the heap to check visibility.
       */
      InstrCountTuples2(node, 1);
      if (!index_fetch_heap(scandesc, node->ioss_TableSlot))
        continue; /* no visible tuple, try next index entry */

      ExecClearTuple(node->ioss_TableSlot);

      /*
       * Only MVCC snapshots are supported here, so there should be no
       * need to keep following the HOT chain once a visible entry has
       * been found.  If we did want to allow that, we'd need to keep
       * more state to remember not to call index_getnext_tid next time.
       */
      if (scandesc->xs_heap_continue)
        elog(ERROR, "non-MVCC snapshots are not supported in index-only scans");

      /*
       * Note: at this point we are holding a pin on the heap page, as
       * recorded in scandesc->xs_cbuf.  We could release that pin now,
       * but it's not clear whether it's a win to do so.  The next index
       * entry might require a visit to the same heap page.
       */

      tuple_from_heap = true;
    }

    /* Fill the scan tuple slot with data from the index */
    StoreIndexTuple(node, slot, scandesc);

    /*
     * If the index was lossy, we have to recheck the index quals.
     */
    if (scandesc->xs_recheck) {
      econtext->ecxt_scantuple = slot;
      if (!ExecQualAndReset(node->recheckqual, econtext)) {
        /* Fails recheck, so drop it and loop back for another */
        InstrCountFiltered2(node, 1);
        continue;
      }
    }

    /*
     * We don't currently support rechecking ORDER BY distances.  (In
     * principle, if the index can support retrieval of the originally
     * indexed value, it should be able to produce an exact distance
     * calculation too.  So it's not clear that adding code here for
     * recheck/re-sort would be worth the trouble.  But we should at least
     * throw an error if someone tries it.)
     */
    if (scandesc->numberOfOrderBys > 0 && scandesc->xs_recheckorderby)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("lossy distance functions are not supported in "
                             "index-only scans")));

    return slot;
  }

  /*
   * if we get here it means the index scan failed so we are at the end of
   * the scan..
   */
  return ExecClearTuple(slot);
}

/*
 * StoreIndexTuple
 *		Fill the slot with the data the index AM returned.
 *
 * The data might be provided in either HeapTuple (xs_hitup) or IndexTuple
 * (xs_itup) format.  Conceivably an index AM might fill both fields, in which
 * case we prefer the heap format, since it's probably a bit cheaper to fill a
 * slot from.
 *
 * At some point this might be generally-useful functionality, but
 * right now we don't need it elsewhere.
 */
static void StoreIndexTuple(IndexOnlyScanState *node, TupleTableSlot *slot,
                            IndexScanDesc scandesc) {
  ExecClearTuple(slot);

  /*
   * We must deform the tuple using the tupdesc the index AM formed it with
   * (xs_hitupdesc or xs_itupdesc), not the slot's tupdesc.  The datums
   * returned by the index AM must be binary compatible, but the descriptors
   * may align each column differently in certain rare cases. (Actually,
   * btree's "name" opclass stores cstring tuples that _aren't_ even binary
   * compatible, in the strictest sense.  We directly handle that here.)
   */
  if (scandesc->xs_hitup) {
    Assert(slot->tts_tupleDescriptor->natts == scandesc->xs_hitupdesc->natts);

    heap_deform_tuple(scandesc->xs_hitup, scandesc->xs_hitupdesc,
                      slot->tts_values, slot->tts_isnull);
  } else if (scandesc->xs_itup) {
    Assert(slot->tts_tupleDescriptor->natts == scandesc->xs_itupdesc->natts);

    index_deform_tuple(scandesc->xs_itup, scandesc->xs_itupdesc,
                       slot->tts_values, slot->tts_isnull);

    /*
     * Copy all name columns stored as cstrings back into a NAMEDATALEN
     * byte sized allocation.  We mark this branch as unlikely as
     * generally "name" is used only for the system catalogs and this
     * would have to be a user query running on those or some other user
     * table with an index on a name column.
     */
    if (unlikely(node->ioss_NameCStringAttNums != NULL)) {
      int attcount = node->ioss_NameCStringCount;

      for (int idx = 0; idx < attcount; idx++) {
        int attnum = node->ioss_NameCStringAttNums[idx];
        Name name;

        /* skip null Datums */
        if (slot->tts_isnull[attnum])
          continue;

        /*
         * allocate the NAMEDATALEN and copy the datum into that
         * memory
         */
        name = (Name)MemoryContextAlloc(
            node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory, NAMEDATALEN);

        /* use namestrcpy to zero-pad all trailing bytes */
        namestrcpy(name, DatumGetCString(slot->tts_values[attnum]));
        slot->tts_values[attnum] = NameGetDatum(name);
      }
    }
  } else
    elog(ERROR, "no data returned for index-only scan");

  ExecStoreVirtualTuple(slot);
}

/*
 * IndexOnlyRecheck -- access method routine to recheck a tuple in EvalPlanQual
 *
 * This can't really happen, since an index can't supply CTID which would
 * be necessary data for any potential EvalPlanQual target relation.  If it
 * did happen, the EPQ code would pass us the wrong data, namely a heap
 * tuple not an index tuple.  So throw an error.
 */
static bool IndexOnlyRecheck(IndexOnlyScanState *node, TupleTableSlot *slot) {
  elog(ERROR, "EvalPlanQual recheck is not supported in index-only scans");
  return false; /* keep compiler quiet */
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScan(node)
 * ----------------------------------------------------------------
 */
static TupleTableSlot *ExecIndexOnlyScan(PlanState *pstate) {
  IndexOnlyScanState *node = castNode(IndexOnlyScanState, pstate);

  /*
   * If we have runtime keys and they've not already been set up, do it now.
   */
  if (node->ioss_NumRuntimeKeys != 0 && !node->ioss_RuntimeKeysReady)
    ExecReScan((PlanState *)node);

  return ExecScan(&node->ss, (ExecScanAccessMtd)IndexOnlyNext,
                  (ExecScanRecheckMtd)IndexOnlyRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanIndexOnlyScan(node)
 *
 *		Recalculates the values of any scan keys whose value depends on
 *		information known at runtime, then rescans the indexed relation.
 *
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 * ----------------------------------------------------------------
 */
void ExecReScanIndexOnlyScan(IndexOnlyScanState *node) {
  /*
   * If we are doing runtime key calculations (ie, any of the index key
   * values weren't simple Consts), compute the new key values.  But first,
   * reset the context so we don't leak memory as each outer tuple is
   * scanned.  Note this assumes that we will recalculate *all* runtime keys
   * on each call.
   */
  if (node->ioss_NumRuntimeKeys != 0) {
    ExprContext *econtext = node->ioss_RuntimeContext;

    ResetExprContext(econtext);
    ExecIndexEvalRuntimeKeys(econtext, node->ioss_RuntimeKeys,
                             node->ioss_NumRuntimeKeys);
  }
  node->ioss_RuntimeKeysReady = true;

  /* reset index scan */
  if (node->ioss_ScanDesc)
    index_rescan(node->ioss_ScanDesc, node->ioss_ScanKeys,
                 node->ioss_NumScanKeys, node->ioss_OrderByKeys,
                 node->ioss_NumOrderByKeys);
  /* sm's scan begins again, with the keys as they are now, when next read. */
  if (node->ioss_SmScan != NULL) {
    smstore_endscan(node->ioss_SmScan);
    node->ioss_SmScan = NULL;
  }

  ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndIndexOnlyScan
 * ----------------------------------------------------------------
 */
void ExecEndIndexOnlyScan(IndexOnlyScanState *node) {
  Relation indexRelationDesc;
  IndexScanDesc indexScanDesc;

  /*
   * extract information from the node
   */
  indexRelationDesc = node->ioss_RelationDesc;
  indexScanDesc = node->ioss_ScanDesc;

  /*
   * close the index relation (no-op if we didn't open it)
   */
  if (indexScanDesc)
    index_endscan(indexScanDesc);
  if (node->ioss_SmScan != NULL)
    smstore_endscan(node->ioss_SmScan);
  if (indexRelationDesc)
    index_close(indexRelationDesc, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyMarkPos
 *
 * Note: we assume that no caller attempts to set a mark before having read
 * at least one tuple.  Otherwise, ioss_ScanDesc might still be NULL.
 * ----------------------------------------------------------------
 */
void ExecIndexOnlyMarkPos(IndexOnlyScanState *node) {
  EState *estate = node->ss.ps.state;
  EPQState *epqstate = estate->es_epq_active;

  if (epqstate != NULL) {
    /*
     * We are inside an EvalPlanQual recheck.  If a test tuple exists for
     * this relation, then we shouldn't access the index at all.  We would
     * instead need to save, and later restore, the state of the
     * relsubs_done flag, so that re-fetching the test tuple is possible.
     * However, given the assumption that no caller sets a mark at the
     * start of the scan, we can only get here with relsubs_done[i]
     * already set, and so no state need be saved.
     */
    Index scanrelid = ((Scan *)node->ss.ps.plan)->scanrelid;

    Assert(scanrelid > 0);
    if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
        epqstate->relsubs_rowmark[scanrelid - 1] != NULL) {
      /* Verify the claim above */
      if (!epqstate->relsubs_done[scanrelid - 1])
        elog(ERROR, "unexpected ExecIndexOnlyMarkPos call in EPQ recheck");
      return;
    }
  }

  index_markpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyRestrPos
 * ----------------------------------------------------------------
 */
void ExecIndexOnlyRestrPos(IndexOnlyScanState *node) {
  EState *estate = node->ss.ps.state;
  EPQState *epqstate = estate->es_epq_active;

  if (estate->es_epq_active != NULL) {
    /* See comments in ExecIndexMarkPos */
    Index scanrelid = ((Scan *)node->ss.ps.plan)->scanrelid;

    Assert(scanrelid > 0);
    if (epqstate->relsubs_slot[scanrelid - 1] != NULL ||
        epqstate->relsubs_rowmark[scanrelid - 1] != NULL) {
      /* Verify the claim above */
      if (!epqstate->relsubs_done[scanrelid - 1])
        elog(ERROR, "unexpected ExecIndexOnlyRestrPos call in EPQ recheck");
      return;
    }
  }

  index_restrpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexOnlyScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relation.
 * ----------------------------------------------------------------
 */
IndexOnlyScanState *ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate,
                                          int eflags) {
  IndexOnlyScanState *indexstate;
  Relation currentRelation;
  Relation indexRelation;
  LOCKMODE lockmode;
  TupleDesc tupDesc;
  int indnkeyatts;
  int namecount;

  /*
   * create state structure
   */
  indexstate = makeNode(IndexOnlyScanState);
  indexstate->ss.ps.plan = (Plan *)node;
  indexstate->ss.ps.state = estate;
  indexstate->ss.ps.ExecProcNode = ExecIndexOnlyScan;

  /*
   * Miscellaneous initialization
   *
   * create expression context for node
   */
  ExecAssignExprContext(estate, &indexstate->ss.ps);

  /*
   * open the scan relation
   */
  currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

  indexstate->ss.ss_currentRelation = currentRelation;
  indexstate->ss.ss_currentScanDesc = NULL; /* no heap scan here */

  /*
   * Build the scan tuple type using the indextlist generated by the
   * planner.  We use this, rather than the index's physical tuple
   * descriptor, because the latter contains storage column types not the
   * types of the original datums.  (It's the AM's responsibility to return
   * suitable data anyway.)
   */
  tupDesc = ExecTypeFromTL(node->indextlist);
  ExecInitScanTupleSlot(estate, &indexstate->ss, tupDesc, &TTSOpsVirtual);

  /*
   * We need another slot, in a format that's suitable for the table AM, for
   * when we need to fetch a tuple from the table for rechecking visibility.
   */
  indexstate->ioss_TableSlot = ExecAllocTableSlot(
      &estate->es_tupleTable, RelationGetDescr(currentRelation),
      table_slot_callbacks(currentRelation));

  /*
   * Initialize result type and projection info.  The node's targetlist will
   * contain Vars with varno = INDEX_VAR, referencing the scan tuple.
   */
  ExecInitResultTypeTL(&indexstate->ss.ps);
  ExecAssignScanProjectionInfoWithVarno(&indexstate->ss, INDEX_VAR);

  /*
   * initialize child expressions
   *
   * Note: we don't initialize all of the indexorderby expression, only the
   * sub-parts corresponding to runtime keys (see below).
   */
  indexstate->ss.ps.qual =
      ExecInitQual(node->scan.plan.qual, (PlanState *)indexstate);
  indexstate->recheckqual =
      ExecInitQual(node->recheckqual, (PlanState *)indexstate);

  /*
   * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
   * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
   * references to nonexistent indexes.
   */
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    return indexstate;

  /* Open the index relation. */
  lockmode = exec_rt_fetch(node->scan.scanrelid, estate)->rellockmode;
  indexRelation = index_open(node->indexid, lockmode);
  indexstate->ioss_RelationDesc = indexRelation;

  /*
   * Initialize index-specific scan state
   */
  indexstate->ioss_RuntimeKeysReady = false;
  indexstate->ioss_RuntimeKeys = NULL;
  indexstate->ioss_NumRuntimeKeys = 0;

  /*
   * build the index scan keys from the index qualification
   */
  ExecIndexBuildScanKeys(
      (PlanState *)indexstate, indexRelation, node->indexqual, false,
      &indexstate->ioss_ScanKeys, &indexstate->ioss_NumScanKeys,
      &indexstate->ioss_RuntimeKeys, &indexstate->ioss_NumRuntimeKeys,
      NULL, /* no ArrayKeys */
      NULL);

  /*
   * any ORDER BY exprs have to be turned into scankeys in the same way
   */
  ExecIndexBuildScanKeys(
      (PlanState *)indexstate, indexRelation, node->indexorderby, true,
      &indexstate->ioss_OrderByKeys, &indexstate->ioss_NumOrderByKeys,
      &indexstate->ioss_RuntimeKeys, &indexstate->ioss_NumRuntimeKeys,
      NULL, /* no ArrayKeys */
      NULL);

  /*
   * If we have runtime keys, we need an ExprContext to evaluate them. The
   * node's standard context won't do because we want to reset that context
   * for every tuple.  So, build another context just like the other one...
   * -tgl 7/11/00
   */
  if (indexstate->ioss_NumRuntimeKeys != 0) {
    ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

    ExecAssignExprContext(estate, &indexstate->ss.ps);
    indexstate->ioss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
    indexstate->ss.ps.ps_ExprContext = stdecontext;
  } else {
    indexstate->ioss_RuntimeContext = NULL;
  }

  indexstate->ioss_NameCStringAttNums = NULL;
  indnkeyatts = indexRelation->rd_index->indnkeyatts;
  namecount = 0;

  /*
   * The "name" type for btree uses text_ops which results in storing
   * cstrings in the indexed keys rather than names.  Here we detect that in
   * a generic way in case other index AMs want to do the same optimization.
   * Check for opclasses with an opcintype of NAMEOID and an index tuple
   * descriptor with CSTRINGOID.  If any of these are found, create an array
   * marking the index attribute number of each of them.  StoreIndexTuple()
   * handles copying the name Datums into a NAMEDATALEN-byte allocation.
   */

  /* First, count the number of such index keys */
  for (int attnum = 0; attnum < indnkeyatts; attnum++) {
    if (TupleDescAttr(indexRelation->rd_att, attnum)->atttypid == CSTRINGOID &&
        indexRelation->rd_opcintype[attnum] == NAMEOID)
      namecount++;
  }

  if (namecount > 0) {
    int idx = 0;

    /*
     * Now create an array to mark the attribute numbers of the keys that
     * need to be converted from cstring to name.
     */
    indexstate->ioss_NameCStringAttNums =
        (AttrNumber *)palloc(sizeof(AttrNumber) * namecount);

    for (int attnum = 0; attnum < indnkeyatts; attnum++) {
      if (TupleDescAttr(indexRelation->rd_att, attnum)->atttypid ==
              CSTRINGOID &&
          indexRelation->rd_opcintype[attnum] == NAMEOID)
        indexstate->ioss_NameCStringAttNums[idx++] = (AttrNumber)attnum;
    }
  }

  indexstate->ioss_NameCStringCount = namecount;

  /* One of a table in sm begins now, as an index scan's (nodeIndexscan.c). */
  if (RelationIsSm(indexstate->ss.ss_currentRelation) &&
      indexstate->ioss_NumRuntimeKeys == 0)
    indexstate->ioss_SmScan = smstore_index_beginscan(
        indexstate->ss.ss_currentRelation, indexstate->ioss_RelationDesc,
        indexstate->ioss_ScanKeys, indexstate->ioss_NumScanKeys);

  /*
   * all done.
   */
  return indexstate;
}

/* ----------------------------------------------------------------
 *		Parallel Index-only Scan Support
 * ----------------------------------------------------------------
 */
