/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/pquery.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "access/smstore.h"
#include "nodes/parsenodes.h"
#include "utils/portal.h"

struct PlannedStmt; /* avoid including plannodes.h here */

extern thread_local /*LOCAL*/ PGDLLIMPORT Portal ActivePortal;

extern PortalStrategy ChoosePortalStrategy(List *stmts);

extern List *FetchPortalTargetList(Portal portal);

extern List *FetchStatementTargetList(Node *stmt);

extern void PortalStart(Portal portal, ParamListInfo params, int eflags,
                        Snapshot snapshot);

extern void PortalSetResultFormat(Portal portal, int nFormats, int16 *formats);

extern bool PortalRun(Portal portal, long count, bool isTopLevel,
                      DestReceiver *dest, DestReceiver *altdest,
                      QueryCompletion *qc);

extern uint64 PortalRunFetch(Portal portal, FetchDirection fdirection,
                             long count, DestReceiver *dest);

extern bool PlannedStmtRequiresSnapshot(struct PlannedStmt *pstmt);

extern void EnsurePortalSnapshotExists(void);

/*
 * Run run, a step of portal's executor, at the portal's view in sm
 * (PortalData's smView), if it has one, as its query's snapshot is pushed
 * around each step: so a FETCH reads as its DECLARE did.
 */
template <typename F> static inline void PortalRunInView(Portal portal, F run) {
  void *outer;

  if (portal->smView == NULL) {
    run();
    return;
  }
  outer = smstore_view_resume(portal->smView);
  PG_TRY();
  {
    run();
  }
  PG_FINALLY();
  {
    smstore_view_end(outer);
  }
  PG_END_TRY();
}

#endif /* PQUERY_H */
