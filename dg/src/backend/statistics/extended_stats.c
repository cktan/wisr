/*-------------------------------------------------------------------------
 *
 * extended_stats.c
 *	  POSTGRES extended statistics
 *
 * Generic code supporting statistics objects created via CREATE STATISTICS.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/extended_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

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

/*
 * Used internally to refer to an individual statistics object, i.e.,
 * a pg_statistic_ext entry.
 */
typedef struct StatExtEntry {
  Oid statOid;        /* OID of pg_statistic_ext entry */
  char *schema;       /* statistics object's schema */
  char *name;         /* statistics object's name */
  Bitmapset *columns; /* attribute numbers covered by the object */
  List *types;        /* 'char' list of enabled statistics kinds */
  int stattarget;     /* statistics target (-1 for default) */
  List *exprs;        /* expressions */
} StatExtEntry;

/*
 * statext_is_kind_built
 *		Is this stat kind built in the given pg_statistic_ext_data
 *tuple?
 */
bool statext_is_kind_built(HeapTuple htup, char type) {
  AttrNumber attnum;

  switch (type) {
  case STATS_EXT_NDISTINCT:
    attnum = Anum_pg_statistic_ext_data_stxdndistinct;
    break;

  case STATS_EXT_DEPENDENCIES:
    attnum = Anum_pg_statistic_ext_data_stxddependencies;
    break;

  case STATS_EXT_MCV:
    attnum = Anum_pg_statistic_ext_data_stxdmcv;
    break;

  case STATS_EXT_EXPRESSIONS:
    attnum = Anum_pg_statistic_ext_data_stxdexpr;
    break;

  default:
    elog(ERROR, "unexpected statistics type requested: %d", type);
  }

  return !heap_attisnull(htup, attnum, NULL);
}

/*
 * has_stats_of_kind
 *		Check whether the list contains statistic of a given kind
 */
bool has_stats_of_kind(List *stats, char requiredkind) {
  ListCell *l;

  foreach (l, stats) {
    StatisticExtInfo *stat = (StatisticExtInfo *)lfirst(l);

    if (stat->kind == requiredkind)
      return true;
  }

  return false;
}

/*
 * stat_find_expression
 *		Search for an expression in statistics object's list of
 *expressions.
 *
 * Returns the index of the expression in the statistics object's list of
 * expressions, or -1 if not found.
 */
static int stat_find_expression(StatisticExtInfo *stat, Node *expr) {
  ListCell *lc;
  int idx;

  idx = 0;
  foreach (lc, stat->exprs) {
    Node *stat_expr = (Node *)lfirst(lc);

    if (equal(stat_expr, expr))
      return idx;
    idx++;
  }

  /* Expression not found */
  return -1;
}

/*
 * stat_covers_expressions
 * 		Test whether a statistics object covers all expressions in a
 * list.
 *
 * Returns true if all expressions are covered.  If expr_idxs is non-NULL, it
 * is populated with the indexes of the expressions found.
 */
static bool stat_covers_expressions(StatisticExtInfo *stat, List *exprs,
                                    Bitmapset **expr_idxs) {
  ListCell *lc;

  foreach (lc, exprs) {
    Node *expr = (Node *)lfirst(lc);
    int expr_idx;

    expr_idx = stat_find_expression(stat, expr);
    if (expr_idx == -1)
      return false;

    if (expr_idxs != NULL)
      *expr_idxs = bms_add_member(*expr_idxs, expr_idx);
  }

  /* If we reach here, all expressions are covered */
  return true;
}

/*
 * choose_best_statistics
 *		Look for and return statistics with the specified 'requiredkind'
 *which have keys that match at least two of the given attnums.  Return NULL if
 *		there's no match.
 *
 * The current selection criteria is very simple - we choose the statistics
 * object referencing the most attributes in covered (and still unestimated
 * clauses), breaking ties in favor of objects with fewer keys overall.
 *
 * The clause_attnums is an array of bitmaps, storing attnums for individual
 * clauses. A NULL element means the clause is either incompatible or already
 * estimated.
 *
 * XXX If multiple statistics objects tie on both criteria, then which object
 * is chosen depends on the order that they appear in the stats list. Perhaps
 * further tiebreakers are needed.
 */
StatisticExtInfo *choose_best_statistics(List *stats, char requiredkind,
                                         bool inh, Bitmapset **clause_attnums,
                                         List **clause_exprs, int nclauses) {
  ListCell *lc;
  StatisticExtInfo *best_match = NULL;
  int best_num_matched = 2;                         /* goal #1: maximize */
  int best_match_keys = (STATS_MAX_DIMENSIONS + 1); /* goal #2: minimize */

  foreach (lc, stats) {
    int i;
    StatisticExtInfo *info = (StatisticExtInfo *)lfirst(lc);
    Bitmapset *matched_attnums = NULL;
    Bitmapset *matched_exprs = NULL;
    int num_matched;
    int numkeys;

    /* skip statistics that are not of the correct type */
    if (info->kind != requiredkind)
      continue;

    /* skip statistics with mismatching inheritance flag */
    if (info->inherit != inh)
      continue;

    /*
     * Collect attributes and expressions in remaining (unestimated)
     * clauses fully covered by this statistic object.
     *
     * We know already estimated clauses have both clause_attnums and
     * clause_exprs set to NULL. We leave the pointers NULL if already
     * estimated, or we reset them to NULL after estimating the clause.
     */
    for (i = 0; i < nclauses; i++) {
      Bitmapset *expr_idxs = NULL;

      /* ignore incompatible/estimated clauses */
      if (!clause_attnums[i] && !clause_exprs[i])
        continue;

      /* ignore clauses that are not covered by this object */
      if (!bms_is_subset(clause_attnums[i], info->keys) ||
          !stat_covers_expressions(info, clause_exprs[i], &expr_idxs))
        continue;

      /* record attnums and indexes of expressions covered */
      matched_attnums = bms_add_members(matched_attnums, clause_attnums[i]);
      matched_exprs = bms_add_members(matched_exprs, expr_idxs);
    }

    num_matched =
        bms_num_members(matched_attnums) + bms_num_members(matched_exprs);

    bms_free(matched_attnums);
    bms_free(matched_exprs);

    /*
     * save the actual number of keys in the stats so that we can choose
     * the narrowest stats with the most matching keys.
     */
    numkeys = bms_num_members(info->keys) + list_length(info->exprs);

    /*
     * Use this object when it increases the number of matched attributes
     * and expressions or when it matches the same number of attributes
     * and expressions but these stats have fewer keys than any previous
     * match.
     */
    if (num_matched > best_num_matched ||
        (num_matched == best_num_matched && numkeys < best_match_keys)) {
      best_match = info;
      best_num_matched = num_matched;
      best_match_keys = numkeys;
    }
  }

  return best_match;
}

/*
 * statext_is_compatible_clause_internal
 *		Determines if the clause is compatible with MCV lists.
 *
 * To be compatible, the given clause must be a combination of supported
 * clauses built from Vars or sub-expressions (where a sub-expression is
 * something that exactly matches an expression found in statistics objects).
 * This function recursively examines the clause and extracts any
 * sub-expressions that will need to be matched against statistics.
 *
 * Currently, we only support the following types of clauses:
 *
 * (a) OpExprs of the form (Var/Expr op Const), or (Const op Var/Expr), where
 * the op is one of ("=", "<", ">", ">=", "<=")
 *
 * (b) (Var/Expr IS [NOT] NULL)
 *
 * (c) combinations using AND/OR/NOT
 *
 * (d) ScalarArrayOpExprs of the form (Var/Expr op ANY (Const)) or
 * (Var/Expr op ALL (Const))
 *
 * In the future, the range of supported clauses may be expanded to more
 * complex cases, for example (Var op Var).
 *
 * Arguments:
 * clause: (sub)clause to be inspected (bare clause, not a RestrictInfo)
 * relid: rel that all Vars in clause must belong to
 * *attnums: input/output parameter collecting attribute numbers of all
 *		mentioned Vars.  Note that we do not offset the attribute
 *numbers, so we can't cope with system columns. *exprs: input/output parameter
 *collecting primitive subclauses within the clause tree *leakproof:
 *input/output parameter recording the leakproofness of the clause tree.  This
 *should be true initially, and will be set to false if any operator function
 *used in an OpExpr is not leakproof.
 *
 * Returns false if there is something we definitively can't handle.
 * On true return, we can proceed to match the *exprs against statistics.
 */
static bool statext_is_compatible_clause_internal(PlannerInfo *root,
                                                  Node *clause, Index relid,
                                                  Bitmapset **attnums,
                                                  List **exprs,
                                                  bool *leakproof) {
  /* Look inside any binary-compatible relabeling (as in examine_variable) */
  if (IsA(clause, RelabelType))
    clause = (Node *)((RelabelType *)clause)->arg;

  /* plain Var references (boolean Vars or recursive checks) */
  if (IsA(clause, Var)) {
    Var *var = (Var *)clause;

    /* Ensure var is from the correct relation */
    if (var->varno != relid)
      return false;

    /* we also better ensure the Var is from the current level */
    if (var->varlevelsup > 0)
      return false;

    /*
     * Also reject system attributes and whole-row Vars (we don't allow
     * stats on those).
     */
    if (!AttrNumberIsForUserDefinedAttr(var->varattno))
      return false;

    /* OK, record the attnum for later permissions checks. */
    *attnums = bms_add_member(*attnums, var->varattno);

    return true;
  }

  /* (Var/Expr op Const) or (Const op Var/Expr) */
  if (is_opclause(clause)) {
    OpExpr *expr = (OpExpr *)clause;
    Node *clause_expr;

    /* Only expressions with two arguments are considered compatible. */
    if (list_length(expr->args) != 2)
      return false;

    /* Check if the expression has the right shape */
    if (!examine_opclause_args(expr->args, &clause_expr, NULL, NULL))
      return false;

    /*
     * If it's not one of the supported operators ("=", "<", ">", etc.),
     * just ignore the clause, as it's not compatible with MCV lists.
     *
     * This uses the function for estimating selectivity, not the operator
     * directly (a bit awkward, but well ...).
     */
    switch (get_oprrest(expr->opno)) {
    case F_EQSEL:
    case F_NEQSEL:
    case F_SCALARLTSEL:
    case F_SCALARLESEL:
    case F_SCALARGTSEL:
    case F_SCALARGESEL:
      /* supported, will continue with inspection of the Var/Expr */
      break;

    default:
      /* other estimators are considered unknown/unsupported */
      return false;
    }

    /* Check if the operator is leakproof */
    if (*leakproof)
      *leakproof = get_func_leakproof(get_opcode(expr->opno));

    /* Check (Var op Const) or (Const op Var) clauses by recursing. */
    if (IsA(clause_expr, Var))
      return statext_is_compatible_clause_internal(root, clause_expr, relid,
                                                   attnums, exprs, leakproof);

    /* Otherwise we have (Expr op Const) or (Const op Expr). */
    *exprs = lappend(*exprs, clause_expr);
    return true;
  }

  /* Var/Expr IN Array */
  if (IsA(clause, ScalarArrayOpExpr)) {
    ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *)clause;
    Node *clause_expr;
    bool expronleft;

    /* Only expressions with two arguments are considered compatible. */
    if (list_length(expr->args) != 2)
      return false;

    /* Check if the expression has the right shape (one Var, one Const) */
    if (!examine_opclause_args(expr->args, &clause_expr, NULL, &expronleft))
      return false;

    /* We only support Var on left, Const on right */
    if (!expronleft)
      return false;

    /*
     * If it's not one of the supported operators ("=", "<", ">", etc.),
     * just ignore the clause, as it's not compatible with MCV lists.
     *
     * This uses the function for estimating selectivity, not the operator
     * directly (a bit awkward, but well ...).
     */
    switch (get_oprrest(expr->opno)) {
    case F_EQSEL:
    case F_NEQSEL:
    case F_SCALARLTSEL:
    case F_SCALARLESEL:
    case F_SCALARGTSEL:
    case F_SCALARGESEL:
      /* supported, will continue with inspection of the Var/Expr */
      break;

    default:
      /* other estimators are considered unknown/unsupported */
      return false;
    }

    /* Check if the operator is leakproof */
    if (*leakproof)
      *leakproof = get_func_leakproof(get_opcode(expr->opno));

    /* Check Var IN Array clauses by recursing. */
    if (IsA(clause_expr, Var))
      return statext_is_compatible_clause_internal(root, clause_expr, relid,
                                                   attnums, exprs, leakproof);

    /* Otherwise we have Expr IN Array. */
    *exprs = lappend(*exprs, clause_expr);
    return true;
  }

  /* AND/OR/NOT clause */
  if (is_andclause(clause) || is_orclause(clause) || is_notclause(clause)) {
    /*
     * AND/OR/NOT-clauses are supported if all sub-clauses are supported
     *
     * Perhaps we could improve this by handling mixed cases, when some of
     * the clauses are supported and some are not. Selectivity for the
     * supported subclauses would be computed using extended statistics,
     * and the remaining clauses would be estimated using the traditional
     * algorithm (product of selectivities).
     *
     * It however seems overly complex, and in a way we already do that
     * because if we reject the whole clause as unsupported here, it will
     * be eventually passed to clauselist_selectivity() which does exactly
     * this (split into supported/unsupported clauses etc).
     */
    BoolExpr *expr = (BoolExpr *)clause;
    ListCell *lc;

    foreach (lc, expr->args) {
      /*
       * If we find an incompatible clause in the arguments, treat the
       * whole clause as incompatible.
       */
      if (!statext_is_compatible_clause_internal(
              root, (Node *)lfirst(lc), relid, attnums, exprs, leakproof))
        return false;
    }

    return true;
  }

  /* Var/Expr IS NULL */
  if (IsA(clause, NullTest)) {
    NullTest *nt = (NullTest *)clause;

    /* Check Var IS NULL clauses by recursing. */
    if (IsA(nt->arg, Var))
      return statext_is_compatible_clause_internal(
          root, (Node *)(nt->arg), relid, attnums, exprs, leakproof);

    /* Otherwise we have Expr IS NULL. */
    *exprs = lappend(*exprs, nt->arg);
    return true;
  }

  /*
   * Treat any other expressions as bare expressions to be matched against
   * expressions in statistics objects.
   */
  *exprs = lappend(*exprs, clause);
  return true;
}

/*
 * statext_is_compatible_clause
 *		Determines if the clause is compatible with MCV lists.
 *
 * See statext_is_compatible_clause_internal, above, for the basic rules.
 * This layer deals with RestrictInfo superstructure and applies permissions
 * checks to verify that it's okay to examine all mentioned Vars.
 *
 * Arguments:
 * clause: clause to be inspected (in RestrictInfo form)
 * relid: rel that all Vars in clause must belong to
 * *attnums: input/output parameter collecting attribute numbers of all
 *		mentioned Vars.  Note that we do not offset the attribute
 *numbers, so we can't cope with system columns. *exprs: input/output parameter
 *collecting primitive subclauses within the clause tree
 *
 * Returns false if there is something we definitively can't handle.
 * On true return, we can proceed to match the *exprs against statistics.
 */
static bool statext_is_compatible_clause(PlannerInfo *root, Node *clause,
                                         Index relid, Bitmapset **attnums,
                                         List **exprs) {
  RestrictInfo *rinfo;
  int clause_relid;
  bool leakproof;

  /*
   * Special-case handling for bare BoolExpr AND clauses, because the
   * restrictinfo machinery doesn't build RestrictInfos on top of AND
   * clauses.
   */
  if (is_andclause(clause)) {
    BoolExpr *expr = (BoolExpr *)clause;
    ListCell *lc;

    /*
     * Check that each sub-clause is compatible.  We expect these to be
     * RestrictInfos.
     */
    foreach (lc, expr->args) {
      if (!statext_is_compatible_clause(root, (Node *)lfirst(lc), relid,
                                        attnums, exprs))
        return false;
    }

    return true;
  }

  /* Otherwise it must be a RestrictInfo. */
  if (!IsA(clause, RestrictInfo))
    return false;
  rinfo = (RestrictInfo *)clause;

  /* Pseudoconstants are not really interesting here. */
  if (rinfo->pseudoconstant)
    return false;

  /* Clauses referencing other varnos are incompatible. */
  if (!bms_get_singleton_member(rinfo->clause_relids, &clause_relid) ||
      clause_relid != relid)
    return false;

  /*
   * Check the clause, determine what attributes it references, and whether
   * it includes any non-leakproof operators.
   */
  leakproof = true;
  if (!statext_is_compatible_clause_internal(root, (Node *)rinfo->clause, relid,
                                             attnums, exprs, &leakproof))
    return false;

  /*
   * If the clause includes any non-leakproof operators, check that the user
   * has permission to read all required attributes, otherwise the operators
   * might reveal values from the MCV list that the user doesn't have
   * permission to see.  We require all rows to be selectable --- there must
   * be no securityQuals from security barrier views or RLS policies.  See
   * similar code in examine_variable(), examine_simple_variable(), and
   * statistic_proc_security_check().
   *
   * Note that for an inheritance child, the permission checks are performed
   * on the inheritance root parent, and whole-table select privilege on the
   * parent doesn't guarantee that the user could read all columns of the
   * child. Therefore we must check all referenced columns.
   */
  if (!leakproof) {
    Bitmapset *clause_attnums = NULL;
    int attnum = -1;

    /*
     * We have to check per-column privileges.  *attnums has the attnums
     * for individual Vars we saw, but there may also be Vars within
     * subexpressions in *exprs.  We can use pull_varattnos() to extract
     * those, but there's an impedance mismatch: attnums returned by
     * pull_varattnos() are offset by FirstLowInvalidHeapAttributeNumber,
     * while attnums within *attnums aren't.  Convert *attnums to the
     * offset style so we can combine the results.
     */
    while ((attnum = bms_next_member(*attnums, attnum)) >= 0) {
      clause_attnums = bms_add_member(
          clause_attnums, attnum - FirstLowInvalidHeapAttributeNumber);
    }

    /* Now merge attnums from *exprs into clause_attnums */
    if (*exprs != NIL)
      pull_varattnos((Node *)*exprs, relid, &clause_attnums);

    /* Must have permission to read all rows from these columns */
    if (!all_rows_selectable(root, relid, clause_attnums))
      return false;
  }

  /* If we reach here, the clause is OK */
  return true;
}

/*
 * statext_mcv_clauselist_selectivity
 *		Estimate clauses using the best multi-column statistics.
 *
 * Applies available extended (multi-column) statistics on a table. There may
 * be multiple applicable statistics (with respect to the clauses), in which
 * case we use greedy approach. In each round we select the best statistic on
 * a table (measured by the number of attributes extracted from the clauses
 * and covered by it), and compute the selectivity for the supplied clauses.
 * We repeat this process with the remaining clauses (if any), until none of
 * the available statistics can be used.
 *
 * One of the main challenges with using MCV lists is how to extrapolate the
 * estimate to the data not covered by the MCV list. To do that, we compute
 * not only the "MCV selectivity" (selectivities for MCV items matching the
 * supplied clauses), but also the following related selectivities:
 *
 * - simple selectivity:  Computed without extended statistics, i.e. as if the
 * columns/clauses were independent.
 *
 * - base selectivity:  Similar to simple selectivity, but is computed using
 * the extended statistic by adding up the base frequencies (that we compute
 * and store for each MCV item) of matching MCV items.
 *
 * - total selectivity: Selectivity covered by the whole MCV list.
 *
 * These are passed to mcv_combine_selectivities() which combines them to
 * produce a selectivity estimate that makes use of both per-column statistics
 * and the multi-column MCV statistics.
 *
 * 'estimatedclauses' is an input/output parameter.  We set bits for the
 * 0-based 'clauses' indexes we estimate for and also skip clause items that
 * already have a bit set.
 */
static Selectivity
statext_mcv_clauselist_selectivity(PlannerInfo *root, List *clauses,
                                   int varRelid, JoinType jointype,
                                   SpecialJoinInfo *sjinfo, RelOptInfo *rel,
                                   Bitmapset **estimatedclauses, bool is_or) {
  ListCell *l;
  Bitmapset **list_attnums; /* attnums extracted from the clause */
  List **list_exprs;        /* expressions matched to any statistic */
  int listidx;
  Selectivity sel = (is_or) ? 0.0 : 1.0;
  RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

  /* check if there's any stats that might be useful for us. */
  if (!has_stats_of_kind(rel->statlist, STATS_EXT_MCV))
    return sel;

  list_attnums =
      (Bitmapset **)palloc(sizeof(Bitmapset *) * list_length(clauses));

  /* expressions extracted from complex expressions */
  list_exprs = (List **)palloc(sizeof(Node *) * list_length(clauses));

  /*
   * Pre-process the clauses list to extract the attnums and expressions
   * seen in each item.  We need to determine if there are any clauses which
   * will be useful for selectivity estimations with extended stats.  Along
   * the way we'll record all of the attnums and expressions for each clause
   * in lists which we'll reference later so we don't need to repeat the
   * same work again.
   *
   * We also skip clauses that we already estimated using different types of
   * statistics (we treat them as incompatible).
   */
  listidx = 0;
  foreach (l, clauses) {
    Node *clause = (Node *)lfirst(l);
    Bitmapset *attnums = NULL;
    List *exprs = NIL;

    if (!bms_is_member(listidx, *estimatedclauses) &&
        statext_is_compatible_clause(root, clause, rel->relid, &attnums,
                                     &exprs)) {
      list_attnums[listidx] = attnums;
      list_exprs[listidx] = exprs;
    } else {
      list_attnums[listidx] = NULL;
      list_exprs[listidx] = NIL;
    }

    listidx++;
  }

  /* apply as many extended statistics as possible */
  while (true) {
    StatisticExtInfo *stat;
    List *stat_clauses;
    Bitmapset *simple_clauses;

    /* find the best suited statistics object for these attnums */
    stat =
        choose_best_statistics(rel->statlist, STATS_EXT_MCV, rte->inh,
                               list_attnums, list_exprs, list_length(clauses));

    /*
     * if no (additional) matching stats could be found then we've nothing
     * to do
     */
    if (!stat)
      break;

    /* Ensure choose_best_statistics produced an expected stats type. */
    Assert(stat->kind == STATS_EXT_MCV);

    /* now filter the clauses to be estimated using the selected MCV */
    stat_clauses = NIL;

    /* record which clauses are simple (single column or expression) */
    simple_clauses = NULL;

    listidx = -1;
    foreach (l, clauses) {
      /* Increment the index before we decide if to skip the clause. */
      listidx++;

      /*
       * Ignore clauses from which we did not extract any attnums or
       * expressions (this needs to be consistent with what we do in
       * choose_best_statistics).
       *
       * This also eliminates already estimated clauses - both those
       * estimated before and during applying extended statistics.
       *
       * XXX This check is needed because both bms_is_subset and
       * stat_covers_expressions return true for empty attnums and
       * expressions.
       */
      if (!list_attnums[listidx] && !list_exprs[listidx])
        continue;

      /*
       * The clause was not estimated yet, and we've extracted either
       * attnums or expressions from it. Ignore it if it's not fully
       * covered by the chosen statistics object.
       *
       * We need to check both attributes and expressions, and reject if
       * either is not covered.
       */
      if (!bms_is_subset(list_attnums[listidx], stat->keys) ||
          !stat_covers_expressions(stat, list_exprs[listidx], NULL))
        continue;

      /*
       * Now we know the clause is compatible (we have either attnums or
       * expressions extracted from it), and was not estimated yet.
       */

      /* record simple clauses (single column or expression) */
      if ((list_attnums[listidx] == NULL &&
           list_length(list_exprs[listidx]) == 1) ||
          (list_exprs[listidx] == NIL &&
           bms_membership(list_attnums[listidx]) == BMS_SINGLETON))
        simple_clauses =
            bms_add_member(simple_clauses, list_length(stat_clauses));

      /* add clause to list and mark it as estimated */
      stat_clauses = lappend(stat_clauses, (Node *)lfirst(l));
      *estimatedclauses = bms_add_member(*estimatedclauses, listidx);

      /*
       * Reset the pointers, so that choose_best_statistics knows this
       * clause was estimated and does not consider it again.
       */
      bms_free(list_attnums[listidx]);
      list_attnums[listidx] = NULL;

      list_free(list_exprs[listidx]);
      list_exprs[listidx] = NULL;
    }

    if (is_or) {
      bool *or_matches = NULL;
      Selectivity simple_or_sel = 0.0, stat_sel = 0.0;
      MCVList *mcv_list;

      /* Load the MCV list stored in the statistics object */
      mcv_list = statext_mcv_load(stat->statOid, rte->inh);

      /*
       * Compute the selectivity of the ORed list of clauses covered by
       * this statistics object by estimating each in turn and combining
       * them using the formula P(A OR B) = P(A) + P(B) - P(A AND B).
       * This allows us to use the multivariate MCV stats to better
       * estimate the individual terms and their overlap.
       *
       * Each time we iterate this formula, the clause "A" above is
       * equal to all the clauses processed so far, combined with "OR".
       */
      listidx = 0;
      foreach (l, stat_clauses) {
        Node *clause = (Node *)lfirst(l);
        Selectivity simple_sel, overlap_simple_sel, mcv_sel, mcv_basesel,
            overlap_mcvsel, overlap_basesel, mcv_totalsel, clause_sel,
            overlap_sel;

        /*
         * "Simple" selectivity of the next clause and its overlap
         * with any of the previous clauses.  These are our initial
         * estimates of P(B) and P(A AND B), assuming independence of
         * columns/clauses.
         */
        simple_sel = clause_selectivity_ext(root, clause, varRelid, jointype,
                                            sjinfo, false);

        overlap_simple_sel = simple_or_sel * simple_sel;

        /*
         * New "simple" selectivity of all clauses seen so far,
         * assuming independence.
         */
        simple_or_sel += simple_sel - overlap_simple_sel;
        CLAMP_PROBABILITY(simple_or_sel);

        /*
         * Multi-column estimate of this clause using MCV statistics,
         * along with base and total selectivities, and corresponding
         * selectivities for the overlap term P(A AND B).
         */
        mcv_sel = mcv_clause_selectivity_or(
            root, stat, mcv_list, clause, &or_matches, &mcv_basesel,
            &overlap_mcvsel, &overlap_basesel, &mcv_totalsel);

        /*
         * Combine the simple and multi-column estimates.
         *
         * If this clause is a simple single-column clause, then we
         * just use the simple selectivity estimate for it, since the
         * multi-column statistics are unlikely to improve on that
         * (and in fact could make it worse).  For the overlap, we
         * always make use of the multi-column statistics.
         */
        if (bms_is_member(listidx, simple_clauses))
          clause_sel = simple_sel;
        else
          clause_sel = mcv_combine_selectivities(simple_sel, mcv_sel,
                                                 mcv_basesel, mcv_totalsel);

        overlap_sel = mcv_combine_selectivities(
            overlap_simple_sel, overlap_mcvsel, overlap_basesel, mcv_totalsel);

        /* Factor these into the result for this statistics object */
        stat_sel += clause_sel - overlap_sel;
        CLAMP_PROBABILITY(stat_sel);

        listidx++;
      }

      /*
       * Factor the result for this statistics object into the overall
       * result.  We treat the results from each separate statistics
       * object as independent of one another.
       */
      sel = sel + stat_sel - sel * stat_sel;
    } else /* Implicitly-ANDed list of clauses */
    {
      Selectivity simple_sel, mcv_sel, mcv_basesel, mcv_totalsel, stat_sel;

      /*
       * "Simple" selectivity, i.e. without any extended statistics,
       * essentially assuming independence of the columns/clauses.
       */
      simple_sel = clauselist_selectivity_ext(root, stat_clauses, varRelid,
                                              jointype, sjinfo, false);

      /*
       * Multi-column estimate using MCV statistics, along with base and
       * total selectivities.
       */
      mcv_sel = mcv_clauselist_selectivity(root, stat, stat_clauses, varRelid,
                                           jointype, sjinfo, rel, &mcv_basesel,
                                           &mcv_totalsel);

      /* Combine the simple and multi-column estimates. */
      stat_sel = mcv_combine_selectivities(simple_sel, mcv_sel, mcv_basesel,
                                           mcv_totalsel);

      /* Factor this into the overall result */
      sel *= stat_sel;
    }
  }

  return sel;
}

/*
 * statext_clauselist_selectivity
 *		Estimate clauses using the best multi-column statistics.
 */
Selectivity statext_clauselist_selectivity(PlannerInfo *root, List *clauses,
                                           int varRelid, JoinType jointype,
                                           SpecialJoinInfo *sjinfo,
                                           RelOptInfo *rel,
                                           Bitmapset **estimatedclauses,
                                           bool is_or) {
  Selectivity sel;

  /* First, try estimating clauses using a multivariate MCV list. */
  sel = statext_mcv_clauselist_selectivity(
      root, clauses, varRelid, jointype, sjinfo, rel, estimatedclauses, is_or);

  /*
   * Functional dependencies only work for clauses connected by AND, so for
   * OR clauses we're done.
   */
  if (is_or)
    return sel;

  /*
   * Then, apply functional dependencies on the remaining clauses by calling
   * dependencies_clauselist_selectivity.  Pass 'estimatedclauses' so the
   * function can properly skip clauses already estimated above.
   *
   * The reasoning for applying dependencies last is that the more complex
   * stats can track more complex correlations between the attributes, and
   * so may be considered more reliable.
   *
   * For example, MCV list can give us an exact selectivity for values in
   * two columns, while functional dependencies can only provide information
   * about the overall strength of the dependency.
   */
  sel *= dependencies_clauselist_selectivity(root, clauses, varRelid, jointype,
                                             sjinfo, rel, estimatedclauses);

  return sel;
}

/*
 * examine_opclause_args
 *		Split an operator expression's arguments into Expr and Const
 *parts.
 *
 * Attempts to match the arguments to either (Expr op Const) or (Const op
 * Expr), possibly with a RelabelType on top. When the expression matches this
 * form, returns true, otherwise returns false.
 *
 * Optionally returns pointers to the extracted Expr/Const nodes, when passed
 * non-null pointers (exprp, cstp and expronleftp). The expronleftp flag
 * specifies on which side of the operator we found the expression node.
 */
bool examine_opclause_args(List *args, Node **exprp, Const **cstp,
                           bool *expronleftp) {
  Node *expr;
  Const *cst;
  bool expronleft;
  Node *leftop, *rightop;

  /* enforced by statext_is_compatible_clause_internal */
  Assert(list_length(args) == 2);

  leftop = (Node *)linitial(args);
  rightop = (Node *)lsecond(args);

  /* strip RelabelType from either side of the expression */
  if (IsA(leftop, RelabelType))
    leftop = (Node *)((RelabelType *)leftop)->arg;

  if (IsA(rightop, RelabelType))
    rightop = (Node *)((RelabelType *)rightop)->arg;

  if (IsA(rightop, Const)) {
    expr = (Node *)leftop;
    cst = (Const *)rightop;
    expronleft = true;
  } else if (IsA(leftop, Const)) {
    expr = (Node *)rightop;
    cst = (Const *)leftop;
    expronleft = false;
  } else
    return false;

  /* return pointers to the extracted parts if requested */
  if (exprp)
    *exprp = expr;

  if (cstp)
    *cstp = cst;

  if (expronleftp)
    *expronleftp = expronleft;

  return true;
}

/*
 * Loads pg_statistic record from expression statistics for expression
 * identified by the supplied index.
 *
 * Returns the pg_statistic record found, or NULL if there is no statistics
 * data to use.
 */
HeapTuple statext_expressions_load(Oid stxoid, bool inh, int idx) {
  bool isnull;
  Datum value;
  HeapTuple htup;
  ExpandedArrayHeader *eah;
  HeapTupleHeader td;
  HeapTupleData tmptup;
  HeapTuple tup;

  htup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(stxoid),
                         BoolGetDatum(inh));
  if (!HeapTupleIsValid(htup))
    elog(ERROR, "cache lookup failed for statistics object %u", stxoid);

  value = SysCacheGetAttr(STATEXTDATASTXOID, htup,
                          Anum_pg_statistic_ext_data_stxdexpr, &isnull);
  if (isnull)
    elog(ERROR,
         "requested statistics kind \"%c\" is not yet built for statistics "
         "object %u",
         STATS_EXT_EXPRESSIONS, stxoid);

  eah = DatumGetExpandedArray(value);

  deconstruct_expanded_array(eah);

  if (eah->dnulls && eah->dnulls[idx]) {
    /* No data found for this expression, give up. */
    ReleaseSysCache(htup);
    return NULL;
  }

  td = DatumGetHeapTupleHeader(eah->dvalues[idx]);

  /* Build a temporary HeapTuple control structure */
  tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
  ItemPointerSetInvalid(&(tmptup.t_self));
  tmptup.t_tableOid = InvalidOid;
  tmptup.t_data = td;

  tup = heap_copytuple(&tmptup);

  ReleaseSysCache(htup);

  return tup;
}

