/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/view.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/smstore.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

static void checkViewColumns(TupleDesc newdesc, TupleDesc olddesc);

/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * Create a view in sm, or replace one's query: sm keeps the query, and
 * the relation cache makes the view's rule of it (access/smstore.h).
 *
 * EventTriggerAlterTableStart must have been called already.
 *---------------------------------------------------------------------
 */
static ObjectAddress DefineVirtualRelation(RangeVar *relation, List *tlist,
                                           bool replace, List *options,
                                           Query *viewParse) {
  Oid viewOid;
  LOCKMODE lockmode;
  List *attrList;
  ListCell *t;

  /* Not kept in sm yet (../sm/VIEW.md section 9). */
  if (relation->relpersistence == RELPERSISTENCE_TEMP)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("temporary views are not supported yet")));

  /*
   * create a list of ColumnDef nodes based on the names and types of the
   * (non-junk) targetlist items from the view's SELECT list.
   */
  attrList = NIL;
  foreach (t, tlist) {
    TargetEntry *tle = (TargetEntry *)lfirst(t);

    if (!tle->resjunk) {
      ColumnDef *def = makeColumnDef(tle->resname, exprType((Node *)tle->expr),
                                     exprTypmod((Node *)tle->expr),
                                     exprCollation((Node *)tle->expr));

      /*
       * It's possible that the column is of a collatable type but the
       * collation could not be resolved, so double-check.
       */
      if (type_is_collatable(exprType((Node *)tle->expr))) {
        if (!OidIsValid(def->collOid))
          ereport(
              ERROR,
              (errcode(ERRCODE_INDETERMINATE_COLLATION),
               errmsg("could not determine which collation to use for view "
                      "column \"%s\"",
                      def->colname),
               errhint(
                   "Use the COLLATE clause to set the collation explicitly.")));
      } else
        Assert(!OidIsValid(def->collOid));

      attrList = lappend(attrList, def);
    }
  }

  /*
   * Look up, check permissions on, and lock the creation namespace; also
   * check for a preexisting view with the same name.  This will also set
   * relation->relpersistence to RELPERSISTENCE_TEMP if the selected
   * namespace is temporary.
   */
  lockmode = replace ? AccessExclusiveLock : NoLock;
  (void)RangeVarGetAndCheckCreationNamespace(relation, lockmode, &viewOid);

  if (OidIsValid(viewOid) && replace) {
    Relation rel;

    /* Relation is already locked, but we must build a relcache entry. */
    rel = relation_open(viewOid, NoLock);

    /* Make sure it *is* a view. */
    if (rel->rd_rel->relkind != RELKIND_VIEW)
      ereport(ERROR,
              (errcode(ERRCODE_WRONG_OBJECT_TYPE),
               errmsg("\"%s\" is not a view", RelationGetRelationName(rel))));

    /* Also check it's not in use already */
    CheckTableNotInUse(rel, "CREATE OR REPLACE VIEW");

    /*
     * Verify that the old column list is an initial prefix of the new
     * column list.  A view's columns are its query's select list in sm
     * (access/smstore.h), so new ones need nothing added.
     */
    checkViewColumns(BuildDescForRelation(attrList), rel->rd_att);
    relation_close(rel, NoLock); /* keep the lock! */

    /*
     * The query, in sm, in place of the view's _RETURN rule, and the
     * options, which replace the view's, even if there are none.
     */
    return smstore_replace_view(viewOid, relation->relname, viewParse, options);
  }

  /*
   * The view, in sm: its query in place of a relation and its rule.  A name
   * a relation already has is an error, replace or not.
   */
  return smstore_create_view(relation, viewParse, options);
}

/*
 * Verify that the columns associated with proposed new view definition match
 * the columns of the old view.  This is similar to equalRowTypes(), with code
 * added to generate specific complaints.  Also, we allow the new view to have
 * more columns than the old.
 */
static void checkViewColumns(TupleDesc newdesc, TupleDesc olddesc) {
  int i;

  if (newdesc->natts < olddesc->natts)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("cannot drop columns from view")));

  for (i = 0; i < olddesc->natts; i++) {
    Form_pg_attribute newattr = TupleDescAttr(newdesc, i);
    Form_pg_attribute oldattr = TupleDescAttr(olddesc, i);

    /* XXX msg not right, but we don't support DROP COL on view anyway */
    if (newattr->attisdropped != oldattr->attisdropped)
      ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                      errmsg("cannot drop columns from view")));

    if (strcmp(NameStr(newattr->attname), NameStr(oldattr->attname)) != 0)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
               errmsg("cannot change name of view column \"%s\" to \"%s\"",
                      NameStr(oldattr->attname), NameStr(newattr->attname)),
               errhint("Use ALTER VIEW ... RENAME COLUMN ... to change name of "
                       "view column instead.")));

    /*
     * We cannot allow type, typmod, or collation to change, since these
     * properties may be embedded in Vars of other views/rules referencing
     * this one.  Other column attributes can be ignored.
     */
    if (newattr->atttypid != oldattr->atttypid ||
        newattr->atttypmod != oldattr->atttypmod)
      ereport(
          ERROR,
          (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
           errmsg(
               "cannot change data type of view column \"%s\" from %s to %s",
               NameStr(oldattr->attname),
               format_type_with_typemod(oldattr->atttypid, oldattr->atttypmod),
               format_type_with_typemod(newattr->atttypid,
                                        newattr->atttypmod))));

    /*
     * At this point, attcollations should be both valid or both invalid,
     * so applying get_collation_name unconditionally should be fine.
     */
    if (newattr->attcollation != oldattr->attcollation)
      ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                      errmsg("cannot change collation of view column \"%s\" "
                             "from \"%s\" to \"%s\"",
                             NameStr(oldattr->attname),
                             get_collation_name(oldattr->attcollation),
                             get_collation_name(newattr->attcollation))));
  }

  /*
   * We ignore the constraint fields.  The new view desc can't have any
   * constraints, and the only ones that could be on the old view are
   * defaults, which we are happy to leave in place.
   */
}

static void DefineViewRules(Oid viewOid, Query *viewParse, bool replace) {
  /*
   * Set up the ON SELECT rule.  Since the query has already been through
   * parse analysis, we use DefineQueryRewrite() directly.
   */
  DefineQueryRewrite(pstrdup(ViewSelectRuleName), viewOid, NULL, CMD_SELECT,
                     true, replace, list_make1(viewParse));

  /*
   * Someday: automatic ON INSERT, etc
   */
}

/*
 * DefineView
 *		Execute a CREATE VIEW command.
 */
ObjectAddress DefineView(ViewStmt *stmt, const char *queryString,
                         int stmt_location, int stmt_len) {
  RawStmt *rawstmt;
  Query *viewParse;
  RangeVar *view;
  ListCell *cell;
  bool check_option;
  ObjectAddress address;

  /*
   * Run parse analysis to convert the raw parse tree to a Query.  Note this
   * also acquires sufficient locks on the source table(s).
   */
  rawstmt = makeNode(RawStmt);
  rawstmt->stmt = stmt->query;
  rawstmt->stmt_location = stmt_location;
  rawstmt->stmt_len = stmt_len;

  viewParse = parse_analyze_fixedparams(rawstmt, queryString, NULL, 0, NULL);

  /*
   * The grammar should ensure that the result is a single SELECT Query.
   * However, it doesn't forbid SELECT INTO, so we have to check for that.
   */
  if (!IsA(viewParse, Query))
    elog(ERROR, "unexpected parse analysis result");
  if (viewParse->utilityStmt != NULL &&
      IsA(viewParse->utilityStmt, CreateTableAsStmt))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("views must not contain SELECT INTO")));
  if (viewParse->commandType != CMD_SELECT)
    elog(ERROR, "unexpected parse analysis result");

  /*
   * Check for unsupported cases.  These tests are redundant with ones in
   * DefineQueryRewrite(), but that function will complain about a bogus ON
   * SELECT rule, and we'd rather the message complain about a view.
   */
  if (viewParse->hasModifyingCTE)
    ereport(
        ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("views must not contain data-modifying statements in WITH")));

  /*
   * If the user specified the WITH CHECK OPTION, add it to the list of
   * reloptions.
   */
  if (stmt->withCheckOption == LOCAL_CHECK_OPTION)
    stmt->options =
        lappend(stmt->options,
                makeDefElem("check_option", (Node *)makeString("local"), -1));
  else if (stmt->withCheckOption == CASCADED_CHECK_OPTION)
    stmt->options =
        lappend(stmt->options, makeDefElem("check_option",
                                           (Node *)makeString("cascaded"), -1));

  /*
   * Check that the view is auto-updatable if WITH CHECK OPTION was
   * specified.
   */
  check_option = false;

  foreach (cell, stmt->options) {
    DefElem *defel = (DefElem *)lfirst(cell);

    if (strcmp(defel->defname, "check_option") == 0)
      check_option = true;
  }

  /*
   * If the check option is specified, look to see if the view is actually
   * auto-updatable or not.
   */
  if (check_option) {
    const char *view_updatable_error =
        view_query_is_auto_updatable(viewParse, true);

    if (view_updatable_error)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("WITH CHECK OPTION is supported only on "
                             "automatically updatable views"),
                      errhint("%s", _(view_updatable_error))));
  }

  /*
   * If a list of column names was given, run through and insert these into
   * the actual query tree. - thomas 2000-03-08
   */
  if (stmt->aliases != NIL) {
    ListCell *alist_item = list_head(stmt->aliases);
    ListCell *targetList;

    foreach (targetList, viewParse->targetList) {
      TargetEntry *te = lfirst_node(TargetEntry, targetList);

      /* junk columns don't get aliases */
      if (te->resjunk)
        continue;
      te->resname = pstrdup(strVal(lfirst(alist_item)));
      alist_item = lnext(stmt->aliases, alist_item);
      if (alist_item == NULL)
        break; /* done assigning aliases */
    }

    if (alist_item != NULL)
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("CREATE VIEW specifies more column "
                             "names than columns")));
  }

  /* Unlogged views are not sensible. */
  if (stmt->view->relpersistence == RELPERSISTENCE_UNLOGGED)
    ereport(
        ERROR,
        (errcode(ERRCODE_SYNTAX_ERROR),
         errmsg("views cannot be unlogged because they do not have storage")));

  /*
   * If the user didn't explicitly ask for a temporary view, check whether
   * we need one implicitly.  We allow TEMP to be inserted automatically as
   * long as the CREATE command is consistent with that --- no explicit
   * schema name.
   */
  view = copyObject(stmt->view); /* don't corrupt original command */
  if (view->relpersistence == RELPERSISTENCE_PERMANENT &&
      isQueryUsingTempRelation(viewParse)) {
    view->relpersistence = RELPERSISTENCE_TEMP;
    ereport(NOTICE,
            (errmsg("view \"%s\" will be a temporary view", view->relname)));
  }

  /*
   * Create the view relation
   *
   * NOTE: if it already exists and replace is false, the xact will be
   * aborted.
   */
  address = DefineVirtualRelation(view, viewParse->targetList, stmt->replace,
                                  stmt->options, viewParse);

  return address;
}

/*
 * Use the rules system to store the query for the view.
 */
void StoreViewQuery(Oid viewOid, Query *viewParse, bool replace) {
  /*
   * Now create the rules associated with the view.
   */
  DefineViewRules(viewOid, viewParse, replace);
}
