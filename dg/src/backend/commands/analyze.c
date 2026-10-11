/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  the Postgres statistics generator
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "commands/dbcommands.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "storage/buffile.h"
#include "storage/procarray.h"
#include "utils/attoptcache.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Per-index data for ANALYZE */

/* Default statistics target (GUC parameter) */
thread_local /*LOCAL*/ int default_statistics_target = 100;


/*==========================================================================
 *
 * Code below this point represents the "standard" type-specific statistics
 * analysis algorithms.  This code can be replaced on a per-data-type basis
 * by setting a nonzero value in pg_type.typanalyze.
 *
 *==========================================================================
 */

/*
 * To avoid consuming too much memory during analysis and/or too much space
 * in the resulting pg_statistic rows, we ignore varlena datums that are wider
 * than WIDTH_THRESHOLD (after detoasting!).  This is legitimate for MCV
 * and distinct-value calculations since a wide value is unlikely to be
 * duplicated at all, much less be a most-common value.  For the same reason,
 * ignoring wide values will not affect our estimates of histogram bin
 * boundaries very much.
 */
#define WIDTH_THRESHOLD  1024

#define swapInt(a, b)	do {int _tmp; _tmp=a; a=b; b=_tmp;} while(0)
#define swapDatum(a, b)	do {Datum _tmp; _tmp=a; a=b; b=_tmp;} while(0)

/*
 * Extra information used by the default analysis routines
 */
typedef struct {
  int count; /* # of duplicates */
  int first; /* values[] index of first occurrence */
} ScalarMCVItem;

typedef struct {
  SortSupport ssup;
  int *tupnoLink;
} CompareScalarsContext;

static void compute_trivial_stats(VacAttrStatsP stats,
                                  AnalyzeAttrFetchFunc fetchfunc,
                                  int samplerows, double totalrows);
static void compute_distinct_stats(VacAttrStatsP stats,
                                   AnalyzeAttrFetchFunc fetchfunc,
                                   int samplerows, double totalrows);
static void compute_scalar_stats(VacAttrStatsP stats,
                                 AnalyzeAttrFetchFunc fetchfunc, int samplerows,
                                 double totalrows);
static int compare_scalars(const void *a, const void *b, void *arg);
static int compare_mcvs(const void *a, const void *b, void *arg);
static int analyze_mcv_list(int *mcv_counts, int num_mcv, double stadistinct,
                            double stanullfrac, int samplerows,
                            double totalrows);

/*
 * std_typanalyze -- the default type-specific typanalyze function
 */
bool std_typanalyze(VacAttrStats *stats) {
  Oid ltopr;
  Oid eqopr;
  StdAnalyzeData *mystats;

  /* If the attstattarget column is negative, use the default value */
  if (stats->attstattarget < 0)
    stats->attstattarget = default_statistics_target;

  /* Look for default "<" and "=" operators for column's type */
  get_sort_group_operators(stats->attrtypid, false, false, false, &ltopr,
                           &eqopr, NULL, NULL);

  /* Save the operator info for compute_stats routines */
  mystats = (StdAnalyzeData *)palloc(sizeof(StdAnalyzeData));
  mystats->eqopr = eqopr;
  mystats->eqfunc = OidIsValid(eqopr) ? get_opcode(eqopr) : InvalidOid;
  mystats->ltopr = ltopr;
  stats->extra_data = mystats;

  /*
   * Determine which standard statistics algorithm to use
   */
  if (OidIsValid(eqopr) && OidIsValid(ltopr)) {
    /* Seems to be a scalar datatype */
    stats->compute_stats = compute_scalar_stats;
    /*--------------------
     * The following choice of minrows is based on the paper
     * "Random sampling for histogram construction: how much is enough?"
     * by Surajit Chaudhuri, Rajeev Motwani and Vivek Narasayya, in
     * Proceedings of ACM SIGMOD International Conference on Management
     * of Data, 1998, Pages 436-447.  Their Corollary 1 to Theorem 5
     * says that for table size n, histogram size k, maximum relative
     * error in bin size f, and error probability gamma, the minimum
     * random sample size is
     *		r = 4 * k * ln(2*n/gamma) / f^2
     * Taking f = 0.5, gamma = 0.01, n = 10^6 rows, we obtain
     *		r = 305.82 * k
     * Note that because of the log function, the dependence on n is
     * quite weak; even at n = 10^12, a 300*k sample gives <= 0.66
     * bin size error with probability 0.99.  So there's no real need to
     * scale for n, which is a good thing because we don't necessarily
     * know it at this point.
     *--------------------
     */
    stats->minrows = 300 * stats->attstattarget;
  } else if (OidIsValid(eqopr)) {
    /* We can still recognize distinct values */
    stats->compute_stats = compute_distinct_stats;
    /* Might as well use the same minrows as above */
    stats->minrows = 300 * stats->attstattarget;
  } else {
    /* Can't do much but the trivial stuff */
    stats->compute_stats = compute_trivial_stats;
    /* Might as well use the same minrows as above */
    stats->minrows = 300 * stats->attstattarget;
  }

  return true;
}

/*
 *	compute_trivial_stats() -- compute very basic column statistics
 *
 *	We use this when we cannot find a hash "=" operator for the datatype.
 *
 *	We determine the fraction of non-null rows and the average datum width.
 */
static void compute_trivial_stats(VacAttrStatsP stats,
                                  AnalyzeAttrFetchFunc fetchfunc,
                                  int samplerows, double totalrows) {
  int i;
  int null_cnt = 0;
  int nonnull_cnt = 0;
  double total_width = 0;
  bool is_varlena =
      (!stats->attrtype->typbyval && stats->attrtype->typlen == -1);
  bool is_varwidth =
      (!stats->attrtype->typbyval && stats->attrtype->typlen < 0);

  for (i = 0; i < samplerows; i++) {
    Datum value;
    bool isnull;

    vacuum_delay_point(true);

    value = fetchfunc(stats, i, &isnull);

    /* Check for null/nonnull */
    if (isnull) {
      null_cnt++;
      continue;
    }
    nonnull_cnt++;

    /*
     * If it's a variable-width field, add up widths for average width
     * calculation.  Note that if the value is toasted, we use the toasted
     * width.  We don't bother with this calculation if it's a fixed-width
     * type.
     */
    if (is_varlena) {
      total_width += VARSIZE_ANY(DatumGetPointer(value));
    } else if (is_varwidth) {
      /* must be cstring */
      total_width += strlen(DatumGetCString(value)) + 1;
    }
  }

  /* We can only compute average width if we found some non-null values. */
  if (nonnull_cnt > 0) {
    stats->stats_valid = true;
    /* Do the simple null-frac and width stats */
    stats->stanullfrac = (double)null_cnt / (double)samplerows;
    if (is_varwidth)
      stats->stawidth = total_width / (double)nonnull_cnt;
    else
      stats->stawidth = stats->attrtype->typlen;
    stats->stadistinct = 0.0; /* "unknown" */
  } else if (null_cnt > 0) {
    /* We found only nulls; assume the column is entirely null */
    stats->stats_valid = true;
    stats->stanullfrac = 1.0;
    if (is_varwidth)
      stats->stawidth = 0; /* "unknown" */
    else
      stats->stawidth = stats->attrtype->typlen;
    stats->stadistinct = 0.0; /* "unknown" */
  }
}

/*
 *	compute_distinct_stats() -- compute column statistics including
 *ndistinct
 *
 *	We use this when we can find only an "=" operator for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, and the (estimated) number of distinct values.
 *
 *	The most common values are determined by brute force: we keep a list
 *	of previously seen values, ordered by number of times seen, as we scan
 *	the samples.  A newly seen value is inserted just after the last
 *	multiply-seen value, causing the bottommost (oldest) singly-seen value
 *	to drop off the list.  The accuracy of this method, and also its cost,
 *	depend mainly on the length of the list we are willing to keep.
 */
static void compute_distinct_stats(VacAttrStatsP stats,
                                   AnalyzeAttrFetchFunc fetchfunc,
                                   int samplerows, double totalrows) {
  int i;
  int null_cnt = 0;
  int nonnull_cnt = 0;
  int toowide_cnt = 0;
  double total_width = 0;
  bool is_varlena =
      (!stats->attrtype->typbyval && stats->attrtype->typlen == -1);
  bool is_varwidth =
      (!stats->attrtype->typbyval && stats->attrtype->typlen < 0);
  FmgrInfo f_cmpeq;
  typedef struct {
    Datum value;
    int count;
  } TrackItem;
  TrackItem *track;
  int track_cnt, track_max;
  int num_mcv = stats->attstattarget;
  StdAnalyzeData *mystats = (StdAnalyzeData *)stats->extra_data;

  /*
   * We track up to 2*n values for an n-element MCV list; but at least 10
   */
  track_max = 2 * num_mcv;
  if (track_max < 10)
    track_max = 10;
  track = (TrackItem *)palloc(track_max * sizeof(TrackItem));
  track_cnt = 0;

  fmgr_info(mystats->eqfunc, &f_cmpeq);

  for (i = 0; i < samplerows; i++) {
    Datum value;
    bool isnull;
    bool match;
    int firstcount1, j;

    vacuum_delay_point(true);

    value = fetchfunc(stats, i, &isnull);

    /* Check for null/nonnull */
    if (isnull) {
      null_cnt++;
      continue;
    }
    nonnull_cnt++;

    /*
     * If it's a variable-width field, add up widths for average width
     * calculation.  Note that if the value is toasted, we use the toasted
     * width.  We don't bother with this calculation if it's a fixed-width
     * type.
     */
    if (is_varlena) {
      total_width += VARSIZE_ANY(DatumGetPointer(value));

      /*
       * If the value is toasted, we want to detoast it just once to
       * avoid repeated detoastings and resultant excess memory usage
       * during the comparisons.  Also, check to see if the value is
       * excessively wide, and if so don't detoast at all --- just
       * ignore the value.
       */
      if (toast_raw_datum_size(value) > WIDTH_THRESHOLD) {
        toowide_cnt++;
        continue;
      }
      value = PointerGetDatum(PG_DETOAST_DATUM(value));
    } else if (is_varwidth) {
      /* must be cstring */
      total_width += strlen(DatumGetCString(value)) + 1;
    }

    /*
     * See if the value matches anything we're already tracking.
     */
    match = false;
    firstcount1 = track_cnt;
    for (j = 0; j < track_cnt; j++) {
      if (DatumGetBool(FunctionCall2Coll(&f_cmpeq, stats->attrcollid, value,
                                         track[j].value))) {
        match = true;
        break;
      }
      if (j < firstcount1 && track[j].count == 1)
        firstcount1 = j;
    }

    if (match) {
      /* Found a match */
      track[j].count++;
      /* This value may now need to "bubble up" in the track list */
      while (j > 0 && track[j].count > track[j - 1].count) {
        swapDatum(track[j].value, track[j - 1].value);
        swapInt(track[j].count, track[j - 1].count);
        j--;
      }
    } else {
      /* No match.  Insert at head of count-1 list */
      if (track_cnt < track_max)
        track_cnt++;
      for (j = track_cnt - 1; j > firstcount1; j--) {
        track[j].value = track[j - 1].value;
        track[j].count = track[j - 1].count;
      }
      if (firstcount1 < track_cnt) {
        track[firstcount1].value = value;
        track[firstcount1].count = 1;
      }
    }
  }

  /* We can only compute real stats if we found some non-null values. */
  if (nonnull_cnt > 0) {
    int nmultiple, summultiple;

    stats->stats_valid = true;
    /* Do the simple null-frac and width stats */
    stats->stanullfrac = (double)null_cnt / (double)samplerows;
    if (is_varwidth)
      stats->stawidth = total_width / (double)nonnull_cnt;
    else
      stats->stawidth = stats->attrtype->typlen;

    /* Count the number of values we found multiple times */
    summultiple = 0;
    for (nmultiple = 0; nmultiple < track_cnt; nmultiple++) {
      if (track[nmultiple].count == 1)
        break;
      summultiple += track[nmultiple].count;
    }

    if (nmultiple == 0) {
      /*
       * If we found no repeated non-null values, assume it's a unique
       * column; but be sure to discount for any nulls we found.
       */
      stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
    } else if (track_cnt < track_max && toowide_cnt == 0 &&
               nmultiple == track_cnt) {
      /*
       * Our track list includes every value in the sample, and every
       * value appeared more than once.  Assume the column has just
       * these values.  (This case is meant to address columns with
       * small, fixed sets of possible values, such as boolean or enum
       * columns.  If there are any values that appear just once in the
       * sample, including too-wide values, we should assume that that's
       * not what we're dealing with.)
       */
      stats->stadistinct = track_cnt;
    } else {
      /*----------
       * Estimate the number of distinct values using the estimator
       * proposed by Haas and Stokes in IBM Research Report RJ 10025:
       *		n*d / (n - f1 + f1*n/N)
       * where f1 is the number of distinct values that occurred
       * exactly once in our sample of n rows (from a total of N),
       * and d is the total number of distinct values in the sample.
       * This is their Duj1 estimator; the other estimators they
       * recommend are considerably more complex, and are numerically
       * very unstable when n is much smaller than N.
       *
       * In this calculation, we consider only non-nulls.  We used to
       * include rows with null values in the n and N counts, but that
       * leads to inaccurate answers in columns with many nulls, and
       * it's intuitively bogus anyway considering the desired result is
       * the number of distinct non-null values.
       *
       * We assume (not very reliably!) that all the multiply-occurring
       * values are reflected in the final track[] list, and the other
       * nonnull values all appeared but once.  (XXX this usually
       * results in a drastic overestimate of ndistinct.  Can we do
       * any better?)
       *----------
       */
      int f1 = nonnull_cnt - summultiple;
      int d = f1 + nmultiple;
      double n = samplerows - null_cnt;
      double N = totalrows * (1.0 - stats->stanullfrac);
      double stadistinct;

      /* N == 0 shouldn't happen, but just in case ... */
      if (N > 0)
        stadistinct = (n * d) / ((n - f1) + f1 * n / N);
      else
        stadistinct = 0;

      /* Clamp to sane range in case of roundoff error */
      if (stadistinct < d)
        stadistinct = d;
      if (stadistinct > N)
        stadistinct = N;
      /* And round to integer */
      stats->stadistinct = floor(stadistinct + 0.5);
    }

    /*
     * If we estimated the number of distinct values at more than 10% of
     * the total row count (a very arbitrary limit), then assume that
     * stadistinct should scale with the row count rather than be a fixed
     * value.
     */
    if (stats->stadistinct > 0.1 * totalrows)
      stats->stadistinct = -(stats->stadistinct / totalrows);

    /*
     * Decide how many values are worth storing as most-common values. If
     * we are able to generate a complete MCV list (all the values in the
     * sample will fit, and we think these are all the ones in the table),
     * then do so.  Otherwise, store only those values that are
     * significantly more common than the values not in the list.
     *
     * Note: the first of these cases is meant to address columns with
     * small, fixed sets of possible values, such as boolean or enum
     * columns.  If we can *completely* represent the column population by
     * an MCV list that will fit into the stats target, then we should do
     * so and thus provide the planner with complete information.  But if
     * the MCV list is not complete, it's generally worth being more
     * selective, and not just filling it all the way up to the stats
     * target.
     */
    if (track_cnt < track_max && toowide_cnt == 0 && stats->stadistinct > 0 &&
        track_cnt <= num_mcv) {
      /* Track list includes all values seen, and all will fit */
      num_mcv = track_cnt;
    } else {
      int *mcv_counts;

      /* Incomplete list; decide how many values are worth keeping */
      if (num_mcv > track_cnt)
        num_mcv = track_cnt;

      if (num_mcv > 0) {
        mcv_counts = (int *)palloc(num_mcv * sizeof(int));
        for (i = 0; i < num_mcv; i++)
          mcv_counts[i] = track[i].count;

        num_mcv = analyze_mcv_list(mcv_counts, num_mcv, stats->stadistinct,
                                   stats->stanullfrac, samplerows, totalrows);
      }
    }

    /* Generate MCV slot entry */
    if (num_mcv > 0) {
      MemoryContext old_context;
      Datum *mcv_values;
      float4 *mcv_freqs;

      /* Must copy the target values into anl_context */
      old_context = MemoryContextSwitchTo(stats->anl_context);
      mcv_values = (Datum *)palloc(num_mcv * sizeof(Datum));
      mcv_freqs = (float4 *)palloc(num_mcv * sizeof(float4));
      for (i = 0; i < num_mcv; i++) {
        mcv_values[i] = datumCopy(track[i].value, stats->attrtype->typbyval,
                                  stats->attrtype->typlen);
        mcv_freqs[i] = (double)track[i].count / (double)samplerows;
      }
      MemoryContextSwitchTo(old_context);

      stats->stakind[0] = STATISTIC_KIND_MCV;
      stats->staop[0] = mystats->eqopr;
      stats->stacoll[0] = stats->attrcollid;
      stats->stanumbers[0] = mcv_freqs;
      stats->numnumbers[0] = num_mcv;
      stats->stavalues[0] = mcv_values;
      stats->numvalues[0] = num_mcv;

      /*
       * Accept the defaults for stats->statypid and others. They have
       * been set before we were called (see vacuum.h)
       */
    }
  } else if (null_cnt > 0) {
    /* We found only nulls; assume the column is entirely null */
    stats->stats_valid = true;
    stats->stanullfrac = 1.0;
    if (is_varwidth)
      stats->stawidth = 0; /* "unknown" */
    else
      stats->stawidth = stats->attrtype->typlen;
    stats->stadistinct = 0.0; /* "unknown" */
  }

  /* We don't need to bother cleaning up any of our temporary palloc's */
}

/*
 *	compute_scalar_stats() -- compute column statistics
 *
 *	We use this when we can find "=" and "<" operators for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, the (estimated) number of distinct values, the
 *	distribution histogram, and the correlation of physical to logical
 *order.
 *
 *	The desired stats can be determined fairly easily after sorting the
 *	data values into order.
 */
static void compute_scalar_stats(VacAttrStatsP stats,
                                 AnalyzeAttrFetchFunc fetchfunc, int samplerows,
                                 double totalrows) {
  int i;
  int null_cnt = 0;
  int nonnull_cnt = 0;
  int toowide_cnt = 0;
  double total_width = 0;
  bool is_varlena =
      (!stats->attrtype->typbyval && stats->attrtype->typlen == -1);
  bool is_varwidth =
      (!stats->attrtype->typbyval && stats->attrtype->typlen < 0);
  double corr_xysum;
  SortSupportData ssup;
  ScalarItem *values;
  int values_cnt = 0;
  int *tupnoLink;
  ScalarMCVItem *track;
  int track_cnt = 0;
  int num_mcv = stats->attstattarget;
  int num_bins = stats->attstattarget;
  StdAnalyzeData *mystats = (StdAnalyzeData *)stats->extra_data;

  values = (ScalarItem *)palloc(samplerows * sizeof(ScalarItem));
  tupnoLink = (int *)palloc(samplerows * sizeof(int));
  track = (ScalarMCVItem *)palloc(num_mcv * sizeof(ScalarMCVItem));

  memset(&ssup, 0, sizeof(ssup));
  ssup.ssup_cxt = CurrentMemoryContext;
  ssup.ssup_collation = stats->attrcollid;
  ssup.ssup_nulls_first = false;

  /*
   * For now, don't perform abbreviated key conversion, because full values
   * are required for MCV slot generation.  Supporting that optimization
   * would necessitate teaching compare_scalars() to call a tie-breaker.
   */
  ssup.abbreviate = false;

  PrepareSortSupportFromOrderingOp(mystats->ltopr, &ssup);

  /* Initial scan to find sortable values */
  for (i = 0; i < samplerows; i++) {
    Datum value;
    bool isnull;

    vacuum_delay_point(true);

    value = fetchfunc(stats, i, &isnull);

    /* Check for null/nonnull */
    if (isnull) {
      null_cnt++;
      continue;
    }
    nonnull_cnt++;

    /*
     * If it's a variable-width field, add up widths for average width
     * calculation.  Note that if the value is toasted, we use the toasted
     * width.  We don't bother with this calculation if it's a fixed-width
     * type.
     */
    if (is_varlena) {
      total_width += VARSIZE_ANY(DatumGetPointer(value));

      /*
       * If the value is toasted, we want to detoast it just once to
       * avoid repeated detoastings and resultant excess memory usage
       * during the comparisons.  Also, check to see if the value is
       * excessively wide, and if so don't detoast at all --- just
       * ignore the value.
       */
      if (toast_raw_datum_size(value) > WIDTH_THRESHOLD) {
        toowide_cnt++;
        continue;
      }
      value = PointerGetDatum(PG_DETOAST_DATUM(value));
    } else if (is_varwidth) {
      /* must be cstring */
      total_width += strlen(DatumGetCString(value)) + 1;
    }

    /* Add it to the list to be sorted */
    values[values_cnt].value = value;
    values[values_cnt].tupno = values_cnt;
    tupnoLink[values_cnt] = values_cnt;
    values_cnt++;
  }

  /* We can only compute real stats if we found some sortable values. */
  if (values_cnt > 0) {
    int ndistinct, /* # distinct values in sample */
        nmultiple, /* # that appear multiple times */
        num_hist, dups_cnt;
    int slot_idx = 0;
    CompareScalarsContext cxt;

    /* Sort the collected values */
    cxt.ssup = &ssup;
    cxt.tupnoLink = tupnoLink;
    qsort_interruptible(values, values_cnt, sizeof(ScalarItem), compare_scalars,
                        &cxt);

    /*
     * Now scan the values in order, find the most common ones, and also
     * accumulate ordering-correlation statistics.
     *
     * To determine which are most common, we first have to count the
     * number of duplicates of each value.  The duplicates are adjacent in
     * the sorted list, so a brute-force approach is to compare successive
     * datum values until we find two that are not equal. However, that
     * requires N-1 invocations of the datum comparison routine, which are
     * completely redundant with work that was done during the sort.  (The
     * sort algorithm must at some point have compared each pair of items
     * that are adjacent in the sorted order; otherwise it could not know
     * that it's ordered the pair correctly.) We exploit this by having
     * compare_scalars remember the highest tupno index that each
     * ScalarItem has been found equal to.  At the end of the sort, a
     * ScalarItem's tupnoLink will still point to itself if and only if it
     * is the last item of its group of duplicates (since the group will
     * be ordered by tupno).
     */
    corr_xysum = 0;
    ndistinct = 0;
    nmultiple = 0;
    dups_cnt = 0;
    for (i = 0; i < values_cnt; i++) {
      int tupno = values[i].tupno;

      corr_xysum += ((double)i) * ((double)tupno);
      dups_cnt++;
      if (tupnoLink[tupno] == tupno) {
        /* Reached end of duplicates of this value */
        ndistinct++;
        if (dups_cnt > 1) {
          nmultiple++;
          if (track_cnt < num_mcv || dups_cnt > track[track_cnt - 1].count) {
            /*
             * Found a new item for the mcv list; find its
             * position, bubbling down old items if needed. Loop
             * invariant is that j points at an empty/ replaceable
             * slot.
             */
            int j;

            if (track_cnt < num_mcv)
              track_cnt++;
            for (j = track_cnt - 1; j > 0; j--) {
              if (dups_cnt <= track[j - 1].count)
                break;
              track[j].count = track[j - 1].count;
              track[j].first = track[j - 1].first;
            }
            track[j].count = dups_cnt;
            track[j].first = i + 1 - dups_cnt;
          }
        }
        dups_cnt = 0;
      }
    }

    stats->stats_valid = true;
    /* Do the simple null-frac and width stats */
    stats->stanullfrac = (double)null_cnt / (double)samplerows;
    if (is_varwidth)
      stats->stawidth = total_width / (double)nonnull_cnt;
    else
      stats->stawidth = stats->attrtype->typlen;

    if (nmultiple == 0) {
      /*
       * If we found no repeated non-null values, assume it's a unique
       * column; but be sure to discount for any nulls we found.
       */
      stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
    } else if (toowide_cnt == 0 && nmultiple == ndistinct) {
      /*
       * Every value in the sample appeared more than once.  Assume the
       * column has just these values.  (This case is meant to address
       * columns with small, fixed sets of possible values, such as
       * boolean or enum columns.  If there are any values that appear
       * just once in the sample, including too-wide values, we should
       * assume that that's not what we're dealing with.)
       */
      stats->stadistinct = ndistinct;
    } else {
      /*----------
       * Estimate the number of distinct values using the estimator
       * proposed by Haas and Stokes in IBM Research Report RJ 10025:
       *		n*d / (n - f1 + f1*n/N)
       * where f1 is the number of distinct values that occurred
       * exactly once in our sample of n rows (from a total of N),
       * and d is the total number of distinct values in the sample.
       * This is their Duj1 estimator; the other estimators they
       * recommend are considerably more complex, and are numerically
       * very unstable when n is much smaller than N.
       *
       * In this calculation, we consider only non-nulls.  We used to
       * include rows with null values in the n and N counts, but that
       * leads to inaccurate answers in columns with many nulls, and
       * it's intuitively bogus anyway considering the desired result is
       * the number of distinct non-null values.
       *
       * Overwidth values are assumed to have been distinct.
       *----------
       */
      int f1 = ndistinct - nmultiple + toowide_cnt;
      int d = f1 + nmultiple;
      double n = samplerows - null_cnt;
      double N = totalrows * (1.0 - stats->stanullfrac);
      double stadistinct;

      /* N == 0 shouldn't happen, but just in case ... */
      if (N > 0)
        stadistinct = (n * d) / ((n - f1) + f1 * n / N);
      else
        stadistinct = 0;

      /* Clamp to sane range in case of roundoff error */
      if (stadistinct < d)
        stadistinct = d;
      if (stadistinct > N)
        stadistinct = N;
      /* And round to integer */
      stats->stadistinct = floor(stadistinct + 0.5);
    }

    /*
     * If we estimated the number of distinct values at more than 10% of
     * the total row count (a very arbitrary limit), then assume that
     * stadistinct should scale with the row count rather than be a fixed
     * value.
     */
    if (stats->stadistinct > 0.1 * totalrows)
      stats->stadistinct = -(stats->stadistinct / totalrows);

    /*
     * Decide how many values are worth storing as most-common values. If
     * we are able to generate a complete MCV list (all the values in the
     * sample will fit, and we think these are all the ones in the table),
     * then do so.  Otherwise, store only those values that are
     * significantly more common than the values not in the list.
     *
     * Note: the first of these cases is meant to address columns with
     * small, fixed sets of possible values, such as boolean or enum
     * columns.  If we can *completely* represent the column population by
     * an MCV list that will fit into the stats target, then we should do
     * so and thus provide the planner with complete information.  But if
     * the MCV list is not complete, it's generally worth being more
     * selective, and not just filling it all the way up to the stats
     * target.
     */
    if (track_cnt == ndistinct && toowide_cnt == 0 && stats->stadistinct > 0 &&
        track_cnt <= num_mcv) {
      /* Track list includes all values seen, and all will fit */
      num_mcv = track_cnt;
    } else {
      int *mcv_counts;

      /* Incomplete list; decide how many values are worth keeping */
      if (num_mcv > track_cnt)
        num_mcv = track_cnt;

      if (num_mcv > 0) {
        mcv_counts = (int *)palloc(num_mcv * sizeof(int));
        for (i = 0; i < num_mcv; i++)
          mcv_counts[i] = track[i].count;

        num_mcv = analyze_mcv_list(mcv_counts, num_mcv, stats->stadistinct,
                                   stats->stanullfrac, samplerows, totalrows);
      }
    }

    /* Generate MCV slot entry */
    if (num_mcv > 0) {
      MemoryContext old_context;
      Datum *mcv_values;
      float4 *mcv_freqs;

      /* Must copy the target values into anl_context */
      old_context = MemoryContextSwitchTo(stats->anl_context);
      mcv_values = (Datum *)palloc(num_mcv * sizeof(Datum));
      mcv_freqs = (float4 *)palloc(num_mcv * sizeof(float4));
      for (i = 0; i < num_mcv; i++) {
        mcv_values[i] =
            datumCopy(values[track[i].first].value, stats->attrtype->typbyval,
                      stats->attrtype->typlen);
        mcv_freqs[i] = (double)track[i].count / (double)samplerows;
      }
      MemoryContextSwitchTo(old_context);

      stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
      stats->staop[slot_idx] = mystats->eqopr;
      stats->stacoll[slot_idx] = stats->attrcollid;
      stats->stanumbers[slot_idx] = mcv_freqs;
      stats->numnumbers[slot_idx] = num_mcv;
      stats->stavalues[slot_idx] = mcv_values;
      stats->numvalues[slot_idx] = num_mcv;

      /*
       * Accept the defaults for stats->statypid and others. They have
       * been set before we were called (see vacuum.h)
       */
      slot_idx++;
    }

    /*
     * Generate a histogram slot entry if there are at least two distinct
     * values not accounted for in the MCV list.  (This ensures the
     * histogram won't collapse to empty or a singleton.)
     */
    num_hist = ndistinct - num_mcv;
    if (num_hist > num_bins)
      num_hist = num_bins + 1;
    if (num_hist >= 2) {
      MemoryContext old_context;
      Datum *hist_values;
      int nvals;
      int pos, posfrac, delta, deltafrac;

      /* Sort the MCV items into position order to speed next loop */
      qsort_interruptible(track, num_mcv, sizeof(ScalarMCVItem), compare_mcvs,
                          NULL);

      /*
       * Collapse out the MCV items from the values[] array.
       *
       * Note we destroy the values[] array here... but we don't need it
       * for anything more.  We do, however, still need values_cnt.
       * nvals will be the number of remaining entries in values[].
       */
      if (num_mcv > 0) {
        int src, dest;
        int j;

        src = dest = 0;
        j = 0; /* index of next interesting MCV item */
        while (src < values_cnt) {
          int ncopy;

          if (j < num_mcv) {
            int first = track[j].first;

            if (src >= first) {
              /* advance past this MCV item */
              src = first + track[j].count;
              j++;
              continue;
            }
            ncopy = first - src;
          } else
            ncopy = values_cnt - src;
          memmove(&values[dest], &values[src], ncopy * sizeof(ScalarItem));
          src += ncopy;
          dest += ncopy;
        }
        nvals = dest;
      } else
        nvals = values_cnt;
      Assert(nvals >= num_hist);

      /* Must copy the target values into anl_context */
      old_context = MemoryContextSwitchTo(stats->anl_context);
      hist_values = (Datum *)palloc(num_hist * sizeof(Datum));

      /*
       * The object of this loop is to copy the first and last values[]
       * entries along with evenly-spaced values in between.  So the
       * i'th value is values[(i * (nvals - 1)) / (num_hist - 1)].  But
       * computing that subscript directly risks integer overflow when
       * the stats target is more than a couple thousand.  Instead we
       * add (nvals - 1) / (num_hist - 1) to pos at each step, tracking
       * the integral and fractional parts of the sum separately.
       */
      delta = (nvals - 1) / (num_hist - 1);
      deltafrac = (nvals - 1) % (num_hist - 1);
      pos = posfrac = 0;

      for (i = 0; i < num_hist; i++) {
        hist_values[i] = datumCopy(values[pos].value, stats->attrtype->typbyval,
                                   stats->attrtype->typlen);
        pos += delta;
        posfrac += deltafrac;
        if (posfrac >= (num_hist - 1)) {
          /* fractional part exceeds 1, carry to integer part */
          pos++;
          posfrac -= (num_hist - 1);
        }
      }

      MemoryContextSwitchTo(old_context);

      stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
      stats->staop[slot_idx] = mystats->ltopr;
      stats->stacoll[slot_idx] = stats->attrcollid;
      stats->stavalues[slot_idx] = hist_values;
      stats->numvalues[slot_idx] = num_hist;

      /*
       * Accept the defaults for stats->statypid and others. They have
       * been set before we were called (see vacuum.h)
       */
      slot_idx++;
    }

    /* Generate a correlation entry if there are multiple values */
    if (values_cnt > 1) {
      MemoryContext old_context;
      float4 *corrs;
      double corr_xsum, corr_x2sum;

      /* Must copy the target values into anl_context */
      old_context = MemoryContextSwitchTo(stats->anl_context);
      corrs = (float4 *)palloc(sizeof(float4));
      MemoryContextSwitchTo(old_context);

      /*----------
       * Since we know the x and y value sets are both
       *		0, 1, ..., values_cnt-1
       * we have sum(x) = sum(y) =
       *		(values_cnt-1)*values_cnt / 2
       * and sum(x^2) = sum(y^2) =
       *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
       *----------
       */
      corr_xsum = ((double)(values_cnt - 1)) * ((double)values_cnt) / 2.0;
      corr_x2sum = ((double)(values_cnt - 1)) * ((double)values_cnt) *
                   (double)(2 * values_cnt - 1) / 6.0;

      /* And the correlation coefficient reduces to */
      corrs[0] = (values_cnt * corr_xysum - corr_xsum * corr_xsum) /
                 (values_cnt * corr_x2sum - corr_xsum * corr_xsum);

      stats->stakind[slot_idx] = STATISTIC_KIND_CORRELATION;
      stats->staop[slot_idx] = mystats->ltopr;
      stats->stacoll[slot_idx] = stats->attrcollid;
      stats->stanumbers[slot_idx] = corrs;
      stats->numnumbers[slot_idx] = 1;
      slot_idx++;
    }
  } else if (nonnull_cnt > 0) {
    /* We found some non-null values, but they were all too wide */
    Assert(nonnull_cnt == toowide_cnt);
    stats->stats_valid = true;
    /* Do the simple null-frac and width stats */
    stats->stanullfrac = (double)null_cnt / (double)samplerows;
    if (is_varwidth)
      stats->stawidth = total_width / (double)nonnull_cnt;
    else
      stats->stawidth = stats->attrtype->typlen;
    /* Assume all too-wide values are distinct, so it's a unique column */
    stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
  } else if (null_cnt > 0) {
    /* We found only nulls; assume the column is entirely null */
    stats->stats_valid = true;
    stats->stanullfrac = 1.0;
    if (is_varwidth)
      stats->stawidth = 0; /* "unknown" */
    else
      stats->stawidth = stats->attrtype->typlen;
    stats->stadistinct = 0.0; /* "unknown" */
  }

  /* We don't need to bother cleaning up any of our temporary palloc's */
}

/*
 * Comparator for sorting ScalarItems
 *
 * Aside from sorting the items, we update the tupnoLink[] array
 * whenever two ScalarItems are found to contain equal datums.  The array
 * is indexed by tupno; for each ScalarItem, it contains the highest
 * tupno that that item's datum has been found to be equal to.  This allows
 * us to avoid additional comparisons in compute_scalar_stats().
 */
static int compare_scalars(const void *a, const void *b, void *arg) {
  Datum da = ((const ScalarItem *)a)->value;
  int ta = ((const ScalarItem *)a)->tupno;
  Datum db = ((const ScalarItem *)b)->value;
  int tb = ((const ScalarItem *)b)->tupno;
  CompareScalarsContext *cxt = (CompareScalarsContext *)arg;
  int compare;

  compare = ApplySortComparator(da, false, db, false, cxt->ssup);
  if (compare != 0)
    return compare;

  /*
   * The two datums are equal, so update cxt->tupnoLink[].
   */
  if (cxt->tupnoLink[ta] < tb)
    cxt->tupnoLink[ta] = tb;
  if (cxt->tupnoLink[tb] < ta)
    cxt->tupnoLink[tb] = ta;

  /*
   * For equal datums, sort by tupno
   */
  return ta - tb;
}

/*
 * Comparator for sorting ScalarMCVItems by position
 */
static int compare_mcvs(const void *a, const void *b, void *arg) {
  int da = ((const ScalarMCVItem *)a)->first;
  int db = ((const ScalarMCVItem *)b)->first;

  return da - db;
}

/*
 * Analyze the list of common values in the sample and decide how many are
 * worth storing in the table's MCV list.
 *
 * mcv_counts is assumed to be a list of the counts of the most common values
 * seen in the sample, starting with the most common.  The return value is the
 * number that are significantly more common than the values not in the list,
 * and which are therefore deemed worth storing in the table's MCV list.
 */
static int analyze_mcv_list(int *mcv_counts, int num_mcv, double stadistinct,
                            double stanullfrac, int samplerows,
                            double totalrows) {
  double ndistinct_table;
  double sumcount;
  int i;

  /*
   * If the entire table was sampled, keep the whole list.  This also
   * protects us against division by zero in the code below.
   */
  if (samplerows == totalrows || totalrows <= 1.0)
    return num_mcv;

  /* Re-extract the estimated number of distinct nonnull values in table */
  ndistinct_table = stadistinct;
  if (ndistinct_table < 0)
    ndistinct_table = -ndistinct_table * totalrows;

  /*
   * Exclude the least common values from the MCV list, if they are not
   * significantly more common than the estimated selectivity they would
   * have if they weren't in the list.  All non-MCV values are assumed to be
   * equally common, after taking into account the frequencies of all the
   * values in the MCV list and the number of nulls (c.f. eqsel()).
   *
   * Here sumcount tracks the total count of all but the last (least common)
   * value in the MCV list, allowing us to determine the effect of excluding
   * that value from the list.
   *
   * Note that we deliberately do this by removing values from the full
   * list, rather than starting with an empty list and adding values,
   * because the latter approach can fail to add any values if all the most
   * common values have around the same frequency and make up the majority
   * of the table, so that the overall average frequency of all values is
   * roughly the same as that of the common values.  This would lead to any
   * uncommon values being significantly overestimated.
   */
  sumcount = 0.0;
  for (i = 0; i < num_mcv - 1; i++)
    sumcount += mcv_counts[i];

  while (num_mcv > 0) {
    double selec, otherdistinct, N, n, K, variance, stddev;

    /*
     * Estimated selectivity the least common value would have if it
     * wasn't in the MCV list (c.f. eqsel()).
     */
    selec = 1.0 - sumcount / samplerows - stanullfrac;
    if (selec < 0.0)
      selec = 0.0;
    if (selec > 1.0)
      selec = 1.0;
    otherdistinct = ndistinct_table - (num_mcv - 1);
    if (otherdistinct > 1)
      selec /= otherdistinct;

    /*
     * If the value is kept in the MCV list, its population frequency is
     * assumed to equal its sample frequency.  We use the lower end of a
     * textbook continuity-corrected Wald-type confidence interval to
     * determine if that is significantly more common than the non-MCV
     * frequency --- specifically we assume the population frequency is
     * highly likely to be within around 2 standard errors of the sample
     * frequency, which equates to an interval of 2 standard deviations
     * either side of the sample count, plus an additional 0.5 for the
     * continuity correction.  Since we are sampling without replacement,
     * this is a hypergeometric distribution.
     *
     * XXX: Empirically, this approach seems to work quite well, but it
     * may be worth considering more advanced techniques for estimating
     * the confidence interval of the hypergeometric distribution.
     */
    N = totalrows;
    n = samplerows;
    K = N * mcv_counts[num_mcv - 1] / n;
    variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
    stddev = sqrt(variance);

    if (mcv_counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5) {
      /*
       * The value is significantly more common than the non-MCV
       * selectivity would suggest.  Keep it, and all the other more
       * common values in the list.
       */
      break;
    } else {
      /* Discard this value and consider the next least common value */
      num_mcv--;
      if (num_mcv == 0)
        break;
      sumcount -= mcv_counts[num_mcv - 1];
    }
  }
  return num_mcv;
}
