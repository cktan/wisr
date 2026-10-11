/*-------------------------------------------------------------------------
 *
 * tuplesortvariants.c
 *	  Implementation of tuple sorting variants.
 *
 * This module handles the sorting of heap tuples, index tuples, or single
 * Datums.  The implementation is based on the generalized tuple sorting
 * facility given in tuplesort.c.  Support other kinds of sortable objects
 * could be easily added here, another module, or even an extension.
 *
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesortvariants.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "executor/executor.h"
#include "pg_trace.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/tuplesort.h"

/* sort-type codes for sort__start probes */
#define HEAP_SORT		0
#define DATUM_SORT		2

static void removeabbrev_heap(Tuplesortstate *state, SortTuple *stups,
                              int count);
static void removeabbrev_datum(Tuplesortstate *state, SortTuple *stups,
                               int count);
static int comparetup_heap(const SortTuple *a, const SortTuple *b,
                           Tuplesortstate *state);
static int comparetup_heap_tiebreak(const SortTuple *a, const SortTuple *b,
                                    Tuplesortstate *state);
static void writetup_heap(Tuplesortstate *state, LogicalTape *tape,
                          SortTuple *stup);
static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
                         LogicalTape *tape, unsigned int len);
static int comparetup_datum(const SortTuple *a, const SortTuple *b,
                            Tuplesortstate *state);
static int comparetup_datum_tiebreak(const SortTuple *a, const SortTuple *b,
                                     Tuplesortstate *state);
static void writetup_datum(Tuplesortstate *state, LogicalTape *tape,
                           SortTuple *stup);
static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
                          LogicalTape *tape, unsigned int len);

/*
 * Data structure pointed by "TuplesortPublic.arg" for the Datum case.
 * Set by tuplesort_begin_datum and used only by the DatumTuple routines.
 */
typedef struct {
  /* the datatype oid of Datum's to be sorted */
  Oid datumType;
  /* we need typelen in order to know how to copy the Datums. */
  int datumTypeLen;
} TuplesortDatumArg;

Tuplesortstate *tuplesort_begin_heap(TupleDesc tupDesc, int nkeys,
                                     AttrNumber *attNums, Oid *sortOperators,
                                     Oid *sortCollations, bool *nullsFirstFlags,
                                     int workMem, int sortopt) {
  Tuplesortstate *state = tuplesort_begin_common(workMem, sortopt);
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MemoryContext oldcontext;
  int i;

  oldcontext = MemoryContextSwitchTo(base->maincontext);

  Assert(nkeys > 0);

  if (trace_sort)
    elog(LOG, "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
         nkeys, workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');

  base->nKeys = nkeys;

  TRACE_POSTGRESQL_SORT_START(HEAP_SORT, false, /* no unique check */
                              nkeys, workMem, sortopt & TUPLESORT_RANDOMACCESS,
                              0 /* not parallel */);

  base->removeabbrev = removeabbrev_heap;
  base->comparetup = comparetup_heap;
  base->comparetup_tiebreak = comparetup_heap_tiebreak;
  base->writetup = writetup_heap;
  base->readtup = readtup_heap;
  base->haveDatum1 = true;
  base->arg = tupDesc; /* assume we need not copy tupDesc */

  /* Prepare SortSupport data for each column */
  base->sortKeys = (SortSupport)palloc0(nkeys * sizeof(SortSupportData));

  for (i = 0; i < nkeys; i++) {
    SortSupport sortKey = base->sortKeys + i;

    Assert(attNums[i] != 0);
    Assert(sortOperators[i] != 0);

    sortKey->ssup_cxt = CurrentMemoryContext;
    sortKey->ssup_collation = sortCollations[i];
    sortKey->ssup_nulls_first = nullsFirstFlags[i];
    sortKey->ssup_attno = attNums[i];
    /* Convey if abbreviation optimization is applicable in principle */
    sortKey->abbreviate = (i == 0 && base->haveDatum1);

    PrepareSortSupportFromOrderingOp(sortOperators[i], sortKey);
  }

  /*
   * The "onlyKey" optimization cannot be used with abbreviated keys, since
   * tie-breaker comparisons may be required.  Typically, the optimization
   * is only of value to pass-by-value types anyway, whereas abbreviated
   * keys are typically only of value to pass-by-reference types.
   */
  if (nkeys == 1 && !base->sortKeys->abbrev_converter)
    base->onlyKey = base->sortKeys;

  MemoryContextSwitchTo(oldcontext);

  return state;
}

Tuplesortstate *tuplesort_begin_datum(Oid datumType, Oid sortOperator,
                                      Oid sortCollation, bool nullsFirstFlag,
                                      int workMem, int sortopt) {
  Tuplesortstate *state = tuplesort_begin_common(workMem, sortopt);
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  TuplesortDatumArg *arg;
  MemoryContext oldcontext;
  int16 typlen;
  bool typbyval;

  oldcontext = MemoryContextSwitchTo(base->maincontext);
  arg = (TuplesortDatumArg *)palloc(sizeof(TuplesortDatumArg));

  if (trace_sort)
    elog(LOG, "begin datum sort: workMem = %d, randomAccess = %c", workMem,
         sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');

  base->nKeys = 1; /* always a one-column sort */

  TRACE_POSTGRESQL_SORT_START(DATUM_SORT, false, /* no unique check */
                              1, workMem, sortopt & TUPLESORT_RANDOMACCESS,
                              0 /* not parallel */);

  base->removeabbrev = removeabbrev_datum;
  base->comparetup = comparetup_datum;
  base->comparetup_tiebreak = comparetup_datum_tiebreak;
  base->writetup = writetup_datum;
  base->readtup = readtup_datum;
  base->haveDatum1 = true;
  base->arg = arg;

  arg->datumType = datumType;

  /* lookup necessary attributes of the datum type */
  get_typlenbyval(datumType, &typlen, &typbyval);
  arg->datumTypeLen = typlen;
  base->tuples = !typbyval;

  /* Prepare SortSupport data */
  base->sortKeys = (SortSupport)palloc0(sizeof(SortSupportData));

  base->sortKeys->ssup_cxt = CurrentMemoryContext;
  base->sortKeys->ssup_collation = sortCollation;
  base->sortKeys->ssup_nulls_first = nullsFirstFlag;

  /*
   * Abbreviation is possible here only for by-reference types.  In theory,
   * a pass-by-value datatype could have an abbreviated form that is cheaper
   * to compare.  In a tuple sort, we could support that, because we can
   * always extract the original datum from the tuple as needed.  Here, we
   * can't, because a datum sort only stores a single copy of the datum; the
   * "tuple" field of each SortTuple is NULL.
   */
  base->sortKeys->abbreviate = !typbyval;

  PrepareSortSupportFromOrderingOp(sortOperator, base->sortKeys);

  /*
   * The "onlyKey" optimization cannot be used with abbreviated keys, since
   * tie-breaker comparisons may be required.  Typically, the optimization
   * is only of value to pass-by-value types anyway, whereas abbreviated
   * keys are typically only of value to pass-by-reference types.
   */
  if (!base->sortKeys->abbrev_converter)
    base->onlyKey = base->sortKeys;

  MemoryContextSwitchTo(oldcontext);

  return state;
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MemoryContext oldcontext = MemoryContextSwitchTo(base->tuplecontext);
  TupleDesc tupDesc = (TupleDesc)base->arg;
  SortTuple stup;
  MinimalTuple tuple;
  HeapTupleData htup;
  Size tuplen;

  /* copy the tuple into sort storage */
  tuple = ExecCopySlotMinimalTuple(slot);
  stup.tuple = tuple;
  /* set up first-column key value */
  htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
  htup.t_data = (HeapTupleHeader)((char *)tuple - MINIMAL_TUPLE_OFFSET);
  stup.datum1 =
      heap_getattr(&htup, base->sortKeys[0].ssup_attno, tupDesc, &stup.isnull1);

  /* GetMemoryChunkSpace is not supported for bump contexts */
  if (TupleSortUseBumpTupleCxt(base->sortopt))
    tuplen = MAXALIGN(tuple->t_len);
  else
    tuplen = GetMemoryChunkSpace(tuple);

  tuplesort_puttuple_common(
      state, &stup, base->sortKeys->abbrev_converter && !stup.isnull1, tuplen);

  MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
void tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MemoryContext oldcontext = MemoryContextSwitchTo(base->tuplecontext);
  TuplesortDatumArg *arg = (TuplesortDatumArg *)base->arg;
  SortTuple stup;

  /*
   * Pass-by-value types or null values are just stored directly in
   * stup.datum1 (and stup.tuple is not used and set to NULL).
   *
   * Non-null pass-by-reference values need to be copied into memory we
   * control, and possibly abbreviated. The copied value is pointed to by
   * stup.tuple and is treated as the canonical copy (e.g. to return via
   * tuplesort_getdatum or when writing to tape); stup.datum1 gets the
   * abbreviated value if abbreviation is happening, otherwise it's
   * identical to stup.tuple.
   */

  if (isNull || !base->tuples) {
    /*
     * Set datum1 to zeroed representation for NULLs (to be consistent,
     * and to support cheap inequality tests for NULL abbreviated keys).
     */
    stup.datum1 = !isNull ? val : (Datum)0;
    stup.isnull1 = isNull;
    stup.tuple = NULL; /* no separate storage */
  } else {
    stup.isnull1 = false;
    stup.datum1 = datumCopy(val, false, arg->datumTypeLen);
    stup.tuple = DatumGetPointer(stup.datum1);
  }

  tuplesort_puttuple_common(
      state, &stup, base->tuples && base->sortKeys->abbrev_converter && !isNull,
      0);

  MemoryContextSwitchTo(oldcontext);
}

/*
 * Fetch the next tuple in either forward or back direction.
 * If successful, put tuple in slot and return true; else, clear the slot
 * and return false.
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value in leading attribute will set abbreviated value to zeroed
 * representation, which caller may rely on in abbreviated inequality check.
 *
 * If copy is true, the slot receives a tuple that's been copied into the
 * caller's memory context, so that it will stay valid regardless of future
 * manipulations of the tuplesort's state (up to and including deleting the
 * tuplesort).  If copy is false, the slot will just receive a pointer to a
 * tuple held within the tuplesort, which is more efficient, but only safe for
 * callers that are prepared to have any subsequent manipulation of the
 * tuplesort's state invalidate slot contents.
 */
bool tuplesort_gettupleslot(Tuplesortstate *state, bool forward, bool copy,
                            TupleTableSlot *slot, Datum *abbrev) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
  SortTuple stup;

  if (!tuplesort_gettuple_common(state, forward, &stup))
    stup.tuple = NULL;

  MemoryContextSwitchTo(oldcontext);

  if (stup.tuple) {
    /* Record abbreviated key for caller */
    if (base->sortKeys->abbrev_converter && abbrev)
      *abbrev = stup.datum1;

    if (copy)
      stup.tuple = heap_copy_minimal_tuple((MinimalTuple)stup.tuple, 0);

    ExecStoreMinimalTuple((MinimalTuple)stup.tuple, slot, copy);
    return true;
  } else {
    ExecClearTuple(slot);
    return false;
  }
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns false if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * in caller's context, and is now owned by the caller (this differs from
 * similar routines for other types of tuplesorts).
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value will have a zeroed abbreviated value representation, which caller
 * may rely on in abbreviated inequality check.
 *
 * For byref Datums, if copy is true, *val is set to a copy of the Datum
 * copied into the caller's memory context, so that it will stay valid
 * regardless of future manipulations of the tuplesort's state (up to and
 * including deleting the tuplesort).  If copy is false, *val will just be
 * set to a pointer to the Datum held within the tuplesort, which is more
 * efficient, but only safe for callers that are prepared to have any
 * subsequent manipulation of the tuplesort's state invalidate slot contents.
 * For byval Datums, the value of the 'copy' parameter has no effect.

 */
bool tuplesort_getdatum(Tuplesortstate *state, bool forward, bool copy,
                        Datum *val, bool *isNull, Datum *abbrev) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MemoryContext oldcontext = MemoryContextSwitchTo(base->sortcontext);
  TuplesortDatumArg *arg = (TuplesortDatumArg *)base->arg;
  SortTuple stup;

  if (!tuplesort_gettuple_common(state, forward, &stup)) {
    MemoryContextSwitchTo(oldcontext);
    return false;
  }

  /* Ensure we copy into caller's memory context */
  MemoryContextSwitchTo(oldcontext);

  /* Record abbreviated key for caller */
  if (base->sortKeys->abbrev_converter && abbrev)
    *abbrev = stup.datum1;

  if (stup.isnull1 || !base->tuples) {
    *val = stup.datum1;
    *isNull = stup.isnull1;
  } else {
    /* use stup.tuple because stup.datum1 may be an abbreviation */
    if (copy)
      *val = datumCopy(PointerGetDatum(stup.tuple), false, arg->datumTypeLen);
    else
      *val = PointerGetDatum(stup.tuple);
    *isNull = false;
  }

  return true;
}

/*
 * Routines specialized for HeapTuple (actually MinimalTuple) case
 */

static void removeabbrev_heap(Tuplesortstate *state, SortTuple *stups,
                              int count) {
  int i;
  TuplesortPublic *base = TuplesortstateGetPublic(state);

  for (i = 0; i < count; i++) {
    HeapTupleData htup;

    htup.t_len = ((MinimalTuple)stups[i].tuple)->t_len + MINIMAL_TUPLE_OFFSET;
    htup.t_data =
        (HeapTupleHeader)((char *)stups[i].tuple - MINIMAL_TUPLE_OFFSET);
    stups[i].datum1 = heap_getattr(&htup, base->sortKeys[0].ssup_attno,
                                   (TupleDesc)base->arg, &stups[i].isnull1);
  }
}

static int comparetup_heap(const SortTuple *a, const SortTuple *b,
                           Tuplesortstate *state) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  SortSupport sortKey = base->sortKeys;
  int32 compare;

  /* Compare the leading sort key */
  compare = ApplySortComparator(a->datum1, a->isnull1, b->datum1, b->isnull1,
                                sortKey);
  if (compare != 0)
    return compare;

  /* Compare additional sort keys */
  return comparetup_heap_tiebreak(a, b, state);
}

static int comparetup_heap_tiebreak(const SortTuple *a, const SortTuple *b,
                                    Tuplesortstate *state) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  SortSupport sortKey = base->sortKeys;
  HeapTupleData ltup;
  HeapTupleData rtup;
  TupleDesc tupDesc;
  int nkey;
  int32 compare;
  AttrNumber attno;
  Datum datum1, datum2;
  bool isnull1, isnull2;

  ltup.t_len = ((MinimalTuple)a->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
  ltup.t_data = (HeapTupleHeader)((char *)a->tuple - MINIMAL_TUPLE_OFFSET);
  rtup.t_len = ((MinimalTuple)b->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
  rtup.t_data = (HeapTupleHeader)((char *)b->tuple - MINIMAL_TUPLE_OFFSET);
  tupDesc = (TupleDesc)base->arg;

  if (sortKey->abbrev_converter) {
    attno = sortKey->ssup_attno;

    datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
    datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

    compare = ApplySortAbbrevFullComparator(datum1, isnull1, datum2, isnull2,
                                            sortKey);
    if (compare != 0)
      return compare;
  }

  sortKey++;
  for (nkey = 1; nkey < base->nKeys; nkey++, sortKey++) {
    attno = sortKey->ssup_attno;

    datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
    datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

    compare = ApplySortComparator(datum1, isnull1, datum2, isnull2, sortKey);
    if (compare != 0)
      return compare;
  }

  return 0;
}

static void writetup_heap(Tuplesortstate *state, LogicalTape *tape,
                          SortTuple *stup) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  MinimalTuple tuple = (MinimalTuple)stup->tuple;

  /* the part of the MinimalTuple we'll write: */
  char *tupbody = (char *)tuple + MINIMAL_TUPLE_DATA_OFFSET;
  unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

  /* total on-disk footprint: */
  unsigned int tuplen = tupbodylen + sizeof(int);

  LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
  LogicalTapeWrite(tape, tupbody, tupbodylen);
  if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
    LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
                         LogicalTape *tape, unsigned int len) {
  unsigned int tupbodylen = len - sizeof(int);
  unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
  MinimalTuple tuple = (MinimalTuple)tuplesort_readtup_alloc(state, tuplen);
  char *tupbody = (char *)tuple + MINIMAL_TUPLE_DATA_OFFSET;
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  HeapTupleData htup;

  /* read in the tuple proper */
  tuple->t_len = tuplen;
  LogicalTapeReadExact(tape, tupbody, tupbodylen);
  if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
    LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
  stup->tuple = tuple;
  /* set up first-column key value */
  htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
  htup.t_data = (HeapTupleHeader)((char *)tuple - MINIMAL_TUPLE_OFFSET);
  stup->datum1 = heap_getattr(&htup, base->sortKeys[0].ssup_attno,
                              (TupleDesc)base->arg, &stup->isnull1);
}

/*
 * Routines specialized for the CLUSTER case (HeapTuple data, with
 * comparisons per a btree index definition)
 */

/*
 * Routines specialized for IndexTuple case
 *
 * The btree and hash cases require separate comparison functions, but the
 * IndexTuple representation is the same so the copy/write/read support
 * functions can be shared.
 */

/*
 * Routines specialized for BrinTuple case
 */

/*
 * Routines specialized for GIN case
 */

/*
 * Routines specialized for DatumTuple case
 */

static void removeabbrev_datum(Tuplesortstate *state, SortTuple *stups,
                               int count) {
  int i;

  for (i = 0; i < count; i++)
    stups[i].datum1 = PointerGetDatum(stups[i].tuple);
}

static int comparetup_datum(const SortTuple *a, const SortTuple *b,
                            Tuplesortstate *state) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  int compare;

  compare = ApplySortComparator(a->datum1, a->isnull1, b->datum1, b->isnull1,
                                base->sortKeys);
  if (compare != 0)
    return compare;

  return comparetup_datum_tiebreak(a, b, state);
}

static int comparetup_datum_tiebreak(const SortTuple *a, const SortTuple *b,
                                     Tuplesortstate *state) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  int32 compare = 0;

  /* if we have abbreviations, then "tuple" has the original value */
  if (base->sortKeys->abbrev_converter)
    compare = ApplySortAbbrevFullComparator(
        PointerGetDatum(a->tuple), a->isnull1, PointerGetDatum(b->tuple),
        b->isnull1, base->sortKeys);

  return compare;
}

static void writetup_datum(Tuplesortstate *state, LogicalTape *tape,
                           SortTuple *stup) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  TuplesortDatumArg *arg = (TuplesortDatumArg *)base->arg;
  void *waddr;
  unsigned int tuplen;
  unsigned int writtenlen;

  if (stup->isnull1) {
    waddr = NULL;
    tuplen = 0;
  } else if (!base->tuples) {
    waddr = &stup->datum1;
    tuplen = sizeof(Datum);
  } else {
    waddr = stup->tuple;
    tuplen =
        datumGetSize(PointerGetDatum(stup->tuple), false, arg->datumTypeLen);
    Assert(tuplen != 0);
  }

  writtenlen = tuplen + sizeof(unsigned int);

  LogicalTapeWrite(tape, &writtenlen, sizeof(writtenlen));
  LogicalTapeWrite(tape, waddr, tuplen);
  if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
    LogicalTapeWrite(tape, &writtenlen, sizeof(writtenlen));
}

static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
                          LogicalTape *tape, unsigned int len) {
  TuplesortPublic *base = TuplesortstateGetPublic(state);
  unsigned int tuplen = len - sizeof(unsigned int);

  if (tuplen == 0) {
    /* it's NULL */
    stup->datum1 = (Datum)0;
    stup->isnull1 = true;
    stup->tuple = NULL;
  } else if (!base->tuples) {
    Assert(tuplen == sizeof(Datum));
    LogicalTapeReadExact(tape, &stup->datum1, tuplen);
    stup->isnull1 = false;
    stup->tuple = NULL;
  } else {
    void *raddr = tuplesort_readtup_alloc(state, tuplen);

    LogicalTapeReadExact(tape, raddr, tuplen);
    stup->datum1 = PointerGetDatum(raddr);
    stup->isnull1 = false;
    stup->tuple = raddr;
  }

  if (base->sortopt & TUPLESORT_RANDOMACCESS) /* need trailing length word? */
    LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
}
