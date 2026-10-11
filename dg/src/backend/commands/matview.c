/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct {
  DestReceiver pub; /* publicly-known function pointers */
  Oid transientoid; /* OID of new heap into which to store */
  /* These fields are filled by transientrel_startup: */
  Relation transientrel; /* relation to write to */
  CommandId output_cid;  /* cmin to insert in output tuples */
  int ti_options;        /* table_tuple_insert performance options */
} DR_transientrel;

static void transientrel_startup(DestReceiver *self, int operation,
                                 TupleDesc typeinfo);
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void SetMatViewPopulatedState(Relation relation, bool newstate) {
  Relation pgrel;
  HeapTuple tuple;

  Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

  /*
   * Update relation's pg_class entry.  Crucial side-effect: other backends
   * (and this one too!) are sent SI message to make them rebuild relcache
   * entries.
   */
  pgrel = table_open(RelationRelationId, RowExclusiveLock);
  tuple =
      SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(RelationGetRelid(relation)));
  if (!HeapTupleIsValid(tuple))
    elog(ERROR, "cache lookup failed for relation %u",
         RelationGetRelid(relation));

  ((Form_pg_class)GETSTRUCT(tuple))->relispopulated = newstate;

  CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

  heap_freetuple(tuple);
  table_close(pgrel, RowExclusiveLock);

  /*
   * Advance command counter to make the updated pg_class row locally
   * visible.
   */
  CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field shows whether the clause was used.
 */
ObjectAddress ExecRefreshMatView(RefreshMatViewStmt *stmt,
                                 const char *queryString, QueryCompletion *qc) {
  Oid matviewOid;
  LOCKMODE lockmode;

  /* Determine strength of lock needed. */
  lockmode = stmt->concurrent ? ExclusiveLock : AccessExclusiveLock;

  /*
   * Get a lock until end of transaction.
   */
  matviewOid = RangeVarGetRelidExtended(stmt->relation, lockmode, 0,
                                        RangeVarCallbackMaintainsTable, NULL);

  return RefreshMatViewByOid(matviewOid, false, stmt->skipData,
                             stmt->concurrent, queryString, qc);
}

/*
 * RefreshMatViewByOid -- refresh, or first fill, the materialized view: the
 * heap's rewrite, gone with it, made a new table and swapped it in.  A table
 * in sm has no such rewrite yet, so it is refused.
 */
ObjectAddress RefreshMatViewByOid(Oid matviewOid, bool is_create, bool skipData,
                                  bool concurrent, const char *queryString,
                                  QueryCompletion *qc) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("REFRESH MATERIALIZED VIEW is not supported yet")));
  return InvalidObjectAddress; /* not reached */
}

DestReceiver *CreateTransientRelDestReceiver(Oid transientoid) {
  DR_transientrel *self = (DR_transientrel *)palloc0(sizeof(DR_transientrel));

  self->pub.receiveSlot = transientrel_receive;
  self->pub.rStartup = transientrel_startup;
  self->pub.rShutdown = transientrel_shutdown;
  self->pub.rDestroy = transientrel_destroy;
  self->pub.mydest = DestTransientRel;
  self->transientoid = transientoid;

  return (DestReceiver *)self;
}

/*
 * transientrel_startup --- executor startup
 */
static void transientrel_startup(DestReceiver *self, int operation,
                                 TupleDesc typeinfo) {
  DR_transientrel *myState = (DR_transientrel *)self;
  Relation transientrel;

  transientrel = table_open(myState->transientoid, NoLock);

  /*
   * Fill private fields of myState for use by later routines
   */
  myState->transientrel = transientrel;
  myState->output_cid = GetCurrentCommandId(true);
  myState->ti_options = TABLE_INSERT_SKIP_FSM | TABLE_INSERT_FROZEN;

  /*
   * Valid smgr_targblock implies something already wrote to the relation.
   * This may be harmless, but this function hasn't planned for it.
   */
  Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self) {
  DR_transientrel *myState = (DR_transientrel *)self;

  /*
   * Note that the input slot might not be of the type of the target
   * relation. That's supported by table_tuple_insert(), but slightly less
   * efficient than inserting with the right slot - but the alternative
   * would be to copy into a slot of the right type, which would not be
   * cheap either. This also doesn't allow accessing per-AM data (say a
   * tuple's xmin), but since we don't do that here...
   */

  table_tuple_insert(myState->transientrel, slot, myState->output_cid,
                     myState->ti_options, NULL);

  /* We know this is a newly created relation, so there are no indexes */

  return true;
}

/*
 * transientrel_shutdown --- executor end
 */
static void transientrel_shutdown(DestReceiver *self) {
  DR_transientrel *myState = (DR_transientrel *)self;

  table_finish_bulk_insert(myState->transientrel, myState->ti_options);

  /* close transientrel, but keep lock until commit */
  table_close(myState->transientrel, NoLock);
  myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void transientrel_destroy(DestReceiver *self) { pfree(self); }

/*
 * Whether DML may modify a materialized view, as REFRESH ... CONCURRENTLY's
 * own statements did: never, as there is no refresh.
 */
bool MatViewIncrementalMaintenanceIsEnabled(void) { return false; }
