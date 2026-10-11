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
#include "utils/rel.h"
#include "utils/memutils.h"

/* Whether this session's PostgreSQL transaction has begun an sm one. */
static thread_local /*LOCAL*/ bool sm_xact_open = false;

/*
 * Whether a statement has begun since sm last heard of one: the next call
 * in the sm transaction begins one there too (smstore_statement_start()).
 */
static thread_local /*LOCAL*/ bool statement_pending = false;

/* Whether this thread has registered xact_callback(). */
static thread_local /*LOCAL*/ bool xact_callback_registered = false;

/* A table this transaction opened in sm, to read or to write. */
typedef struct open_table_t {
  Oid oid;
  bool write;
  sm_tbl_t *tbl;
} open_table_t;

/*
 * The tables this transaction opened, open_table_t's in
 * TopTransactionContext, closed as it ends.
 */
static thread_local /*LOCAL*/ List *open_tables = NIL;

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
  ListCell *l;

  if (!sm_xact_open)
    return;
  if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_ABORT) {
    /* A handle is good for its transaction only. */
    foreach (l, open_tables)
      sm_tbl_close(((open_table_t *)lfirst(l))->tbl);
    open_tables = NIL;
  }
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

/* Begin sm's statement, if PostgreSQL has begun one since it last did. */
static void statement_begin(sm_session_t *s) {
  if (!statement_pending)
    return;
  statement_pending = false;
  if (sm_stmt_begin(s) != 0)
    sm_error("begin a statement");
}

/*
 * This session's sm session, in an sm transaction: begun at the first call
 * that needs one in the PostgreSQL transaction, at its isolation level, and
 * ended with it (xact_callback()).
 */
static sm_session_t *xact_session(void) {
  sm_session_t *s = session();

  if (sm_xact_open) {
    statement_begin(s);
    return s;
  }
  if (!xact_callback_registered) {
    /* Once a thread: it lasts as long as the session. */
    RegisterXactCallback(xact_callback, NULL);
    xact_callback_registered = true;
  }
  if (sm_begin(s, xact_level(), XactReadOnly) != 0)
    sm_error("begin a transaction");
  sm_xact_open = true;
  statement_pending = true;
  statement_begin(s);
  return s;
}

/*
 * A statement has begun: the next call in the sm transaction begins one in
 * sm, so that the statement reads at one snapshot under READ COMMITTED, and
 * does not see the rows it writes itself (sm_stmt_begin).
 */
void smstore_statement_start(void) { statement_pending = true; }

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

/*
 * The table oid, opened in this transaction to read, or to write, taking
 * the lock that goes with it until the transaction ends; NULL if there is
 * no such table.
 */
static sm_tbl_t *open_table(Oid oid, bool write) {
  sm_session_t *s = xact_session();
  open_table_t *ot;
  sm_tbl_t *tbl;
  MemoryContext old;
  ListCell *l;

  foreach (l, open_tables) {
    ot = (open_table_t *)lfirst(l);
    if (ot->oid == oid && (ot->write || !write))
      return ot->tbl;
  }
  if (sm_tbl_open(s, oid, write ? SM_LOCKMODE_IX : SM_LOCKMODE_IS, &tbl) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      return NULL;
    sm_error("open a table");
  }
  /* Kept until the transaction ends, which closes it. */
  old = MemoryContextSwitchTo(TopTransactionContext);
  ot = (open_table_t *)palloc(sizeof(open_table_t));
  ot->oid = oid;
  ot->write = write;
  ot->tbl = tbl;
  open_tables = lappend(open_tables, ot);
  MemoryContextSwitchTo(old);
  return tbl;
}

/* PostgreSQL's type for sm's type code (sm2/TYPE.md section 3). */
static Oid type_oid(uint8_t code) {
  switch (code) {
  case 2:
    return INT2OID;
  case 3:
    return INT4OID;
  case 4:
    return INT8OID;
  default:
    elog(ERROR, "sm type code %d has no type here", code);
  }
  return InvalidOid; /* not reached */
}

/*
 * The table oid as sm3 defines it, palloc'd in the current context, or
 * NULL if there is no such table.  It opens the table to read, in this
 * transaction.
 */
smstore_table_t *smstore_table_desc(Oid oid) {
  sm_tbl_t *tbl = open_table(oid, false);
  const sm_table_def_t *def;
  smstore_table_t *desc;

  if (tbl == NULL)
    return NULL;
  def = sm_tbl_def(tbl);
  desc = (smstore_table_t *)palloc(sizeof(smstore_table_t));
  desc->oid = oid;
  desc->nspoid = (Oid)def->nspoid;
  desc->name = pstrdup(def->name.c_str());
  desc->ncolumns = (int)def->columns.size();
  desc->columns =
      (smstore_column_t *)palloc0(desc->ncolumns * sizeof(smstore_column_t));
  for (int i = 0; i < desc->ncolumns; i++) {
    const sm_column_def_t &c = def->columns[i];
    smstore_column_t *col = &desc->columns[i];

    col->name = pstrdup(c.name.c_str());
    col->dropped = c.dropped;
    if (!c.dropped) {
      col->typid = type_oid(c.type_code);
      col->typmod = c.typmod;
      col->not_null = c.not_null;
    }
  }
  return desc;
}

/*
 * A row's rowno as a tid, for the slot that carries it: a tid is 48 bits,
 * and an offset of 0 is no tid, so each block holds 65535 rownos, the
 * offset one above the rowno's remainder.
 */
void smstore_rowno_to_tid(uint64 rowno, ItemPointer tid) {
  if (rowno / 0xFFFF > MaxBlockNumber)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("row number %llu does not fit a tid",
                           (unsigned long long)rowno)));
  ItemPointerSet(tid, (BlockNumber)(rowno / 0xFFFF),
                 (OffsetNumber)(rowno % 0xFFFF + 1));
}

/* The rowno a tid carries (smstore_rowno_to_tid()). */
uint64 smstore_tid_to_rowno(ItemPointer tid) {
  return (uint64)ItemPointerGetBlockNumber(tid) * 0xFFFF +
         ItemPointerGetOffsetNumber(tid) - 1;
}

/* Insert the slot's row into rel, a table in sm; its tid is its rowno. */
void smstore_insert(Relation rel, TupleTableSlot *slot) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), true);
  uint64_t rowno;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  slot_getallattrs(slot);
  if (sm_tbl_insert(tbl, (const sm_datum_t *)slot->tts_values, slot->tts_isnull,
                    slot->tts_nvalid, &rowno) != 0)
    sm_error("insert a row");
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
}

/* A scan of a table in sm: the table, and sm's cursor over its rows. */
struct smstore_scan_t {
  sm_tbl_t *tbl;
  sm_rel_scan_t *cursor;
};

/* Begin a scan of every row of rel, a table in sm, in rowno order. */
smstore_scan_t *smstore_beginscan(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), false);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  if (sm_tbl_scan(tbl, 0, UINT64_MAX, &scan->cursor) != 0)
    sm_error("scan a table");
  return scan;
}

/*
 * The scan's next row into slot, as a virtual tuple, its tid its rowno;
 * false at the end.  A value by reference points into sm's tuple, good
 * until the next row.
 */
bool smstore_getnext(smstore_scan_t *scan, TupleTableSlot *slot) {
  uint64_t rowno;
  const void *tuple;
  size_t len;

  ExecClearTuple(slot);
  switch (sm_rel_scannxt(scan->cursor, &rowno, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("scan a table");
  }
  if (sm_tbl_deform(scan->tbl, tuple, len, (sm_datum_t *)slot->tts_values,
                    slot->tts_isnull, slot->tts_tupleDescriptor->natts) != 0)
    sm_error("read a row");
  ExecStoreVirtualTuple(slot);
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
  return true;
}

/* End a scan; the table stays open until the transaction ends. */
void smstore_endscan(smstore_scan_t *scan) {
  sm_rel_scanend(scan->cursor);
  pfree(scan);
}
