/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan			scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecReScanTidScan	rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 */
#include "postgres.h"

#include "access/rowkey.h"
#include "access/smstore.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeTidscan.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * It's sufficient to check varattno to identify the rowno variable, which
 * names a row of a table in sm without a primary key, as any Var in the
 * relation scan qual must be for our table.  digres has no CTID.
 */
#define IsRownoVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == RownoAttributeNumber)

/* one element in tss_tidexprs */
typedef struct TidExpr {
  ExprState *exprstate; /* ExprState for a rowno-yielding subexpr */
  bool isarray;         /* if true, it yields an array of rownos */
  Oid rownotype;        /* the rownos' integer type */
  CurrentOfExpr *cexpr; /* alternatively, we can have CURRENT OF */
} TidExpr;

static void TidExprListCreate(TidScanState *tidstate);
static void TidListEval(TidScanState *tidstate);
static int rowno_comparator(const void *a, const void *b);
static TupleTableSlot *TidNext(TidScanState *node);
static bool RownoKey(Datum value, Oid typid, RowKey *key);

/*
 * Extract the qual subexpressions that yield TIDs to search for,
 * and compile them into ExprStates if they're ordinary expressions.
 *
 * CURRENT OF is a special case that we can't compile usefully;
 * just drop it into the TidExpr list as-is.
 */
static void TidExprListCreate(TidScanState *tidstate) {
  TidScan *node = (TidScan *)tidstate->ss.ps.plan;
  ListCell *l;

  tidstate->tss_tidexprs = NIL;
  tidstate->tss_isCurrentOf = false;

  foreach (l, node->tidquals) {
    Expr *expr = (Expr *)lfirst(l);
    TidExpr *tidexpr = (TidExpr *)palloc0(sizeof(TidExpr));

    if (is_opclause(expr)) {
      Node *arg1;
      Node *arg2;

      arg1 = get_leftop(expr);
      arg2 = get_rightop(expr);
      if (IsRownoVar(arg1)) {
        tidexpr->exprstate = ExecInitExpr((Expr *)arg2, &tidstate->ss.ps);
        tidexpr->rownotype = exprType(arg2);
      } else if (IsRownoVar(arg2)) {
        tidexpr->exprstate = ExecInitExpr((Expr *)arg1, &tidstate->ss.ps);
        tidexpr->rownotype = exprType(arg1);
      } else
        elog(ERROR, "could not identify rowno variable");
      tidexpr->isarray = false;
    } else if (expr && IsA(expr, ScalarArrayOpExpr)) {
      ScalarArrayOpExpr *saex = (ScalarArrayOpExpr *)expr;

      Assert(IsRownoVar(linitial(saex->args)));
      tidexpr->exprstate =
          ExecInitExpr((Expr *)lsecond(saex->args), &tidstate->ss.ps);
      tidexpr->isarray = true;
      tidexpr->rownotype =
          get_element_type(exprType((Node *)lsecond(saex->args)));
    } else if (expr && IsA(expr, CurrentOfExpr)) {
      CurrentOfExpr *cexpr = (CurrentOfExpr *)expr;

      tidexpr->cexpr = cexpr;
      tidstate->tss_isCurrentOf = true;
    } else
      elog(ERROR, "could not identify rowno expression");

    tidstate->tss_tidexprs = lappend(tidstate->tss_tidexprs, tidexpr);
  }

  /* CurrentOfExpr could never appear OR'd with something else */
  Assert(list_length(tidstate->tss_tidexprs) == 1 ||
         !tidstate->tss_isCurrentOf);
}

/*
 * Compute the list of rows to be visited, by evaluating the expressions for
 * them: each a row's key (access/rowkey.h), a keyless table's rowno, or the
 * row a cursor is on, copied.
 *
 * (The result is actually an array, not a list.)
 */
static void TidListEval(TidScanState *tidstate) {
  ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
  Relation rel = tidstate->ss.ss_currentRelation;
  RowKey *keyList;
  int numAllocKeys;
  int numKeys;
  ListCell *l;

  /*
   * Start scan on-demand - initializing a scan isn't free (e.g. heap stats
   * the size of the table), so it makes sense to delay that until needed -
   * the node might never get executed.
   */
  if (tidstate->ss.ss_currentScanDesc == NULL)
    tidstate->ss.ss_currentScanDesc =
        table_beginscan_tid(rel, tidstate->ss.ps.state->es_snapshot);

  /*
   * We initialize the array with enough slots for the case that all quals
   * are simple OpExprs or CurrentOfExprs.  If there are any
   * ScalarArrayOpExprs, we may have to enlarge the array.
   */
  numAllocKeys = list_length(tidstate->tss_tidexprs);
  keyList = (RowKey *)palloc(numAllocKeys * sizeof(RowKey));
  numKeys = 0;

  foreach (l, tidstate->tss_tidexprs) {
    TidExpr *tidexpr = (TidExpr *)lfirst(l);
    bool isNull;

    if (tidexpr->exprstate && !tidexpr->isarray) {
      Datum value =
          ExecEvalExprSwitchContext(tidexpr->exprstate, econtext, &isNull);

      if (isNull || !RownoKey(value, tidexpr->rownotype, &keyList[numKeys]))
        continue;
      if (++numKeys >= numAllocKeys) {
        numAllocKeys *= 2;
        keyList = (RowKey *)repalloc(keyList, numAllocKeys * sizeof(RowKey));
      }
    } else if (tidexpr->exprstate && tidexpr->isarray) {
      Datum arraydatum;
      ArrayType *itemarray;
      Datum *ipdatums;
      bool *ipnulls;
      int ndatums;
      int i;
      int16 typlen;
      bool typbyval;
      char typalign;

      arraydatum =
          ExecEvalExprSwitchContext(tidexpr->exprstate, econtext, &isNull);
      if (isNull)
        continue;
      itemarray = DatumGetArrayTypeP(arraydatum);
      get_typlenbyvalalign(tidexpr->rownotype, &typlen, &typbyval, &typalign);
      deconstruct_array(itemarray, tidexpr->rownotype, typlen, typbyval,
                        typalign, &ipdatums, &ipnulls, &ndatums);
      if (numKeys + ndatums >= numAllocKeys) {
        numAllocKeys = numKeys + ndatums + 1;
        keyList = (RowKey *)repalloc(keyList, numAllocKeys * sizeof(RowKey));
      }
      for (i = 0; i < ndatums; i++) {
        if (!ipnulls[i] &&
            RownoKey(ipdatums[i], tidexpr->rownotype, &keyList[numKeys]))
          numKeys++;
      }
      pfree(ipdatums);
      pfree(ipnulls);
    } else {
      RowKey cursor_key;

      Assert(tidexpr->cexpr);
      if (execCurrentOf(tidexpr->cexpr, econtext, RelationGetRelid(rel),
                        &cursor_key)) {
        /* The cursor's row's values last only while it is on it. */
        RowKeyCopy(rel, &cursor_key, &keyList[numKeys]);
        if (++numKeys >= numAllocKeys) {
          numAllocKeys *= 2;
          keyList = (RowKey *)repalloc(keyList, numAllocKeys * sizeof(RowKey));
        }
      }
    }
  }

  /*
   * Sort the array of rownos into order, and eliminate duplicates.
   * Eliminating duplicates is necessary since we want OR semantics across
   * the list.  Sorting makes it easier to detect duplicates, and as a bonus
   * ensures that we will visit the table in key order.
   */
  if (numKeys > 1) {
    /* CurrentOfExpr could never appear OR'd with something else */
    Assert(!tidstate->tss_isCurrentOf);

    qsort(keyList, numKeys, sizeof(RowKey), rowno_comparator);
    numKeys = qunique(keyList, numKeys, sizeof(RowKey), rowno_comparator);
  }

  tidstate->tss_TidList = keyList;
  tidstate->tss_NumTids = numKeys;
  tidstate->tss_TidPtr = -1;
}

/*
 * The key of the row of a keyless table whose rowno is value, of the integer
 * type typid, into *key; false if no row has it, as for one not positive.
 */
static bool RownoKey(Datum value, Oid typid, RowKey *key) {
  int64 rowno;

  switch (typid) {
  case INT2OID:
    rowno = DatumGetInt16(value);
    break;
  case INT4OID:
    rowno = DatumGetInt32(value);
    break;
  default:
    rowno = DatumGetInt64(value);
    break;
  }
  if (rowno <= 0)
    return false;
  key->nkeys = 1;
  key->values[0] = Int64GetDatum(rowno);
  key->isnull[0] = false;
  return true;
}

/*
 * qsort comparator for the keys of rows of a keyless table, by rowno
 */
static int rowno_comparator(const void *a, const void *b) {
  int64 ra = DatumGetInt64(((const RowKey *)a)->values[0]);
  int64 rb = DatumGetInt64(((const RowKey *)b)->values[0]);

  return ra < rb ? -1 : ra > rb ? 1 : 0;
}

/* ----------------------------------------------------------------
 *		TidNext
 *
 *		Retrieve a tuple from the TidScan node's currentRelation
 *		using the tids in the TidScanState information.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *TidNext(TidScanState *node) {
  EState *estate;
  ScanDirection direction;
  Snapshot snapshot;
  Relation heapRelation;
  TupleTableSlot *slot;
  RowKey *keyList;
  int numKeys;
  bool bBackward;

  /*
   * extract necessary information from tid scan node
   */
  estate = node->ss.ps.state;
  direction = estate->es_direction;
  snapshot = estate->es_snapshot;
  heapRelation = node->ss.ss_currentRelation;
  slot = node->ss.ss_ScanTupleSlot;

  /*
   * First time through, compute the list of rows to be visited
   */
  if (node->tss_TidList == NULL)
    TidListEval(node);

  keyList = node->tss_TidList;
  numKeys = node->tss_NumTids;

  /*
   * Initialize or advance scan position, depending on direction.
   */
  bBackward = ScanDirectionIsBackward(direction);
  if (bBackward) {
    if (node->tss_TidPtr < 0) {
      /* initialize for backward scan */
      node->tss_TidPtr = numKeys - 1;
    } else
      node->tss_TidPtr--;
  } else {
    if (node->tss_TidPtr < 0) {
      /* initialize for forward scan */
      node->tss_TidPtr = 0;
    } else
      node->tss_TidPtr++;
  }

  while (node->tss_TidPtr >= 0 && node->tss_TidPtr < numKeys) {
    RowKey *key = &keyList[node->tss_TidPtr];

    /*
     * A table in sm's row is read up to the last column the plan names,
     * as the statement sees it, a row WHERE CURRENT OF names too.
     */
    if (RelationIsSm(heapRelation)
            ? smstore_fetch_row(heapRelation, key, node->tss_SmNatts, slot)
            : table_tuple_fetch_row_version(heapRelation, key, snapshot, slot))
      return slot;

    /* No such row, or failed snapshot qual; try next */
    if (bBackward)
      node->tss_TidPtr--;
    else
      node->tss_TidPtr++;

    CHECK_FOR_INTERRUPTS();
  }

  /*
   * if we get here it means the tid scan failed so we are at the end of the
   * scan..
   */
  return ExecClearTuple(slot);
}

/*
 * TidRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool TidRecheck(TidScanState *node, TupleTableSlot *slot) {
  RowKey key;

  /* WHERE CURRENT OF always intends to resolve to the latest tuple */
  if (node->tss_isCurrentOf)
    return true;

  if (node->tss_TidList == NULL)
    TidListEval(node);

  /*
   * Binary search the list to see if this row's rowno is mentioned and
   * return true if it is.
   */
  RowKeyFromSlot(node->ss.ss_currentRelation, slot, &key);
  return bsearch(&key, node->tss_TidList, node->tss_NumTids, sizeof(RowKey),
                 rowno_comparator) != NULL;
}

/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		Scans the relation using tids and returns
 *		   the next qualifying tuple in the direction specified.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the
 *tuple returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying
 *tuple.
 *		  -- tss_TidPtr is -1.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *ExecTidScan(PlanState *pstate) {
  TidScanState *node = castNode(TidScanState, pstate);

  return ExecScan(&node->ss, (ExecScanAccessMtd)TidNext,
                  (ExecScanRecheckMtd)TidRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidScan(node)
 * ----------------------------------------------------------------
 */
void ExecReScanTidScan(TidScanState *node) {
  if (node->tss_TidList)
    pfree(node->tss_TidList);
  node->tss_TidList = NULL;
  node->tss_NumTids = 0;
  node->tss_TidPtr = -1;

  /* not really necessary, but seems good form */
  if (node->ss.ss_currentScanDesc)
    table_rescan(node->ss.ss_currentScanDesc, NULL);

  ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void ExecEndTidScan(TidScanState *node) {
  if (node->ss.ss_currentScanDesc)
    table_endscan(node->ss.ss_currentScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitTidScan
 *
 *		Initializes the tid scan's state information, creates
 *		scan keys, and opens the base and tid relations.
 *
 *		Parameters:
 *		  node: TidScan node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
TidScanState *ExecInitTidScan(TidScan *node, EState *estate, int eflags) {
  TidScanState *tidstate;
  Relation currentRelation;

  /*
   * create state structure
   */
  tidstate = makeNode(TidScanState);
  tidstate->ss.ps.plan = (Plan *)node;
  tidstate->ss.ps.state = estate;
  tidstate->ss.ps.ExecProcNode = ExecTidScan;

  /*
   * Miscellaneous initialization
   *
   * create expression context for node
   */
  ExecAssignExprContext(estate, &tidstate->ss.ps);

  /*
   * mark tid list as not computed yet
   */
  tidstate->tss_TidList = NULL;
  tidstate->tss_NumTids = 0;
  tidstate->tss_TidPtr = -1;

  /*
   * open the scan relation
   */
  currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

  tidstate->ss.ss_currentRelation = currentRelation;
  tidstate->ss.ss_currentScanDesc = NULL; /* no heap scan here */
  if (RelationIsSm(currentRelation))
    tidstate->tss_SmNatts = smstore_scan_natts(&node->scan, currentRelation);

  /*
   * get the scan type from the relation descriptor.
   */
  ExecInitScanTupleSlot(estate, &tidstate->ss,
                        RelationGetDescr(currentRelation),
                        table_slot_callbacks(currentRelation));

  /*
   * Initialize result type and projection.
   */
  ExecInitResultTypeTL(&tidstate->ss.ps);
  ExecAssignScanProjectionInfo(&tidstate->ss);

  /*
   * initialize child expressions
   */
  tidstate->ss.ps.qual =
      ExecInitQual(node->scan.plan.qual, (PlanState *)tidstate);

  TidExprListCreate(tidstate);

  /*
   * all done.
   */
  return tidstate;
}
