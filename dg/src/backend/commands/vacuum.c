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

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
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
thread_local /*LOCAL*/ bool track_cost_delay_timing;

/*
 * Variables for cost-based vacuum delay, set from the GUCs by vacuum().  They
 * are initialized here to the defaults for client backends executing
 * ANALYZE.
 */
thread_local /*LOCAL*/ double vacuum_cost_delay = 0;
thread_local /*LOCAL*/ int vacuum_cost_limit = 200;

/* non-export function prototypes */
static List *expand_vacuum_rel(VacuumRelation *vrel, MemoryContext vac_context,
                               int options);
static List *get_all_vacuum_rels(MemoryContext vac_context, int options);

/*
 * GUC check function to ensure GUC value specified is within the allowable
 * range.
 */
bool check_vacuum_buffer_usage_limit(int *newval, void **extra,
                                     GucSource source) {
  /* Value upper and lower hard limits are inclusive */
  if (*newval == 0 || (*newval >= MIN_BAS_VAC_RING_SIZE_KB &&
                       *newval <= MAX_BAS_VAC_RING_SIZE_KB))
    return true;

  /* Value does not fall within any allowable range */
  GUC_check_errdetail("\"%s\" must be 0 or between %d kB and %d kB.",
                      "vacuum_buffer_usage_limit", MIN_BAS_VAC_RING_SIZE_KB,
                      MAX_BAS_VAC_RING_SIZE_KB);

  return false;
}

/*
 * Primary entry point for the ANALYZE command.  VACUUM, whatever its
 * options, returns at once with success: sm collects its own garbage, so
 * there is nothing for it to do.
 *
 * This is mainly a preparation wrapper for the real operations that will
 * happen in vacuum().
 */
void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel) {
  VacuumParams params;
  BufferAccessStrategy bstrategy = NULL;
  bool verbose = false;
  bool skip_locked = false;
  int ring_size;
  MemoryContext vac_context;
  MemoryContext old_context;
  ListCell *lc;

  if (vacstmt->is_vacuumcmd)
    return;

  /*
   * Set this to an invalid value so it is clear whether or not a
   * BUFFER_USAGE_LIMIT was specified when making the access strategy.
   */
  ring_size = -1;

  /* Parse options list */
  foreach (lc, vacstmt->options) {
    DefElem *opt = (DefElem *)lfirst(lc);

    if (strcmp(opt->defname, "verbose") == 0)
      verbose = defGetBoolean(opt);
    else if (strcmp(opt->defname, "skip_locked") == 0)
      skip_locked = defGetBoolean(opt);
    else if (strcmp(opt->defname, "buffer_usage_limit") == 0) {
      const char *hintmsg;
      int result;
      char *vac_buffer_size;

      vac_buffer_size = defGetString(opt);

      /*
       * Check that the specified value is valid and the size falls
       * within the hard upper and lower limits if it is not 0.
       */
      if (!parse_int(vac_buffer_size, &result, GUC_UNIT_KB, &hintmsg) ||
          (result != 0 && (result < MIN_BAS_VAC_RING_SIZE_KB ||
                           result > MAX_BAS_VAC_RING_SIZE_KB))) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("BUFFER_USAGE_LIMIT option must be 0 or between %d kB "
                        "and %d kB",
                        MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB),
                 hintmsg ? errhint("%s", _(hintmsg)) : 0));
      }

      ring_size = result;
    } else
      ereport(ERROR,
              (errcode(ERRCODE_SYNTAX_ERROR),
               errmsg("unrecognized %s option \"%s\"", "ANALYZE", opt->defname),
               parser_errposition(pstate, opt->location)));
  }

  /* Set analyze options */
  params.options = VACOPT_ANALYZE | (verbose ? VACOPT_VERBOSE : 0) |
                   (skip_locked ? VACOPT_SKIP_LOCKED : 0);

  /*
   * Create special memory context for cross-transaction storage.
   *
   * Since it is a child of PortalContext, it will go away eventually even
   * if we suffer an error; there's no need for special abort cleanup logic.
   */
  vac_context =
      AllocSetContextCreate(PortalContext, "Vacuum", ALLOCSET_DEFAULT_SIZES);

  /*
   * Make a buffer strategy object in the cross-transaction memory context.
   * If BUFFER_USAGE_LIMIT was specified by the command, it overrides the
   * value of VacuumBufferUsageLimit.  Either value may be 0, in which case
   * GetAccessStrategyWithSize() will return NULL, effectively allowing full
   * use of shared buffers.
   */
  old_context = MemoryContextSwitchTo(vac_context);
  if (ring_size == -1)
    ring_size = VacuumBufferUsageLimit;
  bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, ring_size);
  MemoryContextSwitchTo(old_context);

  /* Now go through the common routine */
  vacuum(vacstmt->rels, &params, bstrategy, vac_context, isTopLevel);

  /* Finally, clean up the vacuum memory context */
  MemoryContextDelete(vac_context);
}

/*
 * Internal entry point for the ANALYZE command.
 *
 * relations, if not NIL, is a list of VacuumRelation to process; otherwise,
 * we process all relevant tables in the database.  For each VacuumRelation,
 * if a valid OID is supplied, the table with that OID is what to process;
 * otherwise, the VacuumRelation's RangeVar indicates what to process.
 *
 * params contains a set of parameters that can be used to customize the
 * behavior.
 *
 * bstrategy may be passed in as NULL when the caller does not want to
 * restrict the number of shared_buffers that ANALYZE can use, otherwise, the
 * caller must build a BufferAccessStrategy with the number of shared_buffers
 * that ANALYZE should try to limit itself to using.
 *
 * isTopLevel should be passed down from ProcessUtility.
 *
 * It is the caller's responsibility that all parameters are allocated in a
 * memory context that will not disappear at transaction commit.
 */
void vacuum(List *relations, VacuumParams *params,
            BufferAccessStrategy bstrategy, MemoryContext vac_context,
            bool isTopLevel) {
  static thread_local /*LOCAL*/ bool in_vacuum = false;

  volatile bool in_outer_xact, use_own_xacts;

  Assert(params != NULL);

  /* ANALYZE can run either inside or outside a transaction block. */
  in_outer_xact = IsInTransactionBlock(isTopLevel);

  /*
   * Check for and disallow recursive calls.  This could happen when
   * ANALYZE calls a hostile index expression that itself calls ANALYZE.
   */
  if (in_vacuum)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("%s cannot be executed from VACUUM or ANALYZE",
                           "ANALYZE")));

  /*
   * Build list of relation(s) to process, putting any new data in
   * vac_context for safekeeping.
   */
  if (relations != NIL) {
    List *newrels = NIL;
    ListCell *lc;

    foreach (lc, relations) {
      VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
      List *sublist;
      MemoryContext old_context;

      sublist = expand_vacuum_rel(vrel, vac_context, params->options);
      old_context = MemoryContextSwitchTo(vac_context);
      newrels = list_concat(newrels, sublist);
      MemoryContextSwitchTo(old_context);
    }
    relations = newrels;
  } else
    relations = get_all_vacuum_rels(vac_context, params->options);

  /*
   * Decide whether we need to start/commit our own transactions.  If inside
   * a transaction block, we cannot.  There's no need to do so if only
   * processing one relation.  For multiple relations when not within a
   * transaction block, use own transactions so we can release locks sooner.
   */
  use_own_xacts = !in_outer_xact && list_length(relations) > 1;

  /*
   * analyze_rel expects to be entered with a transaction active.  We
   * commit the transaction started in PostgresMain() here, start one for
   * each relation, and start another one before exiting to match the commit
   * waiting for us back in PostgresMain().
   */
  if (use_own_xacts) {
    if (ActiveSnapshotSet())
      PopActiveSnapshot();

    /* matches the StartTransaction in PostgresMain() */
    CommitTransactionCommand();
  }

  /* Turn vacuum cost accounting on or off, and set/clear in_vacuum */
  PG_TRY();
  {
    ListCell *cur;

    in_vacuum = true;
    vacuum_cost_delay = VacuumCostDelay;
    vacuum_cost_limit = VacuumCostLimit;
    VacuumCostActive = (vacuum_cost_delay > 0);
    VacuumCostBalance = 0;

    /*
     * Loop to process each selected relation.
     */
    foreach (cur, relations) {
      VacuumRelation *vrel = lfirst_node(VacuumRelation, cur);

      /*
       * If using separate xacts, start one for analyze. Otherwise, we can
       * use the outer transaction.
       */
      if (use_own_xacts) {
        StartTransactionCommand();
        /* functions in indexes may want a snapshot set */
        PushActiveSnapshot(GetTransactionSnapshot());
      }

      analyze_rel(vrel->oid, vrel->relation, params, vrel->va_cols,
                  in_outer_xact, bstrategy);

      if (use_own_xacts) {
        PopActiveSnapshot();
        /* standard_ProcessUtility() does CCI if !use_own_xacts */
        CommandCounterIncrement();
        CommitTransactionCommand();
      } else {
        /*
         * If we're not using separate xacts, better separate the ANALYZE
         * actions with CCIs.  This avoids trouble if user says "ANALYZE t,
         * t".
         */
        CommandCounterIncrement();
      }
    }
  }
  PG_FINALLY();
  {
    in_vacuum = false;
    VacuumCostActive = false;
    VacuumCostBalance = 0;
  }
  PG_END_TRY();

  /*
   * This matches the CommitTransaction waiting for us in PostgresMain().
   */
  if (use_own_xacts)
    StartTransactionCommand();
}

/*
 * Check if the current user has privileges to vacuum or analyze the relation.
 * If not, issue a WARNING log message and return false to let the caller
 * decide what to do with this relation.  This routine is used to decide if a
 * relation can be processed for VACUUM or ANALYZE.
 */
bool vacuum_is_permitted_for_relation(Oid relid, Form_pg_class reltuple,
                                      bits32 options) {
  char *relname;

  Assert((options & VACOPT_ANALYZE) != 0);

  /*----------
   * A role has privileges to vacuum or analyze the relation if any of the
   * following are true:
   *   - the role owns the current database and the relation is not shared
   *   - the role has the MAINTAIN privilege on the relation
   *----------
   */
  if ((object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
       !reltuple->relisshared) ||
      pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK)
    return true;

  relname = NameStr(reltuple->relname);

  ereport(WARNING, (errmsg("permission denied to analyze \"%s\", skipping it",
                           relname)));

  return false;
}

/*
 * vacuum_open_relation
 *
 * This routine is used for attempting to open and lock a relation which
 * is going to be analyzed.  If the relation cannot be opened
 * or locked, a log is emitted if possible.
 */
Relation vacuum_open_relation(Oid relid, RangeVar *relation, bits32 options,
                              LOCKMODE lmode) {
  Relation rel;
  bool rel_lock = true;

  Assert((options & VACOPT_ANALYZE) != 0);

  /*
   * Open the relation and get the appropriate lock on it.
   *
   * There's a race condition here: the relation may have gone away since
   * the last time we saw it.  If so, we don't need to vacuum or analyze it.
   *
   * If we've been asked not to wait for the relation lock, acquire it first
   * in non-blocking mode, before calling try_relation_open().
   */
  if (!(options & VACOPT_SKIP_LOCKED))
    rel = try_relation_open(relid, lmode);
  else if (ConditionalLockRelationOid(relid, lmode))
    rel = try_relation_open(relid, NoLock);
  else {
    rel = NULL;
    rel_lock = false;
  }

  /* if relation is opened, leave */
  if (rel)
    return rel;

  /*
   * Relation could not be opened, hence generate if possible a log
   * informing on the situation.
   *
   * If the RangeVar is not defined, we do not have enough information to
   * provide a meaningful log statement.  Chances are that the caller has
   * intentionally not provided this information so that this logging is
   * skipped, anyway.
   */
  if (relation == NULL)
    return NULL;

  if (!rel_lock)
    ereport(WARNING,
            (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
             errmsg("skipping analyze of \"%s\" --- lock not available",
                    relation->relname)));
  else
    ereport(WARNING,
            (errcode(ERRCODE_UNDEFINED_TABLE),
             errmsg("skipping analyze of \"%s\" --- relation no longer exists",
                    relation->relname)));

  return NULL;
}

/*
 * Given a VacuumRelation, fill in the table OID if it wasn't specified,
 * and optionally add VacuumRelations for partitions or inheritance children.
 *
 * If a VacuumRelation does not have an OID supplied and is a partitioned
 * table, an extra entry will be added to the output for each partition.
 *
 * We take care not to modify the input data structure, but instead build
 * new VacuumRelation(s) to return.  (But note that they will reference
 * unmodified parts of the input, eg column lists.)  New data structures
 * are made in vac_context.
 */
static List *expand_vacuum_rel(VacuumRelation *vrel, MemoryContext vac_context,
                               int options) {
  List *vacrels = NIL;
  MemoryContext oldcontext;

  /* If caller supplied OID, there's nothing we need do here. */
  if (OidIsValid(vrel->oid)) {
    oldcontext = MemoryContextSwitchTo(vac_context);
    vacrels = lappend(vacrels, vrel);
    MemoryContextSwitchTo(oldcontext);
  } else {
    /*
     * Process a specific relation, and possibly partitions or child
     * tables thereof.
     */
    Oid relid;
    HeapTuple tuple;
    Form_pg_class classForm;
    bool include_children;
    int rvr_opts;

    /*
     * We transiently take AccessShareLock to protect the syscache lookup
     * below, as well as find_all_inheritors's expectation that the caller
     * holds some lock on the starting relation.
     */
    rvr_opts = (options & VACOPT_SKIP_LOCKED) ? RVR_SKIP_LOCKED : 0;
    relid = RangeVarGetRelidExtended(vrel->relation, AccessShareLock, rvr_opts,
                                     NULL, NULL);

    /*
     * If the lock is unavailable, emit the same log statement that
     * vacuum_rel() and analyze_rel() would.
     */
    if (!OidIsValid(relid)) {
      ereport(WARNING,
              (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
               errmsg("skipping analyze of \"%s\" --- lock not available",
                      vrel->relation->relname)));
      return vacrels;
    }

    /*
     * To check whether the relation is a partitioned table and its
     * ownership, fetch its syscache entry.
     */
    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple))
      elog(ERROR, "cache lookup failed for relation %u", relid);
    classForm = (Form_pg_class)GETSTRUCT(tuple);

    /*
     * Make a returnable VacuumRelation for this rel if the user has the
     * required privileges.
     */
    if (vacuum_is_permitted_for_relation(relid, classForm, options)) {
      oldcontext = MemoryContextSwitchTo(vac_context);
      vacrels = lappend(
          vacrels, makeVacuumRelation(vrel->relation, relid, vrel->va_cols));
      MemoryContextSwitchTo(oldcontext);
    }

    include_children = vrel->relation->inh;

    ReleaseSysCache(tuple);

    /*
     * Unless the user has specified ONLY, make relation list entries for
     * its partitions or inheritance child tables.  Note that the list
     * returned by find_all_inheritors() includes the passed-in OID, so we
     * have to skip that.  There's no point in taking locks on the
     * individual partitions or child tables yet, and doing so would just
     * add unnecessary deadlock risk.  For this last reason, we do not yet
     * check the ownership of the partitions/tables, which get added to
     * the list to process.  Ownership will be checked later on anyway.
     */
    if (include_children) {
      List *part_oids = find_all_inheritors(relid, NoLock, NULL);
      ListCell *part_lc;

      foreach (part_lc, part_oids) {
        Oid part_oid = lfirst_oid(part_lc);

        if (part_oid == relid)
          continue; /* ignore original table */

        /*
         * We omit a RangeVar since it wouldn't be appropriate to
         * complain about failure to open one of these relations
         * later.
         */
        oldcontext = MemoryContextSwitchTo(vac_context);
        vacrels =
            lappend(vacrels, makeVacuumRelation(NULL, part_oid, vrel->va_cols));
        MemoryContextSwitchTo(oldcontext);
      }
    }

    /*
     * Release lock again.  This means that by the time we actually try to
     * process the table, it might be gone or renamed.  In the former case
     * we'll silently ignore it; in the latter case we'll process it
     * anyway, but we must beware that the RangeVar doesn't necessarily
     * identify it anymore.  This isn't ideal, perhaps, but there's little
     * practical alternative, since we're typically going to commit this
     * transaction and begin a new one between now and then.  Moreover,
     * holding locks on multiple relations would create significant risk
     * of deadlock.
     */
    UnlockRelationOid(relid, AccessShareLock);
  }

  return vacrels;
}

/*
 * Construct a list of VacuumRelations for all vacuumable rels in
 * the current database.  The list is built in vac_context.
 */
static List *get_all_vacuum_rels(MemoryContext vac_context, int options) {
  List *vacrels = NIL;
  Relation pgclass;
  TableScanDesc scan;
  HeapTuple tuple;

  pgclass = table_open(RelationRelationId, AccessShareLock);

  scan = table_beginscan_catalog(pgclass, 0, NULL);

  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
    Form_pg_class classForm = (Form_pg_class)GETSTRUCT(tuple);
    MemoryContext oldcontext;
    Oid relid = classForm->oid;

    /*
     * We include partitioned tables here; depending on which operation is
     * to be performed, caller will decide whether to process or ignore
     * them.
     */
    if (classForm->relkind != RELKIND_RELATION &&
        classForm->relkind != RELKIND_MATVIEW &&
        classForm->relkind != RELKIND_PARTITIONED_TABLE)
      continue;

    /* check permissions of relation */
    if (!vacuum_is_permitted_for_relation(relid, classForm, options))
      continue;

    /*
     * Build VacuumRelation(s) specifying the table OIDs to be processed.
     * We omit a RangeVar since it wouldn't be appropriate to complain
     * about failure to open one of these relations later.
     */
    oldcontext = MemoryContextSwitchTo(vac_context);
    vacrels = lappend(vacrels, makeVacuumRelation(NULL, relid, NIL));
    MemoryContextSwitchTo(oldcontext);
  }

  table_endscan(scan);
  table_close(pgclass, AccessShareLock);

  return vacrels;
}

/*
 *	vac_update_relstats() -- update statistics for one relation
 *
 *		Update the whole-relation statistics that are kept in its
 *pg_class row.  There are additional stats that will be updated if we are doing
 *ANALYZE, but we always update these stats.  This routine works for both index
 *and heap relation entries in pg_class.
 *
 *		We violate transaction semantics here by overwriting the rel's
 *		existing pg_class tuple with the new values.  This is reasonably
 *		safe as long as we're sure that the new values are correct
 *whether or not this transaction commits.  The reason for doing this is that if
 *		we updated these tuples in the usual way, vacuuming pg_class
 *itself wouldn't work very well --- by the time we got done with a vacuum
 *		cycle, most of the tuples in pg_class would've been obsoleted.
 *Of course, this only works for fixed-size not-null columns, but these are.
 *
 *		Another reason for doing it this way is that when we are in a
 *lazy VACUUM and have PROC_IN_VACUUM set, we mustn't do any regular updates.
 *		Somebody vacuuming pg_class might think they could delete a
 *tuple marked with xmin = our xid.
 *
 *		In addition to fundamentally nontransactional statistics such as
 *		relpages and relallvisible, we try to maintain certain
 *lazily-updated DDL flags such as relhasindex, by clearing them if no longer
 *correct. It's safe to do this in VACUUM, which can't run in parallel with
 *		CREATE INDEX/RULE/TRIGGER and can't be part of a transaction
 *block. However, it's *not* safe to do it in an ANALYZE that's within an outer
 *transaction, because for example the current transaction might have dropped
 *the last index; then we'd think relhasindex should be cleared, but if the
 *transaction later rolls back this would be wrong. So we refrain from updating
 *the DDL flags if we're inside an outer transaction.  This is OK since
 *postponing the flag maintenance is always allowable.
 *
 *		Note: num_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 */
void vac_update_relstats(Relation relation, BlockNumber num_pages,
                         double num_tuples, BlockNumber num_all_visible_pages,
                         BlockNumber num_all_frozen_pages, bool hasindex,
                         TransactionId frozenxid, MultiXactId minmulti,
                         bool *frozenxid_updated, bool *minmulti_updated,
                         bool in_outer_xact) {
  Oid relid = RelationGetRelid(relation);
  Relation rd;
  ScanKeyData key[1];
  HeapTuple ctup;
  void *inplace_state;
  Form_pg_class pgcform;
  bool dirty, futurexid, futuremxid;
  TransactionId oldfrozenxid;
  MultiXactId oldminmulti;

  rd = table_open(RelationRelationId, RowExclusiveLock);

  /* Fetch a copy of the tuple to scribble on */
  ScanKeyInit(&key[0], Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
              ObjectIdGetDatum(relid));
  systable_inplace_update_begin(rd, ClassOidIndexId, true, NULL, 1, key, &ctup,
                                &inplace_state);
  if (!HeapTupleIsValid(ctup))
    elog(ERROR, "pg_class entry for relid %u vanished during vacuuming", relid);
  pgcform = (Form_pg_class)GETSTRUCT(ctup);

  /* Apply statistical updates, if any, to copied tuple */

  dirty = false;
  if (pgcform->relpages != (int32)num_pages) {
    pgcform->relpages = (int32)num_pages;
    dirty = true;
  }
  if (pgcform->reltuples != (float4)num_tuples) {
    pgcform->reltuples = (float4)num_tuples;
    dirty = true;
  }
  if (pgcform->relallvisible != (int32)num_all_visible_pages) {
    pgcform->relallvisible = (int32)num_all_visible_pages;
    dirty = true;
  }
  if (pgcform->relallfrozen != (int32)num_all_frozen_pages) {
    pgcform->relallfrozen = (int32)num_all_frozen_pages;
    dirty = true;
  }

  /* Apply DDL updates, but not inside an outer transaction (see above) */

  if (!in_outer_xact) {
    /*
     * If we didn't find any indexes, reset relhasindex.
     */
    if (pgcform->relhasindex && !hasindex) {
      pgcform->relhasindex = false;
      dirty = true;
    }

    /* We also clear relhasrules and relhastriggers if needed */
    if (pgcform->relhasrules && relation->rd_rules == NULL) {
      pgcform->relhasrules = false;
      dirty = true;
    }
    if (pgcform->relhastriggers && relation->trigdesc == NULL) {
      pgcform->relhastriggers = false;
      dirty = true;
    }
  }

  /*
   * Update relfrozenxid, unless caller passed InvalidTransactionId
   * indicating it has no new data.
   *
   * Ordinarily, we don't let relfrozenxid go backwards.  However, if the
   * stored relfrozenxid is "in the future" then it seems best to assume
   * it's corrupt, and overwrite with the oldest remaining XID in the table.
   */
  oldfrozenxid = pgcform->relfrozenxid;
  futurexid = false;
  if (frozenxid_updated)
    *frozenxid_updated = false;
  if (TransactionIdIsNormal(frozenxid) && oldfrozenxid != frozenxid) {
    bool update = false;

    if (TransactionIdPrecedes(oldfrozenxid, frozenxid))
      update = true;
    else if (TransactionIdPrecedes(ReadNextTransactionId(), oldfrozenxid))
      futurexid = update = true;

    if (update) {
      pgcform->relfrozenxid = frozenxid;
      dirty = true;
      if (frozenxid_updated)
        *frozenxid_updated = true;
    }
  }

  /* Similarly for relminmxid */
  oldminmulti = pgcform->relminmxid;
  futuremxid = false;
  if (minmulti_updated)
    *minmulti_updated = false;
  if (MultiXactIdIsValid(minmulti) && oldminmulti != minmulti) {
    bool update = false;

    if (MultiXactIdPrecedes(oldminmulti, minmulti))
      update = true;
    else if (MultiXactIdPrecedes(ReadNextMultiXactId(), oldminmulti))
      futuremxid = update = true;

    if (update) {
      pgcform->relminmxid = minmulti;
      dirty = true;
      if (minmulti_updated)
        *minmulti_updated = true;
    }
  }

  /* If anything changed, write out the tuple. */
  if (dirty)
    systable_inplace_update_finish(inplace_state, ctup);
  else
    systable_inplace_update_cancel(inplace_state);

  table_close(rd, RowExclusiveLock);

  if (futurexid)
    ereport(WARNING, (errcode(ERRCODE_DATA_CORRUPTED),
                      errmsg_internal("overwrote invalid relfrozenxid value %u "
                                      "with new value %u for table \"%s\"",
                                      oldfrozenxid, frozenxid,
                                      RelationGetRelationName(relation))));
  if (futuremxid)
    ereport(WARNING, (errcode(ERRCODE_DATA_CORRUPTED),
                      errmsg_internal("overwrote invalid relminmxid value %u "
                                      "with new value %u for table \"%s\"",
                                      oldminmulti, minmulti,
                                      RelationGetRelationName(relation))));
}

/*
 * Open all the vacuumable indexes of the given relation, obtaining the
 * specified kind of lock on each.  Return an array of Relation pointers for
 * the indexes into *Irel, and the number of indexes into *nindexes.
 *
 * We consider an index vacuumable if it is marked insertable (indisready).
 * If it isn't, probably a CREATE INDEX CONCURRENTLY command failed early in
 * execution, and what we have is too corrupt to be processable.  We will
 * vacuum even if the index isn't indisvalid; this is important because in a
 * unique index, uniqueness checks will be performed anyway and had better not
 * hit dangling index pointers.
 */
void vac_open_indexes(Relation relation, LOCKMODE lockmode, int *nindexes,
                      Relation **Irel) {
  List *indexoidlist;
  ListCell *indexoidscan;
  int i;

  Assert(lockmode != NoLock);

  indexoidlist = RelationGetIndexList(relation);

  /* allocate enough memory for all indexes */
  i = list_length(indexoidlist);

  if (i > 0)
    *Irel = (Relation *)palloc(i * sizeof(Relation));
  else
    *Irel = NULL;

  /* collect just the ready indexes */
  i = 0;
  foreach (indexoidscan, indexoidlist) {
    Oid indexoid = lfirst_oid(indexoidscan);
    Relation indrel;

    indrel = index_open(indexoid, lockmode);
    if (indrel->rd_index->indisready)
      (*Irel)[i++] = indrel;
    else
      index_close(indrel, lockmode);
  }

  *nindexes = i;

  list_free(indexoidlist);
}

/*
 * Release the resources acquired by vac_open_indexes.  Optionally release
 * the locks (say NoLock to keep 'em).
 */
void vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode) {
  if (Irel == NULL)
    return;

  while (nindexes--) {
    Relation ind = Irel[nindexes];

    index_close(ind, lockmode);
  }
  pfree(Irel);
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
    instr_time delay_start;

    if (msec > vacuum_cost_delay * 4)
      msec = vacuum_cost_delay * 4;

    if (track_cost_delay_timing)
      INSTR_TIME_SET_CURRENT(delay_start);

    pgstat_report_wait_start(WAIT_EVENT_VACUUM_DELAY);
    pg_usleep(msec * 1000);
    pgstat_report_wait_end();

    if (track_cost_delay_timing) {
      instr_time delay_end;
      instr_time delay;

      INSTR_TIME_SET_CURRENT(delay_end);
      INSTR_TIME_SET_ZERO(delay);
      INSTR_TIME_ACCUM_DIFF(delay, delay_end, delay_start);

      if (is_analyze)
        pgstat_progress_incr_param(PROGRESS_ANALYZE_DELAY_TIME,
                                   INSTR_TIME_GET_NANOSEC(delay));
      else
        pgstat_progress_incr_param(PROGRESS_VACUUM_DELAY_TIME,
                                   INSTR_TIME_GET_NANOSEC(delay));
    }

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
