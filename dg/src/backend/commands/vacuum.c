/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  Control and dispatch code for the ANALYZE command.
 *
 * VACUUM is accepted and does nothing: sm collects its own garbage.
 * ANALYZE itself is in analyze.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/smstore.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * GUC parameters
 */

/*
 * Variables for cost-based vacuum delay, set from the GUCs by vacuum().  They
 * are initialized here to the defaults for client backends executing
 * ANALYZE.
 */
thread_local /*LOCAL*/ double vacuum_cost_delay = 0;
thread_local /*LOCAL*/ int vacuum_cost_limit = 200;

/*
 * The entry point for VACUUM and ANALYZE.  VACUUM, whatever its options,
 * returns at once with success: sm collects its own garbage, so there is
 * nothing for it to do.  ANALYZE is sm's (STATISTICS.md section 2.1).
 */
void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel) {
  bool verbose = false;
  bool skip_locked = false;
  ListCell *lc;

  if (vacstmt->is_vacuumcmd)
    return;

  /* Parse options list */
  foreach (lc, vacstmt->options) {
    DefElem *opt = (DefElem *)lfirst(lc);

    if (strcmp(opt->defname, "verbose") == 0)
      verbose = defGetBoolean(opt);
    else if (strcmp(opt->defname, "skip_locked") == 0)
      skip_locked = defGetBoolean(opt);
    else
      ereport(ERROR,
              (errcode(ERRCODE_SYNTAX_ERROR),
               errmsg("unrecognized %s option \"%s\"", "ANALYZE", opt->defname),
               parser_errposition(pstate, opt->location)));
  }

  /* Every table is sm's, and sm analyzes it. */
  smstore_analyze(vacstmt->rels, verbose, skip_locked);
}

/*
 * vacuum_delay_point --- check for interrupts and cost-based delay.
 *
 * This should be called in each major loop of VACUUM processing,
 * typically once per page processed.
 */
void vacuum_delay_point(bool is_analyze) {
  double msec = 0;

  /* Always check for interrupts */
  CHECK_FOR_INTERRUPTS();

  if (InterruptPending || !VacuumCostActive)
    return;

  if (VacuumCostBalance >= vacuum_cost_limit)
    msec = vacuum_cost_delay * VacuumCostBalance / vacuum_cost_limit;

  /* Nap if appropriate */
  if (msec > 0) {
    if (msec > vacuum_cost_delay * 4)
      msec = vacuum_cost_delay * 4;

    pg_usleep(msec * 1000);

    /*
     * We don't want to ignore postmaster death during very long vacuums
     * with vacuum_cost_delay configured.  We can't use the usual
     * WaitLatch() approach here because we want microsecond-based sleep
     * durations above.
     */
    if (IsUnderPostmaster && !PostmasterIsAlive())
      exit(1);

    VacuumCostBalance = 0;

    /* Might have gotten an interrupt while sleeping */
    CHECK_FOR_INTERRUPTS();
  }
}
