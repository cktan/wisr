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

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "access/detoast.h"
#include "access/smstore.h"
#include "access/xact.h"
#include "access/relation.h"
#include "catalog/index.h"
#include "parser/parse_node.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "storage/lockdefs.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/fmgrprotos.h"
#include "utils/datadir.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/array.h"
#include "catalog/pg_statistic.h"
#include "access/transam.h"
#include "access/htup_details.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
#include "executor/executor.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"

/*
 * Where a row's numerics are made from sm's datums, reset at each row: the
 * slot copies them when it is materialized (slot_from_sm()).
 */
static thread_local /*LOCAL*/ MemoryContext numeric_scratch = NULL;

/* Whether this session's PostgreSQL transaction has begun an sm one. */
static thread_local /*LOCAL*/ bool sm_xact_open = false;

/*
 * Whether a statement has begun since sm last heard of one: the next call
 * in the sm transaction begins one there too (smstore_statement_start()).
 */
static thread_local /*LOCAL*/ bool statement_pending = false;

/* A handle of a table this transaction opened in sm, to read or to write. */
typedef struct open_table_t {
  sm_lockmode_t mode;
  sm_tbl_t *tbl;
  int level; /* the subtransaction nesting level that holds it */
} open_table_t;

/* The tables a transaction opened: each oid's handles, one a lock mode. */
typedef std::unordered_map<Oid, std::vector<open_table_t>> open_tables_t;

/*
 * This session's state in sm: the tables its transaction opened, closed as
 * it ends: tcb->smstore.
 */
struct smstore_state_t {
  open_tables_t open_tables;
  /* sm's namespaces' epoch when this session last flushed them. */
  uint64_t nsp_epoch = 0;
};

/* The tables this session's transaction opened. */
static inline open_tables_t &open_tables(void) {
  return tcb->smstore->open_tables;
}

/* Make a thread's state in sm: no table open. */
smstore_state_t *smstore_state_new(void) { return new smstore_state_t(); }

/* Free a thread's state in sm. */
void smstore_state_free_t::operator()(smstore_state_t *state) const {
  delete state;
}

/*
 * The depth of sm's savepoint for each subtransaction open, the one at
 * nesting level 2 first, in TopTransactionContext.  A subtransaction has one
 * from its first call into sm on: one that has made none has nothing to
 * undo, and gets its savepoint when it makes one (savepoints_begin()).
 */
static thread_local /*LOCAL*/ List *savepoints = NIL;

/*
 * The row the statement keeps, to be fetched by its rowno again: one a
 * write found changed by a commit since the statement's snapshot, then its
 * newest version, which smstore_lock_latest() read; or one just deleted, as
 * it was.  The statement's own read of the rowno would give neither.
 * kept_tuple is in TopTransactionContext, and NULL when the row is yet to
 * be read; kept_oid is InvalidOid when none is kept.
 */
static thread_local /*LOCAL*/ Oid kept_oid = InvalidOid;

/*
 * The table a bulk load is begun on (smstore_load()), InvalidOid when none
 * is.  sm ends a load with its transaction, so this is forgotten then.
 */
static thread_local /*LOCAL*/ Oid load_oid = InvalidOid;
static thread_local /*LOCAL*/ uint64 kept_rowno = 0;
static thread_local /*LOCAL*/ char *kept_tuple = NULL;
static thread_local /*LOCAL*/ Size kept_len = 0;

/*
 * Whether sm's namespaces' epoch has moved since this session last asked
 * (sm_cat_namespace_epoch()): a schema made or dropped, which no lock
 * guards the lookup of, so what it caches of them is to go (SHMEM.md
 * section 5.1).  False for a session with no storage.
 */
bool smstore_namespace_epoch_moved(void) {
  sm_session_t *s = datadir_sm_session();
  uint64_t epoch;

  if (s == NULL)
    return false;
  epoch = sm_cat_namespace_epoch(s);
  if (epoch == tcb->smstore->nsp_epoch)
    return false;
  tcb->smstore->nsp_epoch = epoch;
  return true;
}

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

    /* Only -1 is an error: 1, a lock not had at once, is an answer. */
    if (rc != -1 || sm_errnum() != SM_ERR_INTERRUPTED)
      return rc;
    CHECK_FOR_INTERRUPTS();
  }
}

/*
 * Close the tables the aborting subtransaction opened: sm's rollback undid
 * what they did, a truncate's new store among it, and released their locks,
 * so a handle of them is stale.  The next use opens one afresh.
 */
static void close_subxact_tables(void) {
  int level = GetCurrentTransactionNestLevel();
  open_tables_t &ots = open_tables();

  for (auto it = ots.begin(); it != ots.end();) {
    std::vector<open_table_t> &handles = it->second;

    for (size_t i = 0; i < handles.size();) {
      if (handles[i].level >= level) {
        sm_tbl_close(handles[i].tbl);
        handles.erase(handles.begin() + i);
      } else
        i++;
    }
    it = handles.empty() ? ots.erase(it) : std::next(it);
  }
}

/* Close the tables this transaction opened: a handle is good for it only. */
static void close_tables(void) {
  for (auto &[oid, handles] : open_tables())
    for (open_table_t &ot : handles)
      sm_tbl_close(ot.tbl);
  open_tables().clear();
  savepoints = NIL;
  /* The kept row's memory goes with the transaction. */
  kept_oid = InvalidOid;
  kept_tuple = NULL;
  /* And so does a bulk load: sm ended it, and the handle is closed. */
  load_oid = InvalidOid;
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
  if (sm_commit(session()) != 0) {
    int code = sm_sqlstate();
    std::string why = sm_errmsg();

    /*
     * A commit sm refuses may leave its transaction running, as one with a
     * bulk load still open does: end it, or it keeps its locks.  One sm
     * aborted itself takes this as nothing.
     */
    (void)sm_abort(session());
    ereport(ERROR,
            (errcode(code), errmsg("could not commit: %s", why.c_str())));
  }
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
  int level = GetCurrentTransactionNestLevel();

  /* A savepoint sm cannot forget is a transaction it ended: next call says. */
  if (depth > 0)
    (void)sm_forget(session(), depth);
  /* The tables the subtransaction opened are its parent's now. */
  for (auto &[oid, handles] : open_tables())
    for (open_table_t &ot : handles)
      if (ot.level >= level)
        ot.level = level - 1;
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
  close_subxact_tables();
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
void smstore_statement_start(void) {
  statement_pending = true;
  /* A row is kept for the statement that wrote it, and no longer. */
  if (kept_tuple != NULL)
    pfree(kept_tuple);
  kept_oid = InvalidOid;
  kept_tuple = NULL;
}

/*
 * A numeric column with no precision is numeric(18, 3), DuckDB's DECIMAL
 * (DECIMAL.md section 2.3): sm keeps a decimal's scale in its schema.
 */
#define NUMERIC_DEFAULT_TYPMOD (((18 << 16) | 3) + VARHDRSZ)

/* The precision of a numeric column's typmod, the default's if none. */
static int numeric_precision(int32 typmod) {
  if (typmod < (int32)VARHDRSZ)
    typmod = NUMERIC_DEFAULT_TYPMOD;
  return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

/* The scale of a numeric column's typmod. */
static int numeric_scale_of(int32 typmod) {
  if (typmod < (int32)VARHDRSZ)
    typmod = NUMERIC_DEFAULT_TYPMOD;
  return (typmod - VARHDRSZ) & 0x7ff;
}

/* The integer of num, the number times 10^scale, as it is. */
static __int128 numeric_value(const decimal_t *num) {
  return (__int128)(((unsigned __int128)(uint64)num->hi << 64) | num->lo);
}

/*
 * sm's datum for num in a column of typmod: its integer at the column's
 * scale, by value for decimal64, for decimal128 16 bytes in buf.  The
 * value is rounded to the column's scale and checked against its
 * precision, as an assignment is.
 */
static sm_datum_t numeric_to_sm(Datum num, int32 typmod, uint8 *buf) {
  int32 t = typmod < (int32)VARHDRSZ ? NUMERIC_DEFAULT_TYPMOD : typmod;
  Numeric v = DatumGetNumeric(
      DirectFunctionCall2(numeric, num, Int32GetDatum(t)));
  __int128 i = numeric_value(v);

  if (numeric_precision(typmod) <= 18)
    return (sm_datum_t)(int64)i;
  memcpy(buf, &i, 16);
  return (sm_datum_t)buf;
}

/* The numeric of sm's datum d, of a column of typmod, palloc'd. */
static Datum numeric_from_sm(sm_datum_t d, int32 typmod) {
  Numeric res = (Numeric)palloc0(sizeof(decimal_t));
  __int128 i;

  if (numeric_precision(typmod) <= 18)
    i = (int64)d;
  else
    memcpy(&i, (const void *)d, 16);
  res->lo = (uint64)(unsigned __int128)i;
  res->hi = (int64)((unsigned __int128)i >> 64);
  res->scale = (int16)numeric_scale_of(typmod);
  return NumericGetDatum(res);
}

/*
 * sm's datum for a scan's bound num on a numeric column of typmod: the
 * column's integer at or below it, or with upper at or above it, kept
 * within the column's precision.  So the bound takes in every value the
 * key's own would, and the keys, checked again on each row, decide.
 */
static sm_datum_t numeric_bound(Datum num, int32 typmod, bool upper,
                                uint8 *buf) {
  const decimal_t *v = DatumGetNumeric(num);
  int s = numeric_scale_of(typmod), vs = v->scale;
  int p = numeric_precision(typmod);
  __int128 i = numeric_value(v), limit = 1;
  bool clamped = false;

  for (int k = 0; k < p; k++)
    limit *= 10;
  limit -= 1; /* the column's largest integer, 10^p - 1 */
  for (; vs < s; vs++)
    if (__builtin_mul_overflow(i, 10, &i)) {
      i = i < 0 ? -limit : limit;
      clamped = true;
      break;
    }
  if (!clamped)
    for (; vs > s; vs--) {
      __int128 q = i / 10, r = i % 10;

      /* Floor for a lower bound, ceiling for an upper one. */
      if (r != 0 && (upper ? r > 0 : r < 0))
        q += upper ? 1 : -1;
      i = q;
    }
  if (i > limit)
    i = limit;
  if (i < -limit)
    i = -limit;
  if (p <= 18)
    return (sm_datum_t)(int64)i;
  memcpy(buf, &i, 16);
  return (sm_datum_t)buf;
}

/* Whether a row of desc has a numeric column, whose datums are converted. */
static bool has_numeric(TupleDesc desc) {
  for (int i = 0; i < desc->natts; i++)
    if (TupleDescAttr(desc, i)->atttypid == NUMERICOID &&
        !TupleDescAttr(desc, i)->attisdropped)
      return true;
  return false;
}

/*
 * Whether the TOAST pointer val is the one old, the tuple of a row of tbl,
 * holds for its column at ordinal: what an update that leaves the column
 * alone gives back, which sm keeps without a write.
 */
static bool own_pointer(sm_tbl_t *tbl, const std::string *old, int ordinal,
                        sm_datum_t val) {
  sm_datum_t oval;
  bool onull;

  if (old == NULL ||
      sm_tbl_decode(tbl, old->data(), old->size(), ordinal, &oval, &onull) !=
          0)
    return false;
  return !onull && sm_datum_is_toast(oval) &&
         memcmp((const void *)oval, (const void *)val,
                VARSIZE_EXTERNAL((struct varlena *)val)) == 0;
}

/*
 * The datums of a row of desc as sm takes them: values itself, or a
 * palloc'd copy where one differs: each numeric its integer at its column's
 * scale, and each TOAST pointer of sm's its value, read, unless it is old's
 * own for its column -- old the tuple of the row of tbl an update replaces,
 * or NULL.  sm takes no other pointer.
 */
static const sm_datum_t *row_to_sm(sm_tbl_t *tbl, TupleDesc desc,
                                   Datum *values, bool *isnull, int natts,
                                   const std::string *old) {
  sm_datum_t *out = NULL;

  for (int i = 0; i < natts && i < desc->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(desc, i);
    sm_datum_t val;

    if (isnull[i])
      continue;
    if (att->atttypid == NUMERICOID)
      val = numeric_to_sm(values[i], att->atttypmod, (uint8 *)palloc(16));
    else if (att->attlen == -1 &&
             VARATT_IS_EXTERNAL_SM(DatumGetPointer(values[i])) &&
             !own_pointer(tbl, old, i, (sm_datum_t)values[i]))
      val = (sm_datum_t)detoast_external_attr(
          (struct varlena *)DatumGetPointer(values[i]));
    else
      continue;
    if (out == NULL) {
      out = (sm_datum_t *)palloc(natts * sizeof(sm_datum_t));
      memcpy(out, values, natts * sizeof(sm_datum_t));
    }
    out[i] = val;
  }
  return out != NULL ? out : (const sm_datum_t *)values;
}

/* Whether a row of desc, natts values, has a TOAST pointer of sm's. */
static bool has_toast(TupleDesc desc, Datum *values, bool *isnull,
                      int natts) {
  for (int i = 0; i < natts && i < desc->natts; i++)
    if (!isnull[i] && TupleDescAttr(desc, i)->attlen == -1 &&
        VARATT_IS_EXTERNAL_SM(DatumGetPointer(values[i])))
      return true;
  return false;
}

/*
 * The numeric columns of a row of desc in slot, as sm gave them, made
 * numerics, and the slot made to own its values: sm's point into a tuple
 * kept only until the next row.
 */
static void slot_from_sm(TupleTableSlot *slot) {
  TupleDesc desc = slot->tts_tupleDescriptor;
  MemoryContext old;

  if (!has_numeric(desc))
    return;
  /* The numerics are copied into the slot by its materializing. */
  if (numeric_scratch == NULL)
    numeric_scratch = AllocSetContextCreate(
        TopMemoryContext, "sm numerics", ALLOCSET_SMALL_SIZES);
  MemoryContextReset(numeric_scratch);
  old = MemoryContextSwitchTo(numeric_scratch);
  for (int i = 0; i < desc->natts && i < slot->tts_nvalid; i++)
    if (TupleDescAttr(desc, i)->atttypid == NUMERICOID && !slot->tts_isnull[i])
      slot->tts_values[i] =
          numeric_from_sm((sm_datum_t)slot->tts_values[i],
                          TupleDescAttr(desc, i)->atttypmod);
  MemoryContextSwitchTo(old);
  ExecMaterializeSlot(slot);
}

/*
 * sm's type code for a column of type typid and typmod (sm2/TYPE.md section
 * 3): numeric is decimal64 up to 18 digits, decimal128 up to 38.
 */
static uint8_t type_code(Oid typid, int32 typmod) {
  switch (typid) {
  case NUMERICOID:
    return numeric_precision(typmod) <= 18 ? 17 : 18;
  case BOOLOID:
    return 1;
  case INT2OID:
    return 2;
  case INT4OID:
    return 3;
  case INT8OID:
    return 4;
  case FLOAT4OID:
    return 5;
  case FLOAT8OID:
    return 6;
  case TEXTOID:
    return 8;
  case VARCHAROID:
    return 9;
  case BYTEAOID:
    return 10;
  case DATEOID:
    return 11;
  case TIMEOID:
    return 12;
  case TIMESTAMPOID:
    return 13;
  case TIMESTAMPTZOID:
    return 14;
  case INTERVALOID:
    return 15;
  case UUIDOID:
    return 16;
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
 * The name of the schema nspoid in sm, palloc'd, or NULL if there is none.
 */
char *smstore_namespace_name(Oid nspoid) {
  std::string name;

  switch (sm_cat_namespace_name(session(), nspoid, name)) {
  case 0:
    return pstrdup(name.c_str());
  case 1:
    return NULL;
  default:
    sm_error("look up a schema");
  }
  return NULL; /* not reached */
}

/*
 * The names of the schemas in sm, palloc'd, sorted, as this transaction sees
 * them: pg_namespace's rows (catalog/catrel.c).
 */
List *smstore_namespace_names(void) {
  std::vector<std::string> names;
  List *out = NIL;

  if (sm_cat_list_namespaces(xact_session(), names) != 0)
    sm_error("list the schemas");
  for (const std::string &name : names)
    out = lappend(out, pstrdup(name.c_str()));
  return out;
}

static void remember_index(Oid indexoid, Oid table);
static Oid type_oid(uint8_t code);

/* Add to *out a relation of sm's, made from what is given; it is returned. */
static smstore_relation_t *add_relation(List **out, uint64_t oid,
                                        const std::string &name, Oid nspoid,
                                        char kind, int natts, bool hasindex,
                                        bool temp) {
  smstore_relation_t *r = (smstore_relation_t *)palloc0(sizeof(*r));

  r->oid = (Oid)oid;
  r->name = pstrdup(name.c_str());
  r->nspoid = nspoid;
  r->kind = kind;
  r->natts = natts;
  r->hasindex = hasindex;
  r->temp = temp;
  r->attnames = (char **)palloc0(Max(natts, 1) * sizeof(char *));
  r->typids = (Oid *)palloc0(Max(natts, 1) * sizeof(Oid));
  r->typmods = (int32 *)palloc0(Max(natts, 1) * sizeof(int32));
  r->notnull = (bool *)palloc0(Max(natts, 1) * sizeof(bool));
  r->defaults = (char **)palloc0(Max(natts, 1) * sizeof(char *));
  r->identity = (char *)palloc0(Max(natts, 1) * sizeof(char));
  *out = lappend(*out, r);
  return r;
}

/* Set r's column i to the table's column c. */
static void set_column(smstore_relation_t *r, int i, const sm_column_def_t &c) {
  r->attnames[i] = pstrdup(c.name.c_str());
  r->typmods[i] = c.typmod;
  if (c.dropped)
    return;
  r->typids[i] = type_oid(c.type_code);
  r->notnull[i] = c.not_null;
}

/* Add to *out the index x of the table def, in pg_index's terms. */
static void add_index(List **out, const sm_table_def_t &def,
                      const sm_index_def_t &x, Oid nspoid, bool temp) {
  int nkeys = (int)x.key_attnums.size();
  int natts = nkeys + (int)x.include_attnums.size();
  smstore_relation_t *r = add_relation(out, x.oid, x.name, nspoid,
                                       RELKIND_INDEX, natts, false, temp);

  r->table = (Oid)def.oid;
  r->unique = x.unique;
  r->primary = x.primary;
  r->valid = x.state == SM_INDEX_VALID;
  r->nkeys = nkeys;
  r->attnums = (int16 *)palloc0(natts * sizeof(int16));
  r->desc = (bool *)palloc0(nkeys * sizeof(bool));
  r->nulls_first = (bool *)palloc0(nkeys * sizeof(bool));
  for (int i = 0; i < natts; i++) {
    int32_t at = i < nkeys ? x.key_attnums[i] : x.include_attnums[i - nkeys];

    /* An expression's place is 0, as pg_index has it. */
    if (at >= 0) {
      r->attnums[i] = (int16)(at + 1);
      set_column(r, i, def.columns[at]);
      r->notnull[i] = false;
    }
  }
  for (int k = 0; k < nkeys; k++) {
    r->desc[k] = x.key[k].desc;
    r->nulls_first[k] = x.key[k].nulls_first;
  }
  /* So the index can be opened by its oid alone (RelationBuildDesc()). */
  remember_index(r->oid, r->table);
}

/* Add to *out the tables of sm's namespace nspoid, with their indexes. */
/*
 * Add to *out the sequence oid, named name in nspoid, with its START,
 * INCREMENT and owner as sm keeps them.
 */
static void add_sequence(sm_session_t *s, uint64_t oid, const char *name,
                         Oid nspoid, List **out) {
  smstore_relation_t *r =
      add_relation(out, oid, name, nspoid, RELKIND_SEQUENCE, 3, false, false);
  sm_sequence_def_t seq;
  sm_table_def_t owner;

  if (sm_cat_sequence_def(s, oid, &seq) != 0)
    sm_error("read a sequence");
  r->seqstart = (int64)seq.start;
  r->seqincrement = (int64)seq.increment;
  if (seq.owner == 0)
    return;
  r->owner = (Oid)seq.owner;
  r->owner_attnum = (int16)(seq.owner_attnum + 1);
  /* An identity column's sequence, or a serial's: as pg_depend tells. */
  if (sm_cat_table_def_oid(s, seq.owner, owner) == 0 &&
      (size_t)seq.owner_attnum < owner.columns.size())
    r->owner_identity = owner.columns[seq.owner_attnum].identity != 0;
}

/* Add to *out the table def, with its columns, and its indexes. */
static void add_table(const sm_table_def_t &def, List **out) {
  bool temp = def.nspoid == SM_NSP_TEMP;
  Oid pgnsp = temp ? PG_TEMP_NAMESPACE : (Oid)def.nspoid;
  smstore_relation_t *r =
      add_relation(out, def.oid, def.name, pgnsp, RELKIND_RELATION,
                   (int)def.columns.size(), !def.indexes.empty(), temp);

  for (size_t i = 0; i < def.columns.size(); i++) {
    const sm_column_def_t &c = def.columns[i];

    set_column(r, (int)i, c);
    if (!c.dropped && c.default_expr)
      r->defaults[i] = pstrdup(c.default_expr->c_str());
    r->identity[i] = c.dropped ? 0 : c.identity;
  }
  for (const sm_index_def_t &x : def.indexes)
    add_index(out, def, x, pgnsp, temp);
}

/* Add to *out the tables of sm's namespace nspoid, with their indexes. */
static void add_tables(sm_session_t *s, uint64_t nspoid, List **out) {
  std::vector<std::string> names;

  if (sm_cat_list_tables(s, nspoid, names) != 0)
    sm_error("list a schema's tables");
  for (const std::string &name : names) {
    sm_table_def_t def;

    /* Not locked: a listing does not wait for a table's DDL. */
    switch (sm_cat_table_def(s, nspoid, name.c_str(), def)) {
    case 0:
      add_table(def, out);
      break;
    case 1:
      break;
    default:
      sm_error("read a table's definition");
    }
  }
}

/*
 * The relations of sm's that smstore_relations() would give for the one
 * relation oid, a table, an index or a sequence: it and, for a table or an
 * index, the table and all its indexes.  Every relation if oid is an index
 * whose table this thread has not met; none if there is no such relation.
 * For a catalog query on one relation, which needs no other's rows.
 */
List *smstore_relations_of(Oid oid) {
  sm_session_t *s = xact_session();
  Oid table = smstore_index_table(oid);
  sm_table_def_t def;
  Oid nspoid;
  char *seq;
  List *out = NIL;

  /* sm has no relation below the first oid a user's object takes. */
  if (oid < FirstNormalObjectId)
    return NIL;
  /* Not locked, as smstore_relations() is not. */
  switch (sm_cat_table_def_oid(s, OidIsValid(table) ? table : oid, def)) {
  case 0:
    add_table(def, &out);
    return out;
  case 1:
    break;
  default:
    sm_error("read a table's definition");
  }
  if ((seq = smstore_sequence_lookup(oid, &nspoid)) != NULL) {
    add_sequence(s, oid, seq, nspoid, &out);
    return out;
  }
  /* An index of a table not met: it may be any table's. */
  return OidIsValid(table) ? NIL : smstore_relations();
}

/*
 * Every relation in sm, palloc'd smstore_relation_t, as this transaction
 * sees them: each schema's tables, their indexes and its sequences, and the
 * session's temporary tables, in pg_temp.  pg_class's rows
 * (catalog/catrel.c).
 */
List *smstore_relations(void) {
  sm_session_t *s = xact_session();
  std::vector<std::string> nsps;
  List *out = NIL;

  if (sm_cat_list_namespaces(s, nsps) != 0)
    sm_error("list the schemas");
  for (const std::string &nsp : nsps) {
    std::vector<std::string> seqs;
    uint64_t nspoid, oid;

    if (sm_cat_namespace_oid(s, nsp.c_str(), &nspoid) != 0)
      continue;
    add_tables(s, nspoid, &out);
    if (sm_cat_list_sequences(s, nspoid, seqs) != 0)
      sm_error("list a schema's sequences");
    for (const std::string &seq : seqs)
      if (sm_cat_sequence_oid(s, nspoid, seq.c_str(), &oid) == 0)
        add_sequence(s, oid, seq.c_str(), (Oid)nspoid, &out);
  }
  add_tables(s, SM_NSP_TEMP, &out);
  return out;
}

/*
 * The oid of the sequence named name in nspoid, or InvalidOid if there is
 * none.  sm reads it from its catalog in the transaction, which this begins.
 */
static Oid sequence_oid(const char *name, Oid nspoid) {
  uint64_t oid;

  switch (sm_cat_sequence_oid(xact_session(), nspoid, name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    return InvalidOid;
  default:
    sm_error("look up a sequence");
  }
  return InvalidOid; /* not reached */
}

/*
 * The namespace nspoid as sm knows it.  The session's temporary tables are
 * in no namespace in sm, which SM_NSP_TEMP stands for; digres's own
 * temporary namespace, PG_TEMP_NAMESPACE, is compiled in and never sm's.
 */
static uint64_t sm_nspoid(Oid nspoid) {
  return nspoid == PG_TEMP_NAMESPACE ? SM_NSP_TEMP : nspoid;
}

/*
 * The oid of the relation named name in nspoid, a table, an index or a
 * sequence, or InvalidOid if there is none.
 */
Oid smstore_relation_oid(const char *name, Oid nspoid) {
  uint64_t oid, table;

  /* No namespace holds nothing: InvalidOid is SM_NSP_TEMP's number. */
  if (!OidIsValid(nspoid))
    return InvalidOid;
  switch (sm_cat_table_oid(session(), sm_nspoid(nspoid), name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    break;
  default:
    sm_error("look up a table");
  }
  switch (sm_cat_index_oid(session(), sm_nspoid(nspoid), name, &table, &oid)) {
  case 0:
    /* So the index can be opened by its oid alone (RelationBuildDesc()). */
    remember_index((Oid)oid, (Oid)table);
    return (Oid)oid;
  case 1:
    break;
  default:
    sm_error("look up an index");
  }
  /* A sequence is never temporary yet. */
  return nspoid == PG_TEMP_NAMESPACE ? InvalidOid : sequence_oid(name, nspoid);
}

static void drop_table(RangeVar *rv, bool missing_ok);
static void drop_sequence(RangeVar *rv, bool missing_ok);

/*
 * Create the schema name in sm, in this transaction, and return its oid.
 * Every session's search path is recomputed once this one commits, and
 * this session's at its next command.
 */
Oid smstore_create_namespace(const char *name) {
  sm_session_t *s = xact_session();
  uint64_t oid;

  if (waiting([&] { return sm_cat_create_namespace(s, name, &oid); }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_SCHEMA),
                      errmsg("schema \"%s\" already exists", name)));
    sm_error("create a schema");
  }
  /* A search path naming it may now find it. */
  CacheInvalidateCatalog(NamespaceRelationId);
  return (Oid)oid;
}

/*
 * The tables in the schema nspoid, sorted by name, dropped with it by DROP
 * SCHEMA ... CASCADE.
 */
static std::vector<std::string> namespace_tables(Oid nspoid) {
  std::vector<std::string> names;

  if (sm_cat_list_tables(xact_session(), nspoid, names) != 0)
    sm_error("list a schema's tables");
  return names;
}

/* The sequences in the schema nspoid, sorted by name, as for its tables. */
static std::vector<std::string> namespace_sequences(Oid nspoid) {
  std::vector<std::string> names;

  if (sm_cat_list_sequences(xact_session(), nspoid, names) != 0)
    sm_error("list a schema's sequences");
  return names;
}

/*
 * Drop the schema nspname, nspoid, from sm, in this transaction.  With
 * cascade its tables go first, each with its indexes and the sequences its
 * columns own, then its other sequences; without, a schema that holds any
 * fails.
 */
static void drop_namespace(const char *nspname, Oid nspoid, bool cascade) {
  if (cascade) {
    std::vector<std::string> tables = namespace_tables(nspoid);
    std::vector<std::string> sequences;
    int n;
    StringInfoData detail;

    for (const std::string &t : tables)
      drop_table(makeRangeVar(pstrdup(nspname), pstrdup(t.c_str()), -1),
                 false);
    /*
     * The sequences left: an owned one went with its table, unnamed.  The
     * list is read in a new statement, which sees the drops.
     */
    CommandCounterIncrement();
    sequences = namespace_sequences(nspoid);
    n = (int)(tables.size() + sequences.size());

    initStringInfo(&detail);
    for (const std::string &t : tables)
      appendStringInfo(&detail, "%sdrop cascades to table %s.%s",
                       detail.len > 0 ? "\n" : "", nspname, t.c_str());
    for (const std::string &q : sequences)
      appendStringInfo(&detail, "%sdrop cascades to sequence %s.%s",
                       detail.len > 0 ? "\n" : "", nspname, q.c_str());
    if (n == 1)
      ereport(NOTICE, (errmsg_internal("%s", detail.data)));
    else if (n > 1)
      ereport(NOTICE, (errmsg_plural("drop cascades to %d other object",
                                     "drop cascades to %d other objects", n,
                                     n),
                       errdetail_internal("%s", detail.data)));
    for (const std::string &q : sequences)
      drop_sequence(makeRangeVar(pstrdup(nspname), pstrdup(q.c_str()), -1),
                    false);
  }
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_namespace(s, nspoid); }) != 0) {
    if (sm_errnum() == SM_ERR_INUSE)
      ereport(ERROR,
              (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
               errmsg("cannot drop schema %s because other objects depend "
                      "on it",
                      nspname),
               errhint("Use DROP ... CASCADE to drop the dependent objects "
                       "too.")));
    if (sm_errnum() == SM_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("schema \"%s\" cannot be dropped", nspname)));
    sm_error("drop a schema");
  }
  /* A search path naming it no longer finds it. */
  CacheInvalidateCatalog(NamespaceRelationId);
}

/*
 * Drop the schemas a DROP SCHEMA names, from sm, in this transaction.  One
 * that is not there is an error, or with IF EXISTS a notice.
 */
void smstore_drop_namespaces(DropStmt *stmt) {
  ListCell *l;

  foreach (l, stmt->objects) {
    const char *name = strVal(lfirst(l));
    Oid nspoid = smstore_namespace_oid(name);

    if (!OidIsValid(nspoid)) {
      if (!stmt->missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("schema \"%s\" does not exist", name)));
      ereport(NOTICE,
              (errmsg("schema \"%s\" does not exist, skipping", name)));
      continue;
    }
    drop_namespace(name, nspoid, stmt->behavior == DROP_CASCADE);
  }
}

/*
 * Whether constraints, a column's, are only those sm's tables take: NOT
 * NULL, a PRIMARY KEY or UNIQUE, which parse analysis has made an index
 * statement of its own, and a DEFAULT, which it has made the column's
 * raw_default.
 */
static bool only_taken(List *constraints) {
  ListCell *l;

  foreach (l, constraints) {
    ConstrType type = lfirst_node(Constraint, l)->contype;

    if (type != CONSTR_NOTNULL && type != CONSTR_PRIMARY &&
        type != CONSTR_UNIQUE && type != CONSTR_DEFAULT &&
        type != CONSTR_IDENTITY)
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
      stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE TABLE supports only columns, NOT NULL, "
                           "PRIMARY KEY, UNIQUE and TEMPORARY yet")));
}

/*
 * col's DEFAULT as sm keeps it, the cooked expression's nodeToString(),
 * palloc'd, or NULL with none: coerced to the column's type and checked
 * as PostgreSQL's DEFAULT is (cookDefault()).
 */
static const char *column_default(ColumnDef *col, Oid typid, int32 typmod) {
  ParseState *pstate;
  Node *expr;

  if (col->raw_default == NULL)
    return NULL;
  pstate = make_parsestate(NULL);
  expr = cookDefault(pstate, col->raw_default, typid, typmod, col->colname, 0);
  free_parsestate(pstate);
  return nodeToString(expr);
}

/*
 * Create the table a CREATE TABLE, after parse analysis, describes, in sm,
 * in this transaction: its columns and their NOT NULL.  Returns its address.
 */
ObjectAddress smstore_create_table(CreateStmt *stmt) {
  std::vector<sm_column_spec_t> columns;
  Oid nspoid;
  bool temp;
  uint64_t oid;
  ObjectAddress address;
  ListCell *l;

  check_create_table(stmt);
  nspoid = RangeVarGetCreationNamespace(stmt->relation);
  /* A table in pg_temp is temporary, and a temporary one is in no other. */
  RangeVarAdjustRelationPersistence(stmt->relation, nspoid);
  temp = stmt->relation->relpersistence == RELPERSISTENCE_TEMP;
  if (!temp && stmt->oncommit != ONCOMMIT_NOOP)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("ON COMMIT can only be used on temporary tables")));
  foreach (l, stmt->tableElts) {
    ColumnDef *col = lfirst_node(ColumnDef, l);
    Oid typid;
    int32 typmod;

    if (col->collClause != NULL || col->cooked_default != NULL ||
        col->generated != 0 || !only_taken(col->constraints))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("column \"%s\" asks for what CREATE TABLE "
                             "does not support yet",
                             col->colname)));
    typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
    /* A numeric with no precision is the default one, kept in sm. */
    if (typid == NUMERICOID && typmod < (int32)VARHDRSZ)
      typmod = NUMERIC_DEFAULT_TYPMOD;
    columns.push_back({col->colname, type_code(typid, typmod), typmod,
                       column_not_null(stmt, col),
                       column_default(col, typid, typmod)});
    /*
     * An identity column's sequence was made before the table, by parse
     * analysis; sm makes the column own it as it makes the table.
     */
    if (col->identity != 0) {
      columns.back().identity = col->identity;
      columns.back().identity_seq =
          RangeVarGetRelid(col->identitySequence, NoLock, false);
    }
  }
  sm_session_t *s = xact_session();

  /* A temporary table is the session's alone, in no namespace of sm's. */
  if (waiting([&] {
        return temp ? sm_cat_create_temp_table(s, stmt->relation->relname,
                                               columns, &oid)
                    : sm_cat_create_table(s, nspoid, stmt->relation->relname,
                                          columns, &oid);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists",
                             stmt->relation->relname)));
    sm_error("create a table");
  }
  /* What its ON COMMIT asks is done before each commit (xact.c). */
  register_on_commit_action((Oid)oid, stmt->oncommit);
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}

/* Refuse what a CREATE SEQUENCE asks that sm's sequences do not have. */
static void refuse_sequence_option(const char *what) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("CREATE SEQUENCE supports only START and INCREMENT "
                         "yet, not %s",
                         what)));
}

/*
 * Create the sequence a CREATE SEQUENCE describes, in sm, in this
 * transaction: counting up from START, by INCREMENT, both at least 1.
 * Returns its address, or InvalidObjectAddress if IF NOT EXISTS found the
 * name taken.
 */
ObjectAddress smstore_create_sequence(CreateSeqStmt *stmt) {
  const char *name = stmt->sequence->relname;
  int64 start = 1, increment = 1;
  Oid nspoid;
  uint64_t oid;
  ObjectAddress address;
  ListCell *l;

  if (stmt->ownerId != InvalidOid)
    refuse_sequence_option("an owner");
  if (stmt->sequence->relpersistence != RELPERSISTENCE_PERMANENT)
    refuse_sequence_option("TEMPORARY or UNLOGGED");
  foreach (l, stmt->options) {
    DefElem *opt = lfirst_node(DefElem, l);

    if (strcmp(opt->defname, "start") == 0)
      start = defGetInt64(opt);
    else if (strcmp(opt->defname, "increment") == 0)
      increment = defGetInt64(opt);
    else if (strcmp(opt->defname, "as") == 0) {
      /*
       * sm's counter is a bigint's, and keeps no bound: AS smallint or
       * integer, as a SERIAL column's sequence is, is taken, and a value
       * past the type's range fails where it is stored, out of range.
       */
      Oid type = typenameTypeId(NULL, defGetTypeName(opt));

      if (type != INT2OID && type != INT4OID && type != INT8OID)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        stmt->for_identity
                            ? errmsg("identity column type must be smallint, "
                                     "integer, or bigint")
                            : errmsg("sequence type must be smallint, "
                                     "integer, or bigint")));
    } else
      refuse_sequence_option(opt->defname);
  }
  if (start < 1 || increment < 1)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a sequence counts up from 1 or more, by 1 or "
                           "more, yet")));
  nspoid = RangeVarGetCreationNamespace(stmt->sequence);
  if (stmt->if_not_exists && OidIsValid(smstore_relation_oid(name, nspoid))) {
    ereport(NOTICE, (errcode(ERRCODE_DUPLICATE_TABLE),
                     errmsg("relation \"%s\" already exists, skipping", name)));
    return InvalidObjectAddress;
  }
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_create_sequence(s, nspoid, name, (uint64_t)start,
                                      (uint64_t)increment, &oid);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists", name)));
    sm_error("create a sequence");
  }
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}

/*
 * The name of the sequence oid, as this transaction sees it, or NULL if
 * there is none.
 */
static char *sequence_name(Oid oid) {
  Oid nspoid;

  return smstore_sequence_lookup(oid, &nspoid);
}

/*
 * The name of the sequence oid, as this transaction sees it, and its
 * namespace in *nspoid; NULL if there is none.
 */
char *smstore_sequence_lookup(Oid oid, Oid *nspoid) {
  uint64_t nsp;
  std::string name;

  switch (sm_cat_sequence_name(xact_session(), oid, &nsp, name)) {
  case 0:
    *nspoid = (Oid)nsp;
    return pstrdup(name.c_str());
  case 1:
    return NULL;
  default:
    sm_error("look up a sequence");
  }
  return NULL; /* not reached */
}

/* Whether there is a sequence oid, as this transaction sees it. */
bool smstore_sequence_exists(Oid oid) { return sequence_name(oid) != NULL; }

/* The name of the sequence oid, for a message; its oid if it is gone. */
const char *smstore_sequence_name(Oid oid) {
  char *name = sequence_name(oid);

  return name != NULL ? name : psprintf("%u", oid);
}

/*
 * The next value of the sequence oid, from sm.  It is not transactional:
 * a rollback does not give it back.
 */
int64 smstore_nextval(Oid oid) {
  sm_session_t *s = xact_session();
  uint64_t value;

  if (sm_cat_nextval(s, oid, &value) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg("sequence with OID %u does not exist", oid)));
    sm_error("draw a sequence's next value");
  }
  if (value > (uint64_t)PG_INT64_MAX)
    ereport(ERROR, (errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
                    errmsg("nextval: reached maximum value of sequence "
                           "\"%s\" (" INT64_FORMAT ")",
                           smstore_sequence_name(oid), PG_INT64_MAX)));
  return (int64)value;
}

/*
 * Drop the sequence rv names, from sm, in this transaction.  One that is not
 * there is an error, or with missing_ok a notice; a relation of another kind
 * by that name is an error.
 */
static void drop_sequence(RangeVar *rv, bool missing_ok) {
  Oid oid = RangeVarGetRelid(rv, NoLock, true);

  if (!OidIsValid(oid)) {
    if (!missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg("sequence \"%s\" does not exist", rv->relname)));
    ereport(NOTICE, (errmsg("sequence \"%s\" does not exist, skipping",
                            rv->relname)));
    return;
  }
  if (!smstore_sequence_exists(oid))
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a sequence", rv->relname)));
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_sequence(s, oid); }) != 0) {
    if (sm_errnum() == SM_ERR_INUSE)
      ereport(ERROR,
              (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
               errmsg("cannot drop sequence %s because other objects depend "
                      "on it",
                      rv->relname),
               errdetail("A column owns it: it is dropped with its table."),
               errhint("Drop the column, or for a SERIAL column's, use ALTER "
                       "SEQUENCE ... OWNED BY NONE first.")));
    sm_error("drop a sequence");
  }
}

/*
 * Drop the sequences a DROP SEQUENCE names, from sm, in this transaction.
 * CASCADE and RESTRICT are the same: nothing depends on a sequence in sm.
 */
void smstore_drop_sequences(DropStmt *stmt) {
  ListCell *l;

  foreach (l, stmt->objects)
    drop_sequence(makeRangeVarFromNameList((List *)lfirst(l)),
                  stmt->missing_ok);
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

/* This transaction's handle of the table oid in a mode covering mode. */
static sm_tbl_t *held_table(Oid oid, sm_lockmode_t mode) {
  auto it = open_tables().find(oid);

  if (it != open_tables().end())
    for (open_table_t &ot : it->second)
      if (mode_covers(ot.mode, mode))
        return ot.tbl;
  return NULL;
}

/*
 * The table oid, opened in this transaction in mode, taking the lock that
 * goes with it until the transaction ends: IS to read, IX to write, a
 * stronger one for LOCK TABLE.  NULL if there is no such table.
 */
static sm_tbl_t *open_table(Oid oid, sm_lockmode_t mode) {
  sm_session_t *s = xact_session();
  sm_tbl_t *tbl = held_table(oid, mode);

  if (tbl != NULL)
    return tbl;
  if (waiting([&] { return sm_tbl_open(s, oid, mode, &tbl); }) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      return NULL;
    sm_error("open a table");
  }
  /* Kept until the transaction ends, which closes it. */
  open_tables()[oid].push_back({mode, tbl, GetCurrentTransactionNestLevel()});
  return tbl;
}

/*
 * As open_table, but never waiting for the lock: NULL with *blocked true if
 * another transaction holds it in a mode that conflicts, NULL with *blocked
 * false if there is no such table.
 */
static sm_tbl_t *try_open_table(Oid oid, sm_lockmode_t mode, bool *blocked) {
  sm_session_t *s = xact_session();
  sm_tbl_t *tbl = held_table(oid, mode);
  int rc;

  *blocked = false;
  if (tbl != NULL)
    return tbl;
  /* Not through waiting(): this call never waits, so is never cut short. */
  rc = sm_tbl_tryopen(s, oid, mode, &tbl);
  if (rc == 1) {
    *blocked = true;
    return NULL;
  }
  if (rc != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      return NULL;
    sm_error("open a table");
  }
  /* Kept until the transaction ends, which closes it. */
  open_tables()[oid].push_back({mode, tbl, GetCurrentTransactionNestLevel()});
  return tbl;
}

/* An index's table, by the index's oid. */
typedef struct index_table_t {
  Oid index; /* the hash key */
  Oid table;
} index_table_t;

/*
 * The table of each index of the tables smstore_table_desc() described, or
 * that a listing (smstore_relations()) or a lookup by name found, in this
 * thread: made on first use, in CacheMemoryContext, with the descriptors.
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
  case 1:
    return BOOLOID;
  case 2:
    return INT2OID;
  case 3:
    return INT4OID;
  case 4:
    return INT8OID;
  case 5:
    return FLOAT4OID;
  case 6:
    return FLOAT8OID;
  case 8:
    return TEXTOID;
  case 9:
    return VARCHAROID;
  case 10:
    return BYTEAOID;
  case 11:
    return DATEOID;
  case 12:
    return TIMEOID;
  case 13:
    return TIMESTAMPOID;
  case 14:
    return TIMESTAMPTZOID;
  case 15:
    return INTERVALOID;
  case 16:
    return UUIDOID;
  case 17:
  case 18:
    return NUMERICOID;
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
  desc->version = def->version;
  desc->nspoid =
      def->nspoid == SM_NSP_TEMP ? PG_TEMP_NAMESPACE : (Oid)def->nspoid;
  desc->name = pstrdup(def->name.c_str());
  desc->ncolumns = (int)def->columns.size();
  desc->columns =
      (smstore_column_t *)palloc0(desc->ncolumns * sizeof(smstore_column_t));
  for (int i = 0; i < desc->ncolumns; i++) {
    const sm_column_def_t &c = def->columns[i];
    smstore_column_t *col = &desc->columns[i];

    col->name = pstrdup(c.name.c_str());
    col->dropped = c.dropped;
    col->default_expr =
        c.default_expr ? pstrdup(c.default_expr->c_str()) : NULL;
    col->identity = c.identity;
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
    ix->primary = x.primary;
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
 * The sequence the identity column attnum (1-based) of the table relid
 * takes its values from, or InvalidOid if it is not one.
 */
Oid smstore_identity_sequence(Oid relid, AttrNumber attnum) {
  sm_tbl_t *tbl = open_table(relid, SM_LOCKMODE_IS);

  if (tbl == NULL || attnum < 1 ||
      attnum > (AttrNumber)sm_tbl_def(tbl)->columns.size())
    return InvalidOid;
  const sm_column_def_t &c = sm_tbl_def(tbl)->columns[attnum - 1];
  return c.identity != 0 && !c.dropped ? (Oid)c.identity_seq : InvalidOid;
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
  const sm_datum_t *row =
      row_to_sm(tbl, slot->tts_tupleDescriptor, slot->tts_values,
                slot->tts_isnull, slot->tts_nvalid, NULL);
  if (waiting([&] {
        return sm_tbl_insert(tbl, row, slot->tts_isnull, slot->tts_nvalid,
                             &rowno);
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
 * Insert the slot's row into rel, a table in sm, unless a row already has
 * its key in one of the unique indexes arbiters lists by oid, or in any
 * unique index of rel if the list is empty: INSERT ... ON CONFLICT.  True if
 * one has, with that row's tid in *conflict and nothing written; false with
 * the row inserted, its tid in the slot.  This waits for another session's
 * insert of the key that is not yet committed, and above READ COMMITTED
 * fails on a key taken since the snapshot.  The row found is not locked,
 * nor its key.
 */
bool smstore_insert_arbiter(Relation rel, TupleTableSlot *slot, List *arbiters,
                            ItemPointer conflict) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IX);
  std::vector<uint64_t> oids;
  uint64_t rowno;
  ListCell *l;
  int rc;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  foreach (l, arbiters)
    oids.push_back(lfirst_oid(l));
  slot_getallattrs(slot);
  const sm_datum_t *row =
      row_to_sm(tbl, slot->tts_tupleDescriptor, slot->tts_values,
                slot->tts_isnull, slot->tts_nvalid, NULL);
  rc = waiting([&] {
    return sm_tbl_insert_arbiter(tbl, row, slot->tts_isnull, slot->tts_nvalid,
                                 oids.data(), oids.size(), &rowno);
  });
  if (rc < 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR,
              (errcode(ERRCODE_UNIQUE_VIOLATION),
               errmsg("duplicate key value violates a unique index on \"%s\"",
                      RelationGetRelationName(rel)),
               errdetail("%s", sm_errmsg())));
    sm_error("insert a row");
  }
  if (rc == 1) {
    smstore_rowno_to_tid(rowno, conflict);
    return true;
  }
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
  return false;
}

/* Raise the error of a bulk load's call that failed, on rel. */
static void load_error(Relation rel, const char *what) {
  if (sm_errnum() == SM_ERR_DUPKEY)
    ereport(ERROR,
            (errcode(ERRCODE_UNIQUE_VIOLATION),
             errmsg("duplicate key value violates a unique index on \"%s\"",
                    RelationGetRelationName(rel)),
             errdetail("%s", sm_errmsg())));
  sm_error(what);
}

/*
 * Hand the n slots' rows to sm's bulk load of rel, a table in sm, beginning
 * the load at the first: COPY's rows, a batch at a time.  Into an empty
 * table sm writes them to sorted files, which its store takes whole when the
 * load ends; for that it takes the table's lock exclusive until the
 * transaction ends, so no other session reads or writes the table meanwhile,
 * and a duplicate key is found at the end.  Into a table with rows it
 * inserts each (../sm/BULKLOAD.md).  Each slot's tid is its row's rowno.
 */
void smstore_load(Relation rel, TupleTableSlot **slots, int n) {
  Oid oid = RelationGetRelid(rel);
  sm_tbl_t *tbl = open_table(oid, SM_LOCKMODE_IX);

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  if (load_oid != oid) {
    /* Waits for the table's other users, if it is empty. */
    if (waiting([&] { return sm_tbl_load_begin(tbl); }) != 0)
      load_error(rel, "begin a bulk load");
    load_oid = oid;
  }
  for (int i = 0; i < n; i++) {
    TupleTableSlot *slot = slots[i];
    uint64_t rowno;

    slot_getallattrs(slot);
    const sm_datum_t *row =
        row_to_sm(tbl, slot->tts_tupleDescriptor, slot->tts_values,
                  slot->tts_isnull, slot->tts_nvalid, NULL);
    if (waiting([&] {
          return sm_tbl_load_add(tbl, row, slot->tts_isnull, slot->tts_nvalid,
                                 &rowno);
        }) != 0)
      load_error(rel, "load a row");
    smstore_rowno_to_tid(rowno, &slot->tts_tid);
    slot->tts_tableOid = oid;
  }
}

/*
 * End the bulk load begun on rel, if one is: its rows are in the table, and
 * a key that two of them share fails here.
 */
void smstore_load_end(Relation rel) {
  Oid oid = RelationGetRelid(rel);

  if (load_oid != oid)
    return;
  load_oid = InvalidOid;
  if (sm_tbl_load_end(open_table(oid, SM_LOCKMODE_IX)) != 0)
    load_error(rel, "end a bulk load");
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
  slot_from_sm(slot);
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
 * Drop the table rv names, from sm, in this transaction: with its indexes,
 * rows and sequence.  One that is not there is an error, or with missing_ok
 * a notice.
 */
static void drop_table(RangeVar *rv, bool missing_ok) {
  Oid oid;
  Relation rel;

  /* Locked against other sessions' use until the transaction ends. */
  oid = RangeVarGetRelid(rv, AccessExclusiveLock, true);
  if (!OidIsValid(oid)) {
    if (!missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg("table \"%s\" does not exist", rv->relname)));
    ereport(NOTICE,
            (errmsg("table \"%s\" does not exist, skipping", rv->relname)));
    return;
  }
  /* A catalog a query can read is not sm's to drop. */
  if (oid < FirstNormalObjectId)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied: \"%s\" is a system catalog",
                           rv->relname)));
  rel = relation_open(oid, NoLock);
  if (rel->rd_rel->relkind != RELKIND_RELATION)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a table", rv->relname)));
  relation_close(rel, NoLock);
  smstore_drop_table(oid);
}

/*
 * Drop the table oid from sm, in this transaction, with its indexes, rows
 * and sequence: DROP TABLE's, and ON COMMIT DROP's.
 */
void smstore_drop_table(Oid oid) {
  Relation rel = relation_open(oid, NoLock);

  /* Every session forgets its descriptor once this one commits. */
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);
  /* Its ON COMMIT action, if it has one, goes with it. */
  remove_on_commit_action(oid);
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_table(s, oid); }) != 0)
    sm_error("drop a table");
}

/*
 * Drop every temporary table of the session, in this transaction: DISCARD
 * TEMP's.  sm drops them itself when the session closes its database.
 */
void smstore_drop_temp_tables(void) {
  std::vector<std::string> names;

  if (sm_cat_list_tables(xact_session(), SM_NSP_TEMP, names) != 0)
    sm_error("list the temporary tables");
  for (const std::string &name : names)
    smstore_drop_table(smstore_relation_oid(name.c_str(), PG_TEMP_NAMESPACE));
}

/*
 * Drop the tables a DROP TABLE names, from sm, in this transaction: each
 * with its indexes, rows and sequence.  CASCADE and RESTRICT are the same:
 * nothing but its indexes depends on a table in sm.
 */
void smstore_drop_tables(DropStmt *stmt) {
  ListCell *l;

  foreach (l, stmt->objects)
    drop_table(makeRangeVarFromNameList((List *)lfirst(l)), stmt->missing_ok);
}

/*
 * Ready the table relid, named name, for an ALTER that changes its
 * definition: an error if it is not a table, or if this transaction is
 * using it.  Every session forgets its descriptor once this one commits,
 * and this one at its next command; and this transaction's handles of it,
 * which hold the old definition, are closed, so the next use opens one
 * afresh.
 */
static void alter_table_rel(Oid relid, const char *name) {
  /*
   * No sm lock is held yet, and none is taken here: the DDL takes X itself,
   * and a NOT NULL column with no default asks for it without waiting (step
   * 110).  What is read -- the relkind, this session's use of the table --
   * no DDL changes, so the descriptor is had without relation_open(), which
   * asserts a lock held.
   */
  Relation rel = RelationIdGetRelation(relid);

  if (!RelationIsValid(rel))
    elog(ERROR, "could not open relation with OID %u", relid);

  if (rel->rd_rel->relkind != RELKIND_RELATION)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a table", name)));
  /* No scan of it is open, so no handle closed below is in use. */
  CheckTableNotInUse(rel, "ALTER TABLE");
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);

  auto it = open_tables().find(relid);

  if (it == open_tables().end())
    return;
  for (open_table_t &ot : it->second)
    sm_tbl_close(ot.tbl);
  open_tables().erase(it);
}

/* Refuse a column name a system column has, as PostgreSQL does. */
static void check_column_name(const char *name) {
  if (SystemAttributeByName(name) != NULL)
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
                    errmsg("column name \"%s\" conflicts with a system column "
                           "name",
                           name)));
}

/* Whether type names one of the serial types, which are not types. */
static bool is_serial(const TypeName *type) {
  static const char *const serials[] = {"smallserial", "serial2",   "serial",
                                        "serial4",     "bigserial", "serial8"};
  const char *name;

  if (type->arrayBounds != NIL || list_length(type->names) > 2 ||
      (list_length(type->names) == 2 &&
       strcmp(strVal(linitial(type->names)), "pg_catalog") != 0))
    return false;
  name = strVal(llast(type->names));
  for (const char *serial : serials)
    if (strcmp(name, serial) == 0)
      return true;
  return false;
}

/*
 * The value of expr, a column's DEFAULT that calls nothing volatile, as sm
 * takes a datum: what rows older than the column read it as.  False if it
 * is null.  A varlena is untoasted, a numeric made sm's in buf.
 */
static bool default_value(Expr *expr, Oid typid, int32 typmod, sm_datum_t *out,
                          uint8 *buf) {
  EState *estate = CreateExecutorState();
  ExprState *state;
  Datum value;
  bool isnull;
  int16 typlen;
  bool typbyval;

  /* Evaluated once, now, as PostgreSQL's attmissingval is. */
  state = ExecPrepareExpr(expression_planner(expr), estate);
  value = ExecEvalExpr(state, GetPerTupleExprContext(estate), &isnull);
  if (!isnull) {
    get_typlenbyval(typid, &typlen, &typbyval);
    if (typid == NUMERICOID)
      *out = numeric_to_sm(value, typmod, buf);
    else if (typlen == -1)
      *out = (sm_datum_t)PointerGetDatum(
          pg_detoast_datum_copy((struct varlena *)DatumGetPointer(value)));
    else
      *out = (sm_datum_t)datumCopy(value, typbyval, typlen);
  }
  FreeExecutorState(estate);
  return !isnull;
}

/*
 * ADD COLUMN col to rel, the table relid, in sm: NOT NULL and a DEFAULT
 * only.  Rows already there read the default, evaluated now, so one that
 * calls something volatile is refused, as is a serial column: PostgreSQL
 * rewrites the table for those (../sm/NEXT.md).  NOT NULL with no default
 * needs the table empty.
 */
static void add_column(Oid relid, const char *relname, ColumnDef *col,
                       bool if_not_exists) {
  Node *raw_default = NULL;
  bool not_null = col->is_not_null;
  Oid typid;
  int32 typmod;
  const char *default_expr = NULL;
  sm_datum_t missing = 0;
  bool has_missing = false;
  uint8 buf[16];
  ListCell *l;

  check_column_name(col->colname);
  if (col->typeName != NULL && is_serial(col->typeName))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER TABLE ... ADD COLUMN of a serial column is "
                           "not supported yet")));
  foreach (l, col->constraints) {
    Constraint *c = lfirst_node(Constraint, l);

    if (c->contype == CONSTR_NOTNULL)
      not_null = true;
    else if (c->contype == CONSTR_DEFAULT)
      raw_default = c->raw_expr;
    else if (c->contype != CONSTR_NULL)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ALTER TABLE ... ADD COLUMN supports only NOT "
                             "NULL and DEFAULT yet")));
  }
  if (col->collClause != NULL || col->generated != 0 || col->identity != 0 ||
      col->compression != NULL || col->storage_name != NULL)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER TABLE ... ADD COLUMN supports only NOT "
                           "NULL and DEFAULT yet")));
  typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
  /* A numeric with no precision is the default one, kept in sm. */
  if (typid == NUMERICOID && typmod < (int32)VARHDRSZ)
    typmod = NUMERIC_DEFAULT_TYPMOD;
  if (raw_default != NULL) {
    ParseState *pstate = make_parsestate(NULL);
    Node *expr =
        cookDefault(pstate, raw_default, typid, typmod, col->colname, 0);

    free_parsestate(pstate);
    if (contain_volatile_functions(expr))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ALTER TABLE ... ADD COLUMN with a volatile "
                             "DEFAULT is not supported yet")));
    default_expr = nodeToString(expr);
    has_missing = default_value((Expr *)expr, typid, typmod, &missing, buf);
  }
  sm_column_spec_t spec = {col->colname, type_code(typid, typmod), typmod,
                           not_null, default_expr};
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_add_column(s, relid, spec,
                                 has_missing ? &missing : nullptr);
      }) == 0)
    return;
  if (sm_errnum() == SM_ERR_DUPKEY) {
    if (!if_not_exists)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
                      errmsg("column \"%s\" of relation \"%s\" already exists",
                             col->colname, relname)));
    ereport(NOTICE,
            (errcode(ERRCODE_DUPLICATE_COLUMN),
             errmsg("column \"%s\" of relation \"%s\" already exists, skipping",
                    col->colname, relname)));
    return;
  }
  if (sm_errnum() == SM_ERR_NOTNULL)
    ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION),
                    errmsg("column \"%s\" of relation \"%s\" contains null "
                           "values",
                           col->colname, relname)));
  if (sm_errnum() == SM_ERR_BLOCKED)
    ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                    errmsg("could not obtain lock on relation \"%s\"", relname),
                    errdetail("A NOT NULL column with no default is added "
                              "only to an empty table no one else is using.")));
  sm_error("add a column");
}

/* DROP COLUMN name of the table relid, in sm; it reads as null from now. */
static void drop_column(Oid relid, const char *relname, const char *name,
                        bool missing_ok) {
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_column(s, relid, name); }) == 0)
    return;
  if (sm_errnum() == SM_ERR_NOTFOUND) {
    if (!missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                      errmsg("column \"%s\" of relation \"%s\" does not exist",
                             name, relname)));
    ereport(NOTICE, (errmsg("column \"%s\" of relation \"%s\" does not exist, "
                            "skipping",
                            name, relname)));
    return;
  }
  /* An index on it would have to go too: CASCADE does not do that yet. */
  if (sm_errnum() == SM_ERR_INVALID)
    ereport(ERROR,
            (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
             errmsg("cannot drop column %s of table %s because other objects "
                    "depend on it",
                    name, relname),
             errdetail("%s", sm_errmsg()), errhint("Drop the index first.")));
  sm_error("drop a column");
}

/*
 * Do an ALTER TABLE of the table relid, in sm, in this transaction, one
 * subcommand after another: ADD COLUMN and DROP COLUMN, the subcommands
 * AlterTableLookupRelation() lets through.
 */
void smstore_alter_table(Oid relid, AlterTableStmt *stmt) {
  const char *relname = stmt->relation->relname;
  ListCell *l;

  foreach (l, stmt->cmds) {
    AlterTableCmd *cmd = lfirst_node(AlterTableCmd, l);

    alter_table_rel(relid, relname);
    if (cmd->subtype == AT_AddColumn)
      add_column(relid, relname, castNode(ColumnDef, cmd->def),
                 cmd->missing_ok);
    else
      drop_column(relid, relname, cmd->name, cmd->missing_ok);
    /* The next subcommand sees what this one did. */
    CommandCounterIncrement();
  }
}

/*
 * The table an ALTER TABLE ... RENAME names, locked, or InvalidOid if it is
 * not there and stmt says IF EXISTS.
 */
static Oid rename_target(RenameStmt *stmt) {
  /* Locked against other sessions' use until the transaction ends. */
  Oid relid = RangeVarGetRelid(stmt->relation, AccessExclusiveLock, true);

  if (!OidIsValid(relid)) {
    if (!stmt->missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg("relation \"%s\" does not exist",
                             stmt->relation->relname)));
    ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping",
                            stmt->relation->relname)));
    return InvalidOid;
  }
  alter_table_rel(relid, stmt->relation->relname);
  return relid;
}

/*
 * ALTER TABLE ... RENAME TO: rename the table in sm, in this transaction,
 * in its schema.  Returns its address.
 */
ObjectAddress smstore_rename_table(RenameStmt *stmt) {
  Oid relid = rename_target(stmt);
  ObjectAddress address;

  if (!OidIsValid(relid))
    return InvalidObjectAddress;
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_rename_table(s, relid, stmt->newname); }) !=
      0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists", stmt->newname)));
    sm_error("rename a table");
  }
  ObjectAddressSet(address, RelationRelationId, relid);
  return address;
}

/*
 * ALTER TABLE ... RENAME COLUMN: rename the column in sm, in this
 * transaction; its indexes go on using it.  Returns its address.
 */
ObjectAddress smstore_rename_column(RenameStmt *stmt) {
  Oid relid = rename_target(stmt);
  const char *relname = stmt->relation->relname;
  AttrNumber attnum = InvalidAttrNumber;
  ObjectAddress address;
  Relation rel;

  if (!OidIsValid(relid))
    return InvalidObjectAddress;
  check_column_name(stmt->newname);
  /*
   * The column's attnum, for the address alone: sm renames by name, under
   * the X it takes.  So the descriptor is had without a lock, as
   * alter_table_rel() has it.
   */
  rel = RelationIdGetRelation(relid);
  if (!RelationIsValid(rel))
    elog(ERROR, "could not open relation with OID %u", relid);
  for (int i = 0; i < RelationGetDescr(rel)->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);

    if (!att->attisdropped && strcmp(NameStr(att->attname), stmt->subname) == 0)
      attnum = att->attnum;
  }
  relation_close(rel, NoLock);
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_rename_column(s, relid, stmt->subname, stmt->newname);
      }) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                      errmsg("column \"%s\" does not exist", stmt->subname)));
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
                      errmsg("column \"%s\" of relation \"%s\" already exists",
                             stmt->newname, relname)));
    sm_error("rename a column");
  }
  ObjectAddressSubSet(address, RelationRelationId, relid, attnum);
  return address;
}

/*
 * The table oid's statistics, as this transaction sees them, nullptr if it
 * was never analyzed: sm's, shared by every session and copied by none.
 */
static std::shared_ptr<const sm_stats_t> table_stats(Oid oid) {
  std::shared_ptr<const sm_stats_t> st;

  if (sm_tbl_stats(xact_session(), oid, st) < 0)
    sm_error("read a table's statistics");
  return st;
}

/*
 * The compression of a table's rows on disk when it was analyzed: its heap's
 * bytes in sorted files over its rows' bytes, at most 1, and 1 when nothing
 * was on disk (STATISTICS.md section 2.2).
 */
static double compression(const sm_stats_t &st) {
  double raw = st.live_rows * st.avg_row_bytes;

  if (st.heap_disk == 0 || raw <= 0)
    return 1.0;
  return std::min(1.0, (double)st.heap_disk / raw);
}

/*
 * How much the table has grown since it was analyzed, and its heap's bytes
 * now, in *now, at the compression recorded: bytes in memtables are
 * uncompressed, so they count at c.  The growth is its bytes now over then;
 * but when the analyze found nothing on disk, its compression is unknown,
 * and a flush since would read as a shrink, so the growth is then its row
 * numbers now over then, which counts inserts and not deletes
 * (STATISTICS.md section 2.2).
 */
static double growth(sm_tbl_t *tbl, const sm_stats_t &st, double *now) {
  uint64_t disk, mem, n;
  double c = compression(st), then;

  if (sm_tbl_size(tbl, &disk, &mem) != 0)
    sm_error("size a table");
  *now = disk + mem * c;
  if (st.heap_disk == 0) {
    if (sm_tbl_rowno_limit(tbl, &n) != 0)
      sm_error("read a table's row numbers");
    return st.rowno_limit > 0 ? (double)n / st.rowno_limit : 1.0;
  }
  then = st.heap_disk + st.heap_mem * c;
  return then > 0 ? *now / then : 1.0;
}

/*
 * A table's pages and rows for the planner (STATISTICS.md section 2.2): its
 * bytes now, in 8 KB pages, and the rows the last analyze counted, grown or
 * shrunk with its bytes since.  False for a table never analyzed, or whose
 * heap holds no bytes, new or truncated: the caller plans it as a heap
 * never analyzed.
 */
bool smstore_estimate_size(Oid oid, double *pages, double *tuples) {
  sm_tbl_t *tbl = open_table(oid, SM_LOCKMODE_IS);
  double now;

  if (tbl == NULL)
    return false;
  std::shared_ptr<const sm_stats_t> st = table_stats(oid);
  if (st == nullptr)
    return false;
  double g = growth(tbl, *st, &now);
  if (now <= 0)
    return false;
  *tuples = st->live_rows * g;
  *pages = std::max(1.0, std::ceil(now / BLCKSZ));
  return true;
}

/*
 * An index's pages and entries for the planner: its bytes now, in pages,
 * and the entries the last analyze counted -- a partial index's share of
 * the rows -- grown as its table has.  False if its table was never
 * analyzed, or the index was made since.
 */
bool smstore_estimate_index_size(Oid table, Oid index, double *pages,
                                 double *tuples) {
  sm_tbl_t *tbl = open_table(table, SM_LOCKMODE_IS);
  uint64_t disk, mem;
  double now;

  if (tbl == NULL)
    return false;
  std::shared_ptr<const sm_stats_t> st = table_stats(table);
  if (st == nullptr)
    return false;
  for (const sm_stats_index_t &x : st->indexes) {
    if (x.oid != index)
      continue;
    double g = growth(tbl, *st, &now);
    if (sm_tbl_idx_size(tbl, index, &disk, &mem) != 0)
      sm_error("size an index");
    *tuples = x.entries * g;
    *pages = std::max(1.0, std::ceil((disk + mem * compression(*st)) / BLCKSZ));
    return true;
  }
  return false;
}

/*
 * The bytes rel, a table or an index in sm, takes now: in sorted files and
 * in memtables, as sm counts them (sm_tbl_size, sm_tbl_idx_size), for
 * PostgreSQL's size functions (dbsize.c).  0 for one no longer there.
 */
int64 smstore_relation_bytes(Relation rel) {
  bool index = rel->rd_rel->relkind == RELKIND_INDEX;
  sm_tbl_t *tbl = open_table(
      index ? rel->rd_index->indrelid : RelationGetRelid(rel), SM_LOCKMODE_IS);
  uint64_t disk, mem;

  if (tbl == NULL)
    return 0;
  if ((index ? sm_tbl_idx_size(tbl, RelationGetRelid(rel), &disk, &mem)
             : sm_tbl_size(tbl, &disk, &mem)) != 0)
    sm_error("size a table");
  return (int64)(disk + mem);
}

/* An array of the float4s f, n of them, for a statistic's numbers. */
static Datum float4_array(const float *f, int n) {
  std::vector<Datum> d;

  for (int i = 0; i < n; i++)
    d.push_back(Float4GetDatum(f[i]));
  return PointerGetDatum(construct_array_builtin(d.data(), n, FLOAT4OID));
}

/*
 * The datum of a value sm kept in a statistic, bytes, of a column of typid
 * and typmod: sm's datum, or for numeric the numeric made of it.
 */
static Datum stat_datum(const sm_type_t *smtype, Oid typid, int32 typmod,
                        const std::string &bytes) {
  sm_datum_t d = sm_datum_of_bytes(smtype, bytes);

  return typid == NUMERICOID ? numeric_from_sm(d, typmod) : (Datum)d;
}

/*
 * The pg_statistic row of relid's column attnum, made from sm's statistics
 * (STATISTICS.md section 2.3): a table's column, or an expression of one of
 * its indexes, by its index's oid.  Its slots are what PostgreSQL's ANALYZE
 * fills -- the common values with the type's =, the histogram and the
 * correlation with its < -- with the column's collation.  NULL if there is
 * none: a table never analyzed, a column without statistics, or an
 * inheritance tree.  The caller frees it with heap_freetuple().
 */
HeapTuple smstore_statistic_tuple(Oid relid, AttrNumber attnum, bool inh) {
  if (inh || attnum <= 0 || relid < FirstNormalObjectId)
    return NULL;
  /* An index's statistics are its table's, by the index's oid. */
  Oid table = smstore_index_table(relid);
  bool is_index = OidIsValid(table);

  if (!is_index)
    table = relid;
  std::shared_ptr<const sm_stats_t> st = table_stats(table);
  const sm_stat_t *found = NULL;

  if (st == nullptr)
    return NULL;
  for (const sm_stat_t &x : st->stats)
    if (x.has_index == is_index && (!is_index || x.index == relid) &&
        x.position == attnum - 1) {
      found = &x;
      break;
    }
  if (found == NULL)
    return NULL;

  /* The column's type and collation, from its descriptor. */
  Relation rel = relation_open(relid, NoLock);
  Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
  Oid typid = att->atttypid, coll = att->attcollation;
  int16 typlen = att->attlen;
  bool typbyval = att->attbyval;
  char typalign = att->attalign;
  int32 typmod = att->atttypmod;

  relation_close(rel, NoLock);
  TypeCacheEntry *tc =
      lookup_type_cache(typid, TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR);
  const sm_type_t *smtype = sm_type(type_code(typid, typmod));
  auto values_array = [&](const std::vector<std::string> &bytes) {
    std::vector<Datum> d;

    for (const std::string &b : bytes)
      d.push_back(stat_datum(smtype, typid, typmod, b));
    return PointerGetDatum(construct_array(d.data(), (int)d.size(), typid,
                                           typlen, typbyval, typalign));
  };

  Datum values[Natts_pg_statistic];
  bool nulls[Natts_pg_statistic];

  memset(values, 0, sizeof(values));
  memset(nulls, false, sizeof(nulls));
  values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
  values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
  values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(false);
  values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(found->null_frac);
  values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(found->width);
  values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(found->distinct);
  for (int k = 0; k < STATISTIC_NUM_SLOTS; k++) {
    nulls[Anum_pg_statistic_stanumbers1 - 1 + k] = true;
    nulls[Anum_pg_statistic_stavalues1 - 1 + k] = true;
  }
  /* Fill the next slot with kind, op, and its numbers and values. */
  int k = 0;
  auto slot = [&](int16 kind, Oid op, Datum numbers, bool has_numbers,
                  Datum vals, bool has_values) {
    values[Anum_pg_statistic_stakind1 - 1 + k] = Int16GetDatum(kind);
    values[Anum_pg_statistic_staop1 - 1 + k] = ObjectIdGetDatum(op);
    values[Anum_pg_statistic_stacoll1 - 1 + k] = ObjectIdGetDatum(coll);
    if (has_numbers) {
      values[Anum_pg_statistic_stanumbers1 - 1 + k] = numbers;
      nulls[Anum_pg_statistic_stanumbers1 - 1 + k] = false;
    }
    if (has_values) {
      values[Anum_pg_statistic_stavalues1 - 1 + k] = vals;
      nulls[Anum_pg_statistic_stavalues1 - 1 + k] = false;
    }
    k++;
  };
  if (!found->mcv.empty() && OidIsValid(tc->eq_opr))
    slot(STATISTIC_KIND_MCV, tc->eq_opr,
         float4_array(found->mcv_freq.data(), (int)found->mcv_freq.size()),
         true, values_array(found->mcv), true);
  if (found->bounds.size() >= 2 && OidIsValid(tc->lt_opr))
    slot(STATISTIC_KIND_HISTOGRAM, tc->lt_opr, 0, false,
         values_array(found->bounds), true);
  if (found->has_correlation && OidIsValid(tc->lt_opr))
    slot(STATISTIC_KIND_CORRELATION, tc->lt_opr,
         float4_array(&found->correlation, 1), true, 0, false);

  /* Formed in pg_statistic's shape, which it has no rows of. */
  TupleDesc desc = statistic_tupdesc();
  HeapTuple tup = heap_form_tuple(desc, values, nulls);

  FreeTupleDesc(desc);
  return tup;
}

/* An array of values' text, by the type's output function, as text[]. */
static Datum text_array(const sm_type_t *smtype, Oid typid, int32 typmod,
                        const std::vector<std::string> &bytes) {
  Oid out;
  bool varlena;
  std::vector<Datum> d;

  getTypeOutputInfo(typid, &out, &varlena);
  for (const std::string &b : bytes)
    d.push_back(CStringGetTextDatum(
        OidOutputFunctionCall(out, stat_datum(smtype, typid, typmod, b))));
  return PointerGetDatum(
      construct_array_builtin(d.data(), (int)d.size(), TEXTOID));
}

/*
 * Put the pg_stats rows of the table oid in sm into rsinfo's tuplestore,
 * one a statistic (STATISTICS.md section 2.5): a column's under the
 * table's name, an index expression's under the index's.  None if it was
 * never analyzed.
 */
static void put_stats_rows(ReturnSetInfo *rsinfo, Oid oid) {
  std::shared_ptr<const sm_stats_t> st = table_stats(oid);

  if (st == nullptr)
    return;
  for (const sm_stat_t &x : st->stats) {
    /* The column is the table's, or the index's for an expression. */
    Relation rel = relation_open(x.has_index ? (Oid)x.index : oid, NoLock);
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), x.position);
    const sm_type_t *smtype = sm_type(type_code(att->atttypid, att->atttypmod));
    NameData nsp, name;
    Datum values[17];
    bool nulls[17];

    namestrcpy(&nsp, get_namespace_name(RelationGetNamespace(rel)));
    namestrcpy(&name, RelationGetRelationName(rel));
    memset(nulls, true, sizeof(nulls));
    values[0] = NameGetDatum(&nsp);
    values[1] = NameGetDatum(&name);
    values[2] = NameGetDatum(&att->attname);
    values[3] = BoolGetDatum(false);
    values[4] = Float4GetDatum(x.null_frac);
    values[5] = Int32GetDatum(x.width);
    values[6] = Float4GetDatum(x.distinct);
    for (int i = 0; i <= 6; i++)
      nulls[i] = false;
    if (!x.mcv.empty()) {
      values[7] = text_array(smtype, att->atttypid, att->atttypmod, x.mcv);
      values[8] = float4_array(x.mcv_freq.data(), (int)x.mcv_freq.size());
      nulls[7] = nulls[8] = false;
    }
    if (x.bounds.size() >= 2) {
      values[9] = text_array(smtype, att->atttypid, att->atttypmod, x.bounds);
      nulls[9] = false;
    }
    if (x.has_correlation) {
      values[10] = Float4GetDatum(x.correlation);
      nulls[10] = false;
    }
    /* The tuplestore copies the row, attname too, before the close. */
    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    relation_close(rel, NoLock);
  }
}

/* pg_stats(): the pg_stats rows of every table in every schema in sm. */
Datum pg_stats_all(PG_FUNCTION_ARGS) {
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  std::vector<std::string> nspnames;

  InitMaterializedSRF(fcinfo, 0);
  if (sm_cat_list_namespaces(xact_session(), nspnames) != 0)
    sm_error("list the schemas");
  for (const std::string &nsp : nspnames) {
    Oid nspoid = smstore_namespace_oid(nsp.c_str());
    std::vector<std::string> tables;

    if (sm_cat_list_tables(xact_session(), nspoid, tables) != 0)
      sm_error("list a schema's tables");
    for (const std::string &t : tables)
      put_stats_rows(rsinfo, smstore_relation_oid(t.c_str(), nspoid));
  }
  return (Datum)0;
}

/* pg_stats(rel): the pg_stats rows of the table rel, none if not in sm. */
Datum pg_stats_rel(PG_FUNCTION_ARGS) {
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  Oid oid = PG_GETARG_OID(0);

  InitMaterializedSRF(fcinfo, 0);
  if (oid >= FirstNormalObjectId && !OidIsValid(smstore_index_table(oid)))
    put_stats_rows(rsinfo, oid);
  return (Datum)0;
}

/*
 * SKIP_LOCKED's report of a table it passed over: PostgreSQL's warning if
 * the statement named it, nothing in an ANALYZE of every table.
 */
static void skip_locked_table(const char *name, bool named) {
  if (named)
    ereport(
        WARNING,
        (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
         errmsg("skipping analyze of \"%s\" --- lock not available", name)));
}

/*
 * Analyze the table oid, named name for a message, in sm, in this
 * transaction: opened as a reader, so writers go on meanwhile.  A name
 * that is not a table's, a sequence's or an index's, is passed over with
 * a warning, as PostgreSQL passes over what it cannot analyze.  With
 * skip_locked a table that would be waited for -- held X, or analyzed by
 * another transaction not ended -- is passed over too, with a warning if
 * the statement named it.
 */
static void analyze_table(Oid oid, const char *name, bool verbose,
                          bool skip_locked, bool named) {
  bool blocked = false;
  sm_tbl_t *tbl = skip_locked ? try_open_table(oid, SM_LOCKMODE_IS, &blocked)
                              : open_table(oid, SM_LOCKMODE_IS);
  int rc;

  if (blocked) {
    skip_locked_table(name, named);
    return;
  }
  if (tbl == NULL) {
    ereport(WARNING,
            (errmsg("skipping \"%s\" --- cannot analyze non-tables", name)));
    return;
  }
  Relation rel = relation_open(oid, NoLock);

  /* sm keeps no statistics for a temporary table yet, and refuses. */
  if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
    relation_close(rel, NoLock);
    ereport(WARNING, (errmsg("skipping \"%s\" --- statistics are not kept "
                             "for a temporary table yet",
                             name)));
    return;
  }
  /* An analyze in sm samples, and may be interrupted: so through waiting(). */
  rc = waiting([&] {
    return skip_locked ? sm_tbl_tryanalyze(tbl, default_statistics_target)
                       : sm_tbl_analyze(tbl, default_statistics_target);
  });
  if (rc == 1) {
    relation_close(rel, NoLock);
    skip_locked_table(name, named);
    return;
  }
  if (rc != 0)
    sm_error("analyze a table");
  /* Plans of the table are made again, with its new statistics. */
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);
  if (verbose) {
    std::shared_ptr<const sm_stats_t> st = table_stats(oid);

    if (st != nullptr)
      ereport(INFO,
              (errmsg("\"%s\": %.0f live rows, %llu rows in sample, of %llu "
                      "row numbers in runs of %d",
                      name, st->live_rows, (unsigned long long)st->rows_sampled,
                      (unsigned long long)st->rowno_limit, st->run_length)));
  }
}

/*
 * ANALYZE: each table rels names, or with none every table in sm, analyzed
 * by sm in this transaction, so the statistics commit and roll back with
 * it (STATISTICS.md section 2.1).  Each takes default_statistics_target; a
 * column list is refused.  skip_locked is SKIP_LOCKED: a table held X,
 * or being analyzed by another transaction, is not waited for.
 */
void smstore_analyze(List *rels, bool verbose, bool skip_locked) {
  ListCell *l;

  if (rels == NIL) {
    std::vector<std::string> nspnames;

    if (sm_cat_list_namespaces(xact_session(), nspnames) != 0)
      sm_error("list the schemas");
    for (const std::string &nsp : nspnames) {
      Oid nspoid = smstore_namespace_oid(nsp.c_str());
      std::vector<std::string> tables;

      if (sm_cat_list_tables(xact_session(), nspoid, tables) != 0)
        sm_error("list a schema's tables");
      for (const std::string &t : tables)
        analyze_table(smstore_relation_oid(t.c_str(), nspoid), t.c_str(),
                      verbose, skip_locked, false);
    }
    return;
  }
  foreach (l, rels) {
    VacuumRelation *vrel = lfirst_node(VacuumRelation, l);

    if (vrel->va_cols != NIL)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ANALYZE of chosen columns is not supported yet")));
    analyze_table(RangeVarGetRelid(vrel->relation, NoLock, false),
                  vrel->relation->relname, verbose, skip_locked, true);
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
 * sm's lock for PostgreSQL's lockmode on a relation (smstore_lock_relation()):
 * X for DDL, IX to write, IS for the rest.
 */
static sm_lockmode_t relation_lock_mode(LOCKMODE lockmode) {
  return lockmode >= ExclusiveLock      ? SM_LOCKMODE_EX
         : lockmode == RowExclusiveLock ? SM_LOCKMODE_IX
                                        : SM_LOCKMODE_IS;
}

/*
 * What sm locks for a lock on relid: an index's table, as an index's
 * definition is its table's (sm3 keeps it in the table's record), so that
 * opening an index before its table reads no definition a DDL is changing;
 * else relid itself.
 */
static Oid lock_target(Oid relid) {
  Oid table = smstore_index_table(relid);

  return OidIsValid(table) ? table : relid;
}

/*
 * Lock the relation relid, if it is a table in sm, as PostgreSQL locks a
 * relation before it reads its definition (lmgr.c): so a definition a DDL
 * is changing is read once the DDL has ended.  Only the conflict with DDL,
 * which takes X, matters here: the call into sm that uses the table takes
 * what it needs, so a writer takes IX and anything not exclusive IS -- an
 * index build or an analyze in sm keeps no writer out.  An index known by
 * its table (smstore_index_table()) is locked as its table; a sequence is
 * not a table in sm, and takes nothing.
 */
void smstore_lock_relation(Oid relid, LOCKMODE lockmode) {
  /* Held until the transaction ends, as the handle is. */
  sm_tbl_t *tbl = open_table(lock_target(relid), relation_lock_mode(lockmode));

  /* What this session caches of it is from the version now held, or goes. */
  if (tbl != NULL)
    RelationCheckSmVersion(relid, lock_target(relid), sm_tbl_def(tbl)->version);
}

/*
 * As smstore_lock_relation, if the lock is free now: false, with nothing
 * taken, if another transaction holds it in a mode that conflicts.  A
 * relation that is no table in sm takes nothing, and is true.
 */
bool smstore_trylock_relation(Oid relid, LOCKMODE lockmode) {
  bool blocked;

  /* Held until the transaction ends, as the handle is. */
  sm_tbl_t *tbl = try_open_table(lock_target(relid),
                                 relation_lock_mode(lockmode), &blocked);

  /* What this session caches of it is from the version now held, or goes. */
  if (tbl != NULL)
    RelationCheckSmVersion(relid, lock_target(relid), sm_tbl_def(tbl)->version);
  return !blocked;
}

/*
 * Whether this transaction holds sm's lock on the relation relid in a mode
 * covering what lockmode takes (smstore_lock_relation()): on the table, or
 * for an index, on its table.  A sequence takes no lock in sm, so has none
 * to hold, and is held.
 */
bool smstore_relation_held(Oid relid, LOCKMODE lockmode) {
  return held_table(lock_target(relid), relation_lock_mode(lockmode)) != NULL ||
         smstore_sequence_exists(relid);
}

/*
 * TRUNCATE rel, a table in sm: its rows and its indexes' entries go, in
 * this transaction, which a rollback undoes.  The table is opened
 * exclusively, so the truncate waits for every other session's use of it.
 * The caller has checked this session has no scan of it open.
 */
void smstore_truncate(Relation rel) {
  Oid oid = RelationGetRelid(rel);
  sm_tbl_t *tbl = open_table(oid, SM_LOCKMODE_EX);

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  if (sm_tbl_truncate(tbl) != 0)
    sm_error("truncate a table");

  /*
   * The table's rows are in a new store now, which only the handle that
   * truncated it knows: close the transaction's other handles of it, so the
   * next use opens one afresh.
   */
  std::vector<open_table_t> &handles = open_tables()[oid];

  for (size_t i = 0; i < handles.size();) {
    if (handles[i].tbl != tbl) {
      sm_tbl_close(handles[i].tbl);
      handles.erase(handles.begin() + i);
    } else
      i++;
  }
}

/*
 * LOCK TABLE on rel, a table in sm: sm's table lock in the mode closest to
 * lockmode, held until the transaction ends.  With nowait, a lock another
 * transaction holds in a mode that conflicts fails at once, as PostgreSQL's.
 */
void smstore_lock_table(Relation rel, LOCKMODE lockmode, bool nowait) {
  sm_lockmode_t mode = table_lock_mode(lockmode);
  bool blocked = false;
  sm_tbl_t *tbl = nowait ? try_open_table(RelationGetRelid(rel), mode, &blocked)
                         : open_table(RelationGetRelid(rel), mode);

  if (blocked)
    ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                    errmsg("could not obtain lock on relation \"%s\"",
                           RelationGetRelationName(rel))));
  if (tbl == NULL)
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
 * Make the column an ALTER SEQUENCE ... OWNED BY names own the sequence, in
 * sm, in this transaction, or no column with OWNED BY NONE: the sequence is
 * then dropped with the table or the column.  Returns its address, or
 * InvalidObjectAddress if IF EXISTS found no sequence.
 */
ObjectAddress smstore_own_sequence(AlterSeqStmt *stmt, List *owned_by) {
  Oid oid = RangeVarGetRelid(stmt->sequence, NoLock, true);
  int nnames = list_length(owned_by);
  uint64_t table = 0, seq_nspoid;
  int32_t attnum = -1;
  std::string seq_name;
  ObjectAddress address;

  if (!OidIsValid(oid)) {
    if (!stmt->missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg("relation \"%s\" does not exist",
                             stmt->sequence->relname)));
    ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping",
                            stmt->sequence->relname)));
    return InvalidObjectAddress;
  }
  sm_session_t *s = xact_session();

  switch (sm_cat_sequence_name(s, oid, &seq_nspoid, seq_name)) {
  case 0:
    break;
  case 1:
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a sequence",
                           stmt->sequence->relname)));
    break;
  default:
    sm_error("look up a sequence");
  }
  if (nnames == 1) {
    if (strcmp(strVal(linitial(owned_by)), "none") != 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid OWNED BY option"),
                      errhint("Specify OWNED BY table.column or OWNED BY "
                              "NONE.")));
  } else {
    RangeVar *rv =
        makeRangeVarFromNameList(list_copy_head(owned_by, nnames - 1));
    Oid relid = RangeVarGetRelid(rv, NoLock, false);
    Relation rel;

    if (smstore_sequence_exists(relid))
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("sequence cannot be owned by relation \"%s\"",
                             rv->relname)));
    rel = relation_open(relid, NoLock);
    if (RelationGetNamespace(rel) != (Oid)seq_nspoid)
      ereport(ERROR,
              (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
               errmsg("sequence must be in same schema as table it is linked "
                      "to")));
    attnum = column_place(rel, strVal(llast(owned_by)));
    relation_close(rel, NoLock);
    table = relid;
  }
  if (waiting([&] { return sm_cat_own_sequence(s, oid, table, attnum); }) !=
      0) {
    if (sm_errnum() == SM_ERR_INUSE)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("cannot change ownership of identity sequence"),
                      errdetail("Sequence \"%s\" is linked to an identity "
                                "column.",
                                seq_name.c_str())));
    sm_error("give a sequence its owner");
  }
  ObjectAddressSet(address, RelationRelationId, oid);
  return address;
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
    /*
     * sm keys an interval in an order finer than PostgreSQL's: '1 mon' and
     * '30 days', which PostgreSQL calls equal, are two keys
     * (../sm/sm2/TYPE.md section 3.10), so = through the index would miss
     * one and UNIQUE would take both.  So an interval is never an index
     * key; it may be an INCLUDE column.
     */
    if (TupleDescAttr(RelationGetDescr(rel), key.attnum)->atttypid ==
        INTERVALOID)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index key of type interval is not "
                             "supported"),
                      errdetail("sm orders intervals PostgreSQL calls equal "
                                "as distinct keys.")));
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
                                   std::string(), {}, &oid, stmt->primary);
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
    switch (sm_cat_index_oid(session(), sm_nspoid(lfirst_oid(l)), rv->relname,
                             table, index)) {
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
    /*
     * Every session's descriptor of the table relearns its indexes, and
     * one of the index is gone, as PostgreSQL's index_drop() has it.
     */
    rel = relation_open((Oid)table, NoLock);
    CacheInvalidateRelcache(rel);
    relation_close(rel, NoLock);
    rel = relation_open((Oid)index, NoLock);
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
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), c);
    /* A key's datum as sm takes it: a numeric's integer at the scale. */
    auto bound = [&](ScanKey key, bool upper) {
      if (att->atttypid != NUMERICOID)
        return (sm_datum_t)key->sk_argument;
      return numeric_bound(key->sk_argument, att->atttypmod, upper,
                           (uint8 *)palloc(16));
    };
    bool numeric_col = att->atttypid == NUMERICOID;

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
      lo_vals[c] = bound(eq, false);
      hi_vals[c] = bound(eq, true);
      lo.n = hi.n = c + 1;
      /* A numeric's bounds may differ: no later column narrows them. */
      if (numeric_col && lo_vals[c] != hi_vals[c]) {
        lo.inclusive = hi.inclusive = true;
        break;
      }
      continue;
    }
    /* In the index's order a DESC column's high values come first. */
    ScanKey first = desc ? above : below, last = desc ? below : above;

    if (first != NULL) {
      lo_vals[c] = bound(first, first == above);
      lo.n = c + 1;
      lo.inclusive = numeric_col ||
                     first->sk_strategy == BTGreaterEqualStrategyNumber ||
                     first->sk_strategy == BTLessEqualStrategyNumber;
    }
    if (last != NULL) {
      hi_vals[c] = bound(last, last == above);
      hi.n = c + 1;
      hi.inclusive = numeric_col ||
                     last->sk_strategy == BTGreaterEqualStrategyNumber ||
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

/* The tuple of the row at rowno into slot, cleared, as a virtual tuple. */
static void tuple_into_slot(sm_tbl_t *tbl, uint64_t rowno, const void *tuple,
                            size_t len, TupleTableSlot *slot) {
  if (sm_tbl_deform(tbl, tuple, len, (sm_datum_t *)slot->tts_values,
                    slot->tts_isnull, slot->tts_tupleDescriptor->natts) != 0)
    sm_error("read a row");
  ExecStoreVirtualTuple(slot);
  slot_from_sm(slot);
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
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
  tuple_into_slot(tbl, rowno, tuple, len, slot);
  /*
   * A point read's bytes last only until the session's next call to sm,
   * which another scan in the statement makes, as a join's inner side
   * does: the slot keeps a copy.
   */
  ExecMaterializeSlot(slot);
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
    bool from_row = false;

    ExecClearTuple(slot);
    switch (sm_idx_scan_key(scan->icursor, (sm_datum_t *)slot->tts_values,
                            slot->tts_isnull, scan->form->indnkeyatts)) {
    case 0:
      entry_includes(scan, slot);
      break;
    case 1:
      if (!entry_from_row(scan, rowno, slot))
        continue;
      from_row = true;
      break;
    default:
      sm_error("read an index entry");
    }
    ExecStoreVirtualTuple(slot);
    slot_from_sm(slot);
    /* A row read by rowno lasts only until sm's next call, as read_row's. */
    if (from_row)
      ExecMaterializeSlot(slot);
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

/* rel, a table in sm, opened to write in this transaction. */
static sm_tbl_t *table_to_write(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IX);

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  return tbl;
}

/*
 * Keep the row at rowno of the table oid for the statement, with a copy of
 * its tuple, or with none yet if tuple is NULL.
 */
static void keep_row(Oid oid, uint64 rowno, const void *tuple, size_t len) {
  if (kept_tuple != NULL)
    pfree(kept_tuple);
  kept_oid = oid;
  kept_rowno = rowno;
  kept_tuple = NULL;
  kept_len = len;
  if (tuple != NULL) {
    /* Until the statement ends, or the transaction: its context's. */
    kept_tuple = (char *)MemoryContextAlloc(TopTransactionContext, Max(len, 1));
    memcpy(kept_tuple, tuple, len);
  }
}

/*
 * The value sm's TOAST pointer attr names, as a palloc'd varlena with a
 * 4-byte header, read from the store the pointer names.  It is read as the
 * statement sees the store: a pointer is to be fetched in the statement
 * that read it.
 */
struct varlena *smstore_fetch_toast(struct varlena *attr) {
  std::string value;
  struct varlena *result;

  if (sm_toast_fetch(xact_session(), (sm_datum_t)attr, &value) != 0)
    sm_error("read a value stored out of line");
  result = (struct varlena *)palloc(value.size() + VARHDRSZ);
  SET_VARSIZE(result, value.size() + VARHDRSZ);
  memcpy(VARDATA(result), value.data(), value.size());
  return result;
}

/*
 * The length of the value sm's TOAST pointer attr names, its varlena header
 * left out: the pointer's 4 bytes after its two, little-endian, as the
 * machine is (../sm/sm2/VARLENA.md section 2).
 */
Size smstore_toast_size(struct varlena *attr) {
  uint32 len;

  memcpy(&len, (const char *)attr + 2, sizeof(len));
  return len;
}

/*
 * The row of rel at tid into slot, as a virtual tuple: the kept one if it
 * is that row, else as the statement sees it; false if it sees none.  The
 * executor fetches the row it is to update, the newest version of one it
 * locked, and the row it deleted, for RETURNING.
 */
bool smstore_fetch_row(Relation rel, ItemPointer tid, TupleTableSlot *slot) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  uint64 rowno = smstore_tid_to_rowno(tid);

  if (tbl == NULL)
    return false;
  if (kept_oid == RelationGetRelid(rel) && kept_rowno == rowno &&
      kept_tuple != NULL) {
    ExecClearTuple(slot);
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len, slot);
    return true;
  }
  return read_row(tbl, rowno, slot);
}

/*
 * What an update or a delete of the row at rowno of rel came to, rc from
 * sm, as the executor is told it.  A commit since the statement's snapshot
 * changed the row, under READ COMMITTED: TM_Updated, and the row is kept
 * for smstore_lock_latest() to read; sm holds its lock.  The statement has
 * already changed the row: TM_SelfModified, by this command, so the
 * executor passes over it.  At the other levels sm has aborted the
 * transaction instead, which is an error here, as any other failure is.
 */
static TM_Result write_result(Relation rel, uint64 rowno, int rc, CommandId cid,
                              TM_FailureData *tmfd, const char *what) {
  if (rc == 0)
    return TM_Ok;
  tmfd->xmax = InvalidTransactionId;
  smstore_rowno_to_tid(rowno, &tmfd->ctid);
  switch (sm_errnum()) {
  case SM_ERR_UPDATED:
    keep_row(RelationGetRelid(rel), rowno, NULL, 0);
    return TM_Updated;
  case SM_ERR_SELFMODIFIED:
    tmfd->cmax = cid;
    return TM_SelfModified;
  case SM_ERR_NOTFOUND:
    return TM_Deleted;
  case SM_ERR_DUPKEY:
    ereport(ERROR,
            (errcode(ERRCODE_UNIQUE_VIOLATION),
             errmsg("duplicate key value violates a unique index on \"%s\"",
                    RelationGetRelationName(rel)),
             errdetail("%s", sm_errmsg())));
  default:
    sm_error(what);
  }
  return TM_Ok; /* not reached */
}

/*
 * Replace the row of rel at tid with the slot's: sm writes every index of
 * the table with it, and the row keeps its rowno, so its tid.
 */
TM_Result smstore_update(Relation rel, ItemPointer tid, TupleTableSlot *slot,
                         CommandId cid, TM_FailureData *tmfd) {
  sm_tbl_t *tbl = table_to_write(rel);
  uint64 rowno = smstore_tid_to_rowno(tid);
  std::string old;
  bool have_old = false;

  slot_getallattrs(slot);
  /*
   * The row replaced, to tell its own pointers from others: the kept one if
   * it is that row, the newest version after a write found it changed, else
   * as the statement sees it.
   */
  if (has_toast(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull,
                slot->tts_nvalid)) {
    const void *tuple;
    size_t len;

    if (kept_oid == RelationGetRelid(rel) && kept_rowno == rowno &&
        kept_tuple != NULL) {
      old.assign(kept_tuple, kept_len);
      have_old = true;
    } else {
      switch (sm_tbl_read(tbl, rowno, &tuple, &len)) {
      case 0:
        /* sm's bytes last until its next call: a fetch is one. */
        old.assign((const char *)tuple, len);
        have_old = true;
        break;
      case 1:
        break;
      default:
        sm_error("read a row");
      }
    }
  }
  const sm_datum_t *row =
      row_to_sm(tbl, slot->tts_tupleDescriptor, slot->tts_values,
                slot->tts_isnull, slot->tts_nvalid, have_old ? &old : NULL);
  int rc = waiting([&] {
    return sm_tbl_update(tbl, rowno, row, slot->tts_isnull, slot->tts_nvalid);
  });

  if (rc == 0)
    slot->tts_tid = *tid;
  return write_result(rel, rowno, rc, cid, tmfd, "update a row");
}

/*
 * Delete the row of rel at tid, with its entry in every index.  The row is
 * kept as it was, for RETURNING to fetch.
 */
TM_Result smstore_delete(Relation rel, ItemPointer tid, CommandId cid,
                         TM_FailureData *tmfd) {
  sm_tbl_t *tbl = table_to_write(rel);
  uint64 rowno = smstore_tid_to_rowno(tid);
  std::string old;
  int rc = waiting([&] { return sm_tbl_delete(tbl, rowno, &old); });

  if (rc == 0)
    keep_row(RelationGetRelid(rel), rowno, old.data(), old.size());
  return write_result(rel, rowno, rc, cid, tmfd, "delete a row");
}

/*
 * Lock the row of rel at tid, and put its newest version into slot.
 *
 * After an update or a delete of the row gave TM_Updated, sm holds its lock
 * already, and this reads the version to check again (sm_tbl_read_latest);
 * the row is kept, so the executor's next fetch of it is this version.
 *
 * Else it is SELECT ... FOR UPDATE's or FOR SHARE's: the row is locked in
 * sm, shared or exclusive, to the end of the transaction, waiting for
 * another transaction that holds it or has written it -- or, with NOWAIT,
 * failing, and with SKIP LOCKED answering TM_WouldBlock.  Above READ
 * COMMITTED, sm fails the lock, and the transaction, if the row changed
 * since the snapshot, and what is read after is the snapshot's row.  At
 * READ COMMITTED the newest version is read, and tmfd says whether it is
 * another than the statement saw, for the executor to check its WHERE
 * again.  The slot owns what it holds, since the
 * executor keeps it past sm's next call.  TM_Deleted if the row is gone.
 */
TM_Result smstore_lock_row(Relation rel, ItemPointer tid, TupleTableSlot *slot,
                           LockTupleMode mode, LockWaitPolicy wait_policy,
                           TM_FailureData *tmfd) {
  Oid oid = RelationGetRelid(rel);
  uint64 rowno = smstore_tid_to_rowno(tid);
  bool share = mode == LockTupleShare || mode == LockTupleKeyShare;
  bool after_write =
      kept_oid == oid && kept_rowno == rowno && kept_tuple == NULL;
  sm_tbl_t *tbl =
      share ? open_table(oid, SM_LOCKMODE_IS) : table_to_write(rel);
  const void *tuple;
  size_t len;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  if (!after_write) {
    sm_lockmode_t smmode = share ? SM_LOCKMODE_SH : SM_LOCKMODE_EX;
    int rc;

    /*
     * Held to the end of the transaction, as a write's is.  NOWAIT and SKIP
     * LOCKED ask for it without waiting, and are told when it is another's.
     */
    if (wait_policy == LockWaitBlock)
      rc = waiting([&] { return sm_tbl_lock(tbl, rowno, smmode); });
    else
      rc = sm_tbl_trylock(tbl, rowno, smmode);
    if (rc < 0)
      sm_error("lock a row");
    if (rc == 1) {
      if (wait_policy == LockWaitSkip)
        return TM_WouldBlock;
      ereport(ERROR,
              (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
               errmsg("could not obtain lock on row in relation \"%s\"",
                      RelationGetRelationName(rel))));
    }
  }
  ExecClearTuple(slot);
  /*
   * Above READ COMMITTED the lock has just failed on any version newer than
   * the snapshot, so the snapshot's is the newest; and sm reads a newest
   * version, as such, only at READ COMMITTED.
   */
  if (IsolationUsesXactSnapshot()) {
    switch (sm_tbl_read(tbl, rowno, &tuple, &len)) {
    case 0:
      break;
    case 1:
      return TM_Deleted;
    default:
      sm_error("read a row");
    }
    tmfd->traversed = false;
    tuple_into_slot(tbl, rowno, tuple, len, slot);
    /* sm's bytes last until its next call. */
    ExecMaterializeSlot(slot);
    return TM_Ok;
  }
  switch (sm_tbl_read_latest(tbl, rowno, &tuple, &len)) {
  case 0:
    break;
  case 1:
    if (after_write)
      kept_oid = InvalidOid;
    return TM_Deleted;
  default:
    sm_error("read a row's newest version");
  }
  if (after_write) {
    keep_row(oid, rowno, tuple, len);
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len, slot);
    tmfd->traversed = true;
    return TM_Ok;
  }
  /* sm's bytes last until its next call: the read below is one. */
  std::string latest((const char *)tuple, len);

  switch (sm_tbl_read(tbl, rowno, &tuple, &len)) {
  case 0:
    tmfd->traversed = len != latest.size() ||
                      memcmp(tuple, latest.data(), len) != 0;
    break;
  case 1:
    tmfd->traversed = true;
    break;
  default:
    sm_error("read a row");
  }
  tuple_into_slot(tbl, rowno, latest.data(), latest.size(), slot);
  /* Copied out of latest, which ends with this call. */
  ExecMaterializeSlot(slot);
  return TM_Ok;
}
