/*-------------------------------------------------------------------------
 *
 * smstore.c
 *	  digres's way into sm, the storage stack: its catalog of what a user
 *	  creates, and its tables (access/smstore.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/sm/smstore.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <vector>

#include "access/smstore.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/datadir.h"

/* Whether this session's PostgreSQL transaction has begun an sm one. */
static thread_local /*LOCAL*/ bool sm_xact_open = false;

/* Whether this thread has registered xact_callback(). */
static thread_local /*LOCAL*/ bool xact_callback_registered = false;

/* This session's sm session, or an error if it has none. */
static sm_session_t *session(void) {
  sm_session_t *s = datadir_sm_session();

  if (s == NULL)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("this session has no storage")));
  return s;
}

/* The SQLSTATE of the error sm left. */
static int sm_sqlstate(void) {
  switch (sm_errnum()) {
  case SM_ERR_DEADLOCK:
    return ERRCODE_T_R_DEADLOCK_DETECTED;
  case SM_ERR_INTERRUPTED:
    return ERRCODE_QUERY_CANCELED;
  default:
    return sm_retryable() == 1 ? ERRCODE_T_R_SERIALIZATION_FAILURE
                               : ERRCODE_INTERNAL_ERROR;
  }
}

/* Raise the error sm left, saying what was being done. */
static void sm_error(const char *what) {
  ereport(ERROR, (errcode(sm_sqlstate()),
                  errmsg("could not %s: %s", what, sm_errmsg())));
}

/*
 * End the sm transaction with the PostgreSQL one: commit it before
 * PostgreSQL commits, so that a commit sm refuses fails the transaction;
 * abort it as PostgreSQL aborts.
 */
static void xact_callback(XactEvent event, void *arg) {
  if (!sm_xact_open)
    return;
  switch (event) {
  case XACT_EVENT_PRE_COMMIT:
    sm_xact_open = false;
    if (sm_commit(session()) != 0)
      sm_error("commit");
    break;
  case XACT_EVENT_ABORT:
    sm_xact_open = false;
    /* An abort that fails here has nowhere to go: sm ends it anyway. */
    (void)sm_abort(session());
    break;
  default:
    break;
  }
}

/* sm's isolation level for PostgreSQL's. */
static sm_xctlevel_t xact_level(void) {
  switch (XactIsoLevel) {
  case XACT_SERIALIZABLE:
    return SM_XCT_SERIALIZABLE;
  case XACT_REPEATABLE_READ:
    return SM_XCT_REPEATABLE_READ;
  default:
    return SM_XCT_READ_COMMITTED;
  }
}

/*
 * This session's sm session, in an sm transaction: begun at the first call
 * that needs one in the PostgreSQL transaction, at its isolation level, and
 * ended with it (xact_callback()).
 */
static sm_session_t *xact_session(void) {
  sm_session_t *s = session();

  if (sm_xact_open)
    return s;
  if (!xact_callback_registered) {
    /* Once a thread: it lasts as long as the session. */
    RegisterXactCallback(xact_callback, NULL);
    xact_callback_registered = true;
  }
  if (sm_begin(s, xact_level(), XactReadOnly) != 0)
    sm_error("begin a transaction");
  sm_xact_open = true;
  return s;
}

/* sm's type code for a column of type typid (sm2/TYPE.md section 3). */
static uint8_t type_code(Oid typid) {
  switch (typid) {
  case INT2OID:
    return 2;
  case INT4OID:
    return 3;
  case INT8OID:
    return 4;
  default:
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("type %s is not supported yet", format_type_be(typid))));
  }
  return 0; /* not reached */
}

/* The oid of the namespace named name, or InvalidOid if there is none. */
Oid smstore_namespace_oid(const char *name) {
  uint64_t oid;

  switch (sm_cat_namespace_oid(session(), name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    return InvalidOid;
  default:
    sm_error("look up a schema");
  }
  return InvalidOid; /* not reached */
}

/*
 * The oid of the relation named name in nspoid, a table or an index, or
 * InvalidOid if there is none.
 */
Oid smstore_relation_oid(const char *name, Oid nspoid) {
  uint64_t oid, table;

  switch (sm_cat_table_oid(session(), nspoid, name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    break;
  default:
    sm_error("look up a table");
  }
  switch (sm_cat_index_oid(session(), nspoid, name, &table, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    return InvalidOid;
  default:
    sm_error("look up an index");
  }
  return InvalidOid; /* not reached */
}

/* Whether constraints, a column's, are NOT NULL alone, if any. */
static bool only_not_null(List *constraints) {
  ListCell *l;

  foreach (l, constraints)
    if (lfirst_node(Constraint, l)->contype != CONSTR_NOTNULL)
      return false;
  return true;
}

/* Refuse what a CREATE TABLE asks that sm's tables do not have yet. */
static void check_create_table(const CreateStmt *stmt) {
  if (stmt->inhRelations != NIL || stmt->partbound != NULL ||
      stmt->partspec != NULL || stmt->ofTypename != NULL ||
      stmt->constraints != NIL || stmt->options != NIL ||
      stmt->tablespacename != NULL || stmt->accessMethod != NULL ||
      stmt->oncommit != ONCOMMIT_NOOP ||
      stmt->relation->relpersistence != RELPERSISTENCE_PERMANENT)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE TABLE supports only columns and NOT NULL "
                           "yet")));
}

/*
 * Create the table a CREATE TABLE, after parse analysis, describes, in sm,
 * in this transaction: its columns and their NOT NULL.  Returns its address.
 */
ObjectAddress smstore_create_table(CreateStmt *stmt) {
  std::vector<sm_column_spec_t> columns;
  Oid nspoid;
  uint64_t oid;
  ObjectAddress address;
  ListCell *l;

  check_create_table(stmt);
  nspoid = RangeVarGetCreationNamespace(stmt->relation);
  foreach (l, stmt->tableElts) {
    ColumnDef *col = lfirst_node(ColumnDef, l);
    Oid typid;
    int32 typmod;

    if (col->collClause != NULL || col->raw_default != NULL ||
        col->cooked_default != NULL || col->identity != 0 ||
        col->generated != 0 || !only_not_null(col->constraints))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("column \"%s\" asks for what CREATE TABLE "
                             "does not support yet",
                             col->colname)));
    typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
    columns.push_back(
        {col->colname, type_code(typid), typmod, col->is_not_null});
  }
  if (sm_cat_create_table(xact_session(), nspoid, stmt->relation->relname,
                          columns, &oid) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists",
                             stmt->relation->relname)));
    sm_error("create a table");
  }
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}
