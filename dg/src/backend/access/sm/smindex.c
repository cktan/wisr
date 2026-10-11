/*-------------------------------------------------------------------------
 *
 * smindex.c
 *	  The index access method of an index in sm (access/smindex.h).
 *
 * An index in sm is sm's: sm writes its entries with its table's rows,
 * and smstore.c scans it.  The planner still asks an index's access method
 * what its scans can do and what they cost, and this answers for sm's: a
 * btree, ordered, unique or not, of several columns, scanned forwards a row
 * at a time or into a bitmap of rownos, keeping no position.  Its costs and
 * its strategies are btree's.  It has no build, insert, vacuum or scan
 * function: nothing calls them on an index in sm.
 *
 * IDENTIFICATION
 *	  src/backend/access/sm/smindex.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/nbtree.h"
#include "access/smindex.h"
#include "utils/index_selfuncs.h"

/*
 * A scan through the access method: never, as smstore.c scans an index in
 * sm.  The planner takes a scan's possibility from this function's being.
 */
static bool smindex_gettuple(IndexScanDesc scan, ScanDirection dir) {
  elog(ERROR, "an index in sm is scanned through sm (access/smstore.h)");
  return false; /* not reached */
}

/*
 * A bitmap scan through the access method: never, as nodeBitmapIndexscan.c
 * has smstore.c read an index in sm into a RowBitmap.  The planner plans a
 * bitmap scan only for an access method that has this function.
 */
static int64 smindex_getbitmap(IndexScanDesc scan, TIDBitmap *tbm) {
  elog(ERROR, "an index in sm is scanned through sm (access/smstore.h)");
  return 0; /* not reached */
}

/* The access method of an index in sm, palloc'd as a handler's is. */
IndexAmRoutine *smindex_routine(void) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  amroutine->amstrategies = BTMaxStrategyNumber;
  amroutine->amsupport = BTNProcs;
  amroutine->amoptsprocnum = BTOPTIONS_PROC;
  amroutine->amcanorder = true;
  amroutine->amcanorderbyop = false;
  amroutine->amcanhash = false;
  amroutine->amconsistentequality = true;
  amroutine->amconsistentordering = true;
  /* sm's cursors run forwards, and keep no mark. */
  amroutine->amcanbackward = false;
  amroutine->amcanunique = true;
  amroutine->amcanmulticol = true;
  amroutine->amoptionalkey = true;
  /* smstore.c takes no array of values in a scan key yet. */
  amroutine->amsearcharray = false;
  amroutine->amsearchnulls = true;
  amroutine->amstorage = false;
  amroutine->amclusterable = false;
  amroutine->ampredlocks = false;
  amroutine->amcaninclude = true;
  amroutine->amusemaintenanceworkmem = false;
  amroutine->amsummarizing = false;
  amroutine->amkeytype = InvalidOid;

  /* Not yet: an index-only scan (amcanreturn). */
  amroutine->amcostestimate = btcostestimate;
  amroutine->amgettuple = smindex_gettuple;
  amroutine->amgetbitmap = smindex_getbitmap;
  amroutine->amtranslatestrategy = bttranslatestrategy;
  amroutine->amtranslatecmptype = bttranslatecmptype;

  return amroutine;
}
