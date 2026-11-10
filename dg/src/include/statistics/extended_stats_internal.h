/*-------------------------------------------------------------------------
 *
 * extended_stats_internal.h
 *	  POSTGRES extended statistics internal declarations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/include/statistics/extended_stats_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENDED_STATS_INTERNAL_H
#define EXTENDED_STATS_INTERNAL_H

#include "statistics/statistics.h"
#include "utils/sortsupport.h"

typedef struct {
  Oid eqopr;  /* '=' operator for datatype, if any */
  Oid eqfunc; /* and associated function */
  Oid ltopr;  /* '<' operator for datatype, if any */
} StdAnalyzeData;

typedef struct {
  Datum value; /* a data value */
  int tupno;   /* position index for tuple it came from */
} ScalarItem;

/* (de)serialization info */
typedef struct DimensionInfo {
  int nvalues;        /* number of deduplicated values */
  int nbytes;         /* number of bytes (serialized) */
  int nbytes_aligned; /* size of deserialized data with alignment */
  int typlen;         /* pg_type.typlen */
  bool typbyval;      /* pg_type.typbyval */
} DimensionInfo;

extern MVNDistinct *statext_ndistinct_deserialize(bytea *data);

extern MVDependencies *statext_dependencies_deserialize(bytea *data);

extern MCVList *statext_mcv_deserialize(bytea *data);

extern bool examine_opclause_args(List *args, Node **exprp, Const **cstp,
                                  bool *expronleftp);

extern Selectivity mcv_combine_selectivities(Selectivity simple_sel,
                                             Selectivity mcv_sel,
                                             Selectivity mcv_basesel,
                                             Selectivity mcv_totalsel);

extern Selectivity
mcv_clauselist_selectivity(PlannerInfo *root, StatisticExtInfo *stat,
                           List *clauses, int varRelid, JoinType jointype,
                           SpecialJoinInfo *sjinfo, RelOptInfo *rel,
                           Selectivity *basesel, Selectivity *totalsel);

extern Selectivity
mcv_clause_selectivity_or(PlannerInfo *root, StatisticExtInfo *stat,
                          MCVList *mcv, Node *clause, bool **or_matches,
                          Selectivity *basesel, Selectivity *overlap_mcvsel,
                          Selectivity *overlap_basesel, Selectivity *totalsel);

#endif /* EXTENDED_STATS_INTERNAL_H */
