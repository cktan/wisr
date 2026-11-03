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
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "access/detoast.h"
#include "access/rowkey.h"
#include "access/smstore.h"
#include "catalog/objectaddress.h"
#include "access/sysattr.h"
#include "utils/datum.h"
#include "access/xact.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "catalog/catalog.h"
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
#include "parser/parse_utilcmd.h"
#include "commands/sequence.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lockdefs.h"
#include "storage/proc.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_constraint_d.h"
#include "commands/trigger.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/fmgrprotos.h"
#include "utils/datadir.h"
#include "utils/session_thread.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "utils/pg_global.h"
#include "utils/acl.h"
#include "utils/numeric.h"
#include "utils/portal.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
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
#include "utils/timestamp.h"

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

/* An index CREATE INDEX made: its table, its oid, its name, whether unique. */
struct made_index_t {
  uint64_t table;
  uint64_t index;
  std::string name;
  bool unique;
  /* Its key columns' 0-based places, -1 for an expression. */
  std::vector<int32_t> keys;
  std::string eval; /* its expressions and predicate, or empty */
};

/*
 * What a thread keeps to evaluate a partial or an expression index
 * (smstore_index_eval()): its predicate and expressions made ready, in a
 * memory context under the thread's TopMemoryContext, which goes with it.
 */
struct index_eval_t {
  MemoryContext cxt; /* what it holds is in it */
  EState *estate;
  ExprState *pred; /* NULL: no predicate */
  std::vector<ExprState *> exprs;
  std::vector<int16> typlens; /* each expression's type's */
  std::vector<bool> typbyvals;
  TupleTableSlot *slot; /* the row, virtual, as sm shows it */
};

/* A view this transaction locked (lock_view()): in mode, at level. */
struct held_view_t {
  Oid oid;
  sm_lockmode_t mode;
  int level;
};

/*
 * A pg_statistic row this session made from sm's statistics, kept as its
 * bytes: good while sm hands out the statistics it was made from -- an
 * ANALYZE publishes new ones -- and the column's type and collation are
 * still these.  Holding the statistics keeps their address from being
 * reused while the entry is here.  Empty bytes: there was no row.
 */
struct statistic_entry_t {
  std::shared_ptr<const sm_stats_t> stats;
  Oid typid;
  Oid coll;
  std::string bytes;
  /* The catalog's epoch it was made at, UINT64_MAX for none. */
  uint64_t epoch;
};

/* At most this many rows are kept; past it, they are forgotten. */
static constexpr size_t statistic_cache_max = 1024;

/*
 * A table's or an index's estimate for the planner, as this session last
 * made it (smstore_estimate_size(), smstore_estimate_index_size()): making
 * one reads every sorted file's and memtable's size, which a plan of one
 * row by its key would spend more on than on the row.  Good for
 * size_cache_age while the catalog's epoch is the one it was made at
 * (sm_cat_epoch(), SCHEMACACHE.md section 4.4): a DDL, an ANALYZE and an
 * index made valid move it.
 */
struct estimate_entry_t {
  uint64_t epoch;
  TimestampTz at;
  bool found;       /* false: plan it as never analyzed */
  double pages;
  double tuples;    /* a table's rows; an index's entries at its analyze */
  double live_rows; /* an index's: its table's rows at the analyze */
};

/*
 * What a name means in a namespace, as sm last said (smstore_relation_oid()):
 * a table, an index with its table, a view or a sequence, or nothing.
 * Good while the catalog's epoch is the one it was looked up at
 * (sm_cat_epoch(), SCHEMACACHE.md section 4.1).
 */
struct name_entry_t {
  Oid oid;   /* InvalidOid: nothing by that name */
  Oid table; /* an index's table, else InvalidOid */
};

/* How long a size is good for, in ms: a plan's estimate may be this stale. */
static constexpr int size_cache_age = 10000;

/*
 * This session's state in sm: the tables its transaction opened, closed as
 * it ends: tcb->smstore.
 */
struct smstore_state_t {
  open_tables_t open_tables;
  /* The pg_statistic rows it made, by relation oid << 16 | column. */
  std::unordered_map<uint64_t, statistic_entry_t> statistics;
  /* The estimates it made, by table oid << 32 | index oid, 0 for the heap. */
  std::unordered_map<uint64_t, estimate_entry_t> estimates;
  /* The names it looked up, by namespace and name, and the epoch of all. */
  std::map<std::pair<Oid, std::string>, name_entry_t> names;
  uint64_t names_epoch = 0;
  /* The epoch each relation's cached version was last checked at. */
  std::unordered_map<Oid, uint64_t> checked;
  /* The epoch each index was last found usable at (smstore_index_usable). */
  std::unordered_map<Oid, uint64_t> usable;
  /* The views it locked, as a table's lock is its handle's (open_tables). */
  std::vector<held_view_t> held_views;
  /* sm's namespaces' epoch when this session last flushed them. */
  uint64_t nsp_epoch = 0;
  /* The indexes this transaction made, and those its commit is to wait on. */
  std::vector<made_index_t> made_indexes;
  std::vector<made_index_t> built_after_commit;
  /* The indexes this thread has evaluated rows for, by oid. */
  std::unordered_map<uint64_t, index_eval_t> evals;
  /*
   * The error the evaluator last failed with, in eval_error_cxt, raised
   * again when sm's call that asked it fails with it (sm_error()).  The
   * context is reset as the next is kept.
   */
  ErrorData *eval_error = nullptr;
  MemoryContext eval_error_cxt = nullptr;
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
 * The row the statement keeps, to be fetched by its key again: one a write
 * found changed by a commit since the statement's snapshot, then its newest
 * version, which smstore_lock_row() read; or one just deleted, as it was.
 * The statement's own read of the key would give neither.  kept_key is the
 * key encoded (sm_tbl_row_key()), kept_tuple views the row pinned in
 * kept_ref, and is NULL when the row is yet to be read; kept_oid is
 * InvalidOid when none is kept.
 */
static thread_local /*LOCAL*/ Oid kept_oid = InvalidOid;

/*
 * The table a bulk load is begun on (smstore_load()), InvalidOid when none
 * is.  sm ends a load with its transaction, so this is forgotten then.
 */
static thread_local /*LOCAL*/ Oid load_oid = InvalidOid;
static thread_local /*LOCAL*/ std::string kept_key;
static thread_local /*LOCAL*/ const char *kept_tuple = NULL;
static thread_local /*LOCAL*/ Size kept_len = 0;
static thread_local /*LOCAL*/ sm_pin_ref_t kept_ref = {};

/*
 * The scans open in the statement, for a fetch, write or lock of a row by
 * its tid to find the one whose cursor is on it (scan_at()), and take the
 * row's key, and the row, from there: no read of the _id entry, in a table
 * with a primary key (../sm/CLUSTER.md section 6).  An ended scan leaves;
 * the list is forgotten at a statement's start, a subtransaction's end and
 * the transaction's, so a scan an error freed is never looked at, and one
 * forgotten while still open is only not found.
 */
static thread_local /*LOCAL*/ std::vector<smstore_scan_t *> open_scans;

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
  case SM_ERR_MOVED:
    return ERRCODE_T_R_SERIALIZATION_FAILURE;
  default:
    return sm_retryable() == 1 ? ERRCODE_T_R_SERIALIZATION_FAILURE
                               : ERRCODE_INTERNAL_ERROR;
  }
}

/* Raise the error sm left, saying what was being done. */
static void sm_error(const char *what) {
  ErrorData *eval_error = tcb->smstore->eval_error;

  /*
   * The evaluator's error, if sm's call failed with it: PostgreSQL's, as
   * the expression raised it (smstore_index_eval()).
   */
  if (NULL != eval_error && SM_ERR_INVALID == sm_errnum() &&
      0 == strcmp(sm_errmsg(), eval_error->message)) {
    tcb->smstore->eval_error = NULL;
    ReThrowError(eval_error);
  }
  /* PostgreSQL's words for a deadlock, which clients and tests look for. */
  if (sm_errnum() == SM_ERR_DEADLOCK)
    ereport(ERROR,
            (errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
             errmsg("deadlock detected"),
             errdetail("This transaction was chosen to end a cycle of waits "
                       "while it tried to %s.",
                       what)));
  /*
   * A row a concurrent update moved to another key, its primary key
   * changed: it is not followed there (../sm/ROWKEY.md section 2.3), as
   * PostgreSQL follows no row moved to another partition.
   */
  if (sm_errnum() == SM_ERR_MOVED)
    ereport(ERROR,
            (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
             errmsg("could not serialize access due to concurrent update"),
             errdetail("The row's primary key was changed by a concurrent "
                       "update while this transaction tried to %s.",
                       what)));
  /*
   * A scan begun inside a savepoint, such as a cursor's inner scan begun
   * again there, which a rollback to it closed: PostgreSQL would go on.
   */
  if (sm_errnum() == SM_ERR_SCANCLOSED)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("could not %s: cannot recreate the old view of a scan "
                    "closed by a rollback to a savepoint",
                    what),
             errdetail("The scan began after the savepoint was set.")));
  ereport(ERROR, (errcode(sm_sqlstate()),
                  errmsg("could not %s: %s", what, sm_errmsg())));
}

/*
 * Call f, a call into sm that may wait for another transaction, until it
 * does not fail on an interrupted wait.
 * 1. Call f.  An interrupt in this thread's wait mask ends sm's wait, with
 *    SM_ERR_INTERRUPTED (miscinit.c sets the mask).
 * 2. Return what f returned, unless it failed with SM_ERR_INTERRUPTED.
 * 3. Call CHECK_FOR_INTERRUPTS().  It acts on the interrupt, raising the
 *    error a cancel, an end or a timeout calls for.
 * 4. Go to step 1.  An interrupt that calls for nothing, or that must wait
 *    until interrupts are allowed, has been taken, so the call waits again.
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
  /* A scan of the subtransaction's may be freed without its end. */
  open_scans.clear();
}

/* Close the tables this transaction opened: a handle is good for it only. */
static void close_tables(void) {
  for (auto &[oid, handles] : open_tables())
    for (open_table_t &ot : handles)
      sm_tbl_close(ot.tbl);
  open_tables().clear();
  tcb->smstore->held_views.clear();
  savepoints = NIL;
  open_scans.clear();
  /* The kept row's pin goes with the transaction, which releases it. */
  kept_oid = InvalidOid;
  kept_tuple = NULL;
  kept_ref = {};
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
  open_scans.clear();
  /* A row is kept for the statement that wrote it, and no longer. */
  sm_unpin(&kept_ref);
  kept_oid = InvalidOid;
  kept_tuple = NULL;
}

/*
 * The client's statement has ended: release every pin the session holds,
 * as no row the statement pinned is wanted after it, so that one an error
 * left behind lasts no longer than its statement.  Not while a cursor is
 * open: a FETCH leaves its plan's slots holding rows the next FETCH may
 * read, a nested loop's outer row (../sm/PIN.md section 6.4).
 */
void smstore_statement_end(void) {
  sm_session_t *s = datadir_sm_session();

  if (NULL == s || !ThereAreNoReadyPortals())
    return;
  /* The kept row is the statement's, and its pin goes with the rest. */
  kept_oid = InvalidOid;
  kept_tuple = NULL;
  kept_ref = {};
  sm_release_all_pins(s);
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
 * The running statement's view in sm, its snapshot and the transaction's
 * writes it sees, for a cursor's later runs to read at
 * (smstore_view_resume()), as its query's snapshot is PostgreSQL's
 * (sm_stmt_save).  Freed by smstore_view_free().
 */
void *smstore_view_save(void) {
  sm_stmt_t *view;

  if (sm_stmt_save(xact_session(), &view) != 0)
    sm_error("save a statement's view");
  return view;
}

/*
 * Read at view inside the running statement until smstore_view_end() of
 * what this returns, the running statement put aside (sm_stmt_resume).
 */
void *smstore_view_resume(void *view) {
  sm_stmt_t *outer;

  if (sm_stmt_resume(xact_session(), (sm_stmt_t *)view, &outer) != 0)
    sm_error("resume a statement's view");
  return outer;
}

/*
 * Put back the statement smstore_view_resume() put aside, and free it.
 * Raises nothing, as it runs while an error unwinds too: one whose
 * transaction sm has aborted has no statement to put back.
 */
void smstore_view_end(void *outer) {
  sm_stmt_end(datadir_sm_session(), (sm_stmt_t *)outer);
}

/* Free a view smstore_view_save() made; NULL is a no-op. */
void smstore_view_free(void *view) { sm_stmt_free((sm_stmt_t *)view); }

/*
 * A decimal column with no precision is the widest of its sm type at three
 * places: numeric(17, 3) for decimal64, numeric(36, 3) for decimal128
 * (DECIMAL.md section 2.3), as sm needs a scale to store at.
 */
#define DECIMAL64_DEFAULT_TYPMOD (((17 << 16) | 3) + VARHDRSZ)
#define DECIMAL128_DEFAULT_TYPMOD (((36 << 16) | 3) + VARHDRSZ)

/* The typmod a decimal column of typid stores at: typmod, or its default. */
static int32 numeric_typmod(Oid typid, int32 typmod) {
  if (typmod >= (int32)VARHDRSZ)
    return typmod;
  return typid == DECIMAL64OID ? DECIMAL64_DEFAULT_TYPMOD
                               : DECIMAL128_DEFAULT_TYPMOD;
}

/* The precision of a decimal column's typmod, given (numeric_typmod()). */
static int numeric_precision(int32 typmod) {
  return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

/* The scale of a decimal column's typmod, given. */
static int numeric_scale_of(int32 typmod) {
  return (typmod - VARHDRSZ) & 0x7ff;
}

/*
 * sm's datum for i, an integer at the scale of a column of the decimal typid
 * and typmod, given: i shifted past the scale's bits, 5 by value for
 * decimal64, 6 in 16 bytes of buf for decimal128 (sm/sm2/TYPE.md section
 * 3.4).
 */
static sm_datum_t numeric_pack(__int128 i, Oid typid, int32 typmod,
                               uint8 *buf) {
  int bits = typid == DECIMAL64OID ? 5 : NUMERIC_SCALE_BITS;
  __int128 d = i * ((__int128)1 << bits) + numeric_scale_of(typmod);

  if (typid == DECIMAL64OID)
    return (sm_datum_t)(int64)d;
  memcpy(buf, &d, 16);
  return (sm_datum_t)buf;
}

/* Whether typid is one of the decimals (DECIMAL.md section 2.3). */
static bool is_decimal(Oid typid) {
  return typid == DECIMAL64OID || typid == DECIMAL128OID;
}

/*
 * sm's datum for num, of a column of the decimal typid and typmod: the
 * value rounded to the column's scale and checked against its precision,
 * as an assignment is, a decimal64's word or a pointer to a decimal128's,
 * which sm stores as they are.
 */
static sm_datum_t numeric_to_sm(Datum num, Oid typid, int32 typmod) {
  int32 t = numeric_typmod(typid, typmod);

  if (typid == DECIMAL64OID)
    return (sm_datum_t)DirectFunctionCall2(decimal64, num, Int32GetDatum(t));
  return (sm_datum_t)DirectFunctionCall2(numeric, num, Int32GetDatum(t));
}

/*
 * sm's datum for a scan's bound num, of the decimal typid, on a column of
 * the decimal coltypid and typmod: the column's integer at or below it, or
 * with upper at or above it, kept within the column's precision.  So the
 * bound takes in every value the key's own would, and the keys, checked
 * again on each row, decide.
 */
static sm_datum_t numeric_bound(Datum num, Oid typid, Oid coltypid,
                                int32 typmod, bool upper, uint8 *buf) {
  int32 t = numeric_typmod(coltypid, typmod);
  int s = numeric_scale_of(t), vs;
  int p = numeric_precision(t);
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
  return numeric_pack(i, coltypid, t, buf);
}

/*
 * Whether the TOAST pointer val is the one old, the tuple of a row of tbl,
 * holds for its column at ordinal: what an update that leaves the column
 * alone gives back, which sm keeps without a write.
 */
static bool own_pointer(sm_tbl_t *tbl, const std::string_view *old,
                        int ordinal, sm_datum_t val) {
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
                                   const std::string_view *old) {
  sm_datum_t *out = NULL;

  for (int i = 0; i < natts && i < desc->natts; i++) {
    Form_pg_attribute att = TupleDescAttr(desc, i);
    sm_datum_t val;

    if (isnull[i])
      continue;
    if (is_decimal(att->atttypid))
      val = numeric_to_sm(values[i], att->atttypid, att->atttypmod);
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
 * sm's type code for a column of type typid (sm2/TYPE.md section 3): each
 * decimal is sm's of its width, whatever its precision.
 */
static uint8_t type_code(Oid typid) {
  switch (typid) {
  case DECIMAL64OID:
    return 17;
  case DECIMAL128OID:
    return 18;
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

/*
 * Name and type the key columns of an index whose eval is eval that are
 * expressions, those of attnums, nkeys of them, that are 0: in key order,
 * each the next of its expressions, named as CREATE INDEX named it.
 */
static void index_expr_columns(const char *eval, const int16 *attnums,
                               int nkeys, char **names, Oid *typids,
                               int32 *typmods) {
  List *exprs, *pred, *colnames;
  int e = 0;

  smstore_index_eval_parts(eval, &exprs, &pred, &colnames);
  for (int k = 0; k < nkeys; k++) {
    if (0 != attnums[k])
      continue;
    Node *expr = (Node *)list_nth(exprs, e);

    names[k] = pstrdup(strVal(list_nth(colnames, e)));
    typids[k] = exprType(expr);
    typmods[k] = exprTypmod(expr);
    e++;
  }
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
  if (!x.eval.empty()) {
    r->eval = pstrdup(x.eval.c_str());
    index_expr_columns(r->eval, r->attnums, nkeys, r->attnames, r->typids,
                       r->typmods);
  }
  for (int k = 0; k < nkeys; k++) {
    r->desc[k] = x.key[k].desc;
    r->nulls_first[k] = x.key[k].nulls_first;
  }
  /* So the index can be opened by its oid alone (RelationBuildDesc()). */
  remember_index(r->oid, r->table);
}

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
 * The relation name means in nspoid, from sm: a table, an index, whose
 * table goes in *table, a view or a sequence; InvalidOid if none.
 */
static Oid lookup_relation_oid(const char *name, Oid nspoid, Oid *table) {
  uint64_t oid, idx_table;

  *table = InvalidOid;
  switch (sm_cat_table_oid(session(), sm_nspoid(nspoid), name, &oid)) {
  case 0:
    return (Oid)oid;
  case 1:
    break;
  default:
    sm_error("look up a table");
  }
  switch (sm_cat_index_oid(session(), sm_nspoid(nspoid), name, &idx_table,
                           &oid)) {
  case 0:
    /* So the index can be opened by its oid alone (RelationBuildDesc()). */
    remember_index((Oid)oid, (Oid)idx_table);
    *table = (Oid)idx_table;
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

/*
 * The oid of the relation named name in nspoid, a table, an index, a view
 * or a sequence, or InvalidOid if there is none: this session's
 * name_entry_t while the catalog's epoch holds, else sm's, kept as one.
 */
Oid smstore_relation_oid(const char *name, Oid nspoid) {
  /* No namespace holds nothing: InvalidOid is SM_NSP_TEMP's number. */
  if (!OidIsValid(nspoid))
    return InvalidOid;
  /* Read before the lookup, so a publish meanwhile retires what it finds. */
  uint64_t epoch = sm_cat_epoch(session());
  auto &names = tcb->smstore->names;

  if (UINT64_MAX != epoch && tcb->smstore->names_epoch != epoch) {
    names.clear();
    tcb->smstore->names_epoch = epoch;
  }
  std::pair<Oid, std::string> key(nspoid, name);

  if (UINT64_MAX != epoch) {
    auto it = names.find(key);

    if (names.end() != it) {
      if (OidIsValid(it->second.table))
        remember_index(it->second.oid, it->second.table);
      return it->second.oid;
    }
  }
  name_entry_t found;

  found.oid = lookup_relation_oid(name, nspoid, &found.table);
  /* With DDL of the transaction's own, sm's answer is the overlay's. */
  if (UINT64_MAX != epoch)
    names[key] = found;
  return found.oid;
}

static void drop_table(RangeVar *rv, bool missing_ok, bool cascade,
                       const std::vector<Oid> &dropping);
static void drop_sequence(RangeVar *rv, bool missing_ok);
static void check_owner(Oid oid, const char *name, bool schema_too);
static bool may_maintain(Oid oid, const char *name, const char *what,
                         bool named);
static void change_owner(Oid relid, const char *relname, RoleSpec *spec);

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
  /* Its owner, the role CREATE SCHEMA runs as, is pg_global's (ROLE.md). */
  pg_global_set_owner(pg_global_dbid(MyDatabaseId), NamespaceRelationId,
                      (Oid)oid, GetUserId(), false);
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
 * Drop the schema nspname, nspoid, from sm, in this transaction.
 * 1. With cascade, drop its views, with the views elsewhere that read them.
 * 2. With cascade, drop its tables, each with its indexes and the sequences
 *    its columns own.
 * 3. With cascade, drop its other sequences.
 * 4. Drop the schema.  Without cascade, a schema that holds any fails.
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

  /* Its owner's entry goes with it, after the commit (ROLE.md). */
  pg_global_forget(pg_global_dbid(MyDatabaseId), nspoid);
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
    /* Its owner may drop it, and what is in it, as PostgreSQL's may. */
    if (!object_ownercheck(NamespaceRelationId, nspoid, GetUserId()))
      aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA, name);
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
 * Whether raw is a default ONUPDATE.md §2 takes beside an update default:
 * a constant, cast or not, or CURRENT_TIMESTAMP, LOCALTIMESTAMP or
 * CURRENT_DATE with no precision.
 */
static bool update_default_kind(const Node *raw) {
  while (IsA(raw, TypeCast))
    raw = ((const TypeCast *)raw)->arg;
  if (IsA(raw, A_Const))
    return true;
  if (!IsA(raw, SQLValueFunction))
    return false;
  SQLValueFunctionOp op = ((const SQLValueFunction *)raw)->op;
  return SVFOP_CURRENT_TIMESTAMP == op || SVFOP_LOCALTIMESTAMP == op ||
         SVFOP_CURRENT_DATE == op;
}

/* col's update default, its CONSTR_DEFAULT marked 'u', or NULL. */
static const Constraint *update_default_of(const ColumnDef *col) {
  foreach_node(Constraint, c, col->constraints) {
    if (CONSTR_DEFAULT == c->contype && 'u' == c->generated_when)
      return c;
  }
  return NULL;
}

/*
 * col's update default as sm keeps it, as column_default() gives its
 * DEFAULT, or NULL with none (ONUPDATE.md §6).  With one, it and
 * raw_default, the column's insert default, must be of
 * update_default_kind().
 */
static const char *column_update_default(ColumnDef *col, Node *raw_default,
                                         Oid typid, int32 typmod) {
  const Constraint *update = update_default_of(col);

  if (NULL == update)
    return NULL;
  if (!update_default_kind(update->raw_expr) ||
      !update_default_kind(raw_default))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
             errmsg("column \"%s\" has an update default, so its defaults "
                    "must be constants, CURRENT_TIMESTAMP, LOCALTIMESTAMP "
                    "or CURRENT_DATE",
                    col->colname)));
  ParseState *pstate = make_parsestate(NULL);
  Node *expr =
      cookDefault(pstate, update->raw_expr, typid, typmod, col->colname, 0);

  free_parsestate(pstate);
  return nodeToString(expr);
}

/*
 * Refuse a relation named relname in nspoid if nspoid is a system schema,
 * pg_catalog or pg_toast, unless allow_system_table_mods: as PostgreSQL's
 * heap_create(), since every search path looks in pg_catalog first, so one
 * made there would hide its name's relation in any schema, in every session.
 * A name a catalog has, compiled into digres and not in sm, is taken first,
 * as heap_create_with_catalog() finds it before heap_create() refuses.
 */
static void check_system_namespace(const char *relname, Oid nspoid) {
  if (allowSystemTableMods ||
      !(IsCatalogNamespace(nspoid) || IsToastNamespace(nspoid)))
    return;
  if (OidIsValid(get_relname_relid(relname, nspoid)))
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" already exists", relname)));
  ereport(ERROR,
          (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
           errmsg("permission denied to create \"%s.%s\"",
                  get_namespace_name(nspoid), relname),
           errdetail("System catalog modifications are currently disallowed.")));
}

static void check_column_name(const char *name);

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
  check_system_namespace(stmt->relation->relname, nspoid);
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

    check_column_name(col->colname);
    /*
     * A name given twice, by the list or by its LIKE clauses, PostgreSQL's
     * MergeAttributes() refuses before sm would.
     */
    for (const sm_column_spec_t &c : columns)
      if (strcmp(c.name, col->colname) == 0)
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
                        errmsg("column \"%s\" specified more than once",
                               col->colname)));
    if (col->collClause != NULL || col->cooked_default != NULL ||
        col->generated != 0 || !only_taken(col->constraints))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("column \"%s\" asks for what CREATE TABLE "
                             "does not support yet",
                             col->colname)));
    typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
    /* A numeric with no precision is its type's default one, kept in sm. */
    if (is_decimal(typid))
      typmod = numeric_typmod(typid, typmod);
    columns.push_back({col->colname, type_code(typid), typmod,
                       column_not_null(stmt, col),
                       column_default(col, typid, typmod)});
    columns.back().on_update_expr =
        column_update_default(col, col->raw_default, typid, typmod);
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
  /* Its owner, the current role, is pg_global's; a temporary one's not. */
  pg_global_set_owner(pg_global_dbid(MyDatabaseId), RelationRelationId,
                      (Oid)oid, GetUserId(), temp);
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
  check_system_namespace(name, nspoid);
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
  /* Its owner, the current role, is pg_global's (ROLE.md). */
  pg_global_set_owner(pg_global_dbid(MyDatabaseId), RelationRelationId,
                      (Oid)oid, GetUserId(), false);
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
  check_owner(oid, rv->relname, true);
  sm_session_t *s = xact_session();

  /* Its owner's entry goes with it, after the commit (ROLE.md). */
  pg_global_forget(pg_global_dbid(MyDatabaseId), oid);
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
  if (NULL == def)
    sm_error("define a table");
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
    ix->tree = x.tree;
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
    ix->eval = x.eval.empty() ? NULL : pstrdup(x.eval.c_str());
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
 * The update defaults of the table relid's columns (ONUPDATE.md §6), each
 * the expression its column's type takes, in a palloc'd array by attnum
 * less one, NULL where a column has none; or NULL if none has one.
 */
Node **smstore_update_defaults(Oid relid) {
  sm_tbl_t *tbl = open_table(relid, SM_LOCKMODE_IS);
  Node **out = NULL;

  if (tbl == NULL)
    return NULL;
  const sm_table_def_t *def = sm_tbl_def(tbl);

  if (NULL == def)
    sm_error("define a table");
  for (size_t i = 0; i < def->columns.size(); i++) {
    const sm_column_def_t &c = def->columns[i];

    if (!c.has_on_update || c.dropped)
      continue;
    if (NULL == out)
      out = (Node **)palloc0(def->columns.size() * sizeof(Node *));
    out[i] = (Node *)stringToNode(c.on_update_expr.c_str());
  }
  return out;
}

/*
 * The sequence the identity column attnum (1-based) of the table relid
 * takes its values from, or InvalidOid if it is not one.
 */
Oid smstore_identity_sequence(Oid relid, AttrNumber attnum) {
  sm_tbl_t *tbl = open_table(relid, SM_LOCKMODE_IS);

  if (tbl == NULL)
    return InvalidOid;
  const sm_table_def_t *def = sm_tbl_def(tbl);

  if (NULL == def)
    sm_error("define a table");
  if (attnum < 1 || attnum > (AttrNumber)def->columns.size())
    return InvalidOid;
  const sm_column_def_t &c = def->columns[attnum - 1];
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
 * The name CREATE INDEX ... USING gives the kind of the index indexoid of
 * the table table, or NULL if the table has no such index.
 */
const char *smstore_index_am(Oid table, Oid indexoid) {
  smstore_table_t *desc = smstore_table_desc(table);

  for (int i = 0; desc != NULL && i < desc->nindexes; i++)
    if (desc->indexes[i].oid == indexoid)
      return desc->indexes[i].tree ? SMSTORE_INDEX_TREE : SMSTORE_INDEX_LSM;
  return NULL;
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
 * Raise the error of a write to rel that sm refused, a unique index holding
 * the key already: PostgreSQL's, naming the index sm names, with the key of
 * the row in slot, all of whose columns are read; sm's words if it names no
 * index, or there is no row, as at a bulk load's end.
 */
[[noreturn]] static void duplicate_write(Relation rel, TupleTableSlot *slot) {
  uint64_t indexoid;
  Datum values[INDEX_MAX_KEYS];
  bool isnull[INDEX_MAX_KEYS];

  sm_errdup(&indexoid);
  if (indexoid == 0 || slot == NULL)
    ereport(ERROR,
            (errcode(ERRCODE_UNIQUE_VIOLATION),
             errmsg("duplicate key value violates a unique index on \"%s\"",
                    RelationGetRelationName(rel)),
             errdetail("%s", sm_errmsg())));
  Relation index = index_open((Oid)indexoid, AccessShareLock);
  EState *estate = CreateExecutorState();

  /* Its key columns, an expression's evaluated on the row. */
  GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
  FormIndexDatum(BuildIndexInfo(index), slot, estate, values, isnull);
  char *key = BuildIndexValueDescription(index, values, isnull);

  ereport(ERROR,
          (errcode(ERRCODE_UNIQUE_VIOLATION),
           errmsg("duplicate key value violates unique constraint \"%s\"",
                  RelationGetRelationName(index)),
           key ? errdetail("Key %s already exists.", key) : 0,
           errtableconstraint(rel, RelationGetRelationName(index))));
  pg_unreachable();
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
      duplicate_write(rel, slot);
    sm_error("insert a row");
  }
  slot->tts_rowno = rowno;
}

static void tuple_into_slot(sm_tbl_t *tbl, uint64_t rowno, const void *tuple,
                            size_t len, int natts, TupleTableSlot *slot);

/*
 * Insert the slot's row into rel, a table in sm, unless a row already has
 * its key in one of the unique indexes arbiters lists by oid, or in any
 * unique index of rel if the list is empty: INSERT ... ON CONFLICT.  True if
 * one has, with that row's key in *conflict, its values palloc'd, and
 * nothing written; false with the row inserted, its tid in the slot.  This
 * waits for another session's insert of the key that is not yet committed, and
 * above READ COMMITTED fails on a key taken since the snapshot.  The row found
 * is not locked, nor its key.
 */
bool smstore_insert_arbiter(Relation rel, TupleTableSlot *slot, List *arbiters,
                            RowKey *conflict) {
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
  std::string found;
  rc = waiting([&] {
    return sm_tbl_insert_arbiter(tbl, row, slot->tts_isnull, slot->tts_nvalid,
                                 oids.data(), oids.size(), &rowno, &found);
  });
  if (rc < 0) {
    if (sm_errnum() == SM_ERR_DUPKEY)
      duplicate_write(rel, slot);
    sm_error("insert a row");
  }
  if (rc == 1) {
    /* Its key, from the row found, copied: the row goes with found. */
    TupleTableSlot *other =
        MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
    RowKey key;

    tuple_into_slot(tbl, rowno, found.data(), found.size(),
                    RelationGetDescr(rel)->natts, other);
    RowKeyFromSlot(rel, other, &key);
    RowKeyCopy(rel, &key, conflict);
    ExecDropSingleTupleTableSlot(other);
    return true;
  }
  slot->tts_rowno = rowno;
  return false;
}

/*
 * Raise the error of a bulk load's call that failed, on rel: of the row in
 * slot, or of none, as at the load's end.
 */
static void load_error(Relation rel, TupleTableSlot *slot, const char *what) {
  if (sm_errnum() == SM_ERR_DUPKEY)
    duplicate_write(rel, slot);
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
      load_error(rel, NULL, "begin a bulk load");
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
      load_error(rel, slot, "load a row");
    slot->tts_rowno = rowno;
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
    load_error(rel, NULL, "end a bulk load");
}

/*
 * The most rows a scan reads ahead in one call (../sm/BATCHREAD.md): enough
 * to share a data block's lookups among rows next to each other, few enough
 * that a scan stopped early has read little it did not use.
 */
static constexpr int read_batch_max = 8;

/*
 * A scan of a table in sm: the table, and sm's cursor over its rows; or,
 * over one of its indexes, sm's cursor over the index's entries and the scan
 * keys every row it reads must meet, with each key's column in the table.
 * A scan of the table's primary key is a cursor over its rows, in the key's
 * order (../sm/CLUSTER.md section 6).
 */
struct smstore_scan_t {
  sm_tbl_t *tbl;
  /* The table's oid, each row's tableoid, as WHERE CURRENT OF checks. */
  Oid relid;
  sm_rel_scan_t *cursor;
  sm_idx_scan_t *icursor;
  /*
   * A cursor over rows' last row, in the cursor's pin: a sequential or a
   * primary key scan's.
   */
  sm_pin_ref_t row_ref;
  ScanKey keys;
  int nkeys;
  AttrNumber *keycols;
  /* Which keys the scan's bounds hold of every row it reads (keys_met()). */
  bool *bounded;
  /* How many keys are not bounded: each row is checked against these. */
  int nchecked;
  /*
   * A key on an expression column, which no column of the row holds: the
   * executor checks each row against the scan's quals (smstore_index_
   * recheck()), and keys_met() leaves it alone.
   */
  bool recheck;
  /* An index scan's: its index, and its last entry's value. */
  Oid index;
  Form_pg_index form;
  const void *value;
  size_t len;
  /* smstore_index_getentry()'s room for an entry's value. */
  sm_datum_t *entry_values;
  bool *entry_isnull;
  size_t entry_ncols;
  /*
   * An index or a fetch scan's pin on the row it read last by rowno, which
   * its slot holds: released by the next read into it, the scan's end, or
   * the transaction's (../sm/ROW.md section 4).
   */
  sm_pin_ref_t ref;
  /*
   * smstore_scan_row()'s row read ahead, past the rowno it was asked for,
   * kept for a later call in the cursor's pin: valid until the cursor's next
   * call.  at_end once the cursor has none left.
   */
  bool ahead;
  bool at_end;
  uint64_t ahead_rowno;
  sm_pin_ref_t ahead_ref;
  /*
   * smstore_sample_slot()'s slot, has_slot once there is one, and its end, a
   * key of the table's past its relid, palloc'd; none for the table's last
   * slot.  smstore_scan_nth()'s count of the rows the cursor read, and the
   * one read ahead's place.
   */
  bool has_slot;
  uint64_t slot;
  char *slot_end;
  size_t slot_end_len;
  uint64_t nread;
  uint64_t ahead_nth;
  /*
   * The row the scan handed out last, while its cursor is on it
   * (scan_at()): its rowno, and whether it is pinned, in row_ref for a
   * cursor over rows, else in ref.  Its key is the cursor's.
   */
  bool cur;
  bool cur_pinned;
  uint64_t cur_rowno;
  /*
   * A primary key scan with an equality on each of the key's columns: the
   * one row they name, read at the scan's start into ref, with no cursor;
   * point_row while it is there and not yet handed out.  Its key, encoded,
   * palloc'd, is the one the cursor would be on.
   */
  bool point;
  bool point_row;
  char *point_key;
  size_t point_len;
  /*
   * An index scan's bounds, those of the probe open when arrays give
   * columns values (../INLIST.md): narrays such columns, each column's
   * values' bounds sorted in the index's order, and the one in use; the
   * next probe takes the next combination.  empty when an array has no
   * value, and the scan no row.
   */
  sm_bound_t lo, hi;
  sm_datum_t *lo_vals, *hi_vals;
  int narrays;
  int *arr_col;
  int *arr_n;
  int *arr_at;
  sm_datum_t **arr_lo, **arr_hi;
  bool empty;
  /* Each array key's elements but its nulls, for keys_met(); NULL if none. */
  Datum **elems;
  int *nelems;
  /*
   * Rows read ahead, many by one sm_tbl_read_keys() (../sm/BATCHREAD.md): a
   * secondary index's entries' rows (ra_rows), or the rows of a primary
   * key's probes (ra_point); ra_done once the entries or probes ended.  ra_n of them, ra_at
   * the next to hand out, each its rowno, its key, copied, its pin and
   * whether the transaction sees it; ra_batch the next read's size, doubling
   * to read_batch_max.  A row handed out moves its pin to ref and its key to
   * cur_key, the row the scan is on.
   */
  bool ra_rows;
  /* A tree's (../sm/BTREE.md): its rows come by a cursor, as a primary
   * key's, each rechecked against its entry by sm. */
  bool tree;
  /*
   * An index-only scan's tree of key columns alone: its cursor hands out
   * an entry with its row unread where vacuum found its leaf all visible
   * (../sm/BTREE.md section 4.1, sm_rel_scan_entries()).
   */
  bool entries_only;
  bool ra_point;
  bool ra_done;
  int ra_n;
  int ra_at;
  int ra_batch;
  uint64_t ra_rowno[read_batch_max];
  char *ra_key[read_batch_max];
  size_t ra_klen[read_batch_max];
  size_t ra_kcap[read_batch_max];
  sm_pin_ref_t ra_ref[read_batch_max];
  int ra_rc[read_batch_max];
  const char *cur_key;
  size_t cur_klen;
};

/*
 * A slot holding a row of a table in sm (../sm/ROW.md section 6): the ref
 * of the pin the row is in, deformed only as far as the executor reads it.
 * With no ref it is a virtual slot, its columns filled by whoever stored
 * them.  Not being virtual, it gets the executor's fetch steps
 * (execExpr.c), so a scan's filter deforms the columns it reads, and its
 * target list those it reads, of the rows the filter passes only.
 */
typedef struct SmRowTupleTableSlot {
  VirtualTupleTableSlot base; /* what a virtual slot keeps */
  sm_pin_ref_t ref;           /* the row's pin, or zero */
} SmRowTupleTableSlot;

/* The ref of the row slot holds, zero if none. */
static sm_pin_ref_t &slot_ref(TupleTableSlot *slot) {
  return ((SmRowTupleTableSlot *)slot)->ref;
}

/* Make a slot: nothing to set up, as a virtual one has none. */
static void sm_row_init(TupleTableSlot *slot) { TTSOpsVirtual.init(slot); }

/* Free what the slot owns, as a virtual one does. */
static void sm_row_release(TupleTableSlot *slot) {
  TTSOpsVirtual.release(slot);
}

/*
 * Empty the slot.  Its row's ref goes, not the pin, which is the reader's:
 * the scan's, or the index scan's ref.
 */
static void sm_row_clear(TupleTableSlot *slot) {
  slot_ref(slot) = {};
  TTSOpsVirtual.clear(slot);
}

/*
 * Deform the slot's row up to column natts, from where it stopped; the
 * columns before stay as they are.  A stale ref is an error: the row's pin
 * was released while the slot still held it.
 */
static void sm_row_getsomeattrs(TupleTableSlot *slot, int natts) {
  if (slot_ref(slot).pin == NULL) {
    /* As a virtual slot, every column is filled already. */
    TTSOpsVirtual.getsomeattrs(slot, natts);
    return;
  }
  if (sm_pin_deform(slot_ref(slot), slot->tts_nvalid, natts,
                    (sm_datum_t *)slot->tts_values, slot->tts_isnull) != 0)
    sm_error("read a row");
  slot->tts_nvalid = natts;
}

/* A system column: the executor serves the ones sm has (tuptable.h). */
static Datum sm_row_getsysattr(TupleTableSlot *slot, int attnum,
                               bool *isnull) {
  return TTSOpsVirtual.getsysattr(slot, attnum, isnull);
}

/* Whether this transaction wrote the row: sm keeps no such thing. */
static bool sm_row_is_current_xact_tuple(TupleTableSlot *slot) {
  return TTSOpsVirtual.is_current_xact_tuple(slot);
}

/* Every column of the slot's row, deformed now if it holds a ref. */
static void sm_row_deform_all(TupleTableSlot *slot) {
  if (slot_ref(slot).pin != NULL)
    slot_getallattrs(slot);
}

/*
 * Make the slot independent of sm's memory: every column deformed, the ref
 * dropped, and the values by reference copied into the slot's own memory,
 * as a virtual slot materializes.
 */
static void sm_row_materialize(TupleTableSlot *slot) {
  sm_row_deform_all(slot);
  slot_ref(slot) = {};
  TTSOpsVirtual.materialize(slot);
}

/* Copy src into dst, which then owns its values, as a virtual slot does. */
static void sm_row_copyslot(TupleTableSlot *dst, TupleTableSlot *src) {
  slot_ref(dst) = {};
  TTSOpsVirtual.copyslot(dst, src);
}

/* The slot's row as a heap tuple of its own. */
static HeapTuple sm_row_copy_heap_tuple(TupleTableSlot *slot) {
  sm_row_deform_all(slot);
  return TTSOpsVirtual.copy_heap_tuple(slot);
}

/* The slot's row as a minimal tuple of its own. */
static MinimalTuple sm_row_copy_minimal_tuple(TupleTableSlot *slot,
                                              Size extra) {
  sm_row_deform_all(slot);
  return TTSOpsVirtual.copy_minimal_tuple(slot, extra);
}

/* The slot type of a table in sm (smtable_slot_callbacks()). */
const TupleTableSlotOps TTSOpsSmRow = {
    .base_slot_size = sizeof(SmRowTupleTableSlot),
    .init = sm_row_init,
    .release = sm_row_release,
    .clear = sm_row_clear,
    .getsomeattrs = sm_row_getsomeattrs,
    .getsysattr = sm_row_getsysattr,
    .is_current_xact_tuple = sm_row_is_current_xact_tuple,
    .materialize = sm_row_materialize,
    .copyslot = sm_row_copyslot,
    /* It holds no heap or minimal tuple of its own, as a virtual slot. */
    .get_heap_tuple = NULL,
    .get_minimal_tuple = NULL,
    .copy_heap_tuple = sm_row_copy_heap_tuple,
    .copy_minimal_tuple = sm_row_copy_minimal_tuple,
};

/*
 * The row ref pins into slot, cleared, its tid its rowno.  A slot of sm's
 * type keeps the ref, and deforms the row as the executor reads it; another
 * is filled whole now, as a virtual tuple.  Either way a value by reference
 * points into the row, and lasts as long as the pin.
 */
static void ref_into_slot(sm_pin_ref_t ref, uint64_t rowno,
                          TupleTableSlot *slot) {
  if (slot->tts_ops == &TTSOpsSmRow) {
    slot_ref(slot) = ref;
    slot->tts_flags &= ~TTS_FLAG_EMPTY;
    slot->tts_nvalid = 0;
  } else {
    if (sm_pin_deform(ref, 0, slot->tts_tupleDescriptor->natts,
                      (sm_datum_t *)slot->tts_values, slot->tts_isnull) != 0)
      sm_error("read a row");
    ExecStoreVirtualTuple(slot);
  }
  slot->tts_rowno = rowno;
}

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
  slot->tts_rowno = rowno;
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
 * The columns of each row of rel a TID scan of plan reads, from the first:
 * up to the last its target list and quals name, or every one for a
 * reference to the whole row.  Its projection and quals fetch no more; a
 * scan with no projection names every column, as its target list is the
 * table's.  Other scans deform a row as they read it (TTSOpsSmRow).
 */
int smstore_scan_natts(const Scan *plan, Relation rel) {
  Bitmapset *attrs = NULL;

  pull_varattnos((Node *)plan->plan.targetlist, plan->scanrelid, &attrs);
  pull_varattnos((Node *)plan->plan.qual, plan->scanrelid, &attrs);
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
 * Begin a scan of every row of rel, a table in sm, each row deformed as it
 * is read: in rowno order, for a scan that moves to a rowno
 * (smstore_scan_seek()); else in its key's order, its primary key's, one
 * sorted run (../sm/CLUSTER.md section 6).
 */
smstore_scan_t *smstore_beginscan(Relation rel, bool by_rowno) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->tbl = tbl;
  scan->relid = RelationGetRelid(rel);
  if ((by_rowno ? sm_tbl_scan(tbl, 0, UINT64_MAX, &scan->cursor)
                : sm_tbl_scan_rows(tbl, &scan->cursor)) != 0)
    sm_error("scan a table");
  open_scans.push_back(scan);
  return scan;
}

/*
 * The scan's next row into slot, by its pin's ref (ref_into_slot()), its
 * tid its rowno; false at the end.  The ref, and a value by reference, is
 * good until the next row.
 */
bool smstore_getnext(smstore_scan_t *scan, TupleTableSlot *slot) {
  uint64_t rowno;

  ExecClearTuple(slot);
  slot->tts_tableOid = scan->relid;
  scan->cur = false;
  /* Into the cursor's pin, which its next row makes stale. */
  switch (sm_rel_scannxt(scan->cursor, &rowno, &scan->row_ref)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("scan a table");
  }
  scan->cur = true;
  scan->cur_pinned = true;
  scan->cur_rowno = rowno;
  ref_into_slot(scan->row_ref, rowno, slot);
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
  /* Its cursor reads ahead, so is on no row it handed out. */
  scan->cur = false;
  while (!scan->at_end && (!scan->ahead || scan->ahead_rowno < rowno)) {
    switch (sm_rel_scannxt(scan->cursor, &scan->ahead_rowno, &scan->ahead_ref)) {
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
  ref_into_slot(scan->ahead_ref, rowno, slot);
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

/*
 * The slots a sample scan of rel, a table with a primary key, cuts it
 * into: a block's bytes each, by sm's estimate of its bytes in sorted files
 * and memtables together (../sm/ROWKEY.md section 2.4); 0 if it has none.
 */
uint64 smstore_sample_slots(Relation rel) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  uint64_t disk, mem;

  if (NULL == tbl)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  if (0 != sm_tbl_size(tbl, &disk, &mem))
    sm_error("size a table");
  return Min((disk + mem + BLCKSZ - 1) / BLCKSZ, (uint64)MaxBlockNumber);
}

/*
 * Move a scan in key order (smstore_beginscan(), not by rowno) forward to
 * slot of nslots, as a sample scan does to a block it picked: the rows from
 * the key at its start to the key at its end, sm_tbl_keys_at()'s.
 * 1. Find the keys at the slot's start and at its end.  The slot after the
 *    last one starts where that one ended, its start never estimated again:
 *    sm's estimate of a memtable is sampled anew each time, and could leave
 *    a gap.
 * 2. Keep the slot's end; none for the table's last slot.
 * 3. Leave the cursor where it is at the scan's end, or for the slot after
 *    the last one.
 * 4. Keep a row read ahead at or past the slot's start, as the cursor is
 *    past it already.  Slots are taken in increasing order.
 * 5. Else move the cursor to the slot's start.
 */
void smstore_sample_slot(smstore_scan_t *scan, uint64 slot, uint64 nslots) {
  bool next = scan->has_slot && slot == scan->slot + 1;
  double fs[2] = {(double)slot / nslots, (double)(slot + 1) / nslots};
  size_t from = next ? 1 : 0, to = slot + 1 < nslots ? 2 : 1;
  std::vector<std::string> keys;
  const void *key;
  size_t len;

  keys.resize(from);
  if (from < to) {
    std::vector<std::string> found;

    if (0 != sm_tbl_keys_at(scan->tbl, fs + from, to - from, found))
      sm_error("find a table's sample block");
    keys.insert(keys.end(), found.begin(), found.end());
  }
  scan->has_slot = true;
  scan->slot = slot;
  if (scan->slot_end)
    pfree(scan->slot_end);
  scan->slot_end = NULL;
  if (2 == keys.size()) {
    scan->slot_end = (char *)palloc(Max(keys[1].size(), 1));
    memcpy(scan->slot_end, keys[1].data(), keys[1].size());
    scan->slot_end_len = keys[1].size();
  }
  if (scan->at_end || next)
    return;
  if (scan->ahead) {
    if (0 != sm_rel_scan_key(scan->cursor, &key, &len))
      sm_error("read a table's key");
    if (std::string_view((const char *)key, len) >= keys[0])
      return;
  }
  scan->ahead = false;
  if (0 != sm_rel_scan_seek_key(scan->cursor, keys[0].data(), keys[0].size()))
    sm_error("move a table's scan");
}

/*
 * The next row of the scan's slot (smstore_sample_slot()) into slot; false
 * when the slot has no more.  A row read past the slot's end is kept for
 * the next slot.
 */
bool smstore_sample_row(smstore_scan_t *scan, TupleTableSlot *slot) {
  const void *key;
  size_t len;

  ExecClearTuple(slot);
  slot->tts_tableOid = scan->relid;
  /* Its cursor reads ahead, so is on no row it handed out. */
  scan->cur = false;
  if (!scan->ahead && !scan->at_end) {
    switch (
        sm_rel_scannxt(scan->cursor, &scan->ahead_rowno, &scan->ahead_ref)) {
    case 0:
      scan->ahead = true;
      break;
    case 1:
      scan->at_end = true;
      break;
    default:
      sm_error("scan a table");
    }
  }
  if (!scan->ahead)
    return false;
  if (scan->slot_end) {
    if (0 != sm_rel_scan_key(scan->cursor, &key, &len))
      sm_error("read a table's key");
    if (std::string_view((const char *)key, len) >=
        std::string_view(scan->slot_end, scan->slot_end_len))
      return false;
  }
  scan->ahead = false;
  ref_into_slot(scan->ahead_ref, scan->ahead_rowno, slot);
  return true;
}

/*
 * The n-th row, from 1, of a scan in key order into slot, as
 * smstore_scan_row() the row at a rowno: a row read past it is kept, so
 * places asked for in increasing order read each row once at most; false if
 * the scan has fewer rows.  With no slot, only whether it has the n-th, which
 * is then kept.
 */
bool smstore_scan_nth(smstore_scan_t *scan, uint64 n, TupleTableSlot *slot) {
  if (slot) {
    ExecClearTuple(slot);
    slot->tts_tableOid = scan->relid;
  }
  /* Its cursor reads ahead, so is on no row it handed out. */
  scan->cur = false;
  while (!scan->at_end && (!scan->ahead || scan->ahead_nth < n)) {
    switch (
        sm_rel_scannxt(scan->cursor, &scan->ahead_rowno, &scan->ahead_ref)) {
    case 0:
      scan->ahead = true;
      scan->ahead_nth = ++scan->nread;
      break;
    case 1:
      scan->ahead = false;
      scan->at_end = true;
      break;
    default:
      sm_error("scan a table");
    }
  }
  if (!scan->ahead || scan->ahead_nth != n)
    return false;
  if (slot) {
    scan->ahead = false;
    ref_into_slot(scan->ahead_ref, scan->ahead_rowno, slot);
  }
  return true;
}

/* End a scan; the table stays open until the transaction ends. */
void smstore_endscan(smstore_scan_t *scan) {
  std::erase(open_scans, scan);
  sm_rel_scanend(scan->cursor);
  sm_idx_scanend(scan->icursor);
  /* The row it read last by rowno is no one's now, nor those read ahead. */
  sm_unpin(&scan->ref);
  for (int i = scan->ra_at; i < scan->ra_n; i++)
    sm_unpin(&scan->ra_ref[i]);
  for (int i = 0; i < read_batch_max; i++)
    if (scan->ra_key[i])
      pfree(scan->ra_key[i]);
  if (scan->slot_end)
    pfree(scan->slot_end);
  if (scan->point_key)
    pfree(scan->point_key);
  pfree(scan);
}

static bool table_def_now(Oid oid, sm_table_def_t &out);
static bool read_row_at(sm_tbl_t *tbl, const std::string &key, int natts,
                        TupleTableSlot *slot);
static void forget_table(Oid relid);

/*
 * Drop the foreign keys of other tables that reference the table oid,
 * named relname, as DROP TABLE ... CASCADE does, with PostgreSQL's notice;
 * without cascade, PostgreSQL's error.  One of a table in dropping, which
 * the same statement drops, is dropped without a word.  With index, the
 * index of a constraint, conname, that DROP CONSTRAINT drops, only those
 * that reference it, the table's own among them.
 */
static void drop_referencing(Oid oid, const char *relname, bool cascade,
                             const std::vector<Oid> &dropping,
                             uint64_t index = 0, const char *conname = NULL) {
  sm_table_def_t def;
  std::vector<std::pair<uint64_t, uint64_t>> drop; /* (table, fkey) */
  StringInfoData detail;
  int n = 0;

  if (!table_def_now(oid, def))
    return;
  initStringInfo(&detail);
  for (const sm_fkey_ref_t &r : def.referenced_by) {
    smstore_fkey_t fk;
    sm_table_def_t child;
    bool found = smstore_get_fkey((Oid)r.fkey, &fk);

    if (0 == index ? r.table == oid : !found || fk.ref_index != (Oid)index)
      continue;
    drop.push_back({r.table, r.fkey});
    if (std::find(dropping.begin(), dropping.end(), (Oid)r.table) !=
        dropping.end())
      continue;
    if (!found || !table_def_now((Oid)r.table, child))
      continue;
    /* A line each, as PostgreSQL's reportDependentObjects() writes them. */
    if (n++ > 0)
      appendStringInfoChar(&detail, '\n');
    appendStringInfo(&detail, "%sconstraint %s on table %s",
                     cascade ? "drop cascades to " : "", NameStr(fk.name),
                     child.name.c_str());
    if (!cascade)
      appendStringInfo(&detail, " depends on %s %s",
                       0 == index ? "table" : "index",
                       0 == index ? relname : conname);
  }
  if (n > 0 && !cascade && 0 != index)
    ereport(ERROR, (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                    errmsg("cannot drop constraint %s on table %s because "
                           "other objects depend on it",
                           conname, relname),
                    errdetail("%s", detail.data),
                    errhint("Use DROP ... CASCADE to drop the dependent "
                            "objects too.")));
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

/*
 * Drop the table rv names, from sm, in this transaction: with its indexes,
 * rows and sequence.  One that is not there is an error, or with missing_ok
 * a notice.
 */
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
  check_owner(oid, rv->relname, true);
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

  /* Its owner's entry goes with it, after the commit (ROLE.md). */
  pg_global_forget(pg_global_dbid(MyDatabaseId), oid);
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

  check_system_namespace(rv->relname, nspoid);
  sm_session_t *s = xact_session();
  sm_view_def_t def = view_def(s, query, options);
  uint64_t oid;
  ObjectAddress address;

  if (waiting([&] {
        return sm_cat_create_view(s, sm_nspoid(nspoid), rv->relname, def, &oid);
      }) != 0)
    view_error(rv->relname, "create a view");
  /* Its owner, the current role, is pg_global's; a temporary one's not. */
  pg_global_set_owner(pg_global_dbid(MyDatabaseId), RelationRelationId,
                      (Oid)oid, GetUserId(), PG_TEMP_NAMESPACE == nspoid);
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
  check_owner(oid, name, false);
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
  /* Its owner's entry goes with it, after the commit (ROLE.md). */
  pg_global_forget(pg_global_dbid(MyDatabaseId), oid);
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
    check_owner(oid, rv->relname, true);
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
 * definition.
 * 1. Raise an error if it is not a table.
 * 2. Raise an error if this transaction is using it.
 * 3. Forget its definition (forget_table()).  Every session forgets its
 *    descriptor once this one commits, and this one at its next command.
 *    This transaction's handles of it, which hold the old definition, are
 *    closed, so the next use opens one afresh.
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

  /* A type parse analysis resolved, a serial's among them, by its oid. */
  if (NIL == type->names || type->arrayBounds != NIL ||
      list_length(type->names) > 2 ||
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
 * value, of the type typid and typmod, as sm takes a datum, in the current
 * memory context: a varlena untoasted, a numeric made sm's.
 */
static sm_datum_t value_to_sm(Datum value, Oid typid, int32 typmod) {
  int16 typlen;
  bool typbyval;

  if (is_decimal(typid))
    return numeric_to_sm(value, typid, typmod);
  get_typlenbyval(typid, &typlen, &typbyval);
  if (-1 == typlen)
    return (sm_datum_t)PointerGetDatum(
        pg_detoast_datum_copy((struct varlena *)DatumGetPointer(value)));
  return (sm_datum_t)datumCopy(value, typbyval, typlen);
}

/*
 * The value of expr, a column's DEFAULT that calls nothing volatile, as sm
 * takes a datum: what rows older than the column read it as.  False if it
 * is null.  A varlena is untoasted, a numeric made sm's.
 */
static bool default_value(Expr *expr, Oid typid, int32 typmod,
                          sm_datum_t *out) {
  EState *estate = CreateExecutorState();
  ExprState *state;
  Datum value;
  bool isnull;

  /* Evaluated once, now, as PostgreSQL's attmissingval is. */
  state = ExecPrepareExpr(expression_planner(expr), estate);
  value = ExecEvalExpr(state, GetPerTupleExprContext(estate), &isnull);
  if (!isnull)
    *out = value_to_sm(value, typid, typmod);
  FreeExecutorState(estate);
  return !isnull;
}

/*
 * Give each row of tbl, the table relid, its own value of the column it
 * was just given, last of its columns: expr, its volatile DEFAULT, evaluated
 * for the row, as PostgreSQL's rewrite of the table gives one.  Until then
 * every row reads it as null.
 */
static void fill_column(Oid relid, Expr *expr, Oid typid, int32 typmod) {
  /* A statement of its own, which sees the new column. */
  CommandCounterIncrement();
  sm_tbl_t *tbl = open_table(relid, SM_LOCKMODE_IX);

  if (NULL == tbl)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation with OID %u does not exist", relid)));
  const sm_table_def_t *def = sm_tbl_def(tbl);

  if (NULL == def)
    sm_error("define a table");
  size_t n = def->columns.size();
  std::vector<sm_datum_t> values(n);
  std::unique_ptr<bool[]> isnull(new bool[n]);
  const sm_index_def_t *pk = NULL;

  for (const sm_index_def_t &x : def->indexes)
    if (x.primary)
      pk = &x;
  EState *estate = CreateExecutorState();
  ExprContext *econtext = GetPerTupleExprContext(estate);
  ExprState *state = ExecPrepareExpr(expression_planner(expr), estate);
  sm_rel_scan_t *scan;

  /*
   * The cursor does not see the rows this loop writes, so each row is
   * filled once.  An error leaves it to the transaction's end to close.
   */
  if (0 != sm_tbl_scan_rows(tbl, &scan))
    sm_error("scan a table");
  for (;;) {
    uint64_t rowno;
    const void *tuple;
    size_t len;
    bool null;
    Datum v;
    int rc;

    CHECK_FOR_INTERRUPTS();
    rc = sm_rel_scannxt(scan, &rowno, &tuple, &len);
    if (1 == rc)
      break;
    if (0 != rc)
      sm_error("scan a table");
    /* Its other columns as they are, its pointers out of line its own. */
    if (0 != sm_tbl_deform(tbl, tuple, len, values.data(), isnull.get(), n))
      sm_error("read a row");
    ResetExprContext(econtext);
    MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

    v = ExecEvalExpr(state, econtext, &null);
    isnull[n - 1] = null;
    if (!null)
      values[n - 1] = value_to_sm(v, typid, typmod);
    MemoryContextSwitchTo(old);
    /* The row's name: its primary key's columns, or its rowno. */
    sm_datum_t kv[INDEX_MAX_KEYS];
    bool kn[INDEX_MAX_KEYS] = {false};
    size_t nk = 1;

    kv[0] = (sm_datum_t)Int64GetDatum((int64)rowno);
    if (NULL != pk) {
      nk = pk->key_attnums.size();
      for (size_t k = 0; k < nk; k++) {
        kv[k] = values[pk->key_attnums[k]];
        kn[k] = isnull[pk->key_attnums[k]];
      }
    }
    if (0 != waiting([&] {
          return sm_tbl_update(tbl, sm_row_key_t{kv, kn, nk}, values.data(),
                               isnull.get(), n);
        }))
      sm_error("give a row its column's default");
  }
  sm_rel_scanend(scan);
  FreeExecutorState(estate);
}

/*
 * ADD COLUMN col to rel, the table relid, in sm: NOT NULL and a DEFAULT
 * only, or a serial or an identity column as parse analysis made it
 * (add_generated_column()).
 * 1. Evaluate the default now, unless it calls something volatile.  Rows
 *    already there read this value.
 * 2. Add the column.  NOT NULL with no default needs the table empty.
 * 3. Evaluate a default that calls something volatile for each row, and
 *    give the row its value (fill_column()).
 * 4. Check NOT NULL of every row, after step 3.
 * 5. Make an identity column identity, once it is NOT NULL.
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
  Expr *volatile_default = NULL;
  Oid identity_seq = InvalidOid;
  ListCell *l;

  check_column_name(col->colname);
  foreach (l, col->constraints) {
    Constraint *c = lfirst_node(Constraint, l);

    if (c->contype == CONSTR_NOTNULL)
      not_null = true;
    else if (c->contype == CONSTR_DEFAULT && 'u' != c->generated_when)
      raw_default = c->raw_expr;
    else if (c->contype == CONSTR_DEFAULT && c != update_default_of(col))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("multiple default values specified for column "
                             "\"%s\" of table \"%s\"",
                             col->colname, relname)));
    else if (c->contype != CONSTR_NULL && c->contype != CONSTR_DEFAULT)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ALTER TABLE ... ADD COLUMN supports only NOT "
                             "NULL and DEFAULT yet")));
  }
  if (NULL != col->collClause || 0 != col->generated ||
      NULL != col->compression || NULL != col->storage_name)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER TABLE ... ADD COLUMN supports only NOT "
                           "NULL and DEFAULT yet")));
  /* What parse analysis made of a serial's: its nextval() and NOT NULL. */
  if (NULL != col->raw_default)
    raw_default = col->raw_default;
  /* Without a plain default, the update default is the column's too. */
  if (NULL == raw_default && NULL != update_default_of(col))
    raw_default = update_default_of(col)->raw_expr;
  typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
  /* A numeric with no precision is its type's default one, kept in sm. */
  if (is_decimal(typid))
    typmod = numeric_typmod(typid, typmod);
  const char *update_default =
      column_update_default(col, raw_default, typid, typmod);
  if (raw_default != NULL) {
    ParseState *pstate = make_parsestate(NULL);
    Node *expr =
        cookDefault(pstate, raw_default, typid, typmod, col->colname, 0);

    free_parsestate(pstate);
    default_expr = nodeToString(expr);
    /* Each row its own value, given after the column is made. */
    if (contain_volatile_functions(expr))
      volatile_default = (Expr *)expr;
    else
      has_missing = default_value((Expr *)expr, typid, typmod, &missing);
  }
  /*
   * An identity column, its sequence made by parse analysis: each row is
   * given the sequence's next value, as its default would be, and the
   * column is made identity once it is NOT NULL.
   */
  if (0 != col->identity) {
    NextValueExpr *next = makeNode(NextValueExpr);

    identity_seq = RangeVarGetRelid(col->identitySequence, NoLock, false);
    next->seqid = identity_seq;
    next->typeId = typid;
    volatile_default = (Expr *)next;
  }
  /* NOT NULL only once every row holds its value. */
  sm_column_spec_t spec = {col->colname, type_code(typid), typmod,
                           not_null && NULL == volatile_default, default_expr};
  spec.on_update_expr = update_default;
  sm_session_t *s = xact_session();

  if (0 == waiting([&] {
        return sm_cat_add_column(s, relid, spec,
                                 has_missing ? &missing : nullptr);
      })) {
    if (NULL == volatile_default)
      return;
    fill_column(relid, volatile_default, typid, typmod);
    if (not_null) {
      /*
       * A statement of its own, which sees the rows filled: the session's
       * next fetch begins it in sm.
       */
      CommandCounterIncrement();
      s = xact_session();
      if (0 != waiting([&] {
            return sm_cat_set_not_null(s, relid, col->colname);
          })) {
        if (SM_ERR_NOTNULL == sm_errnum())
          ereport(ERROR,
                  (errcode(ERRCODE_NOT_NULL_VIOLATION),
                   errmsg("column \"%s\" of relation \"%s\" contains null "
                          "values",
                          col->colname, relname)));
        sm_error("make a column NOT NULL");
      }
    }
    if (OidIsValid(identity_seq)) {
      /* A statement of its own, which sees the column NOT NULL. */
      CommandCounterIncrement();
      s = xact_session();
      if (0 != waiting([&] {
            return sm_cat_set_identity(s, relid, col->colname, col->identity,
                                       identity_seq);
          }))
        sm_error("make a column an identity column");
    }
    /* The handles opened to fill it hold the column as it was made. */
    forget_table(relid);
    return;
  }
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
                            bool missing_ok, bool cascade);
static void validate_constraint(Oid relid, const char *relname,
                                const char *name);

static void alter_view(Oid relid, AlterTableStmt *stmt);

static void alter_column_type(Oid relid, const char *relname,
                              AlterTableCmd *cmd);

/* Whether cmd adds a serial or an identity column, for parse analysis. */
static bool generated_column(AlterTableCmd *cmd) {
  ColumnDef *col = castNode(ColumnDef, cmd->def);

  if (NULL != col->typeName && is_serial(col->typeName))
    return true;
  foreach_node(Constraint, c, col->constraints) {
    if (CONSTR_IDENTITY == c->contype)
      return true;
  }
  return false;
}

/*
 * ADD COLUMN of a serial or an identity column, cmd of stmt, as
 * PostgreSQL's ALTER TABLE does it.
 * 1. Run parse analysis, which makes it a column with its sequence.
 * 2. Run the sequence's CREATE SEQUENCE, before the column.
 * 3. Add the column.  add_column() gives each row a value.
 * 4. Run the OWNED BY that gives a serial's sequence its owner, after the
 *    column.
 */
static void add_generated_column(Oid relid, const char *relname,
                                 AlterTableStmt *stmt, AlterTableCmd *cmd) {
  AlterTableStmt *one = makeNode(AlterTableStmt);
  ParseState *pstate = make_parsestate(NULL);
  List *before = NIL, *after = NIL;

  one->relation = stmt->relation;
  one->objtype = stmt->objtype;
  one->cmds = list_make1(cmd);
  /* Locked first, as parse analysis opens the table assuming it is. */
  relation_close(relation_open(relid, AccessShareLock), NoLock);
  one = transformAlterTableStmt(relid, one, NULL, &before, &after);
  foreach_ptr(Node, n, before) {
    if (!IsA(n, CreateSeqStmt))
      elog(ERROR, "unexpected statement before ADD COLUMN: %d",
           (int)nodeTag(n));
    /*
     * Parse analysis gives it the table's owner, as a role; a sequence in sm
     * has none, as the one CREATE TABLE makes for a serial has none.
     */
    castNode(CreateSeqStmt, n)->ownerId = InvalidOid;
    DefineSequence(pstate, castNode(CreateSeqStmt, n));
  }
  /* The column's default names the sequence, which it must see. */
  CommandCounterIncrement();
  foreach_node(AlterTableCmd, c, one->cmds) {
    if (AT_AddColumn == c->subtype)
      add_column(relid, relname, castNode(ColumnDef, c->def), c->missing_ok);
    /* The column's NOT NULL, which add_column() made already. */
    else if (AT_AddConstraint != c->subtype ||
             CONSTR_NOTNULL != castNode(Constraint, c->def)->contype)
      elog(ERROR, "unexpected ALTER TABLE subcommand: %d", (int)c->subtype);
  }
  /* The OWNED BY names the column, which it must see. */
  CommandCounterIncrement();
  foreach_ptr(Node, n, after) {
    if (!IsA(n, AlterSeqStmt))
      elog(ERROR, "unexpected statement after ADD COLUMN: %d",
           (int)nodeTag(n));
    AlterSequence(pstate, castNode(AlterSeqStmt, n));
  }
  free_parsestate(pstate);
}

/*
 * ADD PRIMARY KEY or ADD UNIQUE, cmd of stmt, as PostgreSQL's ALTER TABLE
 * does it.
 * 1. Run parse analysis, which makes it the constraint's CREATE INDEX.
 * 2. Make a primary key's columns NOT NULL first, as ATPrepAddPrimaryKey()
 *    has them (../PKDDL.md).
 * 3. Make the index with smstore_create_index().
 */
static void add_index_constraint(Oid relid, const char *relname,
                                 AlterTableStmt *stmt, AlterTableCmd *cmd) {
  Constraint *con = castNode(Constraint, cmd->def);
  AlterTableStmt *one = makeNode(AlterTableStmt);
  List *before = NIL, *after = NIL;

  if (NULL != con->indexname)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER TABLE ... ADD CONSTRAINT ... USING INDEX "
                           "is not supported yet")));
  if (con->deferrable || con->initdeferred)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a DEFERRABLE primary key or unique constraint is "
                           "not supported yet")));
  one->relation = stmt->relation;
  one->objtype = stmt->objtype;
  one->cmds = list_make1(cmd);
  /* Locked first, as parse analysis opens the table assuming it is. */
  relation_close(relation_open(relid, AccessShareLock), NoLock);
  one = transformAlterTableStmt(relid, one, NULL, &before, &after);
  if (NIL != before || NIL != after)
    elog(ERROR, "unexpected statement with ADD CONSTRAINT");
  foreach_node(AlterTableCmd, c, one->cmds) {
    if (AT_AddIndex != c->subtype)
      elog(ERROR, "unexpected ALTER TABLE subcommand: %d", (int)c->subtype);
    IndexStmt *index = castNode(IndexStmt, c->def);
    Relation rel = relation_open(relid, NoLock);
    sm_table_def_t def;
    bool made_not_null = false;

    if (index->primary && table_def_now(relid, def))
      for (const sm_index_def_t &x : def.indexes)
        if (x.primary)
          ereport(ERROR,
                  (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                   errmsg("multiple primary keys for table \"%s\" are not "
                          "allowed",
                          relname)));
    foreach_node(IndexElem, elem, index->indexParams) {
      AttrNumber attnum = InvalidAttrNumber;

      for (int i = 0; i < RelationGetDescr(rel)->natts; i++) {
        Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);

        if (!att->attisdropped &&
            0 == strcmp(NameStr(att->attname), elem->name))
          attnum = i + 1;
      }

      /* A primary key's words are its SET NOT NULL's, a unique one's. */
      if (InvalidAttrNumber == attnum && index->primary)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" of relation \"%s\" does not "
                               "exist",
                               elem->name, relname)));
      if (InvalidAttrNumber == attnum)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                        errmsg("column \"%s\" named in key does not exist",
                               elem->name)));
      if (!index->primary || TupleDescAttr(RelationGetDescr(rel),
                                           attnum - 1)->attnotnull)
        continue;
      sm_session_t *s = xact_session();

      if (0 != waiting([&] {
            return sm_cat_set_not_null(s, relid, elem->name);
          })) {
        if (SM_ERR_NOTNULL == sm_errnum())
          ereport(ERROR,
                  (errcode(ERRCODE_NOT_NULL_VIOLATION),
                   errmsg("column \"%s\" of relation \"%s\" contains null "
                          "values",
                          elem->name, relname)));
        sm_error("make a column NOT NULL");
      }
      made_not_null = true;
    }
    relation_close(rel, NoLock);
    /* The index is made on columns it sees NOT NULL. */
    if (made_not_null) {
      forget_table(relid);
      CommandCounterIncrement();
    }
    smstore_create_index(relid, index);
  }
}

/*
 * Do an ALTER TABLE of the table relid, in sm, in this transaction, one
 * subcommand after another: ADD COLUMN, DROP COLUMN, ALTER COLUMN TYPE,
 * ADD, DROP and VALIDATE CONSTRAINT of a foreign key, and ADD and DROP
 * CONSTRAINT of a primary key or a unique constraint, the subcommands
 * AlterTableLookupRelation() lets through.  A view's is alter_view()'s.
 */
void smstore_alter_table(Oid relid, AlterTableStmt *stmt) {
  const char *relname = stmt->relation->relname;
  ListCell *l;

  check_owner(relid, relname, false);
  /* OWNER TO alone: a table's, a view's, a sequence's or an index's. */
  if (1 == list_length(stmt->cmds) &&
      AT_ChangeOwner == linitial_node(AlterTableCmd, stmt->cmds)->subtype) {
    change_owner(relid, relname,
                 linitial_node(AlterTableCmd, stmt->cmds)->newowner);
    return;
  }
  if (get_rel_relkind(relid) == RELKIND_VIEW) {
    alter_view(relid, stmt);
    return;
  }
  foreach (l, stmt->cmds) {
    AlterTableCmd *cmd = lfirst_node(AlterTableCmd, l);

    alter_table_rel(relid, relname);
    if (AT_AddColumn == cmd->subtype && generated_column(cmd))
      add_generated_column(relid, relname, stmt, cmd);
    else if (AT_AddColumn == cmd->subtype)
      add_column(relid, relname, castNode(ColumnDef, cmd->def),
                 cmd->missing_ok);
    else if (cmd->subtype == AT_AddConstraint) {
      Constraint *con = castNode(Constraint, cmd->def);

      if (CONSTR_PRIMARY == con->contype || CONSTR_UNIQUE == con->contype)
        add_index_constraint(relid, relname, stmt, cmd);
      else if (con->contype != CONSTR_FOREIGN)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("ALTER TABLE ... ADD CONSTRAINT supports only "
                               "FOREIGN KEY, PRIMARY KEY and UNIQUE yet")));
      else
        add_fkey(relid, relname, con);
    } else if (cmd->subtype == AT_DropConstraint)
      drop_constraint(relid, relname, cmd->name, cmd->missing_ok,
                      cmd->behavior == DROP_CASCADE);
    else if (cmd->subtype == AT_ValidateConstraint)
      validate_constraint(relid, relname, cmd->name);
    else if (AT_AlterColumnType == cmd->subtype)
      alter_column_type(relid, relname, cmd);
    else
      drop_column(relid, relname, cmd->name, cmd->missing_ok,
                  cmd->behavior == DROP_CASCADE);
    /* The next subcommand sees what this one did. */
    CommandCounterIncrement();
  }
}

/*
 * Refuse DDL on the relation oid, named name, to a role that does not own
 * it, nor, if schema_too, as DROP allows, its schema: as PostgreSQL's
 * RangeVarCallbackOwnsRelation() and RangeVarCallbackForDropRelation()
 * do.  The owner is pg_global's (utils/init/pg_global.c).
 */
static void check_owner(Oid oid, const char *name, bool schema_too) {
  char kind = get_rel_relkind(oid);

  if (object_ownercheck(RelationRelationId, oid, GetUserId()))
    return;
  if (schema_too && object_ownercheck(NamespaceRelationId,
                                      get_rel_namespace(oid), GetUserId()))
    return;
  aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(kind), name);
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
  check_owner(relid, stmt->relation->relname, false);
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
 * makes one of (../sm/FOREIGNKEY.md).
 * 1. Choose the referenced columns, their unique index and the operators,
 *    as ATAddForeignKeyConstraint() does.
 * 2. Put the foreign key in sm, which keeps it, and takes locks X on both
 *    tables.
 * 3. Unless it is NOT VALID, or the table is new, check the rows there by
 *    RI_Initial_Check() (check_fkey_rows()), in this transaction.
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
    bool among = false, again = false;

    fkey_column(rel, strVal(lfirst(l)), numdelset, delsetcols, t, c);
    for (int i = 0; i < numfks; i++)
      among |= fkattnum[i] == delsetcols[numdelset];
    if (!among)
      ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                      errmsg("column \"%s\" referenced in ON DELETE SET "
                             "action must be part of foreign key",
                             strVal(lfirst(l)))));
    /*
     * A column named again is dropped, as validateFkOnDeleteSetColumns()
     * drops it: the action's UPDATE would set it twice, and fail.
     */
    for (int i = 0; i < numdelset; i++)
      again |= delsetcols[i] == delsetcols[numdelset];
    if (!again)
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

/*
 * The index of the primary key or the unique constraint name of the table
 * relid, the constraint's own name, or 0 if it has none.
 */
static uint64_t constraint_index(Oid relid, const char *name) {
  sm_table_def_t def;

  if (table_def_now(relid, def))
    for (const sm_index_def_t &x : def.indexes)
      if ((x.primary || x.constraint) && x.name == name)
        return x.oid;
  return 0;
}

static void drop_index(uint64_t table, uint64_t index);

/*
 * ALTER TABLE ... DROP CONSTRAINT: a foreign key, or a primary key or a
 * unique constraint, whose index goes with it.  A foreign key that
 * references that index is what depends on it: CASCADE drops it, RESTRICT
 * is an error.  A primary key's columns stay NOT NULL, as PostgreSQL's do.
 */
static void drop_constraint(Oid relid, const char *relname, const char *name,
                            bool missing_ok, bool cascade) {
  sm_fkey_def_t f;

  if (!fkey_named(relid, name, f)) {
    uint64_t index = constraint_index(relid, name);

    if (0 != index) {
      drop_referencing(relid, relname, cascade, {}, index, name);
      drop_index(relid, index);
      return;
    }
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
 * This session's estimate of table oid (index 0) or of its index, if made at
 * epoch, the catalog's now, within size_cache_age; else NULL.
 */
static const estimate_entry_t *kept_estimate(Oid oid, uint64_t index,
                                             uint64_t epoch) {
  if (UINT64_MAX == epoch)
    return NULL;
  auto &estimates = tcb->smstore->estimates;
  auto it = estimates.find((uint64_t)oid << 32 | index);

  if (estimates.end() == it || it->second.epoch != epoch ||
      TimestampDifferenceExceeds(it->second.at, GetCurrentTimestamp(),
                                 size_cache_age))
    return NULL;
  return &it->second;
}

/*
 * Keep e as this session's estimate of table oid (index 0) or of its
 * index, made at epoch; none while the transaction has DDL of its own.
 */
static void keep_estimate(Oid oid, uint64_t index, uint64_t epoch,
                          estimate_entry_t e) {
  auto &estimates = tcb->smstore->estimates;

  if (UINT64_MAX == epoch)
    return;
  /* Past the limit, the estimates are forgotten, and made again as needed. */
  if (estimates.size() >= statistic_cache_max)
    estimates.clear();
  e.epoch = epoch;
  e.at = GetCurrentTimestamp();
  estimates[(uint64_t)oid << 32 | index] = e;
}

/*
 * How much the table has grown since it was analyzed, and its heap's bytes
 * now, in *now.
 * 1. Count its heap's bytes now, at the compression recorded: bytes in
 *    memtables are uncompressed, so they count at c.
 * 2. If the analyze found nothing on disk, and the table has row numbers,
 *    return its row numbers now over then.  Its compression is unknown, and
 *    a flush since would read as a shrink.  Row numbers count inserts and
 *    not deletes (STATISTICS.md section 2.2).
 * 3. Else return its bytes now over then.  A table with a primary key draws
 *    no row numbers (../sm/ROWKEY.md section 2.1), and its bytes say all the
 *    same.
 */
static double growth(sm_tbl_t *tbl, const sm_stats_t &st, double *now) {
  uint64_t disk, mem, n;
  double c = compression(st), then;

  if (sm_tbl_size(tbl, &disk, &mem) != 0)
    sm_error("size a table");
  *now = disk + mem * c;
  /* A table with a primary key has no row numbers: its bytes say. */
  if (st.heap_disk == 0 && st.rowno_limit > 0) {
    if (sm_tbl_rowno_limit(tbl, &n) != 0)
      sm_error("read a table's row numbers");
    return (double)n / st.rowno_limit;
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
  /* Read before the estimate, so a publish meanwhile retires it. */
  uint64_t epoch = sm_cat_epoch(session());
  const estimate_entry_t *kept = kept_estimate(oid, 0, epoch);

  if (NULL != kept) {
    *pages = kept->pages;
    *tuples = kept->tuples;
    return kept->found;
  }
  sm_tbl_t *tbl = open_table(oid, SM_LOCKMODE_IS);
  estimate_entry_t e = {};
  double now;

  if (tbl == NULL)
    return false;
  std::shared_ptr<const sm_stats_t> st = table_stats(oid);
  if (st != nullptr) {
    double g = growth(tbl, *st, &now);

    if (now > 0) {
      e.found = true;
      e.tuples = st->live_rows * g;
      e.pages = std::max(1.0, std::ceil(now / BLCKSZ));
    }
  }
  keep_estimate(oid, 0, epoch, e);
  *pages = e.pages;
  *tuples = e.tuples;
  return e.found;
}

/*
 * An index's pages and entries for the planner: its bytes now, in pages,
 * and the entries the last analyze counted -- a partial index's share of
 * the rows -- grown as its table has, from the last analyze's rows to
 * table_tuples, the plan's estimate of the table's.  False if its table
 * was never analyzed, or the index was made since.
 */
bool smstore_estimate_index_size(Oid table, Oid index, double table_tuples,
                                 double *pages, double *tuples) {
  /* Read before the estimate, so a publish meanwhile retires it. */
  uint64_t epoch = sm_cat_epoch(session());
  const estimate_entry_t *kept = kept_estimate(table, index, epoch);
  estimate_entry_t e = {};

  if (NULL != kept) {
    e = *kept;
  } else {
    sm_tbl_t *tbl = open_table(table, SM_LOCKMODE_IS);
    uint64_t disk, mem;

    if (tbl == NULL)
      return false;
    std::shared_ptr<const sm_stats_t> st = table_stats(table);
    /* A table never analyzed has statistics for none of its indexes. */
    for (size_t i = 0; st != nullptr && i < st->indexes.size(); i++) {
      const sm_stats_index_t &x = st->indexes[i];

      if (x.oid != index)
        continue;
      if (sm_tbl_idx_size(tbl, index, &disk, &mem) != 0)
        sm_error("size an index");
      e.found = true;
      e.tuples = x.entries;
      e.live_rows = st->live_rows;
      e.pages =
          std::max(1.0, std::ceil((disk + mem * compression(*st)) / BLCKSZ));
      break;
    }
    keep_estimate(table, index, epoch, e);
  }
  if (!e.found)
    return false;
  /* The table's growth as the plan has it: no second look at its size. */
  double g = e.live_rows > 0 ? table_tuples / e.live_rows : 1.0;
  *tuples = e.tuples * g;
  *pages = e.pages;
  return true;
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
 * The datum of a value sm kept in a statistic, bytes: sm's datum, a
 * decimal's as it is.
 */
static Datum stat_datum(const sm_type_t *smtype, const std::string &bytes) {
  return (Datum)sm_datum_of_bytes(smtype, bytes);
}

/*
 * Keep entry as the row of key, made in full: one left half made by an
 * error would read as no statistics.  Past the most kept, start over.
 */
static void keep_statistic(uint64_t key, statistic_entry_t entry) {
  auto &kept = tcb->smstore->statistics;

  if (kept.size() >= statistic_cache_max)
    kept.clear();
  kept[key] = std::move(entry);
}

/*
 * A copy of the pg_statistic row in bytes, palloc'd, for the caller to free
 * with heap_freetuple(); NULL for empty bytes.
 */
static HeapTuple statistic_copy(const std::string &bytes) {
  HeapTupleData kept;

  if (bytes.empty())
    return NULL;
  kept.t_len = (uint32)bytes.size();
  ItemPointerSetInvalid(&kept.t_self);
  kept.t_tableOid = InvalidOid;
  kept.t_data = (HeapTupleHeader)bytes.data();
  return heap_copytuple(&kept);
}

/*
 * The pg_statistic row of relid's column attnum, made from sm's statistics
 * (STATISTICS.md section 2.3): a table's column, or an expression of one of
 * its indexes, by its index's oid.  Its slots are what PostgreSQL's ANALYZE
 * fills -- the common values with the type's =, the histogram and the
 * correlation with its < -- with the column's collation.  NULL if there is
 * none: a table never analyzed, a column without statistics, or an
 * inheritance tree.  The caller frees it with heap_freetuple().  A row is
 * made once from each version of the statistics, and copied out after.
 */
HeapTuple smstore_statistic_tuple(Oid relid, AttrNumber attnum, bool inh) {
  if (inh || attnum <= 0 || relid < FirstNormalObjectId)
    return NULL;
  uint64_t key = (uint64_t)relid << 16 | (uint16)attnum;
  auto &kept = tcb->smstore->statistics;
  /* Read before the lookup, so a publish meanwhile retires what it finds. */
  uint64_t epoch = sm_cat_epoch(session());

  /* Made while the epoch holds: no analyze or DDL since (SCHEMACACHE.md). */
  if (UINT64_MAX != epoch) {
    auto it = kept.find(key);

    if (kept.end() != it && it->second.epoch == epoch)
      return statistic_copy(it->second.bytes);
  }
  /* An index's statistics are its table's, by the index's oid. */
  Oid table = smstore_index_table(relid);
  bool is_index = OidIsValid(table);

  if (!is_index)
    table = relid;
  std::shared_ptr<const sm_stats_t> st = table_stats(table);

  if (st == nullptr)
    return NULL;

  /* The column's type and collation, from its descriptor. */
  Relation rel = relation_open(relid, NoLock);
  Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
  Oid typid = att->atttypid, coll = att->attcollation;
  int16 typlen = att->attlen;
  bool typbyval = att->attbyval;
  char typalign = att->attalign;

  relation_close(rel, NoLock);

  /* The row made before, if from these statistics and this type. */
  auto it = kept.find(key);

  if (it != kept.end() && it->second.stats == st &&
      it->second.typid == typid && it->second.coll == coll) {
    it->second.epoch = epoch;
    return statistic_copy(it->second.bytes);
  }

  const sm_stat_t *found = NULL;

  for (const sm_stat_t &x : st->stats)
    if (x.index.has_value() == is_index && (!is_index || x.index == relid) &&
        x.position == attnum - 1) {
      found = &x;
      break;
    }
  if (found == NULL) {
    keep_statistic(key,
                   statistic_entry_t{st, typid, coll, std::string(), epoch});
    return NULL;
  }
  TypeCacheEntry *tc =
      lookup_type_cache(typid, TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR);
  const sm_type_t *smtype = sm_type(type_code(typid));
  auto values_array = [&](const std::vector<std::string> &bytes) {
    std::vector<Datum> d;

    for (const std::string &b : bytes)
      d.push_back(stat_datum(smtype, b));
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
  if (found->correlation && OidIsValid(tc->lt_opr))
    slot(STATISTIC_KIND_CORRELATION, tc->lt_opr,
         float4_array(&*found->correlation, 1), true, 0, false);

  /* Formed in pg_statistic's shape, which it has no rows of, and kept. */
  HeapTuple tup = heap_form_tuple(statistic_tupdesc(), values, nulls);

  keep_statistic(key, statistic_entry_t{st, typid, coll,
                                        std::string((const char *)tup->t_data,
                                                    tup->t_len),
                                        epoch});
  return tup;
}

/*
 * The average width of relid's column attnum, as its pg_statistic row says
 * (smstore_statistic_tuple()), 0 if it has none: read from the row kept,
 * not a copy of it, which a plan reads the width of for each column it
 * returns.
 */
int32 smstore_statistic_width(Oid relid, AttrNumber attnum) {
  if (attnum <= 0 || relid < FirstNormalObjectId)
    return 0;
  auto &kept = tcb->smstore->statistics;
  /* Read before the lookup, so a publish meanwhile retires what it finds. */
  uint64_t epoch = sm_cat_epoch(session());

  /* Made while the epoch holds: no analyze or DDL since (SCHEMACACHE.md). */
  if (UINT64_MAX != epoch) {
    auto it = kept.find((uint64_t)relid << 16 | (uint16)attnum);

    if (kept.end() != it && it->second.epoch == epoch) {
      const std::string &bytes = it->second.bytes;
      HeapTupleData row;

      if (bytes.empty())
        return 0;
      row.t_data = (HeapTupleHeader) const_cast<char *>(bytes.data());
      return ((Form_pg_statistic)GETSTRUCT(&row))->stawidth;
    }
  }
  /* Made, and kept, as the planner's row is. */
  HeapTuple tup = smstore_statistic_tuple(relid, attnum, false);
  int32 width = 0;

  if (HeapTupleIsValid(tup)) {
    width = ((Form_pg_statistic)GETSTRUCT(tup))->stawidth;
    heap_freetuple(tup);
  }
  return width;
}

/* An array of values' text, by the type's output function, as text[]. */
static Datum text_array(const sm_type_t *smtype, Oid typid,
                        const std::vector<std::string> &bytes) {
  Oid out;
  bool varlena;
  std::vector<Datum> d;

  getTypeOutputInfo(typid, &out, &varlena);
  for (const std::string &b : bytes)
    d.push_back(CStringGetTextDatum(
        OidOutputFunctionCall(out, stat_datum(smtype, b))));
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
        relation_open(x.index ? (Oid)*x.index : oid, AccessShareLock);
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), x.position);
    const sm_type_t *smtype = sm_type(type_code(att->atttypid));
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
      values[7] = text_array(smtype, att->atttypid, x.mcv);
      values[8] = float4_array(x.mcv_freq.data(), (int)x.mcv_freq.size());
      nulls[7] = nulls[8] = false;
    }
    if (x.bounds.size() >= 2) {
      values[9] = text_array(smtype, att->atttypid, x.bounds);
      nulls[9] = false;
    }
    if (x.correlation) {
      values[10] = Float4GetDatum(*x.correlation);
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
  sm_tbl_t *tbl;
  int rc;

  if (!may_maintain(oid, name, "analyze", named))
    return;
  tbl = skip_locked ? try_open_table(oid, SM_LOCKMODE_IS, &blocked)
                    : open_table(oid, SM_LOCKMODE_IS);

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

/* Every table in sm, as this transaction sees them, and its name. */
static std::vector<std::pair<Oid, std::string>> every_table() {
  std::vector<std::pair<Oid, std::string>> out;
  std::vector<std::string> nspnames;

  if (sm_cat_list_namespaces(xact_session(), nspnames) != 0)
    sm_error("list the schemas");
  for (const std::string &nsp : nspnames) {
    Oid nspoid = smstore_namespace_oid(nsp.c_str());
    std::vector<std::string> tables;

    if (sm_cat_list_tables(xact_session(), nspoid, tables) != 0)
      sm_error("list a schema's tables");
    for (const std::string &t : tables)
      out.push_back({smstore_relation_oid(t.c_str(), nspoid), t});
  }
  return out;
}

/*
 * The table whose column owns the sequence oid, or InvalidOid if none does.
 */
static Oid sequence_owner_table(Oid oid) {
  sm_sequence_def_t seq;

  if (sm_cat_sequence_def(xact_session(), oid, &seq) != 0)
    sm_error("read a sequence");
  return (Oid)seq.owner;
}

/*
 * ALTER ... OWNER TO: make the role spec names the owner of the relation
 * relid, named relname, in pg_global, as PostgreSQL's ATExecChangeOwner()
 * does.  A role that is not a superuser must be able to become the new
 * owner, which must have CREATE on the schema.  A table's sequences, those
 * its columns own, go with it; an index's owner is its table's, and a
 * sequence a column owns its table's.  Its ACL, if it keeps one, names
 * the new owner where it named the old (pg_global_change_owner()).
 */
static void change_owner_to(Oid relid, const char *relname, Oid owner,
                            bool check);

static void change_owner(Oid relid, const char *relname, RoleSpec *spec) {
  change_owner_to(relid, relname, get_rolespec_oid(spec, false), true);
}

/*
 * REASSIGN OWNED's: make owner the owner of the relation relid, as ALTER
 * ... OWNER TO would, with no check, as PostgreSQL's shdepReassignOwned()
 * has it.  An index, and a sequence a column owns, go with their table.
 */
void smstore_reassign_owner(Oid relid, Oid owner) {
  char kind = get_rel_relkind(relid);

  /* An entry a crash left behind names no relation. */
  if ('\0' == kind || RELKIND_INDEX == kind ||
      (RELKIND_SEQUENCE == kind && OidIsValid(sequence_owner_table(relid))))
    return;
  change_owner_to(relid, get_rel_name(relid), owner, false);
}

/* change_owner()'s, with check_owner()'s checks if check. */
static void change_owner_to(Oid relid, const char *relname, Oid owner,
                            bool check) {
  char kind = get_rel_relkind(relid);
  Oid nspoid = get_rel_namespace(relid);
  uint64_t dbid = pg_global_dbid(MyDatabaseId);
  Oid table;

  if (RELKIND_INDEX == kind) {
    ereport(WARNING,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
             errmsg("cannot change owner of index \"%s\"", relname),
             errhint("Change the ownership of the index's table instead.")));
    return;
  }
  if (RELKIND_SEQUENCE == kind &&
      OidIsValid(table = sequence_owner_table(relid)))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot change owner of sequence \"%s\"", relname),
                    errdetail("Sequence \"%s\" is linked to table \"%s\".",
                              relname, get_rel_name(table))));
  if (pg_global_owner(relid) == owner)
    return;
  if (check && !superuser()) {
    AclResult result;

    check_can_set_role(GetUserId(), owner);
    result = object_aclcheck(NamespaceRelationId, nspoid, owner, ACL_CREATE);
    if (ACLCHECK_OK != result)
      aclcheck_error(result, OBJECT_SCHEMA, get_namespace_name(nspoid));
  }
  pg_global_change_owner(dbid, RelationRelationId, relid, owner,
                         RELPERSISTENCE_TEMP == get_rel_persistence(relid));
  if (RELKIND_RELATION != kind || PG_TEMP_NAMESPACE == nspoid)
    return;
  /* Its sequences, which are in its schema, as OWNED BY requires. */
  std::vector<std::string> names;

  if (sm_cat_list_sequences(xact_session(), sm_nspoid(nspoid), names) != 0)
    sm_error("list a schema's sequences");
  for (const std::string &name : names) {
    Oid seq = sequence_oid(name.c_str(), nspoid);

    if (OidIsValid(seq) && sequence_owner_table(seq) == relid)
      pg_global_change_owner(dbid, RelationRelationId, seq, owner, false);
  }
}

/*
 * Whether this role may vacuum or analyze, what, the table oid, named
 * name: with MAINTAIN on it, which its owner has, as PostgreSQL 18's
 * vacuum_is_permitted_for_relation() asks.  One the statement named, and
 * it may not, is passed over with PostgreSQL's warning; one it did not,
 * quietly.
 */
static bool may_maintain(Oid oid, const char *name, const char *what,
                         bool named) {
  if (ACLCHECK_OK == pg_class_aclcheck(oid, GetUserId(), ACL_MAINTAIN))
    return true;
  if (named)
    ereport(WARNING, (errmsg("permission denied to %s \"%s\", skipping it",
                             what, name)));
  return false;
}

/*
 * Compact the table oid's store in sm, named name for a message, its heap
 * and its indexes into one sorted run: opened as a reader, so writers go on
 * meanwhile, and DDL waits.  What is not a table is passed over with a
 * warning, as PostgreSQL passes over what it cannot vacuum; with
 * skip_locked a table held X is passed over too, with a warning if named.
 */
static void vacuum_table(Oid oid, const char *name, bool skip_locked,
                         bool named) {
  bool blocked = false;
  sm_tbl_t *tbl;

  if (!may_maintain(oid, name, "vacuum", named))
    return;
  tbl = skip_locked ? try_open_table(oid, SM_LOCKMODE_IS, &blocked)
                    : open_table(oid, SM_LOCKMODE_IS);

  if (blocked) {
    if (named)
      ereport(WARNING,
              (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
               errmsg("skipping vacuum of \"%s\" --- lock not available",
                      name)));
    return;
  }
  if (NULL == tbl) {
    ereport(WARNING, (errmsg("skipping \"%s\" --- cannot vacuum non-tables",
                             name)));
    return;
  }
  /* The store's files are written again: seconds, for a large table. */
  if (0 != sm_tbl_compact(tbl))
    sm_error("vacuum a table");
}

/*
 * VACUUM: each table rels names, or with none every table in sm, its store
 * compacted (sm_tbl_compact()), so a lookup visits one sorted run and the
 * versions no reader needs are dropped.  skip_locked is SKIP_LOCKED.
 */
void smstore_vacuum(List *rels, bool skip_locked) {
  ListCell *l;

  if (rels == NIL) {
    for (const auto &[oid, name] : every_table())
      vacuum_table(oid, name.c_str(), skip_locked, false);
    return;
  }
  foreach (l, rels) {
    VacuumRelation *vrel = lfirst_node(VacuumRelation, l);

    vacuum_table(RangeVarGetRelid(vrel->relation, NoLock, false),
                 vrel->relation->relname, skip_locked, true);
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
    for (const auto &[oid, name] : every_table())
      analyze_table(oid, name.c_str(), verbose, skip_locked, false);
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

static void check_version(Oid relid, sm_tbl_t *tbl);

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
    check_version(relid, tbl);
  else if (lock_view(relid, mode, &version))
    RelationCheckSmVersion(relid, relid, version);
}

/*
 * Bring what this session caches of relid to the version tbl, just opened,
 * holds (RelationCheckSmVersion()), unless it was checked at the catalog's
 * epoch now: no publish has moved it since (SCHEMACACHE.md section 4.2).
 * Read after the open, whose lock waits out a DDL and its publish.
 */
static void check_version(Oid relid, sm_tbl_t *tbl) {
  uint64_t epoch = sm_cat_epoch(session());
  auto &checked = tcb->smstore->checked;

  if (UINT64_MAX != epoch) {
    auto it = checked.find(relid);

    if (checked.end() != it && it->second == epoch)
      return;
  }
  RelationCheckSmVersion(relid, lock_target(relid), sm_tbl_version(tbl));
  if (UINT64_MAX == epoch)
    return;
  /* Past the limit, the epochs are forgotten, and checked again as needed. */
  if (checked.size() >= statistic_cache_max)
    checked.clear();
  checked[relid] = epoch;
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
    check_version(relid, tbl);
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

/*
 * A partial or an expression index's eval, as digres writes it for sm to
 * keep (../EXPRINDEX.md section 4.1): a List of its expressions in key
 * order, its predicate as an implicit-AND list, and its expression
 * columns' names, nodeToString()'s; each NIL if it has none.
 */
static std::string index_eval_string(List *exprs, List *pred, List *names) {
  char *s = nodeToString(list_make3(exprs, pred, names));
  std::string out(s);

  pfree(s);
  return out;
}

/* The parts of eval, an index's, index_eval_string()'s, palloc'd. */
void smstore_index_eval_parts(const char *eval, List **exprs, List **pred,
                              List **names) {
  List *parts = (List *)stringToNode(eval);

  *exprs = (List *)linitial(parts);
  *pred = (List *)lsecond(parts);
  *names = (List *)lthird(parts);
}

/*
 * Make index's predicate and expressions, eval, ready to evaluate in this
 * thread, over a row of natts columns: once, kept for the thread's life
 * (index_eval_t), as an index's eval never changes and its oid is never
 * reused.  A row of more columns, one added since, makes it again.
 */
static index_eval_t *index_eval_ready(uint64_t index, const std::string &eval,
                                      size_t natts) {
  auto it = tcb->smstore->evals.find(index);

  if (tcb->smstore->evals.end() != it &&
      (size_t)it->second.slot->tts_tupleDescriptor->natts == natts)
    return &it->second;
  /*
   * The catalogs are read in a transaction: on one of sm's index builders,
   * which runs none, one of its own, which reads nothing of sm's.
   */
  bool own_xact = !IsTransactionState();

  if (own_xact)
    StartTransactionCommand();
  MemoryContext cxt = AllocSetContextCreate(TopMemoryContext,
                                            "index evaluator",
                                            ALLOCSET_SMALL_SIZES);
  MemoryContext old = MemoryContextSwitchTo(cxt);
  index_eval_t e = {};
  List *exprs, *pred, *names;

  e.cxt = cxt;
  try {
    smstore_index_eval_parts(eval.c_str(), &exprs, &pred, &names);
    e.estate = CreateExecutorState();
    e.pred = NIL != pred ? ExecPrepareQual(pred, e.estate) : NULL;
    foreach_ptr(Expr, expr, exprs) {
      int16 typlen;
      bool typbyval;

      e.exprs.push_back(ExecPrepareExpr(expr, e.estate));
      get_typlenbyval(exprType((Node *)expr), &typlen, &typbyval);
      e.typlens.push_back(typlen);
      e.typbyvals.push_back(typbyval);
    }
    /*
     * Its columns are sm's datums, which a Var of the row reads as they are;
     * those read are typed by their Vars, as the executor checks them.
     */
    TupleDesc desc = CreateTemplateTupleDesc((int)natts);

    foreach_node(Var, var,
                 pull_var_clause((Node *)list_concat_copy(exprs, pred), 0)) {
      if (var->varattno > 0 && var->varattno <= (AttrNumber)natts)
        TupleDescInitEntry(desc, var->varattno, NULL, var->vartype,
                           var->vartypmod, 0);
    }
    e.slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
  } catch (...) {
    MemoryContextSwitchTo(old);
    MemoryContextDelete(cxt);
    if (own_xact)
      AbortCurrentTransaction();
    throw;
  }
  MemoryContextSwitchTo(old);
  if (own_xact)
    CommitTransactionCommand();
  if (tcb->smstore->evals.end() != it) {
    MemoryContextDelete(it->second.cxt);
    tcb->smstore->evals.erase(it);
  }
  return &tcb->smstore->evals.emplace(index, std::move(e)).first->second;
}

/* value, of a type of typlen and typbyval, as the bytes sm keys it by. */
static std::string key_value_bytes(Datum value, int16 typlen, bool typbyval) {
  std::string out;

  if (typbyval) {
    out.resize(typlen);
    memcpy(out.data(), &value, typlen);
  } else if (-1 == typlen) {
    struct varlena *v = pg_detoast_datum_packed(
        (struct varlena *)DatumGetPointer(value));

    out.assign((const char *)v, VARSIZE_ANY(v));
  } else
    out.assign((const char *)DatumGetPointer(value), typlen);
  return out;
}

/*
 * Keep the error just caught, and say it to sm: sm's call that asked the
 * evaluator fails with it, and sm_error() raises it again, as PostgreSQL's.
 */
static int keep_eval_error(void) {
  smstore_state_t *st = tcb->smstore.get();

  MemoryContextSwitchTo(TopMemoryContext);
  if (NULL == st->eval_error_cxt)
    st->eval_error_cxt = AllocSetContextCreate(
        TopMemoryContext, "index evaluator error", ALLOCSET_SMALL_SIZES);
  else
    MemoryContextReset(st->eval_error_cxt);
  MemoryContextSwitchTo(st->eval_error_cxt);
  st->eval_error = CopyErrorData();
  FlushErrorState();
  MemoryContextSwitchTo(TopMemoryContext);
  return sm_seterr(SM_ERR_INVALID, st->eval_error->message);
}

/*
 * sm's evaluator of digres's partial and expression indexes
 * (sm_index_eval_fn, ../EXPRINDEX.md section 4.3): whether the row of n
 * values belongs in index, by its predicate, and its expressions' values,
 * by PostgreSQL's executor.  An error is caught and kept, never thrown
 * through sm (keep_eval_error()).
 */
static int smstore_index_eval(void *ctx, uint64_t table, uint64_t index,
                              const std::string &eval,
                              const sm_datum_t *values, const bool *isnull,
                              size_t n, bool *belongs,
                              std::vector<std::string> &out,
                              bool *out_isnull) noexcept {
  MemoryContext old;
  int rc = 0;

  /* One of sm's index builders, at its first call: made able to evaluate. */
  session_thread_adopt();
  old = CurrentMemoryContext;
  PG_TRY();
  {
    index_eval_t *e = index_eval_ready(index, eval, n);
    ExprContext *econtext = GetPerTupleExprContext(e->estate);
    TupleTableSlot *slot = e->slot;

    ExecClearTuple(slot);
    memcpy(slot->tts_values, values, n * sizeof(Datum));
    memcpy(slot->tts_isnull, isnull, n * sizeof(bool));
    ExecStoreVirtualTuple(slot);
    econtext->ecxt_scantuple = slot;
    *belongs = NULL == e->pred || ExecQual(e->pred, econtext);
    for (size_t k = 0; *belongs && k < e->exprs.size(); k++) {
      Datum value =
          ExecEvalExprSwitchContext(e->exprs[k], econtext, &out_isnull[k]);

      if (!out_isnull[k])
        out[k] = key_value_bytes(value, e->typlens[k], e->typbyvals[k]);
    }
    ResetExprContext(econtext);
  }
  PG_CATCH();
  { rc = keep_eval_error(); }
  PG_END_TRY();
  MemoryContextSwitchTo(old);
  return rc;
}

/* Give sm digres's evaluator, before a database is opened (datadir.c). */
int smstore_set_index_evaluator(sm_env_t *env) {
  return sm_set_index_evaluator(env, smstore_index_eval, NULL);
}

/*
 * Whether a CREATE INDEX asks for a tree, USING btree: a B+tree of the
 * index's own pages (../sm/BTREE.md).  USING lsm, the default, asks for
 * entries that are each a key of RocksDB's.
 */
static bool tree_kind(const IndexStmt *stmt) {
  bool tree = strcmp(stmt->accessMethod, SMSTORE_INDEX_TREE) == 0;

  if (tree && stmt->primary)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a primary key is not a tree")));
  return tree;
}

/* Refuse what a CREATE INDEX asks that sm's indexes do not have yet. */
static void check_create_index(const IndexStmt *stmt) {
  ListCell *l;

  if (strcmp(stmt->accessMethod, SMSTORE_INDEX_LSM) != 0 &&
      strcmp(stmt->accessMethod, SMSTORE_INDEX_TREE) != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("access method \"%s\" is not supported",
                           stmt->accessMethod)));
  if (stmt->options != NIL)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("storage parameter \"%s\" is not supported",
                    linitial_node(DefElem, stmt->options)->defname)));
  if (stmt->excludeOpNames != NIL || stmt->tableSpace != NULL ||
      stmt->nulls_not_distinct ||
      stmt->deferrable)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("CREATE INDEX supports only columns, expressions, "
                           "WHERE, UNIQUE and INCLUDE yet")));
  foreach (l, stmt->indexIncludingParams) {
    IndexElem *elem = lfirst_node(IndexElem, l);

    if (elem->name == NULL)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("expressions are not supported in included "
                             "columns")));
  }
}

/*
 * Refuse what PostgreSQL refuses of a predicate or an index expression,
 * expr, which what (DefineIndex(), CheckPredicate(), ComputeIndexAttrs()):
 * a function not immutable, a system column; and what sm cannot evaluate
 * a row for: the whole row.  Add the columns it reads, 0-based, to reads.
 */
static void check_index_expr(Node *expr, const char *what,
                             std::vector<int32_t> &reads) {
  Bitmapset *attrs = NULL;
  int i = -1;

  if (contain_mutable_functions_after_planning((Expr *)expr))
    ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                    errmsg("functions in index %s must be marked IMMUTABLE",
                           what)));
  pull_varattnos(expr, 1, &attrs);
  while ((i = bms_next_member(attrs, i)) >= 0) {
    AttrNumber attno = i + FirstLowInvalidHeapAttributeNumber;

    if (attno < 0)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("index creation on system columns is not "
                             "supported")));
    if (0 == attno)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index %s that reads the whole row is not "
                             "supported",
                             what)));
    if (reads.end() == std::find(reads.begin(), reads.end(), attno - 1))
      reads.push_back(attno - 1);
  }
}

/*
 * Refuse an operator class or a collation elem names for a key of type
 * atttype and collation attcollation that is not the one sm keys it by:
 * the type's default btree operator class and the column's collation, as
 * the relcache describes the index (RelationInitIndexAccessInfo()).
 */
static void check_index_key(const IndexElem *elem, Oid atttype,
                            Oid attcollation) {
  if (elem->opclass != NIL) {
    Oid opclass = ResolveOpClass(elem->opclass, atttype, DEFAULT_INDEX_TYPE,
                                 BTREE_AM_OID);

    if (opclass !=
        ResolveOpClass(NIL, atttype, DEFAULT_INDEX_TYPE, BTREE_AM_OID))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("operator class %s is not supported yet",
                             generate_opclass_name(opclass)),
                      errdetail("An index key takes only its type's default "
                                "operator class.")));
    /* No core btree operator class has options. */
    if (elem->opclassopts != NIL)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("operator class %s has no options",
                             generate_opclass_name(opclass))));
  }
  if (elem->collation == NIL)
    return;
  if (!type_is_collatable(atttype))
    ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("collations are not supported by type %s",
                           format_type_be(atttype))));
  if (LookupCollation(NULL, elem->collation, -1) != attcollation)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("collation \"%s\" is not supported for an index "
                           "key yet",
                           NameListToString(elem->collation)),
                    errdetail("An index key takes only its column's "
                              "collation.")));
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
 * The key of a row of the duplicate key sm's last error met, in its table,
 * as sm_errdup() gives it; empty if sm names none.
 */
static std::string error_dup_row(void) {
  std::string row;
  uint64_t index;

  sm_errdup(&index, &row);
  return row;
}

/*
 * PostgreSQL's words for the key the row at row, a key of the table relid,
 * holds in the columns keys, "(k)=(1)", a key of -1 the next of the
 * expressions eval holds, an index's (index_eval_string()), evaluated on
 * the row; empty if the transaction does not see the row, or the table is
 * gone.  The column conv_at, 0-based, if one, is shown as conv, ALTER
 * COLUMN TYPE's transform, gives it from the row.
 */
static std::string dup_key(Oid relid, const std::vector<int32_t> &keys,
                           const std::string &eval, const std::string &row,
                           int32_t conv_at = -1, Expr *conv = NULL) {
  Relation rel = try_relation_open(relid, AccessShareLock);
  sm_tbl_t *tbl;
  std::string out;
  int natts = 0;
  List *exprs = NIL, *pred, *expr_names;

  if (rel == NULL)
    return out;
  tbl = open_table(relid, SM_LOCKMODE_IS);
  if (!eval.empty())
    smstore_index_eval_parts(eval.c_str(), &exprs, &pred, &expr_names);
  /* An expression may read any column, a transform among them. */
  natts = NIL != exprs || NULL != conv ? RelationGetDescr(rel)->natts : 0;
  for (int32_t k : keys)
    natts = Max(natts, k + 1);
  TupleTableSlot *slot =
      MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
  if (NULL != tbl && !row.empty() && read_row_at(tbl, row, natts, slot)) {
    EState *estate = CreateExecutorState();
    ExprContext *econtext = GetPerTupleExprContext(estate);
    std::string names, values;
    int e = 0;

    econtext->ecxt_scantuple = slot;
    for (int32_t k : keys) {
      Oid typid, output;
      Datum value;
      bool isnull, varlena;

      if (!names.empty()) {
        names += ", ";
        values += ", ";
      }
      if (k >= 0 && k == conv_at) {
        Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), k);

        names += quote_identifier(NameStr(att->attname));
        typid = exprType((Node *)conv);
        value = ExecEvalExprSwitchContext(ExecPrepareExpr(conv, estate),
                                          econtext, &isnull);
      } else if (k >= 0) {
        Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), k);

        names += quote_identifier(NameStr(att->attname));
        typid = att->atttypid;
        value = slot->tts_values[k];
        isnull = slot->tts_isnull[k];
      } else {
        Expr *expr = (Expr *)list_nth(exprs, e++);

        names += pg_get_index_expr_column((Node *)expr, relid);
        typid = exprType((Node *)expr);
        value = ExecEvalExprSwitchContext(ExecPrepareExpr(expr, estate),
                                          econtext, &isnull);
      }
      if (isnull) {
        values += "null";
        continue;
      }
      getTypeOutputInfo(typid, &output, &varlena);
      values += OidOutputFunctionCall(output, value);
    }
    out = "(" + names + ")=(" + values + ")";
    FreeExecutorState(estate);
  }
  ExecDropSingleTupleTableSlot(slot);
  relation_close(rel, AccessShareLock);
  return out;
}

/*
 * Raise the error of the unique index name whose build met a duplicate key:
 * the key, as PostgreSQL says it; why, sm's message, in its place if it
 * could not be read.
 */
[[noreturn]] static void
duplicate_build(const char *name, const std::string &key, const char *why) {
  ereport(ERROR,
          (errcode(ERRCODE_UNIQUE_VIOLATION),
           errmsg("could not create unique index \"%s\"", name),
           key.empty() ? errdetail("%s", why)
                       : errdetail("Key %s is duplicated.", key.c_str())));
  pg_unreachable();
}

/*
 * What ALTER COLUMN TYPE's converter evaluates for each row: the transform,
 * over a row of the table before the change, and the new type's length and
 * passing.
 */
struct alter_conv_t {
  EState *estate;
  ExprState *expr;
  TupleTableSlot *slot;
  int16 typlen;
  bool typbyval;
};

/*
 * sm's converter of ALTER COLUMN TYPE (sm_column_conv_fn, ../ALTERTYPE.md
 * section 3): the row's new value of the column, the transform evaluated by
 * PostgreSQL's executor, as ATRewriteTable() evaluates it.  An error is
 * caught and kept, never thrown through sm (keep_eval_error()).
 */
static int alter_conv(void *ctx, const sm_datum_t *values, const bool *isnull,
                      size_t n, std::string &out, bool *out_isnull) noexcept {
  alter_conv_t *c = static_cast<alter_conv_t *>(ctx);
  MemoryContext old = CurrentMemoryContext;
  int rc = 0;

  PG_TRY();
  {
    ExprContext *econtext = GetPerTupleExprContext(c->estate);
    TupleTableSlot *slot = c->slot;
    size_t natts = Min(n, (size_t)slot->tts_tupleDescriptor->natts);

    /* sm's datums are PostgreSQL's, bit for bit (../DECIMAL.md). */
    ExecClearTuple(slot);
    memcpy(slot->tts_values, values, natts * sizeof(Datum));
    memcpy(slot->tts_isnull, isnull, natts * sizeof(bool));
    ExecStoreVirtualTuple(slot);
    econtext->ecxt_scantuple = slot;
    Datum value = ExecEvalExprSwitchContext(c->expr, econtext, out_isnull);

    if (!*out_isnull)
      out = key_value_bytes(value, c->typlen, c->typbyval);
    ResetExprContext(econtext);
  }
  PG_CATCH();
  { rc = keep_eval_error(); }
  PG_END_TRY();
  MemoryContextSwitchTo(old);
  return rc;
}

/*
 * Whether the transform expr, of the column varattno, may change a value,
 * as PostgreSQL's ATColumnChangeRequiresRewrite() asks: anything but the
 * column itself, relabelled, may.  A function may, even one PostgreSQL
 * passes for timestamp and timestamptz, which sm stores as two types.
 */
static bool transform_changes(Node *expr, AttrNumber varattno) {
  for (;;) {
    if (IsA(expr, Var) && ((Var *)expr)->varattno == varattno)
      return false;
    if (!IsA(expr, RelabelType))
      return true;
    expr = (Node *)((RelabelType *)expr)->arg;
  }
}

/*
 * expr, a default as sm keeps it (nodeToString()), coerced to typid and
 * typmod as ATExecAlterColumnType() coerces a column's default, as sm is to
 * keep it.  what names the default in the error of one that cannot be.
 */
static std::string retyped_default(const std::string &expr, Oid typid,
                                   int32 typmod, const char *colname,
                                   const char *what) {
  Node *e = strip_implicit_coercions((Node *)stringToNode(expr.c_str()));
  Node *c = coerce_to_target_type(NULL, e, exprType(e), typid, typmod,
                                  COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST,
                                  -1);

  if (NULL == c)
    ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("%s for column \"%s\" cannot be cast "
                           "automatically to type %s",
                           what, colname, format_type_be(typid))));
  return nodeToString(c);
}

/*
 * Refuse ALTER COLUMN TYPE of the column at, 0-based, of the table relid,
 * whose definition is def, when what reads it would read it stale: a view,
 * in PostgreSQL's words; and, which digres does not make again yet
 * (../ALTERTYPE.md section 8), an expression or a partial index, a foreign
 * key on either side, an identity column.
 */
static void check_retype(Oid relid, const sm_table_def_t &def, int32_t at,
                         const char *colname) {
  auto in = [at](const std::vector<int32_t> &v) {
    return std::find(v.begin(), v.end(), at) != v.end();
  };
  std::vector<sm_view_reader_t> views;
  sm_session_t *s = xact_session();

  if (0 != sm_cat_view_readers(s, relid, views))
    sm_error("find the views of a table");
  for (const sm_view_reader_t &v : views)
    for (const sm_view_read_t &r : v.reads)
      if ((Oid)r.relation == relid && r.attnum == at)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter type of a column used by a view "
                               "or rule"),
                        errdetail("rule _RETURN on view %s depends on column "
                                  "\"%s\"",
                                  v.name.c_str(), colname)));
  for (const sm_index_def_t &x : def.indexes)
    if (!x.eval.empty() && in(x.reads))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ALTER COLUMN TYPE of a column an expression or "
                             "a partial index reads is not supported yet"),
                      errdetail("Index \"%s\" reads column \"%s\".",
                                x.name.c_str(), colname)));
  for (const sm_fkey_def_t &fk : def.fkeys)
    if (in(fk.columns))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("ALTER COLUMN TYPE of a column a foreign key "
                             "uses is not supported yet"),
                      errdetail("Foreign key \"%s\" uses column \"%s\".",
                                fk.name.c_str(), colname)));
  for (const sm_fkey_ref_t &ref : def.referenced_by) {
    sm_table_def_t child;

    if (!table_def_now((Oid)ref.table, child))
      continue;
    for (const sm_fkey_def_t &fk : child.fkeys)
      if (fk.oid == ref.fkey && in(fk.ref_columns))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("ALTER COLUMN TYPE of a column a foreign key "
                               "references is not supported yet"),
                        errdetail("Foreign key \"%s\" references column "
                                  "\"%s\".",
                                  fk.name.c_str(), colname)));
  }
  if (0 != def.columns[at].identity)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER COLUMN TYPE of an identity column is not "
                           "supported yet")));
}

/*
 * ALTER COLUMN ... TYPE, cmd, of the table relid named relname, as
 * PostgreSQL's ATPrepAlterColumnType() and ATExecAlterColumnType() do it
 * (../ALTERTYPE.md).
 * 1. Find the column, and refuse what would read it stale (check_retype())
 *    and a COLLATE, which CREATE TABLE refuses too.
 * 2. Make the transform: USING's expression, or the column, coerced to the
 *    new type, with PostgreSQL's error if it cannot be.
 * 3. Coerce the column's default and update default to the new type.
 * 4. Ask sm to change the record alone if no value changes, as PostgreSQL
 *    rewrites no row then; else to rewrite the rows, each converted by the
 *    transform (alter_conv()).
 * 5. Raise sm's failure in PostgreSQL's words.
 */
static void alter_column_type(Oid relid, const char *relname,
                              AlterTableCmd *cmd) {
  ColumnDef *coldef = castNode(ColumnDef, cmd->def);
  const char *colname = cmd->name;
  /* Locked, as parse analysis of USING opens the table assuming it is. */
  Relation rel = relation_open(relid, AccessShareLock);
  TupleDesc desc = RelationGetDescr(rel);
  AttrNumber attnum = InvalidAttrNumber;
  sm_table_def_t def;

  for (int i = 0; i < desc->natts; i++)
    if (!TupleDescAttr(desc, i)->attisdropped &&
        0 == strcmp(NameStr(TupleDescAttr(desc, i)->attname), colname))
      attnum = i + 1;
  if (InvalidAttrNumber == attnum)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" of relation \"%s\" does not exist",
                           colname, relname)));
  if (NULL != coldef->collClause)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("ALTER COLUMN TYPE ... COLLATE is not supported "
                           "yet")));
  if (!table_def_now(relid, def))
    elog(ERROR, "table %u is not in sm", relid);
  check_retype(relid, def, attnum - 1, colname);

  Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
  ParseState *pstate = make_parsestate(NULL);
  Oid typid;
  int32 typmod;
  Node *transform;

  typenameTypeIdAndMod(pstate, coldef->typeName, &typid, &typmod);
  /* A numeric with no precision is its type's default one, kept in sm. */
  if (is_decimal(typid))
    typmod = numeric_typmod(typid, typmod);
  /* The row is the table's before the change, as USING reads it. */
  ParseNamespaceItem *nsitem = addRangeTableEntryForRelation(
      pstate, rel, AccessShareLock, NULL, false, true);

  addNSItemToQuery(pstate, nsitem, false, true, true);
  if (NULL != coldef->raw_default)
    transform = transformExpr(pstate, coldef->raw_default,
                              EXPR_KIND_ALTER_COL_TRANSFORM);
  else
    transform = (Node *)makeVar(1, attnum, att->atttypid, att->atttypmod,
                                att->attcollation, 0);
  Node *coerced = coerce_to_target_type(pstate, transform, exprType(transform),
                                        typid, typmod, COERCION_ASSIGNMENT,
                                        COERCE_IMPLICIT_CAST, -1);

  if (NULL == coerced && NULL != coldef->raw_default)
    ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("result of USING clause for column \"%s\""
                           " cannot be cast automatically to type %s",
                           colname, format_type_be(typid)),
                    errhint("You might need to add an explicit cast.")));
  if (NULL == coerced)
    ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("column \"%s\" cannot be cast automatically to type %s",
                    colname, format_type_be(typid)),
             errhint("You might need to specify \"USING %s::%s\".",
                     quote_identifier(colname),
                     format_type_with_typemod(typid, typmod))));
  assign_expr_collations(pstate, coerced);
  /* Planned now, so a coercion that changes nothing is seen as such. */
  transform = (Node *)expression_planner((Expr *)coerced);
  free_parsestate(pstate);

  const sm_column_def_t &col = def.columns[attnum - 1];
  std::string new_default, new_update;
  sm_column_type_t to = {type_code(typid), typmod};

  if (col.has_default) {
    new_default =
        retyped_default(col.default_expr, typid, typmod, colname, "default");
    to.default_expr = new_default.c_str();
  }
  if (col.has_on_update) {
    new_update = retyped_default(col.on_update_expr, typid, typmod, colname,
                                 "update default");
    to.on_update_expr = new_update.c_str();
  }
  /*
   * No value changes, and sm stores them as they are: the same type, or
   * text and varchar (../../../../sm/sm2/TYPE.md section 3).
   */
  uint8_t was = col.type_code;
  bool texts = (TEXTOID == att->atttypid || VARCHAROID == att->atttypid) &&
               (TEXTOID == typid || VARCHAROID == typid);
  bool keep =
      !transform_changes(transform, attnum) && (was == to.type_code || texts);
  alter_conv_t conv = {};

  if (!keep) {
    conv.estate = CreateExecutorState();
    conv.expr = ExecPrepareExpr((Expr *)transform, conv.estate);
    conv.slot =
        MakeSingleTupleTableSlot(CreateTupleDescCopy(desc), &TTSOpsVirtual);
    get_typlenbyval(typid, &conv.typlen, &conv.typbyval);
  }
  relation_close(rel, NoLock);
  sm_session_t *s = xact_session();
  int rc = waiting([&] {
    return sm_cat_alter_column_type(s, relid, colname, to,
                                    keep ? nullptr : alter_conv, &conv);
  });

  if (!keep) {
    ExecDropSingleTupleTableSlot(conv.slot);
    FreeExecutorState(conv.estate);
  }
  /* The handles this transaction holds have the column's old type. */
  forget_table(relid);
  if (0 == rc)
    return;
  if (SM_ERR_NOTNULL == sm_errnum())
    ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION),
                    errmsg("column \"%s\" of relation \"%s\" contains null "
                           "values",
                           colname, relname)));
  if (SM_ERR_DUPKEY == sm_errnum()) {
    std::string why = sm_errmsg();
    std::string row;
    uint64_t index = 0;

    sm_errdup(&index, &row);
    for (const sm_index_def_t &x : def.indexes)
      if (x.oid == index)
        duplicate_build(x.name.c_str(),
                        dup_key(relid, x.key_attnums, x.eval, row, attnum - 1,
                                (Expr *)transform),
                        why.c_str());
  }
  sm_error("alter a column's type");
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

  List *exprs = NIL, *pred = NIL, *expr_names = NIL;
  std::vector<int32_t> reads;
  List *colnames;
  std::string eval;

  check_owner(relid, stmt->relation->relname, false);
  check_create_index(stmt);
  rel = relation_open(relid, NoLock);
  /* The INCLUDE columns name the index too, as DefineIndex() has it. */
  colnames = ChooseIndexColumnNames(
      list_concat_copy(stmt->indexParams, stmt->indexIncludingParams));
  foreach (l, stmt->indexParams) {
    IndexElem *elem = lfirst_node(IndexElem, l);
    sm_index_key_spec_t key = {};
    bool desc = elem->ordering == SORTBY_DESC;
    Node *expr = elem->expr;
    Oid typid, collation;

    /* "(column)" is the column, as ComputeIndexAttrs() takes it. */
    while (NULL != expr && IsA(expr, CollateExpr))
      expr = (Node *)((CollateExpr *)expr)->arg;
    if (NULL != expr && IsA(expr, Var) &&
        ((Var *)expr)->varattno > InvalidAttrNumber)
      key.attnum = ((Var *)expr)->varattno - 1;
    else if (NULL != expr) {
      key.attnum = -1;
      expr = elem->expr;
      check_index_expr(expr, "expression", reads);
      exprs = lappend(exprs, expr);
      expr_names = lappend(
          expr_names,
          makeString(pstrdup((char *)list_nth(colnames,
                                              foreach_current_index(l)))));
    } else
      key.attnum = column_place(rel, elem->name);
    if (key.attnum >= 0) {
      Form_pg_attribute att =
          TupleDescAttr(RelationGetDescr(rel), key.attnum);

      typid = att->atttypid;
      collation = att->attcollation;
    } else {
      typid = exprType(expr);
      key.typmod = exprTypmod(expr);
      key.type_code = type_code(typid);
      /*
       * A decimal is keyed at a scale, its typmod's, which an expression
       * of none would round its values to.
       */
      if (is_decimal(typid) && key.typmod < (int32)VARHDRSZ)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("an index expression of type numeric needs a "
                        "precision and scale"),
                 errhint("Cast the expression to numeric(p, s).")));
    }
    /*
     * An expression's own collation, "(column COLLATE c)"'s among them, as
     * check_index_key() takes one: a column's own, and for an expression
     * the default, the database's, C's order, which sm keys text in.
     * PostgreSQL would keep a COLLATE on top as the key's collation.
     */
    if (NULL != elem->expr) {
      Oid c = exprCollation(elem->expr);

      if (key.attnum >= 0 ? c != collation
                          : OidIsValid(c) && DEFAULT_COLLATION_OID != c)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("collation \"%s\" is not supported for an "
                               "index key yet",
                               get_collation_name(c))));
      collation = c;
    }
    check_index_key(elem, typid, collation);
    /*
     * sm keys an interval in an order finer than PostgreSQL's: '1 mon' and
     * '30 days', which PostgreSQL calls equal, are two keys
     * (../sm/sm2/TYPE.md section 3.10), so = through the index would miss
     * one and UNIQUE would take both.  So an interval is never an index
     * key; it may be an INCLUDE column.
     */
    if (typid == INTERVALOID)
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
  if (NULL != stmt->whereClause) {
    check_index_expr(stmt->whereClause, "predicate", reads);
    pred = make_ands_implicit((Expr *)stmt->whereClause);
  }
  if (NIL != exprs || NIL != pred)
    eval = index_eval_string(exprs, pred, expr_names);
  name = stmt->idxname != NULL
             ? stmt->idxname
             : ChooseIndexName(RelationGetRelationName(rel),
                               RelationGetNamespace(rel),
                               colnames, NIL, stmt->primary,
                               stmt->isconstraint);
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
                                   eval, reads, &oid, stmt->primary,
                                   stmt->isconstraint, tree_kind(stmt));
      }) != 0) {
    /* A name taken since it was looked up, or a unique key duplicated. */
    if (sm_errnum() == SM_ERR_DUPKEY && !stmt->unique)
      ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE),
                      errmsg("relation \"%s\" already exists", name)));
    if (sm_errnum() == SM_ERR_DUPKEY) {
      std::string why = sm_errmsg();
      std::string row = error_dup_row();
      std::vector<int32_t> places;

      for (const sm_index_key_spec_t &k : keys)
        places.push_back(k.attnum);
      duplicate_build(name, dup_key(relid, places, eval, row), why.c_str());
    }
    sm_error("create an index");
  }
  /*
   * The handles this transaction holds were opened without the index, so
   * would neither write it nor describe it (sm_tbl_def()); a primary key
   * also keys the table's rows, which sm has keyed anew, maybe in a new
   * store (../sm/CLUSTER.md section 8).
   */
  forget_table(relid);
  made_index_t made = {relid, oid, name, stmt->unique, {}, eval};

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
 * (sm_cat_create_index()).
 * 1. Wait for each index's build in turn.
 * 2. Drop an index whose build failed, in a transaction of its own.
 * 3. Raise the build's error, so CREATE INDEX ends as PostgreSQL's does:
 *    with an index or with none (DECISION.md).
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
    std::string row = error_dup_row();
    std::string key;

    /*
     * A failed index is good only for dropping: its own transaction does,
     * reading first the key a duplicate's row holds.
     */
    StartTransactionCommand();
    if (errnum == SM_ERR_DUPKEY && m.unique)
      key = dup_key(m.table, m.keys, m.eval, row);
    if (waiting([&] {
          return sm_cat_drop_index(xact_session(), m.table, m.index);
        }) != 0 &&
        sm_errnum() != SM_ERR_NOTFOUND)
      sm_error("drop an index whose build failed");
    CommitTransactionCommand();
    if (errnum == SM_ERR_DUPKEY && m.unique)
      duplicate_build(m.name.c_str(), key, why.c_str());
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
 * Drop the index of the table, from sm, in this transaction.  A table's
 * descriptor forgets the index in every session at commit.
 */
static void drop_index(uint64_t table, uint64_t index) {
  Relation rel;

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
  /*
   * A primary key's drop keys the rows anew, in a new store
   * (../sm/CLUSTER.md section 8), which the handles this transaction holds
   * do not know; another index's leaves them be, and they reopen at no
   * more cost than this.
   */
  forget_table((Oid)table);
}

/*
 * Drop the indexes a DROP INDEX names, from sm, in this transaction.  The
 * index of a primary key or a unique constraint is the constraint's, which
 * DROP CONSTRAINT drops, as PostgreSQL has it.
 */
void smstore_drop_indexes(DropStmt *stmt) {
  ListCell *l;

  if (stmt->concurrent)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DROP INDEX CONCURRENTLY is not supported")));
  foreach (l, stmt->objects) {
    RangeVar *rv = makeRangeVarFromNameList((List *)lfirst(l));
    uint64_t table, index;
    sm_table_def_t def;

    if (!find_index(rv, &table, &index)) {
      if (!stmt->missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("index \"%s\" does not exist", rv->relname)));
      ereport(NOTICE,
              (errmsg("index \"%s\" does not exist, skipping", rv->relname)));
      continue;
    }
    check_owner((Oid)index, rv->relname, true);
    if (table_def_now((Oid)table, def))
      for (const sm_index_def_t &x : def.indexes)
        if (x.oid == index && (x.primary || x.constraint))
          ereport(ERROR,
                  (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                   errmsg("cannot drop index %s because constraint %s on "
                          "table %s requires it",
                          x.name.c_str(), x.name.c_str(), def.name.c_str()),
                   errhint("You can drop constraint %s on table %s instead.",
                           x.name.c_str(), def.name.c_str())));
    drop_index(table, index);
  }
}

/*
 * Whether this transaction may scan rel's index indexoid now: built, valid,
 * and valid since a count its snapshot is not older than.  An index found
 * usable stays so while the catalog's epoch holds (SCHEMACACHE.md section
 * 4.6): a valid index stays valid until dropped, which moves the epoch, and
 * a snapshot of this session's is never older than one before it.  One not
 * usable is asked again each time: its build may end, or a newer snapshot
 * see it.
 */
bool smstore_index_usable(Relation rel, Oid indexoid) {
  auto &usable = tcb->smstore->usable;
  /* Read before the lookup, so a publish meanwhile retires what it finds. */
  uint64_t epoch = sm_cat_epoch(session());

  if (UINT64_MAX != epoch) {
    auto it = usable.find(indexoid);

    if (usable.end() != it && epoch == it->second)
      return true;
  }
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);

  if (NULL == tbl || !sm_tbl_index_usable(tbl, indexoid))
    return false;
  /* None kept while the transaction has DDL of its own. */
  if (UINT64_MAX != epoch) {
    /* Past the limit, they are forgotten, and asked again as needed. */
    if (usable.size() >= statistic_cache_max)
      usable.clear();
    usable[indexoid] = epoch;
  }
  return true;
}

/* Whether type is one of the integers, int2, int4 and int8. */
static bool int_type(Oid type) {
  return INT2OID == type || INT4OID == type || INT8OID == type;
}

/* The integer d, of the integer type type, widened. */
static int64 int_value(Datum d, Oid type) {
  if (INT2OID == type)
    return DatumGetInt16(d);
  if (INT4OID == type)
    return DatumGetInt32(d);
  return DatumGetInt64(d);
}

/*
 * A key of an integer type other than its column's, an int4 constant on an
 * int8 column, as most are: its value as a datum of the column's type, which
 * an index bound takes; false if the value does not fit it.
 */
static bool int_key_datum(ScanKey key, Oid coltype, Datum *out) {
  int64 v = int_value(key->sk_argument, key->sk_subtype);

  if (INT2OID == coltype && (v < PG_INT16_MIN || v > PG_INT16_MAX))
    return false;
  if (INT4OID == coltype && (v < PG_INT32_MIN || v > PG_INT32_MAX))
    return false;
  *out = INT2OID == coltype   ? Int16GetDatum((int16)v)
         : INT4OID == coltype ? Int32GetDatum((int32)v)
                              : Int64GetDatum(v);
  return true;
}

/*
 * Whether a scan key is a plain comparison with a value, usable as a bound:
 * of the column's type, or an integer of another integer type that the
 * column's holds.  The others are checked of each row alone (keys_met()).
 */
static bool plain_key(ScanKey key, Oid coltype) {
  Datum d;

  if (0 != (key->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_SEARCHARRAY |
                             SK_SEARCHNULL | SK_SEARCHNOTNULL)))
    return false;
  if (InvalidOid == key->sk_subtype || coltype == key->sk_subtype)
    return true;
  return int_type(coltype) && int_type(key->sk_subtype) &&
         int_key_datum(key, coltype, &d);
}

/*
 * Whether the row in slot meets every scan key: each compares the row's
 * value in the key's column, by the key's operator, with the key's value.
 * A null meets only IS NULL.  A key the scan's bounds hold is not checked
 * (smstore_index_beginscan()).  With index_shaped, slot holds an index
 * entry's columns, not a row's.
 */
static bool keys_met(smstore_scan_t *scan, TupleTableSlot *slot,
                     bool index_shaped) {
  if (0 == scan->nchecked)
    return true;
  for (int i = 0; i < scan->nkeys; i++) {
    ScanKey key = &scan->keys[i];
    int col = (index_shaped ? key->sk_attno : scan->keycols[i]) - 1;
    bool isnull;

    if (scan->bounded[i])
      continue;

    /* A row's slot deforms as it is read: up to the key's column. */
    slot_getsomeattrs(slot, col + 1);
    isnull = slot->tts_isnull[col];

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
    /* An array's key is met by any of its elements. */
    if (key->sk_flags & SK_SEARCHARRAY) {
      bool met = false;

      for (int e = 0; e < scan->nelems[i] && !met; e++)
        met = DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
                                             slot->tts_values[col],
                                             scan->elems[i][e]));
      if (!met)
        return false;
      continue;
    }
    if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
                                        slot->tts_values[col],
                                        key->sk_argument)))
      return false;
  }
  return true;
}

/*
 * The elements of an array key's array but its nulls, palloc'd, into
 * *elems; their number.
 */
static int array_elems(ScanKey key, Datum **elems) {
  ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
  int16 elmlen;
  bool elmbyval;
  char elmalign;
  Datum *vals;
  bool *nulls;
  int n, kept = 0;

  get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
  deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign, &vals,
                    &nulls, &n);
  for (int i = 0; i < n; i++)
    if (!nulls[i])
      vals[kept++] = vals[i];
  *elems = vals;
  return kept;
}

/*
 * Open the index scan's probe at its bounds now (../INLIST.md): with point,
 * the row an equality on each of the primary key's columns names, read by
 * its key; else a range of the primary key's rows or of another index's
 * entries.  A tree's cursor, once open, is aimed at the probe's bounds;
 * any other last probe's cursor is closed first.
 */
static void open_probe(smstore_scan_t *scan) {
  /* A tree's cursor is aimed at the next probe's bounds, not made anew. */
  if (scan->tree && scan->cursor != NULL) {
    if (sm_rel_scan_rebound(scan->cursor, scan->lo.n > 0 ? &scan->lo : NULL,
                            scan->hi.n > 0 ? &scan->hi : NULL) != 0)
      sm_error("scan an index");
    return;
  }
  sm_rel_scanend(scan->cursor);
  sm_idx_scanend(scan->icursor);
  scan->cursor = NULL;
  scan->icursor = NULL;
  /*
   * An equality on each of the primary key's columns names one row: read it
   * by its key, as a cursor would see it now, with none opened.
   */
  if (scan->point) {
    sm_row_key_t key = {scan->lo_vals, scan->lo.isnull,
                        (size_t)scan->form->indnkeyatts};
    std::string bytes;
    const void *tuple;
    size_t len;

    if (0 != sm_tbl_row_key(scan->tbl, key, bytes))
      sm_error("name a row by its key");
    scan->point_row = false;
    if (scan->point_key)
      pfree(scan->point_key);
    scan->point_key = (char *)palloc(Max(bytes.size(), 1));
    memcpy(scan->point_key, bytes.data(), bytes.size());
    scan->point_len = bytes.size();
    switch (sm_tbl_read_key(scan->tbl, bytes.data(), bytes.size(), &scan->ref,
                            &tuple, &len)) {
    case 0:
      scan->point_row = true;
      break;
    case 1:
      break;
    default:
      sm_error("read a row");
    }
    return;
  }
  /*
   * The primary key's entries are the rows, keyed by it: read in place.
   * A tree's are its rows too, each read and rechecked by sm.
   */
  if (scan->form->indisprimary || scan->tree
          ? sm_tbl_idx_scan_rows(scan->tbl, scan->index,
                                 scan->lo.n > 0 ? &scan->lo : NULL,
                                 scan->hi.n > 0 ? &scan->hi : NULL, NULL,
                                 &scan->cursor) != 0
          : sm_tbl_idx_scan(scan->tbl, scan->index,
                            scan->lo.n > 0 ? &scan->lo : NULL,
                            scan->hi.n > 0 ? &scan->hi : NULL, NULL,
                            &scan->icursor) != 0)
    sm_error("scan an index");
  if (scan->entries_only && 0 != sm_rel_scan_entries(scan->cursor))
    sm_error("scan an index");
}

/*
 * Set the index scan's bounds to its next probe's: the next combination of
 * its arrays' values, the last array's first; false after the last.
 */
static bool advance_probe(smstore_scan_t *scan) {
  int a = scan->narrays - 1;

  while (a >= 0 && ++scan->arr_at[a] == scan->arr_n[a])
    scan->arr_at[a--] = 0;
  if (a < 0)
    return false;
  for (int i = 0; i < scan->narrays; i++) {
    scan->lo_vals[scan->arr_col[i]] = scan->arr_lo[i][scan->arr_at[i]];
    scan->hi_vals[scan->arr_col[i]] = scan->arr_hi[i][scan->arr_at[i]];
  }
  return true;
}

/* Open the index scan's next probe; false after the last. */
static bool next_probe(smstore_scan_t *scan) {
  if (!advance_probe(scan))
    return false;
  open_probe(scan);
  return true;
}

/*
 * Begin a scan of index, a btree of heap, a table in sm, for the rows that
 * meet keys: btree scan keys on the index's columns, each row deformed as it
 * is read.  Equal keys on its
 * leading columns are a prefix both bounds share; the next column's ranges
 * bound it below and above, in the index's own order, so a DESC column's
 * upper value is its lower bound.  An array's equality on a column of the
 * prefix gives it its values, the scan one probe a value, in the index's
 * order (../INLIST.md).  Every row read is checked against every
 * key, so a key that bounds nothing still holds; but not a key the bounds
 * hold exactly, one of a primary key's or a tree's integer columns: its
 * rows, keyed by those columns, are read only between the bounds
 * (../sm/sm2/SM2_DESIGN.md section 5, a scan's bounds), and none is null.
 */
smstore_scan_t *smstore_index_beginscan(Relation heap, Relation index,
                                        ScanKey keys, int nkeys) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(heap), SM_LOCKMODE_IS);
  Form_pg_index form = index->rd_index;
  int ncols = form->indnkeyatts;
  sm_datum_t *lo_vals = (sm_datum_t *)palloc0(ncols * sizeof(sm_datum_t));
  sm_datum_t *hi_vals = (sm_datum_t *)palloc0(ncols * sizeof(sm_datum_t));
  bool *nulls = (bool *)palloc0(ncols * sizeof(bool));
  bool *bounded = (bool *)palloc0(Max(nkeys, 1) * sizeof(bool));
  sm_bound_t lo = {lo_vals, nulls, 0, true};
  sm_bound_t hi = {hi_vals, nulls, 0, true};
  int neq = 0; /* the leading columns an equality gives one value */
  smstore_scan_t *scan;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(heap))));
  /* A tree's rows, as a primary key's, each hold its bounds (sm3). */
  bool tree = sm_tbl_index_tree(tbl, RelationGetRelid(index));

  for (int i = 0; i < nkeys; i++)
    if (keys[i].sk_flags & SK_ROW_HEADER)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("an index scan in sm takes no row comparison "
                             "yet")));

  scan = (smstore_scan_t *)palloc0(sizeof(smstore_scan_t));
  scan->arr_col = (int *)palloc0(ncols * sizeof(int));
  scan->arr_n = (int *)palloc0(ncols * sizeof(int));
  scan->arr_at = (int *)palloc0(ncols * sizeof(int));
  scan->arr_lo = (sm_datum_t **)palloc0(ncols * sizeof(sm_datum_t *));
  scan->arr_hi = (sm_datum_t **)palloc0(ncols * sizeof(sm_datum_t *));
  scan->elems = (Datum **)palloc0(Max(nkeys, 1) * sizeof(Datum *));
  scan->nelems = (int *)palloc0(Max(nkeys, 1) * sizeof(int));
  /* Each array's elements, but a null array's: it meets no row. */
  for (int i = 0; i < nkeys; i++)
    if ((keys[i].sk_flags & SK_SEARCHARRAY) &&
        !(keys[i].sk_flags & SK_ISNULL))
      scan->nelems[i] = array_elems(&keys[i], &scan->elems[i]);

  for (int c = 0; c < ncols; c++) {
    Oid coltype = index->rd_opcintype[c];
    bool desc = (index->rd_indoption[c] & INDOPTION_DESC) != 0;
    ScanKey eq = NULL, below = NULL, above = NULL, in = NULL;
    Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), c);
    /*
     * A key's datum as sm takes it: a numeric's integer at the scale, an
     * integer of another integer type the column's (plain_key()).
     */
    auto bound = [&](ScanKey key, bool upper) {
      Datum d;

      if (OidIsValid(key->sk_subtype) && coltype != key->sk_subtype &&
          int_type(coltype) && int_key_datum(key, coltype, &d))
        return (sm_datum_t)d;
      if (!is_decimal(att->atttypid))
        return (sm_datum_t)key->sk_argument;
      return numeric_bound(key->sk_argument,
                           OidIsValid(key->sk_subtype) ? key->sk_subtype
                                                       : att->atttypid,
                           att->atttypid, att->atttypmod, upper,
                           (uint8 *)palloc(16));
    };
    bool numeric_col = is_decimal(att->atttypid);
    /* Whether a key bounding this column holds of every row read. */
    bool exact = (form->indisprimary || tree) && int_type(coltype);

    for (int i = 0; i < nkeys; i++) {
      ScanKey key = &keys[i];

      /*
       * An array's equality, its elements of the column's type or an
       * integer's, gives the column values if no plain equality does.
       */
      if (key->sk_attno == c + 1 && (key->sk_flags & SK_SEARCHARRAY) &&
          key->sk_strategy == BTEqualStrategyNumber &&
          (!OidIsValid(key->sk_subtype) || coltype == key->sk_subtype ||
           (int_type(coltype) && int_type(key->sk_subtype))))
        in = key;
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
      bounded[eq - keys] = exact;
      /* A numeric's bounds may differ: no later column narrows them. */
      if (numeric_col && lo_vals[c] != hi_vals[c]) {
        lo.inclusive = hi.inclusive = true;
        break;
      }
      neq++;
      continue;
    }
    if (in != NULL) {
      int a = scan->narrays++, n = 0;
      int k = in - keys;
      Datum *vals = (Datum *)palloc(Max(scan->nelems[k], 1) * sizeof(Datum));
      /* The column's operator class is its type's default btree one. */
      FmgrInfo *cmp =
          &lookup_type_cache(coltype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
      Oid coll = index->rd_indcollation[c];
      bool spread = false;

      /* Each element as the column's datum; one out of its range, none. */
      for (int e = 0; e < scan->nelems[k]; e++) {
        ScanKeyData one = *in;

        one.sk_argument = scan->elems[k][e];
        one.sk_flags &= ~SK_SEARCHARRAY;
        if (!OidIsValid(in->sk_subtype) || coltype == in->sk_subtype)
          vals[n++] = one.sk_argument;
        else if (int_key_datum(&one, coltype, &vals[n]))
          n++;
      }
      /* In the index's order, each value once. */
      std::sort(vals, vals + n, [&](Datum x, Datum y) {
        int32 r = DatumGetInt32(FunctionCall2Coll(cmp, coll, x, y));
        return desc ? r > 0 : r < 0;
      });
      int m = 0;
      for (int e = 0; e < n; e++)
        if (0 == m ||
            0 != DatumGetInt32(FunctionCall2Coll(cmp, coll, vals[m - 1],
                                                 vals[e])))
          vals[m++] = vals[e];
      scan->arr_col[a] = c;
      scan->arr_n[a] = m;
      scan->arr_lo[a] = (sm_datum_t *)palloc(Max(m, 1) * sizeof(sm_datum_t));
      scan->arr_hi[a] = (sm_datum_t *)palloc(Max(m, 1) * sizeof(sm_datum_t));
      for (int e = 0; e < m; e++) {
        ScanKeyData one = *in;

        one.sk_argument = vals[e];
        one.sk_subtype = coltype;
        one.sk_flags &= ~SK_SEARCHARRAY;
        scan->arr_lo[a][e] = bound(&one, false);
        scan->arr_hi[a][e] = bound(&one, true);
        spread |= scan->arr_lo[a][e] != scan->arr_hi[a][e];
      }
      if (0 == m) {
        scan->empty = true;
        break;
      }
      lo_vals[c] = scan->arr_lo[a][0];
      hi_vals[c] = scan->arr_hi[a][0];
      lo.n = hi.n = c + 1;
      bounded[k] = exact;
      /* A numeric's bounds may differ: no later column narrows them. */
      if (numeric_col && spread) {
        lo.inclusive = hi.inclusive = true;
        break;
      }
      neq++;
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
      bounded[first - keys] = exact;
    }
    if (last != NULL) {
      hi_vals[c] = bound(last, last == above);
      hi.n = c + 1;
      hi.inclusive = numeric_col ||
                     last->sk_strategy == BTGreaterEqualStrategyNumber ||
                     last->sk_strategy == BTLessEqualStrategyNumber;
      bounded[last - keys] = exact;
    }
    break;
  }

  scan->tbl = tbl;
  scan->relid = RelationGetRelid(heap);
  scan->keys = keys;
  scan->nkeys = nkeys;
  scan->bounded = bounded;
  scan->index = RelationGetRelid(index);
  scan->tree = tree;
  scan->form = form;
  scan->lo = lo;
  scan->hi = hi;
  scan->lo_vals = lo_vals;
  scan->hi_vals = hi_vals;
  scan->keycols = (AttrNumber *)palloc(Max(nkeys, 1) * sizeof(AttrNumber));
  /* Each row is checked against every key not bounded (keys_met()). */
  for (int i = 0; i < nkeys; i++) {
    scan->keycols[i] = form->indkey.values[keys[i].sk_attno - 1];
    if (0 == scan->keycols[i]) {
      bounded[i] = true;
      scan->recheck = true;
    }
    if (!bounded[i])
      scan->nchecked++;
  }
  /* An equality on each of the primary key's columns: a read by key. */
  scan->point = form->indisprimary && ncols == neq;
  /*
   * Rows read ahead, many a call (../sm/BATCHREAD.md): a secondary index's
   * entries' rows, and an array's probes of the whole primary key, whose
   * first probe is read with the rest.
   */
  scan->ra_rows = !form->indisprimary && !scan->tree;
  scan->ra_point = scan->point && scan->narrays > 0;
  scan->ra_batch = 1;
  if (!scan->empty && !scan->ra_point)
    open_probe(scan);
  open_scans.push_back(scan);
  return scan;
}

/* Keep len bytes of key in the read-ahead's place i, grown as needed. */
static void ra_keep_key(smstore_scan_t *scan, int i, const void *key,
                        size_t len) {
  if (scan->ra_kcap[i] < len) {
    size_t cap = Max(len, 64);

    /* In the scan's own context, as it lives as long as the scan. */
    scan->ra_key[i] =
        scan->ra_key[i] != NULL
            ? (char *)repalloc(scan->ra_key[i], cap)
            : (char *)MemoryContextAlloc(GetMemoryChunkContext(scan), cap);
    scan->ra_kcap[i] = cap;
  }
  memcpy(scan->ra_key[i], key, len);
  scan->ra_klen[i] = len;
}

/*
 * Read the rows of the first n keys kept, in one call, to be handed out
 * from the first; the next read is twice as many, up to read_batch_max.
 */
static void ra_read(smstore_scan_t *scan, int n) {
  const void *keys[read_batch_max];
  const void *tuples[read_batch_max];
  size_t tuple_lens[read_batch_max];

  for (int i = 0; i < n; i++)
    keys[i] = scan->ra_key[i];
  if (sm_tbl_read_keys(scan->tbl, keys, scan->ra_klen, (size_t)n,
                       scan->ra_ref, tuples, tuple_lens, scan->ra_rc) != 0)
    sm_error("read rows");
  scan->ra_n = n;
  scan->ra_at = 0;
  scan->ra_batch = Min(scan->ra_batch * 2, read_batch_max);
}

/*
 * The next row read ahead that the transaction sees, made the row the scan
 * is on: its pin in ref, as a single read would leave it, its rowno and its
 * key the scan's.  False if every row read ahead is handed out.
 */
static bool ra_next(smstore_scan_t *scan) {
  while (scan->ra_at < scan->ra_n) {
    int i = scan->ra_at++;

    if (scan->ra_rc[i] != 0)
      continue;
    /* The row handed out before is no one's now. */
    sm_unpin(&scan->ref);
    scan->ref = scan->ra_ref[i];
    scan->ra_ref[i] = sm_pin_ref_t{};
    scan->cur = true;
    scan->cur_pinned = true;
    scan->cur_rowno = scan->ra_rowno[i];
    scan->cur_key = scan->ra_key[i];
    scan->cur_klen = scan->ra_klen[i];
    return true;
  }
  return false;
}

/*
 * A secondary index's scan's next row that meets its keys into slot, by its
 * pin, its tid its rowno; false at the end.  Its entries' rows are read
 * ahead, many in one call (../sm/BATCHREAD.md), their keys copied as the
 * cursor moves on.  Out of line, so a primary key's scan pays for none of it.
 */
static pg_noinline bool next_read_row(smstore_scan_t *scan,
                                      TupleTableSlot *slot) {
  for (;;) {
    ExecClearTuple(slot);
    if (ra_next(scan)) {
      ref_into_slot(scan->ref, scan->cur_rowno, slot);
      if (keys_met(scan, slot, false))
        return true;
      continue;
    }
    int n = 0;

    scan->ra_n = scan->ra_at = 0;
    /* Its entries ended: asked again, the probes would begin again. */
    if (scan->ra_done)
      return false;
    while (n < scan->ra_batch) {
      const void *key;
      size_t klen;

      if (!smstore_index_getrowno(scan)) {
        scan->ra_done = true;
        break;
      }
      if (sm_idx_scan_row(scan->icursor, &key, &klen) != 0)
        sm_error("read an index entry");
      ra_keep_key(scan, n, key, klen);
      scan->ra_rowno[n++] = scan->cur_rowno;
    }
    /* On no row until one read ahead is handed out. */
    scan->cur = false;
    if (n == 0)
      return false;
    ra_read(scan, n);
  }
}

/*
 * An array's probes of the whole primary key: the next row they name that
 * the transaction sees, made the row the scan is on; false after the last.
 * The probes' keys are made from their bounds and their rows read ahead,
 * many in one call (../sm/BATCHREAD.md).
 */
static pg_noinline bool next_point_row(smstore_scan_t *scan) {
  for (;;) {
    if (ra_next(scan))
      return true;
    scan->ra_n = scan->ra_at = 0;
    if (scan->ra_done)
      return false;
    int n = 0;

    while (n < scan->ra_batch) {
      sm_row_key_t key = {scan->lo_vals, scan->lo.isnull,
                          (size_t)scan->form->indnkeyatts};
      std::string bytes;

      if (0 != sm_tbl_row_key(scan->tbl, key, bytes))
        sm_error("name a row by its key");
      ra_keep_key(scan, n, bytes.data(), bytes.size());
      /* A row read by its key has no rowno. */
      scan->ra_rowno[n++] = 0;
      if (!advance_probe(scan)) {
        scan->ra_done = true;
        break;
      }
    }
    ra_read(scan, n);
  }
}

/*
 * The row at key, a key of the table past its relid, as this transaction
 * sees it, into slot, a materialized virtual tuple of its first natts
 * columns, with no rowno; false, the slot empty, if it sees none.
 */
static bool read_row_at(sm_tbl_t *tbl, const std::string &key, int natts,
                        TupleTableSlot *slot) {
  sm_pin_ref_t ref = {};
  const void *tuple;
  size_t len;

  ExecClearTuple(slot);
  switch (sm_tbl_read_key(tbl, key.data(), key.size(), &ref, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a row");
  }
  tuple_into_slot(tbl, 0, tuple, len, natts, slot);
  ExecMaterializeSlot(slot);
  /* The slot has its copy: the row's pin is no one's now. */
  sm_unpin(&ref);
  return true;
}

/*
 * Whether the executor is to check each row the index scan gives against
 * its quals: a key on an expression, which the scan does not check.
 */
bool smstore_index_recheck(smstore_scan_t *scan) { return scan->recheck; }

/*
 * The index scan's next row that meets its keys into slot, by its pin, its
 * tid its rowno; false at the end.  A secondary index's rows are read ahead
 * (next_read_row()).
 */
bool smstore_index_getnext(smstore_scan_t *scan, TupleTableSlot *slot) {
  slot->tts_tableOid = scan->relid;
  if (scan->ra_rows)
    return next_read_row(scan, slot);
  while (smstore_index_getrowno(scan)) {
    /* A primary key's entry is its row, in the cursor's pin or the scan's. */
    ExecClearTuple(slot);
    ref_into_slot(scan->point ? scan->ref : scan->row_ref, scan->cur_rowno,
                  slot);
    if (keys_met(scan, slot, false))
      return true;
  }
  ExecClearTuple(slot);
  return false;
}

/*
 * smstore_index_getrowno() of a secondary index's scan.  Out of line, so
 * a primary key's scan takes no address of a local, and pays for no stack
 * protector.
 */
static pg_noinline bool next_entry(smstore_scan_t *scan) {
  bool cut;

  switch (sm_idx_scannxt(scan->icursor, &scan->cur_rowno, &cut, &scan->value,
                         &scan->len)) {
  case 0:
    /* Its row is pinned once read (next_read_row(), entry_from_row()). */
    scan->cur = true;
    scan->cur_pinned = false;
    return true;
  case 1:
    return false;
  default:
    sm_error("scan an index");
  }
  return false; /* not reached */
}

/*
 * The open probe's next entry, its rowno into scan->cur_rowno; false at
 * the probe's end (smstore_index_getrowno()).
 */
static inline bool probe_row(smstore_scan_t *scan) {
  /* The one row a primary key's equalities name, once; a keyed row's is 0. */
  if (scan->point) {
    if (!scan->point_row)
      return false;
    scan->point_row = false;
    scan->cur = true;
    scan->cur_pinned = true;
    scan->cur_rowno = 0;
    return true;
  }
  /* A primary key's: its next row, into the cursor's pin. */
  if (scan->cursor != NULL) {
    switch (sm_rel_scannxt(scan->cursor, &scan->cur_rowno, &scan->row_ref)) {
    case 0:
      scan->cur = true;
      scan->cur_pinned = true;
      return true;
    case 1:
      return false;
    default:
      sm_error("scan an index");
    }
  }
  return next_entry(scan);
}

/*
 * The index scan's next entry, its rowno into scan->cur_rowno, the row
 * unread; false at the end.  The entry is within the scan's bounds, but its
 * row may be one the transaction does not see, or not meet every key: who
 * reads the row checks.  When a probe ends, the next opens (../INLIST.md).
 */
bool smstore_index_getrowno(smstore_scan_t *scan) {
  CHECK_FOR_INTERRUPTS();
  scan->cur = false;
  if (scan->empty)
    return false;
  /* An array's probes of the whole primary key, their rows read ahead. */
  if (scan->ra_point)
    return next_point_row(scan);
  for (;;) {
    if (probe_row(scan))
      return true;
    if (0 == scan->narrays || !next_probe(scan))
      return false;
  }
}

/*
 * The entry's INCLUDE columns, from its value, into the slot's columns
 * after the key's.  The value holds the cut flag first, and the row's key
 * last.
 */
static void entry_includes(smstore_scan_t *scan, TupleTableSlot *slot) {
  int nkeys = scan->form->indnkeyatts;

  /* No INCLUDE columns: the value holds none for the slot. */
  if (nkeys == scan->form->indnatts)
    return;
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
    slot->tts_values[i] = (Datum)scan->entry_values[1 + i - nkeys];
    slot->tts_isnull[i] = scan->entry_isnull[1 + i - nkeys];
  }
}

/*
 * The columns the index holds of the row of the scan's last entry into
 * slot, in the index's order: false if the transaction does not see the
 * row.  For an entry whose key does not give its columns back, and for a
 * primary key's, which is its row.  Each is decoded alone, so the row's
 * other columns cost nothing (../sm/sm2/TUPLE.md section 6).
 */
static bool entry_from_row(smstore_scan_t *scan, TupleTableSlot *slot) {
  const void *key, *tuple;
  size_t klen, len;

  if (scan->cursor != NULL || scan->point) {
    /* The row, in its pin, which the slot's values point into. */
    if (sm_pin_tuple(scan->point ? scan->ref : scan->row_ref, &tuple, &len) !=
        0)
      sm_error("read a row");
  } else {
    /* Into the scan's pin, which the slot's values then point into. */
    if (sm_idx_scan_row(scan->icursor, &key, &klen) != 0)
      sm_error("read an index entry");
    switch (sm_tbl_read_key(scan->tbl, key, klen, &scan->ref, &tuple, &len)) {
    case 0:
      break;
    case 1:
      return false;
    default:
      sm_error("read a row");
    }
    scan->cur_pinned = true;
  }
  for (int i = 0; i < scan->form->indnatts; i++)
    if (sm_tbl_decode(scan->tbl, tuple, len, scan->form->indkey.values[i] - 1,
                      (sm_datum_t *)&slot->tts_values[i],
                      &slot->tts_isnull[i]) != 0)
      sm_error("read a row");
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
  slot->tts_tableOid = scan->relid;
  /*
   * A tree of key columns alone gives them from its entries, its rows
   * unread on a leaf all visible; one with INCLUDE columns reads them from
   * the row, which it holds no copy of.
   */
  if (scan->tree && !scan->entries_only &&
      scan->form->indnatts == scan->form->indnkeyatts) {
    scan->entries_only = true;
    if (NULL != scan->cursor && 0 != sm_rel_scan_entries(scan->cursor))
      sm_error("scan an index");
  }
  while (smstore_index_getrowno(scan)) {
    uint64 rowno = scan->cur_rowno;

    ExecClearTuple(slot);
    switch (scan->point ? 1
            : scan->cursor != NULL
                ? (scan->entries_only
                       ? sm_rel_scan_cols(scan->cursor,
                                          (sm_datum_t *)slot->tts_values,
                                          slot->tts_isnull,
                                          scan->form->indnkeyatts)
                       : 1)
                : sm_idx_scan_key(scan->icursor, (sm_datum_t *)slot->tts_values,
                                  slot->tts_isnull, scan->form->indnkeyatts)) {
    case 0:
      entry_includes(scan, slot);
      break;
    case 1:
      /* Its row, pinned until the scan reads the next. */
      if (!entry_from_row(scan, slot))
        continue;
      break;
    default:
      sm_error("read an index entry");
    }
    ExecStoreVirtualTuple(slot);
    slot->tts_rowno = rowno;
    if (keys_met(scan, slot, true))
      return true;
  }
  ExecClearTuple(slot);
  return false;
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
 * Keep the row at key, encoded, of the table oid for the statement: its
 * tuple, the row the caller pinned in kept_ref, or none yet if tuple is
 * NULL.
 */
static void keep_row(Oid oid, const std::string &key, const void *tuple,
                     size_t len) {
  /* Until the statement ends, or the transaction. */
  if (NULL == tuple)
    sm_unpin(&kept_ref);
  kept_oid = oid;
  kept_key = key;
  kept_tuple = (const char *)tuple;
  kept_len = len;
}

/* Whether the row at key, encoded, of the table oid is kept, read. */
static bool kept_at(Oid oid, const std::string &key) {
  return kept_oid == oid && kept_key == key && kept_tuple != NULL;
}

/*
 * The columns of rel's row key, as RowKey holds them (access/rowkey.h): its
 * primary key's, attribute numbers from 1, or its rowno alone; their count.
 * A relation that is no table in sm, a catalog relation, has none of its
 * own: its rowno, which a plan that writes it, never run, names.
 */
int RowKeyAttnums(Relation rel, AttrNumber *attnums) {
  if (!RelationIsSm(rel)) {
    attnums[0] = RownoAttributeNumber;
    return 1;
  }
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  for (const sm_index_def_t &x : sm_tbl_def(tbl)->indexes) {
    if (!x.primary)
      continue;
    for (size_t i = 0; i < x.key_attnums.size(); i++)
      attnums[i] = (AttrNumber)(x.key_attnums[i] + 1);
    return (int)x.key_attnums.size();
  }
  attnums[0] = RownoAttributeNumber;
  return 1;
}

/*
 * The name of the junk column that carries rel's row key's column i, of
 * attribute attnum, in a plan that updates or deletes its rows: "_id"
 * in a keyless table, else one named for rel and the column.
 */
const char *RowKeyColumnName(Relation rel, int i, AttrNumber attnum) {
  if (RownoAttributeNumber == attnum)
    return "_id";
  return psprintf("rowkey%u_%d", RelationGetRelid(rel), i);
}

/* The key of the row of rel in slot into *key, its values the slot's. */
void RowKeyFromSlot(Relation rel, TupleTableSlot *slot, RowKey *key) {
  AttrNumber atts[INDEX_MAX_KEYS];

  key->nkeys = RowKeyAttnums(rel, atts);
  for (int i = 0; i < key->nkeys; i++)
    key->values[i] =
        RownoAttributeNumber == atts[i]
            ? slot_getsysattr(slot, RownoAttributeNumber, &key->isnull[i])
            : slot_getattr(slot, atts[i], &key->isnull[i]);
}

/* *src of rel into *dst, its values by reference copied, palloc'd. */
void RowKeyCopy(Relation rel, const RowKey *src, RowKey *dst) {
  AttrNumber atts[INDEX_MAX_KEYS];
  TupleDesc desc = RelationGetDescr(rel);

  RowKeyAttnums(rel, atts);
  *dst = *src;
  for (int i = 0; i < src->nkeys; i++) {
    if (src->isnull[i] || RownoAttributeNumber == atts[i])
      continue;
    Form_pg_attribute att = TupleDescAttr(desc, atts[i] - 1);

    dst->values[i] = datumCopy(src->values[i], att->attbyval, att->attlen);
  }
}

/*
 * key, a row's of rel, as sm takes it, its values in vals: each decimal its
 * integer at its column's scale, as row_to_sm() makes a row's.
 */
static sm_row_key_t key_to_sm(Relation rel, const RowKey *key,
                              sm_datum_t *vals) {
  AttrNumber atts[INDEX_MAX_KEYS];
  TupleDesc desc = RelationGetDescr(rel);

  RowKeyAttnums(rel, atts);
  for (int i = 0; i < key->nkeys; i++) {
    vals[i] = (sm_datum_t)key->values[i];
    if (key->isnull[i] || RownoAttributeNumber == atts[i])
      continue;
    Form_pg_attribute att = TupleDescAttr(desc, atts[i] - 1);

    if (is_decimal(att->atttypid))
      vals[i] = numeric_to_sm(key->values[i], att->atttypid, att->atttypmod);
  }
  return sm_row_key_t{vals, key->isnull, (size_t)key->nkeys};
}

/* key, a row's of tbl, encoded, as a cursor on the row gives it. */
static std::string key_bytes(sm_tbl_t *tbl, const sm_row_key_t &key) {
  std::string out;

  if (0 != sm_tbl_row_key(tbl, key, out))
    sm_error("name a row by its key");
  return out;
}

/*
 * The rowno of the row key names in a table without a primary key, its
 * tid's; 0, no tid, in one with.
 */
static uint64 key_rowno(Relation rel, const RowKey *key) {
  AttrNumber atts[INDEX_MAX_KEYS];

  RowKeyAttnums(rel, atts);
  return RownoAttributeNumber == atts[0] ? (uint64)DatumGetInt64(key->values[0])
                                         : 0;
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
 * The open scan of the table oid whose cursor is on the row at key,
 * encoded, the row it handed out last; NULL if none is.
 */
static smstore_scan_t *scan_at(Oid oid, const std::string &key);

/*
 * The key of the row the scan's cursor is on, encoded, into *key and *len:
 * NULL with no scan.  Valid until the cursor moves.
 */
static void scan_key(smstore_scan_t *scan, const void **key, size_t *len) {
  *key = NULL;
  *len = 0;
  /* A row read ahead: its own key, the cursor being on past it. */
  if (scan != NULL && (scan->ra_rows || scan->ra_point)) {
    *key = scan->cur_key;
    *len = scan->cur_klen;
  } else if (scan != NULL && scan->point) {
    *key = scan->point_key;
    *len = scan->point_len;
  } else if (scan != NULL &&
             (scan->cursor != NULL
                  ? sm_rel_scan_key(scan->cursor, key, len)
                  : sm_idx_scan_row(scan->icursor, key, len)) != 0)
    sm_error("read a row's key");
}

/* The open scan of the table oid on the row at key; see above. */
static smstore_scan_t *scan_at(Oid oid, const std::string &key) {
  for (smstore_scan_t *scan : open_scans) {
    const void *k;
    size_t len;

    if (!scan->cur || scan->relid != oid)
      continue;
    scan_key(scan, &k, &len);
    if (len == key.size() && 0 == memcmp(k, key.data(), len))
      return scan;
  }
  return NULL;
}

/*
 * The row the scan's cursor is on, as its pin holds it, into *tuple and
 * *len; false with no scan, if the scan has not read the row, or if the
 * pin has let it go.
 */
static bool scan_tuple(smstore_scan_t *scan, const void **tuple, size_t *len) {
  return scan != NULL && scan->cur_pinned &&
         sm_pin_tuple(scan->cursor != NULL ? scan->row_ref : scan->ref, tuple,
                      len) == 0;
}

/*
 * The row of rel key names into slot, as a virtual tuple of its first natts
 * columns; false if the statement sees none.  The executor fetches the row
 * it is to update, the newest version of one it locked, the row it deleted,
 * for RETURNING, and a TID scan's rows.
 * 1. Take the kept row, if it is that row.
 * 2. Else take the row as the statement sees it, from the scan that read
 *    it, if one is on it (scan_at()).
 * 3. Else read the row as the statement sees it.
 */
bool smstore_fetch_row(Relation rel, const RowKey *key, int natts,
                       TupleTableSlot *slot) {
  sm_tbl_t *tbl = open_table(RelationGetRelid(rel), SM_LOCKMODE_IS);
  sm_datum_t vals[INDEX_MAX_KEYS];

  if (tbl == NULL)
    return false;
  sm_row_key_t sk = key_to_sm(rel, key, vals);
  std::string bytes = key_bytes(tbl, sk);
  uint64 rowno = key_rowno(rel, key);

  slot->tts_tableOid = RelationGetRelid(rel);
  if (kept_at(RelationGetRelid(rel), bytes)) {
    ExecClearTuple(slot);
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len, natts, slot);
    return true;
  }
  const void *tuple;
  size_t len;
  smstore_scan_t *scan = scan_at(RelationGetRelid(rel), bytes);

  if (scan_tuple(scan, &tuple, &len)) {
    ExecClearTuple(slot);
    tuple_into_slot(tbl, scan->cur_rowno, tuple, len, natts, slot);
    /* The scan's pin lasts until its cursor moves. */
    ExecMaterializeSlot(slot);
    return true;
  }
  ExecClearTuple(slot);
  switch (sm_tbl_read(tbl, sk, &tuple, &len)) {
  case 0:
    break;
  case 1:
    return false;
  default:
    sm_error("read a row");
  }
  tuple_into_slot(tbl, rowno, tuple, len, natts, slot);
  /* sm lends the row only until its next call. */
  ExecMaterializeSlot(slot);
  return true;
}

/*
 * What an update or a delete of the row at key, encoded, of rel came to,
 * rc from sm, as the executor is told it.  A commit since the statement's
 * snapshot changed the row, under READ COMMITTED: TM_Updated, and the row
 * is kept for smstore_lock_row() to read; sm holds its lock.  The statement
 * has already changed the row: TM_SelfModified, by this command, so the
 * executor passes over it.  At the other levels sm has aborted the
 * transaction instead, which is an error here, as any other failure is; a
 * duplicate key is the new row's, in slot.
 */
static TM_Result write_result(Relation rel, const std::string &key, int rc,
                              TupleTableSlot *slot, TM_FailureData *tmfd,
                              const char *what) {
  if (rc == 0)
    return TM_Ok;
  tmfd->xmax = InvalidTransactionId;
  switch (sm_errnum()) {
  case SM_ERR_UPDATED:
    keep_row(RelationGetRelid(rel), key, NULL, 0);
    return TM_Updated;
  case SM_ERR_SELFMODIFIED:
    return TM_SelfModified;
  case SM_ERR_NOTFOUND:
    return TM_Deleted;
  case SM_ERR_DUPKEY:
    duplicate_write(rel, slot);
  default:
    sm_error(what);
  }
  return TM_Ok; /* not reached */
}

/*
 * Replace the row of rel key names with the slot's: sm writes every index
 * of the table with it.
 */
TM_Result smstore_update(Relation rel, const RowKey *key, TupleTableSlot *slot,
                         TM_FailureData *tmfd) {
  sm_tbl_t *tbl = table_to_write(rel);
  sm_datum_t vals[INDEX_MAX_KEYS];
  sm_row_key_t sk = key_to_sm(rel, key, vals);
  std::string bytes = key_bytes(tbl, sk);
  std::string_view old;
  sm_pin_ref_t ref = {};
  bool have_old = false;

  slot_getallattrs(slot);
  /*
   * The row replaced, to tell its own pointers from others: the kept one if
   * it is that row, the newest version after a write found it changed, the
   * one the scan that read it is on, else as the statement sees it.
   */
  if (has_toast(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull,
                slot->tts_nvalid)) {
    const void *tuple;
    size_t len;

    if (kept_at(RelationGetRelid(rel), bytes)) {
      old = std::string_view(kept_tuple, kept_len);
      have_old = true;
    } else if (scan_tuple(scan_at(RelationGetRelid(rel), bytes), &tuple,
                          &len)) {
      old = std::string_view((const char *)tuple, len);
      have_old = true;
    } else {
      switch (sm_tbl_read(tbl, sk, &ref, &tuple, &len)) {
      case 0:
        /* Pinned, as the fetches of the values out of line are calls. */
        old = std::string_view((const char *)tuple, len);
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
    return sm_tbl_update(tbl, sk, row, slot->tts_isnull, slot->tts_nvalid);
  });

  sm_unpin(&ref);
  return write_result(rel, bytes, rc, slot, tmfd, "update a row");
}

/*
 * Delete the row of rel key names, with its entry in every index.  The row
 * is kept as it was, for RETURNING to fetch.
 */
TM_Result smstore_delete(Relation rel, const RowKey *key,
                         TM_FailureData *tmfd) {
  sm_tbl_t *tbl = table_to_write(rel);
  sm_datum_t vals[INDEX_MAX_KEYS];
  sm_row_key_t sk = key_to_sm(rel, key, vals);
  std::string bytes = key_bytes(tbl, sk);
  const void *old;
  size_t len;

  /* The row deleted, pinned in kept_ref, which lets go of the last. */
  int rc =
      waiting([&] { return sm_tbl_delete(tbl, sk, &kept_ref, &old, &len); });

  if (rc == 0)
    keep_row(RelationGetRelid(rel), bytes, old, len);
  return write_result(rel, bytes, rc, NULL, tmfd, "delete a row");
}

/*
 * Lock the row of rel key names, and put its newest version into slot.
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
 * executor keeps it past sm's next call.  TM_Deleted if the row is gone;
 * TM_SelfModified, with nothing locked, if this transaction changed it
 * after the running statement began, as after a FOR UPDATE cursor's
 * DECLARE: the executor passes over it, as PostgreSQL's.
 */
TM_Result smstore_lock_row(Relation rel, const RowKey *key,
                           TupleTableSlot *slot, LockTupleMode mode,
                           LockWaitPolicy wait_policy, TM_FailureData *tmfd) {
  Oid oid = RelationGetRelid(rel);
  bool share = mode == LockTupleShare || mode == LockTupleKeyShare;
  sm_tbl_t *tbl =
      share ? open_table(oid, SM_LOCKMODE_IS) : table_to_write(rel);
  const void *tuple;
  size_t len;

  if (tbl == NULL)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("relation \"%s\" does not exist",
                           RelationGetRelationName(rel))));
  sm_datum_t vals[INDEX_MAX_KEYS];
  sm_row_key_t sk = key_to_sm(rel, key, vals);
  std::string bytes = key_bytes(tbl, sk);
  uint64 rowno = key_rowno(rel, key);
  bool after_write = kept_oid == oid && kept_key == bytes && kept_tuple == NULL;

  slot->tts_tableOid = oid;
  if (!after_write) {
    sm_lockmode_t smmode = share ? SM_LOCKMODE_SH : SM_LOCKMODE_EX;
    int rc;

    /*
     * Held to the end of the transaction, as a write's is.  NOWAIT and SKIP
     * LOCKED ask for it without waiting, and are told when it is another's.
     */
    if (wait_policy == LockWaitBlock)
      rc = waiting([&] { return sm_tbl_lock(tbl, sk, smmode); });
    else
      rc = sm_tbl_trylock(tbl, sk, smmode);
    /* Changed by a later command of this transaction: passed over. */
    if (rc < 0 && sm_errnum() == SM_ERR_SELFMODIFIED) {
      tmfd->xmax = InvalidTransactionId;
      return TM_SelfModified;
    }
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
    switch (sm_tbl_read(tbl, sk, &tuple, &len)) {
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
  /* After a write, the version is kept, pinned in kept_ref. */
  switch (after_write ? sm_tbl_read_latest(tbl, sk, &kept_ref, &tuple, &len)
                      : sm_tbl_read_latest(tbl, sk, &tuple, &len)) {
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
    keep_row(oid, bytes, tuple, len);
    tuple_into_slot(tbl, rowno, kept_tuple, kept_len,
                    slot->tts_tupleDescriptor->natts, slot);
    tmfd->traversed = true;
    return TM_Ok;
  }
  /* sm's bytes last until its next call: the read below is one. */
  std::string latest((const char *)tuple, len);

  switch (sm_tbl_read(tbl, sk, &tuple, &len)) {
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
