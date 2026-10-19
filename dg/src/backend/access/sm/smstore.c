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
#include "access/reloptions.h"
#include "catalog/dependency.h"
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
#include "nodes/nodeFuncs.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lockdefs.h"
#include "storage/proc.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_constraint_d.h"
#include "commands/trigger.h"
#include "parser/parse_coerce.h"
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
#include "utils/timeout.h"
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
/* An index CREATE INDEX made: its table, its oid, its name, whether unique. */
struct made_index_t {
  uint64_t table;
  uint64_t index;
  std::string name;
  bool unique;
  std::vector<int32_t> keys; /* its key columns' 0-based places */
};

/* A view this transaction locked (lock_view()): in mode, at level. */
struct held_view_t {
  Oid oid;
  sm_lockmode_t mode;
  int level;
};

struct smstore_state_t {
  open_tables_t open_tables;
  /* The views it locked, as a table's lock is its handle's (open_tables). */
  std::vector<held_view_t> held_views;
  /* sm's namespaces' epoch when this session last flushed them. */
  uint64_t nsp_epoch = 0;
  /* The indexes this transaction made, and those its commit is to wait on. */
  std::vector<made_index_t> made_indexes;
  std::vector<made_index_t> built_after_commit;
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
  case SM_ERR_DOOMED:
    return ERRCODE_IN_FAILED_SQL_TRANSACTION;
  default:
    return sm_retryable() == 1 ? ERRCODE_T_R_SERIALIZATION_FAILURE
                               : ERRCODE_INTERNAL_ERROR;
  }
}

/* Raise the error sm left, saying what was being done. */
static void sm_error(const char *what) {
  /* PostgreSQL's words for a deadlock, which clients and tests look for. */
  if (sm_errnum() == SM_ERR_DEADLOCK)
    ereport(ERROR,
            (errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
             errmsg("deadlock detected"),
             errdetail("This transaction was chosen to end a cycle of waits "
                       "while it tried to %s.",
                       what)));
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
  /*
   * lock_timeout ends a wait for a lock that lasts longer, by a cancel the
   * timer raises (postinit.c).  It counts from the call, as sm does not say
   * when the call begins to wait; a call that ends in time disarms it, and
   * a cancel it raised meanwhile is never taken.
   */
  if (LockTimeout > 0)
    enable_timeout_after(LOCK_TIMEOUT, LockTimeout);
  for (;;) {
    int rc = f();

    /* Only -1 is an error: 1, a lock not had at once, is an answer. */
    if (rc != -1 || sm_errnum() != SM_ERR_INTERRUPTED) {
      if (LockTimeout > 0)
        disable_timeout(LOCK_TIMEOUT, false);
      return rc;
    }
    CHECK_FOR_INTERRUPTS();
  }
}

/*
 * Close the tables the aborting subtransaction opened: sm's rollback undid
 * what they did, a truncate's new store among it, and released their locks,
 * so a handle of them is stale.  The next use opens one afresh.  A cursor's
 * scans began with its plan, in the cursor's level, so a handle they read by
 * is not closed here.
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
  /* A view's lock goes with the subtransaction's, as a table's does. */
  std::erase_if(tcb->smstore->held_views,
                [level](const held_view_t &h) { return h.level >= level; });
}

/* Close the tables this transaction opened: a handle is good for it only. */
static void close_tables(void) {
  for (auto &[oid, handles] : open_tables())
    for (open_table_t &ot : handles)
      sm_tbl_close(ot.tbl);
  open_tables().clear();
  tcb->smstore->held_views.clear();
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
  std::vector<made_index_t> made;

  if (!sm_xact_open)
    return;
  close_tables();
  sm_xact_open = false;
  made.swap(tcb->smstore->made_indexes);
  if (sm_commit(session()) == 0) {
    /* Their builds begin now: smstore_wait_indexes() waits for them. */
    for (made_index_t &m : made)
      tcb->smstore->built_after_commit.push_back(std::move(m));
  } else {
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
  /* The indexes it made are gone with it. */
  tcb->smstore->made_indexes.clear();
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
  for (held_view_t &h : tcb->smstore->held_views)
    if (h.level >= level)
      h.level = level - 1;
}

static sm_tbl_t *held_table(Oid oid, sm_lockmode_t mode);

/*
 * End the bulk load a failed statement left open, raising no error: sm
 * rolls back past a closed load, emptying its table, and refuses to while
 * one is open.  A load whose end fails is ended too, its table empty.
 */
static void end_load_quietly(void) {
  sm_tbl_t *tbl;

  if (load_oid == InvalidOid)
    return;
  tbl = held_table(load_oid, SM_LOCKMODE_IX);
  load_oid = InvalidOid;
  if (tbl != NULL)
    (void)sm_tbl_load_end(tbl);
}

/* The subtransaction aborts: its writes are undone, and its savepoint goes. */
void smstore_subabort(void) {
  int depth = subxact_savepoint();

  end_load_quietly();
  /*
   * A rollback that fails, past the undo list's cap, dooms sm's
   * transaction: every later call on it fails with SM_ERR_DOOMED, the
   * commit too, until it is aborted.  In an abort there is nowhere for an
   * error to go, so it is logged as a warning, which does not throw.
   */
  if (depth > 0) {
    if (sm_rollback(session(), depth) != 0)
      ereport(WARNING,
              (errmsg("could not roll back to a savepoint: %s", sm_errmsg())));
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
 * Advance sm's statement to now, keeping its snapshot, as
 * CommandCounterIncrement() advances PostgreSQL's command id: what the
 * statement made in sm, a table and its rowno sequence, it may now write
 * (sm_stmt_advance).
 */
void smstore_statement_advance(void) {
  if (sm_stmt_advance(xact_session()) != 0)
    sm_error("advance a statement");
}

/*
 * A numeric column with no precision is numeric(17, 3), the widest that is
 * sm's decimal64 (DECIMAL.md section 2.3, sm/SM_DECIMAL_PLAN.md).
 */
#define NUMERIC_DEFAULT_TYPMOD (((17 << 16) | 3) + VARHDRSZ)

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

/*
 * The bits of sm's decimal datum below its value that hold its scale: 5
 * for decimal64, a numeric column of up to 17 digits, 6 for decimal128
 * (sm/sm2/TYPE.md section 3.4).
 */
static int numeric_scale_bits(int32 typmod) {
  return numeric_precision(typmod) <= 17 ? 5 : 6;
}

/*
 * sm's datum for i, an integer at the scale of a column of typmod: i
 * shifted past the column's scale, by value for decimal64, for decimal128
 * 16 bytes in buf.
 */
static sm_datum_t numeric_pack(__int128 i, int32 typmod, uint8 *buf) {
  int bits = numeric_scale_bits(typmod);
  __int128 d = i * ((__int128)1 << bits) + numeric_scale_of(typmod);

  if (bits == 5)
    return (sm_datum_t)(int64)d;
  memcpy(buf, &d, 16);
  return (sm_datum_t)buf;
}

/* Whether typid is one of the decimals (DG_DECIMAL_PLAN.md). */
static bool is_decimal(Oid typid) {
  return typid == DECIMAL64OID || typid == DECIMAL128OID;
}

/*
 * sm's datum for num, of the decimal typid, in a column of typmod.  The
 * value is rounded to the column's scale and checked against its
 * precision, as an assignment is; then a decimal64's word, and a
 * decimal128's in a column of decimal128, are sm's datum as they are, and
 * a decimal128's in a column of decimal64 is packed (numeric_pack()).
 */
static sm_datum_t numeric_to_sm(Datum num, Oid typid, int32 typmod,
                                uint8 *buf) {
  int32 t = typmod < (int32)VARHDRSZ ? NUMERIC_DEFAULT_TYPMOD : typmod;
  Numeric v;

  if (typid == DECIMAL64OID)
    return (sm_datum_t)DirectFunctionCall2(decimal64, num, Int32GetDatum(t));
  v = DatumGetNumeric(DirectFunctionCall2(numeric, num, Int32GetDatum(t)));
  if (numeric_scale_bits(typmod) == NUMERIC_SCALE_BITS)
    return (sm_datum_t)v;
  return numeric_pack(decimal_value(v), typmod, buf);
}

/*
 * The datum of the decimal typid of sm's datum d, of a column of typmod:
 * d itself for a decimal64, and for a decimal128 of a column of
 * decimal128, pointing into sm's tuple; a decimal128 palloc'd from a
 * column of decimal64.
 */
static Datum numeric_from_sm(sm_datum_t d, Oid typid, int32 typmod) {
  Numeric res;
  int64 w;

  if (typid == DECIMAL64OID)
    return (Datum)d;
  if (numeric_scale_bits(typmod) == NUMERIC_SCALE_BITS)
    return PointerGetDatum((const void *)d);
  w = (int64)d;
  res = (Numeric)palloc(sizeof(decimal_t));
  decimal_set(res, w >> 5, (int)(w & 31));
  return NumericGetDatum(res);
}

/*
 * sm's datum for a scan's bound num, of the decimal typid, on a decimal
 * column of typmod: the column's integer at or below it, or with upper at
 * or above it, kept within the column's precision.  So the bound takes in
 * every value the key's own would, and the keys, checked again on each
 * row, decide.
 */
static sm_datum_t numeric_bound(Datum num, Oid typid, int32 typmod,
                                bool upper, uint8 *buf) {
  int s = numeric_scale_of(typmod), vs;
  int p = numeric_precision(typmod);
  __int128 i, limit = 1;
  bool clamped = false;

  if (typid == DECIMAL64OID) {
    i = DatumGetInt64(num) >> 5;
    vs = (int)(DatumGetInt64(num) & 31);
  } else {
    i = decimal_value(DatumGetNumeric(num));
    vs = decimal_scale(DatumGetNumeric(num));
  }

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
  return numeric_pack(i, typmod, buf);
}

/*
 * Whether attribute att is a decimal128 kept in sm as a decimal64, whose
 * datums are converted: of an expression's column, made without a typmod.
 */
static bool converted_decimal(Form_pg_attribute att) {
  return att->atttypid == DECIMAL128OID && !att->attisdropped &&
         numeric_scale_bits(att->atttypmod) != NUMERIC_SCALE_BITS;
}

/* Whether a row of desc has a decimal whose datums are converted. */
static bool has_numeric(TupleDesc desc) {
  for (int i = 0; i < desc->natts; i++)
    if (converted_decimal(TupleDescAttr(desc, i)))
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
    if (is_decimal(att->atttypid))
      val = numeric_to_sm(values[i], att->atttypid, att->atttypmod,
                          (uint8 *)palloc(16));
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
 * The decimal128s of a row of desc in slot that sm keeps as decimal64s
 * (has_numeric()), made decimal128s, and the slot made to own its values.
 * Every other decimal is sm's datum as it is.
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
    if (converted_decimal(TupleDescAttr(desc, i)) && !slot->tts_isnull[i])
      slot->tts_values[i] = numeric_from_sm((sm_datum_t)slot->tts_values[i],
                                            DECIMAL128OID,
                                            TupleDescAttr(desc, i)->atttypmod);
  MemoryContextSwitchTo(old);
  ExecMaterializeSlot(slot);
}

/*
 * sm's type code for a column of type typid and typmod (sm2/TYPE.md section
 * 3): a decimal is sm's decimal64 up to 17 digits, decimal128 up to 36, and
 * one with no typmod numeric(17, 3)'s.
 */
static uint8_t type_code(Oid typid, int32 typmod) {
  switch (typid) {
  case DECIMAL64OID:
  case DECIMAL128OID:
    return numeric_precision(typmod) <= 17 ? 17 : 18;
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
  /* char(n): stored padded, as given; sm compares it without the padding. */
  case BPCHAROID:
    return 19;
  /* name: its 64 bytes, NUL-padded, as PostgreSQL stores them. */
  case NAMEOID:
    return 20;
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
  r->constraint = x.constraint;
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

/*
 * Add to *out the view oid, if there is one: its columns those of its
 * query's select list, as the relation cache makes them
 * (RelationBuildSmViewDesc()), and its query.  Not locked, as a listing is
 * not.
 */
static void add_view(sm_session_t *s, uint64_t oid, List **out) {
  sm_view_def_t def;
  Query *query;
  smstore_relation_t *r;
  int natts = 0, i = 0;

  bool temp;

  if (sm_cat_view_def(s, oid, def) != 0)
    return;
  query = linitial_node(Query, (List *)stringToNode(def.query.c_str()));
  foreach_node(TargetEntry, tle, query->targetList) {
    if (!tle->resjunk)
      natts++;
  }
  temp = def.nspoid == SM_NSP_TEMP;
  r = add_relation(out, oid, def.name,
                   temp ? PG_TEMP_NAMESPACE : (Oid)def.nspoid, RELKIND_VIEW,
                   natts, false, temp);
  foreach_node(TargetEntry, tle, query->targetList) {
    if (tle->resjunk)
      continue;
    r->attnames[i] = pstrdup(tle->resname);
    r->typids[i] = exprType((Node *)tle->expr);
    r->typmods[i] = exprTypmod((Node *)tle->expr);
    i++;
  }
  r->query = pstrdup(def.query.c_str());
  r->options = def.options.empty() ? NULL : pstrdup(def.options.c_str());
}

/* Add to *out the views of sm's namespace nspoid. */
static void add_views(sm_session_t *s, uint64_t nspoid, List **out) {
  std::vector<std::string> names;

  if (sm_cat_list_views(s, nspoid, names) != 0)
    sm_error("list a schema's views");
  for (const std::string &name : names) {
    uint64_t oid;

    if (sm_cat_view_oid(s, nspoid, name.c_str(), &oid) == 0)
      add_view(s, oid, out);
  }
}

/* Add to *out the table def, with its columns, and its indexes. */
static void add_table(const sm_table_def_t &def, List **out) {
  bool temp = def.nspoid == SM_NSP_TEMP;
  Oid pgnsp = temp ? PG_TEMP_NAMESPACE : (Oid)def.nspoid;
  smstore_relation_t *r =
      add_relation(out, def.oid, def.name, pgnsp, RELKIND_RELATION,
                   (int)def.columns.size(), !def.indexes.empty(), temp);

  r->nfkeys = (int)def.fkeys.size();
  r->referenced = !def.referenced_by.empty();
  for (size_t i = 0; i < def.columns.size(); i++) {
    const sm_column_def_t &c = def.columns[i];

    set_column(r, (int)i, c);
    if (!c.dropped && c.has_default)
      r->defaults[i] = pstrdup(c.default_expr.c_str());
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
 * relation oid, a table, an index, a view or a sequence: it and, for a
 * table or an index, the table and all its indexes.  Every relation if oid is
 * an index whose table this thread has not met; none if there is no such
 * relation. For a catalog query on one relation, which needs no other's rows.
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
  add_view(s, oid, &out);
  if (out != NIL)
    return out;
  /* An index of a table not met: it may be any table's. */
  return OidIsValid(table) ? NIL : smstore_relations();
}

/*
 * Every relation in sm, palloc'd smstore_relation_t, as this transaction
 * sees them: each schema's tables, their indexes, its views and its
 * sequences, and the session's temporary tables and views, in pg_temp.
 * pg_class's rows (catalog/catrel.c).
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
    add_views(s, nspoid, &out);
    if (sm_cat_list_sequences(s, nspoid, seqs) != 0)
      sm_error("list a schema's sequences");
    for (const std::string &seq : seqs)
      if (sm_cat_sequence_oid(s, nspoid, seq.c_str(), &oid) == 0)
        add_sequence(s, oid, seq.c_str(), (Oid)nspoid, &out);
  }
  add_tables(s, SM_NSP_TEMP, &out);
  add_views(s, SM_NSP_TEMP, &out);
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
 * The oid of the relation named name in nspoid, a table, an index, a view
 * or a sequence, or InvalidOid if there is none.
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
  switch (sm_cat_view_oid(session(), sm_nspoid(nspoid), name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    break;
  default:
    sm_error("look up a view");
  }
  /* A sequence is never temporary yet. */
  return nspoid == PG_TEMP_NAMESPACE ? InvalidOid : sequence_oid(name, nspoid);
}

static void drop_table(RangeVar *rv, bool missing_ok, bool cascade,
                       const std::vector<Oid> &dropping);
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

/* The views in the schema nspoid, sorted by name, as for its tables. */
static std::vector<std::string> namespace_views(Oid nspoid) {
  std::vector<std::string> names;

  if (sm_cat_list_views(xact_session(), nspoid, names) != 0)
    sm_error("list a schema's views");
  return names;
}

static void drop_readers(Oid oid, int32_t attnum, const std::string &what,
                         bool cascade, const std::vector<Oid> &dropping,
                         std::vector<Oid> *dropped);
static void drop_view_oid(Oid oid);

/*
 * Drop the schema nspname, nspoid, from sm, in this transaction.  With
 * cascade its views go first, with the views elsewhere that read them, then
 * its tables, each with its indexes and the sequences its columns own, then
 * its other sequences; without, a schema that holds any fails.
 */
static void drop_namespace(const char *nspname, Oid nspoid, bool cascade) {
  if (cascade) {
    std::vector<std::string> views = namespace_views(nspoid);
    std::vector<std::string> tables = namespace_tables(nspoid);
    std::vector<std::string> sequences;
    std::vector<Oid> view_oids, dropped;
    int n;
    StringInfoData detail;

    for (const std::string &v : views)
      view_oids.push_back(smstore_relation_oid(v.c_str(), nspoid));
    for (size_t i = 0; i < views.size(); i++) {
      Oid v = view_oids[i];

      /* Dropped already, as a reader of another of the schema's views. */
      if (std::find(dropped.begin(), dropped.end(), v) != dropped.end())
        continue;
      drop_readers(v, -1, "view " + views[i], true, view_oids, &dropped);
      drop_view_oid(v);
      dropped.push_back(v);
    }
    for (const std::string &t : tables)
      drop_table(makeRangeVar(pstrdup(nspname), pstrdup(t.c_str()), -1),
                 false, true, {});
    /*
     * The sequences left: an owned one went with its table, unnamed.  The
     * list is read in a new statement, which sees the drops.
     */
    CommandCounterIncrement();
    sequences = namespace_sequences(nspoid);
    n = (int)(views.size() + tables.size() + sequences.size());

    initStringInfo(&detail);
    for (const std::string &v : views)
      appendStringInfo(&detail, "%sdrop cascades to view %s.%s",
                       detail.len > 0 ? "\n" : "", nspname, v.c_str());
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
 * statement of its own, a DEFAULT, which it has made the column's
 * raw_default, and a REFERENCES, which it has made an ALTER TABLE ... ADD
 * FOREIGN KEY to follow.
 */
static bool only_taken(List *constraints) {
  ListCell *l;

  foreach (l, constraints) {
    ConstrType type = lfirst_node(Constraint, l)->contype;

    if (type != CONSTR_NOTNULL && type != CONSTR_PRIMARY &&
        type != CONSTR_UNIQUE && type != CONSTR_DEFAULT &&
        type != CONSTR_IDENTITY && type != CONSTR_FOREIGN)
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

/*
 * Check a CREATE TABLE's WITH (...) and ignore it: sm has no pages to
 * fill and no autovacuum, so fillfactor and autovacuum_enabled are taken,
 * their values checked as PostgreSQL checks them, and kept nowhere.  Any
 * other storage parameter is refused.
 */
static void check_storage_options(List *options) {
  List *fill = NIL;
  ListCell *l;

  foreach (l, options) {
    DefElem *opt = lfirst_node(DefElem, l);
    bool on;

    if (opt->defnamespace == NULL && strcmp(opt->defname, "fillfactor") == 0)
      fill = lappend(fill, opt);
    else if (opt->defnamespace == NULL &&
             strcmp(opt->defname, "autovacuum_enabled") == 0) {
      /* A bare name means true, as transformRelOptions() takes it. */
      const char *value = opt->arg != NULL ? defGetString(opt) : "true";

      if (!parse_bool(value, &on))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid value for boolean option \"%s\": %s",
                               opt->defname, value)));
    } else
      ereport(ERROR,
              (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
               errmsg("storage parameter \"%s%s%s\" is not supported",
                      opt->defnamespace != NULL ? opt->defnamespace : "",
                      opt->defnamespace != NULL ? "." : "", opt->defname),
               errdetail("CREATE TABLE takes only fillfactor and "
                         "autovacuum_enabled, and ignores them.")));
  }
  /* PostgreSQL's own check of fillfactor: its range, and given once. */
  if (fill != NIL)
    (void)heap_reloptions(
        RELKIND_RELATION,
        transformRelOptions((Datum)0, fill, NULL, NULL, false, false), true);
}

/* Refuse what a CREATE TABLE asks that sm's tables do not have yet. */
static void check_create_table(const CreateStmt *stmt) {
  if (stmt->inhRelations != NIL || stmt->partbound != NULL ||
      stmt->partspec != NULL || stmt->ofTypename != NULL ||
      stmt->constraints != NIL || stmt->tablespacename != NULL ||
      stmt->accessMethod != NULL ||
      stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE TABLE supports only columns, NOT NULL, "
                           "PRIMARY KEY, UNIQUE and TEMPORARY yet")));
  check_storage_options(stmt->options);
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
    if (is_decimal(typid) && typmod < (int32)VARHDRSZ)
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
    return DECIMAL64OID;
  case 18:
    return DECIMAL128OID;
  case 19:
    return BPCHAROID;
  case 20:
    return NAMEOID;
  default:
    elog(ERROR, "sm type code %d has no type here", code);
  }
  return InvalidOid; /* not reached */
}

static void fkey_from_sm(const sm_fkey_def_t &def, uint64_t table,
                         smstore_fkey_t *fk);

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
    col->default_expr = c.has_default ? pstrdup(c.default_expr.c_str()) : NULL;
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
  /* Its foreign keys, and those that reference it, whole: their triggers. */
  desc->nfkeys = (int)def->fkeys.size();
  desc->fkeys =
      (smstore_fkey_t *)palloc0(Max(desc->nfkeys, 1) * sizeof(smstore_fkey_t));
  for (int i = 0; i < desc->nfkeys; i++)
    fkey_from_sm(def->fkeys[i], oid, &desc->fkeys[i]);
  desc->nreferenced = 0;
  desc->referenced = (smstore_fkey_t *)palloc0(
      Max(def->referenced_by.size(), 1) * sizeof(smstore_fkey_t));
  for (const sm_fkey_ref_t &r : def->referenced_by)
    if (smstore_get_fkey((Oid)r.fkey, &desc->referenced[desc->nreferenced]))
      desc->nreferenced++;
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
 * smstore_relation_name of relid, which is no table: false if it is no
 * view either.
 */
static bool view_name(sm_session_t *s, Oid relid, uint64 version, char **name,
                      Oid *nspoid) {
  sm_view_def_t def;

  switch (sm_cat_view_def(s, relid, def)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a view's definition");
  }
  if (def.version == version && def.version != SM_VERSION_OWN)
    return true;
  *name = pstrdup(def.name.c_str());
  *nspoid = def.nspoid == SM_NSP_TEMP ? PG_TEMP_NAMESPACE : (Oid)def.nspoid;
  return true;
}

/*
 * Whether sm still has relid, a table, one of its indexes or a view, whose
 * cached entry was built at version, as this transaction sees it, with no
 * lock: for a caller that reads the entry without one
 * (RelationGetClassForm()), which RelationCheckSmVersion() never checks.  If
 * the entry is not current, the relation's name and schema now, palloc'd,
 * into *name and *nspoid; else *name is NULL.  false if relid is gone.
 */
bool smstore_relation_name(Oid relid, uint64 version, char **name,
                           Oid *nspoid) {
  sm_session_t *s = xact_session();
  Oid table = smstore_index_table(relid);
  uint64_t now;
  sm_table_def_t def;

  if (!OidIsValid(table))
    table = relid;
  *name = NULL;
  /* The version alone, which copies nothing, decides the common case. */
  switch (sm_cat_table_version_oid(s, table, &now)) {
  case 0:
    break;
  case 1:
    return view_name(s, relid, version, name, nspoid);
  default:
    sm_error("read a table's version");
  }
  /* One the transaction has changed may have changed again since. */
  if (now == version && now != SM_VERSION_OWN)
    return true;
  switch (sm_cat_table_def_oid(s, table, def)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a table's definition");
  }
  *nspoid = def.nspoid == SM_NSP_TEMP ? PG_TEMP_NAMESPACE : (Oid)def.nspoid;
  if (table == relid) {
    *name = pstrdup(def.name.c_str());
    return true;
  }
  for (const sm_index_def_t &x : def.indexes)
    if (x.oid == relid) {
      *name = pstrdup(x.name.c_str());
      return true;
    }
  return false;
}

/*
 * A row's rowno as a tid, for the slot that carries it: block rowno / 256,
 * SMSTORE_ROWS_PER_BLOCK, and offset one above the remainder, as an offset
 * of 0 is no tid.  A sample scan reads a block of rownos as PostgreSQL's
 * reads a page (access/sm/smtable.c).
 */
void smstore_rowno_to_tid(uint64 rowno, ItemPointer tid) {
  if (!smstore_rowno_fits_tid(rowno, tid))
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("row number %llu does not fit a tid",
                           (unsigned long long)rowno)));
}

/*
 * rowno as a tid, as smstore_rowno_to_tid() makes it; false if it does not
 * fit one, so that no row has it. A TID scan of WHERE rowno = n asks.
 */
bool smstore_rowno_fits_tid(uint64 rowno, ItemPointer tid) {
  if (rowno / SMSTORE_ROWS_PER_BLOCK > MaxBlockNumber)
    return false;
  ItemPointerSet(tid, (BlockNumber)(rowno / SMSTORE_ROWS_PER_BLOCK),
                 (OffsetNumber)(rowno % SMSTORE_ROWS_PER_BLOCK + 1));
  return true;
}

/* The rowno a tid carries (smstore_rowno_to_tid()). */
uint64 smstore_tid_to_rowno(ItemPointer tid) {
  return (uint64)ItemPointerGetBlockNumber(tid) * SMSTORE_ROWS_PER_BLOCK +
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
  /* The table's oid, each row's tableoid, as WHERE CURRENT OF checks. */
  Oid relid;
  /* A table or index scan's: the columns it reads of each row, the first. */
  int natts;
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
  /*
   * smstore_scan_row()'s row read ahead, past the rowno it was asked for,
   * kept for a later call: valid until the cursor's next call.  at_end once
   * the cursor has none left.
   */
  bool ahead;
  bool at_end;
  uint64_t ahead_rowno;
  const void *ahead_tuple;
  size_t ahead_len;
};

/*
 * The tuple of the row at rowno into slot, cleared, as a virtual tuple, its
 * tid its rowno: its first natts columns, the only ones valid.  A value by
 * reference points into sm's tuple, and lasts as long as it does.
 */
static void tuple_into_slot(sm_tbl_t *tbl, uint64_t rowno, const void *tuple,
                            size_t len, int natts, TupleTableSlot *slot) {
  int all = slot->tts_tupleDescriptor->natts;

  if (sm_tbl_deform(tbl, tuple, len, (sm_datum_t *)slot->tts_values,
                    slot->tts_isnull, natts) != 0)
    sm_error("read a row");
  /* The columns left out are null to a materializing, which copies all. */
  memset(slot->tts_isnull + natts, true, all - natts);
  ExecStoreVirtualTuple(slot);
  /*
   * Only they are valid.  The executor reads a virtual slot's values with
   * no deform, so a read past them, of a natts too few, trips an assertion
   * or reads null.
   */
  slot->tts_nvalid = natts;
  slot_from_sm(slot);
  smstore_rowno_to_tid(rowno, &slot->tts_tid);
}

/*
 * The columns of rel from the first up to the last of attrs, attribute
 * numbers offset as pull_varattnos() gives them; every one if attrs has the
 * whole row's.
 */
static int attrs_natts(const Bitmapset *attrs, Relation rel) {
  int last = 0;
  int x = -1;

  while ((x = bms_next_member(attrs, x)) >= 0) {
    AttrNumber attno = x + FirstLowInvalidHeapAttributeNumber;

    if (attno == InvalidAttrNumber)
      return RelationGetNumberOfAttributes(rel);
    last = Max(last, attno);
  }
  return last;
}

/*
 * The columns of each row of rel a scan of plan reads, from the first: up
 * to the last its target list and quals name, a bitmap heap scan's recheck
 * among them, or every one for a reference to the whole row.  Its
 * projection and quals fetch no more; a scan with no projection names every
 * column, as its target list is the table's.
 */
int smstore_scan_natts(const Scan *plan, Relation rel) {
  Bitmapset *attrs = NULL;

  pull_varattnos((Node *)plan->plan.targetlist, plan->scanrelid, &attrs);
  pull_varattnos((Node *)plan->plan.qual, plan->scanrelid, &attrs);
  if (IsA(plan, BitmapHeapScan))
    pull_varattnos((Node *)((const BitmapHeapScan *)plan)->bitmapqualorig,
                   plan->scanrelid, &attrs);
  return attrs_natts(attrs, rel);
}

/*
 * The columns of rel that expr reads of the relation varno, from the first:
 * up to the last it names, or every one for a reference to the whole row.
 */
int smstore_expr_natts(Node *expr, Index varno, Relation rel) {
  Bitmapset *attrs = NULL;

  pull_varattnos(expr, varno, &attrs);
  return attrs_natts(attrs, rel);
}

/*
 * Begin a scan of every row of rel, a table in sm, in rowno order, each
 * row's first natts columns read.
 */
smstore_scan_t *smstore_beginscan(Relation rel, int natts) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  scan->relid = RelationGetRelid(rel);
  scan->natts = natts;
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
  slot->tts_tableOid = scan->relid;
  switch (sm_rel_scannxt(scan->cursor, &rowno, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("scan a table");
  }
  tuple_into_slot(scan->tbl, rowno, tuple, len, scan->natts, slot);
  return true;
}

/*
 * Move the scan forward to rowno, as a sample scan does to the start of a
 * block of rows: a row read ahead at rowno or after is kept, as the cursor
 * is past it already, and only moves forward.
 */
void smstore_scan_seek(smstore_scan_t *scan, uint64 rowno) {
  if (scan->at_end || (scan->ahead && scan->ahead_rowno >= rowno))
    return;
  scan->ahead = false;
  if (sm_rel_scan_seek(scan->cursor, rowno) != 0)
    sm_error("move a table's scan");
}

/*
 * The row at rowno into slot, read forward from where the scan is; false if
 * the table has none there, as a row deleted, or a rowno never used, has
 * none.  A row read past rowno is kept for the next call, so rownos asked
 * for in increasing order read each row once at most.
 */
bool smstore_scan_row(smstore_scan_t *scan, uint64 rowno,
                      TupleTableSlot *slot) {
  ExecClearTuple(slot);
  slot->tts_tableOid = scan->relid;
  while (!scan->at_end && (!scan->ahead || scan->ahead_rowno < rowno)) {
    switch (sm_rel_scannxt(scan->cursor, &scan->ahead_rowno, &scan->ahead_tuple,
                           &scan->ahead_len)) {
    case 0:
      scan->ahead = true;
      break;
    case 1:
      scan->ahead = false;
      scan->at_end = true;
      break;
    default:
      sm_error("scan a table");
    }
  }
  if (!scan->ahead || scan->ahead_rowno != rowno)
    return false;
  scan->ahead = false;
  tuple_into_slot(scan->tbl, rowno, scan->ahead_tuple, scan->ahead_len,
                  scan->natts, slot);
  return true;
}

/* The highest rowno rel, a table in sm, has handed out; 0 if none. */
uint64 smstore_rowno_limit(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  uint64_t n;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  if (sm_tbl_rowno_limit(tbl, &n) != 0)
    sm_error("read a table's row numbers");
  return n;
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
static bool table_def_now(Oid oid, sm_table_def_t &out);
static bool read_row(sm_tbl_t *tbl, uint64_t rowno, int natts,
                     TupleTableSlot *slot);
static void forget_table(Oid relid);

/*
 * Drop the foreign keys of other tables that reference the table oid,
 * named relname, as DROP TABLE ... CASCADE does, with PostgreSQL's notice;
 * without cascade, PostgreSQL's error.  One of a table in dropping, which
 * the same statement drops, is dropped without a word.
 */
static void drop_referencing(Oid oid, const char *relname, bool cascade,
                             const std::vector<Oid> &dropping) {
  sm_table_def_t def;
  std::vector<std::pair<uint64_t, uint64_t>> drop; /* (table, fkey) */
  StringInfoData detail;
  int n = 0;

  if (!table_def_now(oid, def))
    return;
  initStringInfo(&detail);
  for (const sm_fkey_ref_t &r : def.referenced_by) {
    if (r.table == oid)
      continue;
    drop.push_back({r.table, r.fkey});
    if (std::find(dropping.begin(), dropping.end(), (Oid)r.table) !=
        dropping.end())
      continue;
    smstore_fkey_t fk;
    sm_table_def_t child;

    if (!smstore_get_fkey((Oid)r.fkey, &fk) ||
        !table_def_now((Oid)r.table, child))
      continue;
    /* A line each, as PostgreSQL's reportDependentObjects() writes them. */
    if (n++ > 0)
      appendStringInfoChar(&detail, '\n');
    appendStringInfo(&detail, "%sconstraint %s on table %s",
                     cascade ? "drop cascades to " : "", NameStr(fk.name),
                     child.name.c_str());
    if (!cascade)
      appendStringInfo(&detail, " depends on table %s", relname);
  }
  if (n > 0 && !cascade)
    ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                    errmsg("cannot drop table %s because other objects "
                           "depend on it",
                           relname),
                    errdetail("%s", detail.data),
                    errhint("Use DROP ... CASCADE to drop the dependent "
                            "objects too.")));
  if (n == 1)
    ereport(NOTICE, (errmsg("%s", detail.data)));
  else if (n > 1)
    ereport(NOTICE, (errmsg("drop cascades to %d other objects", n),
                     errdetail("%s", detail.data)));
  sm_session_t *s = xact_session();

  for (auto [table, fkey] : drop) {
    if (waiting([&] { return sm_cat_drop_fkey(s, table, fkey); }) != 0)
      sm_error("drop a foreign key");
    forget_table((Oid)table);
  }
}

static void drop_readers(Oid oid, int32_t attnum, const std::string &what,
                         bool cascade, const std::vector<Oid> &dropping,
                         std::vector<Oid> *dropped);

static void drop_table(RangeVar *rv, bool missing_ok, bool cascade,
                       const std::vector<Oid> &dropping) {
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
  drop_readers(oid, -1, std::string("table ") + rv->relname, cascade, {},
               nullptr);
  drop_referencing(oid, rv->relname, cascade, dropping);
  smstore_drop_table(oid);
}

/*
 * Drop the table oid from sm, in this transaction, with its indexes, rows
 * and sequence: DROP TABLE's, and ON COMMIT DROP's.
 */
void smstore_drop_table(Oid oid) {
  /* Locked first, so the descriptor read is the one sm holds. */
  Relation rel = relation_open(oid, AccessExclusiveLock);

  /* Every session forgets its descriptor once this one commits. */
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);
  /* Its ON COMMIT action, if it has one, goes with it. */
  remove_on_commit_action(oid);
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_table(s, oid); }) != 0)
    sm_error("drop a table");
}

static void drop_temp_views(void);

/*
 * Drop every temporary view and table of the session, in this transaction:
 * DISCARD TEMP's.  sm drops them itself when the session closes its
 * database.
 */
void smstore_drop_temp_tables(void) {
  std::vector<std::string> names;

  /* A view first, as it may read a table. */
  drop_temp_views();
  if (sm_cat_list_tables(xact_session(), SM_NSP_TEMP, names) != 0)
    sm_error("list the temporary tables");
  for (const std::string &name : names)
    smstore_drop_table(smstore_relation_oid(name.c_str(), PG_TEMP_NAMESPACE));
}

/*
 * Drop the tables a DROP TABLE names, from sm, in this transaction: each
 * with its indexes, rows and sequence.  A foreign key of another table that
 * references one is what depends on it: CASCADE drops it, RESTRICT is an
 * error, and one of a table the statement drops too is dropped with it.
 */
void smstore_drop_tables(DropStmt *stmt) {
  std::vector<Oid> dropping;
  ListCell *l;

  foreach (l, stmt->objects)
    dropping.push_back(RangeVarGetRelid(
        makeRangeVarFromNameList((List *)lfirst(l)), NoLock, true));
  foreach (l, stmt->objects)
    drop_table(makeRangeVarFromNameList((List *)lfirst(l)), stmt->missing_ok,
               stmt->behavior == DROP_CASCADE, dropping);
}

static bool view_held(Oid oid, sm_lockmode_t mode);
static bool lock_view(Oid oid, sm_lockmode_t mode, uint64_t *version);

/*
 * The view oid as sm keeps it, palloc'd in the current context, or NULL if
 * there is no such view.  It takes the view's lock IS, in this transaction.
 */
smstore_view_t *smstore_view_desc(Oid oid) {
  sm_view_def_t def;
  uint64_t version;
  smstore_view_t *desc;

  if (!view_held(oid, SM_LOCKMODE_IS) &&
      !lock_view(oid, SM_LOCKMODE_IS, &version))
    return NULL;
  switch (sm_cat_view_def(xact_session(), oid, def)) {
  case 0:
    break;
  case 1:
    return NULL;
  default:
    sm_error("read a view's definition");
  }
  desc = (smstore_view_t *)palloc(sizeof(smstore_view_t));
  desc->oid = oid;
  desc->nspoid =
      def.nspoid == SM_NSP_TEMP ? PG_TEMP_NAMESPACE : (Oid)def.nspoid;
  desc->name = pstrdup(def.name.c_str());
  desc->query = pstrdup(def.query.c_str());
  desc->options = def.options.empty() ? NULL : pstrdup(def.options.c_str());
  desc->version = def.version;
  return desc;
}

/*
 * What the view's query reads, as sm keeps it (../sm/VIEW.md section 3):
 * each relation of sm's it names, as recordDependencyOnExpr() finds them,
 * and of a table, each column by its place.  A view is read whole, and a
 * system column is the table's.
 */
static std::vector<sm_view_read_t> view_reads(sm_session_t *s, Query *query) {
  ObjectAddress *refs;
  int n = find_expr_references((Node *)query, NIL, &refs);
  std::vector<sm_view_read_t> out;

  for (int i = 0; i < n; i++) {
    const ObjectAddress &ref = refs[i];
    int32_t attnum = ref.objectSubId > 0 ? ref.objectSubId - 1 : -1;
    sm_view_def_t def;

    /* A compiled-in catalog is no relation of sm's, and is never dropped. */
    if (ref.classId != RelationRelationId || ref.objectId < FirstNormalObjectId)
      continue;
    if (attnum >= 0 && sm_cat_view_def(s, ref.objectId, def) == 0)
      attnum = -1;
    sm_view_read_t r = {ref.objectId, attnum};

    if (std::none_of(out.begin(), out.end(), [&](const sm_view_read_t &o) {
          return o.relation == r.relation && o.attnum == r.attnum;
        }))
      out.push_back(r);
  }
  return out;
}

/*
 * The options of a view, a text[] as pg_class's reloptions holds them, as
 * sm keeps them: untransformRelOptions()'s DefElem list, nodeToString()'s;
 * empty if there are none.
 */
static std::string view_options(Datum reloptions) {
  List *l = reloptions != (Datum)0 ? untransformRelOptions(reloptions) : NIL;

  return l != NIL ? nodeToString(l) : "";
}

/*
 * The text[] of options, a view's as sm keeps them (view_options()), as
 * pg_class's reloptions holds them; (Datum) 0 for none.
 */
Datum smstore_view_reloptions(const char *options) {
  if (options == NULL || options[0] == '\0')
    return (Datum)0;
  return transformRelOptions((Datum)0, (List *)stringToNode(options), NULL,
                             NULL, false, false);
}

/*
 * The view's record for query, with options, CREATE VIEW's DefElems: its
 * query, as pg_rewrite's ev_action, and its options, checked as PostgreSQL
 * checks a view's.
 */
static sm_view_def_t view_def(sm_session_t *s, Query *query, List *options) {
  Datum reloptions =
      transformRelOptions((Datum)0, options, NULL, NULL, false, false);
  sm_view_def_t def;

  (void)view_reloptions(reloptions, true);
  def.query = nodeToString(list_make1(query));
  def.options = view_options(reloptions);
  def.reads = view_reads(s, query);
  return def;
}

/* Raise the error sm left creating or replacing the view name. */
static void view_error(const char *name, const char *what) {
  if (sm_errnum() == SM_ERR_DUPKEY)
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" already exists", name)));
  if (sm_errnum() == SM_ERR_INVALID)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("view \"%s\" cannot be made: %s", name, sm_errmsg())));
  sm_error(what);
}

/*
 * Create the view rv names, its query query and its options options, in
 * sm, in this transaction.  A name a relation of its schema has is an
 * error.
 */
ObjectAddress smstore_create_view(RangeVar *rv, Query *query, List *options) {
  Oid nspoid = RangeVarGetCreationNamespace(rv);
  sm_session_t *s = xact_session();
  sm_view_def_t def = view_def(s, query, options);
  uint64_t oid;
  ObjectAddress address;

  if (waiting([&] {
        return sm_cat_create_view(s, sm_nspoid(nspoid), rv->relname, def, &oid);
      }) != 0)
    view_error(rv->relname, "create a view");
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}

static void forget_table(Oid relid);

/*
 * Replace the query and the options of the view oid, named name, with query
 * and options, in sm, in this transaction: CREATE OR REPLACE VIEW's, whose
 * columns the caller has checked against the view's.
 */
ObjectAddress smstore_replace_view(Oid oid, const char *name, Query *query,
                                   List *options) {
  sm_session_t *s = xact_session();
  sm_view_def_t def = view_def(s, query, options);
  ObjectAddress address;

  if (waiting([&] { return sm_cat_replace_view(s, oid, def); }) != 0)
    view_error(name, "replace a view");
  /* Every session builds its descriptor afresh, this one at its next use. */
  forget_table(oid);
  ObjectAddressSet(address, RelationRelationId, oid);
  return address;
}

/* A view that reads what is dropped, as drop_readers() finds it. */
struct reader_t {
  Oid oid;
  std::string name;
  std::string reads; /* what it reads, for the message: "table t" */
};

/*
 * The views that read the relation oid, named what, directly or through
 * other views, each once: into found in the order PostgreSQL reports them,
 * a view before those that read it, and into order in an order they can be
 * dropped in, a view after those that read it.  attnum, 0-based, takes
 * only the views that read that column of a table; -1 takes every one.
 * Another session's temporary view is named in pg_temp, the one temporary
 * schema digres has, where PostgreSQL names the session's own, pg_temp_1.
 */
static void find_readers(Oid oid, int32_t attnum, const std::string &what,
                         std::vector<reader_t> &found,
                         std::vector<Oid> &order) {
  sm_session_t *s = xact_session();
  std::vector<sm_view_reader_t> views;

  if (sm_cat_view_readers(s, oid, views) != 0)
    sm_error("find the views that read a relation");
  for (const sm_view_reader_t &v : views) {
    if (attnum >= 0 && std::none_of(v.reads.begin(), v.reads.end(),
                                    [&](const sm_view_read_t &r) {
                                      return r.relation == oid &&
                                             r.attnum == attnum;
                                    }))
      continue;
    if (std::any_of(found.begin(), found.end(),
                    [&](const reader_t &r) { return r.oid == (Oid)v.oid; }))
      continue;
    std::string name = v.other ? "pg_temp." + v.name : v.name;

    found.push_back({(Oid)v.oid, name, what});
    find_readers((Oid)v.oid, -1, "view " + name, found, order);
    order.push_back((Oid)v.oid);
  }
}

/* Drop the view oid from sm, in this transaction. */
static void drop_view_oid(Oid oid) {
  sm_session_t *s = xact_session();

  /* Every session forgets its descriptor once this one commits. */
  forget_table(oid);
  if (waiting([&] { return sm_cat_drop_view(s, oid); }) != 0)
    sm_error("drop a view");
}

/*
 * Drop the views that read the relation oid, what ("table t", "view v",
 * "column c of table t", with attnum its place), as DROP ... CASCADE does,
 * with PostgreSQL's notice; without cascade, PostgreSQL's error.  One the
 * statement drops too, in dropping, is dropped without a word.  Each view
 * dropped is added to *dropped, if given.
 */
static void drop_readers(Oid oid, int32_t attnum, const std::string &what,
                         bool cascade, const std::vector<Oid> &dropping,
                         std::vector<Oid> *dropped) {
  std::vector<reader_t> found;
  std::vector<Oid> order;
  StringInfoData detail;
  int n = 0;

  find_readers(oid, attnum, what, found, order);
  initStringInfo(&detail);
  for (const reader_t &r : found) {
    if (std::find(dropping.begin(), dropping.end(), r.oid) != dropping.end())
      continue;
    /* A line each, as PostgreSQL's reportDependentObjects() writes them. */
    if (n++ > 0)
      appendStringInfoChar(&detail, '\n');
    if (cascade)
      appendStringInfo(&detail, "drop cascades to view %s", r.name.c_str());
    else
      appendStringInfo(&detail, "view %s depends on %s", r.name.c_str(),
                       r.reads.c_str());
  }
  if (n > 0 && !cascade)
    ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                    errmsg("cannot drop %s because other objects depend on it",
                           what.c_str()),
                    errdetail("%s", detail.data),
                    errhint("Use DROP ... CASCADE to drop the dependent "
                            "objects too.")));
  if (n == 1)
    ereport(NOTICE, (errmsg("%s", detail.data)));
  else if (n > 1)
    ereport(NOTICE, (errmsg("drop cascades to %d other objects", n),
                     errdetail("%s", detail.data)));
  for (Oid v : order) {
    drop_view_oid(v);
    if (dropped != nullptr)
      dropped->push_back(v);
  }
}

/*
 * Drop the views a DROP VIEW names, from sm, in this transaction.  A view
 * another reads is what depends on it: CASCADE drops the reader, RESTRICT
 * is an error, and one the statement drops too is dropped with it.
 */
void smstore_drop_views(DropStmt *stmt) {
  std::vector<Oid> dropping, dropped;
  ListCell *l;

  foreach (l, stmt->objects)
    dropping.push_back(RangeVarGetRelid(
        makeRangeVarFromNameList((List *)lfirst(l)), NoLock, true));
  foreach (l, stmt->objects) {
    RangeVar *rv = makeRangeVarFromNameList((List *)lfirst(l));
    Oid named = dropping[foreach_current_index(l)];
    Oid oid;
    Relation rel;

    /* Dropped already, as a reader of one named before it. */
    if (OidIsValid(named) &&
        std::find(dropped.begin(), dropped.end(), named) != dropped.end())
      continue;
    /* Locked against other sessions' use until the transaction ends. */
    oid = RangeVarGetRelid(rv, AccessExclusiveLock, true);
    if (!OidIsValid(oid)) {
      if (!stmt->missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("view \"%s\" does not exist", rv->relname)));
      ereport(NOTICE,
              (errmsg("view \"%s\" does not exist, skipping", rv->relname)));
      continue;
    }
    rel = relation_open(oid, NoLock);
    if (rel->rd_rel->relkind != RELKIND_VIEW)
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("\"%s\" is not a view", rv->relname),
                      rel->rd_rel->relkind == RELKIND_RELATION
                          ? errhint("Use DROP TABLE to remove a table.")
                          : 0));
    relation_close(rel, NoLock);
    drop_readers(oid, -1, std::string("view ") + rv->relname,
                 stmt->behavior == DROP_CASCADE, dropping, &dropped);
    drop_view_oid(oid);
    dropped.push_back(oid);
  }
}

/*
 * Drop every temporary view of the session, in this transaction, each after
 * the views that read it.  One dropped already, as a reader of another, has
 * no name any more.
 */
static void drop_temp_views(void) {
  std::vector<std::string> names;

  if (sm_cat_list_views(xact_session(), SM_NSP_TEMP, names) != 0)
    sm_error("list the temporary views");
  for (const std::string &name : names) {
    Oid oid = smstore_relation_oid(name.c_str(), PG_TEMP_NAMESPACE);
    std::vector<reader_t> found;
    std::vector<Oid> order;

    if (!OidIsValid(oid))
      continue;
    find_readers(oid, -1, "", found, order);
    order.push_back(oid);
    for (Oid v : order)
      drop_view_oid(v);
  }
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
  relation_close(rel, NoLock);
  forget_table(relid);
}

/*
 * Forget the table relid's definition, which DDL has changed or is to:
 * every session's descriptor of it once this one commits, this one's at
 * its next command, and this transaction's handles of it, which hold the
 * definition they were opened with, so the next use opens one afresh.
 */
static void forget_table(Oid relid) {
  Relation rel = RelationIdGetRelation(relid);

  if (RelationIsValid(rel)) {
    CacheInvalidateRelcache(rel);
    relation_close(rel, NoLock);
  }
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
    if (is_decimal(typid))
      *out = numeric_to_sm(value, typid, typmod, buf);
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
  if (is_decimal(typid) && typmod < (int32)VARHDRSZ)
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

/*
 * DROP COLUMN name of the table relid, in sm; it reads as null from now.  A
 * view that reads it is what depends on it: CASCADE drops the view, RESTRICT
 * is an error.
 */
static void drop_column(Oid relid, const char *relname, const char *name,
                        bool missing_ok, bool cascade) {
  sm_session_t *s = xact_session();
  sm_table_def_t def;

  if (table_def_now(relid, def))
    for (size_t i = 0; i < def.columns.size(); i++)
      if (!def.columns[i].dropped && def.columns[i].name == name)
        drop_readers(relid, (int32_t)i,
                     std::string("column ") + name + " of table " + relname,
                     cascade, {}, nullptr);

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

static void add_fkey(Oid relid, const char *relname, Constraint *con);
static void drop_constraint(Oid relid, const char *relname, const char *name,
                            bool missing_ok);
static void validate_constraint(Oid relid, const char *relname,
                                const char *name);

static void alter_view(Oid relid, AlterTableStmt *stmt);

/*
 * Do an ALTER TABLE of the table relid, in sm, in this transaction, one
 * subcommand after another: ADD COLUMN, DROP COLUMN, and ADD, DROP and
 * VALIDATE CONSTRAINT of a foreign key, the subcommands
 * AlterTableLookupRelation() lets through.  A view's is alter_view()'s.
 */
void smstore_alter_table(Oid relid, AlterTableStmt *stmt) {
  const char *relname = stmt->relation->relname;
  ListCell *l;

  if (get_rel_relkind(relid) == RELKIND_VIEW) {
    alter_view(relid, stmt);
    return;
  }
  foreach (l, stmt->cmds) {
    AlterTableCmd *cmd = lfirst_node(AlterTableCmd, l);

    alter_table_rel(relid, relname);
    if (cmd->subtype == AT_AddColumn)
      add_column(relid, relname, castNode(ColumnDef, cmd->def),
                 cmd->missing_ok);
    else if (cmd->subtype == AT_AddConstraint) {
      Constraint *con = castNode(Constraint, cmd->def);

      if (con->contype != CONSTR_FOREIGN)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("ALTER TABLE ... ADD CONSTRAINT supports only "
                               "FOREIGN KEY yet")));
      add_fkey(relid, relname, con);
    } else if (cmd->subtype == AT_DropConstraint)
      drop_constraint(relid, relname, cmd->name, cmd->missing_ok);
    else if (cmd->subtype == AT_ValidateConstraint)
      validate_constraint(relid, relname, cmd->name);
    else
      drop_column(relid, relname, cmd->name, cmd->missing_ok,
                  cmd->behavior == DROP_CASCADE);
    /* The next subcommand sees what this one did. */
    CommandCounterIncrement();
  }
}

/*
 * The table or view an ALTER ... RENAME names, locked, or InvalidOid if it
 * is not there and stmt says IF EXISTS.  ALTER VIEW names a view, and ALTER
 * TABLE either, as type says; *view is whether it is one.
 */
static Oid rename_target(RenameStmt *stmt, ObjectType type, bool *view) {
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
  *view = get_rel_relkind(relid) == RELKIND_VIEW;
  if (*view) {
    /* Every session builds its descriptor afresh once this one commits. */
    forget_table(relid);
    return relid;
  }
  if (type == OBJECT_VIEW)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a view", stmt->relation->relname),
                    errhint("Use ALTER TABLE instead.")));
  alter_table_rel(relid, stmt->relation->relname);
  return relid;
}

static void rename_view_column(Oid relid, RenameStmt *stmt);

/*
 * ALTER TABLE or ALTER VIEW ... RENAME TO: rename the table or the view in
 * sm, in this transaction, in its schema.  Returns its address.
 */
ObjectAddress smstore_rename_table(RenameStmt *stmt) {
  bool view;
  Oid relid = rename_target(stmt, stmt->renameType, &view);
  ObjectAddress address;

  if (!OidIsValid(relid))
    return InvalidObjectAddress;
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return view ? sm_cat_rename_view(s, relid, stmt->newname)
                    : sm_cat_rename_table(s, relid, stmt->newname);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists", stmt->newname)));
    sm_error("rename a table");
  }
  ObjectAddressSet(address, RelationRelationId, relid);
  return address;
}

/*
 * ALTER TABLE or ALTER VIEW ... RENAME COLUMN: rename the column in sm, in
 * this transaction; a table's indexes go on using it.  Returns its address.
 */
ObjectAddress smstore_rename_column(RenameStmt *stmt) {
  bool view;
  Oid relid = rename_target(stmt, stmt->relationType, &view);
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
  if (view) {
    rename_view_column(relid, stmt);
    ObjectAddressSubSet(address, RelationRelationId, relid, attnum);
    return address;
  }
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
 * The view relid's record, read under its lock X, taken here, as DDL that
 * changes it reads it: the lock sm's replace takes, which no reader of the
 * view holds with it.
 */
static sm_view_def_t view_def_locked(Oid relid) {
  sm_view_def_t def;
  uint64_t version;

  if (!lock_view(relid, SM_LOCKMODE_EX, &version) ||
      sm_cat_view_def(xact_session(), relid, def) != 0)
    elog(ERROR, "could not open view with OID %u", relid);
  return def;
}

/* Replace the view relid's record with def, in sm, in this transaction. */
static void put_view_def(Oid relid, const sm_view_def_t &def) {
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_replace_view(s, relid, def); }) != 0)
    sm_error("change a view");
  /* Every session builds its descriptor afresh, this one at its next use. */
  forget_table(relid);
}

/*
 * RENAME COLUMN of the view relid: the name its query's select list gives
 * the column, kept in sm with the query, where PostgreSQL renames its
 * pg_attribute row.  What reads the view reads its columns by place.
 */
static void rename_view_column(Oid relid, RenameStmt *stmt) {
  sm_view_def_t def = view_def_locked(relid);
  List *actions = (List *)stringToNode(def.query.c_str());
  TargetEntry *old = NULL;
  bool taken = false;

  foreach_node(TargetEntry, tle, linitial_node(Query, actions)->targetList) {
    if (tle->resjunk)
      continue;
    if (strcmp(tle->resname, stmt->subname) == 0)
      old = tle;
    taken |= strcmp(tle->resname, stmt->newname) == 0;
  }
  if (old == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" does not exist", stmt->subname)));
  if (taken)
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
                    errmsg("column \"%s\" of relation \"%s\" already exists",
                           stmt->newname, stmt->relation->relname)));
  old->resname = pstrdup(stmt->newname);
  def.query = nodeToString(actions);
  put_view_def(relid, def);
}

/*
 * ALTER VIEW ... SET or RESET (option, ...) of the view relid, in sm, as
 * PostgreSQL's ATExecSetRelOptions() does a view's: the options checked, and
 * check_option kept only on a view that is automatically updatable.
 */
static void alter_view_options(Oid relid, AlterTableCmd *cmd) {
  sm_view_def_t def = view_def_locked(relid);
  Datum reloptions = transformRelOptions(
      smstore_view_reloptions(def.options.c_str()), castNode(List, cmd->def),
      NULL, NULL, false, cmd->subtype == AT_ResetRelOptions);
  Query *query = linitial_node(Query, (List *)stringToNode(def.query.c_str()));

  (void)view_reloptions(reloptions, true);
  def.options = view_options(reloptions);
  foreach_node(DefElem, d,
               reloptions != (Datum)0 ? untransformRelOptions(reloptions)
                                      : NIL) {
    const char *error;

    if (strcmp(d->defname, "check_option") != 0)
      continue;
    error = view_query_is_auto_updatable(query, true);
    if (error != NULL)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("WITH CHECK OPTION is supported only on "
                             "automatically updatable views"),
                      errhint("%s", _(error))));
  }
  put_view_def(relid, def);
}

/*
 * ALTER VIEW, or ALTER TABLE, of the view relid, in sm, one subcommand after
 * another: SET and RESET of its options, the subcommands
 * AlterTableLookupRelation() lets through for a view.
 */
static void alter_view(Oid relid, AlterTableStmt *stmt) {
  foreach_node(AlterTableCmd, cmd, stmt->cmds) {
    alter_view_options(relid, cmd);
    /* The next subcommand sees what this one did. */
    CommandCounterIncrement();
  }
}

static void check_fkey_rows(Oid relid, Oid pkoid, Oid oid,
                            const char *conname);

/*
 * The bytes sm keeps for a foreign key, the host's: its three operators a
 * column pair, then its ON DELETE SET columns, each list its length first,
 * every word 8 bytes, in the machine's order.
 */
static std::string fkey_host(const smstore_fkey_t *fk) {
  std::string b;
  auto put = [&](uint64_t v) { b.append((const char *)&v, sizeof(v)); };

  put((uint64_t)fk->nkeys);
  for (int i = 0; i < fk->nkeys; i++) {
    put(fk->pf_eq_oprs[i]);
    put(fk->pp_eq_oprs[i]);
    put(fk->ff_eq_oprs[i]);
  }
  put((uint64_t)fk->ndelsetcols);
  for (int i = 0; i < fk->ndelsetcols; i++)
    put((uint64_t)fk->delsetcols[i]);
  return b;
}

/* The foreign key def of the table table, from sm, into fk. */
static void fkey_from_sm(const sm_fkey_def_t &def, uint64_t table,
                         smstore_fkey_t *fk) {
  const std::string &b = def.host;
  size_t pos = 0;
  auto get = [&]() {
    uint64_t v = 0;

    if (pos + sizeof(v) > b.size())
      elog(ERROR, "foreign key %llu: its operators are missing",
           (unsigned long long)def.oid);
    memcpy(&v, b.data() + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  };

  memset(fk, 0, sizeof(*fk));
  fk->oid = (Oid)def.oid;
  namestrcpy(&fk->name, def.name.c_str());
  fk->table = (Oid)table;
  fk->ref_table = (Oid)def.ref_table;
  fk->ref_index = (Oid)def.ref_index;
  fk->nkeys = (int)def.columns.size();
  for (int i = 0; i < fk->nkeys; i++) {
    fk->attnums[i] = (AttrNumber)(def.columns[i] + 1);
    fk->ref_attnums[i] = (AttrNumber)(def.ref_columns[i] + 1);
  }
  if ((int)get() != fk->nkeys)
    elog(ERROR, "foreign key %llu: its operators are not its columns'",
         (unsigned long long)def.oid);
  for (int i = 0; i < fk->nkeys; i++) {
    fk->pf_eq_oprs[i] = (Oid)get();
    fk->pp_eq_oprs[i] = (Oid)get();
    fk->ff_eq_oprs[i] = (Oid)get();
  }
  fk->ndelsetcols = (int)get();
  for (int i = 0; i < fk->ndelsetcols && i < INDEX_MAX_KEYS; i++)
    fk->delsetcols[i] = (AttrNumber)get();
  fk->on_update = def.on_update;
  fk->on_delete = def.on_delete;
  fk->match = def.match;
  fk->deferrable = def.deferrable;
  fk->deferred = def.deferred;
  fk->validated = def.validated;
}

/*
 * The foreign key oid as this transaction sees it, into out: false if
 * there is none.  sm takes no lock for it: the caller holds its table's.
 */
bool smstore_get_fkey(Oid oid, smstore_fkey_t *out) {
  sm_fkey_def_t def;
  uint64_t table = 0;

  switch (sm_cat_fkey(xact_session(), oid, &table, &def)) {
  case 0:
    fkey_from_sm(def, table, out);
    return true;
  case 1:
    return false;
  default:
    sm_error("look up a foreign key");
  }
  return false; /* not reached */
}

/*
 * The table oid's definition as this transaction sees it now, into out,
 * under its lock IS: false if there is no such table.  A handle opened
 * earlier in the transaction keeps the definition it was opened with
 * (sm_tbl_def()), which DDL since, an index or a foreign key, may have
 * changed.
 */
static bool table_def_now(Oid oid, sm_table_def_t &out) {
  if (open_table(oid, SM_LOCKMODE_IS) == NULL)
    return false;
  int rc = sm_cat_table_def_oid(xact_session(), oid, out);

  if (rc < 0)
    sm_error("read a table's definition");
  return rc == 0;
}

/*
 * The other tables whose foreign keys reference the table relid, each once,
 * as this transaction sees them.
 */
List *smstore_referencing_tables(Oid relid) {
  sm_table_def_t def;
  List *out = NIL;

  if (!table_def_now(relid, def))
    return NIL;
  for (const sm_fkey_ref_t &r : def.referenced_by)
    if ((Oid)r.table != relid)
      out = list_append_unique_oid(out, (Oid)r.table);
  return out;
}

/*
 * The column named name of rel, its number from 1, type and collation
 * into the arrays at i; an error if it has none.
 */
static void fkey_column(Relation rel, const char *name, int i,
                        AttrNumber *attnum, Oid *typid, Oid *collid) {
  TupleDesc td = RelationGetDescr(rel);

  for (int a = 0; a < td->natts; a++) {
    Form_pg_attribute att = TupleDescAttr(td, a);

    if (!att->attisdropped && strcmp(NameStr(att->attname), name) == 0) {
      attnum[i] = att->attnum;
      typid[i] = att->atttypid;
      collid[i] = att->attcollation;
      return;
    }
  }
  ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                  errmsg("column \"%s\" referenced in foreign key constraint "
                         "does not exist",
                         name)));
}

/*
 * A name for a foreign key of the table relname on the columns cols, free
 * among the table's foreign keys, as ChooseConstraintName() makes one.
 */
static char *fkey_name(const char *relname, List *cols,
                       const sm_table_def_t *def) {
  char buf[NAMEDATALEN * 2];
  ListCell *l;
  int n = 0;

  buf[0] = '\0';
  foreach (l, cols) {
    size_t used = strlen(buf);

    if (used > 0 && used + 1 < sizeof(buf))
      strcat(buf, "_");
    strlcat(buf, strVal(lfirst(l)), sizeof(buf));
    if (strlen(buf) >= NAMEDATALEN)
      break;
  }
  for (;;) {
    char suffix[NAMEDATALEN];
    char *name;
    bool taken = false;

    if (n == 0)
      strlcpy(suffix, "fkey", sizeof(suffix));
    else
      snprintf(suffix, sizeof(suffix), "fkey%d", n);
    name = makeObjectName(relname, buf, suffix);
    for (const sm_fkey_def_t &f : def->fkeys)
      taken |= f.name == name;
    if (!taken)
      return name;
    n++;
  }
}

/*
 * ALTER TABLE ... ADD FOREIGN KEY, and CREATE TABLE's, which parse analysis
 * makes one of: choose the referenced columns, their unique index and the
 * operators as ATAddForeignKeyConstraint() does, and put the foreign key in
 * sm, which keeps it (../sm/FOREIGNKEY.md).  Unless it is NOT VALID, or the
 * table is new, the rows there are checked first, by RI_Initial_Check().
 */
static void add_fkey(Oid relid, const char *relname, Constraint *con) {
  AttrNumber fkattnum[INDEX_MAX_KEYS] = {0}, pkattnum[INDEX_MAX_KEYS] = {0};
  Oid fktypoid[INDEX_MAX_KEYS] = {0}, pktypoid[INDEX_MAX_KEYS] = {0};
  Oid fkcolloid[INDEX_MAX_KEYS] = {0}, pkcolloid[INDEX_MAX_KEYS] = {0};
  AttrNumber delsetcols[INDEX_MAX_KEYS] = {0};
  smstore_fkey_t fk;
  int numfks = 0, numpks = 0, numdelset = 0;
  ListCell *l;

  if (con->fk_with_period || con->pk_with_period)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a foreign key using PERIOD is not supported")));
  if (con->fk_matchtype == FKCONSTR_MATCH_PARTIAL)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("MATCH PARTIAL not yet implemented")));
  /*
   * A deferred check fires at commit, past the statement whose tuple store
   * keeps an sm row's trigger tuples (commands/trigger.c).
   */
  if (con->deferrable || con->initdeferred)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a DEFERRABLE foreign key is not supported yet")));
  if (!con->is_enforced)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a NOT ENFORCED foreign key is not supported yet")));

  Oid pkoid = RangeVarGetRelid(con->pktable, NoLock, false);

  if (pkoid < FirstNormalObjectId)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("referenced relation \"%s\" is not a table",
                           con->pktable->relname)));
  /* The descriptors' columns alone: sm takes the tables' locks, X both. */
  Relation rel = RelationIdGetRelation(relid);
  Relation pkrel = RelationIdGetRelation(pkoid);

  if (!RelationIsValid(rel) || !RelationIsValid(pkrel))
    elog(ERROR, "could not open the relations of a foreign key");
  if ((rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP) !=
      (pkrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
             errmsg(rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP
                        ? "constraints on temporary tables may reference "
                          "only temporary tables"
                        : "constraints on permanent tables may reference "
                          "only permanent tables")));
  foreach (l, con->fk_attrs) {
    if (numfks >= INDEX_MAX_KEYS)
      ereport(ERROR, (errcode(ERRCODE_TOO_MANY_COLUMNS),
                      errmsg("cannot have more than %d keys in a foreign key",
                             INDEX_MAX_KEYS)));
    fkey_column(rel, strVal(lfirst(l)), numfks++, fkattnum, fktypoid,
                fkcolloid);
  }
  foreach (l, con->fk_del_set_cols) {
    Oid t[INDEX_MAX_KEYS], c[INDEX_MAX_KEYS];
    bool among = false;

    fkey_column(rel, strVal(lfirst(l)), numdelset, delsetcols, t, c);
    for (int i = 0; i < numfks; i++)
      among |= fkattnum[i] == delsetcols[numdelset];
    if (!among)
      ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                      errmsg("column \"%s\" referenced in ON DELETE SET "
                             "action must be part of foreign key",
                             strVal(lfirst(l)))));
    numdelset++;
  }

  /* The referenced columns: those named, or the primary key's. */
  sm_table_def_t pkdef_now;

  if (!table_def_now(pkoid, pkdef_now))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           con->pktable->relname)));
  const sm_table_def_t *pkdef = &pkdef_now;
  const sm_index_def_t *index = NULL;

  if (con->pk_attrs == NIL) {
    for (const sm_index_def_t &x : pkdef->indexes)
      if (x.primary)
        index = &x;
    if (index == NULL)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                      errmsg("there is no primary key for referenced table "
                             "\"%s\"",
                             RelationGetRelationName(pkrel))));
    for (int32_t a : index->key_attnums) {
      Form_pg_attribute att = TupleDescAttr(RelationGetDescr(pkrel), a);

      pkattnum[numpks] = att->attnum;
      pktypoid[numpks] = att->atttypid;
      pkcolloid[numpks] = att->attcollation;
      con->pk_attrs = lappend(con->pk_attrs,
                              makeString(pstrdup(NameStr(att->attname))));
      numpks++;
    }
  } else {
    foreach (l, con->pk_attrs) {
      if (numpks >= INDEX_MAX_KEYS)
        ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_COLUMNS),
                 errmsg("cannot have more than %d keys in a foreign key",
                        INDEX_MAX_KEYS)));
      fkey_column(pkrel, strVal(lfirst(l)), numpks++, pkattnum, pktypoid,
                  pkcolloid);
    }
    /* A unique index whose keys are the columns, in any order. */
    for (const sm_index_def_t &x : pkdef->indexes) {
      bool fits = x.unique && x.eval.empty() &&
                  (int)x.key_attnums.size() == numpks &&
                  x.state == SM_INDEX_VALID;

      for (int i = 0; fits && i < numpks; i++) {
        bool found = false;

        for (int32_t a : x.key_attnums)
          found |= a + 1 == pkattnum[i];
        fits = found;
      }
      if (fits && index == NULL)
        index = &x;
    }
    if (index == NULL)
      ereport(ERROR, (errcode(ERRCODE_INVALID_FOREIGN_KEY),
                      errmsg("there is no unique constraint matching given "
                             "keys for referenced table \"%s\"",
                             RelationGetRelationName(pkrel))));
  }
  if (numfks != numpks)
    ereport(ERROR, (errcode(ERRCODE_INVALID_FOREIGN_KEY),
                    errmsg("number of referencing and referenced columns for "
                           "foreign key disagree")));

  /* Its name, chosen as PostgreSQL chooses one, if not given. */
  sm_table_def_t def_now;

  if (!table_def_now(relid, def_now))
    elog(ERROR, "could not open relation with OID %u", relid);
  const char *conname = con->conname != NULL
                            ? con->conname
                            : fkey_name(relname, con->fk_attrs, &def_now);

  /*
   * The equality operators, as ATAddForeignKeyConstraint() finds them, with
   * the default btree class of each referenced column's type for its index
   * column's: sm's index keys have no operator class of their own.
   */
  memset(&fk, 0, sizeof(fk));
  for (int i = 0; i < numpks; i++) {
    Oid pktype = pktypoid[i], fktype = fktypoid[i];
    Oid opclass = GetDefaultOpClass(pktype, BTREE_AM_OID);
    Oid opfamily, opcintype, fktyped, pfeqop, ppeqop, ffeqop = InvalidOid;

    if (!OidIsValid(opclass))
      ereport(ERROR,
              (errcode(ERRCODE_UNDEFINED_OBJECT),
               errmsg("could not identify an equality operator for foreign "
                      "key")));
    opfamily = get_opclass_family(opclass);
    opcintype = get_opclass_input_type(opclass);
    ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
                                 BTEqualStrategyNumber);
    if (!OidIsValid(ppeqop))
      elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
           BTEqualStrategyNumber, opcintype, opcintype, opfamily);
    fktyped = getBaseType(fktype);
    pfeqop = get_opfamily_member(opfamily, opcintype, fktyped,
                                 BTEqualStrategyNumber);
    if (OidIsValid(pfeqop))
      ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
                                   BTEqualStrategyNumber);
    if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop))) {
      Oid input_typeids[2] = {pktype, fktype};
      Oid target_typeids[2] = {opcintype, opcintype};

      if (can_coerce_type(2, input_typeids, target_typeids,
                          COERCION_IMPLICIT))
        pfeqop = ffeqop = ppeqop;
    }
    if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
      ereport(ERROR,
              (errcode(ERRCODE_DATATYPE_MISMATCH),
               errmsg("foreign key constraint \"%s\" cannot be implemented",
                      conname),
               errdetail("Key columns \"%s\" of the referencing table and "
                         "\"%s\" of the referenced table "
                         "are of incompatible types: %s and %s.",
                         strVal(list_nth(con->fk_attrs, i)),
                         strVal(list_nth(con->pk_attrs, i)),
                         format_type_be(fktype), format_type_be(pktype))));
    if (OidIsValid(pkcolloid[i]) && OidIsValid(fkcolloid[i]) &&
        pkcolloid[i] != fkcolloid[i] &&
        (!get_collation_isdeterministic(pkcolloid[i]) ||
         !get_collation_isdeterministic(fkcolloid[i])))
      ereport(ERROR,
              (errcode(ERRCODE_COLLATION_MISMATCH),
               errmsg("foreign key constraint \"%s\" cannot be implemented",
                      conname),
               errdetail("Key columns \"%s\" of the referencing table and "
                         "\"%s\" of the referenced table "
                         "have incompatible collations: \"%s\" and \"%s\".  "
                         "If either collation is nondeterministic, then both "
                         "collations have to be the same.",
                         strVal(list_nth(con->fk_attrs, i)),
                         strVal(list_nth(con->pk_attrs, i)),
                         get_collation_name(fkcolloid[i]),
                         get_collation_name(pkcolloid[i]))));
    fk.pf_eq_oprs[i] = pfeqop;
    fk.pp_eq_oprs[i] = ppeqop;
    fk.ff_eq_oprs[i] = ffeqop;
  }
  fk.nkeys = numfks;
  fk.ndelsetcols = numdelset;
  memcpy(fk.delsetcols, delsetcols, sizeof(delsetcols));

  /*
   * The rows there are checked once it is in sm, in this transaction, under
   * the locks X it takes on both tables, unless the table is new or it is
   * NOT VALID; validated says whether they were.
   */
  bool check = !con->skip_validation && con->initially_valid;
  sm_fkey_def_t def;

  def.name = conname;
  def.ref_table = pkoid;
  def.ref_index = index->oid;
  for (int i = 0; i < numfks; i++) {
    def.columns.push_back(fkattnum[i] - 1);
    def.ref_columns.push_back(pkattnum[i] - 1);
  }
  def.on_update = con->fk_upd_action;
  def.on_delete = con->fk_del_action;
  def.match = con->fk_matchtype;
  def.validated = con->initially_valid;
  def.host = fkey_host(&fk);
  relation_close(rel, NoLock);
  relation_close(pkrel, NoLock);

  sm_session_t *s = xact_session();
  uint64_t oid = 0;

  if (waiting([&] { return sm_cat_create_fkey(s, relid, def, &oid); }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT),
                      errmsg("constraint \"%s\" for relation \"%s\" already "
                             "exists",
                             conname, relname)));
    sm_error("create a foreign key");
  }
  /* Both tables' descriptors relearn their triggers, at the next command. */
  forget_table(relid);
  forget_table(pkoid);
  CommandCounterIncrement();
  if (check)
    check_fkey_rows(relid, pkoid, (Oid)oid, conname);
}

/*
 * Check that every row of the table relid meets its foreign key oid, named
 * conname, to the table pkoid: RI_Initial_Check()'s one query, as
 * validateForeignKeyConstraint() makes it.
 */
static void check_fkey_rows(Oid relid, Oid pkoid, Oid oid,
                            const char *conname) {
  Trigger trig;
  /*
   * Held to the end of the transaction, as VALIDATE CONSTRAINT takes them:
   * the rows checked do not change before it ends.  ADD FOREIGN KEY holds
   * both X already.
   */
  Relation rel = table_open(relid, ShareUpdateExclusiveLock);
  Relation pkrel = table_open(pkoid, RowShareLock);

  memset(&trig, 0, sizeof(trig));
  trig.tgoid = InvalidOid;
  trig.tgname = pstrdup(conname);
  trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
  trig.tgisinternal = true;
  trig.tgconstrrelid = pkoid;
  trig.tgconstraint = oid;
  if (!RI_Initial_Check(&trig, rel, pkrel))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("the rows of \"%s\" cannot be checked against "
                           "foreign key \"%s\"",
                           RelationGetRelationName(rel), conname)));
  table_close(pkrel, NoLock);
  table_close(rel, NoLock);
}

/* The foreign key of the table relid named name, into out: false if none. */
static bool fkey_named(Oid relid, const char *name, sm_fkey_def_t &out) {
  sm_table_def_t def;

  if (!table_def_now(relid, def))
    return false;
  for (const sm_fkey_def_t &f : def.fkeys)
    if (f.name == name) {
      out = f;
      return true;
    }
  return false;
}

/* An error for a constraint name the table relname has none of. */
static void no_constraint(const char *name, const char *relname) {
  ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                  errmsg("constraint \"%s\" of relation \"%s\" does not exist",
                         name, relname)));
}

/* ALTER TABLE ... DROP CONSTRAINT: a foreign key, the only one sm keeps. */
static void drop_constraint(Oid relid, const char *relname, const char *name,
                            bool missing_ok) {
  sm_fkey_def_t f;

  if (!fkey_named(relid, name, f)) {
    if (!missing_ok)
      no_constraint(name, relname);
    ereport(NOTICE, (errmsg("constraint \"%s\" of relation \"%s\" does not "
                            "exist, skipping",
                            name, relname)));
    return;
  }
  uint64_t oid = f.oid;
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_drop_fkey(s, relid, oid); }) != 0)
    sm_error("drop a foreign key");
  forget_table(relid);
  forget_table((Oid)f.ref_table);
}

/* ALTER TABLE ... VALIDATE CONSTRAINT: check the rows, then say so. */
static void validate_constraint(Oid relid, const char *relname,
                                const char *name) {
  sm_fkey_def_t f;

  if (!fkey_named(relid, name, f))
    no_constraint(name, relname);
  if (f.validated)
    return;
  uint64_t oid = f.oid;
  Oid pkoid = (Oid)f.ref_table;
  std::string conname = f.name;

  check_fkey_rows(relid, pkoid, (Oid)oid, conname.c_str());
  sm_session_t *s = xact_session();

  if (waiting([&] { return sm_cat_validate_fkey(s, relid, oid); }) != 0)
    sm_error("validate a foreign key");
  forget_table(relid);
}

/* ALTER TABLE ... RENAME CONSTRAINT, of a foreign key.  Returns its address. */
ObjectAddress smstore_rename_constraint(RenameStmt *stmt) {
  bool view;
  Oid relid = rename_target(stmt, OBJECT_TABLE, &view);
  ObjectAddress address;

  if (!OidIsValid(relid))
    return InvalidObjectAddress;
  /* A view has no constraint of its own. */
  if (view)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a table", stmt->relation->relname)));
  sm_fkey_def_t f;

  if (!fkey_named(relid, stmt->subname, f))
    no_constraint(stmt->subname, stmt->relation->relname);
  uint64_t oid = f.oid;
  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_rename_fkey(s, relid, oid, stmt->newname);
      }) != 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT),
                      errmsg("constraint \"%s\" for relation \"%s\" already "
                             "exists",
                             stmt->newname, stmt->relation->relname)));
    sm_error("rename a foreign key");
  }
  forget_table(relid);
  ObjectAddressSet(address, ConstraintRelationId, (Oid)oid);
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

  return is_decimal(typid) ? numeric_from_sm(d, typid, typmod) : (Datum)d;
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
    /*
     * The column is the table's, or the index's for an expression.  Locked
     * first, so the descriptor read is the one sm holds.
     */
    Relation rel =
        relation_open(x.has_index ? (Oid)x.index : oid, AccessShareLock);
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

/* Whether this transaction holds the view oid's lock in a mode covering mode.
 */
static bool view_held(Oid oid, sm_lockmode_t mode) {
  for (const held_view_t &h : tcb->smstore->held_views)
    if (h.oid == oid && mode_covers(h.mode, mode))
      return true;
  return false;
}

/*
 * Take the view oid's lock in mode, until the transaction ends, and set
 * *version to its version under the lock: false if oid is no view.  A view
 * has a table's lock in sm (../sm/VIEW.md section 7): a query takes it
 * IS, its DDL X.
 */
static bool lock_view(Oid oid, sm_lockmode_t mode, uint64_t *version) {
  sm_session_t *s = xact_session();
  int rc;

  rc = waiting([&] { return sm_cat_lock_view(s, oid, mode, version); });
  if (rc < 0)
    sm_error("lock a view");
  if (rc == 1)
    return false;
  /* Remembered until the transaction ends, which releases it. */
  if (!view_held(oid, mode))
    tcb->smstore->held_views.push_back(
        {oid, mode, GetCurrentTransactionNestLevel()});
  return true;
}

/*
 * Lock the relation relid, if it is a table in sm, as PostgreSQL locks a
 * relation before it reads its definition (lmgr.c): so a definition a DDL
 * is changing is read once the DDL has ended.  Only the conflict with DDL,
 * which takes X, matters here: the call into sm that uses the table takes
 * what it needs, so a writer takes IX and anything not exclusive IS -- an
 * index build or an analyze in sm keeps no writer out.  An index known by
 * its table (smstore_index_table()) is locked as its table, and a view by a
 * lock of its own; a sequence is not a table in sm, and takes nothing.
 */
void smstore_lock_relation(Oid relid, LOCKMODE lockmode) {
  sm_lockmode_t mode = relation_lock_mode(lockmode);
  uint64_t version;

  if (view_held(relid, mode))
    return;
  /* Held until the transaction ends, as the handle is. */
  sm_tbl_t *tbl = open_table(lock_target(relid), mode);

  /* What this session caches of it is from the version now held, or goes. */
  if (tbl != NULL)
    RelationCheckSmVersion(relid, lock_target(relid), sm_tbl_def(tbl)->version);
  else if (lock_view(relid, mode, &version))
    RelationCheckSmVersion(relid, relid, version);
}

/*
 * As smstore_lock_relation, if the lock is free now: false, with nothing
 * taken, if another transaction holds it in a mode that conflicts.  A
 * relation that is no table in sm takes nothing, and is true.
 */
bool smstore_trylock_relation(Oid relid, LOCKMODE lockmode) {
  sm_lockmode_t mode = relation_lock_mode(lockmode);
  bool blocked;
  uint64_t version;

  if (view_held(relid, mode))
    return true;
  /* Held until the transaction ends, as the handle is. */
  sm_tbl_t *tbl = try_open_table(lock_target(relid), mode, &blocked);

  /* What this session caches of it is from the version now held, or goes. */
  if (tbl != NULL)
    RelationCheckSmVersion(relid, lock_target(relid), sm_tbl_def(tbl)->version);
  /*
   * A view's lock, which sm's open of no table took as it would a table's,
   * if it was free: so this does not wait.
   */
  else if (!blocked && lock_view(relid, mode, &version))
    RelationCheckSmVersion(relid, relid, version);
  return !blocked;
}

/*
 * Whether this transaction holds sm's lock on the relation relid in a mode
 * covering what lockmode takes (smstore_lock_relation()): on the table, or
 * for an index, on its table, or on a view.  A sequence takes no lock in sm,
 * so has none to hold, and is held.
 */
bool smstore_relation_held(Oid relid, LOCKMODE lockmode) {
  return held_table(lock_target(relid), relation_lock_mode(lockmode)) != NULL ||
         view_held(relid, relation_lock_mode(lockmode)) ||
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
    /* Locked first, as process_owned_by() locks it, to read its columns. */
    rel = relation_open(relid, AccessShareLock);
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
 * The rows of the duplicate key sm's last error met, as sm_errdup() gives
 * them: nrows of them, up to two.
 */
struct dup_rows_t {
  uint64_t rownos[2];
  int nrows;
};

/* The rows of the duplicate key sm's last error met. */
static dup_rows_t error_dup_rows(void) {
  dup_rows_t d;
  uint64_t index;

  d.nrows = sm_errdup(&index, d.rownos);
  return d;
}

/*
 * PostgreSQL's words for the key a row of dup holds in the columns keys of
 * the table relid, "(k)=(1)", the first of them the transaction sees; empty
 * if it sees neither, or the table is gone.
 */
static std::string dup_key(Oid relid, const std::vector<int32_t> &keys,
                           const dup_rows_t &dup) {
  Relation rel = try_relation_open(relid, AccessShareLock);
  sm_tbl_t *tbl;
  std::string out;
  int natts = 0;

  if (rel == NULL)
    return out;
  tbl = open_table(relid, SM_LOCKMODE_IS);
  for (int32_t k : keys)
    natts = Max(natts, k + 1);
  TupleTableSlot *slot =
      MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
  for (int i = 0; tbl != NULL && i < dup.nrows && out.empty(); i++) {
    if (!read_row(tbl, dup.rownos[i], natts, slot))
      continue;
    std::string names, values;

    for (int32_t k : keys) {
      Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), k);
      Oid output;
      bool varlena;

      if (!names.empty()) {
        names += ", ";
        values += ", ";
      }
      names += quote_identifier(NameStr(att->attname));
      if (slot->tts_isnull[k]) {
        values += "null";
        continue;
      }
      getTypeOutputInfo(att->atttypid, &output, &varlena);
      values += OidOutputFunctionCall(output, slot->tts_values[k]);
    }
    out = "(" + names + ")=(" + values + ")";
  }
  ExecDropSingleTupleTableSlot(slot);
  relation_close(rel, AccessShareLock);
  return out;
}

/*
 * Raise the error of the unique index name whose build met a duplicate key:
 * the key, as PostgreSQL says it, and the two rows of dup in a hint, the
 * lower first; why, sm's message, in place of the key if it could not be
 * read.
 */
[[noreturn]] static void duplicate_build(const char *name,
                                         const std::string &key,
                                         const dup_rows_t &dup,
                                         const char *why) {
  ereport(ERROR,
          (errcode(ERRCODE_UNIQUE_VIOLATION),
           errmsg("could not create unique index \"%s\"", name),
           key.empty() ? errdetail("%s", why)
                       : errdetail("Key %s is duplicated.", key.c_str()),
           dup.nrows == 2
               ? errhint("Rows %llu and %llu have it.",
                         (unsigned long long)Min(dup.rownos[0], dup.rownos[1]),
                         (unsigned long long)Max(dup.rownos[0], dup.rownos[1]))
               : 0));
  pg_unreachable();
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
  /* A table, an index and a sequence share a namespace's names. */
  if (OidIsValid(smstore_relation_oid(name, RelationGetNamespace(rel)))) {
    relation_close(rel, NoLock);
    if (stmt->if_not_exists) {
      ereport(NOTICE,
              (errcode(ERRCODE_DUPLICATE_TABLE),
               errmsg("relation \"%s\" already exists, skipping", name)));
      return InvalidObjectAddress;
    }
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" already exists", name)));
  }
  /* Every session's descriptor of the table relearns its indexes. */
  CacheInvalidateRelcache(rel);
  relation_close(rel, NoLock);

  sm_session_t *s = xact_session();

  if (waiting([&] {
        return sm_cat_create_index(s, relid, name, keys, includes, stmt->unique,
                                   std::string(), {}, &oid, stmt->primary,
                                   stmt->isconstraint);
      }) != 0) {
    /* A name taken since it was looked up, or a unique key duplicated. */
    if (sm_errnum() == SM_ERR_DUPKEY && !stmt->unique)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists", name)));
    if (sm_errnum() == SM_ERR_DUPKEY) {
      std::string why = sm_errmsg();
      dup_rows_t dup = error_dup_rows();
      std::vector<int32_t> places;

      for (const sm_index_key_spec_t &k : keys)
        places.push_back(k.attnum);
      duplicate_build(name, dup_key(relid, places, dup), dup, why.c_str());
    }
    sm_error("create an index");
  }
  made_index_t made = {relid, oid, name, stmt->unique, {}};

  for (const sm_index_key_spec_t &k : keys)
    made.keys.push_back(k.attnum);
  tcb->smstore->made_indexes.push_back(std::move(made));
  ObjectAddressSet(address, RelationRelationId, (Oid)oid);
  return address;
}

/*
 * Wait for the builds of the indexes the transaction just committed made,
 * outside any transaction, so the next statement's snapshot may scan them:
 * sm builds one on a table with rows in the background, after the commit
 * (sm_cat_create_index()).  A build that failed is dropped, in a
 * transaction of its own, and its error raised, so CREATE INDEX ends as
 * PostgreSQL's does: with an index or with none (DECISION.md).
 */
void smstore_wait_indexes(void) {
  std::vector<made_index_t> built;

  if (tcb->smstore == nullptr || tcb->smstore->built_after_commit.empty())
    return;
  /* Taken first: a cancel ends the wait with an error, and none is waited
   * for again. */
  built.swap(tcb->smstore->built_after_commit);
  for (const made_index_t &m : built) {
    if (waiting([&] { return sm_cat_index_wait(session(), m.table, m.index); })
        == 0)
      continue;
    /* Dropped since, in its transaction or by another. */
    if (sm_errnum() == SM_ERR_NOTFOUND)
      continue;
    int errnum = sm_errnum();
    int code = sm_sqlstate();
    std::string why = sm_errmsg();
    dup_rows_t dup = error_dup_rows();
    std::string key;

    /*
     * A failed index is good only for dropping: its own transaction does,
     * reading first the key a duplicate's row holds.
     */
    StartTransactionCommand();
    if (errnum == SM_ERR_DUPKEY && m.unique)
      key = dup_key(m.table, m.keys, dup);
    if (waiting([&] {
          return sm_cat_drop_index(xact_session(), m.table, m.index);
        }) != 0 &&
        sm_errnum() != SM_ERR_NOTFOUND)
      sm_error("drop an index whose build failed");
    CommitTransactionCommand();
    if (errnum == SM_ERR_DUPKEY && m.unique)
      duplicate_build(m.name.c_str(), key, dup, why.c_str());
    ereport(ERROR, (errcode(code), errmsg("could not create index \"%s\": %s",
                                          m.name.c_str(), why.c_str())));
  }
}

/*
 * The index named rv, and its table, in rv's namespace or the first on the
 * search path that has one: false if there is none.  The path includes the
 * implicit namespaces, so a temporary index hides one of the same name, and
 * rv's namespace may be pg_temp.
 */
static bool find_index(const RangeVar *rv, uint64_t *table, uint64_t *index) {
  List *path;
  ListCell *l;

  if (rv->schemaname != NULL)
    path = list_make1_oid(LookupExplicitNamespace(rv->schemaname, false));
  else
    path = fetch_search_path(true);
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
     * one of the index is gone, as PostgreSQL's index_drop() has it.  The
     * table is locked first, as index_drop() locks it, so the descriptor
     * read is the one sm holds, and held until the transaction ends.
     */
    rel = relation_open((Oid)table, AccessExclusiveLock);
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
 * meet keys: btree scan keys on the index's columns, each row's first natts
 * columns read, and the keys' columns.  Equal keys on its
 * leading columns are a prefix both bounds share; the next column's ranges
 * bound it below and above, in the index's own order, so a DESC column's
 * upper value is its lower bound.  Every row read is checked against every
 * key, so a key that bounds nothing still holds.
 */
smstore_scan_t *smstore_index_beginscan(Relation heap, Relation index,
                                        ScanKey keys, int nkeys, int natts) {
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
      if (!is_decimal(att->atttypid))
        return (sm_datum_t)key->sk_argument;
      return numeric_bound(key->sk_argument,
                           OidIsValid(key->sk_subtype) ? key->sk_subtype
                                                       : att->atttypid,
                           att->atttypmod, upper, (uint8 *)palloc(16));
    };
    bool numeric_col = is_decimal(att->atttypid);

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
  scan->relid = RelationGetRelid(heap);
  scan->keys = keys;
  scan->nkeys = nkeys;
  scan->index = RelationGetRelid(index);
  scan->form = form;
  scan->row_natts = RelationGetDescr(heap)->natts;
  scan->keycols = (AttrNumber *)palloc(Max(nkeys, 1) * sizeof(AttrNumber));
  scan->natts = natts;
  for (int i = 0; i < nkeys; i++) {
    scan->keycols[i] = form->indkey.values[keys[i].sk_attno - 1];
    /* Each row is checked against every key (keys_met()). */
    scan->natts = Max(scan->natts, scan->keycols[i]);
  }
  if (sm_tbl_idx_scan(tbl, RelationGetRelid(index), lo.n > 0 ? &lo : NULL,
                      hi.n > 0 ? &hi : NULL, NULL, &scan->icursor) != 0)
    sm_error("scan an index");
  return scan;
}

/*
 * The row at rowno, as this transaction sees it, into slot, as a virtual
 * tuple of its first natts columns, its tid its rowno; false, the slot
 * empty, if it sees none.
 */
static bool read_row(sm_tbl_t *tbl, uint64_t rowno, int natts,
                     TupleTableSlot *slot) {
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
  tuple_into_slot(tbl, rowno, tuple, len, natts, slot);
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

  slot->tts_tableOid = scan->relid;
  while (smstore_index_getrowno(scan, &rowno))
    /* A row the transaction does not see is passed. */
    if (read_row(scan->tbl, rowno, scan->natts, slot) &&
        keys_met(scan, slot, false))
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

  slot->tts_tableOid = scan->relid;
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

/*
 * Begin reading rows of rel, a table in sm, by rowno (smstore_fetch()),
 * each row's first natts columns read.
 */
smstore_scan_t *smstore_beginfetch(Relation rel, int natts) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  scan->relid = RelationGetRelid(rel);
  scan->natts = natts;
  return scan;
}

/*
 * The row at rowno into slot, as a virtual tuple of the scan's first natts
 * columns, its tid its rowno; false if the transaction sees none there.
 */
bool smstore_fetch(smstore_scan_t *scan, uint64 rowno, TupleTableSlot *slot) {
  slot->tts_tableOid = scan->relid;
  return read_row(scan->tbl, rowno, scan->natts, slot);
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
 * The row of rel at tid into slot, as a virtual tuple of its first natts
 * columns: the kept one if it is that row, else as the statement sees it;
 * false if it sees none.  The executor fetches the row it is to update, the
 * newest version of one it locked, the row it deleted, for RETURNING, and a
 * TID scan's rows.
 */
bool smstore_fetch_row(Relation rel, ItemPointer tid, int natts,
                       TupleTableSlot *slot) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  uint64 rowno = smstore_tid_to_rowno(tid);

  if (tbl == NULL)
    return false;
  slot->tts_tableOid = RelationGetRelid(rel);
  if (kept_oid == RelationGetRelid(rel) && kept_rowno == rowno &&
      kept_tuple != NULL) {
    ExecClearTuple(slot);
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len, natts, slot);
    return true;
  }
  return read_row(tbl, rowno, natts, slot);
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
static TM_Result write_result(Relation rel, uint64 rowno, int rc,
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
                         TM_FailureData *tmfd) {
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
  return write_result(rel, rowno, rc, tmfd, "update a row");
}

/*
 * Delete the row of rel at tid, with its entry in every index.  The row is
 * kept as it was, for RETURNING to fetch.
 */
TM_Result smstore_delete(Relation rel, ItemPointer tid, TM_FailureData *tmfd) {
  sm_tbl_t *tbl = table_to_write(rel);
  uint64 rowno = smstore_tid_to_rowno(tid);
  std::string old;
  int rc = waiting([&] { return sm_tbl_delete(tbl, rowno, &old); });

  if (rc == 0)
    keep_row(RelationGetRelid(rel), rowno, old.data(), old.size());
  return write_result(rel, rowno, rc, tmfd, "delete a row");
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
  slot->tts_tableOid = oid;
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
    tuple_into_slot(tbl, rowno, tuple, len, slot->tts_tupleDescriptor->natts,
                    slot);
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
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len,
                    slot->tts_tupleDescriptor->natts, slot);
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
  tuple_into_slot(tbl, rowno, latest.data(), latest.size(),
                  slot->tts_tupleDescriptor->natts, slot);
  /* Copied out of latest, which ends with this call. */
  ExecMaterializeSlot(slot);
  return TM_Ok;
}
