/*-------------------------------------------------------------------------
 *
 * mcv.c
 *	  POSTGRES multivariate MCV lists
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mcv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * Computes size of a serialized MCV item, depending on the number of
 * dimensions (columns) the statistic is defined on. The datum values are
 * stored in a separate array (deduplicated, to minimize the size), and
 * so the serialized items only store uint16 indexes into that array.
 *
 * Each serialized item stores (in this order):
 *
 * - indexes to values	  (ndim * sizeof(uint16))
 * - null flags			  (ndim * sizeof(bool))
 * - frequency			  (sizeof(double))
 * - base_frequency		  (sizeof(double))
 *
 * There is no alignment padding within an MCV item.
 * So in total each MCV item requires this many bytes:
 *
 *	 ndim * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double)
 */
#define ITEM_SIZE(ndims)	\
	((ndims) * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double))

/*
 * Used to compute size of serialized MCV list representation.
 */
#define MinSizeOfMCVList		\
	(VARHDRSZ + sizeof(uint32) * 3 + sizeof(AttrNumber))

/*
 * Size of the serialized MCV list, excluding the space needed for
 * deduplicated per-dimension values. The macro is meant to be used
 * when it's not yet safe to access the serialized info about amount
 * of data for each column.
 */
#define SizeOfMCVList(ndims, nitems)	\
	((MinSizeOfMCVList + sizeof(Oid) * (ndims)) + \
	 ((ndims) * sizeof(DimensionInfo)) + \
	 ((nitems) * ITEM_SIZE(ndims)))

/*
 * Compute new value for bitmap item, considering whether it's used for
 * clauses connected by AND/OR.
 */
#define RESULT_MERGE(value, is_or, match) \
	((is_or) ? ((value) || (match)) : ((value) && (match)))

/*
 * When processing a list of clauses, the bitmap item may get set to a value
 * such that additional clauses can't change it. For example, when processing
 * a list of clauses connected to AND, as soon as the item gets set to 'false'
 * then it'll remain like that. Similarly clauses connected by OR and 'true'.
 *
 * Returns true when the value in the bitmap can't change no matter how the
 * remaining clauses are evaluated.
 */
#define RESULT_IS_FINAL(value, is_or)	((is_or) ? (value) : (!(value)))

/*
 * statext_mcv_load
 *		Load the MCV list for the indicated pg_statistic_ext_data tuple.
 */
MCVList *statext_mcv_load(Oid mvoid, bool inh) {
  MCVList *result;
  bool isnull;
  Datum mcvlist;
  HeapTuple htup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(mvoid),
                                   BoolGetDatum(inh));

  if (!HeapTupleIsValid(htup))
    elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

  mcvlist = SysCacheGetAttr(STATEXTDATASTXOID, htup,
                            Anum_pg_statistic_ext_data_stxdmcv, &isnull);

  if (isnull)
    elog(ERROR,
         "requested statistics kind \"%c\" is not yet built for statistics "
         "object %u",
         STATS_EXT_MCV, mvoid);

  result = statext_mcv_deserialize(DatumGetByteaP(mcvlist));

  ReleaseSysCache(htup);

  return result;
}

/*
 * statext_mcv_deserialize
 *		Reads serialized MCV list into MCVList structure.
 *
 * All the memory needed by the MCV list is allocated as a single chunk, so
 * it's possible to simply pfree() it at once.
 */
MCVList *statext_mcv_deserialize(bytea *data) {
  int dim, i;
  Size expected_size;
  MCVList *mcvlist;
  char *raw;
  char *ptr;
  char *endptr PG_USED_FOR_ASSERTS_ONLY;

  int ndims, nitems;
  DimensionInfo *info = NULL;

  /* local allocation buffer (used only for deserialization) */
  Datum **map = NULL;

  /* MCV list */
  Size mcvlen;

  /* buffer used for the result */
  Size datalen;
  char *dataptr;
  char *valuesptr;
  char *isnullptr;

  if (data == NULL)
    return NULL;

  /*
   * We can't possibly deserialize a MCV list if there's not even a complete
   * header. We need an explicit formula here, because we serialize the
   * header fields one by one, so we need to ignore struct alignment.
   */
  if (VARSIZE_ANY(data) < MinSizeOfMCVList)
    elog(ERROR, "invalid MCV size %zu (expected at least %zu)",
         VARSIZE_ANY(data), MinSizeOfMCVList);

  /* read the MCV list header */
  mcvlist = (MCVList *)palloc0(offsetof(MCVList, items));

  /* pointer to the data part (skip the varlena header) */
  raw = (char *)data;
  ptr = VARDATA_ANY(raw);
  endptr = (char *)raw + VARSIZE_ANY(data);

  /* get the header and perform further sanity checks */
  memcpy(&mcvlist->magic, ptr, sizeof(uint32));
  ptr += sizeof(uint32);

  memcpy(&mcvlist->type, ptr, sizeof(uint32));
  ptr += sizeof(uint32);

  memcpy(&mcvlist->nitems, ptr, sizeof(uint32));
  ptr += sizeof(uint32);

  memcpy(&mcvlist->ndimensions, ptr, sizeof(AttrNumber));
  ptr += sizeof(AttrNumber);

  if (mcvlist->magic != STATS_MCV_MAGIC)
    elog(ERROR, "invalid MCV magic %u (expected %u)", mcvlist->magic,
         STATS_MCV_MAGIC);

  if (mcvlist->type != STATS_MCV_TYPE_BASIC)
    elog(ERROR, "invalid MCV type %u (expected %u)", mcvlist->type,
         STATS_MCV_TYPE_BASIC);

  if (mcvlist->ndimensions == 0)
    elog(ERROR, "invalid zero-length dimension array in MCVList");
  else if ((mcvlist->ndimensions > STATS_MAX_DIMENSIONS) ||
           (mcvlist->ndimensions < 0))
    elog(ERROR, "invalid length (%d) dimension array in MCVList",
         mcvlist->ndimensions);

  if (mcvlist->nitems == 0)
    elog(ERROR, "invalid zero-length item array in MCVList");
  else if (mcvlist->nitems > STATS_MCVLIST_MAX_ITEMS)
    elog(ERROR, "invalid length (%u) item array in MCVList", mcvlist->nitems);

  nitems = mcvlist->nitems;
  ndims = mcvlist->ndimensions;

  /*
   * Check amount of data including DimensionInfo for all dimensions and
   * also the serialized items (including uint16 indexes). Also, walk
   * through the dimension information and add it to the sum.
   */
  expected_size = SizeOfMCVList(ndims, nitems);

  /*
   * Check that we have at least the dimension and info records, along with
   * the items. We don't know the size of the serialized values yet. We need
   * to do this check first, before accessing the dimension info.
   */
  if (VARSIZE_ANY(data) < expected_size)
    elog(ERROR, "invalid MCV size %zu (expected %zu)", VARSIZE_ANY(data),
         expected_size);

  /* Now copy the array of type Oids. */
  memcpy(mcvlist->types, ptr, sizeof(Oid) * ndims);
  ptr += (sizeof(Oid) * ndims);

  /* Now it's safe to access the dimension info. */
  info = (DimensionInfo *)palloc(ndims * sizeof(DimensionInfo));

  memcpy(info, ptr, ndims * sizeof(DimensionInfo));
  ptr += (ndims * sizeof(DimensionInfo));

  /* account for the value arrays */
  for (dim = 0; dim < ndims; dim++) {
    /*
     * XXX I wonder if we can/should rely on asserts here. Maybe those
     * checks should be done every time?
     */
    Assert(info[dim].nvalues >= 0);
    Assert(info[dim].nbytes >= 0);

    expected_size += info[dim].nbytes;
  }

  /*
   * Now we know the total expected MCV size, including all the pieces
   * (header, dimension info. items and deduplicated data). So do the final
   * check on size.
   */
  if (VARSIZE_ANY(data) != expected_size)
    elog(ERROR, "invalid MCV size %zu (expected %zu)", VARSIZE_ANY(data),
         expected_size);

  /*
   * We need an array of Datum values for each dimension, so that we can
   * easily translate the uint16 indexes later. We also need a top-level
   * array of pointers to those per-dimension arrays.
   *
   * While allocating the arrays for dimensions, compute how much space we
   * need for a copy of the by-ref data, as we can't simply point to the
   * original values (it might go away).
   */
  datalen = 0; /* space for by-ref data */
  map = (Datum **)palloc(ndims * sizeof(Datum *));

  for (dim = 0; dim < ndims; dim++) {
    map[dim] = (Datum *)palloc(sizeof(Datum) * info[dim].nvalues);

    /* space needed for a copy of data for by-ref types */
    datalen += info[dim].nbytes_aligned;
  }

  /*
   * Now resize the MCV list so that the allocation includes all the data.
   *
   * Allocate space for a copy of the data, as we can't simply reference the
   * serialized data - it's not aligned properly, and it may disappear while
   * we're still using the MCV list, e.g. due to catcache release.
   *
   * We do care about alignment here, because we will allocate all the
   * pieces at once, but then use pointers to different parts.
   */
  mcvlen = MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

  /* arrays of values and isnull flags for all MCV items */
  mcvlen += nitems * MAXALIGN(sizeof(Datum) * ndims);
  mcvlen += nitems * MAXALIGN(sizeof(bool) * ndims);

  /* we don't quite need to align this, but it makes some asserts easier */
  mcvlen += MAXALIGN(datalen);

  /* now resize the deserialized MCV list, and compute pointers to parts */
  mcvlist = (MCVList *)repalloc(mcvlist, mcvlen);

  /* pointer to the beginning of values/isnull arrays */
  valuesptr = (char *)mcvlist +
              MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

  isnullptr = valuesptr + (nitems * MAXALIGN(sizeof(Datum) * ndims));

  dataptr = isnullptr + (nitems * MAXALIGN(sizeof(bool) * ndims));

  /*
   * Build mapping (index => value) for translating the serialized data into
   * the in-memory representation.
   */
  for (dim = 0; dim < ndims; dim++) {
    /* remember start position in the input array */
    char *start PG_USED_FOR_ASSERTS_ONLY = ptr;

    if (info[dim].typbyval) {
      /* for by-val types we simply copy data into the mapping */
      for (i = 0; i < info[dim].nvalues; i++) {
        Datum v = 0;

        memcpy(&v, ptr, info[dim].typlen);
        ptr += info[dim].typlen;

        map[dim][i] = fetch_att(&v, true, info[dim].typlen);

        /* no under/overflow of input array */
        Assert(ptr <= (start + info[dim].nbytes));
      }
    } else {
      /* for by-ref types we need to also make a copy of the data */

      /* passed by reference, but fixed length (name, tid, ...) */
      if (info[dim].typlen > 0) {
        for (i = 0; i < info[dim].nvalues; i++) {
          memcpy(dataptr, ptr, info[dim].typlen);
          ptr += info[dim].typlen;

          /* just point into the array */
          map[dim][i] = PointerGetDatum(dataptr);
          dataptr += MAXALIGN(info[dim].typlen);
        }
      } else if (info[dim].typlen == -1) {
        /* varlena */
        for (i = 0; i < info[dim].nvalues; i++) {
          uint32 len;

          /* read the uint32 length */
          memcpy(&len, ptr, sizeof(uint32));
          ptr += sizeof(uint32);

          /* the length is data-only */
          SET_VARSIZE(dataptr, len + VARHDRSZ);
          memcpy(VARDATA(dataptr), ptr, len);
          ptr += len;

          /* just point into the array */
          map[dim][i] = PointerGetDatum(dataptr);

          /* skip to place of the next deserialized value */
          dataptr += MAXALIGN(len + VARHDRSZ);
        }
      } else if (info[dim].typlen == -2) {
        /* cstring */
        for (i = 0; i < info[dim].nvalues; i++) {
          uint32 len;

          memcpy(&len, ptr, sizeof(uint32));
          ptr += sizeof(uint32);

          memcpy(dataptr, ptr, len);
          ptr += len;

          /* just point into the array */
          map[dim][i] = PointerGetDatum(dataptr);
          dataptr += MAXALIGN(len);
        }
      }

      /* no under/overflow of input array */
      Assert(ptr <= (start + info[dim].nbytes));

      /* no overflow of the output mcv value */
      Assert(dataptr <= ((char *)mcvlist + mcvlen));
    }

    /* check we consumed input data for this dimension exactly */
    Assert(ptr == (start + info[dim].nbytes));
  }

  /* we should have also filled the MCV list exactly */
  Assert(dataptr == ((char *)mcvlist + mcvlen));

  /* deserialize the MCV items and translate the indexes to Datums */
  for (i = 0; i < nitems; i++) {
    MCVItem *item = &mcvlist->items[i];

    item->values = (Datum *)valuesptr;
    valuesptr += MAXALIGN(sizeof(Datum) * ndims);

    item->isnull = (bool *)isnullptr;
    isnullptr += MAXALIGN(sizeof(bool) * ndims);

    memcpy(item->isnull, ptr, sizeof(bool) * ndims);
    ptr += sizeof(bool) * ndims;

    memcpy(&item->frequency, ptr, sizeof(double));
    ptr += sizeof(double);

    memcpy(&item->base_frequency, ptr, sizeof(double));
    ptr += sizeof(double);

    /* finally translate the indexes (for non-NULL only) */
    for (dim = 0; dim < ndims; dim++) {
      uint16 index;

      memcpy(&index, ptr, sizeof(uint16));
      ptr += sizeof(uint16);

      if (item->isnull[dim])
        continue;

      item->values[dim] = map[dim][index];
    }

    /* check we're not overflowing the input */
    Assert(ptr <= endptr);
  }

  /* check that we processed all the data */
  Assert(ptr == endptr);

  /* release the buffers used for mapping */
  for (dim = 0; dim < ndims; dim++)
    pfree(map[dim]);

  pfree(map);

  return mcvlist;
}

/*
 * SRF with details about buckets of a histogram:
 *
 * - item ID (0...nitems)
 * - values (string array)
 * - nulls only (boolean array)
 * - frequency (double precision)
 * - base_frequency (double precision)
 *
 * The input is the OID of the statistics, and there are no rows returned if
 * the statistics contains no histogram.
 */
Datum pg_stats_ext_mcvlist_items(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL()) {
    MemoryContext oldcontext;
    MCVList *mcvlist;
    TupleDesc tupdesc;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /* switch to memory context appropriate for multiple function calls */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    mcvlist = statext_mcv_deserialize(PG_GETARG_BYTEA_P(0));

    funcctx->user_fctx = mcvlist;

    /* total number of tuples to be returned */
    funcctx->max_calls = 0;
    if (funcctx->user_fctx != NULL)
      funcctx->max_calls = mcvlist->nitems;

    /* Build a tuple descriptor for our result type */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("function returning record called in context "
                             "that cannot accept type record")));
    tupdesc = BlessTupleDesc(tupdesc);

    /*
     * generate attribute metadata needed later to produce tuples from raw
     * C strings
     */
    funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

    MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();

  if (funcctx->call_cntr < funcctx->max_calls) /* do when there is more
                                                * left to send */
  {
    Datum values[5];
    bool nulls[5];
    HeapTuple tuple;
    Datum result;
    ArrayBuildState *astate_values = NULL;
    ArrayBuildState *astate_nulls = NULL;

    int i;
    MCVList *mcvlist;
    MCVItem *item;

    mcvlist = (MCVList *)funcctx->user_fctx;

    Assert(funcctx->call_cntr < mcvlist->nitems);

    item = &mcvlist->items[funcctx->call_cntr];

    for (i = 0; i < mcvlist->ndimensions; i++) {

      astate_nulls =
          accumArrayResult(astate_nulls, BoolGetDatum(item->isnull[i]), false,
                           BOOLOID, CurrentMemoryContext);

      if (!item->isnull[i]) {
        bool isvarlena;
        Oid outfunc;
        FmgrInfo fmgrinfo;
        Datum val;
        text *txt;

        /* lookup output func for the type */
        getTypeOutputInfo(mcvlist->types[i], &outfunc, &isvarlena);
        fmgr_info(outfunc, &fmgrinfo);

        val = FunctionCall1(&fmgrinfo, item->values[i]);
        txt = cstring_to_text(DatumGetPointer(val));

        astate_values = accumArrayResult(astate_values, PointerGetDatum(txt),
                                         false, TEXTOID, CurrentMemoryContext);
      } else
        astate_values = accumArrayResult(astate_values, (Datum)0, true, TEXTOID,
                                         CurrentMemoryContext);
    }

    values[0] = Int32GetDatum(funcctx->call_cntr);
    values[1] = makeArrayResult(astate_values, CurrentMemoryContext);
    values[2] = makeArrayResult(astate_nulls, CurrentMemoryContext);
    values[3] = Float8GetDatum(item->frequency);
    values[4] = Float8GetDatum(item->base_frequency);

    /* no NULLs in the tuple */
    memset(nulls, 0, sizeof(nulls));

    /* build a tuple */
    tuple = heap_form_tuple(funcctx->attinmeta->tupdesc, values, nulls);

    /* make the tuple into a datum */
    result = HeapTupleGetDatum(tuple);

    SRF_RETURN_NEXT(funcctx, result);
  } else /* do when there is no more left */
  {
    SRF_RETURN_DONE(funcctx);
  }
}

/*
 * pg_mcv_list_in		- input routine for type pg_mcv_list.
 *
 * pg_mcv_list is real enough to be a table column, but it has no operations
 * of its own, and disallows input too
 */
Datum pg_mcv_list_in(PG_FUNCTION_ARGS) {
  /*
   * pg_mcv_list stores the data in binary form and parsing text input is
   * not needed, so disallow this.
   */
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot accept a value of type %s", "pg_mcv_list")));

  PG_RETURN_VOID(); /* keep compiler quiet */
}

/*
 * pg_mcv_list_out		- output routine for type pg_mcv_list.
 *
 * MCV lists are serialized into a bytea value, so we simply call byteaout()
 * to serialize the value into text. But it'd be nice to serialize that into
 * a meaningful representation (e.g. for inspection by people).
 *
 * XXX This should probably return something meaningful, similar to what
 * pg_dependencies_out does. Not sure how to deal with the deduplicated
 * values, though - do we want to expand that or not?
 */
Datum pg_mcv_list_out(PG_FUNCTION_ARGS) { return byteaout(fcinfo); }

/*
 * pg_mcv_list_recv		- binary input routine for type pg_mcv_list.
 */
Datum pg_mcv_list_recv(PG_FUNCTION_ARGS) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot accept a value of type %s", "pg_mcv_list")));

  PG_RETURN_VOID(); /* keep compiler quiet */
}

/*
 * pg_mcv_list_send		- binary output routine for type pg_mcv_list.
 *
 * MCV lists are serialized in a bytea value (although the type is named
 * differently), so let's just send that.
 */
Datum pg_mcv_list_send(PG_FUNCTION_ARGS) { return byteasend(fcinfo); }

/*
 * match the attribute/expression to a dimension of the statistic
 *
 * Returns the zero-based index of the matching statistics dimension.
 * Optionally determines the collation.
 */
static int mcv_match_expression(Node *expr, Bitmapset *keys, List *exprs,
                                Oid *collid) {
  int idx;

  if (IsA(expr, Var)) {
    /* simple Var, so just lookup using varattno */
    Var *var = (Var *)expr;

    if (collid)
      *collid = var->varcollid;

    idx = bms_member_index(keys, var->varattno);

    if (idx < 0)
      elog(ERROR, "variable not found in statistics object");
  } else {
    /* expression - lookup in stats expressions */
    ListCell *lc;

    if (collid)
      *collid = exprCollation(expr);

    /* expressions are stored after the simple columns */
    idx = bms_num_members(keys);
    foreach (lc, exprs) {
      Node *stat_expr = (Node *)lfirst(lc);

      if (equal(expr, stat_expr))
        break;

      idx++;
    }

    if (lc == NULL)
      elog(ERROR, "expression not found in statistics object");
  }

  return idx;
}

/*
 * mcv_get_match_bitmap
 *	Evaluate clauses using the MCV list, and update the match bitmap.
 *
 * A match bitmap keeps match/mismatch status for each MCV item, and we
 * update it based on additional clauses. We also use it to skip items
 * that can't possibly match (e.g. item marked as "mismatch" can't change
 * to "match" when evaluating AND clause list).
 *
 * The function also returns a flag indicating whether there was an
 * equality condition for all attributes, the minimum frequency in the MCV
 * list, and a total MCV frequency (sum of frequencies for all items).
 *
 * XXX Currently the match bitmap uses a bool for each MCV item, which is
 * somewhat wasteful as we could do with just a single bit, thus reducing
 * the size to ~1/8. It would also allow us to combine bitmaps simply using
 * & and |, which should be faster than min/max. The bitmaps are fairly
 * small, though (thanks to the cap on the MCV list size).
 */
static bool *mcv_get_match_bitmap(PlannerInfo *root, List *clauses,
                                  Bitmapset *keys, List *exprs,
                                  MCVList *mcvlist, bool is_or) {
  ListCell *l;
  bool *matches;

  /* The bitmap may be partially built. */
  Assert(clauses != NIL);
  Assert(mcvlist != NULL);
  Assert(mcvlist->nitems > 0);
  Assert(mcvlist->nitems <= STATS_MCVLIST_MAX_ITEMS);

  matches = (bool *)palloc(sizeof(bool) * mcvlist->nitems);
  memset(matches, !is_or, sizeof(bool) * mcvlist->nitems);

  /*
   * Loop through the list of clauses, and for each of them evaluate all the
   * MCV items not yet eliminated by the preceding clauses.
   */
  foreach (l, clauses) {
    Node *clause = (Node *)lfirst(l);

    /* if it's a RestrictInfo, then extract the clause */
    if (IsA(clause, RestrictInfo))
      clause = (Node *)((RestrictInfo *)clause)->clause;

    /*
     * Handle the various types of clauses - OpClause, NullTest and
     * AND/OR/NOT
     */
    if (is_opclause(clause)) {
      OpExpr *expr = (OpExpr *)clause;
      FmgrInfo opproc;

      /* valid only after examine_opclause_args returns true */
      Node *clause_expr;
      Const *cst;
      bool expronleft;
      int idx;
      Oid collid;

      fmgr_info(get_opcode(expr->opno), &opproc);

      /* extract the var/expr and const from the expression */
      if (!examine_opclause_args(expr->args, &clause_expr, &cst, &expronleft))
        elog(ERROR, "incompatible clause");

      /* match the attribute/expression to a dimension of the statistic */
      idx = mcv_match_expression(clause_expr, keys, exprs, &collid);

      /*
       * Walk through the MCV items and evaluate the current clause. We
       * can skip items that were already ruled out, and terminate if
       * there are no remaining MCV items that might possibly match.
       */
      for (int i = 0; i < mcvlist->nitems; i++) {
        bool match = true;
        MCVItem *item = &mcvlist->items[i];

        Assert(idx >= 0);

        /*
         * When the MCV item or the Const value is NULL we can treat
         * this as a mismatch. We must not call the operator because
         * of strictness.
         */
        if (item->isnull[idx] || cst->constisnull) {
          matches[i] = RESULT_MERGE(matches[i], is_or, false);
          continue;
        }

        /*
         * Skip MCV items that can't change result in the bitmap. Once
         * the value gets false for AND-lists, or true for OR-lists,
         * we don't need to look at more clauses.
         */
        if (RESULT_IS_FINAL(matches[i], is_or))
          continue;

        /*
         * First check whether the constant is below the lower
         * boundary (in that case we can skip the bucket, because
         * there's no overlap).
         *
         * We don't store collations used to build the statistics, but
         * we can use the collation for the attribute itself, as
         * stored in varcollid. We do reset the statistics after a
         * type change (including collation change), so this is OK.
         * For expressions, we use the collation extracted from the
         * expression itself.
         */
        if (expronleft)
          match = DatumGetBool(FunctionCall2Coll(
              &opproc, collid, item->values[idx], cst->constvalue));
        else
          match = DatumGetBool(FunctionCall2Coll(
              &opproc, collid, cst->constvalue, item->values[idx]));

        /* update the match bitmap with the result */
        matches[i] = RESULT_MERGE(matches[i], is_or, match);
      }
    } else if (IsA(clause, ScalarArrayOpExpr)) {
      ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *)clause;
      FmgrInfo opproc;

      /* valid only after examine_opclause_args returns true */
      Node *clause_expr;
      Const *cst;
      bool expronleft;
      Oid collid;
      int idx;

      /* array evaluation */
      ArrayType *arrayval;
      int16 elmlen;
      bool elmbyval;
      char elmalign;
      int num_elems;
      Datum *elem_values;
      bool *elem_nulls;

      fmgr_info(get_opcode(expr->opno), &opproc);

      /* extract the var/expr and const from the expression */
      if (!examine_opclause_args(expr->args, &clause_expr, &cst, &expronleft))
        elog(ERROR, "incompatible clause");

      /* We expect Var on left */
      if (!expronleft)
        elog(ERROR, "incompatible clause");

      /*
       * Deconstruct the array constant, unless it's NULL (we'll cover
       * that case below)
       */
      if (!cst->constisnull) {
        arrayval = DatumGetArrayTypeP(cst->constvalue);
        get_typlenbyvalalign(ARR_ELEMTYPE(arrayval), &elmlen, &elmbyval,
                             &elmalign);
        deconstruct_array(arrayval, ARR_ELEMTYPE(arrayval), elmlen, elmbyval,
                          elmalign, &elem_values, &elem_nulls, &num_elems);
      }

      /* match the attribute/expression to a dimension of the statistic */
      idx = mcv_match_expression(clause_expr, keys, exprs, &collid);

      /*
       * Walk through the MCV items and evaluate the current clause. We
       * can skip items that were already ruled out, and terminate if
       * there are no remaining MCV items that might possibly match.
       */
      for (int i = 0; i < mcvlist->nitems; i++) {
        int j;
        bool match = !expr->useOr;
        MCVItem *item = &mcvlist->items[i];

        /*
         * When the MCV item or the Const value is NULL we can treat
         * this as a mismatch. We must not call the operator because
         * of strictness.
         */
        if (item->isnull[idx] || cst->constisnull) {
          matches[i] = RESULT_MERGE(matches[i], is_or, false);
          continue;
        }

        /*
         * Skip MCV items that can't change result in the bitmap. Once
         * the value gets false for AND-lists, or true for OR-lists,
         * we don't need to look at more clauses.
         */
        if (RESULT_IS_FINAL(matches[i], is_or))
          continue;

        for (j = 0; j < num_elems; j++) {
          Datum elem_value = elem_values[j];
          bool elem_isnull = elem_nulls[j];
          bool elem_match;

          /* NULL values always evaluate as not matching. */
          if (elem_isnull) {
            match = RESULT_MERGE(match, expr->useOr, false);
            continue;
          }

          /*
           * Stop evaluating the array elements once we reach a
           * matching value that can't change - ALL() is the same as
           * AND-list, ANY() is the same as OR-list.
           */
          if (RESULT_IS_FINAL(match, expr->useOr))
            break;

          elem_match = DatumGetBool(FunctionCall2Coll(
              &opproc, collid, item->values[idx], elem_value));

          match = RESULT_MERGE(match, expr->useOr, elem_match);
        }

        /* update the match bitmap with the result */
        matches[i] = RESULT_MERGE(matches[i], is_or, match);
      }
    } else if (IsA(clause, NullTest)) {
      NullTest *expr = (NullTest *)clause;
      Node *clause_expr = (Node *)(expr->arg);

      /* match the attribute/expression to a dimension of the statistic */
      int idx = mcv_match_expression(clause_expr, keys, exprs, NULL);

      /*
       * Walk through the MCV items and evaluate the current clause. We
       * can skip items that were already ruled out, and terminate if
       * there are no remaining MCV items that might possibly match.
       */
      for (int i = 0; i < mcvlist->nitems; i++) {
        bool match = false; /* assume mismatch */
        MCVItem *item = &mcvlist->items[i];

        /* if the clause mismatches the MCV item, update the bitmap */
        switch (expr->nulltesttype) {
        case IS_NULL:
          match = (item->isnull[idx]) ? true : match;
          break;

        case IS_NOT_NULL:
          match = (!item->isnull[idx]) ? true : match;
          break;
        }

        /* now, update the match bitmap, depending on OR/AND type */
        matches[i] = RESULT_MERGE(matches[i], is_or, match);
      }
    } else if (is_orclause(clause) || is_andclause(clause)) {
      /* AND/OR clause, with all subclauses being compatible */

      int i;
      BoolExpr *bool_clause = ((BoolExpr *)clause);
      List *bool_clauses = bool_clause->args;

      /* match/mismatch bitmap for each MCV item */
      bool *bool_matches = NULL;

      Assert(bool_clauses != NIL);
      Assert(list_length(bool_clauses) >= 2);

      /* build the match bitmap for the OR-clauses */
      bool_matches = mcv_get_match_bitmap(root, bool_clauses, keys, exprs,
                                          mcvlist, is_orclause(clause));

      /*
       * Merge the bitmap produced by mcv_get_match_bitmap into the
       * current one. We need to consider if we're evaluating AND or OR
       * condition when merging the results.
       */
      for (i = 0; i < mcvlist->nitems; i++)
        matches[i] = RESULT_MERGE(matches[i], is_or, bool_matches[i]);

      pfree(bool_matches);
    } else if (is_notclause(clause)) {
      /* NOT clause, with all subclauses compatible */

      int i;
      BoolExpr *not_clause = ((BoolExpr *)clause);
      List *not_args = not_clause->args;

      /* match/mismatch bitmap for each MCV item */
      bool *not_matches = NULL;

      Assert(not_args != NIL);
      Assert(list_length(not_args) == 1);

      /* build the match bitmap for the NOT-clause */
      not_matches =
          mcv_get_match_bitmap(root, not_args, keys, exprs, mcvlist, false);

      /*
       * Merge the bitmap produced by mcv_get_match_bitmap into the
       * current one. We're handling a NOT clause, so invert the result
       * before merging it into the global bitmap.
       */
      for (i = 0; i < mcvlist->nitems; i++)
        matches[i] = RESULT_MERGE(matches[i], is_or, !not_matches[i]);

      pfree(not_matches);
    } else if (IsA(clause, Var)) {
      /* Var (has to be a boolean Var, possibly from below NOT) */

      Var *var = (Var *)(clause);

      /* match the attribute to a dimension of the statistic */
      int idx = bms_member_index(keys, var->varattno);

      Assert(var->vartype == BOOLOID);

      /*
       * Walk through the MCV items and evaluate the current clause. We
       * can skip items that were already ruled out, and terminate if
       * there are no remaining MCV items that might possibly match.
       */
      for (int i = 0; i < mcvlist->nitems; i++) {
        MCVItem *item = &mcvlist->items[i];
        bool match = false;

        /* if the item is NULL, it's a mismatch */
        if (!item->isnull[idx] && DatumGetBool(item->values[idx]))
          match = true;

        /* update the result bitmap */
        matches[i] = RESULT_MERGE(matches[i], is_or, match);
      }
    } else {
      /* Otherwise, it must be a bare boolean-returning expression */
      int idx;

      /* match the expression to a dimension of the statistic */
      idx = mcv_match_expression(clause, keys, exprs, NULL);

      /*
       * Walk through the MCV items and evaluate the current clause. We
       * can skip items that were already ruled out, and terminate if
       * there are no remaining MCV items that might possibly match.
       */
      for (int i = 0; i < mcvlist->nitems; i++) {
        bool match;
        MCVItem *item = &mcvlist->items[i];

        /* "match" just means it's bool TRUE */
        match = !item->isnull[idx] && DatumGetBool(item->values[idx]);

        /* now, update the match bitmap, depending on OR/AND type */
        matches[i] = RESULT_MERGE(matches[i], is_or, match);
      }
    }
  }

  return matches;
}

/*
 * mcv_combine_selectivities
 * 		Combine per-column and multi-column MCV selectivity estimates.
 *
 * simple_sel is a "simple" selectivity estimate (produced without using any
 * extended statistics, essentially assuming independence of columns/clauses).
 *
 * mcv_sel and mcv_basesel are sums of the frequencies and base frequencies of
 * all matching MCV items.  The difference (mcv_sel - mcv_basesel) is then
 * essentially interpreted as a correction to be added to simple_sel, as
 * described below.
 *
 * mcv_totalsel is the sum of the frequencies of all MCV items (not just the
 * matching ones).  This is used as an upper bound on the portion of the
 * selectivity estimates not covered by the MCV statistics.
 *
 * Note: While simple and base selectivities are defined in a quite similar
 * way, the values are computed differently and are not therefore equal. The
 * simple selectivity is computed as a product of per-clause estimates, while
 * the base selectivity is computed by adding up base frequencies of matching
 * items of the multi-column MCV list. So the values may differ for two main
 * reasons - (a) the MCV list may not cover 100% of the data and (b) some of
 * the MCV items did not match the estimated clauses.
 *
 * As both (a) and (b) reduce the base selectivity value, it generally holds
 * that (simple_sel >= mcv_basesel). If the MCV list covers all the data, the
 * values may be equal.
 *
 * So, other_sel = (simple_sel - mcv_basesel) is an estimate for the part not
 * covered by the MCV list, and (mcv_sel - mcv_basesel) may be seen as a
 * correction for the part covered by the MCV list. Those two statements are
 * actually equivalent.
 */
Selectivity mcv_combine_selectivities(Selectivity simple_sel,
                                      Selectivity mcv_sel,
                                      Selectivity mcv_basesel,
                                      Selectivity mcv_totalsel) {
  Selectivity other_sel;
  Selectivity sel;

  /* estimated selectivity of values not covered by MCV matches */
  other_sel = simple_sel - mcv_basesel;
  CLAMP_PROBABILITY(other_sel);

  /* this non-MCV selectivity cannot exceed 1 - mcv_totalsel */
  if (other_sel > 1.0 - mcv_totalsel)
    other_sel = 1.0 - mcv_totalsel;

  /* overall selectivity is the sum of the MCV and non-MCV parts */
  sel = mcv_sel + other_sel;
  CLAMP_PROBABILITY(sel);

  return sel;
}

/*
 * mcv_clauselist_selectivity
 *		Use MCV statistics to estimate the selectivity of an
 *implicitly-ANDed list of clauses.
 *
 * This determines which MCV items match every clause in the list and returns
 * the sum of the frequencies of those items.
 *
 * In addition, it returns the sum of the base frequencies of each of those
 * items (that is the sum of the selectivities that each item would have if
 * the columns were independent of one another), and the total selectivity of
 * all the MCV items (not just the matching ones).  These are expected to be
 * used together with a "simple" selectivity estimate (one based only on
 * per-column statistics) to produce an overall selectivity estimate that
 * makes use of both per-column and multi-column statistics --- see
 * mcv_combine_selectivities().
 */
Selectivity mcv_clauselist_selectivity(PlannerInfo *root,
                                       StatisticExtInfo *stat, List *clauses,
                                       int varRelid, JoinType jointype,
                                       SpecialJoinInfo *sjinfo, RelOptInfo *rel,
                                       Selectivity *basesel,
                                       Selectivity *totalsel) {
  int i;
  MCVList *mcv;
  Selectivity s = 0.0;
  RangeTblEntry *rte = root->simple_rte_array[rel->relid];

  /* match/mismatch bitmap for each MCV item */
  bool *matches = NULL;

  /* load the MCV list stored in the statistics object */
  mcv = statext_mcv_load(stat->statOid, rte->inh);

  /* build a match bitmap for the clauses */
  matches =
      mcv_get_match_bitmap(root, clauses, stat->keys, stat->exprs, mcv, false);

  /* sum frequencies for all the matching MCV items */
  *basesel = 0.0;
  *totalsel = 0.0;
  for (i = 0; i < mcv->nitems; i++) {
    *totalsel += mcv->items[i].frequency;

    if (matches[i] != false) {
      *basesel += mcv->items[i].base_frequency;
      s += mcv->items[i].frequency;
    }
  }

  return s;
}

/*
 * mcv_clause_selectivity_or
 *		Use MCV statistics to estimate the selectivity of a clause that
 *		appears in an ORed list of clauses.
 *
 * As with mcv_clauselist_selectivity() this determines which MCV items match
 * the clause and returns both the sum of the frequencies and the sum of the
 * base frequencies of those items, as well as the sum of the frequencies of
 * all MCV items (not just the matching ones) so that this information can be
 * used by mcv_combine_selectivities() to produce a selectivity estimate that
 * makes use of both per-column and multi-column statistics.
 *
 * Additionally, we return information to help compute the overall selectivity
 * of the ORed list of clauses assumed to contain this clause.  This function
 * is intended to be called for each clause in the ORed list of clauses,
 * allowing the overall selectivity to be computed using the following
 * algorithm:
 *
 * Suppose P[n] = P(C[1] OR C[2] OR ... OR C[n]) is the combined selectivity
 * of the first n clauses in the list.  Then the combined selectivity taking
 * into account the next clause C[n+1] can be written as
 *
 *		P[n+1] = P[n] + P(C[n+1]) - P((C[1] OR ... OR C[n]) AND C[n+1])
 *
 * The final term above represents the overlap between the clauses examined so
 * far and the (n+1)'th clause.  To estimate its selectivity, we track the
 * match bitmap for the ORed list of clauses examined so far and examine its
 * intersection with the match bitmap for the (n+1)'th clause.
 *
 * We then also return the sums of the MCV item frequencies and base
 * frequencies for the match bitmap intersection corresponding to the overlap
 * term above, so that they can be combined with a simple selectivity estimate
 * for that term.
 *
 * The parameter "or_matches" is an in/out parameter tracking the match bitmap
 * for the clauses examined so far.  The caller is expected to set it to NULL
 * the first time it calls this function.
 */
Selectivity mcv_clause_selectivity_or(PlannerInfo *root, StatisticExtInfo *stat,
                                      MCVList *mcv, Node *clause,
                                      bool **or_matches, Selectivity *basesel,
                                      Selectivity *overlap_mcvsel,
                                      Selectivity *overlap_basesel,
                                      Selectivity *totalsel) {
  Selectivity s = 0.0;
  bool *new_matches;
  int i;

  /* build the OR-matches bitmap, if not built already */
  if (*or_matches == NULL)
    *or_matches = (bool *)palloc0(sizeof(bool) * mcv->nitems);

  /* build the match bitmap for the new clause */
  new_matches = mcv_get_match_bitmap(root, list_make1(clause), stat->keys,
                                     stat->exprs, mcv, false);

  /*
   * Sum the frequencies for all the MCV items matching this clause and also
   * those matching the overlap between this clause and any of the preceding
   * clauses as described above.
   */
  *basesel = 0.0;
  *overlap_mcvsel = 0.0;
  *overlap_basesel = 0.0;
  *totalsel = 0.0;
  for (i = 0; i < mcv->nitems; i++) {
    *totalsel += mcv->items[i].frequency;

    if (new_matches[i]) {
      s += mcv->items[i].frequency;
      *basesel += mcv->items[i].base_frequency;

      if ((*or_matches)[i]) {
        *overlap_mcvsel += mcv->items[i].frequency;
        *overlap_basesel += mcv->items[i].base_frequency;
      }
    }

    /* update the OR-matches bitmap for the next clause */
    (*or_matches)[i] = (*or_matches)[i] || new_matches[i];
  }

  pfree(new_matches);

  return s;
}
