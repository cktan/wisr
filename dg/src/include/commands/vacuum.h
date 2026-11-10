/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	  header file for postgres vacuum cleaner and statistics analyzer
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/commands/vacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include "access/htup.h"
#include "access/genam.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "parser/parse_node.h"
#include "storage/buf.h"
#include "storage/lock.h"
#include "utils/relcache.h"

/*----------
 * ANALYZE builds one of these structs for each attribute (column) that is
 * to be analyzed.  The struct and subsidiary data are in anl_context,
 * so they live until the end of the ANALYZE operation.
 *
 * The type-specific typanalyze function is passed a pointer to this struct
 * and must return true to continue analysis, false to skip analysis of this
 * column.  In the true case it must set the compute_stats and minrows fields,
 * and can optionally set extra_data to pass additional info to compute_stats.
 * minrows is its request for the minimum number of sample rows to be gathered
 * (but note this request might not be honored, eg if there are fewer rows
 * than that in the table).
 *
 * The compute_stats routine will be called after sample rows have been
 * gathered.  Aside from this struct, it is passed:
 *		fetchfunc: a function for accessing the column values from the
 *				   sample rows
 *		samplerows: the number of sample tuples
 *		totalrows: estimated total number of rows in relation
 * The fetchfunc may be called with rownum running from 0 to samplerows-1.
 * It returns a Datum and an isNull flag.
 *
 * compute_stats should set stats_valid true if it is able to compute
 * any useful statistics.  If it does, the remainder of the struct holds
 * the information to be stored in a pg_statistic row for the column.  Be
 * careful to allocate any pointed-to data in anl_context, which will NOT
 * be CurrentMemoryContext when compute_stats is called.
 *
 * Note: all comparisons done for statistical purposes should use the
 * underlying column's collation (attcollation), except in situations
 * where a noncollatable container type contains a collatable type;
 * in that case use the type's default collation.  Be sure to record
 * the appropriate collation in stacoll.
 *----------
 */
typedef struct VacAttrStats *VacAttrStatsP;

typedef Datum (*AnalyzeAttrFetchFunc)(VacAttrStatsP stats, int rownum,
                                      bool *isNull);

typedef void (*AnalyzeAttrComputeStatsFunc)(VacAttrStatsP stats,
                                            AnalyzeAttrFetchFunc fetchfunc,
                                            int samplerows, double totalrows);

typedef struct VacAttrStats {
  /*
   * These fields are set up by the main ANALYZE code before invoking the
   * type-specific typanalyze function.  They don't necessarily match what
   * is in pg_attribute, because some index opclasses store a different type
   * than the underlying column/expression.  Therefore, use these fields for
   * information about the datatype being fed to the typanalyze function.
   */
  int attstattarget;         /* -1 to use default */
  Oid attrtypid;             /* type of data being analyzed */
  int32 attrtypmod;          /* typmod of data being analyzed */
  Form_pg_type attrtype;     /* copy of pg_type row for attrtypid */
  Oid attrcollid;            /* collation of data being analyzed */
  MemoryContext anl_context; /* where to save long-lived data */

  /*
   * These fields must be filled in by the typanalyze routine, unless it
   * returns false.
   */
  AnalyzeAttrComputeStatsFunc compute_stats; /* function pointer */
  int minrows;      /* Minimum # of rows wanted for stats */
  void *extra_data; /* for extra type-specific data */

  /*
   * These fields are to be filled in by the compute_stats routine. (They
   * are initialized to zero when the struct is created.)
   */
  bool stats_valid;
  float4 stanullfrac; /* fraction of entries that are NULL */
  int32 stawidth;     /* average width of column values */
  float4 stadistinct; /* # distinct values */
  int16 stakind[STATISTIC_NUM_SLOTS];
  Oid staop[STATISTIC_NUM_SLOTS];
  Oid stacoll[STATISTIC_NUM_SLOTS];
  int numnumbers[STATISTIC_NUM_SLOTS];
  float4 *stanumbers[STATISTIC_NUM_SLOTS];
  int numvalues[STATISTIC_NUM_SLOTS];
  Datum *stavalues[STATISTIC_NUM_SLOTS];

  /*
   * These fields describe the stavalues[n] element types. They will be
   * initialized to match attrtypid, but a custom typanalyze function might
   * want to store an array of something other than the analyzed column's
   * elements. It should then overwrite these fields.
   */
  Oid statypid[STATISTIC_NUM_SLOTS];
  int16 statyplen[STATISTIC_NUM_SLOTS];
  bool statypbyval[STATISTIC_NUM_SLOTS];
  char statypalign[STATISTIC_NUM_SLOTS];

  /*
   * These fields are private to the main ANALYZE code and should not be
   * looked at by type-specific functions.
   */
  int tupattnum;   /* attribute number within tuples */
  HeapTuple *rows; /* access info for std fetch function */
  TupleDesc tupDesc;
  Datum *exprvals; /* access info for index fetch function */
  bool *exprnulls;
  int rowstride;
} VacAttrStats;

/* flag bits for VacuumParams->options */
#define VACOPT_ANALYZE 0x02     /* do ANALYZE */
#define VACOPT_VERBOSE 0x04     /* output INFO instrumentation messages */
#define VACOPT_SKIP_LOCKED 0x20 /* skip if cannot get lock */

/*
 * Parameters customizing behavior of ANALYZE.  VACOPT_ANALYZE is always
 * set in options.
 */
typedef struct VacuumParams {
  bits32 options; /* bitmask of VACOPT_* */
} VacuumParams;

/*
 * VacuumCutoffs is immutable state that describes the cutoffs used by VACUUM.
 * Established at the beginning of each VACUUM operation.
 */
struct VacuumCutoffs {
  /*
   * Existing pg_class fields at start of VACUUM
   */
  TransactionId relfrozenxid;
  MultiXactId relminmxid;

  /*
   * OldestXmin is the Xid below which tuples deleted by any xact (that
   * committed) should be considered DEAD, not just RECENTLY_DEAD.
   *
   * OldestMxact is the Mxid below which MultiXacts are definitely not seen
   * as visible by any running transaction.
   *
   * OldestXmin and OldestMxact are also the most recent values that can
   * ever be passed to vac_update_relstats() as frozenxid and minmulti
   * arguments at the end of VACUUM.  These same values should be passed
   * when it turns out that VACUUM will leave no unfrozen XIDs/MXIDs behind
   * in the table.
   */
  TransactionId OldestXmin;
  MultiXactId OldestMxact;

  /*
   * FreezeLimit is the Xid below which all Xids are definitely frozen or
   * removed in pages VACUUM scans and cleanup locks.
   *
   * MultiXactCutoff is the value below which all MultiXactIds are
   * definitely removed from Xmax in pages VACUUM scans and cleanup locks.
   */
  TransactionId FreezeLimit;
  MultiXactId MultiXactCutoff;
};

/* GUC parameters */
extern thread_local /*LOCAL*/ PGDLLIMPORT int
    default_statistics_target; /* PGDLLIMPORT for PostGIS */

/*
 * Maximum value for default_statistics_target and per-column statistics
 * targets.  This is fairly arbitrary, mainly to prevent users from creating
 * unreasonably large statistics that the system cannot handle well.
 */
#define MAX_STATISTICS_TARGET 10000

extern thread_local /*LOCAL*/ PGDLLIMPORT double vacuum_cost_delay;
extern thread_local /*LOCAL*/ PGDLLIMPORT int vacuum_cost_limit;

/* in commands/vacuum.c */
extern void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt,
                       bool isTopLevel);
extern void vacuum_delay_point(bool is_analyze);

/* in commands/analyze.c */
extern bool std_typanalyze(VacAttrStats *stats);

/* in utils/misc/sampling.c --- duplicate of declarations in utils/sampling.h */
extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif /* VACUUM_H */
