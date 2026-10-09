/*-------------------------------------------------------------------------
 *
 * hashjoin.h
 *	  internal structures for hash joins
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/hashjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/buffile.h"
#include "storage/lwlock.h"

/* ----------------------------------------------------------------
 *				hash-join hash table structures
 *
 * Each active hashjoin has a HashJoinTable structure, which is
 * palloc'd in the executor's per-query context.  Other storage needed for
 * each hashjoin is kept in child contexts, three for each hashjoin:
 *   - HashTableContext (hashCxt): the parent hash table storage context
 *   - HashSpillContext (spillCxt): storage for temp files buffers
 *   - HashBatchContext (batchCxt): storage for a batch in serial hash join
 *
 * The hashtable contexts are made children of the per-query context, ensuring
 * that they will be discarded at end of statement even if the join is
 * aborted early by an error.  (Likewise, any temporary files we make will
 * be cleaned up by the virtual file manager in event of an error.)
 *
 * Storage that should live through the entire join is allocated from the
 * "hashCxt" (mainly the hashtable's metadata). Also, the "hashCxt" context is
 * the parent of "spillCxt" and "batchCxt". It makes it easy and fast to
 * release the storage when we don't need it anymore.
 *
 * Data associated with temp files is allocated in the "spillCxt" context
 * which lives for the duration of the entire join as batch files'
 * creation and usage may span batch execution. These files are
 * explicitly destroyed by calling BufFileClose() when the code is done
 * with them. The aim of this context is to help accounting for the
 * memory allocated for temp files and their buffers.
 *
 * Finally, data used only during a single batch's execution is allocated
 * in the "batchCxt". By resetting the batchCxt at the end of each batch,
 * we free all the per-batch storage reliably and without tedium.
 *
 * During first scan of inner relation, we get its tuples from executor.
 * If nbatch > 1 then tuples that don't belong in first batch get saved
 * into inner-batch temp files. The same statements apply for the
 * first scan of the outer relation, except we write tuples to outer-batch
 * temp files.  After finishing the first scan, we do the following for
 * each remaining batch:
 *	1. Read tuples from inner batch file, load into hash buckets.
 *	2. Read tuples from outer batch file, match to hash buckets and output.
 *
 * It is possible to increase nbatch on the fly if the in-memory hash table
 * gets too big.  The hash-value-to-batch computation is arranged so that this
 * can only cause a tuple to go into a later batch than previously thought,
 * never into an earlier batch.  When we increase nbatch, we rescan the hash
 * table and dump out any tuples that are now of a later batch to the correct
 * inner batch file.  Subsequently, while reading either inner or outer batch
 * files, we might find tuples that no longer belong to the current batch;
 * if so, we just dump them out to the correct batch file.
 * ----------------------------------------------------------------
 */

/* these are in nodes/execnodes.h: */
/* typedef struct HashJoinTupleData *HashJoinTuple; */
/* typedef struct HashJoinTableData *HashJoinTable; */

typedef struct HashJoinTupleData {
  /* link to next tuple in same bucket */
  struct HashJoinTupleData *next;
  uint32 hashvalue; /* tuple's hash code */
  /* Tuple data, in MinimalTuple format, follows on a MAXALIGN boundary */
} HashJoinTupleData;

#define HJTUPLE_OVERHEAD  MAXALIGN(sizeof(HashJoinTupleData))
#define HJTUPLE_MINTUPLE(hjtup)  \
	((MinimalTuple) ((char *) (hjtup) + HJTUPLE_OVERHEAD))

/*
 * If the outer relation's distribution is sufficiently nonuniform, we attempt
 * to optimize the join by treating the hash values corresponding to the outer
 * relation's MCVs specially.  Inner relation tuples matching these hash
 * values go into the "skew" hashtable instead of the main hashtable, and
 * outer relation tuples with these hash values are matched against that
 * table instead of the main one.  Thus, tuples with these hash values are
 * effectively handled as part of the first batch and will never go to disk.
 * The skew hashtable is limited to SKEW_HASH_MEM_PERCENT of the total memory
 * allowed for the join; while building the hashtables, we decrease the number
 * of MCVs being specially treated if needed to stay under this limit.
 *
 * Note: you might wonder why we look at the outer relation stats for this,
 * rather than the inner.  One reason is that the outer relation is typically
 * bigger, so we get more I/O savings by optimizing for its most common values.
 * Also, for similarly-sized relations, the planner prefers to put the more
 * uniformly distributed relation on the inside, so we're more likely to find
 * interesting skew in the outer relation.
 */
typedef struct HashSkewBucket {
  uint32 hashvalue;     /* common hash value */
  HashJoinTuple tuples; /* linked list of inner-relation tuples */
} HashSkewBucket;

#define SKEW_BUCKET_OVERHEAD  MAXALIGN(sizeof(HashSkewBucket))
#define INVALID_SKEW_BUCKET_NO	(-1)
#define SKEW_HASH_MEM_PERCENT  2
#define SKEW_MIN_OUTER_FRACTION  0.01

/*
 * To reduce palloc overhead, the HashJoinTuples for the current batch are
 * packed in 32kB buffers instead of pallocing each tuple individually.
 */
typedef struct HashMemoryChunkData {
  int ntuples;   /* number of tuples stored in this chunk */
  size_t maxlen; /* size of the chunk's tuple buffer */
  size_t used;   /* number of buffer bytes already used */

  /* pointer to the next chunk (linked list) */
  struct HashMemoryChunkData *next;

  /*
   * The chunk's tuple buffer starts after the HashMemoryChunkData struct,
   * at offset HASH_CHUNK_HEADER_SIZE (which must be maxaligned).  Note that
   * that offset is not included in "maxlen" or "used".
   */
} HashMemoryChunkData;

typedef struct HashMemoryChunkData *HashMemoryChunk;

#define HASH_CHUNK_SIZE			((Size) (32 * 1024))
#define HASH_CHUNK_HEADER_SIZE	MAXALIGN(sizeof(HashMemoryChunkData))
#define HASH_CHUNK_DATA(hc)		(((char *) (hc)) + HASH_CHUNK_HEADER_SIZE)
/* tuples exceeding HASH_CHUNK_THRESHOLD bytes are put in their own chunk */
#define HASH_CHUNK_THRESHOLD	(HASH_CHUNK_SIZE / 4)

typedef struct HashJoinTableData {
  int nbuckets;      /* # buckets in the in-memory hash table */
  int log2_nbuckets; /* its log2 (nbuckets must be a power of 2) */

  int nbuckets_original;     /* # buckets when starting the first hash */
  int nbuckets_optimal;      /* optimal # buckets (per batch) */
  int log2_nbuckets_optimal; /* log2(nbuckets_optimal) */

  /* buckets[i] is head of list of tuples in i'th in-memory bucket */
  struct HashJoinTupleData **buckets;

  bool skewEnabled;            /* are we using skew optimization? */
  HashSkewBucket **skewBucket; /* hashtable of skew buckets */
  int skewBucketLen;           /* size of skewBucket array (a power of 2!) */
  int nSkewBuckets;            /* number of active skew buckets */
  int *skewBucketNums;         /* array indexes of active skew buckets */

  int nbatch;   /* number of batches */
  int curbatch; /* current batch #; 0 during 1st pass */

  int nbatch_original; /* nbatch when we started inner scan */
  int nbatch_outstart; /* nbatch when we started outer scan */

  bool growEnabled; /* flag to shut off nbatch increases */

  double totalTuples; /* # tuples obtained from inner plan */
  double skewTuples;  /* # tuples inserted into skew tuples */

  /*
   * These arrays are allocated for the life of the hash join, but only if
   * nbatch > 1.  A file is opened only when we first write a tuple into it
   * (otherwise its pointer remains NULL).  Note that the zero'th array
   * elements never get used, since we will process rather than dump out any
   * tuples of batch zero.
   */
  BufFile **innerBatchFile; /* buffered virtual temp file per batch */
  BufFile **outerBatchFile; /* buffered virtual temp file per batch */

  Size spaceUsed;        /* memory space currently used by tuples */
  Size spaceAllowed;     /* upper limit for space used */
  Size spacePeak;        /* peak space used */
  Size spaceUsedSkew;    /* skew hash table's current space usage */
  Size spaceAllowedSkew; /* upper limit for skew hashtable */

  MemoryContext hashCxt;  /* context for whole-hash-join storage */
  MemoryContext batchCxt; /* context for this-batch-only storage */
  MemoryContext spillCxt; /* context for spilling to temp files */

  /* used for dense allocation of tuples (into linked chunks) */
  HashMemoryChunk chunks; /* one list for the whole batch */
} HashJoinTableData;

#endif /* HASHJOIN_H */
