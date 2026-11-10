/*-------------------------------------------------------------------------
 *
 * mvdistinct.c
 *	  POSTGRES multivariate ndistinct coefficients
 *
 * Estimating number of groups in a combination of columns (e.g. for GROUP BY)
 * is tricky, and the estimation error is often significant.

 * The multivariate ndistinct coefficients address this by storing ndistinct
 * estimates for combinations of the user-specified columns.  So for example
 * given a statistics object on three columns (a,b,c), this module estimates
 * and stores n-distinct for (a,b), (a,c), (b,c) and (a,b,c).  The per-column
 * estimates are already available in pg_statistic.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mvdistinct.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "lib/stringinfo.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/fmgrprotos.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

/* size of the struct header fields (magic, type, nitems) */
#define SizeOfHeader		(3 * sizeof(uint32))

/* size of a serialized ndistinct item (coefficient, natts, atts) */
#define SizeOfItem(natts) \
	(sizeof(double) + sizeof(int) + (natts) * sizeof(AttrNumber))

/* minimal size of a ndistinct item (with two attributes) */
#define MinSizeOfItem	SizeOfItem(2)

/* minimal size of mvndistinct, when all items are minimal */
#define MinSizeOfItems(nitems)	\
	(SizeOfHeader + (nitems) * MinSizeOfItem)

/* Combination generator API */

/* internal state for generator of k-combinations of n elements */
typedef struct CombinationGenerator {
  int k;             /* size of the combination */
  int n;             /* total number of elements */
  int current;       /* index of the next combination to return */
  int ncombinations; /* number of combinations (size of array) */
  int *combinations; /* array of pre-built combinations */
} CombinationGenerator;

/*
 * statext_ndistinct_load
 *		Load the ndistinct value for the indicated pg_statistic_ext
 *tuple
 */
MVNDistinct *statext_ndistinct_load(Oid mvoid, bool inh) {
  MVNDistinct *result;
  bool isnull;
  Datum ndist;
  HeapTuple htup;

  htup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(mvoid),
                         BoolGetDatum(inh));
  if (!HeapTupleIsValid(htup))
    elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

  ndist = SysCacheGetAttr(STATEXTDATASTXOID, htup,
                          Anum_pg_statistic_ext_data_stxdndistinct, &isnull);
  if (isnull)
    elog(ERROR,
         "requested statistics kind \"%c\" is not yet built for statistics "
         "object %u",
         STATS_EXT_NDISTINCT, mvoid);

  result = statext_ndistinct_deserialize(DatumGetByteaPP(ndist));

  ReleaseSysCache(htup);

  return result;
}

/*
 * statext_ndistinct_deserialize
 *		Read an on-disk bytea format MVNDistinct to in-memory format
 */
MVNDistinct *statext_ndistinct_deserialize(bytea *data) {
  int i;
  Size minimum_size;
  MVNDistinct ndist;
  MVNDistinct *ndistinct;
  char *tmp;

  if (data == NULL)
    return NULL;

  /* we expect at least the basic fields of MVNDistinct struct */
  if (VARSIZE_ANY_EXHDR(data) < SizeOfHeader)
    elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
         VARSIZE_ANY_EXHDR(data), SizeOfHeader);

  /* initialize pointer to the data part (skip the varlena header) */
  tmp = VARDATA_ANY(data);

  /* read the header fields and perform basic sanity checks */
  memcpy(&ndist.magic, tmp, sizeof(uint32));
  tmp += sizeof(uint32);
  memcpy(&ndist.type, tmp, sizeof(uint32));
  tmp += sizeof(uint32);
  memcpy(&ndist.nitems, tmp, sizeof(uint32));
  tmp += sizeof(uint32);

  if (ndist.magic != STATS_NDISTINCT_MAGIC)
    elog(ERROR, "invalid ndistinct magic %08x (expected %08x)", ndist.magic,
         STATS_NDISTINCT_MAGIC);
  if (ndist.type != STATS_NDISTINCT_TYPE_BASIC)
    elog(ERROR, "invalid ndistinct type %d (expected %d)", ndist.type,
         STATS_NDISTINCT_TYPE_BASIC);
  if (ndist.nitems == 0)
    elog(ERROR, "invalid zero-length item array in MVNDistinct");

  /* what minimum bytea size do we expect for those parameters */
  minimum_size = MinSizeOfItems(ndist.nitems);
  if (VARSIZE_ANY_EXHDR(data) < minimum_size)
    elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
         VARSIZE_ANY_EXHDR(data), minimum_size);

  /*
   * Allocate space for the ndistinct items (no space for each item's
   * attnos: those live in bitmapsets allocated separately)
   */
  ndistinct = (MVNDistinct *)palloc0(MAXALIGN(offsetof(MVNDistinct, items)) +
                                     (ndist.nitems * sizeof(MVNDistinctItem)));
  ndistinct->magic = ndist.magic;
  ndistinct->type = ndist.type;
  ndistinct->nitems = ndist.nitems;

  for (i = 0; i < ndistinct->nitems; i++) {
    MVNDistinctItem *item = &ndistinct->items[i];

    /* ndistinct value */
    memcpy(&item->ndistinct, tmp, sizeof(double));
    tmp += sizeof(double);

    /* number of attributes */
    memcpy(&item->nattributes, tmp, sizeof(int));
    tmp += sizeof(int);
    Assert((item->nattributes >= 2) &&
           (item->nattributes <= STATS_MAX_DIMENSIONS));

    item->attributes =
        (AttrNumber *)palloc(item->nattributes * sizeof(AttrNumber));

    memcpy(item->attributes, tmp, sizeof(AttrNumber) * item->nattributes);
    tmp += sizeof(AttrNumber) * item->nattributes;

    /* still within the bytea */
    Assert(tmp <= ((char *)data + VARSIZE_ANY(data)));
  }

  /* we should have consumed the whole bytea exactly */
  Assert(tmp == ((char *)data + VARSIZE_ANY(data)));

  return ndistinct;
}

/*
 * pg_ndistinct_in
 *		input routine for type pg_ndistinct
 *
 * pg_ndistinct is real enough to be a table column, but it has no
 * operations of its own, and disallows input (just like pg_node_tree).
 */
Datum pg_ndistinct_in(PG_FUNCTION_ARGS) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot accept a value of type %s", "pg_ndistinct")));

  PG_RETURN_VOID(); /* keep compiler quiet */
}

/*
 * pg_ndistinct
 *		output routine for type pg_ndistinct
 *
 * Produces a human-readable representation of the value.
 */
Datum pg_ndistinct_out(PG_FUNCTION_ARGS) {
  bytea *data = PG_GETARG_BYTEA_PP(0);
  MVNDistinct *ndist = statext_ndistinct_deserialize(data);
  int i;
  StringInfoData str;

  initStringInfo(&str);
  appendStringInfoChar(&str, '{');

  for (i = 0; i < ndist->nitems; i++) {
    int j;
    MVNDistinctItem item = ndist->items[i];

    if (i > 0)
      appendStringInfoString(&str, ", ");

    for (j = 0; j < item.nattributes; j++) {
      AttrNumber attnum = item.attributes[j];

      appendStringInfo(&str, "%s%d", (j == 0) ? "\"" : ", ", attnum);
    }
    appendStringInfo(&str, "\": %d", (int)item.ndistinct);
  }

  appendStringInfoChar(&str, '}');

  PG_RETURN_CSTRING(str.data);
}

/*
 * pg_ndistinct_recv
 *		binary input routine for type pg_ndistinct
 */
Datum pg_ndistinct_recv(PG_FUNCTION_ARGS) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot accept a value of type %s", "pg_ndistinct")));

  PG_RETURN_VOID(); /* keep compiler quiet */
}

/*
 * pg_ndistinct_send
 *		binary output routine for type pg_ndistinct
 *
 * n-distinct is serialized into a bytea value, so let's send that.
 */
Datum pg_ndistinct_send(PG_FUNCTION_ARGS) { return byteasend(fcinfo); }

/*
 * generate_combinations_recurse
 *		given a prefix, generate all possible combinations
 *
 * Given a prefix (first few elements of the combination), generate following
 * elements recursively. We generate the combinations in lexicographic order,
 * which eliminates permutations of the same combination.
 */
static void generate_combinations_recurse(CombinationGenerator *state,
                                          int index, int start, int *current) {
  /* If we haven't filled all the elements, simply recurse. */
  if (index < state->k) {
    int i;

    /*
     * The values have to be in ascending order, so make sure we start
     * with the value passed by parameter.
     */

    for (i = start; i < state->n; i++) {
      current[index] = i;
      generate_combinations_recurse(state, (index + 1), (i + 1), current);
    }

    return;
  } else {
    /* we got a valid combination, add it to the array */
    memcpy(&state->combinations[(state->k * state->current)], current,
           state->k * sizeof(int));
    state->current++;
  }
}

