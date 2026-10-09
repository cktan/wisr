/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHashjoin.c
 *
 * HASH JOIN
 *
 * This is based on the "hybrid hash join" algorithm described shortly in the
 * following page
 *
 *   https://en.wikipedia.org/wiki/Hash_join#Hybrid_hash_join
 *
 * and in detail in the referenced paper:
 *
 *   "An Adaptive Hash Join Algorithm for Multiuser Environments"
 *   Hansjörg Zeller; Jim Gray (1990). Proceedings of the 16th VLDB conference.
 *   Brisbane: 186–197.
 *
 * If the inner side tuples of a hash join do not fit in memory, the hash join
 * can be executed in multiple batches.
 *
 * If the statistics on the inner side relation are accurate, planner chooses a
 * multi-batch strategy and estimates the number of batches.
 *
 * The query executor measures the real size of the hashtable and increases the
 * number of batches if the hashtable grows too large.
 *
 * The number of batches is always a power of two, so an increase in the number
 * of batches doubles it.
 *
 * Hash join measures batch size lazily -- waiting until it is loading a
 * batch to determine if it will fit in memory. While inserting tuples into the
 * hashtable, hash join will, if that tuple were to exceed work_mem,
 * dump out the hashtable and reassign them either to other batch files or the
 * current batch resident in the hashtable.
 *
 * The executor makes a best
 * effort. If a particular batch will not fit in memory, it tries doubling the
 * number of batches. If after a batch increase, there is a batch which
 * retained all or none of its tuples, the executor disables growth in the
 * number of batches globally. After growth is disabled, all batches that would
 * have previously triggered an increase in the number of batches instead
 * exceed the space allowed.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/wait_event.h"

/*
 * States of the ExecHashJoin state machine
 */
#define HJ_BUILD_HASHTABLE		1
#define HJ_NEED_NEW_OUTER		2
#define HJ_SCAN_BUCKET			3
#define HJ_FILL_OUTER_TUPLE		4
#define HJ_FILL_INNER_TUPLES	5
#define HJ_NEED_NEW_BATCH		6

/* Returns true if doing null-fill on outer relation */
#define HJ_FILL_OUTER(hjstate)	((hjstate)->hj_NullInnerTupleSlot != NULL)
/* Returns true if doing null-fill on inner relation */
#define HJ_FILL_INNER(hjstate)	((hjstate)->hj_NullOuterTupleSlot != NULL)

static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *outerNode,
                                                 HashJoinState *hjstate,
                                                 uint32 *hashvalue);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
                                                 BufFile *file,
                                                 uint32 *hashvalue,
                                                 TupleTableSlot *tupleSlot);
static bool ExecHashJoinNewBatch(HashJoinState *hjstate);

/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		This function implements the Hybrid Hashjoin algorithm.
 *
 *		Note: the relation we build hash table on is the "inner"
 *			  the other one is "outer".
 * ----------------------------------------------------------------
 */
static TupleTableSlot * /* return: a tuple or NULL */
ExecHashJoin(PlanState *pstate) {
  HashJoinState *node = castNode(HashJoinState, pstate);
  PlanState *outerNode;
  HashState *hashNode;
  ExprState *joinqual;
  ExprState *otherqual;
  ExprContext *econtext;
  HashJoinTable hashtable;
  TupleTableSlot *outerTupleSlot;
  uint32 hashvalue;
  int batchno;

  /*
   * get information from HashJoin node
   */
  joinqual = node->js.joinqual;
  otherqual = node->js.ps.qual;
  hashNode = (HashState *)innerPlanState(node);
  outerNode = outerPlanState(node);
  hashtable = node->hj_HashTable;
  econtext = node->js.ps.ps_ExprContext;

  /*
   * Reset per-tuple memory context to free any expression evaluation
   * storage allocated in the previous tuple cycle.
   */
  ResetExprContext(econtext);

  /*
   * run the hash join state machine
   */
  for (;;) {
    /*
     * It's possible to iterate this loop many times before returning a
     * tuple, in some pathological cases such as needing to move much of
     * the current batch to a later batch.  So let's check for interrupts
     * each time through.
     */
    CHECK_FOR_INTERRUPTS();

    switch (node->hj_JoinState) {
    case HJ_BUILD_HASHTABLE:

      /*
       * First time through: build hash table for inner relation.
       */
      Assert(hashtable == NULL);

      /*
       * If the outer relation is completely empty, and it's not
       * right/right-anti/full join, we can quit without building
       * the hash table.  However, for an inner join it is only a
       * win to check this when the outer relation's startup cost is
       * less than the projected cost of building the hash table.
       * Otherwise it's best to build the hash table first and see
       * if the inner relation is empty.  (When it's a left join, we
       * should always make this check, since we aren't going to be
       * able to skip the join on the strength of an empty inner
       * relation anyway.)
       *
       * If we are rescanning the join, we make use of information
       * gained on the previous scan: don't bother to try the
       * prefetch if the previous scan found the outer relation
       * nonempty. This is not 100% reliable since with new
       * parameters the outer relation might yield different
       * results, but it's a good heuristic.
       *
       * The only way to make the check is to try to fetch a tuple
       * from the outer plan node.  If we succeed, we have to stash
       * it away for later consumption by ExecHashJoinOuterGetTuple.
       */
      if (HJ_FILL_INNER(node)) {
        /* no chance to not build the hash table */
        node->hj_FirstOuterTupleSlot = NULL;
      } else if (HJ_FILL_OUTER(node) || (outerNode->plan->startup_cost <
                                             hashNode->ps.plan->total_cost &&
                                         !node->hj_OuterNotEmpty)) {
        node->hj_FirstOuterTupleSlot = ExecProcNode(outerNode);
        if (TupIsNull(node->hj_FirstOuterTupleSlot)) {
          node->hj_OuterNotEmpty = false;
          return NULL;
        } else
          node->hj_OuterNotEmpty = true;
      } else
        node->hj_FirstOuterTupleSlot = NULL;

      /*
       * Create the hash table.
       */
      hashtable = ExecHashTableCreate(hashNode);
      node->hj_HashTable = hashtable;

      /*
       * Execute the Hash node, to build the hash table.
       */
      hashNode->hashtable = hashtable;
      (void)MultiExecProcNode((PlanState *)hashNode);

      /*
       * If the inner relation is completely empty, and we're not
       * doing a left outer join, we can quit without scanning the
       * outer relation.
       */
      if (hashtable->totalTuples == 0 && !HJ_FILL_OUTER(node))
        return NULL;

      /*
       * need to remember whether nbatch has increased since we
       * began scanning the outer relation
       */
      hashtable->nbatch_outstart = hashtable->nbatch;

      /*
       * Reset OuterNotEmpty for scan.  (It's OK if we fetched a
       * tuple above, because ExecHashJoinOuterGetTuple will
       * immediately set it again.)
       */
      node->hj_OuterNotEmpty = false;

      node->hj_JoinState = HJ_NEED_NEW_OUTER;

      /* FALL THRU */

    case HJ_NEED_NEW_OUTER:

      /*
       * We don't have an outer tuple, try to get the next one
       */
      outerTupleSlot = ExecHashJoinOuterGetTuple(outerNode, node, &hashvalue);

      if (TupIsNull(outerTupleSlot)) {
        /* end of batch, or maybe whole join */
        if (HJ_FILL_INNER(node)) {
          /* set up to scan for unmatched inner tuples */
          ExecPrepHashTableForUnmatched(node);
          node->hj_JoinState = HJ_FILL_INNER_TUPLES;
        } else
          node->hj_JoinState = HJ_NEED_NEW_BATCH;
        continue;
      }

      econtext->ecxt_outertuple = outerTupleSlot;
      node->hj_MatchedOuter = false;

      /*
       * Find the corresponding bucket for this tuple in the main
       * hash table or skew hash table.
       */
      node->hj_CurHashValue = hashvalue;
      ExecHashGetBucketAndBatch(hashtable, hashvalue, &node->hj_CurBucketNo,
                                &batchno);
      node->hj_CurSkewBucketNo = ExecHashGetSkewBucket(hashtable, hashvalue);
      node->hj_CurTuple = NULL;

      /*
       * The tuple might not belong to the current batch (where
       * "current batch" includes the skew buckets if any).
       */
      if (batchno != hashtable->curbatch &&
          node->hj_CurSkewBucketNo == INVALID_SKEW_BUCKET_NO) {
        bool shouldFree;
        MinimalTuple mintuple =
            ExecFetchSlotMinimalTuple(outerTupleSlot, &shouldFree);

        /*
         * Need to postpone this outer tuple to a later batch.
         * Save it in the corresponding outer-batch file.
         */
        Assert(batchno > hashtable->curbatch);
        ExecHashJoinSaveTuple(mintuple, hashvalue,
                              &hashtable->outerBatchFile[batchno], hashtable);

        if (shouldFree)
          heap_free_minimal_tuple(mintuple);

        /* Loop around, staying in HJ_NEED_NEW_OUTER state */
        continue;
      }

      /* OK, let's scan the bucket for matches */
      node->hj_JoinState = HJ_SCAN_BUCKET;

      /* FALL THRU */

    case HJ_SCAN_BUCKET:

      /*
       * Scan the selected hash bucket for matches to current outer
       */
      if (!ExecScanHashBucket(node, econtext)) {
        /* out of matches; check for possible outer-join fill */
        node->hj_JoinState = HJ_FILL_OUTER_TUPLE;
        continue;
      }

      /*
       * In a right-semijoin, we only need the first match for each
       * inner tuple.
       */
      if (node->js.jointype == JOIN_RIGHT_SEMI &&
          HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple)))
        continue;

      /*
       * We've got a match, but still need to test non-hashed quals.
       * ExecScanHashBucket already set up all the state needed to
       * call ExecQual.
       *
       * If we pass the qual, then save state for next call and have
       * ExecProject form the projection, store it in the tuple
       * table, and return the slot.
       *
       * Only the joinquals determine tuple match status, but all
       * quals must pass to actually return the tuple.
       */
      if (joinqual == NULL || ExecQual(joinqual, econtext)) {
        node->hj_MatchedOuter = true;

        /*
         * This is really only needed if HJ_FILL_INNER(node) or if
         * we are in a right-semijoin, but we'll avoid the branch
         * and just set it always.
         */
        if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple)))
          HeapTupleHeaderSetMatch(HJTUPLE_MINTUPLE(node->hj_CurTuple));

        /* In an antijoin, we never return a matched tuple */
        if (node->js.jointype == JOIN_ANTI) {
          node->hj_JoinState = HJ_NEED_NEW_OUTER;
          continue;
        }

        /*
         * If we only need to consider the first matching inner
         * tuple, then advance to next outer tuple after we've
         * processed this one.
         */
        if (node->js.single_match)
          node->hj_JoinState = HJ_NEED_NEW_OUTER;

        /*
         * In a right-antijoin, we never return a matched tuple.
         * If it's not an inner_unique join, we need to stay on
         * the current outer tuple to continue scanning the inner
         * side for matches.
         */
        if (node->js.jointype == JOIN_RIGHT_ANTI)
          continue;

        if (otherqual == NULL || ExecQual(otherqual, econtext))
          return ExecProject(node->js.ps.ps_ProjInfo);
        else
          InstrCountFiltered2(node, 1);
      } else
        InstrCountFiltered1(node, 1);
      break;

    case HJ_FILL_OUTER_TUPLE:

      /*
       * The current outer tuple has run out of matches, so check
       * whether to emit a dummy outer-join tuple.  Whether we emit
       * one or not, the next state is NEED_NEW_OUTER.
       */
      node->hj_JoinState = HJ_NEED_NEW_OUTER;

      if (!node->hj_MatchedOuter && HJ_FILL_OUTER(node)) {
        /*
         * Generate a fake join tuple with nulls for the inner
         * tuple, and return it if it passes the non-join quals.
         */
        econtext->ecxt_innertuple = node->hj_NullInnerTupleSlot;

        if (otherqual == NULL || ExecQual(otherqual, econtext))
          return ExecProject(node->js.ps.ps_ProjInfo);
        else
          InstrCountFiltered2(node, 1);
      }
      break;

    case HJ_FILL_INNER_TUPLES:

      /*
       * We have finished a batch, but we are doing
       * right/right-anti/full join, so any unmatched inner tuples
       * in the hashtable have to be emitted before we continue to
       * the next batch.
       */
      if (!ExecScanHashTableForUnmatched(node, econtext)) {
        /* no more unmatched tuples */
        node->hj_JoinState = HJ_NEED_NEW_BATCH;
        continue;
      }

      /*
       * Generate a fake join tuple with nulls for the outer tuple,
       * and return it if it passes the non-join quals.
       */
      econtext->ecxt_outertuple = node->hj_NullOuterTupleSlot;

      if (otherqual == NULL || ExecQual(otherqual, econtext))
        return ExecProject(node->js.ps.ps_ProjInfo);
      else
        InstrCountFiltered2(node, 1);
      break;

    case HJ_NEED_NEW_BATCH:

      /*
       * Try to advance to next batch.  Done if there are no more.
       */
      if (!ExecHashJoinNewBatch(node))
        return NULL; /* end of join */
      node->hj_JoinState = HJ_NEED_NEW_OUTER;
      break;

    default:
      elog(ERROR, "unrecognized hashjoin state: %d", (int)node->hj_JoinState);
    }
  }
}

/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags) {
  HashJoinState *hjstate;
  Plan *outerNode;
  Hash *hashNode;
  TupleDesc outerDesc, innerDesc;
  const TupleTableSlotOps *ops;

  /* check for unsupported flags */
  Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

  /*
   * create state structure
   */
  hjstate = makeNode(HashJoinState);
  hjstate->js.ps.plan = (Plan *)node;
  hjstate->js.ps.state = estate;

  hjstate->js.ps.ExecProcNode = ExecHashJoin;
  hjstate->js.jointype = node->join.jointype;

  /*
   * Miscellaneous initialization
   *
   * create expression context for node
   */
  ExecAssignExprContext(estate, &hjstate->js.ps);

  /*
   * initialize child nodes
   *
   * Note: we could suppress the REWIND flag for the inner input, which
   * would amount to betting that the hash will be a single batch.  Not
   * clear if this would be a win or not.
   */
  outerNode = outerPlan(node);
  hashNode = (Hash *)innerPlan(node);

  outerPlanState(hjstate) = ExecInitNode(outerNode, estate, eflags);
  outerDesc = ExecGetResultType(outerPlanState(hjstate));
  innerPlanState(hjstate) = ExecInitNode((Plan *)hashNode, estate, eflags);
  innerDesc = ExecGetResultType(innerPlanState(hjstate));

  /*
   * Initialize result slot, type and projection.
   */
  ExecInitResultTupleSlotTL(&hjstate->js.ps, &TTSOpsVirtual);
  ExecAssignProjectionInfo(&hjstate->js.ps, NULL);

  /*
   * tuple table initialization
   */
  ops = ExecGetResultSlotOps(outerPlanState(hjstate), NULL);
  hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate, outerDesc, ops);

  /*
   * detect whether we need only consider the first matching inner tuple
   */
  hjstate->js.single_match =
      (node->join.inner_unique || node->join.jointype == JOIN_SEMI);

  /* set up null tuples for outer joins, if needed */
  switch (node->join.jointype) {
  case JOIN_INNER:
  case JOIN_SEMI:
  case JOIN_RIGHT_SEMI:
    break;
  case JOIN_LEFT:
  case JOIN_ANTI:
    hjstate->hj_NullInnerTupleSlot =
        ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
    break;
  case JOIN_RIGHT:
  case JOIN_RIGHT_ANTI:
    hjstate->hj_NullOuterTupleSlot =
        ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
    break;
  case JOIN_FULL:
    hjstate->hj_NullOuterTupleSlot =
        ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
    hjstate->hj_NullInnerTupleSlot =
        ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
    break;
  default:
    elog(ERROR, "unrecognized join type: %d", (int)node->join.jointype);
  }

  /*
   * now for some voodoo.  our temporary tuple slot is actually the result
   * tuple slot of the Hash node (which is our inner plan).  we can do this
   * because Hash nodes don't return tuples via ExecProcNode() -- instead
   * the hash join node uses ExecScanHashBucket() to get at the contents of
   * the hash table.  -cim 6/9/91
   */
  {
    HashState *hashstate = (HashState *)innerPlanState(hjstate);
    Hash *hash = (Hash *)hashstate->ps.plan;
    TupleTableSlot *slot = hashstate->ps.ps_ResultTupleSlot;
    Oid *outer_hashfuncid;
    Oid *inner_hashfuncid;
    bool *hash_strict;
    ListCell *lc;
    int nkeys;

    hjstate->hj_HashTupleSlot = slot;

    /*
     * Build ExprStates to obtain hash values for either side of the join.
     * This must be done here as ExecBuildHash32Expr needs to know how to
     * handle NULL inputs and the required handling of that depends on the
     * jointype.  We don't know the join type in ExecInitHash() and we
     * must build the ExprStates before ExecHashTableCreate() so we
     * properly attribute any SubPlans that exist in the hash expressions
     * to the correct PlanState.
     */
    nkeys = list_length(node->hashoperators);

    outer_hashfuncid = palloc_array(Oid, nkeys);
    inner_hashfuncid = palloc_array(Oid, nkeys);
    hash_strict = palloc_array(bool, nkeys);

    /*
     * Determine the hash function for each side of the join for the given
     * hash operator.
     */
    foreach (lc, node->hashoperators) {
      Oid hashop = lfirst_oid(lc);
      int i = foreach_current_index(lc);

      if (!get_op_hash_functions(hashop, &outer_hashfuncid[i],
                                 &inner_hashfuncid[i]))
        elog(ERROR, "could not find hash function for hash operator %u",
             hashop);
      hash_strict[i] = op_strict(hashop);
    }

    /*
     * Build an ExprState to generate the hash value for the expressions
     * on the outer of the join.  This ExprState must finish generating
     * the hash value when HJ_FILL_OUTER() is true.  Otherwise,
     * ExecBuildHash32Expr will set up the ExprState to abort early if it
     * finds a NULL.  In these cases, we don't need to store these tuples
     * in the hash table as the jointype does not require it.
     */
    hjstate->hj_OuterHash = ExecBuildHash32Expr(
        hjstate->js.ps.ps_ResultTupleDesc, hjstate->js.ps.resultops,
        outer_hashfuncid, node->hashcollations, node->hashkeys, hash_strict,
        &hjstate->js.ps, 0, HJ_FILL_OUTER(hjstate));

    /* As above, but for the inner side of the join */
    hashstate->hash_expr = ExecBuildHash32Expr(
        hashstate->ps.ps_ResultTupleDesc, hashstate->ps.resultops,
        inner_hashfuncid, node->hashcollations, hash->hashkeys, hash_strict,
        &hashstate->ps, 0, HJ_FILL_INNER(hjstate));

    /*
     * Set up the skew table hash function while we have a record of the
     * first key's hash function Oid.
     */
    if (OidIsValid(hash->skewTable)) {
      hashstate->skew_hashfunction = (FmgrInfo *)palloc0(sizeof(FmgrInfo));
      hashstate->skew_collation = linitial_oid(node->hashcollations);
      fmgr_info(outer_hashfuncid[0], hashstate->skew_hashfunction);
    }

    /* no need to keep these */
    pfree(outer_hashfuncid);
    pfree(inner_hashfuncid);
    pfree(hash_strict);
  }

  /*
   * initialize child expressions
   */
  hjstate->js.ps.qual =
      ExecInitQual(node->join.plan.qual, (PlanState *)hjstate);
  hjstate->js.joinqual =
      ExecInitQual(node->join.joinqual, (PlanState *)hjstate);
  hjstate->hashclauses = ExecInitQual(node->hashclauses, (PlanState *)hjstate);

  /*
   * initialize hash-specific info
   */
  hjstate->hj_HashTable = NULL;
  hjstate->hj_FirstOuterTupleSlot = NULL;

  hjstate->hj_CurHashValue = 0;
  hjstate->hj_CurBucketNo = 0;
  hjstate->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
  hjstate->hj_CurTuple = NULL;

  hjstate->hj_JoinState = HJ_BUILD_HASHTABLE;
  hjstate->hj_MatchedOuter = false;
  hjstate->hj_OuterNotEmpty = false;

  return hjstate;
}

/* ----------------------------------------------------------------
 *		ExecEndHashJoin
 *
 *		clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void ExecEndHashJoin(HashJoinState *node) {
  /*
   * Free hash table
   */
  if (node->hj_HashTable) {
    ExecHashTableDestroy(node->hj_HashTable);
    node->hj_HashTable = NULL;
  }

  /*
   * clean up subtrees
   */
  ExecEndNode(outerPlanState(node));
  ExecEndNode(innerPlanState(node));
}

/*
 * ExecHashJoinOuterGetTuple
 *
 *		get the next outer tuple for a hashjoin:
 *either by executing the outer plan node in the first pass, or from the temp
 *		files for the hashjoin batches.
 *
 * Returns a null slot if no more outer tuples (within the current batch).
 *
 * On success, the tuple's hash value is stored at *hashvalue --- this is
 * either originally computed, or re-read from the temp file.
 */
static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *outerNode,
                                                 HashJoinState *hjstate,
                                                 uint32 *hashvalue) {
  HashJoinTable hashtable = hjstate->hj_HashTable;
  int curbatch = hashtable->curbatch;
  TupleTableSlot *slot;

  if (curbatch == 0) /* if it is the first pass */
  {
    /*
     * Check to see if first outer tuple was already fetched by
     * ExecHashJoin() and not used yet.
     */
    slot = hjstate->hj_FirstOuterTupleSlot;
    if (!TupIsNull(slot))
      hjstate->hj_FirstOuterTupleSlot = NULL;
    else
      slot = ExecProcNode(outerNode);

    while (!TupIsNull(slot)) {
      bool isnull;

      /*
       * We have to compute the tuple's hash value.
       */
      ExprContext *econtext = hjstate->js.ps.ps_ExprContext;

      econtext->ecxt_outertuple = slot;

      ResetExprContext(econtext);

      *hashvalue = DatumGetUInt32(
          ExecEvalExprSwitchContext(hjstate->hj_OuterHash, econtext, &isnull));

      if (!isnull) {
        /* remember outer relation is not empty for possible rescan */
        hjstate->hj_OuterNotEmpty = true;

        return slot;
      }

      /*
       * That tuple couldn't match because of a NULL, so discard it and
       * continue with the next one.
       */
      slot = ExecProcNode(outerNode);
    }
  } else if (curbatch < hashtable->nbatch) {
    BufFile *file = hashtable->outerBatchFile[curbatch];

    /*
     * In outer-join cases, we could get here even though the batch file
     * is empty.
     */
    if (file == NULL)
      return NULL;

    slot = ExecHashJoinGetSavedTuple(hjstate, file, hashvalue,
                                     hjstate->hj_OuterTupleSlot);
    if (!TupIsNull(slot))
      return slot;
  }

  /* End of this batch */
  return NULL;
}

/*
 * ExecHashJoinNewBatch
 *		switch to a new hashjoin batch
 *
 * Returns true if successful, false if there are no more batches.
 */
static bool ExecHashJoinNewBatch(HashJoinState *hjstate) {
  HashJoinTable hashtable = hjstate->hj_HashTable;
  int nbatch;
  int curbatch;
  BufFile *innerFile;
  TupleTableSlot *slot;
  uint32 hashvalue;

  nbatch = hashtable->nbatch;
  curbatch = hashtable->curbatch;

  if (curbatch > 0) {
    /*
     * We no longer need the previous outer batch file; close it right
     * away to free disk space.
     */
    if (hashtable->outerBatchFile[curbatch])
      BufFileClose(hashtable->outerBatchFile[curbatch]);
    hashtable->outerBatchFile[curbatch] = NULL;
  } else /* we just finished the first batch */
  {
    /*
     * Reset some of the skew optimization state variables, since we no
     * longer need to consider skew tuples after the first batch. The
     * memory context reset we are about to do will release the skew
     * hashtable itself.
     */
    hashtable->skewEnabled = false;
    hashtable->skewBucket = NULL;
    hashtable->skewBucketNums = NULL;
    hashtable->nSkewBuckets = 0;
    hashtable->spaceUsedSkew = 0;
  }

  /*
   * We can always skip over any batches that are completely empty on both
   * sides.  We can sometimes skip over batches that are empty on only one
   * side, but there are exceptions:
   *
   * 1. In a left/full outer join, we have to process outer batches even if
   * the inner batch is empty.  Similarly, in a right/right-anti/full outer
   * join, we have to process inner batches even if the outer batch is
   * empty.
   *
   * 2. If we have increased nbatch since the initial estimate, we have to
   * scan inner batches since they might contain tuples that need to be
   * reassigned to later inner batches.
   *
   * 3. Similarly, if we have increased nbatch since starting the outer
   * scan, we have to rescan outer batches in case they contain tuples that
   * need to be reassigned.
   */
  curbatch++;
  while (curbatch < nbatch && (hashtable->outerBatchFile[curbatch] == NULL ||
                               hashtable->innerBatchFile[curbatch] == NULL)) {
    if (hashtable->outerBatchFile[curbatch] && HJ_FILL_OUTER(hjstate))
      break; /* must process due to rule 1 */
    if (hashtable->innerBatchFile[curbatch] && HJ_FILL_INNER(hjstate))
      break; /* must process due to rule 1 */
    if (hashtable->innerBatchFile[curbatch] &&
        nbatch != hashtable->nbatch_original)
      break; /* must process due to rule 2 */
    if (hashtable->outerBatchFile[curbatch] &&
        nbatch != hashtable->nbatch_outstart)
      break; /* must process due to rule 3 */
    /* We can ignore this batch. */
    /* Release associated temp files right away. */
    if (hashtable->innerBatchFile[curbatch])
      BufFileClose(hashtable->innerBatchFile[curbatch]);
    hashtable->innerBatchFile[curbatch] = NULL;
    if (hashtable->outerBatchFile[curbatch])
      BufFileClose(hashtable->outerBatchFile[curbatch]);
    hashtable->outerBatchFile[curbatch] = NULL;
    curbatch++;
  }

  if (curbatch >= nbatch)
    return false; /* no more batches */

  hashtable->curbatch = curbatch;

  /*
   * Reload the hash table with the new inner batch (which could be empty)
   */
  ExecHashTableReset(hashtable);

  innerFile = hashtable->innerBatchFile[curbatch];

  if (innerFile != NULL) {
    if (BufFileSeek(innerFile, 0, 0, SEEK_SET))
      ereport(ERROR, (errcode_for_file_access(),
                      errmsg("could not rewind hash-join temporary file")));

    while ((slot = ExecHashJoinGetSavedTuple(hjstate, innerFile, &hashvalue,
                                             hjstate->hj_HashTupleSlot))) {
      /*
       * NOTE: some tuples may be sent to future batches.  Also, it is
       * possible for hashtable->nbatch to be increased here!
       */
      ExecHashTableInsert(hashtable, slot, hashvalue);
    }

    /*
     * after we build the hash table, the inner batch file is no longer
     * needed
     */
    BufFileClose(innerFile);
    hashtable->innerBatchFile[curbatch] = NULL;
  }

  /*
   * Rewind outer batch file (if present), so that we can start reading it.
   */
  if (hashtable->outerBatchFile[curbatch] != NULL) {
    if (BufFileSeek(hashtable->outerBatchFile[curbatch], 0, 0, SEEK_SET))
      ereport(ERROR, (errcode_for_file_access(),
                      errmsg("could not rewind hash-join temporary file")));
  }

  return true;
}

/*
 * ExecHashJoinSaveTuple
 *		save a tuple to a batch file.
 *
 * The data recorded in the file for each tuple is its hash value,
 * then the tuple in MinimalTuple format.
 *
 * fileptr points to a batch file in one of the hashtable arrays.
 *
 * The batch files (and their buffers) are allocated in the spill context
 * created for the hashtable.
 */
void ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
                           BufFile **fileptr, HashJoinTable hashtable) {
  BufFile *file = *fileptr;

  /*
   * The batch file is lazily created. If this is the first tuple written to
   * this batch, the batch file is created and its buffer is allocated in
   * the spillCxt context, NOT in the batchCxt.
   *
   * During the build phase, buffered files are created for inner batches.
   * Each batch's buffered file is closed (and its buffer freed) after the
   * batch is loaded into memory during the outer side scan. Therefore, it
   * is necessary to allocate the batch file buffer in a memory context
   * which outlives the batch itself.
   *
   * Also, we use spillCxt instead of hashCxt for a better accounting of the
   * spilling memory consumption.
   */
  if (file == NULL) {
    MemoryContext oldctx = MemoryContextSwitchTo(hashtable->spillCxt);

    file = BufFileCreateTemp(false);
    *fileptr = file;

    MemoryContextSwitchTo(oldctx);
  }

  BufFileWrite(file, &hashvalue, sizeof(uint32));
  BufFileWrite(file, tuple, tuple->t_len);
}

/*
 * ExecHashJoinGetSavedTuple
 *		read the next tuple from a batch file.  Return NULL if no more.
 *
 * On success, *hashvalue is set to the tuple's hash value, and the tuple
 * itself is stored in the given slot.
 */
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
                                                 BufFile *file,
                                                 uint32 *hashvalue,
                                                 TupleTableSlot *tupleSlot) {
  uint32 header[2];
  size_t nread;
  MinimalTuple tuple;

  /*
   * We check for interrupts here because this is typically taken as an
   * alternative code path to an ExecProcNode() call, which would include
   * such a check.
   */
  CHECK_FOR_INTERRUPTS();

  /*
   * Since both the hash value and the MinimalTuple length word are uint32,
   * we can read them both in one BufFileRead() call without any type
   * cheating.
   */
  nread = BufFileReadMaybeEOF(file, header, sizeof(header), true);
  if (nread == 0) /* end of file */
  {
    ExecClearTuple(tupleSlot);
    return NULL;
  }
  *hashvalue = header[0];
  tuple = (MinimalTuple)palloc(header[1]);
  tuple->t_len = header[1];
  BufFileReadExact(file, (char *)tuple + sizeof(uint32),
                   header[1] - sizeof(uint32));
  ExecForceStoreMinimalTuple(tuple, tupleSlot, true);
  return tupleSlot;
}

void ExecReScanHashJoin(HashJoinState *node) {
  PlanState *outerPlan = outerPlanState(node);
  PlanState *innerPlan = innerPlanState(node);

  /*
   * In a multi-batch join, we currently have to do rescans the hard way,
   * primarily because batch temp files may have already been released. But
   * if it's a single-batch join, and there is no parameter change for the
   * inner subnode, then we can just re-use the existing hash table without
   * rebuilding it.
   */
  if (node->hj_HashTable != NULL) {
    if (node->hj_HashTable->nbatch == 1 && innerPlan->chgParam == NULL) {
      /*
       * Okay to reuse the hash table; needn't rescan inner, either.
       *
       * However, if it's a right/right-anti/right-semi/full join, we'd
       * better reset the inner-tuple match flags contained in the
       * table.
       */
      if (HJ_FILL_INNER(node) || node->js.jointype == JOIN_RIGHT_SEMI)
        ExecHashTableResetMatchFlags(node->hj_HashTable);

      /*
       * Also, we need to reset our state about the emptiness of the
       * outer relation, so that the new scan of the outer will update
       * it correctly if it turns out to be empty this time. (There's no
       * harm in clearing it now because ExecHashJoin won't need the
       * info.  In the other cases, where the hash table doesn't exist
       * or we are destroying it, we leave this state alone because
       * ExecHashJoin will need it the first time through.)
       */
      node->hj_OuterNotEmpty = false;

      /* ExecHashJoin can skip the BUILD_HASHTABLE step */
      node->hj_JoinState = HJ_NEED_NEW_OUTER;
    } else {
      /* must destroy and rebuild hash table */
      HashState *hashNode = castNode(HashState, innerPlan);

      Assert(hashNode->hashtable == node->hj_HashTable);
      /* accumulate stats from old hash table, if wanted */
      /* (this should match ExecShutdownHash) */
      if (hashNode->ps.instrument && !hashNode->hinstrument)
        hashNode->hinstrument =
            (HashInstrumentation *)palloc0(sizeof(HashInstrumentation));
      if (hashNode->hinstrument)
        ExecHashAccumInstrumentation(hashNode->hinstrument,
                                     hashNode->hashtable);
      /* for safety, be sure to clear child plan node's pointer too */
      hashNode->hashtable = NULL;

      ExecHashTableDestroy(node->hj_HashTable);
      node->hj_HashTable = NULL;
      node->hj_JoinState = HJ_BUILD_HASHTABLE;

      /*
       * if chgParam of subnode is not null then plan will be re-scanned
       * by first ExecProcNode.
       */
      if (innerPlan->chgParam == NULL)
        ExecReScan(innerPlan);
    }
  }

  /* Always reset intra-tuple state */
  node->hj_CurHashValue = 0;
  node->hj_CurBucketNo = 0;
  node->hj_CurSkewBucketNo = INVALID_SKEW_BUCKET_NO;
  node->hj_CurTuple = NULL;

  node->hj_MatchedOuter = false;
  node->hj_FirstOuterTupleSlot = NULL;

  /*
   * if chgParam of subnode is not null then plan will be re-scanned by
   * first ExecProcNode.
   */
  if (outerPlan->chgParam == NULL)
    ExecReScan(outerPlan);
}
