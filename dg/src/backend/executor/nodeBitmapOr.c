/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.c
 *	  routines to handle BitmapOr nodes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapOr.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitBitmapOr	- initialize the BitmapOr node
 *		MultiExecBitmapOr	- retrieve the result bitmap from the
 *node ExecEndBitmapOr		- shut down the BitmapOr node ExecReScanBitmapOr
 *- rescan the BitmapOr node
 *
 *	 NOTES
 *		BitmapOr nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans,
 *		much like Append nodes.  The logic is much simpler than
 *		Append, however, since we needn't cope with forward/backward
 *		execution.
 */

#include "postgres.h"

#include "access/smstore.h"
#include "executor/executor.h"
#include "executor/nodeBitmapOr.h"
#include "miscadmin.h"
#include "nodes/rowbitmap.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 *		ExecBitmapOr
 *
 *		stub for pro forma compliance
 * ----------------------------------------------------------------
 */
static TupleTableSlot *ExecBitmapOr(PlanState *pstate) {
  elog(ERROR, "BitmapOr node does not support ExecProcNode call convention");
  return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapOr
 *
 *		Begin all of the subscans of the BitmapOr node.
 * ----------------------------------------------------------------
 */
BitmapOrState *ExecInitBitmapOr(BitmapOr *node, EState *estate, int eflags) {
  BitmapOrState *bitmaporstate = makeNode(BitmapOrState);
  PlanState **bitmapplanstates;
  int nplans;
  int i;
  ListCell *l;
  Plan *initNode;

  /* check for unsupported flags */
  Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

  /*
   * Set up empty vector of subplan states
   */
  nplans = list_length(node->bitmapplans);

  bitmapplanstates = (PlanState **)palloc0(nplans * sizeof(PlanState *));

  /*
   * create new BitmapOrState for our BitmapOr node
   */
  bitmaporstate->ps.plan = (Plan *)node;
  bitmaporstate->ps.state = estate;
  bitmaporstate->ps.ExecProcNode = ExecBitmapOr;
  bitmaporstate->bitmapplans = bitmapplanstates;
  bitmaporstate->nplans = nplans;

  /*
   * call ExecInitNode on each of the plans to be executed and save the
   * results into the array "bitmapplanstates".
   */
  i = 0;
  foreach (l, node->bitmapplans) {
    initNode = (Plan *)lfirst(l);
    bitmapplanstates[i] = ExecInitNode(initNode, estate, eflags);
    i++;
  }

  /*
   * Miscellaneous initialization
   *
   * BitmapOr plans don't have expression contexts because they never call
   * ExecQual or ExecProject.  They don't need any tuple slots either.
   */

  return bitmaporstate;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapOr
 * ----------------------------------------------------------------
 */
Node *MultiExecBitmapOr(BitmapOrState *node) {
  PlanState **bitmapplans;
  int nplans;
  int i;
  Node *result = NULL;

  /* must provide our own instrumentation support */
  if (node->ps.instrument)
    InstrStartNode(node->ps.instrument);

  /*
   * get information from the node
   */
  bitmapplans = node->bitmapplans;
  nplans = node->nplans;

  /*
   * Scan all the subplans and OR their result bitmaps
   */
  for (i = 0; i < nplans; i++) {
    PlanState *subnode = bitmapplans[i];
    Node *subresult;

    /*
     * We can special-case BitmapIndexScan children to avoid an explicit
     * tbm_union step for each child: just pass down the current result
     * bitmap and let the child OR directly into it.
     */
    if (IsA(subnode, BitmapIndexScanState)) {
      if (result == NULL) /* first subplan */
      {
        /* An index in sm gives rownos, not tids. */
        if (RelationIsSm(((BitmapIndexScanState *)subnode)->biss_RelationDesc))
          result = (Node *)rbm_create();
        else
          /* XXX should we use less than work_mem for this? */
          result = (Node *)tbm_create(work_mem * (Size)1024);
      }

      ((BitmapIndexScanState *)subnode)->biss_result = result;

      subresult = MultiExecProcNode(subnode);

      if (subresult != result)
        elog(ERROR, "unrecognized result from subplan");
    } else {
      /* standard implementation */
      subresult = MultiExecProcNode(subnode);

      /* Every subplan's is a TIDBitmap, or for a table in sm a RowBitmap. */
      if (!subresult ||
          !(IsA(subresult, TIDBitmap) || IsA(subresult, RowBitmap)))
        elog(ERROR, "unrecognized result from subplan");

      if (result == NULL)
        result = subresult; /* first subplan */
      else if (IsA(result, RowBitmap)) {
        rbm_union(castNode(RowBitmap, result), castNode(RowBitmap, subresult));
        rbm_free(castNode(RowBitmap, subresult));
      } else {
        tbm_union(castNode(TIDBitmap, result), castNode(TIDBitmap, subresult));
        tbm_free(castNode(TIDBitmap, subresult));
      }
    }
  }

  /* We could return an empty result set here? */
  if (result == NULL)
    elog(ERROR, "BitmapOr doesn't support zero inputs");

  /* must provide our own instrumentation support */
  if (node->ps.instrument)
    InstrStopNode(node->ps.instrument, 0 /* XXX */);

  return result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapOr
 *
 *		Shuts down the subscans of the BitmapOr node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void ExecEndBitmapOr(BitmapOrState *node) {
  PlanState **bitmapplans;
  int nplans;
  int i;

  /*
   * get information from the node
   */
  bitmapplans = node->bitmapplans;
  nplans = node->nplans;

  /*
   * shut down each of the subscans (that we've initialized)
   */
  for (i = 0; i < nplans; i++) {
    if (bitmapplans[i])
      ExecEndNode(bitmapplans[i]);
  }
}

void ExecReScanBitmapOr(BitmapOrState *node) {
  int i;

  for (i = 0; i < node->nplans; i++) {
    PlanState *subnode = node->bitmapplans[i];

    /*
     * ExecReScan doesn't know about my subplans, so I have to do
     * changed-parameter signaling myself.
     */
    if (node->ps.chgParam != NULL)
      UpdateChangedParamSet(subnode, node->ps.chgParam);

    /*
     * If chgParam of subnode is not null then plan will be re-scanned by
     * first ExecProcNode.
     */
    if (subnode->chgParam == NULL)
      ExecReScan(subnode);
  }
}
