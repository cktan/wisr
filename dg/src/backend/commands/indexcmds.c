/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES define and remove index code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/indexcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/cat_builtin.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "parser/parse_utilcmd.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/pg_rusage.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* non-export function prototypes */
static bool CompareOpclassOptions(const Datum *opts1, const Datum *opts2,
                                  int natts);
static void CheckPredicate(Expr *predicate);
static void
ComputeIndexAttrs(IndexInfo *indexInfo, Oid *typeOids, Oid *collationOids,
                  Oid *opclassOids, Datum *opclassOptions, int16 *colOptions,
                  const List *attList, const List *exclusionOpNames, Oid relId,
                  const char *accessMethodName, Oid accessMethodId,
                  bool amcanorder, bool isconstraint, bool iswithoutoverlaps,
                  Oid ddl_userid, int ddl_sec_context, int *ddl_save_nestlevel);
static char *ChooseIndexNameAddition(const List *colnames);
static void update_relispartition(Oid relationId, bool newval);
static inline void set_indexsafe_procflags(void);

/*
 * callback argument type for RangeVarCallbackForReindexIndex()
 */
struct ReindexIndexCallbackState {
  ReindexParams params; /* options from statement */
  Oid locked_table_oid; /* tracks previously locked table */
};

/*
 * callback arguments for reindex_error_callback()
 */
typedef struct ReindexErrorInfo {
  char *relname;
  char *relnamespace;
  char relkind;
} ReindexErrorInfo;

/*
 * CheckIndexCompatible
 *		Determine whether an existing index definition is compatible
 *with a prospective index definition, such that the existing index storage
 *		could become the storage of the new index, avoiding a rebuild.
 *
 * 'oldId': the OID of the existing index
 * 'accessMethodName': name of the AM to use.
 * 'attributeList': a list of IndexElem specifying columns and expressions
 *		to index on.
 * 'exclusionOpNames': list of names of exclusion-constraint operators,
 *		or NIL if not an exclusion constraint.
 * 'isWithoutOverlaps': true iff this index has a WITHOUT OVERLAPS clause.
 *
 * This is tailored to the needs of ALTER TABLE ALTER TYPE, which recreates
 * any indexes that depended on a changing column from their pg_get_indexdef
 * or pg_get_constraintdef definitions.  We omit some of the sanity checks of
 * DefineIndex.  We assume that the old and new indexes have the same number
 * of columns and that if one has an expression column or predicate, both do.
 * Errors arising from the attribute list still apply.
 *
 * Most column type changes that can skip a table rewrite do not invalidate
 * indexes.  We acknowledge this when all operator classes, collations and
 * exclusion operators match.  Though we could further permit intra-opfamily
 * changes for btree and hash indexes, that adds subtle complexity with no
 * concrete benefit for core types. Note, that INCLUDE columns aren't
 * checked by this function, for them it's enough that table rewrite is
 * skipped.
 *
 * When a comparison or exclusion operator has a polymorphic input type, the
 * actual input types must also match.  This defends against the possibility
 * that operators could vary behavior in response to get_fn_expr_argtype().
 * At present, this hazard is theoretical: check_exclusion_constraint() and
 * all core index access methods decline to set fn_expr for such calls.
 *
 * We do not yet implement a test to verify compatibility of expression
 * columns or predicates, so assume any such index is incompatible.
 */
bool CheckIndexCompatible(Oid oldId, const char *accessMethodName,
                          const List *attributeList,
                          const List *exclusionOpNames,
                          bool isWithoutOverlaps) {
  bool isconstraint;
  Oid *typeIds;
  Oid *collationIds;
  Oid *opclassIds;
  Datum *opclassOptions;
  Oid accessMethodId;
  Oid relationId;
  HeapTuple tuple;
  Form_pg_index indexForm;
  Form_pg_am accessMethodForm;
  IndexAmRoutine *amRoutine;
  bool amcanorder;
  bool amsummarizing;
  int16 *coloptions;
  IndexInfo *indexInfo;
  int numberOfAttributes;
  int old_natts;
  bool ret = true;
  oidvector *old_indclass;
  oidvector *old_indcollation;
  Relation irel;
  int i;
  Datum d;

  /* Caller should already have the relation locked in some way. */
  relationId = IndexGetRelation(oldId, false);

  /*
   * We can pretend isconstraint = false unconditionally.  It only serves to
   * decide the text of an error message that should never happen for us.
   */
  isconstraint = false;

  numberOfAttributes = list_length(attributeList);
  Assert(numberOfAttributes > 0);
  Assert(numberOfAttributes <= INDEX_MAX_KEYS);

  /* look up the access method */
  tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
  if (!HeapTupleIsValid(tuple))
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("access method \"%s\" does not exist", accessMethodName)));
  accessMethodForm = (Form_pg_am)GETSTRUCT(tuple);
  accessMethodId = accessMethodForm->oid;
  amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);
  ReleaseSysCache(tuple);

  amcanorder = amRoutine->amcanorder;
  amsummarizing = amRoutine->amsummarizing;

  /*
   * Compute the operator classes, collations, and exclusion operators for
   * the new index, so we can test whether it's compatible with the existing
   * one.  Note that ComputeIndexAttrs might fail here, but that's OK:
   * DefineIndex would have failed later.  Our attributeList contains only
   * key attributes, thus we're filling ii_NumIndexAttrs and
   * ii_NumIndexKeyAttrs with same value.
   */
  indexInfo = makeIndexInfo(numberOfAttributes, numberOfAttributes,
                            accessMethodId, NIL, NIL, false, false, false,
                            false, amsummarizing, isWithoutOverlaps);
  typeIds = palloc_array(Oid, numberOfAttributes);
  collationIds = palloc_array(Oid, numberOfAttributes);
  opclassIds = palloc_array(Oid, numberOfAttributes);
  opclassOptions = palloc_array(Datum, numberOfAttributes);
  coloptions = palloc_array(int16, numberOfAttributes);
  ComputeIndexAttrs(indexInfo, typeIds, collationIds, opclassIds,
                    opclassOptions, coloptions, attributeList, exclusionOpNames,
                    relationId, accessMethodName, accessMethodId, amcanorder,
                    isconstraint, isWithoutOverlaps, InvalidOid, 0, NULL);

  /* Get the soon-obsolete pg_index tuple. */
  tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldId));
  if (!HeapTupleIsValid(tuple))
    elog(ERROR, "cache lookup failed for index %u", oldId);
  indexForm = (Form_pg_index)GETSTRUCT(tuple);

  /*
   * We don't assess expressions or predicates; assume incompatibility.
   * Also, if the index is invalid for any reason, treat it as incompatible.
   */
  if (!(heap_attisnull(tuple, Anum_pg_index_indpred, NULL) &&
        heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) &&
        indexForm->indisvalid)) {
    ReleaseSysCache(tuple);
    return false;
  }

  /* Any change in operator class or collation breaks compatibility. */
  old_natts = indexForm->indnkeyatts;
  Assert(old_natts == numberOfAttributes);

  d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indcollation);
  old_indcollation = (oidvector *)DatumGetPointer(d);

  d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indclass);
  old_indclass = (oidvector *)DatumGetPointer(d);

  ret =
      (memcmp(old_indclass->values, opclassIds, old_natts * sizeof(Oid)) == 0 &&
       memcmp(old_indcollation->values, collationIds,
              old_natts * sizeof(Oid)) == 0);

  ReleaseSysCache(tuple);

  if (!ret)
    return false;

  /* For polymorphic opcintype, column type changes break compatibility. */
  irel = index_open(oldId, AccessShareLock); /* caller probably has a lock */
  for (i = 0; i < old_natts; i++) {
    if (IsPolymorphicType(get_opclass_input_type(opclassIds[i])) &&
        TupleDescAttr(irel->rd_att, i)->atttypid != typeIds[i]) {
      ret = false;
      break;
    }
  }

  /* Any change in opclass options break compatibility. */
  if (ret) {
    Datum *oldOpclassOptions = palloc_array(Datum, old_natts);

    for (i = 0; i < old_natts; i++)
      oldOpclassOptions[i] = get_attoptions(oldId, i + 1);

    ret = CompareOpclassOptions(oldOpclassOptions, opclassOptions, old_natts);

    pfree(oldOpclassOptions);
  }

  /* Any change in exclusion operator selections breaks compatibility. */
  if (ret && indexInfo->ii_ExclusionOps != NULL) {
    Oid *old_operators, *old_procs;
    uint16 *old_strats;

    RelationGetExclusionInfo(irel, &old_operators, &old_procs, &old_strats);
    ret = memcmp(old_operators, indexInfo->ii_ExclusionOps,
                 old_natts * sizeof(Oid)) == 0;

    /* Require an exact input type match for polymorphic operators. */
    if (ret) {
      for (i = 0; i < old_natts && ret; i++) {
        Oid left, right;

        op_input_types(indexInfo->ii_ExclusionOps[i], &left, &right);
        if ((IsPolymorphicType(left) || IsPolymorphicType(right)) &&
            TupleDescAttr(irel->rd_att, i)->atttypid != typeIds[i]) {
          ret = false;
          break;
        }
      }
    }
  }

  index_close(irel, NoLock);
  return ret;
}

/*
 * CompareOpclassOptions
 *
 * Compare per-column opclass options which are represented by arrays of text[]
 * datums.  Both elements of arrays and array themselves can be NULL.
 */
static bool CompareOpclassOptions(const Datum *opts1, const Datum *opts2,
                                  int natts) {
  int i;
  FmgrInfo fm;

  if (!opts1 && !opts2)
    return true;

  fmgr_info(F_ARRAY_EQ, &fm);
  for (i = 0; i < natts; i++) {
    Datum opt1 = opts1 ? opts1[i] : (Datum)0;
    Datum opt2 = opts2 ? opts2[i] : (Datum)0;

    if (opt1 == (Datum)0) {
      if (opt2 == (Datum)0)
        continue;
      else
        return false;
    } else if (opt2 == (Datum)0)
      return false;

    /*
     * Compare non-NULL text[] datums.  Use C collation to enforce binary
     * equivalence of texts, because we don't know anything about the
     * semantics of opclass options.
     */
    if (!DatumGetBool(FunctionCall2Coll(&fm, C_COLLATION_OID, opt1, opt2)))
      return false;
  }

  return true;
}

/*
 * WaitForOlderSnapshots
 *
 * Wait for transactions that might have an older snapshot than the given xmin
 * limit, because it might not contain tuples deleted just before it has
 * been taken. Obtain a list of VXIDs of such transactions, and wait for them
 * individually. This is used when building an index concurrently.
 *
 * We can exclude any running transactions that have xmin > the xmin given;
 * their oldest snapshot must be newer than our xmin limit.
 * We can also exclude any transactions that have xmin = zero, since they
 * evidently have no live snapshot at all (and any one they might be in
 * process of taking is certainly newer than ours).  Transactions in other
 * DBs can be ignored too, since they'll never even be able to see the
 * index being worked on.
 *
 * We can also exclude autovacuum processes and processes running manual
 * lazy VACUUMs, because they won't be fazed by missing index entries
 * either.  (Manual ANALYZEs, however, can't be excluded because they
 * might be within transactions that are going to do arbitrary operations
 * later.)  Processes running CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY
 * on indexes that are neither expressional nor partial are also safe to
 * ignore, since we know that those processes won't examine any data
 * outside the table they're indexing.
 *
 * Also, GetCurrentVirtualXIDs never reports our own vxid, so we need not
 * check for that.
 *
 * If a process goes idle-in-transaction with xmin zero, we do not need to
 * wait for it anymore, per the above argument.  We do not have the
 * infrastructure right now to stop waiting if that happens, but we can at
 * least avoid the folly of waiting when it is idle at the time we would
 * begin to wait.  We do this by repeatedly rechecking the output of
 * GetCurrentVirtualXIDs.  If, during any iteration, a particular vxid
 * doesn't show up in the output, we know we can forget about it.
 */
void WaitForOlderSnapshots(TransactionId limitXmin) {
  /*
   * Nothing to wait for: what a session sees is sm's snapshot, and
   * PostgreSQL's is a constant that holds back no one (dg/SHMEM.md section
   * 5), so no virtual xid is waited for.
   */
}

/*
 * DefineIndex
 *		Creates a new index.
 *
 * This function manages the current userid according to the needs of pg_dump.
 * Recreating old-database catalog entries in new-database is fine, regardless
 * of which users would have permission to recreate those entries now.  That's
 * just preservation of state.  Running opaque expressions, like calling a
 * function named in a catalog entry or evaluating a pg_node_tree in a catalog
 * entry, as anyone other than the object owner, is not fine.  To adhere to
 * those principles and to remain fail-safe, use the table owner userid for
 * most ACL checks.  Use the original userid for ACL checks reached without
 * traversing opaque expressions.  (pg_dump can predict such ACL checks from
 * catalogs.)  Overall, this is a mess.  Future DDL development should
 * consider offering one DDL command for catalog setup and a separate DDL
 * command for steps that run opaque expressions.
 *
 * 'tableId': the OID of the table relation on which the index is to be
 *		created
 * 'stmt': IndexStmt describing the properties of the new index.
 * 'indexRelationId': normally InvalidOid, but during bootstrap can be
 *		nonzero to specify a preselected OID for the index.
 * 'parentIndexId': the OID of the parent index; InvalidOid if not the child
 *		of a partitioned index.
 * 'parentConstraintId': the OID of the parent constraint; InvalidOid if not
 *		the child of a constraint (only used when recursing)
 * 'total_parts': total number of direct and indirect partitions of relation;
 *		pass -1 if not known or rel is not partitioned.
 * 'is_alter_table': this is due to an ALTER rather than a CREATE operation.
 * 'check_rights': check for CREATE rights in namespace and tablespace.  (This
 *		should be true except when ALTER is deleting/recreating an
 *index.) 'check_not_in_use': check for table not already in use in current
 *session. This should be true unless caller is holding the table open, in which
 *		case the caller had better have checked it earlier.
 * 'skip_build': make the catalog entries but don't create the index files
 * 'quiet': suppress the NOTICE chatter ordinarily provided for constraints.
 *
 * Returns the object address of the created index.
 */
ObjectAddress DefineIndex(Oid tableId, IndexStmt *stmt, Oid indexRelationId,
                          Oid parentIndexId, Oid parentConstraintId,
                          int total_parts, bool is_alter_table,
                          bool check_rights, bool check_not_in_use,
                          bool skip_build, bool quiet) {
  bool concurrent;
  char *indexRelationName;
  char *accessMethodName;
  Oid *typeIds;
  Oid *collationIds;
  Oid *opclassIds;
  Datum *opclassOptions;
  Oid accessMethodId;
  Oid namespaceId;
  Oid tablespaceId;
  Oid createdConstraintId = InvalidOid;
  List *indexColNames;
  List *allIndexParams;
  Relation rel;
  HeapTuple tuple;
  Form_pg_am accessMethodForm;
  IndexAmRoutine *amRoutine;
  bool amcanorder;
  bool amissummarizing;
  amoptions_function amoptions;
  bool exclusion;
  bool partitioned;
  bool safe_index;
  Datum reloptions;
  int16 *coloptions;
  IndexInfo *indexInfo;
  bits16 flags;
  bits16 constr_flags;
  int numberOfAttributes;
  int numberOfKeyAttributes;
  TransactionId limitXmin;
  ObjectAddress address;
  LockRelId heaprelid;
  LOCKMODE lockmode;
  Snapshot snapshot;
  Oid root_save_userid;
  int root_save_sec_context;
  int root_save_nestlevel;

  root_save_nestlevel = NewGUCNestLevel();

  RestrictSearchPath();

  /*
   * Some callers need us to run with an empty default_tablespace; this is a
   * necessary hack to be able to reproduce catalog state accurately when
   * recreating indexes after table-rewriting ALTER TABLE.
   */
  if (stmt->reset_default_tblspc)
    (void)set_config_option("default_tablespace", "", PGC_USERSET,
                            PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);

  /*
   * Force non-concurrent build on temporary relations, even if CONCURRENTLY
   * was requested.  Other backends can't access a temporary relation, so
   * there's no harm in grabbing a stronger lock, and a non-concurrent DROP
   * is more efficient.  Do this before any use of the concurrent option is
   * done.
   */
  if (stmt->concurrent && get_rel_persistence(tableId) != RELPERSISTENCE_TEMP)
    concurrent = true;
  else
    concurrent = false;

  /*
   * Start progress report.  If we're building a partition, this was already
   * done.
   */
  if (!OidIsValid(parentIndexId)) {
  }

  /*
   * count key attributes in index
   */
  numberOfKeyAttributes = list_length(stmt->indexParams);

  /*
   * Calculate the new list of index columns including both key columns and
   * INCLUDE columns.  Later we can determine which of these are key
   * columns, and which are just part of the INCLUDE list by checking the
   * list position.  A list item in a position less than ii_NumIndexKeyAttrs
   * is part of the key columns, and anything equal to and over is part of
   * the INCLUDE columns.
   */
  allIndexParams =
      list_concat_copy(stmt->indexParams, stmt->indexIncludingParams);
  numberOfAttributes = list_length(allIndexParams);

  if (numberOfKeyAttributes <= 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                    errmsg("must specify at least one column")));
  if (numberOfAttributes > INDEX_MAX_KEYS)
    ereport(ERROR, (errcode(ERRCODE_TOO_MANY_COLUMNS),
                    errmsg("cannot use more than %d columns in an index",
                           INDEX_MAX_KEYS)));

  /*
   * Only SELECT ... FOR UPDATE/SHARE are allowed while doing a standard
   * index build; but for concurrent builds we allow INSERT/UPDATE/DELETE
   * (but not VACUUM).
   *
   * NB: Caller is responsible for making sure that tableId refers to the
   * relation on which the index should be built; except in bootstrap mode,
   * this will typically require the caller to have already locked the
   * relation.  To avoid lock upgrade hazards, that lock should be at least
   * as strong as the one we take here.
   *
   * NB: If the lock strength here ever changes, code that is run by
   * parallel workers under the control of certain particular ambuild
   * functions will need to be updated, too.
   */
  lockmode = concurrent ? ShareUpdateExclusiveLock : ShareLock;
  rel = table_open(tableId, lockmode);

  /*
   * Switch to the table owner's userid, so that any index functions are run
   * as that user.  Also lock down security-restricted operations.  We
   * already arranged to make GUC variable changes local to this command.
   */
  GetUserIdAndSecContext(&root_save_userid, &root_save_sec_context);
  SetUserIdAndSecContext(rel->rd_rel->relowner,
                         root_save_sec_context | SECURITY_RESTRICTED_OPERATION);

  namespaceId = RelationGetNamespace(rel);

  /*
   * It has exclusion constraint behavior if it's an EXCLUDE constraint or a
   * temporal PRIMARY KEY/UNIQUE constraint
   */
  exclusion = stmt->excludeOpNames || stmt->iswithoutoverlaps;

  /* Ensure that it makes sense to index this kind of relation */
  switch (rel->rd_rel->relkind) {
  case RELKIND_RELATION:
  case RELKIND_MATVIEW:
  case RELKIND_PARTITIONED_TABLE:
    /* OK */
    break;
  default:
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("cannot create index on relation \"%s\"",
                           RelationGetRelationName(rel)),
                    errdetail_relkind_not_supported(rel->rd_rel->relkind)));
    break;
  }

  /*
   * Establish behavior for partitioned tables, and verify sanity of
   * parameters.
   *
   * We do not build an actual index in this case; we only create a few
   * catalog entries.  The actual indexes are built by recursing for each
   * partition.
   */
  partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
  if (partitioned) {
    /*
     * Note: we check 'stmt->concurrent' rather than 'concurrent', so that
     * the error is thrown also for temporary tables.  Seems better to be
     * consistent, even though we could do it on temporary table because
     * we're not actually doing it concurrently.
     */
    if (stmt->concurrent)
      ereport(
          ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg(
               "cannot create index on partitioned table \"%s\" concurrently",
               RelationGetRelationName(rel))));
  }

  /*
   * Don't try to CREATE INDEX on temp tables of other backends.
   */
  if (RELATION_IS_OTHER_TEMP(rel))
    ereport(
        ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg(
             "cannot create indexes on temporary tables of other sessions")));

  /*
   * Unless our caller vouches for having checked this already, insist that
   * the table not be in use by our own session, either.  Otherwise we might
   * fail to make entries in the new index (for instance, if an INSERT or
   * UPDATE is in progress and has already made its list of target indexes).
   */
  if (check_not_in_use)
    CheckTableNotInUse(rel, "CREATE INDEX");

  /*
   * Verify we (still) have CREATE rights in the rel's namespace.
   * (Presumably we did when the rel was created, but maybe not anymore.)
   * Skip check if caller doesn't want it.  Also skip check if
   * bootstrapping, since permissions machinery may not be working yet.
   */
  if (check_rights && !IsBootstrapProcessingMode()) {
    AclResult aclresult;

    aclresult = object_aclcheck(NamespaceRelationId, namespaceId,
                                root_save_userid, ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
      aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceId));
  }

  /*
   * Select tablespace to use.  If not specified, use default tablespace
   * (which may in turn default to database's default).
   */
  if (stmt->tableSpace) {
    tablespaceId = get_tablespace_oid(stmt->tableSpace, false);
    if (partitioned && tablespaceId == MyDatabaseTableSpace)
      ereport(
          ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg(
               "cannot specify default tablespace for partitioned relations")));
  } else {
    /* The database's: there are no others. */
    tablespaceId = InvalidOid;
  }

  /* Check tablespace permissions */
  if (check_rights && OidIsValid(tablespaceId) &&
      tablespaceId != MyDatabaseTableSpace) {
    AclResult aclresult;

    aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId,
                                root_save_userid, ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
      aclcheck_error(aclresult, OBJECT_TABLESPACE,
                     get_tablespace_name(tablespaceId));
  }

  /*
   * Force shared indexes into the pg_global tablespace.  This is a bit of a
   * hack but seems simpler than marking them in the BKI commands.  On the
   * other hand, if it's not shared, don't allow it to be placed there.
   */
  if (rel->rd_rel->relisshared)
    tablespaceId = GLOBALTABLESPACE_OID;
  else if (tablespaceId == GLOBALTABLESPACE_OID)
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg(
             "only shared relations can be placed in pg_global tablespace")));

  /*
   * Choose the index column names.
   */
  indexColNames = ChooseIndexColumnNames(allIndexParams);

  /*
   * Select name for index if caller didn't specify
   */
  indexRelationName = stmt->idxname;
  if (indexRelationName == NULL)
    indexRelationName = ChooseIndexName(
        RelationGetRelationName(rel), namespaceId, indexColNames,
        stmt->excludeOpNames, stmt->primary, stmt->isconstraint);

  /*
   * look up the access method, verify it can handle the requested features
   */
  accessMethodName = stmt->accessMethod;
  tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
  if (!HeapTupleIsValid(tuple)) {
    /*
     * Hack to provide more-or-less-transparent updating of old RTREE
     * indexes to GiST: if RTREE is requested and not found, use GIST.
     */
    if (strcmp(accessMethodName, "rtree") == 0) {
      ereport(NOTICE, (errmsg("substituting access method \"gist\" for "
                              "obsolete method \"rtree\"")));
      accessMethodName = "gist";
      tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
    }

    if (!HeapTupleIsValid(tuple))
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                      errmsg("access method \"%s\" does not exist",
                             accessMethodName)));
  }
  accessMethodForm = (Form_pg_am)GETSTRUCT(tuple);
  accessMethodId = accessMethodForm->oid;
  amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);

  if (stmt->unique && !stmt->iswithoutoverlaps && !amRoutine->amcanunique)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("access method \"%s\" does not support unique indexes",
                    accessMethodName)));
  if (stmt->indexIncludingParams != NIL && !amRoutine->amcaninclude)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("access method \"%s\" does not support included columns",
                    accessMethodName)));
  if (numberOfKeyAttributes > 1 && !amRoutine->amcanmulticol)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("access method \"%s\" does not support multicolumn indexes",
                    accessMethodName)));
  if (exclusion && amRoutine->amgettuple == NULL)
    ereport(
        ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("access method \"%s\" does not support exclusion constraints",
                accessMethodName)));
  if (stmt->iswithoutoverlaps && strcmp(accessMethodName, "gist") != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" does not support WITHOUT "
                           "OVERLAPS constraints",
                           accessMethodName)));

  amcanorder = amRoutine->amcanorder;
  amoptions = amRoutine->amoptions;
  amissummarizing = amRoutine->amsummarizing;

  pfree(amRoutine);
  ReleaseSysCache(tuple);

  /*
   * Validate predicate, if given
   */
  if (stmt->whereClause)
    CheckPredicate((Expr *)stmt->whereClause);

  /*
   * Parse AM-specific options, convert to text array form, validate.
   */
  reloptions =
      transformRelOptions((Datum)0, stmt->options, NULL, NULL, false, false);

  (void)index_reloptions(amoptions, reloptions, true);

  /*
   * Prepare arguments for index_create, primarily an IndexInfo structure.
   * Note that predicates must be in implicit-AND format.  In a concurrent
   * build, mark it not-ready-for-inserts.
   */
  indexInfo =
      makeIndexInfo(numberOfAttributes, numberOfKeyAttributes, accessMethodId,
                    NIL, /* expressions, NIL for now */
                    make_ands_implicit((Expr *)stmt->whereClause), stmt->unique,
                    stmt->nulls_not_distinct, !concurrent, concurrent,
                    amissummarizing, stmt->iswithoutoverlaps);

  typeIds = palloc_array(Oid, numberOfAttributes);
  collationIds = palloc_array(Oid, numberOfAttributes);
  opclassIds = palloc_array(Oid, numberOfAttributes);
  opclassOptions = palloc_array(Datum, numberOfAttributes);
  coloptions = palloc_array(int16, numberOfAttributes);
  ComputeIndexAttrs(
      indexInfo, typeIds, collationIds, opclassIds, opclassOptions, coloptions,
      allIndexParams, stmt->excludeOpNames, tableId, accessMethodName,
      accessMethodId, amcanorder, stmt->isconstraint, stmt->iswithoutoverlaps,
      root_save_userid, root_save_sec_context, &root_save_nestlevel);

  /*
   * The below call to index_create() creates the dependencies on types.  We
   * are responsible for checking USAGE.
   */
  if (check_rights) {
    if (indexInfo->ii_Expressions)
      CheckUsageOnTypesInSingleRelExpr((Node *)indexInfo->ii_Expressions,
                                       tableId, root_save_userid);
    if (indexInfo->ii_Predicate)
      CheckUsageOnTypesInSingleRelExpr((Node *)indexInfo->ii_Predicate, tableId,
                                       root_save_userid);
  }

  /*
   * Extra checks when creating a PRIMARY KEY index.
   */
  if (stmt->primary)
    index_check_primary_key(rel, indexInfo, is_alter_table, stmt);

  /*
   * If this table is partitioned and we're creating a unique index, primary
   * key, or exclusion constraint, make sure that the partition key is a
   * subset of the index's columns.  Otherwise it would be possible to
   * violate uniqueness by putting values that ought to be unique in
   * different partitions.
   *
   * We could lift this limitation if we had global indexes, but those have
   * their own problems, so this is a useful feature combination.
   */
  if (partitioned && (stmt->unique || exclusion)) {
    PartitionKey key = RelationGetPartitionKey(rel);
    const char *constraint_type;
    int i;

    if (stmt->primary)
      constraint_type = "PRIMARY KEY";
    else if (stmt->unique)
      constraint_type = "UNIQUE";
    else if (stmt->excludeOpNames)
      constraint_type = "EXCLUDE";
    else {
      elog(ERROR, "unknown constraint type");
      constraint_type = NULL; /* keep compiler quiet */
    }

    /*
     * Verify that all the columns in the partition key appear in the
     * unique key definition, with the same notion of equality.
     */
    for (i = 0; i < key->partnatts; i++) {
      bool found = false;
      int eq_strategy;
      Oid ptkey_eqop;
      int j;

      /*
       * Identify the equality operator associated with this partkey
       * column.  For list and range partitioning, partkeys use btree
       * operator classes; hash partitioning uses hash operator classes.
       * (Keep this in sync with ComputePartitionAttrs!)
       */
      if (key->strategy == PARTITION_STRATEGY_HASH)
        eq_strategy = HTEqualStrategyNumber;
      else
        eq_strategy = BTEqualStrategyNumber;

      ptkey_eqop =
          get_opfamily_member(key->partopfamily[i], key->partopcintype[i],
                              key->partopcintype[i], eq_strategy);
      if (!OidIsValid(ptkey_eqop))
        elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
             eq_strategy, key->partopcintype[i], key->partopcintype[i],
             key->partopfamily[i]);

      /*
       * It may be possible to support UNIQUE constraints when partition
       * keys are expressions, but is it worth it?  Give up for now.
       */
      if (key->partattrs[i] == 0)
        ereport(
            ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("unsupported %s constraint with partition key definition",
                    constraint_type),
             errdetail("%s constraints cannot be used when partition keys "
                       "include expressions.",
                       constraint_type)));

      /* Search the index column(s) for a match */
      for (j = 0; j < indexInfo->ii_NumIndexKeyAttrs; j++) {
        if (key->partattrs[i] == indexInfo->ii_IndexAttrNumbers[j]) {
          /*
           * Matched the column, now what about the collation and
           * equality op?
           */
          Oid idx_opfamily;
          Oid idx_opcintype;

          if (key->partcollation[i] != collationIds[j])
            continue;

          if (get_opclass_opfamily_and_input_type(opclassIds[j], &idx_opfamily,
                                                  &idx_opcintype)) {
            Oid idx_eqop = InvalidOid;

            if (stmt->unique && !stmt->iswithoutoverlaps)
              idx_eqop = get_opfamily_member_for_cmptype(
                  idx_opfamily, idx_opcintype, idx_opcintype, COMPARE_EQ);
            else if (exclusion)
              idx_eqop = indexInfo->ii_ExclusionOps[j];

            if (!idx_eqop)
              ereport(
                  ERROR, errcode(ERRCODE_UNDEFINED_OBJECT),
                  errmsg("could not identify an equality operator for type %s",
                         format_type_be(idx_opcintype)),
                  errdetail("There is no suitable operator in operator family "
                            "\"%s\" for access method \"%s\".",
                            get_opfamily_name(idx_opfamily, false),
                            get_am_name(get_opfamily_method(idx_opfamily))));

            if (ptkey_eqop == idx_eqop) {
              found = true;
              break;
            } else if (exclusion) {
              /*
               * We found a match, but it's not an equality
               * operator. Instead of failing below with an
               * error message about a missing column, fail now
               * and explain that the operator is wrong.
               */
              Form_pg_attribute att =
                  TupleDescAttr(RelationGetDescr(rel), key->partattrs[i] - 1);

              ereport(ERROR,
                      (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                       errmsg("cannot match partition key to index on column "
                              "\"%s\" using non-equal operator \"%s\"",
                              NameStr(att->attname),
                              get_opname(indexInfo->ii_ExclusionOps[j]))));
            }
          }
        }
      }

      if (!found) {
        Form_pg_attribute att;

        att = TupleDescAttr(RelationGetDescr(rel), key->partattrs[i] - 1);
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("unique constraint on partitioned table must "
                               "include all partitioning columns"),
                        errdetail("%s constraint on table \"%s\" lacks column "
                                  "\"%s\" which is part of the partition key.",
                                  constraint_type, RelationGetRelationName(rel),
                                  NameStr(att->attname))));
      }
    }
  }

  /*
   * We disallow indexes on system columns.  They would not necessarily get
   * updated correctly, and they don't seem useful anyway.
   *
   * Also disallow virtual generated columns in indexes (use expression
   * index instead).
   */
  for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++) {
    AttrNumber attno = indexInfo->ii_IndexAttrNumbers[i];

    if (attno < 0)
      ereport(ERROR,
              (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
               errmsg("index creation on system columns is not supported")));

    if (attno > 0 &&
        TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated ==
            ATTRIBUTE_GENERATED_VIRTUAL)
      ereport(
          ERROR, errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
          stmt->primary ? errmsg("primary keys on virtual generated columns "
                                 "are not supported")
          : stmt->isconstraint
              ? errmsg("unique constraints on virtual generated columns are "
                       "not supported")
              : errmsg(
                    "indexes on virtual generated columns are not supported"));
  }

  /*
   * Also check for system and generated columns used in expressions or
   * predicates.
   */
  if (indexInfo->ii_Expressions || indexInfo->ii_Predicate) {
    Bitmapset *indexattrs = NULL;
    int j;

    pull_varattnos((Node *)indexInfo->ii_Expressions, 1, &indexattrs);
    pull_varattnos((Node *)indexInfo->ii_Predicate, 1, &indexattrs);

    for (int i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++) {
      if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, indexattrs))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("index creation on system columns is not supported")));
    }

    /*
     * XXX Virtual generated columns in index expressions or predicates
     * could be supported, but it needs support in
     * RelationGetIndexExpressions() and RelationGetIndexPredicate().
     */
    j = -1;
    while ((j = bms_next_member(indexattrs, j)) >= 0) {
      AttrNumber attno = j + FirstLowInvalidHeapAttributeNumber;

      if (attno > 0 &&
          TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated ==
              ATTRIBUTE_GENERATED_VIRTUAL)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        stmt->isconstraint
                            ? errmsg("unique constraints on virtual generated "
                                     "columns are not supported")
                            : errmsg("indexes on virtual generated columns are "
                                     "not supported")));
    }
  }

  /* Is index safe for others to ignore?  See set_indexsafe_procflags() */
  safe_index =
      indexInfo->ii_Expressions == NIL && indexInfo->ii_Predicate == NIL;

  /*
   * Report index creation if appropriate (delay this till after most of the
   * error checks)
   */
  if (stmt->isconstraint && !quiet) {
    const char *constraint_type;

    if (stmt->primary)
      constraint_type = "PRIMARY KEY";
    else if (stmt->unique)
      constraint_type = "UNIQUE";
    else if (stmt->excludeOpNames)
      constraint_type = "EXCLUDE";
    else {
      elog(ERROR, "unknown constraint type");
      constraint_type = NULL; /* keep compiler quiet */
    }

    ereport(
        DEBUG1,
        (errmsg_internal(
            "%s %s will create implicit index \"%s\" for table \"%s\"",
            is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
            constraint_type, indexRelationName, RelationGetRelationName(rel))));
  }

  /*
   * A valid stmt->oldNumber implies that we already have a built form of
   * the index.  The caller should also decline any index build.
   */
  Assert(!RelFileNumberIsValid(stmt->oldNumber) || (skip_build && !concurrent));

  /*
   * Make the catalog entries for the index, including constraints. This
   * step also actually builds the index, except if caller requested not to
   * or in concurrent mode, in which case it'll be done later, or doing a
   * partitioned index (because those don't have storage).
   */
  flags = constr_flags = 0;
  if (stmt->isconstraint)
    flags |= INDEX_CREATE_ADD_CONSTRAINT;
  if (skip_build || concurrent || partitioned)
    flags |= INDEX_CREATE_SKIP_BUILD;
  if (stmt->if_not_exists)
    flags |= INDEX_CREATE_IF_NOT_EXISTS;
  if (concurrent)
    flags |= INDEX_CREATE_CONCURRENT;
  if (partitioned)
    flags |= INDEX_CREATE_PARTITIONED;
  if (stmt->primary)
    flags |= INDEX_CREATE_IS_PRIMARY;

  /*
   * If the table is partitioned, and recursion was declined but partitions
   * exist, mark the index as invalid.
   */
  if (partitioned && stmt->relation && !stmt->relation->inh) {
    PartitionDesc pd = RelationGetPartitionDesc(rel, true);

    if (pd->nparts != 0)
      flags |= INDEX_CREATE_INVALID;
  }

  if (stmt->deferrable)
    constr_flags |= INDEX_CONSTR_CREATE_DEFERRABLE;
  if (stmt->initdeferred)
    constr_flags |= INDEX_CONSTR_CREATE_INIT_DEFERRED;
  if (stmt->iswithoutoverlaps)
    constr_flags |= INDEX_CONSTR_CREATE_WITHOUT_OVERLAPS;

  indexRelationId = index_create(
      rel, indexRelationName, indexRelationId, parentIndexId,
      parentConstraintId, stmt->oldNumber, indexInfo, indexColNames,
      accessMethodId, tablespaceId, collationIds, opclassIds, opclassOptions,
      coloptions, NULL, reloptions, flags, constr_flags, allowSystemTableMods,
      !check_rights, &createdConstraintId);

  ObjectAddressSet(address, RelationRelationId, indexRelationId);

  if (!OidIsValid(indexRelationId)) {
    /*
     * Roll back any GUC changes executed by index functions.  Also revert
     * to original default_tablespace if we changed it above.
     */
    AtEOXact_GUC(false, root_save_nestlevel);

    /* Restore userid and security context */
    SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

    table_close(rel, NoLock);

    return address;
  }

  /*
   * Roll back any GUC changes executed by index functions, and keep
   * subsequent changes local to this command.  This is essential if some
   * index function changed a behavior-affecting GUC, e.g. search_path.
   */
  AtEOXact_GUC(false, root_save_nestlevel);
  root_save_nestlevel = NewGUCNestLevel();
  RestrictSearchPath();

  /* Add any requested comment */
  if (stmt->idxcomment != NULL)
    CreateComments(indexRelationId, RelationRelationId, 0, stmt->idxcomment);

  if (partitioned) {
    PartitionDesc partdesc;

    /*
     * Unless caller specified to skip this step (via ONLY), process each
     * partition to make sure they all contain a corresponding index.
     *
     * If we're called internally (no stmt->relation), recurse always.
     */
    partdesc = RelationGetPartitionDesc(rel, true);
    if ((!stmt->relation || stmt->relation->inh) && partdesc->nparts > 0) {
      int nparts = partdesc->nparts;
      Oid *part_oids = palloc_array(Oid, nparts);
      bool invalidate_parent = false;
      Relation parentIndex;
      TupleDesc parentDesc;

      /*
       * Report the total number of partitions at the start of the
       * command; don't update it when being called recursively.
       */
      if (!OidIsValid(parentIndexId)) {
        /*
         * When called by ProcessUtilitySlow, the number of partitions
         * is passed in as an optimization; but other callers pass -1
         * since they don't have the value handy.  This should count
         * partitions the same way, ie one less than the number of
         * relations find_all_inheritors reports.
         *
         * We assume we needn't ask find_all_inheritors to take locks,
         * because that should have happened already for all callers.
         * Even if it did not, this is safe as long as we don't try to
         * touch the partitions here; the worst consequence would be a
         * bogus progress-reporting total.
         */
        if (total_parts < 0) {
          List *children = find_all_inheritors(tableId, NoLock, NULL);

          total_parts = list_length(children) - 1;
          list_free(children);
        }
      }

      /* Make a local copy of partdesc->oids[], just for safety */
      memcpy(part_oids, partdesc->oids, sizeof(Oid) * nparts);

      /*
       * We'll need an IndexInfo describing the parent index.  The one
       * built above is almost good enough, but not quite, because (for
       * example) its predicate expression if any hasn't been through
       * expression preprocessing.  The most reliable way to get an
       * IndexInfo that will match those for child indexes is to build
       * it the same way, using BuildIndexInfo().
       */
      parentIndex = index_open(indexRelationId, lockmode);
      indexInfo = BuildIndexInfo(parentIndex);

      parentDesc = RelationGetDescr(rel);

      /*
       * For each partition, scan all existing indexes; if one matches
       * our index definition and is not already attached to some other
       * parent index, attach it to the one we just created.
       *
       * If none matches, build a new index by calling ourselves
       * recursively with the same options (except for the index name).
       */
      for (int i = 0; i < nparts; i++) {
        Oid childRelid = part_oids[i];
        Relation childrel;
        Oid child_save_userid;
        int child_save_sec_context;
        int child_save_nestlevel;
        List *childidxs;
        ListCell *cell;
        AttrMap *attmap;
        bool found = false;

        childrel = table_open(childRelid, lockmode);

        GetUserIdAndSecContext(&child_save_userid, &child_save_sec_context);
        SetUserIdAndSecContext(childrel->rd_rel->relowner,
                               child_save_sec_context |
                                   SECURITY_RESTRICTED_OPERATION);
        child_save_nestlevel = NewGUCNestLevel();
        RestrictSearchPath();

        /*
         * Don't try to create indexes on foreign tables, though. Skip
         * those if a regular index, or fail if trying to create a
         * constraint index.
         */
        if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE) {
          if (stmt->unique || stmt->primary)
            ereport(
                ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg(
                     "cannot create unique index on partitioned table \"%s\"",
                     RelationGetRelationName(rel)),
                 errdetail("Table \"%s\" contains partitions that are foreign "
                           "tables.",
                           RelationGetRelationName(rel))));

          AtEOXact_GUC(false, child_save_nestlevel);
          SetUserIdAndSecContext(child_save_userid, child_save_sec_context);
          table_close(childrel, lockmode);
          continue;
        }

        childidxs = RelationGetIndexList(childrel);
        attmap = build_attrmap_by_name(RelationGetDescr(childrel), parentDesc,
                                       false);

        foreach (cell, childidxs) {
          Oid cldidxid = lfirst_oid(cell);
          Relation cldidx;
          IndexInfo *cldIdxInfo;

          /* this index is already partition of another one */
          if (has_superclass(cldidxid))
            continue;

          cldidx = index_open(cldidxid, lockmode);
          cldIdxInfo = BuildIndexInfo(cldidx);
          if (CompareIndexInfo(cldIdxInfo, indexInfo, cldidx->rd_indcollation,
                               parentIndex->rd_indcollation,
                               cldidx->rd_opfamily, parentIndex->rd_opfamily,
                               attmap)) {
            Oid cldConstrOid = InvalidOid;

            /*
             * Found a match.
             *
             * If this index is being created in the parent
             * because of a constraint, then the child needs to
             * have a constraint also, so look for one.  If there
             * is no such constraint, this index is no good, so
             * keep looking.
             */
            if (createdConstraintId != InvalidOid) {
              cldConstrOid =
                  get_relation_idx_constraint_oid(childRelid, cldidxid);
              if (cldConstrOid == InvalidOid) {
                index_close(cldidx, lockmode);
                continue;
              }
            }

            /* Attach index to parent and we're done. */
            IndexSetParentIndex(cldidx, indexRelationId);
            if (createdConstraintId != InvalidOid)
              ConstraintSetParentConstraint(cldConstrOid, createdConstraintId,
                                            childRelid);

            if (!cldidx->rd_index->indisvalid)
              invalidate_parent = true;

            found = true;

            /* keep lock till commit */
            index_close(cldidx, NoLock);
            break;
          }

          index_close(cldidx, lockmode);
        }

        list_free(childidxs);
        AtEOXact_GUC(false, child_save_nestlevel);
        SetUserIdAndSecContext(child_save_userid, child_save_sec_context);
        table_close(childrel, NoLock);

        /*
         * If no matching index was found, create our own.
         */
        if (!found) {
          IndexStmt *childStmt;
          ObjectAddress childAddr;

          /*
           * Build an IndexStmt describing the desired child index
           * in the same way that we do during ATTACH PARTITION.
           * Notably, we rely on generateClonedIndexStmt to produce
           * a search-path-independent representation, which the
           * original IndexStmt might not be.
           */
          childStmt = generateClonedIndexStmt(NULL, parentIndex, attmap, NULL);

          /*
           * Recurse as the starting user ID.  Callee will use that
           * for permission checks, then switch again.
           */
          Assert(GetUserId() == child_save_userid);
          SetUserIdAndSecContext(root_save_userid, root_save_sec_context);
          childAddr = DefineIndex(
              childRelid, childStmt, InvalidOid, /* no predefined OID */
              indexRelationId,                   /* this is our child */
              createdConstraintId, -1, is_alter_table, check_rights,
              check_not_in_use, skip_build, quiet);
          SetUserIdAndSecContext(child_save_userid, child_save_sec_context);

          /*
           * Check if the index just created is valid or not, as it
           * could be possible that it has been switched as invalid
           * when recursing across multiple partition levels.
           */
          if (!get_index_isvalid(childAddr.objectId))
            invalidate_parent = true;
        }

        free_attrmap(attmap);
      }

      index_close(parentIndex, lockmode);

      /*
       * The pg_index row we inserted for this index was marked
       * indisvalid=true.  But if we attached an existing index that is
       * invalid, this is incorrect, so update our row to invalid too.
       */
      if (invalidate_parent) {
        Relation pg_index = table_open(IndexRelationId, RowExclusiveLock);
        HeapTuple tup, newtup;

        tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexRelationId));
        if (!HeapTupleIsValid(tup))
          elog(ERROR, "cache lookup failed for index %u", indexRelationId);
        newtup = heap_copytuple(tup);
        ((Form_pg_index)GETSTRUCT(newtup))->indisvalid = false;
        CatalogTupleUpdate(pg_index, &tup->t_self, newtup);
        ReleaseSysCache(tup);
        table_close(pg_index, RowExclusiveLock);
        heap_freetuple(newtup);

        /*
         * CCI here to make this update visible, in case this recurses
         * across multiple partition levels.
         */
        CommandCounterIncrement();
      }
    }

    /*
     * Indexes on partitioned tables are not themselves built, so we're
     * done here.
     */
    AtEOXact_GUC(false, root_save_nestlevel);
    SetUserIdAndSecContext(root_save_userid, root_save_sec_context);
    table_close(rel, NoLock);

    return address;
  }

  AtEOXact_GUC(false, root_save_nestlevel);
  SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

  if (!concurrent) {
    /* Close the heap and we're done, in the non-concurrent case */
    table_close(rel, NoLock);

    return address;
  }

  /* save lockrelid for below, then close rel */
  heaprelid = rel->rd_lockInfo.lockRelId;
  table_close(rel, NoLock);

  /*
   * For a concurrent build, it's important to make the catalog entries
   * visible to other transactions before we start to build the index. That
   * will prevent them from making incompatible HOT updates.  The new index
   * will be marked not indisready and not indisvalid, so that no one else
   * tries to either insert into it or use it for queries.
   *
   * We must commit our current transaction so that the index becomes
   * visible; then start another.  Note that all the data structures we just
   * built are lost in the commit.  The only data we keep past here are the
   * relation IDs.
   *
   * No session-level lock keeps the table: none is held of this lock
   * manager (storage/lmgr/README).
   */
  PopActiveSnapshot();
  CommitTransactionCommand();
  StartTransactionCommand();

  /* Tell concurrent index builds to ignore us, if index qualifies */
  if (safe_index)
    set_indexsafe_procflags();

  /*
   * The index is now visible, so we can report the OID.  While on it,
   * include the report for the beginning of phase 2.
   */
  {}

  /*
   * Phase 2 of concurrent index build (see comments for validate_index()
   * for an overview of how this works)
   *
   * PostgreSQL waited here for every transaction that could have the table
   * open with the old list of indexes.  No lock of this lock manager is
   * held, so there is none to wait for (storage/lmgr/README).
   */

  /*
   * At this moment we are sure that there are no transactions with the
   * table open for write that don't have this new index in their list of
   * indexes.  We have waited out all the existing transactions and any new
   * transaction will have the new index in its list, but the index is still
   * marked as "not-ready-for-inserts".  The index is consulted while
   * deciding HOT-safety though.  This arrangement ensures that no new HOT
   * chains can be created where the new tuple and the old tuple in the
   * chain have different index keys.
   *
   * We now take a new snapshot, and build the index using all tuples that
   * are visible in this snapshot.  We can be sure that any HOT updates to
   * these tuples will be compatible with the index, since any updates made
   * by transactions that didn't know about the index are now committed or
   * rolled back.  Thus, each visible tuple is either the end of its
   * HOT-chain or the extension of the chain is HOT-safe for this index.
   */

  /* Set ActiveSnapshot since functions in the indexes may need it */
  PushActiveSnapshot(GetTransactionSnapshot());

  /* Perform concurrent build of index */
  index_concurrently_build(tableId, indexRelationId);

  /* we can do away with our snapshot */
  PopActiveSnapshot();

  /*
   * Commit this transaction to make the indisready update visible.
   */
  CommitTransactionCommand();
  StartTransactionCommand();

  /* Tell concurrent index builds to ignore us, if index qualifies */
  if (safe_index)
    set_indexsafe_procflags();

  /*
   * Phase 3 of concurrent index build
   *
   * As in phase 2, there is no transaction to wait for.
   */

  /*
   * Now take the "reference snapshot" that will be used by validate_index()
   * to filter candidate tuples.  Beware!  There might still be snapshots in
   * use that treat some transaction as in-progress that our reference
   * snapshot treats as committed.  If such a recently-committed transaction
   * deleted tuples in the table, we will not include them in the index; yet
   * those transactions which see the deleting one as still-in-progress will
   * expect such tuples to be there once we mark the index as valid.
   *
   * We solve this by waiting for all endangered transactions to exit before
   * we mark the index as valid.
   *
   * We also set ActiveSnapshot to this snap, since functions in indexes may
   * need a snapshot.
   */
  snapshot = RegisterSnapshot(GetTransactionSnapshot());
  PushActiveSnapshot(snapshot);

  /*
   * Scan the index and the heap, insert any missing index entries.
   */
  validate_index(tableId, indexRelationId, snapshot);

  /*
   * Drop the reference snapshot.  We must do this before waiting out other
   * snapshot holders, else we will deadlock against other processes also
   * doing CREATE INDEX CONCURRENTLY, which would see our snapshot as one
   * they must wait for.  But first, save the snapshot's xmin to use as
   * limitXmin for GetCurrentVirtualXIDs().
   */
  limitXmin = snapshot->xmin;

  PopActiveSnapshot();
  UnregisterSnapshot(snapshot);

  /*
   * The snapshot subsystem could still contain registered snapshots that
   * are holding back our process's advertised xmin; in particular, if
   * default_transaction_isolation = serializable, there is a transaction
   * snapshot that is still active.  The CatalogSnapshot is likewise a
   * hazard.  To ensure no deadlocks, we must commit and start yet another
   * transaction, and do our wait before any snapshot has been taken in it.
   */
  CommitTransactionCommand();
  StartTransactionCommand();

  /* Tell concurrent index builds to ignore us, if index qualifies */
  if (safe_index)
    set_indexsafe_procflags();

  /* We should now definitely not be advertising any xmin. */
  Assert(tcb->xmin == InvalidTransactionId);

  /*
   * The index is now valid in the sense that it contains all currently
   * interesting tuples.  But since it might not contain tuples deleted just
   * before the reference snap was taken, we have to wait out any
   * transactions that might have older snapshots.
   */
  WaitForOlderSnapshots(limitXmin);

  /*
   * Updating pg_index might involve TOAST table access, so ensure we have a
   * valid snapshot.
   */
  PushActiveSnapshot(GetTransactionSnapshot());

  /*
   * Index can now be marked valid -- update its pg_index entry
   */
  index_set_state_flags(indexRelationId, INDEX_CREATE_SET_VALID);

  PopActiveSnapshot();

  /*
   * The pg_index update will cause backends (including this one) to update
   * relcache entries for the index itself, but we should also send a
   * relcache inval on the parent table to force replanning of cached plans.
   * Otherwise existing sessions might fail to use the new index where it
   * would be useful.  (Note that our earlier commits did not create reasons
   * to replan; so relcache flush on the index itself was sufficient.)
   */
  CacheInvalidateRelcacheByRelid(heaprelid.relId);

  return address;
}

/*
 * CheckPredicate
 *		Checks that the given partial-index predicate is valid.
 *
 * This used to also constrain the form of the predicate to forms that
 * indxpath.c could do something with.  However, that seems overly
 * restrictive.  One useful application of partial indexes is to apply
 * a UNIQUE constraint across a subset of a table, and in that scenario
 * any evaluable predicate will work.  So accept any predicate here
 * (except ones requiring a plan), and let indxpath.c fend for itself.
 */
static void CheckPredicate(Expr *predicate) {
  /*
   * transformExpr() should have already rejected subqueries, aggregates,
   * and window functions, based on the EXPR_KIND_ for a predicate.
   */

  /*
   * A predicate using mutable functions is probably wrong, for the same
   * reasons that we don't allow an index expression to use one.
   */
  if (contain_mutable_functions_after_planning(predicate))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
             errmsg("functions in index predicate must be marked IMMUTABLE")));
}

/*
 * Compute per-index-column information, including indexed column numbers
 * or index expressions, opclasses and their options. Note, all output vectors
 * should be allocated for all columns, including "including" ones.
 *
 * If the caller switched to the table owner, ddl_userid is the role for ACL
 * checks reached without traversing opaque expressions.  Otherwise, it's
 * InvalidOid, and other ddl_* arguments are undefined.
 */
static void ComputeIndexAttrs(IndexInfo *indexInfo, Oid *typeOids,
                              Oid *collationOids, Oid *opclassOids,
                              Datum *opclassOptions, int16 *colOptions,
                              const List *attList, /* list of IndexElem's */
                              const List *exclusionOpNames, Oid relId,
                              const char *accessMethodName, Oid accessMethodId,
                              bool amcanorder, bool isconstraint,
                              bool iswithoutoverlaps, Oid ddl_userid,
                              int ddl_sec_context, int *ddl_save_nestlevel) {
  ListCell *nextExclOp;
  ListCell *lc;
  int attn;
  int nkeycols = indexInfo->ii_NumIndexKeyAttrs;
  Oid save_userid;
  int save_sec_context;

  /* Allocate space for exclusion operator info, if needed */
  if (exclusionOpNames) {
    Assert(list_length(exclusionOpNames) == nkeycols);
    indexInfo->ii_ExclusionOps = palloc_array(Oid, nkeycols);
    indexInfo->ii_ExclusionProcs = palloc_array(Oid, nkeycols);
    indexInfo->ii_ExclusionStrats = palloc_array(uint16, nkeycols);
    nextExclOp = list_head(exclusionOpNames);
  } else
    nextExclOp = NULL;

  /*
   * If this is a WITHOUT OVERLAPS constraint, we need space for exclusion
   * ops, but we don't need to parse anything, so we can let nextExclOp be
   * NULL. Note that for partitions/inheriting/LIKE, exclusionOpNames will
   * be set, so we already allocated above.
   */
  if (iswithoutoverlaps) {
    if (exclusionOpNames == NIL) {
      indexInfo->ii_ExclusionOps = palloc_array(Oid, nkeycols);
      indexInfo->ii_ExclusionProcs = palloc_array(Oid, nkeycols);
      indexInfo->ii_ExclusionStrats = palloc_array(uint16, nkeycols);
    }
    nextExclOp = NULL;
  }

  if (OidIsValid(ddl_userid))
    GetUserIdAndSecContext(&save_userid, &save_sec_context);

  /*
   * process attributeList
   */
  attn = 0;
  foreach (lc, attList) {
    IndexElem *attribute = (IndexElem *)lfirst(lc);
    Oid atttype;
    Oid attcollation;

    /*
     * Process the column-or-expression to be indexed.
     */
    if (attribute->name != NULL) {
      /* Simple index attribute */
      HeapTuple atttuple;
      Form_pg_attribute attform;

      Assert(attribute->expr == NULL);
      atttuple = SearchSysCacheAttName(relId, attribute->name);
      if (!HeapTupleIsValid(atttuple)) {
        /* difference in error message spellings is historical */
        if (isconstraint)
          ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                          errmsg("column \"%s\" named in key does not exist",
                                 attribute->name)));
        else
          ereport(ERROR,
                  (errcode(ERRCODE_UNDEFINED_COLUMN),
                   errmsg("column \"%s\" does not exist", attribute->name)));
      }
      attform = (Form_pg_attribute)GETSTRUCT(atttuple);
      indexInfo->ii_IndexAttrNumbers[attn] = attform->attnum;
      atttype = attform->atttypid;
      attcollation = attform->attcollation;
      ReleaseSysCache(atttuple);
    } else {
      /* Index expression */
      Node *expr = attribute->expr;

      Assert(expr != NULL);

      if (attn >= nkeycols)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("expressions are not supported in included columns")));
      atttype = exprType(expr);
      attcollation = exprCollation(expr);

      /*
       * Strip any top-level COLLATE clause.  This ensures that we treat
       * "x COLLATE y" and "(x COLLATE y)" alike.
       */
      while (IsA(expr, CollateExpr))
        expr = (Node *)((CollateExpr *)expr)->arg;

      if (IsA(expr, Var) && ((Var *)expr)->varattno != InvalidAttrNumber) {
        /*
         * User wrote "(column)" or "(column COLLATE something)".
         * Treat it like simple attribute anyway.
         */
        indexInfo->ii_IndexAttrNumbers[attn] = ((Var *)expr)->varattno;
      } else {
        indexInfo->ii_IndexAttrNumbers[attn] = 0; /* marks expression */
        indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions, expr);

        /*
         * transformExpr() should have already rejected subqueries,
         * aggregates, and window functions, based on the EXPR_KIND_
         * for an index expression.
         */

        /*
         * An expression using mutable functions is probably wrong,
         * since if you aren't going to get the same result for the
         * same data every time, it's not clear what the index entries
         * mean at all.
         */
        if (contain_mutable_functions_after_planning((Expr *)expr))
          ereport(
              ERROR,
              (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
               errmsg(
                   "functions in index expression must be marked IMMUTABLE")));
      }
    }

    typeOids[attn] = atttype;

    /*
     * Included columns have no collation, no opclass and no ordering
     * options.
     */
    if (attn >= nkeycols) {
      if (attribute->collation)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("including column does not support a collation")));
      if (attribute->opclass)
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
             errmsg("including column does not support an operator class")));
      if (attribute->ordering != SORTBY_DEFAULT)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("including column does not support ASC/DESC options")));
      if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
        ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmsg("including column does not support NULLS "
                               "FIRST/LAST options")));

      opclassOids[attn] = InvalidOid;
      opclassOptions[attn] = (Datum)0;
      colOptions[attn] = 0;
      collationOids[attn] = InvalidOid;
      attn++;

      continue;
    }

    /*
     * Apply collation override if any.  Use of ddl_userid is necessary
     * due to ACL checks therein, and it's safe because collations don't
     * contain opaque expressions (or non-opaque expressions).
     */
    if (attribute->collation) {
      if (OidIsValid(ddl_userid)) {
        AtEOXact_GUC(false, *ddl_save_nestlevel);
        SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
      }
      attcollation = get_collation_oid(attribute->collation, false);
      if (OidIsValid(ddl_userid)) {
        SetUserIdAndSecContext(save_userid, save_sec_context);
        *ddl_save_nestlevel = NewGUCNestLevel();
        RestrictSearchPath();
      }
    }

    /*
     * Check we have a collation iff it's a collatable type.  The only
     * expected failures here are (1) COLLATE applied to a noncollatable
     * type, or (2) index expression had an unresolved collation.  But we
     * might as well code this to be a complete consistency check.
     */
    if (type_is_collatable(atttype)) {
      if (!OidIsValid(attcollation))
        ereport(
            ERROR,
            (errcode(ERRCODE_INDETERMINATE_COLLATION),
             errmsg("could not determine which collation to use for index "
                    "expression"),
             errhint(
                 "Use the COLLATE clause to set the collation explicitly.")));
    } else {
      if (OidIsValid(attcollation))
        ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("collations are not supported by type %s",
                               format_type_be(atttype))));
    }

    collationOids[attn] = attcollation;

    /*
     * Identify the opclass to use.  Use of ddl_userid is necessary due to
     * ACL checks therein.  This is safe despite opclasses containing
     * opaque expressions (specifically, functions), because only
     * superusers can define opclasses.
     */
    if (OidIsValid(ddl_userid)) {
      AtEOXact_GUC(false, *ddl_save_nestlevel);
      SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
    }
    opclassOids[attn] = ResolveOpClass(attribute->opclass, atttype,
                                       accessMethodName, accessMethodId);
    if (OidIsValid(ddl_userid)) {
      SetUserIdAndSecContext(save_userid, save_sec_context);
      *ddl_save_nestlevel = NewGUCNestLevel();
      RestrictSearchPath();
    }

    /*
     * Identify the exclusion operator, if any.
     */
    if (nextExclOp) {
      List *opname = (List *)lfirst(nextExclOp);
      Oid opid;
      Oid opfamily;
      int strat;

      /*
       * Find the operator --- it must accept the column datatype
       * without runtime coercion (but binary compatibility is OK).
       * Operators contain opaque expressions (specifically, functions).
       * compatible_oper_opid() boils down to oper() and
       * IsBinaryCoercible().  PostgreSQL would have security problems
       * elsewhere if oper() started calling opaque expressions.
       */
      if (OidIsValid(ddl_userid)) {
        AtEOXact_GUC(false, *ddl_save_nestlevel);
        SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
      }
      opid = compatible_oper_opid(opname, atttype, atttype, false);
      if (OidIsValid(ddl_userid)) {
        SetUserIdAndSecContext(save_userid, save_sec_context);
        *ddl_save_nestlevel = NewGUCNestLevel();
        RestrictSearchPath();
      }

      /*
       * Only allow commutative operators to be used in exclusion
       * constraints. If X conflicts with Y, but Y does not conflict
       * with X, bad things will happen.
       */
      if (get_commutator(opid) != opid)
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("operator %s is not commutative",
                               format_operator(opid)),
                        errdetail("Only commutative operators can be used in "
                                  "exclusion constraints.")));

      /*
       * Operator must be a member of the right opfamily, too
       */
      opfamily = get_opclass_family(opclassOids[attn]);
      strat = get_op_opfamily_strategy(opid, opfamily);
      if (strat == 0)
        ereport(
            ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
             errmsg("operator %s is not a member of operator family \"%s\"",
                    format_operator(opid), get_opfamily_name(opfamily, false)),
             errdetail("The exclusion operator must be related to the index "
                       "operator class for the constraint.")));

      indexInfo->ii_ExclusionOps[attn] = opid;
      indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
      indexInfo->ii_ExclusionStrats[attn] = strat;
      nextExclOp = lnext(exclusionOpNames, nextExclOp);
    } else if (iswithoutoverlaps) {
      CompareType cmptype;
      StrategyNumber strat;
      Oid opid;

      if (attn == nkeycols - 1)
        cmptype = COMPARE_OVERLAP;
      else
        cmptype = COMPARE_EQ;
      GetOperatorFromCompareType(opclassOids[attn], InvalidOid, cmptype, &opid,
                                 &strat);
      indexInfo->ii_ExclusionOps[attn] = opid;
      indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
      indexInfo->ii_ExclusionStrats[attn] = strat;
    }

    /*
     * Set up the per-column options (indoption field).  For now, this is
     * zero for any un-ordered index, while ordered indexes have DESC and
     * NULLS FIRST/LAST options.
     */
    colOptions[attn] = 0;
    if (amcanorder) {
      /* default ordering is ASC */
      if (attribute->ordering == SORTBY_DESC)
        colOptions[attn] |= INDOPTION_DESC;
      /* default null ordering is LAST for ASC, FIRST for DESC */
      if (attribute->nulls_ordering == SORTBY_NULLS_DEFAULT) {
        if (attribute->ordering == SORTBY_DESC)
          colOptions[attn] |= INDOPTION_NULLS_FIRST;
      } else if (attribute->nulls_ordering == SORTBY_NULLS_FIRST)
        colOptions[attn] |= INDOPTION_NULLS_FIRST;
    } else {
      /* index AM does not support ordering */
      if (attribute->ordering != SORTBY_DEFAULT)
        ereport(
            ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("access method \"%s\" does not support ASC/DESC options",
                    accessMethodName)));
      if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("access method \"%s\" does not support NULLS "
                               "FIRST/LAST options",
                               accessMethodName)));
    }

    /* Set up the per-column opclass options (attoptions field). */
    if (attribute->opclassopts) {
      Assert(attn < nkeycols);

      opclassOptions[attn] = transformRelOptions(
          (Datum)0, attribute->opclassopts, NULL, NULL, false, false);
    } else
      opclassOptions[attn] = (Datum)0;

    attn++;
  }
}

/*
 * Resolve possibly-defaulted operator class specification
 *
 * Note: This is used to resolve operator class specifications in index and
 * partition key definitions.
 */
Oid ResolveOpClass(const List *opclass, Oid attrType,
                   const char *accessMethodName, Oid accessMethodId) {
  char *schemaname;
  char *opcname;
  HeapTuple tuple;
  Form_pg_opclass opform;
  Oid opClassId, opInputType;

  if (opclass == NIL) {
    /* no operator class specified, so find the default */
    opClassId = GetDefaultOpClass(attrType, accessMethodId);
    if (!OidIsValid(opClassId))
      ereport(ERROR,
              (errcode(ERRCODE_UNDEFINED_OBJECT),
               errmsg("data type %s has no default operator class for access "
                      "method \"%s\"",
                      format_type_be(attrType), accessMethodName),
               errhint("You must specify an operator class for the index or "
                       "define a default operator class for the data type.")));
    return opClassId;
  }

  /*
   * Specific opclass name given, so look up the opclass.
   */

  /* deconstruct the name list */
  DeconstructQualifiedName(opclass, &schemaname, &opcname);

  if (schemaname) {
    /* Look in specific schema only */
    Oid namespaceId;

    namespaceId = LookupExplicitNamespace(schemaname, false);
    tuple = SearchSysCache3(CLAAMNAMENSP, ObjectIdGetDatum(accessMethodId),
                            PointerGetDatum(opcname),
                            ObjectIdGetDatum(namespaceId));
  } else {
    /* Unqualified opclass name, so search the search path */
    opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
    if (!OidIsValid(opClassId))
      ereport(
          ERROR,
          (errcode(ERRCODE_UNDEFINED_OBJECT),
           errmsg(
               "operator class \"%s\" does not exist for access method \"%s\"",
               opcname, accessMethodName)));
    tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opClassId));
  }

  if (!HeapTupleIsValid(tuple))
    ereport(
        ERROR,
        (errcode(ERRCODE_UNDEFINED_OBJECT),
         errmsg("operator class \"%s\" does not exist for access method \"%s\"",
                NameListToString(opclass), accessMethodName)));

  /*
   * Verify that the index operator class accepts this datatype.  Note we
   * will accept binary compatibility.
   */
  opform = (Form_pg_opclass)GETSTRUCT(tuple);
  opClassId = opform->oid;
  opInputType = opform->opcintype;

  if (!IsBinaryCoercible(attrType, opInputType))
    ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("operator class \"%s\" does not accept data type %s",
                    NameListToString(opclass), format_type_be(attrType))));

  ReleaseSysCache(tuple);

  return opClassId;
}

/*
 * GetDefaultOpClass
 *
 * Given the OIDs of a datatype and an access method, find the default
 * operator class, if any.  Returns InvalidOid if there is none.
 */
Oid GetDefaultOpClass(Oid type_id, Oid am_id) {
  Oid result = InvalidOid;
  int nexact = 0;
  int ncompatible = 0;
  int ncompatiblepreferred = 0;
  int nrows;
  const cat_opclass_t *rows = cat_opclasses(&nrows);
  TYPCATEGORY tcategory;

  /* If it's a domain, look at the base type instead */
  type_id = getBaseType(type_id);

  tcategory = TypeCategory(type_id);

  /*
   * We scan through all the opclasses available for the access method,
   * looking for one that is marked default and matches the target type
   * (either exactly or binary-compatibly, but prefer an exact match).
   *
   * We could find more than one binary-compatible match.  If just one is
   * for a preferred type, use that one; otherwise we fail, forcing the user
   * to specify which one he wants.  (The preferred-type special case is a
   * kluge for varchar: it's binary-compatible to both text and bpchar, so
   * we need a tiebreaker.)  If we find more than one exact match, then
   * someone put bogus entries in pg_opclass.  Every opclass is compiled in
   * (catalog/cat_builtin.h).
   */
  for (int i = 0; i < nrows; i++) {
    const cat_opclass_t *opclass = &rows[i];

    /* ignore altogether if not one of the method's, or not its default */
    if (opclass->opcmethod != am_id || !opclass->opcdefault)
      continue;
    if (opclass->opcintype == type_id) {
      nexact++;
      result = opclass->oid;
    } else if (nexact == 0 && IsBinaryCoercible(type_id, opclass->opcintype)) {
      if (IsPreferredType(tcategory, opclass->opcintype)) {
        ncompatiblepreferred++;
        result = opclass->oid;
      } else if (ncompatiblepreferred == 0) {
        ncompatible++;
        result = opclass->oid;
      }
    }
  }

  /* raise error if pg_opclass contains inconsistent data */
  if (nexact > 1)
    ereport(
        ERROR,
        (errcode(ERRCODE_DUPLICATE_OBJECT),
         errmsg("there are multiple default operator classes for data type %s",
                format_type_be(type_id))));

  if (nexact == 1 || ncompatiblepreferred == 1 ||
      (ncompatiblepreferred == 0 && ncompatible == 1))
    return result;

  return InvalidOid;
}

/*
 * GetOperatorFromCompareType
 *
 * opclass - the opclass to use
 * rhstype - the type for the right-hand side, or InvalidOid to use the type of
 * the given opclass. cmptype - kind of operator to find opid - holds the
 * operator we found strat - holds the output strategy number
 *
 * Finds an operator from a CompareType.  This is used for temporal index
 * constraints (and other temporal features) to look up equality and overlaps
 * operators.  We ask an opclass support function to translate from the
 * compare type to the internal strategy numbers.  Raises ERROR on search
 * failure.
 */
void GetOperatorFromCompareType(Oid opclass, Oid rhstype, CompareType cmptype,
                                Oid *opid, StrategyNumber *strat) {
  Oid amid;
  Oid opfamily;
  Oid opcintype;

  Assert(cmptype == COMPARE_EQ || cmptype == COMPARE_OVERLAP ||
         cmptype == COMPARE_CONTAINED_BY);

  amid = get_opclass_method(opclass);

  *opid = InvalidOid;

  if (get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype)) {
    /*
     * Ask the index AM to translate to its internal stratnum
     */
    *strat = IndexAmTranslateCompareType(cmptype, amid, opfamily, true);
    if (*strat == InvalidStrategy)
      ereport(
          ERROR, errcode(ERRCODE_UNDEFINED_OBJECT),
          cmptype == COMPARE_EQ
              ? errmsg("could not identify an equality operator for type %s",
                       format_type_be(opcintype))
          : cmptype == COMPARE_OVERLAP
              ? errmsg("could not identify an overlaps operator for type %s",
                       format_type_be(opcintype))
          : cmptype == COMPARE_CONTAINED_BY
              ? errmsg("could not identify a contained-by operator for type %s",
                       format_type_be(opcintype))
              : 0,
          errdetail("Could not translate compare type %d for operator family "
                    "\"%s\" of access method \"%s\".",
                    cmptype, get_opfamily_name(opfamily, false),
                    get_am_name(amid)));

    /*
     * We parameterize rhstype so foreign keys can ask for a <@ operator
     * whose rhs matches the aggregate function. For example range_agg
     * returns anymultirange.
     */
    if (!OidIsValid(rhstype))
      rhstype = opcintype;
    *opid = get_opfamily_member(opfamily, opcintype, rhstype, *strat);
  }

  if (!OidIsValid(*opid))
    ereport(
        ERROR, errcode(ERRCODE_UNDEFINED_OBJECT),
        cmptype == COMPARE_EQ
            ? errmsg("could not identify an equality operator for type %s",
                     format_type_be(opcintype))
        : cmptype == COMPARE_OVERLAP
            ? errmsg("could not identify an overlaps operator for type %s",
                     format_type_be(opcintype))
        : cmptype == COMPARE_CONTAINED_BY
            ? errmsg("could not identify a contained-by operator for type %s",
                     format_type_be(opcintype))
            : 0,
        errdetail("There is no suitable operator in operator family \"%s\" for "
                  "access method \"%s\".",
                  get_opfamily_name(opfamily, false), get_am_name(amid)));
}

/*
 *	makeObjectName()
 *
 *	Create a name for an implicitly created index, sequence, constraint,
 *	extended statistics, etc.
 *
 *	The parameters are typically: the original table name, the original
 *field name, and a "type" string (such as "seq" or "pkey").    The field name
 *	and/or type can be NULL if not relevant.
 *
 *	The result is a palloc'd string.
 *
 *	The basic result we want is "name1_name2_label", omitting "_name2" or
 *	"_label" when those parameters are NULL.  However, we must generate
 *	a name with less than NAMEDATALEN characters!  So, we truncate one or
 *	both names if necessary to make a short-enough string.  The label part
 *	is never truncated (so it had better be reasonably short).
 *
 *	The caller is responsible for checking uniqueness of the generated
 *	name and retrying as needed; retrying will be done by altering the
 *	"label" string (which is why we never truncate that part).
 */
char *makeObjectName(const char *name1, const char *name2, const char *label) {
  char *name;
  int overhead = 0; /* chars needed for label and underscores */
  int availchars;   /* chars available for name(s) */
  int name1chars;   /* chars allocated to name1 */
  int name2chars;   /* chars allocated to name2 */
  int ndx;

  name1chars = strlen(name1);
  if (name2) {
    name2chars = strlen(name2);
    overhead++; /* allow for separating underscore */
  } else
    name2chars = 0;
  if (label)
    overhead += strlen(label) + 1;

  availchars = NAMEDATALEN - 1 - overhead;
  Assert(availchars > 0); /* else caller chose a bad label */

  /*
   * If we must truncate, preferentially truncate the longer name. This
   * logic could be expressed without a loop, but it's simple and obvious as
   * a loop.
   */
  while (name1chars + name2chars > availchars) {
    if (name1chars > name2chars)
      name1chars--;
    else
      name2chars--;
  }

  name1chars = pg_mbcliplen(name1, name1chars, name1chars);
  if (name2)
    name2chars = pg_mbcliplen(name2, name2chars, name2chars);

  /* Now construct the string using the chosen lengths */
  name = (char *)palloc(name1chars + name2chars + overhead + 1);
  memcpy(name, name1, name1chars);
  ndx = name1chars;
  if (name2) {
    name[ndx++] = '_';
    memcpy(name + ndx, name2, name2chars);
    ndx += name2chars;
  }
  if (label) {
    name[ndx++] = '_';
    strcpy(name + ndx, label);
  } else
    name[ndx] = '\0';

  return name;
}

/*
 * Select a nonconflicting name for a new relation.  This is ordinarily
 * used to choose index names (which is why it's here) but it can also
 * be used for sequences, or any autogenerated relation kind.
 *
 * name1, name2, and label are used the same way as for makeObjectName(),
 * except that the label can't be NULL; digits will be appended to the label
 * if needed to create a name that is unique within the specified namespace.
 *
 * A constraint's name avoids every relation's, as every name does: a
 * constraint is an index in sm, so its name is a relation's (isconstraint is
 * kept for the callers).
 *
 * Note: someone else may choose the same name concurrently; sm refuses the
 * second to create it.  Choosing several names in one command, create each
 * object before choosing the next.
 *
 * Returns a palloc'd string.
 */
char *ChooseRelationName(const char *name1, const char *name2,
                         const char *label, Oid namespaceid,
                         bool isconstraint) {
  int pass = 0;
  char *relname = NULL;
  char modlabel[NAMEDATALEN];

  /* try the unmodified label first */
  strlcpy(modlabel, label, sizeof(modlabel));

  for (;;) {
    relname = makeObjectName(name1, name2, modlabel);

    /*
     * Is there any conflicting relation name?  A relation is sm's, which also
     * sees what this transaction made; a constraint is an index there, so
     * its name is a relation's too.
     */
    if (!OidIsValid(get_relname_relid(relname, namespaceid)))
      break;

    /* found a conflict, so try a new name component */
    pfree(relname);
    snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
  }

  return relname;
}

/*
 * Select the name to be used for an index.
 *
 * The argument list is pretty ad-hoc :-(
 */
char *ChooseIndexName(const char *tabname, Oid namespaceId,
                      const List *colnames, const List *exclusionOpNames,
                      bool primary, bool isconstraint) {
  char *indexname;

  if (primary) {
    /* the primary key's name does not depend on the specific column(s) */
    indexname = ChooseRelationName(tabname, NULL, "pkey", namespaceId, true);
  } else if (exclusionOpNames != NIL) {
    indexname = ChooseRelationName(tabname, ChooseIndexNameAddition(colnames),
                                   "excl", namespaceId, true);
  } else if (isconstraint) {
    indexname = ChooseRelationName(tabname, ChooseIndexNameAddition(colnames),
                                   "key", namespaceId, true);
  } else {
    indexname = ChooseRelationName(tabname, ChooseIndexNameAddition(colnames),
                                   "idx", namespaceId, false);
  }

  return indexname;
}

/*
 * Generate "name2" for a new index given the list of column names for it
 * (as produced by ChooseIndexColumnNames).  This will be passed to
 * ChooseRelationName along with the parent table name and a suitable label.
 *
 * We know that less than NAMEDATALEN characters will actually be used,
 * so we can truncate the result once we've generated that many.
 *
 * XXX See also ChooseForeignKeyConstraintNameAddition and
 * ChooseExtendedStatisticNameAddition.
 */
static char *ChooseIndexNameAddition(const List *colnames) {
  char buf[NAMEDATALEN * 2];
  int buflen = 0;
  ListCell *lc;

  buf[0] = '\0';
  foreach (lc, colnames) {
    const char *name = (const char *)lfirst(lc);

    if (buflen > 0)
      buf[buflen++] = '_'; /* insert _ between names */

    /*
     * At this point we have buflen <= NAMEDATALEN.  name should be less
     * than NAMEDATALEN already, but use strlcpy for paranoia.
     */
    strlcpy(buf + buflen, name, NAMEDATALEN);
    buflen += strlen(buf + buflen);
    if (buflen >= NAMEDATALEN)
      break;
  }
  return pstrdup(buf);
}

/*
 * Select the actual names to be used for the columns of an index, given the
 * list of IndexElems for the columns.  This is mostly about ensuring the
 * names are unique so we don't get a conflicting-attribute-names error.
 *
 * Returns a List of plain strings (char *, not String nodes).
 */
List *ChooseIndexColumnNames(const List *indexElems) {
  List *result = NIL;
  ListCell *lc;

  foreach (lc, indexElems) {
    IndexElem *ielem = (IndexElem *)lfirst(lc);
    const char *origname;
    const char *curname;
    int i;
    char buf[NAMEDATALEN];

    /* Get the preliminary name from the IndexElem */
    if (ielem->indexcolname)
      origname = ielem->indexcolname; /* caller-specified name */
    else if (ielem->name)
      origname = ielem->name; /* simple column reference */
    else
      origname = "expr"; /* default name for expression */

    /* If it conflicts with any previous column, tweak it */
    curname = origname;
    for (i = 1;; i++) {
      ListCell *lc2;
      char nbuf[32];
      int nlen;

      foreach (lc2, result) {
        if (strcmp(curname, (char *)lfirst(lc2)) == 0)
          break;
      }
      if (lc2 == NULL)
        break; /* found nonconflicting name */

      sprintf(nbuf, "%d", i);

      /* Ensure generated names are shorter than NAMEDATALEN */
      nlen = pg_mbcliplen(origname, strlen(origname),
                          NAMEDATALEN - 1 - strlen(nbuf));
      memcpy(buf, origname, nlen);
      strcpy(buf + nlen, nbuf);
      curname = buf;
    }

    /* And attach to the result list */
    result = lappend(result, pstrdup(curname));
  }
  return result;
}

/*
 * ExecReindex -- REINDEX, of any kind: does nothing, with a warning.  sm
 * keeps an index's entries with its table's rows, so there is nothing to
 * rebuild.
 */
void ExecReindex(ParseState *pstate, const ReindexStmt *stmt, bool isTopLevel) {
  ereport(WARNING, (errmsg("REINDEX does nothing: sm keeps an index's "
                           "entries with its table's rows")));
}

/*
 * Insert or delete an appropriate pg_inherits tuple to make the given index
 * be a partition of the indicated parent index.
 *
 * This also corrects the pg_depend information for the affected index.
 */
void IndexSetParentIndex(Relation partitionIdx, Oid parentOid) {
  Relation pg_inherits;
  ScanKeyData key[2];
  SysScanDesc scan;
  Oid partRelid = RelationGetRelid(partitionIdx);
  HeapTuple tuple;
  bool fix_dependencies;

  /* Make sure this is an index */
  Assert(partitionIdx->rd_rel->relkind == RELKIND_INDEX ||
         partitionIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

  /*
   * Scan pg_inherits for rows linking our index to some parent.
   */
  pg_inherits = relation_open(InheritsRelationId, RowExclusiveLock);
  ScanKeyInit(&key[0], Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
              F_OIDEQ, ObjectIdGetDatum(partRelid));
  ScanKeyInit(&key[1], Anum_pg_inherits_inhseqno, BTEqualStrategyNumber,
              F_INT4EQ, Int32GetDatum(1));
  scan = systable_beginscan(pg_inherits, InheritsRelidSeqnoIndexId, true, NULL,
                            2, key);
  tuple = systable_getnext(scan);

  if (!HeapTupleIsValid(tuple)) {
    if (parentOid == InvalidOid) {
      /*
       * No pg_inherits row, and no parent wanted: nothing to do in this
       * case.
       */
      fix_dependencies = false;
    } else {
      StoreSingleInheritance(partRelid, parentOid, 1);
      fix_dependencies = true;
    }
  } else {
    Form_pg_inherits inhForm = (Form_pg_inherits)GETSTRUCT(tuple);

    if (parentOid == InvalidOid) {
      /*
       * There exists a pg_inherits row, which we want to clear; do so.
       */
      CatalogTupleDelete(pg_inherits, &tuple->t_self);
      fix_dependencies = true;
    } else {
      /*
       * A pg_inherits row exists.  If it's the same we want, then we're
       * good; if it differs, that amounts to a corrupt catalog and
       * should not happen.
       */
      if (inhForm->inhparent != parentOid) {
        /* unexpected: we should not get called in this case */
        elog(ERROR, "bogus pg_inherit row: inhrelid %u inhparent %u",
             inhForm->inhrelid, inhForm->inhparent);
      }

      /* already in the right state */
      fix_dependencies = false;
    }
  }

  /* done with pg_inherits */
  systable_endscan(scan);
  relation_close(pg_inherits, RowExclusiveLock);

  /* set relhassubclass if an index partition has been added to the parent */
  if (OidIsValid(parentOid)) {
    LockRelationOid(parentOid, ShareUpdateExclusiveLock);
    SetRelationHasSubclass(parentOid, true);
  }

  /* set relispartition correctly on the partition */
  update_relispartition(partRelid, OidIsValid(parentOid));

  if (fix_dependencies) {
    /*
     * Insert/delete pg_depend rows.  If setting a parent, add PARTITION
     * dependencies on the parent index and the table; if removing a
     * parent, delete PARTITION dependencies.
     */
    if (OidIsValid(parentOid)) {
      ObjectAddress partIdx;
      ObjectAddress parentIdx;
      ObjectAddress partitionTbl;

      ObjectAddressSet(partIdx, RelationRelationId, partRelid);
      ObjectAddressSet(parentIdx, RelationRelationId, parentOid);
      ObjectAddressSet(partitionTbl, RelationRelationId,
                       partitionIdx->rd_index->indrelid);
      recordDependencyOn(&partIdx, &parentIdx, DEPENDENCY_PARTITION_PRI);
      recordDependencyOn(&partIdx, &partitionTbl, DEPENDENCY_PARTITION_SEC);
    } else {
      deleteDependencyRecordsForClass(RelationRelationId, partRelid,
                                      RelationRelationId,
                                      DEPENDENCY_PARTITION_PRI);
      deleteDependencyRecordsForClass(RelationRelationId, partRelid,
                                      RelationRelationId,
                                      DEPENDENCY_PARTITION_SEC);
    }

    /* make our updates visible */
    CommandCounterIncrement();
  }
}

/*
 * Subroutine of IndexSetParentIndex to update the relispartition flag of the
 * given index to the given value.
 */
static void update_relispartition(Oid relationId, bool newval) {
  HeapTuple tup;
  Relation classRel;
  ItemPointerData otid;

  classRel = table_open(RelationRelationId, RowExclusiveLock);
  tup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relationId));
  if (!HeapTupleIsValid(tup))
    elog(ERROR, "cache lookup failed for relation %u", relationId);
  otid = tup->t_self;
  Assert(((Form_pg_class)GETSTRUCT(tup))->relispartition != newval);
  ((Form_pg_class)GETSTRUCT(tup))->relispartition = newval;
  CatalogTupleUpdate(classRel, &otid, tup);
  heap_freetuple(tup);
  table_close(classRel, RowExclusiveLock);
}

/*
 * Set the PROC_IN_SAFE_IC flag in the session's status flags, which digres
 * does not keep.
 *
 * When doing concurrent index builds, we can set this flag
 * to tell other processes concurrently running CREATE
 * INDEX CONCURRENTLY or REINDEX CONCURRENTLY to ignore us when
 * doing their waits for concurrent snapshots.  On one hand it
 * avoids pointlessly waiting for a process that's not interesting
 * anyway; but more importantly it avoids deadlocks in some cases.
 *
 * This can be done safely only for indexes that don't execute any
 * expressions that could access other tables, so index must not be
 * expressional nor partial.  Caller is responsible for only calling
 * this routine when that assumption holds true.
 *
 * (The flag is reset automatically at transaction end, so it must be
 * set for each transaction.)
 */
static inline void set_indexsafe_procflags(void) {
  /*
   * This should only be called before installing the session's xmin;
   * otherwise, concurrent processes could see an Xmin that moves backwards.
   */
  Assert(tcb->xmin == InvalidTransactionId);

  /*
   * No flag is set: PostgreSQL's PROC_IN_SAFE_IC told other sessions'
   * concurrent index builds to skip this one, and none waits for another
   * (WaitForOlderSnapshots()).
   */
}
