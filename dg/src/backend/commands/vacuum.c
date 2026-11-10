/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  Control and dispatch code for the ANALYZE command.
 *
 * VACUUM compacts each table's store in sm, and ANALYZE has sm analyze it;
 * ANALYZE itself is in analyze.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
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
 * The entry point for VACUUM and ANALYZE.  VACUUM compacts each table's
 * store in sm (smstore_vacuum()), whatever else its options ask: FULL,
 * FREEZE and the rest have nothing more to do in sm.  With ANALYZE it
 * analyzes after, so the sizes recorded are the compacted store's.
 * ANALYZE is sm's (STATISTICS.md section 2.1).
 */
void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel) {
  bool verbose = false;
  bool skip_locked = false;
  bool analyze = false;
  ListCell *lc;

  /* Parse options list */
  foreach (lc, vacstmt->options) {
    DefElem *opt = (DefElem *)lfirst(lc);

    if (strcmp(opt->defname, "verbose") == 0)
      verbose = defGetBoolean(opt);
    else if (strcmp(opt->defname, "skip_locked") == 0)
      skip_locked = defGetBoolean(opt);
    else if (vacstmt->is_vacuumcmd && strcmp(opt->defname, "analyze") == 0)
      analyze = defGetBoolean(opt);
    else if (!vacstmt->is_vacuumcmd)
      ereport(ERROR,
              (errcode(ERRCODE_SYNTAX_ERROR),
               errmsg("unrecognized %s option \"%s\"", "ANALYZE", opt->defname),
               parser_errposition(pstate, opt->location)));
  }

  /* Every table is sm's: sm compacts it, then analyzes it if asked. */
  if (vacstmt->is_vacuumcmd) {
    smstore_vacuum(vacstmt->rels, skip_locked);
    if (!analyze)
      return;
  }
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

    VacuumCostBalance = 0;

    /* Might have gotten an interrupt while sleeping */
    CHECK_FOR_INTERRUPTS();
  }
}
