/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeHash.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecHash	- generate an in-memory hash table of the
 *relation ExecInitHash	- initialize node and subnodes ExecEndHash
 *- shutdown node and subnodes
 */

#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/dynahash.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/wait_event.h"
#include "utils/control_block.h"

static void ExecHashIncreaseNumBatches(HashJoinTable hashtable);
static void ExecHashIncreaseNumBuckets(HashJoinTable hashtable);
static void ExecHashBuildSkewHash(HashState *hashstate, HashJoinTable hashtable,
                                  Hash *node, int mcvsToUse);
static void ExecHashSkewTableInsert(HashJoinTable hashtable,
                                    TupleTableSlot *slot, uint32 hashvalue,
                                    int bucketNumber);
static void ExecHashRemoveNextSkewBucket(HashJoinTable hashtable);

static void *dense_alloc(HashJoinTable hashtable, Size size);
static void MultiExecPrivateHash(HashState *node);

/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		stub for pro forma compliance
 * ----------------------------------------------------------------
 */
static TupleTableSlot *ExecHash(PlanState *pstate) {
  elog(ERROR, "Hash node does not support ExecProcNode call convention");
  return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecHash
 *
 *		build hash table for hashjoin, doing partitioning if more
 *		than one batch is required.
 * ----------------------------------------------------------------
 */
Node *MultiExecHash(HashState *node) {
  /* must provide our own instrumentation support */
  if (node->ps.instrument)
    InstrStartNode(node->ps.instrument);

  MultiExecPrivateHash(node);

  /* must provide our own instrumentation support */
  if (node->ps.instrument)
    InstrStopNode(node->ps.instrument, node->hashtable->totalTuples);

  /*
   * We do not return the hash table directly because it's not a subtype of
   * Node, and so would violate the MultiExecProcNode API.  Instead, our
   * parent Hashjoin node is expected to know how to fish it out of our node
   * state.  Ugly but not really worth cleaning up, since Hashjoin knows
   * quite a bit more about Hash besides that.
   */
  return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecPrivateHash
 *
 *		build a backend-private hash table and (if necessary) batch
 *		files.
 * ----------------------------------------------------------------
 */
static void MultiExecPrivateHash(HashState *node) {
  PlanState *outerNode;
  HashJoinTable hashtable;
  TupleTableSlot *slot;
  ExprContext *econtext;

  /*
   * get state info from node
   */
  outerNode = outerPlanState(node);
  hashtable = node->hashtable;

  /*
   * set expression context
   */
  econtext = node->ps.ps_ExprContext;

  /*
   * Get all tuples from the node below the Hash node and insert into the
   * hash table (or temp files).
   */
  for (;;) {
    bool isnull;
    Datum hashdatum;

    slot = ExecProcNode(outerNode);
    if (TupIsNull(slot))
      break;
    /* We have to compute the hash value */
    econtext->ecxt_outertuple = slot;

    ResetExprContext(econtext);

    hashdatum = ExecEvalExprSwitchContext(node->hash_expr, econtext, &isnull);

    if (!isnull) {
      uint32 hashvalue = DatumGetUInt32(hashdatum);
      int bucketNumber;

      bucketNumber = ExecHashGetSkewBucket(hashtable, hashvalue);
      if (bucketNumber != INVALID_SKEW_BUCKET_NO) {
        /* It's a skew tuple, so put it into that hash table */
        ExecHashSkewTableInsert(hashtable, slot, hashvalue, bucketNumber);
        hashtable->skewTuples += 1;
      } else {
        /* Not subject to skew optimization, so insert normally */
        ExecHashTableInsert(hashtable, slot, hashvalue);
      }
      hashtable->totalTuples += 1;
    }
  }

  /* resize the hash table if needed (NTUP_PER_BUCKET exceeded) */
  if (hashtable->nbuckets != hashtable->nbuckets_optimal)
    ExecHashIncreaseNumBuckets(hashtable);

  /* Account for the buckets in spaceUsed (reported in EXPLAIN ANALYZE) */
  hashtable->spaceUsed += hashtable->nbuckets * sizeof(HashJoinTuple);
  if (hashtable->spaceUsed > hashtable->spacePeak)
    hashtable->spacePeak = hashtable->spaceUsed;
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Init routine for Hash node
 * ----------------------------------------------------------------
 */
HashState *ExecInitHash(Hash *node, EState *estate, int eflags) {
  HashState *hashstate;

  /* check for unsupported flags */
  Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

  /*
   * create state structure
   */
  hashstate = makeNode(HashState);
  hashstate->ps.plan = (Plan *)node;
  hashstate->ps.state = estate;
  hashstate->ps.ExecProcNode = ExecHash;
  /* delay building hashtable until ExecHashTableCreate() in executor run */
  hashstate->hashtable = NULL;

  /*
   * Miscellaneous initialization
   *
   * create expression context for node
   */
  ExecAssignExprContext(estate, &hashstate->ps);

  /*
   * initialize child nodes
   */
  outerPlanState(hashstate) = ExecInitNode(outerPlan(node), estate, eflags);

  /*
   * initialize our result slot and type. No need to build projection
   * because this node doesn't do projections.
   */
  ExecInitResultTupleSlotTL(&hashstate->ps, &TTSOpsMinimalTuple);
  hashstate->ps.ps_ProjInfo = NULL;

  Assert(node->plan.qual == NIL);

  /*
   * Delay initialization of hash_expr until ExecInitHashJoin().  We cannot
   * build the ExprState here as we don't yet know the join type we're going
   * to be hashing values for and we need to know that before calling
   * ExecBuildHash32Expr as the keep_nulls parameter depends on the join
   * type.
   */
  hashstate->hash_expr = NULL;

  return hashstate;
}

/* ---------------------------------------------------------------
 *		ExecEndHash
 *
 *		clean up routine for Hash node
 * ----------------------------------------------------------------
 */
void ExecEndHash(HashState *node) {
  PlanState *outerPlan;

  /*
   * shut down the subplan
   */
  outerPlan = outerPlanState(node);
  ExecEndNode(outerPlan);
}

/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		create an empty hashtable data structure for hashjoin.
 * ----------------------------------------------------------------
 */
HashJoinTable ExecHashTableCreate(HashState *state) {
  Hash *node;
  HashJoinTable hashtable;
  Plan *outerNode;
  size_t space_allowed;
  int nbuckets;
  int nbatch;
  double rows;
  int num_skew_mcvs;
  int log2_nbuckets;
  MemoryContext oldcxt;

  /*
   * Get information about the size of the relation to be hashed (it's the
   * "outer" subtree of this node, but the inner relation of the hashjoin).
   * Compute the appropriate size of the hash table.
   */
  node = (Hash *)state->ps.plan;
  outerNode = outerPlan(node);

  rows = outerNode->plan_rows;

  ExecChooseHashTableSize(rows, outerNode->plan_width,
                          OidIsValid(node->skewTable), &space_allowed,
                          &nbuckets, &nbatch, &num_skew_mcvs);

  /* nbuckets must be a power of 2 */
  log2_nbuckets = my_log2(nbuckets);
  Assert(nbuckets == (1 << log2_nbuckets));

  /*
   * Initialize the hash table control block.
   *
   * The hashtable control block is just palloc'd from the executor's
   * per-query memory context.  Everything else should be kept inside the
   * subsidiary hashCxt, batchCxt or spillCxt.
   */
  hashtable = palloc_object(HashJoinTableData);
  hashtable->nbuckets = nbuckets;
  hashtable->nbuckets_original = nbuckets;
  hashtable->nbuckets_optimal = nbuckets;
  hashtable->log2_nbuckets = log2_nbuckets;
  hashtable->log2_nbuckets_optimal = log2_nbuckets;
  hashtable->buckets = NULL;
  hashtable->skewEnabled = false;
  hashtable->skewBucket = NULL;
  hashtable->skewBucketLen = 0;
  hashtable->nSkewBuckets = 0;
  hashtable->skewBucketNums = NULL;
  hashtable->nbatch = nbatch;
  hashtable->curbatch = 0;
  hashtable->nbatch_original = nbatch;
  hashtable->nbatch_outstart = nbatch;
  hashtable->growEnabled = true;
  hashtable->totalTuples = 0;
  hashtable->skewTuples = 0;
  hashtable->innerBatchFile = NULL;
  hashtable->outerBatchFile = NULL;
  hashtable->spaceUsed = 0;
  hashtable->spacePeak = 0;
  hashtable->spaceAllowed = space_allowed;
  hashtable->spaceUsedSkew = 0;
  hashtable->spaceAllowedSkew =
      hashtable->spaceAllowed * SKEW_HASH_MEM_PERCENT / 100;
  hashtable->chunks = NULL;

#ifdef HJDEBUG
  printf("Hashjoin %p: initial nbatch = %d, nbuckets = %d\n", hashtable, nbatch,
         nbuckets);
#endif

  /*
   * Create temporary memory contexts in which to keep the hashtable working
   * storage.  See notes in executor/hashjoin.h.
   */
  hashtable->hashCxt = AllocSetContextCreate(
      CurrentMemoryContext, "HashTableContext", ALLOCSET_DEFAULT_SIZES);

  hashtable->batchCxt = AllocSetContextCreate(
      hashtable->hashCxt, "HashBatchContext", ALLOCSET_DEFAULT_SIZES);

  hashtable->spillCxt = AllocSetContextCreate(
      hashtable->hashCxt, "HashSpillContext", ALLOCSET_DEFAULT_SIZES);

  /* Allocate data that will live for the life of the hashjoin */

  oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

  if (nbatch > 1) {
    MemoryContext oldctx;

    /*
     * allocate and initialize the file arrays in hashCxt
     */
    oldctx = MemoryContextSwitchTo(hashtable->spillCxt);

    hashtable->innerBatchFile = palloc0_array(BufFile *, nbatch);
    hashtable->outerBatchFile = palloc0_array(BufFile *, nbatch);

    MemoryContextSwitchTo(oldctx);

    /* The files will not be opened until needed. */
  }

  MemoryContextSwitchTo(oldcxt);

  /*
   * Prepare context for the first-scan space allocations; allocate the
   * hashbucket array therein, and set each bucket "empty".
   */
  MemoryContextSwitchTo(hashtable->batchCxt);

  hashtable->buckets = palloc0_array(HashJoinTuple, nbuckets);

  /*
   * Set up for skew optimization, if possible and there's a need for
   * more than one batch.  (In a one-batch join, there's no point in
   * it.)
   */
  if (nbatch > 1)
    ExecHashBuildSkewHash(state, hashtable, node, num_skew_mcvs);

  MemoryContextSwitchTo(oldcxt);

  return hashtable;
}

/*
 * Compute appropriate size for hashtable given the estimated size of the
 * relation to be hashed (number of rows and average row width).
 *
 * This is exported so that the planner's costsize.c can use it.
 */

/* Target bucket loading (tuples per bucket) */
#define NTUP_PER_BUCKET			1

void ExecChooseHashTableSize(double ntuples, int tupwidth, bool useskew,
                             size_t *space_allowed, int *numbuckets,
                             int *numbatches, int *num_skew_mcvs) {
  int tupsize;
  double inner_rel_bytes;
  size_t hash_table_bytes;
  size_t bucket_bytes;
  size_t max_pointers;
  int nbatch = 1;
  int nbuckets;
  double dbuckets;

  /* Force a plausible relation size if no info */
  if (ntuples <= 0.0)
    ntuples = 1000.0;

  /*
   * Estimate tupsize based on footprint of tuple in hashtable... note this
   * does not allow for any palloc overhead.  The manipulations of spaceUsed
   * don't count palloc overhead either.
   */
  tupsize = HJTUPLE_OVERHEAD + MAXALIGN(SizeofMinimalTupleHeader) +
            MAXALIGN(tupwidth);
  inner_rel_bytes = ntuples * tupsize;

  /*
   * Compute in-memory hashtable size limit from GUCs.
   */
  hash_table_bytes = get_hash_memory_limit();

  *space_allowed = hash_table_bytes;

  /*
   * If skew optimization is possible, estimate the number of skew buckets
   * that will fit in the memory allowed, and decrement the assumed space
   * available for the main hash table accordingly.
   *
   * We make the optimistic assumption that each skew bucket will contain
   * one inner-relation tuple.  If that turns out to be low, we will recover
   * at runtime by reducing the number of skew buckets.
   *
   * hashtable->skewBucket will have up to 8 times as many HashSkewBucket
   * pointers as the number of MCVs we allow, since ExecHashBuildSkewHash
   * will round up to the next power of 2 and then multiply by 4 to reduce
   * collisions.
   */
  if (useskew) {
    size_t bytes_per_mcv;
    size_t skew_mcvs;

    /*----------
     * Compute number of MCVs we could hold in hash_table_bytes
     *
     * Divisor is:
     * size of a hash tuple +
     * worst-case size of skewBucket[] per MCV +
     * size of skewBucketNums[] entry +
     * size of skew bucket struct itself
     *----------
     */
    bytes_per_mcv = tupsize + (8 * sizeof(HashSkewBucket *)) + sizeof(int) +
                    SKEW_BUCKET_OVERHEAD;
    skew_mcvs = hash_table_bytes / bytes_per_mcv;

    /*
     * Now scale by SKEW_HASH_MEM_PERCENT (we do it in this order so as
     * not to worry about size_t overflow in the multiplication)
     */
    skew_mcvs = (skew_mcvs * SKEW_HASH_MEM_PERCENT) / 100;

    /* Now clamp to integer range */
    skew_mcvs = Min(skew_mcvs, INT_MAX);

    *num_skew_mcvs = (int)skew_mcvs;

    /* Reduce hash_table_bytes by the amount needed for the skew table */
    if (skew_mcvs > 0)
      hash_table_bytes -= skew_mcvs * bytes_per_mcv;
  } else
    *num_skew_mcvs = 0;

  /*
   * Set nbuckets to achieve an average bucket load of NTUP_PER_BUCKET when
   * memory is filled, assuming a single batch; but limit the value so that
   * the pointer arrays we'll try to allocate do not exceed hash_table_bytes
   * nor MaxAllocSize.
   *
   * Note that both nbuckets and nbatch must be powers of 2 to make
   * ExecHashGetBucketAndBatch fast.
   */
  max_pointers = hash_table_bytes / sizeof(HashJoinTuple);
  max_pointers = Min(max_pointers, MaxAllocSize / sizeof(HashJoinTuple));
  /* If max_pointers isn't a power of 2, must round it down to one */
  max_pointers = pg_prevpower2_size_t(max_pointers);

  /* Also ensure we avoid integer overflow in nbatch and nbuckets */
  /* (this step is redundant given the current value of MaxAllocSize) */
  max_pointers = Min(max_pointers, INT_MAX / 2 + 1);

  dbuckets = ceil(ntuples / NTUP_PER_BUCKET);
  dbuckets = Min(dbuckets, max_pointers);
  nbuckets = (int)dbuckets;
  /* don't let nbuckets be really small, though ... */
  nbuckets = Max(nbuckets, 1024);
  /* ... and force it to be a power of 2. */
  nbuckets = pg_nextpower2_32(nbuckets);

  /*
   * If there's not enough space to store the projected number of tuples and
   * the required bucket headers, we will need multiple batches.
   */
  bucket_bytes = sizeof(HashJoinTuple) * nbuckets;
  if (inner_rel_bytes + bucket_bytes > hash_table_bytes) {
    /* We'll need multiple batches */
    size_t sbuckets;
    double dbatch;
    int minbatch;
    size_t bucket_size;

    /*
     * Estimate the number of buckets we'll want to have when hash_mem is
     * entirely full.  Each bucket will contain a bucket pointer plus
     * NTUP_PER_BUCKET tuples, whose projected size already includes
     * overhead for the hash code, pointer to the next tuple, etc.
     */
    bucket_size = (tupsize * NTUP_PER_BUCKET + sizeof(HashJoinTuple));
    if (hash_table_bytes <= bucket_size)
      sbuckets = 1; /* avoid pg_nextpower2_size_t(0) */
    else
      sbuckets = pg_nextpower2_size_t(hash_table_bytes / bucket_size);
    sbuckets = Min(sbuckets, max_pointers);
    nbuckets = (int)sbuckets;
    nbuckets = pg_nextpower2_32(nbuckets);
    bucket_bytes = nbuckets * sizeof(HashJoinTuple);

    /*
     * Buckets are simple pointers to hashjoin tuples, while tupsize
     * includes the pointer, hash code, and MinimalTupleData.  So buckets
     * should never really exceed 25% of hash_mem (even for
     * NTUP_PER_BUCKET=1); except maybe for hash_mem values that are not
     * 2^N bytes, where we might get more because of doubling. So let's
     * look for 50% here.
     */
    Assert(bucket_bytes <= hash_table_bytes / 2);

    /* Calculate required number of batches. */
    dbatch = ceil(inner_rel_bytes / (hash_table_bytes - bucket_bytes));
    dbatch = Min(dbatch, max_pointers);
    minbatch = (int)dbatch;
    nbatch = pg_nextpower2_32(Max(2, minbatch));
  }

  /*
   * Optimize the total amount of memory consumed by the hash node.
   *
   * The nbatch calculation above focuses on the in-memory hash table,
   * assuming no per-batch overhead. But each batch may have two files, each
   * with a BLCKSZ buffer. For large nbatch values these buffers may use
   * significantly more memory than the hash table.
   *
   * The total memory usage may be expressed by this formula:
   *
   * (inner_rel_bytes / nbatch) + (2 * nbatch * BLCKSZ)
   *
   * where (inner_rel_bytes / nbatch) is the size of the in-memory hash
   * table and (2 * nbatch * BLCKSZ) is the amount of memory used by file
   * buffers.
   *
   * The nbatch calculation however ignores the second part. And for very
   * large inner_rel_bytes, there may be no nbatch that keeps total memory
   * usage under the budget (work_mem * hash_mem_multiplier). To deal with
   * that, we will adjust nbatch to minimize total memory consumption across
   * both the hashtable and file buffers.
   *
   * As we increase the size of the hashtable, the number of batches
   * decreases, and the total memory usage follows a U-shaped curve. We find
   * the minimum nbatch by "walking back" -- checking if halving nbatch
   * would lower the total memory usage. We stop when it no longer helps.
   *
   * We only reduce the number of batches. Adding batches reduces memory
   * usage only when most of the memory is used by the hash table, with
   * total memory usage within the limit or not far from it. We don't want
   * to start batching when not needed, even if that would reduce memory
   * usage.
   *
   * While growing the hashtable, we also adjust the number of buckets to
   * maintain a load factor of NTUP_PER_BUCKET while squeezing tuples back
   * from batches into the hashtable.
   *
   * Note that we can only change nbuckets during initial hashtable sizing.
   * Once we start building the hash, nbuckets is fixed (we may still grow
   * the hash table).
   *
   * We double several parameters (space_allowed, nbuckets, num_skew_mcvs),
   * which introduces a risk of overflow. We avoid this by exiting the loop.
   * We could do something smarter (e.g. capping nbuckets and continue), but
   * the complexity is not worth it. Such cases are extremely rare, and this
   * is a best-effort attempt to reduce memory usage.
   */
  while (nbatch > 1) {
    /* Check that buckets won't overflow MaxAllocSize */
    if (nbuckets > (MaxAllocSize / sizeof(HashJoinTuple) / 2))
      break;

    /* num_skew_mcvs should be less than nbuckets */
    Assert((*num_skew_mcvs) < (INT_MAX / 2));

    /*
     * Check that space_allowed won't overflow SIZE_MAX.
     *
     * We don't use hash_table_bytes here, because it does not include the
     * skew buckets. And we want to limit the overall memory limit.
     */
    if ((*space_allowed) > (SIZE_MAX / 2))
      break;

    /*
     * Will halving the number of batches and doubling the size of the
     * hashtable reduce overall memory usage?
     *
     * This is the same as (S = space_allowed):
     *
     * (S + 2 * nbatch * BLCKSZ) < (S * 2 + nbatch * BLCKSZ)
     *
     * but avoiding intermediate overflow.
     */
    if (nbatch < (*space_allowed) / BLCKSZ)
      break;

    /*
     * MaxAllocSize is sufficiently small that we are not worried about
     * overflowing nbuckets.
     */
    nbuckets *= 2;

    *num_skew_mcvs = (*num_skew_mcvs) * 2;
    *space_allowed = (*space_allowed) * 2;

    nbatch /= 2;
  }

  Assert(nbuckets > 0);
  Assert(nbatch > 0);

  *numbuckets = nbuckets;
  *numbatches = nbatch;
}

/* ----------------------------------------------------------------
 *		ExecHashTableDestroy
 *
 *		destroy a hash table
 * ----------------------------------------------------------------
 */
void ExecHashTableDestroy(HashJoinTable hashtable) {
  int i;

  /*
   * Make sure all the temp files are closed.  We skip batch 0, since it
   * can't have any temp files (and the arrays might not even exist if
   * nbatch is only 1).
   */
  if (hashtable->innerBatchFile != NULL) {
    for (i = 1; i < hashtable->nbatch; i++) {
      if (hashtable->innerBatchFile[i])
        BufFileClose(hashtable->innerBatchFile[i]);
      if (hashtable->outerBatchFile[i])
        BufFileClose(hashtable->outerBatchFile[i]);
    }
  }

  /* Release working memory (batchCxt is a child, so it goes away too) */
  MemoryContextDelete(hashtable->hashCxt);

  /* And drop the control block */
  pfree(hashtable);
}

/*
 * Consider adjusting the allowed hash table size, depending on the number
 * of batches, to minimize the overall memory usage (for both the hashtable
 * and batch files).
 *
 * We're adjusting the size of the hash table, not the (optimal) number of
 * buckets. We can't change that once we start building the hash, due to how
 * ExecHashGetBucketAndBatch calculates batchno/bucketno from the hash. This
 * means the load factor may not be optimal, but we're in damage control so
 * we accept slower lookups. It's still much better than batch explosion.
 *
 * Returns true if we chose to increase the batch size (and thus we don't
 * need to add batches), and false if we should increase nbatch.
 */
static bool ExecHashIncreaseBatchSize(HashJoinTable hashtable) {
  /*
   * How much additional memory would doubling nbatch use? Each batch may
   * require two buffered files (inner/outer), with a BLCKSZ buffer.
   */
  size_t batchSpace = (hashtable->nbatch * 2 * (size_t)BLCKSZ);

  /*
   * Compare the new space needed for doubling nbatch and for enlarging the
   * in-memory hash table. If doubling the hash table needs less memory,
   * just do that. Otherwise, continue with doubling the nbatch.
   *
   * We're either doubling spaceAllowed or batchSpace, so which of those
   * increases the memory usage the least is the same as comparing the
   * values directly.
   */
  if (hashtable->spaceAllowed <= batchSpace) {
    hashtable->spaceAllowed *= 2;
    return true;
  }

  return false;
}

/*
 * ExecHashIncreaseNumBatches
 *		increase the original number of batches in order to reduce
 *		current memory consumption
 */
static void ExecHashIncreaseNumBatches(HashJoinTable hashtable) {
  int oldnbatch = hashtable->nbatch;
  int curbatch = hashtable->curbatch;
  int nbatch;
  long ninmemory;
  long nfreed;
  HashMemoryChunk oldchunks;

  /* do nothing if we've decided to shut off growth */
  if (!hashtable->growEnabled)
    return;

  /* safety check to avoid overflow */
  if (oldnbatch > Min(INT_MAX / 2, MaxAllocSize / (sizeof(void *) * 2)))
    return;

  /* consider increasing size of the in-memory hash table instead */
  if (ExecHashIncreaseBatchSize(hashtable))
    return;

  nbatch = oldnbatch * 2;
  Assert(nbatch > 1);

#ifdef HJDEBUG
  printf("Hashjoin %p: increasing nbatch to %d because space = %zu\n",
         hashtable, nbatch, hashtable->spaceUsed);
#endif

  if (hashtable->innerBatchFile == NULL) {
    MemoryContext oldcxt = MemoryContextSwitchTo(hashtable->spillCxt);

    /* we had no file arrays before */
    hashtable->innerBatchFile = palloc0_array(BufFile *, nbatch);
    hashtable->outerBatchFile = palloc0_array(BufFile *, nbatch);

    MemoryContextSwitchTo(oldcxt);
  } else {
    /* enlarge arrays and zero out added entries */
    hashtable->innerBatchFile = repalloc0_array(hashtable->innerBatchFile,
                                                BufFile *, oldnbatch, nbatch);
    hashtable->outerBatchFile = repalloc0_array(hashtable->outerBatchFile,
                                                BufFile *, oldnbatch, nbatch);
  }

  hashtable->nbatch = nbatch;

  /*
   * Scan through the existing hash table entries and dump out any that are
   * no longer of the current batch.
   */
  ninmemory = nfreed = 0;

  /* If know we need to resize nbuckets, we can do it while rebatching. */
  if (hashtable->nbuckets_optimal != hashtable->nbuckets) {
    /* we never decrease the number of buckets */
    Assert(hashtable->nbuckets_optimal > hashtable->nbuckets);

    hashtable->nbuckets = hashtable->nbuckets_optimal;
    hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

    hashtable->buckets =
        repalloc_array(hashtable->buckets, HashJoinTuple, hashtable->nbuckets);
  }

  /*
   * We will scan through the chunks directly, so that we can reset the
   * buckets now and not have to keep track which tuples in the buckets have
   * already been processed. We will free the old chunks as we go.
   */
  memset(hashtable->buckets, 0, sizeof(HashJoinTuple) * hashtable->nbuckets);
  oldchunks = hashtable->chunks;
  hashtable->chunks = NULL;

  /* so, let's scan through the old chunks, and all tuples in each chunk */
  while (oldchunks != NULL) {
    HashMemoryChunk nextchunk = oldchunks->next;

    /* position within the buffer (up to oldchunks->used) */
    size_t idx = 0;

    /* process all tuples stored in this chunk (and then free it) */
    while (idx < oldchunks->used) {
      HashJoinTuple hashTuple =
          (HashJoinTuple)(HASH_CHUNK_DATA(oldchunks) + idx);
      MinimalTuple tuple = HJTUPLE_MINTUPLE(hashTuple);
      int hashTupleSize = (HJTUPLE_OVERHEAD + tuple->t_len);
      int bucketno;
      int batchno;

      ninmemory++;
      ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue, &bucketno,
                                &batchno);

      if (batchno == curbatch) {
        /* keep tuple in memory - copy it into the new chunk */
        HashJoinTuple copyTuple;

        copyTuple = (HashJoinTuple)dense_alloc(hashtable, hashTupleSize);
        memcpy(copyTuple, hashTuple, hashTupleSize);

        /* and add it back to the appropriate bucket */
        copyTuple->next = hashtable->buckets[bucketno];
        hashtable->buckets[bucketno] = copyTuple;
      } else {
        /* dump it out */
        Assert(batchno > curbatch);
        ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(hashTuple), hashTuple->hashvalue,
                              &hashtable->innerBatchFile[batchno], hashtable);

        hashtable->spaceUsed -= hashTupleSize;
        nfreed++;
      }

      /* next tuple in this chunk */
      idx += MAXALIGN(hashTupleSize);

      /* allow this loop to be cancellable */
      CHECK_FOR_INTERRUPTS();
    }

    /* we're done with this chunk - free it and proceed to the next one */
    pfree(oldchunks);
    oldchunks = nextchunk;
  }

#ifdef HJDEBUG
  printf("Hashjoin %p: freed %ld of %ld tuples, space now %zu\n", hashtable,
         nfreed, ninmemory, hashtable->spaceUsed);
#endif

  /*
   * If we dumped out either all or none of the tuples in the table, disable
   * further expansion of nbatch.  This situation implies that we have
   * enough tuples of identical hashvalues to overflow spaceAllowed.
   * Increasing nbatch will not fix it since there's no way to subdivide the
   * group any more finely. We have to just gut it out and hope the server
   * has enough RAM.
   */
  if (nfreed == 0 || nfreed == ninmemory) {
    hashtable->growEnabled = false;
#ifdef HJDEBUG
    printf("Hashjoin %p: disabling further increase of nbatch\n", hashtable);
#endif
  }
}

/*
 * ExecHashIncreaseNumBuckets
 *		increase the original number of buckets in order to reduce
 *		number of tuples per bucket
 */
static void ExecHashIncreaseNumBuckets(HashJoinTable hashtable) {
  HashMemoryChunk chunk;

  /* do nothing if not an increase (it's called increase for a reason) */
  if (hashtable->nbuckets >= hashtable->nbuckets_optimal)
    return;

#ifdef HJDEBUG
  printf("Hashjoin %p: increasing nbuckets %d => %d\n", hashtable,
         hashtable->nbuckets, hashtable->nbuckets_optimal);
#endif

  hashtable->nbuckets = hashtable->nbuckets_optimal;
  hashtable->log2_nbuckets = hashtable->log2_nbuckets_optimal;

  Assert(hashtable->nbuckets > 1);
  Assert(hashtable->nbuckets <= (INT_MAX / 2));
  Assert(hashtable->nbuckets == (1 << hashtable->log2_nbuckets));

  /*
   * Just reallocate the proper number of buckets - we don't need to walk
   * through them - we can walk the dense-allocated chunks (just like in
   * ExecHashIncreaseNumBatches, but without all the copying into new
   * chunks)
   */
  hashtable->buckets =
      repalloc_array(hashtable->buckets, HashJoinTuple, hashtable->nbuckets);

  memset(hashtable->buckets, 0, hashtable->nbuckets * sizeof(HashJoinTuple));

  /* scan through all tuples in all chunks to rebuild the hash table */
  for (chunk = hashtable->chunks; chunk != NULL; chunk = chunk->next) {
    /* process all tuples stored in this chunk */
    size_t idx = 0;

    while (idx < chunk->used) {
      HashJoinTuple hashTuple = (HashJoinTuple)(HASH_CHUNK_DATA(chunk) + idx);
      int bucketno;
      int batchno;

      ExecHashGetBucketAndBatch(hashtable, hashTuple->hashvalue, &bucketno,
                                &batchno);

      /* add the tuple to the proper bucket */
      hashTuple->next = hashtable->buckets[bucketno];
      hashtable->buckets[bucketno] = hashTuple;

      /* advance index past the tuple */
      idx += MAXALIGN(HJTUPLE_OVERHEAD + HJTUPLE_MINTUPLE(hashTuple)->t_len);
    }

    /* allow this loop to be cancellable */
    CHECK_FOR_INTERRUPTS();
  }
}

/*
 * ExecHashTableInsert
 *		insert a tuple into the hash table depending on the hash value
 *		it may just go to a temp file for later batches
 *
 * Note: the passed TupleTableSlot may contain a regular, minimal, or virtual
 * tuple; the minimal case in particular is certain to happen while reloading
 * tuples from batch files.  We could save some cycles in the regular-tuple
 * case by not forcing the slot contents into minimal form; not clear if it's
 * worth the messiness required.
 */
void ExecHashTableInsert(HashJoinTable hashtable, TupleTableSlot *slot,
                         uint32 hashvalue) {
  bool shouldFree;
  MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
  int bucketno;
  int batchno;

  ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

  /*
   * decide whether to put the tuple in the hash table or a temp file
   */
  if (batchno == hashtable->curbatch) {
    /*
     * put the tuple in hash table
     */
    HashJoinTuple hashTuple;
    int hashTupleSize;
    double ntuples = (hashtable->totalTuples - hashtable->skewTuples);

    /* Create the HashJoinTuple */
    hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
    hashTuple = (HashJoinTuple)dense_alloc(hashtable, hashTupleSize);

    hashTuple->hashvalue = hashvalue;
    memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);

    /*
     * We always reset the tuple-matched flag on insertion.  This is okay
     * even when reloading a tuple from a batch file, since the tuple
     * could not possibly have been matched to an outer tuple before it
     * went into the batch file.
     */
    HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

    /* Push it onto the front of the bucket's list */
    hashTuple->next = hashtable->buckets[bucketno];
    hashtable->buckets[bucketno] = hashTuple;

    /*
     * Increase the (optimal) number of buckets if we just exceeded the
     * NTUP_PER_BUCKET threshold, but only when there's still a single
     * batch.
     */
    if (hashtable->nbatch == 1 &&
        ntuples > (hashtable->nbuckets_optimal * NTUP_PER_BUCKET)) {
      /* Guard against integer overflow and alloc size overflow */
      if (hashtable->nbuckets_optimal <= INT_MAX / 2 &&
          hashtable->nbuckets_optimal * 2 <=
              MaxAllocSize / sizeof(HashJoinTuple)) {
        hashtable->nbuckets_optimal *= 2;
        hashtable->log2_nbuckets_optimal += 1;
      }
    }

    /* Account for space used, and back off if we've used too much */
    hashtable->spaceUsed += hashTupleSize;
    if (hashtable->spaceUsed > hashtable->spacePeak)
      hashtable->spacePeak = hashtable->spaceUsed;
    if (hashtable->spaceUsed +
            hashtable->nbuckets_optimal * sizeof(HashJoinTuple) >
        hashtable->spaceAllowed)
      ExecHashIncreaseNumBatches(hashtable);
  } else {
    /*
     * put the tuple into a temp file for later batches
     */
    Assert(batchno > hashtable->curbatch);
    ExecHashJoinSaveTuple(tuple, hashvalue, &hashtable->innerBatchFile[batchno],
                          hashtable);
  }

  if (shouldFree)
    heap_free_minimal_tuple(tuple);
}

/*
 * ExecHashGetBucketAndBatch
 *		Determine the bucket number and batch number for a hash value
 *
 * Note: on-the-fly increases of nbatch must not change the bucket number
 * for a given hash code (since we don't move tuples to different hash
 * chains), and must only cause the batch number to remain the same or
 * increase.  Our algorithm is
 *		bucketno = hashvalue MOD nbuckets
 *		batchno = ROR(hashvalue, log2_nbuckets) MOD nbatch
 * where nbuckets and nbatch are both expected to be powers of 2, so we can
 * do the computations by shifting and masking.  (This assumes that all hash
 * functions are good about randomizing all their output bits, else we are
 * likely to have very skewed bucket or batch occupancy.)
 *
 * nbuckets and log2_nbuckets may change while nbatch == 1 because of dynamic
 * bucket count growth.  Once we start batching, the value is fixed and does
 * not change over the course of the join (making it possible to compute batch
 * number the way we do here).
 *
 * nbatch is always a power of 2; we increase it only by doubling it.  This
 * effectively adds one more bit to the top of the batchno.  In very large
 * joins, we might run out of bits to add, so we do this by rotating the hash
 * value.  This causes batchno to steal bits from bucketno when the number of
 * virtual buckets exceeds 2^32.  It's better to have longer bucket chains
 * than to lose the ability to divide batches.
 */
void ExecHashGetBucketAndBatch(HashJoinTable hashtable, uint32 hashvalue,
                               int *bucketno, int *batchno) {
  uint32 nbuckets = (uint32)hashtable->nbuckets;
  uint32 nbatch = (uint32)hashtable->nbatch;

  if (nbatch > 1) {
    *bucketno = hashvalue & (nbuckets - 1);
    *batchno =
        pg_rotate_right32(hashvalue, hashtable->log2_nbuckets) & (nbatch - 1);
  } else {
    *bucketno = hashvalue & (nbuckets - 1);
    *batchno = 0;
  }
}

/*
 * ExecScanHashBucket
 *		scan a hash bucket for matches to the current outer tuple
 *
 * The current outer tuple must be stored in econtext->ecxt_outertuple.
 *
 * On success, the inner tuple is stored into hjstate->hj_CurTuple and
 * econtext->ecxt_innertuple, using hjstate->hj_HashTupleSlot as the slot
 * for the latter.
 */
bool ExecScanHashBucket(HashJoinState *hjstate, ExprContext *econtext) {
  ExprState *hjclauses = hjstate->hashclauses;
  HashJoinTable hashtable = hjstate->hj_HashTable;
  HashJoinTuple hashTuple = hjstate->hj_CurTuple;
  uint32 hashvalue = hjstate->hj_CurHashValue;

  /*
   * hj_CurTuple is the address of the tuple last returned from the current
   * bucket, or NULL if it's time to start scanning a new bucket.
   *
   * If the tuple hashed to a skew bucket then scan the skew bucket
   * otherwise scan the standard hashtable bucket.
   */
  if (hashTuple != NULL)
    hashTuple = hashTuple->next;
  else if (hjstate->hj_CurSkewBucketNo != INVALID_SKEW_BUCKET_NO)
    hashTuple = hashtable->skewBucket[hjstate->hj_CurSkewBucketNo]->tuples;
  else
    hashTuple = hashtable->buckets[hjstate->hj_CurBucketNo];

  while (hashTuple != NULL) {
    if (hashTuple->hashvalue == hashvalue) {
      TupleTableSlot *inntuple;

      /* insert hashtable's tuple into exec slot so ExecQual sees it */
      inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
                                       hjstate->hj_HashTupleSlot,
                                       false); /* do not pfree */
      econtext->ecxt_innertuple = inntuple;

      if (ExecQualAndReset(hjclauses, econtext)) {
        hjstate->hj_CurTuple = hashTuple;
        return true;
      }
    }

    hashTuple = hashTuple->next;
  }

  /*
   * no match
   */
  return false;
}

/*
 * ExecPrepHashTableForUnmatched
 *		set up for a series of ExecScanHashTableForUnmatched calls
 */
void ExecPrepHashTableForUnmatched(HashJoinState *hjstate) {
  /*----------
   * During this scan we use the HashJoinState fields as follows:
   *
   * hj_CurBucketNo: next regular bucket to scan
   * hj_CurSkewBucketNo: next skew bucket (an index into skewBucketNums)
   * hj_CurTuple: last tuple returned, or NULL to start next bucket
   *----------
   */
  hjstate->hj_CurBucketNo = 0;
  hjstate->hj_CurSkewBucketNo = 0;
  hjstate->hj_CurTuple = NULL;
}

/*
 * ExecScanHashTableForUnmatched
 *		scan the hash table for unmatched inner tuples
 *
 * On success, the inner tuple is stored into hjstate->hj_CurTuple and
 * econtext->ecxt_innertuple, using hjstate->hj_HashTupleSlot as the slot
 * for the latter.
 */
bool ExecScanHashTableForUnmatched(HashJoinState *hjstate,
                                   ExprContext *econtext) {
  HashJoinTable hashtable = hjstate->hj_HashTable;
  HashJoinTuple hashTuple = hjstate->hj_CurTuple;

  for (;;) {
    /*
     * hj_CurTuple is the address of the tuple last returned from the
     * current bucket, or NULL if it's time to start scanning a new
     * bucket.
     */
    if (hashTuple != NULL)
      hashTuple = hashTuple->next;
    else if (hjstate->hj_CurBucketNo < hashtable->nbuckets) {
      hashTuple = hashtable->buckets[hjstate->hj_CurBucketNo];
      hjstate->hj_CurBucketNo++;
    } else if (hjstate->hj_CurSkewBucketNo < hashtable->nSkewBuckets) {
      int j = hashtable->skewBucketNums[hjstate->hj_CurSkewBucketNo];

      hashTuple = hashtable->skewBucket[j]->tuples;
      hjstate->hj_CurSkewBucketNo++;
    } else
      break; /* finished all buckets */

    while (hashTuple != NULL) {
      if (!HeapTupleHeaderHasMatch(HJTUPLE_MINTUPLE(hashTuple))) {
        TupleTableSlot *inntuple;

        /* insert hashtable's tuple into exec slot */
        inntuple = ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
                                         hjstate->hj_HashTupleSlot,
                                         false); /* do not pfree */
        econtext->ecxt_innertuple = inntuple;

        /*
         * Reset temp memory each time; although this function doesn't
         * do any qual eval, the caller will, so let's keep it
         * parallel to ExecScanHashBucket.
         */
        ResetExprContext(econtext);

        hjstate->hj_CurTuple = hashTuple;
        return true;
      }

      hashTuple = hashTuple->next;
    }

    /* allow this loop to be cancellable */
    CHECK_FOR_INTERRUPTS();
  }

  /*
   * no more unmatched tuples
   */
  return false;
}

/*
 * ExecHashTableReset
 *
 *		reset hash table header for new batch
 */
void ExecHashTableReset(HashJoinTable hashtable) {
  MemoryContext oldcxt;
  int nbuckets = hashtable->nbuckets;

  /*
   * Release all the hash buckets and tuples acquired in the prior pass, and
   * reinitialize the context for a new pass.
   */
  MemoryContextReset(hashtable->batchCxt);
  oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);

  /* Reallocate and reinitialize the hash bucket headers. */
  hashtable->buckets = palloc0_array(HashJoinTuple, nbuckets);

  hashtable->spaceUsed = 0;

  MemoryContextSwitchTo(oldcxt);

  /* Forget the chunks (the memory was freed by the context reset above). */
  hashtable->chunks = NULL;
}

/*
 * ExecHashTableResetMatchFlags
 *		Clear all the HeapTupleHeaderHasMatch flags in the table
 */
void ExecHashTableResetMatchFlags(HashJoinTable hashtable) {
  HashJoinTuple tuple;
  int i;

  /* Reset all flags in the main table ... */
  for (i = 0; i < hashtable->nbuckets; i++) {
    for (tuple = hashtable->buckets[i]; tuple != NULL; tuple = tuple->next)
      HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
  }

  /* ... and the same for the skew buckets, if any */
  for (i = 0; i < hashtable->nSkewBuckets; i++) {
    int j = hashtable->skewBucketNums[i];
    HashSkewBucket *skewBucket = hashtable->skewBucket[j];

    for (tuple = skewBucket->tuples; tuple != NULL; tuple = tuple->next)
      HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(tuple));
  }
}

void ExecReScanHash(HashState *node) {
  PlanState *outerPlan = outerPlanState(node);

  /*
   * if chgParam of subnode is not null then plan will be re-scanned by
   * first ExecProcNode.
   */
  if (outerPlan->chgParam == NULL)
    ExecReScan(outerPlan);
}

/*
 * ExecHashBuildSkewHash
 *
 *		Set up for skew optimization if we can identify the most common
 *values (MCVs) of the outer relation's join key.  We make a skew hash bucket
 *		for the hash value of each MCV, up to the number of slots
 *allowed based on available memory.
 */
static void ExecHashBuildSkewHash(HashState *hashstate, HashJoinTable hashtable,
                                  Hash *node, int mcvsToUse) {
  HeapTupleData *statsTuple;
  AttStatsSlot sslot;

  /* Do nothing if planner didn't identify the outer relation's join key */
  if (!OidIsValid(node->skewTable))
    return;
  /* Also, do nothing if we don't have room for at least one skew bucket */
  if (mcvsToUse <= 0)
    return;

  /*
   * Try to find the MCV statistics for the outer relation's join key.
   */
  statsTuple =
      get_statistic_tuple(node->skewTable, node->skewColumn, node->skewInherit);
  if (!HeapTupleIsValid(statsTuple))
    return;

  if (get_attstatsslot(&sslot, statsTuple, STATISTIC_KIND_MCV, InvalidOid,
                       ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)) {
    double frac;
    int nbuckets;
    int i;

    if (mcvsToUse > sslot.nvalues)
      mcvsToUse = sslot.nvalues;

    /*
     * Calculate the expected fraction of outer relation that will
     * participate in the skew optimization.  If this isn't at least
     * SKEW_MIN_OUTER_FRACTION, don't use skew optimization.
     */
    frac = 0;
    for (i = 0; i < mcvsToUse; i++)
      frac += sslot.numbers[i];
    if (frac < SKEW_MIN_OUTER_FRACTION) {
      free_attstatsslot(&sslot);
      heap_freetuple(statsTuple);
      return;
    }

    /*
     * Okay, set up the skew hashtable.
     *
     * skewBucket[] is an open addressing hashtable with a power of 2 size
     * that is greater than the number of MCV values.  (This ensures there
     * will be at least one null entry, so searches will always
     * terminate.)
     *
     * Note: this code could fail if mcvsToUse exceeds INT_MAX/8 or
     * MaxAllocSize/sizeof(void *)/8, but that is not currently possible
     * since we limit pg_statistic entries to much less than that.
     */
    nbuckets = pg_nextpower2_32(mcvsToUse + 1);
    /* use two more bits just to help avoid collisions */
    nbuckets <<= 2;

    hashtable->skewEnabled = true;
    hashtable->skewBucketLen = nbuckets;

    /*
     * We allocate the bucket memory in the hashtable's batch context. It
     * is only needed during the first batch, and this ensures it will be
     * automatically removed once the first batch is done.
     */
    hashtable->skewBucket = (HashSkewBucket **)MemoryContextAllocZero(
        hashtable->batchCxt, nbuckets * sizeof(HashSkewBucket *));
    hashtable->skewBucketNums = (int *)MemoryContextAllocZero(
        hashtable->batchCxt, mcvsToUse * sizeof(int));

    hashtable->spaceUsed +=
        nbuckets * sizeof(HashSkewBucket *) + mcvsToUse * sizeof(int);
    hashtable->spaceUsedSkew +=
        nbuckets * sizeof(HashSkewBucket *) + mcvsToUse * sizeof(int);
    if (hashtable->spaceUsed > hashtable->spacePeak)
      hashtable->spacePeak = hashtable->spaceUsed;

    /*
     * Create a skew bucket for each MCV hash value.
     *
     * Note: it is very important that we create the buckets in order of
     * decreasing MCV frequency.  If we have to remove some buckets, they
     * must be removed in reverse order of creation (see notes in
     * ExecHashRemoveNextSkewBucket) and we want the least common MCVs to
     * be removed first.
     */

    for (i = 0; i < mcvsToUse; i++) {
      uint32 hashvalue;
      int bucket;

      hashvalue = DatumGetUInt32(FunctionCall1Coll(hashstate->skew_hashfunction,
                                                   hashstate->skew_collation,
                                                   sslot.values[i]));

      /*
       * While we have not hit a hole in the hashtable and have not hit
       * the desired bucket, we have collided with some previous hash
       * value, so try the next bucket location.  NB: this code must
       * match ExecHashGetSkewBucket.
       */
      bucket = hashvalue & (nbuckets - 1);
      while (hashtable->skewBucket[bucket] != NULL &&
             hashtable->skewBucket[bucket]->hashvalue != hashvalue)
        bucket = (bucket + 1) & (nbuckets - 1);

      /*
       * If we found an existing bucket with the same hashvalue, leave
       * it alone.  It's okay for two MCVs to share a hashvalue.
       */
      if (hashtable->skewBucket[bucket] != NULL)
        continue;

      /* Okay, create a new skew bucket for this hashvalue. */
      hashtable->skewBucket[bucket] = (HashSkewBucket *)MemoryContextAlloc(
          hashtable->batchCxt, sizeof(HashSkewBucket));
      hashtable->skewBucket[bucket]->hashvalue = hashvalue;
      hashtable->skewBucket[bucket]->tuples = NULL;
      hashtable->skewBucketNums[hashtable->nSkewBuckets] = bucket;
      hashtable->nSkewBuckets++;
      hashtable->spaceUsed += SKEW_BUCKET_OVERHEAD;
      hashtable->spaceUsedSkew += SKEW_BUCKET_OVERHEAD;
      if (hashtable->spaceUsed > hashtable->spacePeak)
        hashtable->spacePeak = hashtable->spaceUsed;
    }

    free_attstatsslot(&sslot);
  }

  heap_freetuple(statsTuple);
}

/*
 * ExecHashGetSkewBucket
 *
 *		Returns the index of the skew bucket for this hashvalue,
 *		or INVALID_SKEW_BUCKET_NO if the hashvalue is not
 *		associated with any active skew bucket.
 */
int ExecHashGetSkewBucket(HashJoinTable hashtable, uint32 hashvalue) {
  int bucket;

  /*
   * Always return INVALID_SKEW_BUCKET_NO if not doing skew optimization (in
   * particular, this happens after the initial batch is done).
   */
  if (!hashtable->skewEnabled)
    return INVALID_SKEW_BUCKET_NO;

  /*
   * Since skewBucketLen is a power of 2, we can do a modulo by ANDing.
   */
  bucket = hashvalue & (hashtable->skewBucketLen - 1);

  /*
   * While we have not hit a hole in the hashtable and have not hit the
   * desired bucket, we have collided with some other hash value, so try the
   * next bucket location.
   */
  while (hashtable->skewBucket[bucket] != NULL &&
         hashtable->skewBucket[bucket]->hashvalue != hashvalue)
    bucket = (bucket + 1) & (hashtable->skewBucketLen - 1);

  /*
   * Found the desired bucket?
   */
  if (hashtable->skewBucket[bucket] != NULL)
    return bucket;

  /*
   * There must not be any hashtable entry for this hash value.
   */
  return INVALID_SKEW_BUCKET_NO;
}

/*
 * ExecHashSkewTableInsert
 *
 *		Insert a tuple into the skew hashtable.
 *
 * This should generally match up with the current-batch case in
 * ExecHashTableInsert.
 */
static void ExecHashSkewTableInsert(HashJoinTable hashtable,
                                    TupleTableSlot *slot, uint32 hashvalue,
                                    int bucketNumber) {
  bool shouldFree;
  MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
  HashJoinTuple hashTuple;
  int hashTupleSize;

  /* Create the HashJoinTuple */
  hashTupleSize = HJTUPLE_OVERHEAD + tuple->t_len;
  hashTuple =
      (HashJoinTuple)MemoryContextAlloc(hashtable->batchCxt, hashTupleSize);
  hashTuple->hashvalue = hashvalue;
  memcpy(HJTUPLE_MINTUPLE(hashTuple), tuple, tuple->t_len);
  HeapTupleHeaderClearMatch(HJTUPLE_MINTUPLE(hashTuple));

  /* Push it onto the front of the skew bucket's list */
  hashTuple->next = hashtable->skewBucket[bucketNumber]->tuples;
  hashtable->skewBucket[bucketNumber]->tuples = hashTuple;
  Assert(hashTuple != hashTuple->next);

  /* Account for space used, and back off if we've used too much */
  hashtable->spaceUsed += hashTupleSize;
  hashtable->spaceUsedSkew += hashTupleSize;
  if (hashtable->spaceUsed > hashtable->spacePeak)
    hashtable->spacePeak = hashtable->spaceUsed;
  while (hashtable->spaceUsedSkew > hashtable->spaceAllowedSkew)
    ExecHashRemoveNextSkewBucket(hashtable);

  /* Check we are not over the total spaceAllowed, either */
  if (hashtable->spaceUsed > hashtable->spaceAllowed)
    ExecHashIncreaseNumBatches(hashtable);

  if (shouldFree)
    heap_free_minimal_tuple(tuple);
}

/*
 *		ExecHashRemoveNextSkewBucket
 *
 *		Remove the least valuable skew bucket by pushing its tuples into
 *		the main hash table.
 */
static void ExecHashRemoveNextSkewBucket(HashJoinTable hashtable) {
  int bucketToRemove;
  HashSkewBucket *bucket;
  uint32 hashvalue;
  int bucketno;
  int batchno;
  HashJoinTuple hashTuple;

  /* Locate the bucket to remove */
  bucketToRemove = hashtable->skewBucketNums[hashtable->nSkewBuckets - 1];
  bucket = hashtable->skewBucket[bucketToRemove];

  /*
   * Calculate which bucket and batch the tuples belong to in the main
   * hashtable.  They all have the same hash value, so it's the same for all
   * of them.  Also note that it's not possible for nbatch to increase while
   * we are processing the tuples.
   */
  hashvalue = bucket->hashvalue;
  ExecHashGetBucketAndBatch(hashtable, hashvalue, &bucketno, &batchno);

  /* Process all tuples in the bucket */
  hashTuple = bucket->tuples;
  while (hashTuple != NULL) {
    HashJoinTuple nextHashTuple = hashTuple->next;
    MinimalTuple tuple;
    Size tupleSize;

    /*
     * This code must agree with ExecHashTableInsert.  We do not use
     * ExecHashTableInsert directly as ExecHashTableInsert expects a
     * TupleTableSlot while we already have HashJoinTuples.
     */
    tuple = HJTUPLE_MINTUPLE(hashTuple);
    tupleSize = HJTUPLE_OVERHEAD + tuple->t_len;

    /* Decide whether to put the tuple in the hash table or a temp file */
    if (batchno == hashtable->curbatch) {
      /* Move the tuple to the main hash table */
      HashJoinTuple copyTuple;

      /*
       * We must copy the tuple into the dense storage, else it will not
       * be found by, eg, ExecHashIncreaseNumBatches.
       */
      copyTuple = (HashJoinTuple)dense_alloc(hashtable, tupleSize);
      memcpy(copyTuple, hashTuple, tupleSize);
      pfree(hashTuple);

      copyTuple->next = hashtable->buckets[bucketno];
      hashtable->buckets[bucketno] = copyTuple;

      /* We have reduced skew space, but overall space doesn't change */
      hashtable->spaceUsedSkew -= tupleSize;
    } else {
      /* Put the tuple into a temp file for later batches */
      Assert(batchno > hashtable->curbatch);
      ExecHashJoinSaveTuple(tuple, hashvalue,
                            &hashtable->innerBatchFile[batchno], hashtable);
      pfree(hashTuple);
      hashtable->spaceUsed -= tupleSize;
      hashtable->spaceUsedSkew -= tupleSize;
    }

    hashTuple = nextHashTuple;

    /* allow this loop to be cancellable */
    CHECK_FOR_INTERRUPTS();
  }

  /*
   * Free the bucket struct itself and reset the hashtable entry to NULL.
   *
   * NOTE: this is not nearly as simple as it looks on the surface, because
   * of the possibility of collisions in the hashtable.  Suppose that hash
   * values A and B collide at a particular hashtable entry, and that A was
   * entered first so B gets shifted to a different table entry.  If we were
   * to remove A first then ExecHashGetSkewBucket would mistakenly start
   * reporting that B is not in the hashtable, because it would hit the NULL
   * before finding B.  However, we always remove entries in the reverse
   * order of creation, so this failure cannot happen.
   */
  hashtable->skewBucket[bucketToRemove] = NULL;
  hashtable->nSkewBuckets--;
  pfree(bucket);
  hashtable->spaceUsed -= SKEW_BUCKET_OVERHEAD;
  hashtable->spaceUsedSkew -= SKEW_BUCKET_OVERHEAD;

  /*
   * If we have removed all skew buckets then give up on skew optimization.
   * Release the arrays since they aren't useful any more.
   */
  if (hashtable->nSkewBuckets == 0) {
    hashtable->skewEnabled = false;
    pfree(hashtable->skewBucket);
    pfree(hashtable->skewBucketNums);
    hashtable->skewBucket = NULL;
    hashtable->skewBucketNums = NULL;
    hashtable->spaceUsed -= hashtable->spaceUsedSkew;
    hashtable->spaceUsedSkew = 0;
  }
}

/*
 * Collect EXPLAIN stats if needed, saving them into local storage.
 */
void ExecShutdownHash(HashState *node) {
  /* Allocate save space if EXPLAIN'ing and we didn't do so already */
  if (node->ps.instrument && !node->hinstrument)
    node->hinstrument = palloc0_object(HashInstrumentation);
  /* Now accumulate data for the current (final) hash table */
  if (node->hinstrument && node->hashtable)
    ExecHashAccumInstrumentation(node->hinstrument, node->hashtable);
}

/*
 * Accumulate instrumentation data from 'hashtable' into an
 * initially-zeroed HashInstrumentation struct.
 *
 * This is used to merge information across successive hash table instances
 * within a single plan node.  We take the maximum values of each interesting
 * number.  The largest nbuckets and largest nbatch values might have occurred
 * in different instances, so there's some risk of confusion from reporting
 * unrelated numbers; but there's a bigger risk of misdiagnosing a performance
 * issue if we don't report the largest values.  Similarly, we want to report
 * the largest spacePeak regardless of whether it happened in the same
 * instance as the largest nbuckets or nbatch.  All the instances should have
 * the same nbuckets_original and nbatch_original; but there's little value
 * in depending on that here, so handle them the same way.
 */
void ExecHashAccumInstrumentation(HashInstrumentation *instrument,
                                  HashJoinTable hashtable) {
  instrument->nbuckets = Max(instrument->nbuckets, hashtable->nbuckets);
  instrument->nbuckets_original =
      Max(instrument->nbuckets_original, hashtable->nbuckets_original);
  instrument->nbatch = Max(instrument->nbatch, hashtable->nbatch);
  instrument->nbatch_original =
      Max(instrument->nbatch_original, hashtable->nbatch_original);
  instrument->space_peak = Max(instrument->space_peak, hashtable->spacePeak);
}

/*
 * Allocate 'size' bytes from the currently active HashMemoryChunk
 */
static void *dense_alloc(HashJoinTable hashtable, Size size) {
  HashMemoryChunk newChunk;
  char *ptr;

  /* just in case the size is not already aligned properly */
  size = MAXALIGN(size);

  /*
   * If tuple size is larger than threshold, allocate a separate chunk.
   */
  if (size > HASH_CHUNK_THRESHOLD) {
    /* allocate new chunk and put it at the beginning of the list */
    newChunk = (HashMemoryChunk)MemoryContextAlloc(
        hashtable->batchCxt, HASH_CHUNK_HEADER_SIZE + size);
    newChunk->maxlen = size;
    newChunk->used = size;
    newChunk->ntuples = 1;

    /*
     * Add this chunk to the list after the first existing chunk, so that
     * we don't lose the remaining space in the "current" chunk.
     */
    if (hashtable->chunks != NULL) {
      newChunk->next = hashtable->chunks->next;
      hashtable->chunks->next = newChunk;
    } else {
      newChunk->next = hashtable->chunks;
      hashtable->chunks = newChunk;
    }

    return HASH_CHUNK_DATA(newChunk);
  }

  /*
   * See if we have enough space for it in the current chunk (if any). If
   * not, allocate a fresh chunk.
   */
  if ((hashtable->chunks == NULL) ||
      (hashtable->chunks->maxlen - hashtable->chunks->used) < size) {
    /* allocate new chunk and put it at the beginning of the list */
    newChunk = (HashMemoryChunk)MemoryContextAlloc(
        hashtable->batchCxt, HASH_CHUNK_HEADER_SIZE + HASH_CHUNK_SIZE);

    newChunk->maxlen = HASH_CHUNK_SIZE;
    newChunk->used = size;
    newChunk->ntuples = 1;

    newChunk->next = hashtable->chunks;
    hashtable->chunks = newChunk;

    return HASH_CHUNK_DATA(newChunk);
  }

  /* There is enough space in the current chunk, let's add the tuple */
  ptr = HASH_CHUNK_DATA(hashtable->chunks) + hashtable->chunks->used;
  hashtable->chunks->used += size;
  hashtable->chunks->ntuples += 1;

  /* return pointer to the start of the tuple memory */
  return ptr;
}

/*
 * Calculate the limit on how much memory can be used by Hash and similar
 * plan types.  This is work_mem times hash_mem_multiplier, and is
 * expressed in bytes.
 *
 * Exported for use by the planner, as well as other hash-like executor
 * nodes.  This is a rather random place for this, but there is no better
 * place.
 */
size_t get_hash_memory_limit(void) {
  double mem_limit;

  /* Do initial calculation in double arithmetic */
  mem_limit = (double)tcb->guc.work_mem * hash_mem_multiplier * 1024.0;

  /* Clamp in case it doesn't fit in size_t */
  mem_limit = Min(mem_limit, (double)SIZE_MAX);

  return (size_t)mem_limit;
}
