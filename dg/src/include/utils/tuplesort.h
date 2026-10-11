/*-------------------------------------------------------------------------
 *
 * tuplesort.h
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().  Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.  Parallel sorts use a variant of this external sort
 * algorithm, and are typically only used for large amounts of data.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/tuplesort.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESORT_H
#define TUPLESORT_H

#include "access/itup.h"
#include "executor/tuptable.h"
#include "utils/logtape.h"
#include "utils/relcache.h"
#include "utils/sortsupport.h"

/*
 * Tuplesortstate is an opaque type whose details are not known outside
 * tuplesort.c.
 */
typedef struct Tuplesortstate Tuplesortstate;

/*
 * Data structures for reporting sort statistics.  Note that
 * TuplesortInstrumentation can't contain any pointers because we
 * sometimes put it in shared memory.
 *
 * The parallel-sort infrastructure relies on having a zero TuplesortMethod
 * to indicate that a worker never did anything, so we assign zero to
 * SORT_TYPE_STILL_IN_PROGRESS.  The other values of this enum can be
 * OR'ed together to represent a situation where different workers used
 * different methods, so we need a separate bit for each one.  Keep the
 * NUM_TUPLESORTMETHODS constant in sync with the number of bits!
 */
typedef enum {
  SORT_TYPE_STILL_IN_PROGRESS = 0,
  SORT_TYPE_TOP_N_HEAPSORT = 1 << 0,
  SORT_TYPE_QUICKSORT = 1 << 1,
  SORT_TYPE_EXTERNAL_SORT = 1 << 2,
  SORT_TYPE_EXTERNAL_MERGE = 1 << 3,
} TuplesortMethod;

#define NUM_TUPLESORTMETHODS 4

typedef enum {
  SORT_SPACE_TYPE_DISK,
  SORT_SPACE_TYPE_MEMORY,
} TuplesortSpaceType;

/* Bitwise option flags for tuple sorts */
#define TUPLESORT_NONE					0

/* specifies whether non-sequential access to the sort result is required */
#define TUPLESORT_RANDOMACCESS			(1 << 0)

/* specifies if the tuplesort is able to support bounded sorts */
#define TUPLESORT_ALLOWBOUNDED			(1 << 1)

/*
 * For bounded sort, tuples get pfree'd when they fall outside of the bound.
 * When bounded sorts are not required, we can use a bump context for tuple
 * allocation as there's no risk that pfree will ever be called for a tuple.
 * Define a macro to make it easier for code to figure out if we're using a
 * bump allocator.
 */
#define TupleSortUseBumpTupleCxt(opt) (((opt) & TUPLESORT_ALLOWBOUNDED) == 0)

typedef struct TuplesortInstrumentation {
  TuplesortMethod sortMethod;   /* sort algorithm used */
  TuplesortSpaceType spaceType; /* type of space spaceUsed represents */
  int64 spaceUsed;              /* space consumption, in kB */
} TuplesortInstrumentation;

/*
 * The objects we actually sort are SortTuple structs.  These contain
 * a pointer to the tuple proper (might be a MinimalTuple or IndexTuple),
 * which is a separate palloc chunk --- we assume it is just one chunk and
 * can be freed by a simple pfree() (except during merge, where we use a
 * simple slab allocator, and during a non-bounded sort where we use a bump
 * allocator).  SortTuples also contain the tuple's first key column in
 * Datum/nullflag format, and a source/input tape number that tracks which
 * tape each heap element/slot belongs to during merging.
 *
 * Storing the first key column lets us save heap_getattr or index_getattr
 * calls during tuple comparisons.  We could extract and save all the key
 * columns not just the first, but this would increase code complexity and
 * overhead, and wouldn't actually save any comparison cycles in the common
 * case where the first key determines the comparison result.  Note that
 * for a pass-by-reference datatype, datum1 points into the "tuple" storage.
 *
 * There is one special case: when the sort support infrastructure provides an
 * "abbreviated key" representation, where the key is (typically) a pass by
 * value proxy for a pass by reference type.  In this case, the abbreviated key
 * is stored in datum1 in place of the actual first key column.
 *
 * When sorting single Datums, the data value is represented directly by
 * datum1/isnull1 for pass by value types (or null values).  If the datatype is
 * pass-by-reference and isnull1 is false, then "tuple" points to a separately
 * palloc'd data value, otherwise "tuple" is NULL.  The value of datum1 is then
 * either the same pointer as "tuple", or is an abbreviated key value as
 * described above.  Accordingly, "tuple" is always used in preference to
 * datum1 as the authoritative value for pass-by-reference cases.
 */
typedef struct {
  void *tuple;  /* the tuple itself */
  Datum datum1; /* value of first key column */
  bool isnull1; /* is first key column NULL? */
  int srctape;  /* source tape number */
} SortTuple;

typedef int (*SortTupleComparator)(const SortTuple *a, const SortTuple *b,
                                   Tuplesortstate *state);

/*
 * The public part of a Tuple sort operation state.  This data structure
 * contains the definition of sort-variant-specific interface methods and
 * the part of Tuple sort operation state required by their implementations.
 */
typedef struct {
  /*
   * These function pointers decouple the routines that must know what kind
   * of tuple we are sorting from the routines that don't need to know it.
   * They are set up by the tuplesort_begin_xxx routines.
   *
   * Function to compare two tuples; result is per qsort() convention, ie:
   * <0, 0, >0 according as a<b, a=b, a>b.  The API must match
   * qsort_arg_comparator.
   */
  SortTupleComparator comparetup;

  /*
   * Fall back to the full tuple for comparison, but only compare the first
   * sortkey if it was abbreviated. Otherwise, only compare second and later
   * sortkeys.
   */
  SortTupleComparator comparetup_tiebreak;

  /*
   * Alter datum1 representation in the SortTuple's array back from the
   * abbreviated key to the first column value.
   */
  void (*removeabbrev)(Tuplesortstate *state, SortTuple *stups, int count);

  /*
   * Function to write a stored tuple onto tape.  The representation of the
   * tuple on tape need not be the same as it is in memory.
   */
  void (*writetup)(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup);

  /*
   * Function to read a stored tuple from tape back into memory. 'len' is
   * the already-read length of the stored tuple.  The tuple is allocated
   * from the slab memory arena, or is palloc'd, see
   * tuplesort_readtup_alloc().
   */
  void (*readtup)(Tuplesortstate *state, SortTuple *stup, LogicalTape *tape,
                  unsigned int len);

  /*
   * Function to do some specific release of resources for the sort variant.
   * In particular, this function should free everything stored in the "arg"
   * field, which wouldn't be cleared on reset of the Tuple sort memory
   * contexts.  This can be NULL if nothing specific needs to be done.
   */
  void (*freestate)(Tuplesortstate *state);

  /*
   * The subsequent fields are used in the implementations of the functions
   * above.
   */
  MemoryContext maincontext;  /* memory context for tuple sort metadata that
                               * persists across multiple batches */
  MemoryContext sortcontext;  /* memory context holding most sort data */
  MemoryContext tuplecontext; /* sub-context of sortcontext for tuple data */

  /*
   * Whether SortTuple's datum1 and isnull1 members are maintained by the
   * above routines.  If not, some sort specializations are disabled.
   */
  bool haveDatum1;

  /*
   * The sortKeys variable is used by every case other than the hash index
   * case; it is set by tuplesort_begin_xxx.  tupDesc is only used by the
   * MinimalTuple and CLUSTER routines, though.
   */
  int nKeys;            /* number of columns in sort key */
  SortSupport sortKeys; /* array of length nKeys */

  /*
   * This variable is shared by the single-key MinimalTuple case and the
   * Datum case (which both use qsort_ssup()).  Otherwise, it's NULL.  The
   * presence of a value in this field is also checked by various sort
   * specialization functions as an optimization when comparing the leading
   * key in a tiebreak situation to determine if there are any subsequent
   * keys to sort on.
   */
  SortSupport onlyKey;

  int sortopt; /* Bitmask of flags used to setup sort */

  bool tuples; /* Can SortTuple.tuple ever be set? */

  void *arg; /* Specific information for the sort variant */
} TuplesortPublic;

#define TuplesortstateGetPublic(state) ((TuplesortPublic *) state)

/* When using this macro, beware of double evaluation of len */
#define LogicalTapeReadExact(tape, ptr, len) \
	do { \
		if (LogicalTapeRead(tape, ptr, len) != (size_t) (len)) \
			elog(ERROR, "unexpected end of data"); \
	} while(0)

/*
 * We provide multiple interfaces to what is essentially the same code,
 * since different callers have different data to be sorted and want to
 * specify the sort key information differently.  There are two APIs for
 * sorting HeapTuples and two more for sorting IndexTuples.  Yet another
 * API supports sorting bare Datums.
 *
 * The "heap" API actually stores/sorts MinimalTuples, which means it doesn't
 * preserve the system columns (tuple identity and transaction visibility
 * info).  The sort keys are specified by column numbers within the tuples
 * and sort operator OIDs.  We save some cycles by passing and returning the
 * tuples in TupleTableSlots, rather than forming actual HeapTuples (which'd
 * have to be converted to MinimalTuples).  This API works well for sorts
 * executed as parts of plan trees.
 *
 * The "cluster" API stores/sorts full HeapTuples including all visibility
 * info. The sort keys are specified by reference to a btree index that is
 * defined on the relation to be sorted.  Note that putheaptuple/getheaptuple
 * go with this API, not the "begin_heap" one!
 *
 * The "index_btree" API stores/sorts IndexTuples (preserving all their
 * header fields).  The sort keys are specified by a btree index definition.
 *
 * The "index_hash" API is similar to index_btree, but the tuples are
 * actually sorted by their hash codes not the raw data.
 *
 * The "index_brin" API is similar to index_btree, but the tuples are
 * BrinTuple and are sorted by their block number not the raw data.
 */

extern Tuplesortstate *tuplesort_begin_common(int workMem, int sortopt);
extern void tuplesort_set_bound(Tuplesortstate *state, int64 bound);
extern bool tuplesort_used_bound(Tuplesortstate *state);
extern void tuplesort_puttuple_common(Tuplesortstate *state, SortTuple *tuple,
                                      bool useAbbrev, Size tuplen);
extern void tuplesort_performsort(Tuplesortstate *state);
extern bool tuplesort_gettuple_common(Tuplesortstate *state, bool forward,
                                      SortTuple *stup);
extern bool tuplesort_skiptuples(Tuplesortstate *state, int64 ntuples,
                                 bool forward);
extern void tuplesort_end(Tuplesortstate *state);
extern void tuplesort_reset(Tuplesortstate *state);

extern void tuplesort_get_stats(Tuplesortstate *state,
                                TuplesortInstrumentation *stats);
extern const char *tuplesort_method_name(TuplesortMethod m);
extern const char *tuplesort_space_type_name(TuplesortSpaceType t);

extern int tuplesort_merge_order(int64 allowedMem);

/*
 * These routines may only be called if TUPLESORT_RANDOMACCESS was specified
 * during tuplesort_begin_*.  Additionally backwards scan in gettuple/getdatum
 * also require TUPLESORT_RANDOMACCESS.
 */
extern void tuplesort_rescan(Tuplesortstate *state);
extern void tuplesort_markpos(Tuplesortstate *state);
extern void tuplesort_restorepos(Tuplesortstate *state);

extern void *tuplesort_readtup_alloc(Tuplesortstate *state, Size tuplen);

/* tuplesortvariants.c */

extern Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc, int nkeys, AttrNumber *attNums,
                     Oid *sortOperators, Oid *sortCollations,
                     bool *nullsFirstFlags, int workMem, int sortopt);
extern Tuplesortstate *tuplesort_begin_datum(Oid datumType, Oid sortOperator,
                                             Oid sortCollation,
                                             bool nullsFirstFlag, int workMem,
                                             int sortopt);

extern void tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot);
extern void tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull);

extern bool tuplesort_gettupleslot(Tuplesortstate *state, bool forward,
                                   bool copy, TupleTableSlot *slot,
                                   Datum *abbrev);
extern bool tuplesort_getdatum(Tuplesortstate *state, bool forward, bool copy,
                               Datum *val, bool *isNull, Datum *abbrev);

#endif /* TUPLESORT_H */
