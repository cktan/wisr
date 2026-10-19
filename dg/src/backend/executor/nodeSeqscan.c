/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a
 *relation. ExecSeqNext				retrieve next tuple in
 *sequential order. ExecInitSeqScan			creates and initializes
 *a seqscan node. ExecEndSeqScan			releases any storage
 *allocated. ExecReScanSeqScan		rescans the relation
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/smstore.h"
#include "access/sysattr.h"
#include "catalog/catrel.h"
#include "access/tableam.h"
#include "executor/execScan.h"
#include "executor/executor.h"
#include "executor/nodeSeqscan.h"
#include "optimizer/optimizer.h"
#include "utils/rel.h"

static TupleTableSlot *SeqNext(SeqScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *SeqNext(SeqScanState *node) {
  TableScanDesc scandesc;
  EState *estate;
  ScanDirection direction;
  TupleTableSlot *slot;

  /*
   * get information from the estate and scan state
   */
  scandesc = node->ss.ss_currentScanDesc;
  estate = node->ss.ps.state;
  direction = estate->es_direction;
  slot = node->ss.ss_ScanTupleSlot;

  /* A table in sm is read by sm, forwards. */
  if (RelationIsSm(node->ss.ss_currentRelation)) {
    if (!ScanDirectionIsForward(direction))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("a table scan runs forwards only")));
    if (node->sm_scan == NULL)
      node->sm_scan =
          smstore_beginscan(node->ss.ss_currentRelation, node->sm_natts);
    return smstore_getnext(node->sm_scan, slot) ? slot : NULL;
  }

  if (scandesc == NULL) {
    /*
     * We reach here if the scan is not parallel, or if we're serially
     * executing a scan that was planned to be parallel.
     */
    /* A catalog relation's rows may be narrowed (catalog/catrel.c). */
    scandesc =
        node->cat_keys != NIL
            ? catrel_beginscan(node->ss.ss_currentRelation, estate->es_snapshot,
                               node->cat_keys, node->ss.ps.ps_ExprContext)
            : table_beginscan(node->ss.ss_currentRelation, estate->es_snapshot,
                              0, NULL);
    node->ss.ss_currentScanDesc = scandesc;
  }

  /*
   * get the next tuple from the table
   */
  if (table_scan_getnextslot(scandesc, direction, slot))
    return slot;
  return NULL;
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool SeqRecheck(SeqScanState *node, TupleTableSlot *slot) {
  /*
   * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
   * (and this is very bad) - so, here we do not check are keys ok or not.
   */
  return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple. This variant is used when there is no es_epq_active, no
 *qual and no projection.  Passing const-NULLs for these to ExecScanExtended
 *		allows the compiler to eliminate the additional code that would
 *		ordinarily be required for the evaluation of these.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *ExecSeqScan(PlanState *pstate) {
  SeqScanState *node = castNode(SeqScanState, pstate);

  Assert(pstate->state->es_epq_active == NULL);
  Assert(pstate->qual == NULL);
  Assert(pstate->ps_ProjInfo == NULL);

  return ExecScanExtended(&node->ss, (ExecScanAccessMtd)SeqNext,
                          (ExecScanRecheckMtd)SeqRecheck, NULL, NULL, NULL);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation is required.
 */
static TupleTableSlot *ExecSeqScanWithQual(PlanState *pstate) {
  SeqScanState *node = castNode(SeqScanState, pstate);

  Assert(pstate->state->es_epq_active == NULL);
  Assert(pstate->qual != NULL);
  Assert(pstate->ps_ProjInfo == NULL);

  return ExecScanExtended(&node->ss, (ExecScanAccessMtd)SeqNext,
                          (ExecScanRecheckMtd)SeqRecheck, NULL, pstate->qual,
                          NULL);
}

/*
 * Variant of ExecSeqScan() but when projection is required.
 */
static TupleTableSlot *ExecSeqScanWithProject(PlanState *pstate) {
  SeqScanState *node = castNode(SeqScanState, pstate);

  Assert(pstate->state->es_epq_active == NULL);
  Assert(pstate->qual == NULL);
  Assert(pstate->ps_ProjInfo != NULL);

  return ExecScanExtended(&node->ss, (ExecScanAccessMtd)SeqNext,
                          (ExecScanRecheckMtd)SeqRecheck, NULL, NULL,
                          pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation and projection are
 * required.
 */
static TupleTableSlot *ExecSeqScanWithQualProject(PlanState *pstate) {
  SeqScanState *node = castNode(SeqScanState, pstate);

  Assert(pstate->state->es_epq_active == NULL);
  Assert(pstate->qual != NULL);
  Assert(pstate->ps_ProjInfo != NULL);

  return ExecScanExtended(&node->ss, (ExecScanAccessMtd)SeqNext,
                          (ExecScanRecheckMtd)SeqRecheck, NULL, pstate->qual,
                          pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan for when EPQ evaluation is required.  We don't
 * bother adding variants of this for with/without qual and projection as
 * EPQ doesn't seem as exciting a case to optimize for.
 */
static TupleTableSlot *ExecSeqScanEPQ(PlanState *pstate) {
  SeqScanState *node = castNode(SeqScanState, pstate);

  return ExecScan(&node->ss, (ExecScanAccessMtd)SeqNext,
                  (ExecScanRecheckMtd)SeqRecheck);
}

/*
 * The columns of each row of rel the scan reads, from the first: up to the
 * last its target list and quals name, or every one for a reference to the
 * whole row.  Its projection and quals fetch no more; a scan with no
 * projection names every column, as its target list is the table's.
 */
static int scan_natts(SeqScan *plan, Relation rel) {
  Bitmapset *attrs = NULL;
  int last = 0;
  int x = -1;

  pull_varattnos((Node *)plan->scan.plan.targetlist, plan->scan.scanrelid,
                 &attrs);
  pull_varattnos((Node *)plan->scan.plan.qual, plan->scan.scanrelid, &attrs);
  while ((x = bms_next_member(attrs, x)) >= 0) {
    AttrNumber attno = x + FirstLowInvalidHeapAttributeNumber;

    if (attno == InvalidAttrNumber)
      return RelationGetNumberOfAttributes(rel);
    last = Max(last, attno);
  }
  return last;
}

/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *ExecInitSeqScan(SeqScan *node, EState *estate, int eflags) {
  SeqScanState *scanstate;

  /*
   * Once upon a time it was possible to have an outerPlan of a SeqScan, but
   * not any more.
   */
  Assert(outerPlan(node) == NULL);
  Assert(innerPlan(node) == NULL);

  /*
   * create state structure
   */
  scanstate = makeNode(SeqScanState);
  scanstate->ss.ps.plan = (Plan *)node;
  scanstate->ss.ps.state = estate;

  /*
   * Miscellaneous initialization
   *
   * create expression context for node
   */
  ExecAssignExprContext(estate, &scanstate->ss.ps);

  /*
   * open the scan relation
   */
  scanstate->ss.ss_currentRelation =
      ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

  /* and create slot with the appropriate rowtype */
  ExecInitScanTupleSlot(
      estate, &scanstate->ss,
      RelationGetDescr(scanstate->ss.ss_currentRelation),
      RelationIsSm(scanstate->ss.ss_currentRelation)
          ? &TTSOpsVirtual
          : table_slot_callbacks(scanstate->ss.ss_currentRelation));

  /*
   * A scan of a table in sm begins as the plan does, not at its first row: a
   * cursor reads the rows there were when it was declared, by a scan that a
   * rollback to a savepoint set after it leaves open.
   */
  if (RelationIsSm(scanstate->ss.ss_currentRelation)) {
    scanstate->sm_natts = scan_natts(node, scanstate->ss.ss_currentRelation);
    if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
      scanstate->sm_scan = smstore_beginscan(scanstate->ss.ss_currentRelation,
                                             scanstate->sm_natts);
  }

  /*
   * Initialize result type and projection.
   */
  ExecInitResultTypeTL(&scanstate->ss.ps);
  ExecAssignScanProjectionInfo(&scanstate->ss);

  /*
   * initialize child expressions
   */
  scanstate->ss.ps.qual =
      ExecInitQual(node->scan.plan.qual, (PlanState *)scanstate);
  /* The quals that narrow the rows a catalog relation makes. */
  if (scanstate->ss.ss_currentRelation->rd_tableam == catrel_routine())
    scanstate->cat_keys =
        catrel_scan_keys(scanstate->ss.ss_currentRelation, node->scan.plan.qual,
                         node->scan.scanrelid, (PlanState *)scanstate);

  /*
   * When EvalPlanQual() is not in use, assign ExecProcNode for this node
   * based on the presence of qual and projection. Each ExecSeqScan*()
   * variant is optimized for the specific combination of these conditions.
   */
  if (scanstate->ss.ps.state->es_epq_active != NULL)
    scanstate->ss.ps.ExecProcNode = ExecSeqScanEPQ;
  else if (scanstate->ss.ps.qual == NULL) {
    if (scanstate->ss.ps.ps_ProjInfo == NULL)
      scanstate->ss.ps.ExecProcNode = ExecSeqScan;
    else
      scanstate->ss.ps.ExecProcNode = ExecSeqScanWithProject;
  } else {
    if (scanstate->ss.ps.ps_ProjInfo == NULL)
      scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQual;
    else
      scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQualProject;
  }

  return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void ExecEndSeqScan(SeqScanState *node) {
  TableScanDesc scanDesc;

  /*
   * get information from node
   */
  scanDesc = node->ss.ss_currentScanDesc;

  /*
   * close heap scan
   */
  if (scanDesc != NULL)
    table_endscan(scanDesc);
  if (node->sm_scan != NULL)
    smstore_endscan(node->sm_scan);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void ExecReScanSeqScan(SeqScanState *node) {
  TableScanDesc scan;

  scan = node->ss.ss_currentScanDesc;

  /*
   * A catalog relation narrowed by its keys begins again, so a parameter's
   * new value narrows it.
   */
  if (scan != NULL && node->cat_keys != NIL) {
    table_endscan(scan);
    node->ss.ss_currentScanDesc = NULL;
  } else if (scan != NULL)
    table_rescan(scan,  /* scan desc */
                 NULL); /* new scan keys */
  /* A scan of a table in sm begins again at the next row asked for. */
  if (node->sm_scan != NULL) {
    smstore_endscan(node->sm_scan);
    node->sm_scan = NULL;
  }

  ExecScanReScan((ScanState *)node);
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */
