/*-------------------------------------------------------------------------
 *
 * smindex.c
 *	  The index access method of an index in sm (access/smindex.h).
 *
 * An index in sm is sm's: sm writes its entries with its table's rows,
 * and smstore.c scans it.  The planner still asks an index's access method
 * what its scans can do and what they cost, and this answers for sm's: a
 * btree, ordered, unique or not, of several columns, scanned forwards a row
 * at a time or its columns alone, keeping no position, and never into a
 * bitmap: a table in sm is read by its key (../sm/CLUSTER.md section 6).
 * Its costs and its strategies are btree's.  It has no build, insert,
 * vacuum or scan function: nothing calls them on an index in sm.
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
#include "fmgr.h"
#include "utils/fmgrprotos.h"
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
 * Whether an index-only scan can return the index's column attno: every
 * column, key or INCLUDE.  smstore.c decodes the key, and reads the row
 * where the key does not give its columns back.
 */
static bool smindex_canreturn(Relation index, int attno) { return true; }

/* The comparison a btree strategy number stands for. */
static CompareType smindex_translatestrategy(StrategyNumber strategy,
                                             Oid opfamily) {
  switch (strategy) {
  case BTLessStrategyNumber:
    return COMPARE_LT;
  case BTLessEqualStrategyNumber:
    return COMPARE_LE;
  case BTEqualStrategyNumber:
    return COMPARE_EQ;
  case BTGreaterEqualStrategyNumber:
    return COMPARE_GE;
  case BTGreaterStrategyNumber:
    return COMPARE_GT;
  default:
    return COMPARE_INVALID;
  }
}

/* The btree strategy number of a comparison. */
static StrategyNumber smindex_translatecmptype(CompareType cmptype,
                                               Oid opfamily) {
  switch (cmptype) {
  case COMPARE_LT:
    return BTLessStrategyNumber;
  case COMPARE_LE:
    return BTLessEqualStrategyNumber;
  case COMPARE_EQ:
    return BTEqualStrategyNumber;
  case COMPARE_GE:
    return BTGreaterEqualStrategyNumber;
  case COMPARE_GT:
    return BTGreaterStrategyNumber;
  default:
    return InvalidStrategy;
  }
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

  amroutine->amcostestimate = btcostestimate;
  amroutine->amcanreturn = smindex_canreturn;
  amroutine->amgettuple = smindex_gettuple;
  amroutine->amtranslatestrategy = smindex_translatestrategy;
  amroutine->amtranslatecmptype = smindex_translatecmptype;

  return amroutine;
}

/*
 * The handler of pg_am's btree, the one index access method there is: an
 * index made USING btree is an index in sm.
 */
Datum bthandler(PG_FUNCTION_ARGS) { PG_RETURN_POINTER(smindex_routine()); }
