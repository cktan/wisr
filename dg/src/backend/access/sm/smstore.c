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
#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "storage/lockdefs.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/datadir.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/memutils.h"

/* Whether this session's PostgreSQL transaction has begun an sm one. */
static thread_local /*LOCAL*/ bool sm_xact_open = false;

/*
 * Whether a statement has begun since sm last heard of one: the next call
 * in the sm transaction begins one there too (smstore_statement_start()).
 */
static thread_local /*LOCAL*/ bool statement_pending = false;

/* A table this transaction opened in sm, to read or to write. */
typedef struct open_table_t {
  Oid oid;
  sm_lockmode_t mode;
  sm_tbl_t *tbl;
} open_table_t;

/*
 * The tables this transaction opened, open_table_t's in
 * TopTransactionContext, closed as it ends.
 */
static thread_local /*LOCAL*/ List *open_tables = NIL;

/*
 * The depth of sm's savepoint for each subtransaction open, the one at
 * nesting level 2 first, in TopTransactionContext.  A subtransaction has one
 * from its first call into sm on: one that has made none has nothing to
 * undo, and gets its savepoint when it makes one (savepoints_begin()).
 */
static thread_local /*LOCAL*/ List *savepoints = NIL;

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
  case SM_ERR_DUPKEY:
    return ERRCODE_UNIQUE_VIOLATION;
  case SM_ERR_NOTNULL:
    return ERRCODE_NOT_NULL_VIOLATION;
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
 * Call f, a call into sm that may wait for another transaction, until it
 * does not fail on an interrupted wait.  An interrupt in this thread's wait
 * mask ends sm's wait, with SM_ERR_INTERRUPTED (miscinit.c sets the mask);
 * CHECK_FOR_INTERRUPTS() then acts on it, raising the error a cancel, an
 * end or a timeout calls for.  One that calls for nothing, or that must
 * wait until interrupts are allowed, has been taken: the call is made again,
 * and waits again.
 */
template <class F> static int waiting(F f) {
  for (;;) {
    int rc = f();

    if (rc == 0 || sm_errnum() != SM_ERR_INTERRUPTED)
      return rc;
    CHECK_FOR_INTERRUPTS();
  }
}

/* Close the tables this transaction opened: a handle is good for it only. */
static void close_tables(void) {
  ListCell *l;

  foreach (l, open_tables)
    sm_tbl_close(((open_table_t *)lfirst(l))->tbl);
  open_tables = NIL;
  savepoints = NIL;
}

/*
 * Commit the sm transaction, if one has begun: before the PostgreSQL one
 * commits, so that a commit sm refuses fails the transaction.
 */
void smstore_commit(void) {
  if (!sm_xact_open)
    return;
  close_tables();
  sm_xact_open = false;
  if (sm_commit(session()) != 0)
    sm_error("commit");
}

/* Abort the sm transaction, if one has begun, as the PostgreSQL one does. */
void smstore_abort(void) {
  if (!sm_xact_open)
    return;
  close_tables();
  sm_xact_open = false;
  /* An abort that fails here has nowhere to go: sm ends it anyway. */
  (void)sm_abort(session());
}

/*
 * The savepoint of the subtransaction ending, taken off the list, or 0 if it
 * made no call into sm and has none.
 */
static int subxact_savepoint(void) {
  int level = GetCurrentTransactionNestLevel();
  int depth;

  if (!sm_xact_open || list_length(savepoints) < level - 1)
    return 0;
  depth = list_nth_int(savepoints, level - 2);
  savepoints = list_truncate(savepoints, level - 2);
  return depth;
}

/* The subtransaction commits: its savepoint goes, its writes stay. */
void smstore_subcommit(void) {
  int depth = subxact_savepoint();

  /* A savepoint sm cannot forget is a transaction it ended: next call says. */
  if (depth > 0)
    (void)sm_forget(session(), depth);
}

/* The subtransaction aborts: its writes are undone, and its savepoint goes. */
void smstore_subabort(void) {
  int depth = subxact_savepoint();

  /*
   * In an abort there is nowhere for an error to go: a savepoint sm cannot
   * undo is a transaction sm has ended, and the next call says so.
   */
  if (depth > 0) {
    (void)sm_rollback(session(), depth);
    (void)sm_forget(session(), depth);
  }
}

/*
 * Give each subtransaction open a savepoint in sm, if it has none: it is
 * about to make its first call into sm, and nothing before it has anything
 * of its to undo.
 */
static void savepoints_begin(sm_session_t *s) {
  int level = GetCurrentTransactionNestLevel();
  MemoryContext old;

  while (list_length(savepoints) < level - 1) {
    int depth = sm_savepoint(s);

    if (depth < 0)
      sm_error("set a savepoint");
    /* As long as the transaction: the subtransaction's end lets it go. */
    old = MemoryContextSwitchTo(TopTransactionContext);
    savepoints = lappend_int(savepoints, depth);
    MemoryContextSwitchTo(old);
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
 * ended with it (smstore_commit(), smstore_abort()).
 */
static sm_session_t *xact_session(void) {
  sm_session_t *s = session();

  if (sm_xact_open) {
    statement_begin(s);
    savepoints_begin(s);
    return s;
  }
  if (sm_begin(s, xact_level(), XactReadOnly) != 0)
    sm_error("begin a transaction");
  sm_xact_open = true;
  statement_pending = true;
  statement_begin(s);
  savepoints_begin(s);
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

/*
 * Whether constraints, a column's, are only those sm's tables take: NOT
 * NULL, and a PRIMARY KEY or UNIQUE, which parse analysis has made an index
 * statement of its own.
 */
static bool only_taken(List *constraints) {
  ListCell *l;

  foreach (l, constraints) {
    ConstrType type = lfirst_node(Constraint, l)->contype;

    if (type != CONSTR_NOTNULL && type != CONSTR_PRIMARY &&
        type != CONSTR_UNIQUE)
      return false;
  }
  return true;
}

/*
 * Whether col is NOT NULL: said of it, or of its name in one of the
 * statement's NOT NULL constraints, as a PRIMARY KEY's columns are.
 */
static bool column_not_null(const CreateStmt *stmt, const ColumnDef *col) {
  ListCell *l, *k;

  if (col->is_not_null)
    return true;
  foreach (l, stmt->nnconstraints)
    foreach (k, lfirst_node(Constraint, l)->keys)
      if (strcmp(strVal(lfirst(k)), col->colname) == 0)
        return true;
  return false;
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
                    errmsg("CREATE TABLE supports only columns, NOT NULL, "
                           "PRIMARY KEY and UNIQUE yet")));
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
        col->generated != 0 || !only_taken(col->constraints))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("column \"%s\" asks for what CREATE TABLE "
                             "does not support yet",
                             col->colname)));
    typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
    columns.push_back(
        {col->colname, type_code(typid), typmod, column_not_null(stmt, col)});
  }
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_create_table(s, nspoid, stmt->relation->relname, columns,
                                   &oid);
      }) != 0) {
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
 * Whether a table held in mode held may be used as one opened in want: the
 * lock held is at least as strong, and a handle to write with is one opened
 * to write.
 */
static bool mode_covers(sm_lockmode_t held, sm_lockmode_t want) {
  switch (held) {
  case SM_LOCKMODE_EX:
    return true;
  case SM_LOCKMODE_SIX:
    return want != SM_LOCKMODE_EX;
  case SM_LOCKMODE_SH:
    return want == SM_LOCKMODE_SH || want == SM_LOCKMODE_IS;
  case SM_LOCKMODE_IX:
    return want == SM_LOCKMODE_IX || want == SM_LOCKMODE_IS;
  case SM_LOCKMODE_IS:
    return want == SM_LOCKMODE_IS;
  }
  return false;
}

/*
 * The table oid, opened in this transaction in mode, taking the lock that
 * goes with it until the transaction ends: IS to read, IX to write, a
 * stronger one for LOCK TABLE.  NULL if there is no such table.
 */
static sm_tbl_t *open_table(Oid oid, sm_lockmode_t mode) {
  sm_session_t *s = xact_session();
  open_table_t *ot;
  sm_tbl_t *tbl;
  MemoryContext old;
  ListCell *l;

  foreach (l, open_tables) {
    ot = (open_table_t *)lfirst(l);
    if (ot->oid == oid && mode_covers(ot->mode, mode))
      return ot->tbl;
  }
  if (waiting([&] { return sm_tbl_open(s, oid, mode, &tbl); }) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      return NULL;
    sm_error("open a table");
  }
  /* Kept until the transaction ends, which closes it. */
  old = MemoryContextSwitchTo(TopTransactionContext);
  ot = (open_table_t *)palloc(sizeof(open_table_t));
  ot->oid = oid;
  ot->mode = mode;
  ot->tbl = tbl;
  open_tables = lappend(open_tables, ot);
  MemoryContextSwitchTo(old);
  return tbl;
}

/* An index's table, by the index's oid. */
typedef struct index_table_t {
  Oid index; /* the hash key */
  Oid table;
} index_table_t;

/*
 * The table of each index of the tables smstore_table_desc() described, in
 * this thread: made on first use, in CacheMemoryContext, with the
 * descriptors.
 */
static thread_local /*LOCAL*/ HTAB *index_tables = NULL;

/* Remember that the index indexoid is table's. */
static void remember_index(Oid indexoid, Oid table) {
  index_table_t *entry;

  if (index_tables == NULL) {
    HASHCTL ctl;

    ctl.keysize = sizeof(Oid);
    ctl.entrysize = sizeof(index_table_t);
    ctl.hcxt = CacheMemoryContext;
    index_tables = hash_create("sm index tables", 64, &ctl,
                               HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
  }
  entry =
      (index_table_t *)hash_search(index_tables, &indexoid, HASH_ENTER, NULL);
  entry->table = table;
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
  sm_tbl_t *tbl = open_table(oid, SM_LOCKMODE_IS);
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
  desc->nindexes = (int)def->indexes.size();
  desc->indexes =
      (smstore_index_t *)palloc0(desc->nindexes * sizeof(smstore_index_t));
  for (int i = 0; i < desc->nindexes; i++) {
    const sm_index_def_t &x = def->indexes[i];
    smstore_index_t *ix = &desc->indexes[i];

    ix->oid = (Oid)x.oid;
    ix->name = pstrdup(x.name.c_str());
    ix->unique = x.unique;
    ix->usable = sm_tbl_index_usable(tbl, x.oid);
    ix->nkeys = (int)x.key_attnums.size();
    ix->ncolumns = ix->nkeys + (int)x.include_attnums.size();
    ix->attnums = (int16 *)palloc(ix->ncolumns * sizeof(int16));
    ix->desc = (bool *)palloc(ix->nkeys * sizeof(bool));
    ix->nulls_first = (bool *)palloc(ix->nkeys * sizeof(bool));
    for (int k = 0; k < ix->nkeys; k++) {
      ix->attnums[k] = (int16)(x.key_attnums[k] + 1);
      ix->desc[k] = x.key[k].desc;
      ix->nulls_first[k] = x.key[k].nulls_first;
    }
    for (size_t k = 0; k < x.include_attnums.size(); k++)
      ix->attnums[ix->nkeys + k] = (int16)(x.include_attnums[k] + 1);
    remember_index(ix->oid, oid);
  }
  return desc;
}

/*
 * The table of the index indexoid, as last described by
 * smstore_table_desc(), or InvalidOid if no table described has it: an
 * index's descriptor is built from its table's.
 */
Oid smstore_index_table(Oid indexoid) {
  index_table_t *entry;

  if (index_tables == NULL)
    return InvalidOid;
  entry =
      (index_table_t *)hash_search(index_tables, &indexoid, HASH_FIND, NULL);
  return entry != NULL ? entry->table : InvalidOid;
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
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IX);
  uint64_t rowno;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  slot_getallattrs(slot);
  if (waiting([&] {
        return sm_tbl_insert(tbl, (const sm_datum_t *)slot->tts_values,
                             slot->tts_isnull, slot->tts_nvalid, &rowno);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR,
              (errcode(ERRCODE_UNIQUE_VIOLATION),
               errmsg("duplicate key value violates a unique index on \"%s\"",
                      RelationGetRelationName(rel)),
               errdetail("%s", sm_errmsg())));
    sm_error("insert a row");
  }
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
}

/*
 * A scan of a table in sm: the table, and sm's cursor over its rows; or,
 * over one of its indexes, sm's cursor over the index's entries and the scan
 * keys every row it reads must meet, with each key's column in the table.
 */
struct smstore_scan_t {
  sm_tbl_t *tbl;
  sm_rel_scan_t *cursor;
  sm_idx_scan_t *icursor;
  ScanKey keys;
  int nkeys;
  AttrNumber *keycols;
  /* An index scan's: its index, and its last entry's value. */
  Oid index;
  Form_pg_index form;
  const void *value;
  size_t len;
  /* smstore_index_getentry()'s room for an entry's value and a row. */
  sm_datum_t *entry_values;
  bool *entry_isnull;
  size_t entry_ncols;
  Datum *row_values;
  bool *row_isnull;
  int row_natts;
};

/* Begin a scan of every row of rel, a table in sm, in rowno order. */
smstore_scan_t *smstore_beginscan(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
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
  sm_idx_scanend(scan->icursor);
  pfree(scan);
}

/*
 * Drop the tables a DROP TABLE names, from sm, in this transaction: each
 * with its indexes, rows and sequence.  CASCADE and RESTRICT are the same:
 * nothing but its indexes depends on a table in sm.
 */
void smstore_drop_tables(DropStmt *stmt) {
  ListCell *l;

  foreach (l, stmt->objects) {
    RangeVar *rv = makeRangeVarFromNameList((List *)lfirst(l));
    Oid oid;
    Relation rel;

    /* Locked against other sessions' use until the transaction ends. */
    oid = RangeVarGetRelid(rv, AccessExclusiveLock, true);
    if (!OidIsValid(oid)) {
      if (!stmt->missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("table \"%s\" does not exist", rv->relname)));
      ereport(NOTICE,
              (errmsg("table \"%s\" does not exist, skipping", rv->relname)));
      continue;
    }
    rel = relation_open(oid, NoLock);
    if (rel->rd_rel->relkind != RELKIND_RELATION)
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("\"%s\" is not a table", rv->relname)));
    /* Every session forgets its descriptor once this one commits. */
    CacheInvalidateRelcache(rel);
    relation_close(rel, NoLock);
    sm_session_t *s = xact_session();

    if (waiting([&] { return sm_cat_drop_table(s, oid); }) != 0)
      sm_error("drop a table");
  }
}

/*
 * sm's table lock for PostgreSQL's mode: the weakest that conflicts with at
 * least what the mode does.  sm has five; where PostgreSQL's falls between
 * two, the stronger is taken, and may conflict with more.
 */
static sm_lockmode_t table_lock_mode(LOCKMODE lockmode) {
  switch (lockmode) {
  case AccessShareLock:
  case RowShareLock:
    return SM_LOCKMODE_IS;
  case RowExclusiveLock:
    return SM_LOCKMODE_IX;
  case ShareLock:
    return SM_LOCKMODE_SH;
  case ShareUpdateExclusiveLock:
  case ShareRowExclusiveLock:
    return SM_LOCKMODE_SIX;
  default:
    return SM_LOCKMODE_EX;
  }
}

/*
 * LOCK TABLE on rel, a table in sm: sm's table lock in the mode closest to
 * lockmode, held until the transaction ends.  NOWAIT is refused: sm has no
 * way to ask for it yet.
 */
void smstore_lock_table(Relation rel, LOCKMODE lockmode, bool nowait) {
  if (nowait)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("LOCK TABLE ... NOWAIT is not supported yet")));
  if (open_table(RelationGetRelid(rel), table_lock_mode(lockmode)) == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
}

/* Refuse what a CREATE INDEX asks that sm's indexes do not have yet. */
static void check_create_index(const IndexStmt *stmt) {
  ListCell *l;

  if (strcmp(stmt->accessMethod, DEFAULT_INDEX_TYPE) != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" is not supported",
                           stmt->accessMethod)));
  if (stmt->whereClause != NULL || stmt->excludeOpNames != NIL ||
      stmt->options != NIL || stmt->tableSpace != NULL ||
      stmt->nulls_not_distinct || stmt->deferrable)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE INDEX supports only columns, UNIQUE and "
                           "INCLUDE yet")));
  foreach (l, stmt->indexParams) {
    IndexElem *elem = lfirst_node(IndexElem, l);

    if (elem->name == NULL || elem->collation != NIL || elem->opclass != NIL)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index key is a column, with no collation "
                             "or operator class, yet")));
  }
}

/* The 0-based place of rel's column name, or an error if it has none. */
static int32_t column_place(Relation rel, const char *name) {
  TupleDesc desc = RelationGetDescr(rel);

  for (int i = 0; i < desc->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(desc, i);

    if (!att->attisdropped && strcmp(NameStr(att->attname), name) == 0)
      return i;
  }
  ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                  errmsg("column \"%s\" does not exist", name)));
  return -1; /* not reached */
}

/*
 * Create the index a CREATE INDEX, after parse analysis, describes on relid,
 * a table in sm, in this transaction: btree, on columns, UNIQUE or not, with
 * INCLUDE columns.  On a table made in this transaction it is filled now; on
 * any other, once the transaction commits, in the background.  Returns its
 * address.
 */
ObjectAddress smstore_create_index(Oid relid, IndexStmt *stmt) {
  std::vector<sm_index_key_spec_t> keys;
  std::vector<int32_t> includes;
  Relation rel;
  char *name;
  uint64_t oid;
  ObjectAddress address;
  ListCell *l;

  check_create_index(stmt);
  rel = relation_open(relid, NoLock);
  foreach (l, stmt->indexParams) {
    IndexElem *elem = lfirst_node(IndexElem, l);
    sm_index_key_spec_t key = {};
    bool desc = elem->ordering == SORTBY_DESC;

    key.attnum = column_place(rel, elem->name);
    key.desc = desc;
    /* PostgreSQL's default puts nulls last ascending, first descending. */
    key.nulls_first = elem->nulls_ordering == SORTBY_NULLS_DEFAULT
                          ? desc
                          : elem->nulls_ordering == SORTBY_NULLS_FIRST;
    keys.push_back(key);
  }
  foreach (l, stmt->indexIncludingParams)
    includes.push_back(column_place(rel, lfirst_node(IndexElem, l)->name));
  name = stmt->idxname != NULL
             ? stmt->idxname
             : ChooseIndexName(RelationGetRelationName(rel),
                               RelationGetNamespace(rel),
                               ChooseIndexColumnNames(stmt->indexParams), NIL,
                               stmt->primary, stmt->isconstraint);
  /* Every session's descriptor of the table relearns its indexes. */
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);

  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_create_index(s, relid, name, keys, includes, stmt->unique,
                                   std::string(), {}, &oid);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
                      errmsg("could not create unique index \"%s\"", name),
                      errdetail("%s", sm_errmsg())));
    sm_error("create an index");
  }
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}

/*
 * The index named rv, and its table, in rv's namespace or the first on the
 * search path that has one: false if there is none.
 */
static bool find_index(const RangeVar *rv, uint64_t *table, uint64_t *index) {
  List *path;
  ListCell *l;

  if (rv->schemaname != NULL)
    path = list_make1_oid(get_namespace_oid(rv->schemaname, false));
  else
    path = fetch_search_path(false);
  foreach (l, path) {
    switch (
        sm_cat_index_oid(session(), lfirst_oid(l), rv->relname, table, index)) {
    case 0:
      return true;
    case 1:
      break;
    default:
      sm_error("look up an index");
    }
  }
  return false;
}

/*
 * Drop the indexes a DROP INDEX names, from sm, in this transaction.  A
 * table's descriptor forgets the index in every session at commit.
 */
void smstore_drop_indexes(DropStmt *stmt) {
  ListCell *l;

  if (stmt->concurrent)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DROP INDEX CONCURRENTLY is not supported")));
  foreach (l, stmt->objects) {
    RangeVar *rv = makeRangeVarFromNameList((List *)lfirst(l));
    uint64_t table, index;
    Relation rel;

    if (!find_index(rv, &table, &index)) {
      if (!stmt->missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("index \"%s\" does not exist", rv->relname)));
      ereport(NOTICE,
              (errmsg("index \"%s\" does not exist, skipping", rv->relname)));
      continue;
    }
    rel = relation_open((Oid)table, NoLock);
    /* Every session's descriptor of the table relearns its indexes. */
    CacheInvalidateRelcache(rel);
    relation_close(rel, NoLock);

    sm_session_t *s = xact_session();

    if (waiting([&] { return sm_cat_drop_index(s, table, index); }) != 0)
      sm_error("drop an index");
  }
}

/* Whether this transaction may scan rel's index indexoid now: built, valid. */
bool smstore_index_usable(Relation rel, Oid indexoid) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);

  return tbl != NULL && sm_tbl_index_usable(tbl, indexoid);
}

/* Whether a scan key is a plain comparison with a value, usable as a bound. */
static bool plain_key(ScanKey key, Oid coltype) {
  return (key->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_SEARCHARRAY |
                           SK_SEARCHNULL | SK_SEARCHNOTNULL)) == 0 &&
         (key->sk_subtype == InvalidOid || key->sk_subtype == coltype);
}

/*
 * Whether the row in slot meets every scan key: each compares the row's
 * value in the key's column, by the key's operator, with the key's value.
 * A null meets only IS NULL.  With index_shaped, slot holds an index
 * entry's columns, not a row's.
 */
static bool keys_met(smstore_scan_t *scan, TupleTableSlot *slot,
                     bool index_shaped) {
  for (int i = 0; i < scan->nkeys; i++) {
    ScanKey key = &scan->keys[i];
    int col = (index_shaped ? key->sk_attno : scan->keycols[i]) - 1;
    bool isnull = slot->tts_isnull[col];

    if (key->sk_flags & SK_SEARCHNULL) {
      if (!isnull)
        return false;
      continue;
    }
    if (key->sk_flags & SK_SEARCHNOTNULL) {
      if (isnull)
        return false;
      continue;
    }
    if (isnull || (key->sk_flags & SK_ISNULL))
      return false;
    if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
                                        slot->tts_values[col],
                                        key->sk_argument)))
      return false;
  }
  return true;
}

/*
 * Begin a scan of index, a btree of heap, a table in sm, for the rows that
 * meet keys: btree scan keys on the index's columns.  Equal keys on its
 * leading columns are a prefix both bounds share; the next column's ranges
 * bound it below and above, in the index's own order, so a DESC column's
 * upper value is its lower bound.  Every row read is checked against every
 * key, so a key that bounds nothing still holds.
 */
smstore_scan_t *smstore_index_beginscan(Relation heap, Relation index,
                                        ScanKey keys, int nkeys) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(heap), SM_LOCKMODE_IS);
  Form_pg_index form = index->rd_index;
  int ncols = form->indnkeyatts;
  sm_datum_t *lo_vals = (sm_datum_t *)palloc0(ncols * sizeof(sm_datum_t));
  sm_datum_t *hi_vals = (sm_datum_t *)palloc0(ncols * sizeof(sm_datum_t));
  bool *nulls = (bool *)palloc0(ncols * sizeof(bool));
  sm_bound_t lo = {lo_vals, nulls, 0, true};
  sm_bound_t hi = {hi_vals, nulls, 0, true};
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(heap))));
  for (int i = 0; i < nkeys; i++)
    if (keys[i].sk_flags & (SK_ROW_HEADER | SK_SEARCHARRAY))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index scan in sm takes no row comparison "
                             "or array yet")));

  for (int c = 0; c < ncols; c++) {
    Oid coltype = index->rd_opcintype[c];
    bool desc = (index->rd_indoption[c] & INDOPTION_DESC) != 0;
    ScanKey eq = NULL, below = NULL, above = NULL;

    for (int i = 0; i < nkeys; i++) {
      ScanKey key = &keys[i];

      if (key->sk_attno != c + 1 || !plain_key(key, coltype))
        continue;
      switch (key->sk_strategy) {
      case BTEqualStrategyNumber:
        eq = key;
        break;
      case BTGreaterStrategyNumber:
      case BTGreaterEqualStrategyNumber:
        below = key; /* the value's lower end */
        break;
      default:
        above = key; /* the value's upper end */
        break;
      }
    }
    if (eq != NULL) {
      lo_vals[c] = hi_vals[c] = (sm_datum_t)eq->sk_argument;
      lo.n = hi.n = c + 1;
      continue;
    }
    /* In the index's order a DESC column's high values come first. */
    ScanKey first = desc ? above : below, last = desc ? below : above;

    if (first != NULL) {
      lo_vals[c] = (sm_datum_t)first->sk_argument;
      lo.n = c + 1;
      lo.inclusive = first->sk_strategy == BTGreaterEqualStrategyNumber ||
                     first->sk_strategy == BTLessEqualStrategyNumber;
    }
    if (last != NULL) {
      hi_vals[c] = (sm_datum_t)last->sk_argument;
      hi.n = c + 1;
      hi.inclusive = last->sk_strategy == BTGreaterEqualStrategyNumber ||
                     last->sk_strategy == BTLessEqualStrategyNumber;
    }
    break;
  }

  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  scan->keys = keys;
  scan->nkeys = nkeys;
  scan->index = RelationGetRelid(index);
  scan->form = form;
  scan->row_natts = RelationGetDescr(heap)->natts;
  scan->keycols = (AttrNumber *)palloc(Max(nkeys, 1) * sizeof(AttrNumber));
  for (int i = 0; i < nkeys; i++)
    scan->keycols[i] = form->indkey.values[keys[i].sk_attno - 1];
  if (sm_tbl_idx_scan(tbl, RelationGetRelid(index), lo.n > 0 ? &lo : NULL,
                      hi.n > 0 ? &hi : NULL, NULL, &scan->icursor) != 0)
    sm_error("scan an index");
  return scan;
}

/*
 * The row at rowno, as this transaction sees it, into slot, as a virtual
 * tuple, its tid its rowno; false, the slot empty, if it sees none.
 */
static bool read_row(sm_tbl_t *tbl, uint64_t rowno, TupleTableSlot *slot) {
  const void *tuple;
  size_t len;

  ExecClearTuple(slot);
  switch (sm_tbl_read(tbl, rowno, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a row");
  }
  if (sm_tbl_deform(tbl, tuple, len, (sm_datum_t *)slot->tts_values,
                    slot->tts_isnull, slot->tts_tupleDescriptor->natts) != 0)
    sm_error("read a row");
  ExecStoreVirtualTuple(slot);
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
  return true;
}

/*
 * The index scan's next row that meets its keys into slot, as a virtual
 * tuple, its tid its rowno; false at the end.
 */
bool smstore_index_getnext(smstore_scan_t *scan, TupleTableSlot *slot) {
  uint64 rowno;

  while (smstore_index_getrowno(scan, &rowno))
    /* A row the transaction does not see is passed. */
    if (read_row(scan->tbl, rowno, slot) && keys_met(scan, slot, false))
      return true;
  ExecClearTuple(slot);
  return false;
}

/*
 * The rowno of the index scan's next entry into *rowno, the row unread;
 * false at the end.  The entry is within the scan's bounds, but its row may
 * be one the transaction does not see, or not meet every key: who reads
 * the row checks.
 */
bool smstore_index_getrowno(smstore_scan_t *scan, uint64 *rowno) {
  uint64_t r;
  bool cut;
  const void *value;
  size_t len;

  CHECK_FOR_INTERRUPTS();
  switch (sm_idx_scannxt(scan->icursor, &r, &cut, &value, &len)) {
  case 0:
    *rowno = r;
    scan->value = value;
    scan->len = len;
    return true;
  case 1:
    return false;
  default:
    sm_error("scan an index");
  }
  return false; /* not reached */
}

/*
 * The entry's INCLUDE columns, from its value, into the slot's columns
 * after the key's.  The value holds the rowno and the cut flag first.
 */
static void entry_includes(smstore_scan_t *scan, TupleTableSlot *slot) {
  int nkeys = scan->form->indnkeyatts;

  if (scan->entry_values == NULL) {
    scan->entry_ncols = sm_tbl_idx_ncols(scan->tbl, scan->index);
    scan->entry_values =
        (sm_datum_t *)palloc(scan->entry_ncols * sizeof(sm_datum_t));
    scan->entry_isnull = (bool *)palloc(scan->entry_ncols * sizeof(bool));
  }
  if (sm_tbl_idx_deform(scan->tbl, scan->index, scan->value, scan->len,
                        scan->entry_values, scan->entry_isnull,
                        scan->entry_ncols) != 0)
    sm_error("read an index entry");
  for (int i = nkeys; i < scan->form->indnatts; i++) {
    slot->tts_values[i] = (Datum)scan->entry_values[2 + i - nkeys];
    slot->tts_isnull[i] = scan->entry_isnull[2 + i - nkeys];
  }
}

/*
 * The row at rowno's columns the index holds into slot, in the index's
 * order: false if the transaction does not see the row.  For an entry
 * whose key does not give its columns back.
 */
static bool entry_from_row(smstore_scan_t *scan, uint64_t rowno,
                           TupleTableSlot *slot) {
  const void *tuple;
  size_t len;

  if (scan->row_values == NULL) {
    scan->row_values = (Datum *)palloc(scan->row_natts * sizeof(Datum));
    scan->row_isnull = (bool *)palloc(scan->row_natts * sizeof(bool));
  }
  switch (sm_tbl_read(scan->tbl, rowno, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a row");
  }
  if (sm_tbl_deform(scan->tbl, tuple, len, (sm_datum_t *)scan->row_values,
                    scan->row_isnull, scan->row_natts) != 0)
    sm_error("read a row");
  for (int i = 0; i < scan->form->indnatts; i++) {
    int col = scan->form->indkey.values[i] - 1;

    slot->tts_values[i] = scan->row_values[col];
    slot->tts_isnull[i] = scan->row_isnull[col];
  }
  return true;
}

/*
 * The index scan's next entry that meets its keys into slot, a virtual
 * tuple of the index's columns, its tid its rowno; false at the end.  The
 * key gives its columns and the entry's value the INCLUDE ones, with no
 * row read: an entry sm returns is one the transaction sees.  Where the
 * key does not give them back, the row is read.
 */
bool smstore_index_getentry(smstore_scan_t *scan, TupleTableSlot *slot) {
  uint64 rowno;

  while (smstore_index_getrowno(scan, &rowno)) {
    ExecClearTuple(slot);
    switch (sm_idx_scan_key(scan->icursor, (sm_datum_t *)slot->tts_values,
                            slot->tts_isnull, scan->form->indnkeyatts)) {
    case 0:
      entry_includes(scan, slot);
      break;
    case 1:
      if (!entry_from_row(scan, rowno, slot))
        continue;
      break;
    default:
      sm_error("read an index entry");
    }
    ExecStoreVirtualTuple(slot);
    smstore_rowno_to_tid(rowno, &slot->tts_tid);
    if (keys_met(scan, slot, true))
      return true;
  }
  ExecClearTuple(slot);
  return false;
}

/* Begin reading rows of rel, a table in sm, by rowno (smstore_fetch()). */
smstore_scan_t *smstore_beginfetch(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  return scan;
}

/*
 * The row at rowno into slot, as a virtual tuple, its tid its rowno; false
 * if the transaction sees none there.
 */
bool smstore_fetch(smstore_scan_t *scan, uint64 rowno, TupleTableSlot *slot) {
  return read_row(scan->tbl, rowno, slot);
}
