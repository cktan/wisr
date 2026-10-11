/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  top level transaction system support routines
 *
 * See src/backend/access/transam/README for more information.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/xact.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "access/multixact.h"
#include "access/smstore.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_enum.h"
#include "catalog/storage.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

/*
 *	User-tweakable parameters
 */
thread_local /*LOCAL*/ int DefaultXactIsoLevel = XACT_READ_COMMITTED;
thread_local /*LOCAL*/ int XactIsoLevel = XACT_READ_COMMITTED;

thread_local /*LOCAL*/ bool DefaultXactReadOnly = false;
thread_local /*LOCAL*/ bool XactReadOnly;

thread_local /*LOCAL*/ bool DefaultXactDeferrable = false;
thread_local /*LOCAL*/ bool XactDeferrable;

/*
 * CheckXidAlive is a xid value pointing to a possibly ongoing (sub)
 * transaction.  Currently, it is used in logical decoding.  It's possible
 * that such transactions can get aborted while the decoding is ongoing in
 * which case we skip decoding that particular transaction.  To ensure that we
 * check whether the CheckXidAlive is aborted after fetching the tuple from
 * system tables.  We also ensure that during logical decoding we never
 * directly access the tableam or heap APIs because we are checking for the
 * concurrent aborts only in systable_* APIs.
 */
thread_local /*LOCAL*/ TransactionId CheckXidAlive = InvalidTransactionId;
thread_local /*LOCAL*/ bool bsysscan = false;

/*
 * Miscellaneous flag bits to record events which occur on the top level
 * transaction. These flags are only persisted in MyXactFlags and are intended
 * so we remember to do certain things later on in the transaction. This is
 * globally accessible, so can be set from anywhere in the code that requires
 * recording flags.
 */
thread_local /*LOCAL*/ int MyXactFlags;

/*
 *	transaction states - transaction state from server perspective
 */
typedef enum TransState {
  TRANS_DEFAULT,    /* idle */
  TRANS_START,      /* transaction starting */
  TRANS_INPROGRESS, /* inside a valid transaction */
  TRANS_COMMIT,     /* commit in progress */
  TRANS_ABORT,      /* abort in progress */
  TRANS_PREPARE,    /* prepare in progress */
} TransState;

/*
 *	transaction block states - transaction state of client queries
 *
 * Note: the subtransaction states are used only for non-topmost
 * transactions; the others appear only in the topmost transaction.
 */
typedef enum TBlockState {
  /* not-in-transaction-block states */
  TBLOCK_DEFAULT, /* idle */
  TBLOCK_STARTED, /* running single-query transaction */

  /* transaction block states */
  TBLOCK_BEGIN,               /* starting transaction block */
  TBLOCK_INPROGRESS,          /* live transaction */
  TBLOCK_IMPLICIT_INPROGRESS, /* live transaction after implicit BEGIN */
  TBLOCK_END,                 /* COMMIT received */
  TBLOCK_ABORT,               /* failed xact, awaiting ROLLBACK */
  TBLOCK_ABORT_END,           /* failed xact, ROLLBACK received */
  TBLOCK_ABORT_PENDING,       /* live xact, ROLLBACK received */

  /* subtransaction states */
  TBLOCK_SUBBEGIN,         /* starting a subtransaction */
  TBLOCK_SUBINPROGRESS,    /* live subtransaction */
  TBLOCK_SUBRELEASE,       /* RELEASE received */
  TBLOCK_SUBCOMMIT,        /* COMMIT received while TBLOCK_SUBINPROGRESS */
  TBLOCK_SUBABORT,         /* failed subxact, awaiting ROLLBACK */
  TBLOCK_SUBABORT_END,     /* failed subxact, ROLLBACK received */
  TBLOCK_SUBABORT_PENDING, /* live subxact, ROLLBACK received */
  TBLOCK_SUBRESTART,       /* live subxact, ROLLBACK TO received */
  TBLOCK_SUBABORT_RESTART, /* failed subxact, ROLLBACK TO received */
} TBlockState;

/*
 *	transaction state structure
 */
typedef struct TransactionStateData {
  SubTransactionId subTransactionId;   /* my subxact ID */
  char *name;                          /* savepoint name, if any */
  int savepointLevel;                  /* savepoint level */
  TransState state;                    /* low-level state */
  TBlockState blockState;              /* high-level state */
  int nestingLevel;                    /* transaction nesting depth */
  int gucNestLevel;                    /* GUC context nesting depth */
  MemoryContext curTransactionContext; /* my xact-lifetime context */
  ResourceOwner curTransactionOwner;   /* my query resources */
  MemoryContext priorContext; /* CurrentMemoryContext before xact started */
  Oid prevUser;               /* previous CurrentUserId setting */
  int prevSecContext;         /* previous SecurityRestrictionContext */
  bool prevXactReadOnly;      /* entry-time xact r/o state */
  bool chain;                 /* start a new block after this one */
  struct TransactionStateData *parent; /* back link to parent */
} TransactionStateData;

typedef TransactionStateData *TransactionState;

/*
 * CurrentTransactionState always points to the current transaction state
 * block.  It will point to TopTransactionStateData when not in a
 * transaction at all, or when in a top-level transaction.
 */
static thread_local /*LOCAL*/ TransactionStateData TopTransactionStateData = {
    .state = TRANS_DEFAULT,
    .blockState = TBLOCK_DEFAULT,
};

static thread_local /*LOCAL*/ TransactionState CurrentTransactionState =
    &TopTransactionStateData;

/*
 * The subtransaction ID and command ID assignment counters are global
 * to a whole transaction, so we do not keep them in the state stack.
 */
static thread_local /*LOCAL*/ SubTransactionId currentSubTransactionId;
static thread_local /*LOCAL*/ CommandId currentCommandId;
static thread_local /*LOCAL*/ bool currentCommandIdUsed;

/*
 * xactStartTimestamp is the value of transaction_timestamp().
 * stmtStartTimestamp is the value of statement_timestamp().
 * xactStopTimestamp is the time at which we log a commit / abort WAL record,
 * or if that was skipped, the time of the first subsequent
 * GetCurrentTransactionStopTimestamp() call.
 *
 * These do not change as we enter and exit subtransactions, so we don't
 * keep them inside the TransactionState stack.
 */
static thread_local /*LOCAL*/ TimestampTz xactStartTimestamp;
static thread_local /*LOCAL*/ TimestampTz stmtStartTimestamp;
static thread_local /*LOCAL*/ TimestampTz xactStopTimestamp;

/* Flag for logging statements in a transaction. */
thread_local /*LOCAL*/ bool xact_is_sampled = false;

/*
 * Private context for transaction-abort work --- we reserve space for this
 * at startup to ensure that AbortTransaction and AbortSubTransaction can work
 * when we've run out of memory.
 */
static thread_local /*LOCAL*/ MemoryContext TransactionAbortContext = NULL;

/* local function prototypes */
static void AbortTransaction(void);
static void AtAbort_Memory(void);
static void AtCleanup_Memory(void);
static void AtAbort_ResourceOwner(void);
static void AtCCI_LocalCache(void);
static void AtCommit_Memory(void);
static void AtStart_Cache(void);
static void AtStart_Memory(void);
static void AtStart_ResourceOwner(void);
static void CleanupTransaction(void);
static void CheckTransactionBlock(bool isTopLevel, bool throwError,
                                  const char *stmtType);
static void CommitTransaction(void);
static void StartTransaction(void);

static bool CommitTransactionCommandInternal(void);
static bool AbortCurrentTransactionInternal(void);

static void StartSubTransaction(void);
static void CommitSubTransaction(void);
static void AbortSubTransaction(void);
static void CleanupSubTransaction(void);
static void PushTransaction(void);
static void PopTransaction(void);

static void AtSubAbort_Memory(void);
static void AtSubCleanup_Memory(void);
static void AtSubAbort_ResourceOwner(void);
static void AtSubCommit_Memory(void);
static void AtSubStart_Memory(void);
static void AtSubStart_ResourceOwner(void);

static void ShowTransactionState(const char *str);
static void ShowTransactionStateRec(const char *str, TransactionState s);
static const char *BlockStateAsString(TBlockState blockState);
static const char *TransStateAsString(TransState state);

/* ----------------------------------------------------------------
 *	transaction state accessors
 * ----------------------------------------------------------------
 */

/*
 *	IsTransactionState
 *
 *	This returns true if we are inside a valid transaction; that is,
 *	it is safe to initiate database access, take heavyweight locks, etc.
 */
bool IsTransactionState(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * TRANS_DEFAULT and TRANS_ABORT are obviously unsafe states.  However, we
   * also reject the startup/shutdown states TRANS_START, TRANS_COMMIT,
   * TRANS_PREPARE since it might be too soon or too late within those
   * transition states to do anything interesting.  Hence, the only "valid"
   * state is TRANS_INPROGRESS.
   */
  return (s->state == TRANS_INPROGRESS);
}

/*
 *	IsAbortedTransactionBlockState
 *
 *	This returns true if we are within an aborted transaction block.
 */
bool IsAbortedTransactionBlockState(void) {
  TransactionState s = CurrentTransactionState;

  if (s->blockState == TBLOCK_ABORT || s->blockState == TBLOCK_SUBABORT)
    return true;

  return false;
}

/*
 * Refuse a transaction id: digres assigns none.  A transaction is sm's
 * (PLAN.md section 7); only what writes a heap asks for a PostgreSQL xid,
 * and a table in sm is no heap.
 */
static void NoTransactionId(void) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("digres assigns no transaction ids")));
}

/* The main transaction's xid: refused (NoTransactionId()). */
TransactionId GetTopTransactionId(void) {
  NoTransactionId();
  return InvalidTransactionId; /* not reached */
}

/* The main transaction's xid if it has one: never. */
TransactionId GetTopTransactionIdIfAny(void) { return InvalidTransactionId; }

/* The current (sub)transaction's xid: refused (NoTransactionId()). */
TransactionId GetCurrentTransactionId(void) {
  NoTransactionId();
  return InvalidTransactionId; /* not reached */
}

/* The current (sub)transaction's xid if it has one: never. */
TransactionId GetCurrentTransactionIdIfAny(void) {
  return InvalidTransactionId;
}

/* The main transaction's FullTransactionId: refused. */
FullTransactionId GetTopFullTransactionId(void) {
  NoTransactionId();
  return InvalidFullTransactionId; /* not reached */
}

/* The main transaction's FullTransactionId if it has one: never. */
FullTransactionId GetTopFullTransactionIdIfAny(void) {
  return InvalidFullTransactionId;
}

/* The current (sub)transaction's FullTransactionId: refused. */
FullTransactionId GetCurrentFullTransactionId(void) {
  NoTransactionId();
  return InvalidFullTransactionId; /* not reached */
}

/* The current (sub)transaction's FullTransactionId if it has one: never. */
FullTransactionId GetCurrentFullTransactionIdIfAny(void) {
  return InvalidFullTransactionId;
}

/*
 *	GetStableLatestTransactionId
 *
 * The next-to-be-assigned XID, read once and returned for the rest of the
 * transaction: the reference point for the age(xid) function.
 */
TransactionId GetStableLatestTransactionId(void) {
  static thread_local /*LOCAL*/ LocalTransactionId lxid =
      InvalidLocalTransactionId;
  static thread_local /*LOCAL*/ TransactionId stablexid = InvalidTransactionId;

  if (lxid != MyProc->vxid.lxid) {
    lxid = MyProc->vxid.lxid;
    stablexid = ReadNextTransactionId();
  }

  Assert(TransactionIdIsValid(stablexid));

  return stablexid;
}

/*
 *	GetCurrentSubTransactionId
 */
SubTransactionId GetCurrentSubTransactionId(void) {
  TransactionState s = CurrentTransactionState;

  return s->subTransactionId;
}

/*
 *	SubTransactionIsActive
 *
 * Test if the specified subxact ID is still active.  Note caller is
 * responsible for checking whether this ID is relevant to the current xact.
 */
bool SubTransactionIsActive(SubTransactionId subxid) {
  TransactionState s;

  for (s = CurrentTransactionState; s != NULL; s = s->parent) {
    if (s->state == TRANS_ABORT)
      continue;
    if (s->subTransactionId == subxid)
      return true;
  }
  return false;
}

/*
 *	GetCurrentCommandId
 *
 * "used" must be true if the caller intends to use the command ID to mark
 * inserted/updated/deleted tuples.  false means the ID is being fetched
 * for read-only purposes (ie, as a snapshot validity cutoff).  See
 * CommandCounterIncrement() for discussion.
 */
CommandId GetCurrentCommandId(bool used) {
  /* this is global to a transaction, not subtransaction-local */
  if (used)
    currentCommandIdUsed = true;
  return currentCommandId;
}

/*
 *	GetCurrentTransactionStartTimestamp
 */
TimestampTz GetCurrentTransactionStartTimestamp(void) {
  return xactStartTimestamp;
}

/*
 *	GetCurrentStatementStartTimestamp
 */
TimestampTz GetCurrentStatementStartTimestamp(void) {
  return stmtStartTimestamp;
}

/*
 *	GetCurrentTransactionStopTimestamp
 *
 * If the transaction stop time hasn't already been set, which can happen if
 * we decided we don't need to log an XLOG record, set xactStopTimestamp.
 */
TimestampTz GetCurrentTransactionStopTimestamp(void) {
  TransactionState s PG_USED_FOR_ASSERTS_ONLY = CurrentTransactionState;

  /* should only be called after commit / abort processing */
  Assert(s->state == TRANS_DEFAULT || s->state == TRANS_COMMIT ||
         s->state == TRANS_ABORT || s->state == TRANS_PREPARE);

  if (xactStopTimestamp == 0)
    xactStopTimestamp = GetCurrentTimestamp();

  return xactStopTimestamp;
}

/*
 *	SetCurrentStatementStartTimestamp
 */
void SetCurrentStatementStartTimestamp(void) {
  stmtStartTimestamp = GetCurrentTimestamp();
}

/*
 *	GetCurrentTransactionNestLevel
 *
 * Note: this will return zero when not inside any transaction, one when
 * inside a top-level transaction, etc.
 */
int GetCurrentTransactionNestLevel(void) {
  TransactionState s = CurrentTransactionState;

  return s->nestingLevel;
}

/*
 *	TransactionIdIsCurrentTransactionId
 */
bool TransactionIdIsCurrentTransactionId(TransactionId xid) {
  /* digres assigns no xid, so none is this transaction's. */
  return false;
}

/*
 *	CommandCounterIncrement
 */
void CommandCounterIncrement(void) {
  /*
   * If the current value of the command counter hasn't been "used" to mark
   * tuples, we need not increment it, since there's no need to distinguish
   * a read-only command from others.  This helps postpone command counter
   * overflow, and keeps no-op CommandCounterIncrement operations cheap.
   */
  if (currentCommandIdUsed) {
    currentCommandId += 1;
    if (currentCommandId == InvalidCommandId) {
      currentCommandId -= 1;
      ereport(
          ERROR,
          (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
           errmsg("cannot have more than 2^32-2 commands in a transaction")));
    }
    currentCommandIdUsed = false;

    /* Propagate new command ID into static snapshots */
    SnapshotSetCommandId(currentCommandId);

    /*
     * Make any catalog changes done by the just-completed command visible
     * in the local syscache.  We obviously don't need to do this after a
     * read-only command.  (But see hacks in inval.c to make real sure we
     * don't think a command that queued inval messages was read-only.)
     */
    AtCCI_LocalCache();
  }
}

/* ----------------------------------------------------------------
 *						StartTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtStart_Cache
 */
static void AtStart_Cache(void) { AcceptInvalidationMessages(); }

/*
 *	AtStart_Memory
 */
static void AtStart_Memory(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * Remember the memory context that was active prior to transaction start.
   */
  s->priorContext = CurrentMemoryContext;

  /*
   * If this is the first time through, create a private context for
   * AbortTransaction to work in.  By reserving some space now, we can
   * insulate AbortTransaction from out-of-memory scenarios.  Like
   * ErrorContext, we set it up with slow growth rate and a nonzero minimum
   * size, so that space will be reserved immediately.
   */
  if (TransactionAbortContext == NULL)
    TransactionAbortContext =
        AllocSetContextCreate(TopMemoryContext, "TransactionAbortContext",
                              32 * 1024, 32 * 1024, 32 * 1024);

  /*
   * Likewise, if this is the first time through, create a top-level context
   * for transaction-local data.  This context will be reset at transaction
   * end, and then re-used in later transactions.
   */
  if (TopTransactionContext == NULL)
    TopTransactionContext = AllocSetContextCreate(
        TopMemoryContext, "TopTransactionContext", ALLOCSET_DEFAULT_SIZES);

  /*
   * In a top-level transaction, CurTransactionContext is the same as
   * TopTransactionContext.
   */
  CurTransactionContext = TopTransactionContext;
  s->curTransactionContext = CurTransactionContext;

  /* Make the CurTransactionContext active. */
  MemoryContextSwitchTo(CurTransactionContext);
}

/*
 *	AtStart_ResourceOwner
 */
static void AtStart_ResourceOwner(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * We shouldn't have a transaction resource owner already.
   */
  Assert(TopTransactionResourceOwner == NULL);

  /*
   * Create a toplevel resource owner for the transaction.
   */
  s->curTransactionOwner = ResourceOwnerCreate(NULL, "TopTransaction");

  TopTransactionResourceOwner = s->curTransactionOwner;
  CurTransactionResourceOwner = s->curTransactionOwner;
  CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						StartSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubStart_Memory
 */
static void AtSubStart_Memory(void) {
  TransactionState s = CurrentTransactionState;

  Assert(CurTransactionContext != NULL);

  /*
   * Remember the context that was active prior to subtransaction start.
   */
  s->priorContext = CurrentMemoryContext;

  /*
   * Create a CurTransactionContext, which will be used to hold data that
   * survives subtransaction commit but disappears on subtransaction abort.
   * We make it a child of the immediate parent's CurTransactionContext.
   */
  CurTransactionContext = AllocSetContextCreate(
      CurTransactionContext, "CurTransactionContext", ALLOCSET_DEFAULT_SIZES);
  s->curTransactionContext = CurTransactionContext;

  /* Make the CurTransactionContext active. */
  MemoryContextSwitchTo(CurTransactionContext);
}

/*
 * AtSubStart_ResourceOwner
 */
static void AtSubStart_ResourceOwner(void) {
  TransactionState s = CurrentTransactionState;

  Assert(s->parent != NULL);

  /*
   * Create a resource owner for the subtransaction.  We make it a child of
   * the immediate parent's resource owner.
   */
  s->curTransactionOwner =
      ResourceOwnerCreate(s->parent->curTransactionOwner, "SubTransaction");

  CurTransactionResourceOwner = s->curTransactionOwner;
  CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						CommitTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtCCI_LocalCache
 */
static void AtCCI_LocalCache(void) {
  /*
   * Make any pending relation map changes visible.  We must do this before
   * processing local sinval messages, so that the map changes will get
   * reflected into the relcache when relcache invals are processed.
   */
  AtCCI_RelationMap();

  /*
   * Make catalog changes visible to me for the next command.
   */
  CommandEndInvalidationMessages();
}

/*
 *	AtCommit_Memory
 */
static void AtCommit_Memory(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * Return to the memory context that was current before we started the
   * transaction.  (In principle, this could not be any of the contexts we
   * are about to delete.  If it somehow is, assertions in mcxt.c will
   * complain.)
   */
  MemoryContextSwitchTo(s->priorContext);

  /*
   * Release all transaction-local memory.  TopTransactionContext survives
   * but becomes empty; any sub-contexts go away.
   */
  Assert(TopTransactionContext != NULL);
  MemoryContextReset(TopTransactionContext);

  /*
   * Clear these pointers as a pro-forma matter.  (Notionally, while
   * TopTransactionContext still exists, it's currently not associated with
   * this TransactionState struct.)
   */
  CurTransactionContext = NULL;
  s->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						CommitSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubCommit_Memory
 */
static void AtSubCommit_Memory(void) {
  TransactionState s = CurrentTransactionState;

  Assert(s->parent != NULL);

  /* Return to parent transaction level's memory context. */
  CurTransactionContext = s->parent->curTransactionContext;
  MemoryContextSwitchTo(CurTransactionContext);

  /*
   * Ordinarily we cannot throw away the child's CurTransactionContext,
   * since the data it contains will be needed at upper commit.  However, if
   * there isn't actually anything in it, we can throw it away.  This avoids
   * a small memory leak in the common case of "trivial" subxacts.
   */
  if (MemoryContextIsEmpty(s->curTransactionContext)) {
    MemoryContextDelete(s->curTransactionContext);
    s->curTransactionContext = NULL;
  }
}

/* ----------------------------------------------------------------
 *						AbortTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtAbort_Memory
 */
static void AtAbort_Memory(void) {
  /*
   * Switch into TransactionAbortContext, which should have some free space
   * even if nothing else does.  We'll work in this context until we've
   * finished cleaning up.
   *
   * It is barely possible to get here when we've not been able to create
   * TransactionAbortContext yet; if so use TopMemoryContext.
   */
  if (TransactionAbortContext != NULL)
    MemoryContextSwitchTo(TransactionAbortContext);
  else
    MemoryContextSwitchTo(TopMemoryContext);
}

/*
 * AtSubAbort_Memory
 */
static void AtSubAbort_Memory(void) {
  Assert(TransactionAbortContext != NULL);

  MemoryContextSwitchTo(TransactionAbortContext);
}

/*
 *	AtAbort_ResourceOwner
 */
static void AtAbort_ResourceOwner(void) {
  /*
   * Make sure we have a valid ResourceOwner, if possible (else it will be
   * NULL, which is OK)
   */
  CurrentResourceOwner = TopTransactionResourceOwner;
}

/*
 * AtSubAbort_ResourceOwner
 */
static void AtSubAbort_ResourceOwner(void) {
  TransactionState s = CurrentTransactionState;

  /* Make sure we have a valid ResourceOwner */
  CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						CleanupTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtCleanup_Memory
 */
static void AtCleanup_Memory(void) {
  TransactionState s = CurrentTransactionState;

  /* Should be at top level */
  Assert(s->parent == NULL);

  /*
   * Return to the memory context that was current before we started the
   * transaction.  (In principle, this could not be any of the contexts we
   * are about to delete.  If it somehow is, assertions in mcxt.c will
   * complain.)
   */
  MemoryContextSwitchTo(s->priorContext);

  /*
   * Clear the special abort context for next time.
   */
  if (TransactionAbortContext != NULL)
    MemoryContextReset(TransactionAbortContext);

  /*
   * Release all transaction-local memory, the same as in AtCommit_Memory,
   * except we must cope with the possibility that we didn't get as far as
   * creating TopTransactionContext.
   */
  if (TopTransactionContext != NULL)
    MemoryContextReset(TopTransactionContext);

  /*
   * Clear these pointers as a pro-forma matter.  (Notionally, while
   * TopTransactionContext still exists, it's currently not associated with
   * this TransactionState struct.)
   */
  CurTransactionContext = NULL;
  s->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						CleanupSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubCleanup_Memory
 */
static void AtSubCleanup_Memory(void) {
  TransactionState s = CurrentTransactionState;

  Assert(s->parent != NULL);

  /*
   * Return to the memory context that was current before we started the
   * subtransaction.  (In principle, this could not be any of the contexts
   * we are about to delete.  If it somehow is, assertions in mcxt.c will
   * complain.)
   */
  MemoryContextSwitchTo(s->priorContext);

  /* Update CurTransactionContext (might not be same as priorContext) */
  CurTransactionContext = s->parent->curTransactionContext;

  /*
   * Clear the special abort context for next time.
   */
  if (TransactionAbortContext != NULL)
    MemoryContextReset(TransactionAbortContext);

  /*
   * Delete the subxact local memory contexts. Its CurTransactionContext can
   * go too (note this also kills CurTransactionContexts from any children
   * of the subxact).
   */
  if (s->curTransactionContext)
    MemoryContextDelete(s->curTransactionContext);
  s->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						interface routines
 * ----------------------------------------------------------------
 */

/*
 *	StartTransaction
 */
static void StartTransaction(void) {
  TransactionState s;
  VirtualTransactionId vxid;

  /*
   * Let's just make sure the state stack is empty
   */
  s = &TopTransactionStateData;
  CurrentTransactionState = s;

  /* check the current transaction state */
  Assert(s->state == TRANS_DEFAULT);

  /*
   * Set the current transaction state information appropriately during
   * start processing.  Note that once the transaction status is switched
   * this process cannot fail until the user ID and the security context
   * flags are fetched below.
   */
  s->state = TRANS_START;

  /* Determine if statements are logged in this transaction */
  xact_is_sampled =
      log_xact_sample_rate != 0 &&
      (log_xact_sample_rate == 1 ||
       pg_prng_double(&pg_global_prng_state) <= log_xact_sample_rate);

  /*
   * initialize current transaction state fields
   *
   * note: prevXactReadOnly is not used at the outermost level
   */
  s->nestingLevel = 1;
  s->gucNestLevel = 1;

  /*
   * Once the current user ID and the security context flags are fetched,
   * both will be properly reset even if transaction startup fails.
   */
  GetUserIdAndSecContext(&s->prevUser, &s->prevSecContext);

  /* SecurityRestrictionContext should never be set outside a transaction */
  Assert(s->prevSecContext == 0);

  /*
   * Make sure we've reset xact state variables
   */
  XactReadOnly = DefaultXactReadOnly;
  XactDeferrable = DefaultXactDeferrable;
  XactIsoLevel = DefaultXactIsoLevel;
  MyXactFlags = 0;

  /*
   * reinitialize within-transaction counters
   */
  s->subTransactionId = TopSubTransactionId;
  currentSubTransactionId = TopSubTransactionId;
  currentCommandId = FirstCommandId;
  currentCommandIdUsed = false;

  /*
   * must initialize resource-management stuff first
   */
  AtStart_Memory();
  AtStart_ResourceOwner();

  /*
   * Assign a new LocalTransactionId, and combine it with the proc number to
   * form a virtual transaction id.
   */
  vxid.procNumber = MyProcNumber;
  vxid.localTransactionId = GetNextLocalTransactionId();

  /*
   * Lock the virtual transaction id before we announce it in the proc array
   */
  VirtualXactLockTableInsert(vxid);

  /*
   * Advertise it in the proc array.  We assume assignment of
   * localTransactionId is atomic, and the proc number should be set
   * already.
   */
  Assert(MyProc->vxid.procNumber == vxid.procNumber);
  MyProc->vxid.lxid = vxid.localTransactionId;

  TRACE_POSTGRESQL_TRANSACTION_START(vxid.localTransactionId);

  /*
   * set transaction_timestamp() (a/k/a now()).  Normally, we want this to
   * be the same as the first command's statement_timestamp(), so don't do a
   * fresh GetCurrentTimestamp() call (which'd be expensive anyway).  But
   * for transactions started inside procedures (i.e., nonatomic SPI
   * contexts), we do need to advance the timestamp.
   */
  if (!SPI_inside_nonatomic_context())
    xactStartTimestamp = stmtStartTimestamp;
  else
    xactStartTimestamp = GetCurrentTimestamp();
  /* Mark xactStopTimestamp as unset. */
  xactStopTimestamp = 0;

  /*
   * initialize other subsystems for new transaction
   */
  AtStart_GUC();
  AtStart_Cache();
  AfterTriggerBeginXact();

  /*
   * done with start processing, set current transaction state to "in
   * progress"
   */
  s->state = TRANS_INPROGRESS;

  /* Schedule transaction timeout */
  if (TransactionTimeout > 0)
    enable_timeout_after(TRANSACTION_TIMEOUT, TransactionTimeout);

  ShowTransactionState("StartTransaction");
}

/*
 *	CommitTransaction
 *
 * NB: if you change this routine, better look at PrepareTransaction too!
 */
static void CommitTransaction(void) {
  TransactionState s = CurrentTransactionState;

  ShowTransactionState("CommitTransaction");

  /*
   * check the current transaction state
   */
  if (s->state != TRANS_INPROGRESS)
    elog(WARNING, "CommitTransaction while in %s state",
         TransStateAsString(s->state));
  Assert(s->parent == NULL);

  /*
   * Do pre-commit processing that involves calling user-defined code, such
   * as triggers.  SECURITY_RESTRICTED_OPERATION contexts must not queue an
   * action that would run here, because that would bypass the sandbox.
   * Since closing cursors could queue trigger actions, triggers could open
   * cursors, etc, we have to keep looping until there's nothing left to do.
   */
  for (;;) {
    /*
     * Fire all currently pending deferred triggers.
     */
    AfterTriggerFireDeferred();

    /*
     * Close open portals (converting holdable ones into static portals).
     * If there weren't any, we are done ... otherwise loop back to check
     * if they queued deferred triggers.  Lather, rinse, repeat.
     */
    if (!PreCommit_Portals(false))
      break;
  }

  /*
   * The remaining actions cannot call any user-defined code, so it's safe
   * to start shutting down within-transaction services.  But note that most
   * of this stuff could still throw an error, which would switch us into
   * the transaction-abort path.
   */

  /* sm's transaction commits first: one sm refuses fails this one. */
  smstore_commit();

  /* Shut down the deferred-trigger manager */
  AfterTriggerEndXact(true);

  /*
   * Let ON COMMIT management do its thing (must happen after closing
   * cursors, to avoid dangling-reference problems)
   */
  PreCommit_on_commit_actions();

  /*
   * Synchronize files that are created and not WAL-logged during this
   * transaction. This must happen before AtEOXact_RelationMap(), so that we
   * don't see committed-but-broken files after a crash.
   */
  smgrDoPendingSyncs(true);

  /* close large objects before lower-level cleanup */
  AtEOXact_LargeObject(true);

  /*
   * Insert notifications sent by NOTIFY commands into the queue.  This
   * should be late in the pre-commit sequence to minimize time spent
   * holding the notify-insertion lock.  However, this could result in
   * creating a snapshot, so we must do it before serializable cleanup.
   */

  /*
   * Mark serializable transaction as complete for predicate locking
   * purposes.  This should be done as late as we can put it and still allow
   * errors to be raised for failure patterns found at commit.
   */
  PreCommit_CheckForSerializationFailure();

  /* Prevent cancel/die interrupt while cleaning up */
  HOLD_INTERRUPTS();

  /* Commit updates to the relation map --- do this as late as possible */
  AtEOXact_RelationMap(true);

  /*
   * set the current transaction state information appropriately during
   * commit processing
   */
  s->state = TRANS_COMMIT;

  /* Disable transaction timeout */
  if (TransactionTimeout > 0)
    disable_timeout(TRANSACTION_TIMEOUT, false);

  TRACE_POSTGRESQL_TRANSACTION_COMMIT(MyProc->vxid.lxid);

  /*
   * Let others know about no transaction in progress by me, before
   * releasing the locks we hold.  It has no xid to end: digres assigns none.
   */
  ProcArrayEndTransaction(MyProc, InvalidTransactionId);

  /*
   * This is all post-commit cleanup.  Note that if an error is raised here,
   * it's too late to abort the transaction.  This should be just
   * noncritical resource releasing.
   *
   * The ordering of operations is not entirely random.  The idea is:
   * release resources visible to other backends (eg, files, buffer pins);
   * then release locks; then release backend-local resources. We want to
   * release locks at the point where any backend waiting for us will see
   * our transaction as being fully cleaned up.
   *
   * Resources that can be associated with individual queries are handled by
   * the ResourceOwner mechanism.  The other calls here are for backend-wide
   * state.
   */

  CurrentResourceOwner = NULL;
  ResourceOwnerRelease(TopTransactionResourceOwner,
                       RESOURCE_RELEASE_BEFORE_LOCKS, true, true);

  /* Check we've released all buffer pins */
  AtEOXact_Buffers(true);

  /* Clean up the relation cache */
  AtEOXact_RelationCache(true);

  /* Clean up the type cache */
  AtEOXact_TypeCache();

  /*
   * Make catalog changes visible to all backends.  This has to happen after
   * relcache references are dropped (see comments for
   * AtEOXact_RelationCache), but before locks are released (if anyone is
   * waiting for lock on a relation we've modified, we want them to know
   * about the catalog change before they start using the relation).
   */
  AtEOXact_Inval(true);

  AtEOXact_MultiXact();

  ResourceOwnerRelease(TopTransactionResourceOwner, RESOURCE_RELEASE_LOCKS,
                       true, true);
  ResourceOwnerRelease(TopTransactionResourceOwner,
                       RESOURCE_RELEASE_AFTER_LOCKS, true, true);

  /*
   * Likewise, dropping of files deleted during the transaction is best done
   * after releasing relcache and buffer pins.  (This is not strictly
   * necessary during commit, since such pins should have been released
   * already, but this ordering is definitely critical during abort.)  Since
   * this may take many seconds, also delay until after releasing locks.
   * Other backends will observe the attendant catalog changes and not
   * attempt to access affected files.
   */
  smgrDoPendingDeletes(true);

  /*
   * Send out notification signals to other backends (and do other
   * post-commit NOTIFY cleanup).  This must not happen until after our
   * transaction is fully done from the viewpoint of other backends.
   */

  /*
   * Everything after this should be purely internal-to-this-backend
   * cleanup.
   */
  AtEOXact_GUC(true, 1);
  AtEOXact_SPI(true);
  AtEOXact_Enum();
  AtEOXact_on_commit_actions(true);
  AtEOXact_Namespace(true);
  AtEOXact_SMgr();
  AtEOXact_Files(true);
  AtEOXact_ComboCid();
  AtEOXact_HashTables(true);
  AtEOXact_Snapshot(true, false);

  ResourceOwnerDelete(TopTransactionResourceOwner);
  s->curTransactionOwner = NULL;
  CurTransactionResourceOwner = NULL;
  TopTransactionResourceOwner = NULL;

  AtCommit_Memory();

  s->subTransactionId = InvalidSubTransactionId;
  s->nestingLevel = 0;
  s->gucNestLevel = 0;

  /*
   * done with commit processing, set current transaction state back to
   * default
   */
  s->state = TRANS_DEFAULT;

  RESUME_INTERRUPTS();
}

/*
 *	AbortTransaction
 */
static void AbortTransaction(void) {
  TransactionState s = CurrentTransactionState;

  /* Prevent cancel/die interrupt while cleaning up */
  HOLD_INTERRUPTS();

  /* Disable transaction timeout */
  if (TransactionTimeout > 0)
    disable_timeout(TRANSACTION_TIMEOUT, false);

  /* Make sure we have a valid memory context and resource owner */
  AtAbort_Memory();
  AtAbort_ResourceOwner();

  /*
   * Release any LW locks we might be holding as quickly as possible.
   * (Regular locks, however, must be held till we finish aborting.)
   * Releasing LW locks is critical since we might try to grab them again
   * while cleaning up!
   */
  LWLockReleaseAll();

  /* Clean up buffer content locks, too */
  UnlockBuffers();

  /* Cancel condition variable sleep */
  ConditionVariableCancelSleep();

  /*
   * Also clean up any open wait for lock, since the lock manager will choke
   * if we try to wait for another lock before doing this.
   */
  LockErrorCleanup();

  /*
   * If any timeout events are still active, make sure the timeout interrupt
   * is scheduled.  This covers possible loss of a timeout interrupt due to
   * longjmp'ing out of the SIGINT handler (see notes in handle_sig_alarm).
   * We delay this till after LockErrorCleanup so that we don't uselessly
   * reschedule lock or deadlock check timeouts.
   */
  reschedule_timeouts();

  /*
   * Re-enable signals, in case we got here by longjmp'ing out of a signal
   * handler.  We do this fairly early in the sequence so that the timeout
   * infrastructure will be functional if needed while aborting.
   */
  sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

  /*
   * check the current transaction state
   */
  if (s->state != TRANS_INPROGRESS && s->state != TRANS_PREPARE)
    elog(WARNING, "AbortTransaction while in %s state",
         TransStateAsString(s->state));
  Assert(s->parent == NULL);

  /*
   * set the current transaction state information appropriately during the
   * abort processing
   */
  s->state = TRANS_ABORT;

  /*
   * Reset user ID which might have been changed transiently.  We need this
   * to clean up in case control escaped out of a SECURITY DEFINER function
   * or other local change of CurrentUserId; therefore, the prior value of
   * SecurityRestrictionContext also needs to be restored.
   *
   * (Note: it is not necessary to restore session authorization or role
   * settings here because those can only be changed via GUC, and GUC will
   * take care of rolling them back if need be.)
   */
  SetUserIdAndSecContext(s->prevUser, s->prevSecContext);

  /* Forget about any active REINDEX. */
  ResetReindexState(s->nestingLevel);

  /*
   * do abort processing
   */
  AfterTriggerEndXact(false); /* 'false' means it's abort */
  AtAbort_Portals();
  smgrDoPendingSyncs(false);
  AtEOXact_LargeObject(false);
  AtEOXact_RelationMap(false);

  TRACE_POSTGRESQL_TRANSACTION_ABORT(MyProc->vxid.lxid);

  /*
   * Let others know about no transaction in progress by me, before
   * releasing the locks we hold.
   */
  ProcArrayEndTransaction(MyProc, InvalidTransactionId);

  /*
   * Post-abort cleanup.  See notes in CommitTransaction() concerning
   * ordering.  We can skip all of it if the transaction failed before
   * creating a resource owner.
   */
  /* sm's transaction aborts with this one. */
  smstore_abort();
  if (TopTransactionResourceOwner != NULL) {
    ResourceOwnerRelease(TopTransactionResourceOwner,
                         RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
    AtEOXact_Buffers(false);
    AtEOXact_RelationCache(false);
    AtEOXact_TypeCache();
    AtEOXact_Inval(false);
    AtEOXact_MultiXact();
    ResourceOwnerRelease(TopTransactionResourceOwner, RESOURCE_RELEASE_LOCKS,
                         false, true);
    ResourceOwnerRelease(TopTransactionResourceOwner,
                         RESOURCE_RELEASE_AFTER_LOCKS, false, true);
    smgrDoPendingDeletes(false);

    AtEOXact_GUC(false, 1);
    AtEOXact_SPI(false);
    AtEOXact_Enum();
    AtEOXact_on_commit_actions(false);
    AtEOXact_Namespace(false);
    AtEOXact_SMgr();
    AtEOXact_Files(false);
    AtEOXact_ComboCid();
    AtEOXact_HashTables(false);
  }

  /*
   * State remains TRANS_ABORT until CleanupTransaction().
   */
  RESUME_INTERRUPTS();
}

/*
 *	CleanupTransaction
 */
static void CleanupTransaction(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * State should still be TRANS_ABORT from AbortTransaction().
   */
  if (s->state != TRANS_ABORT)
    elog(FATAL, "CleanupTransaction: unexpected state %s",
         TransStateAsString(s->state));

  /*
   * do abort cleanup processing
   */
  AtCleanup_Portals();            /* now safe to release portal memory */
  AtEOXact_Snapshot(false, true); /* and release the transaction's snapshots */

  CurrentResourceOwner = NULL; /* and resource owner */
  if (TopTransactionResourceOwner)
    ResourceOwnerDelete(TopTransactionResourceOwner);
  s->curTransactionOwner = NULL;
  CurTransactionResourceOwner = NULL;
  TopTransactionResourceOwner = NULL;

  AtCleanup_Memory(); /* and transaction memory */

  s->subTransactionId = InvalidSubTransactionId;
  s->nestingLevel = 0;
  s->gucNestLevel = 0;

  /*
   * done with abort processing, set current transaction state back to
   * default
   */
  s->state = TRANS_DEFAULT;
}

/*
 *	StartTransactionCommand
 */
void StartTransactionCommand(void) {
  TransactionState s = CurrentTransactionState;

  /* Each command is a statement of sm's too (access/smstore.h). */
  smstore_statement_start();

  switch (s->blockState) {
    /*
     * if we aren't in a transaction block, we just do our usual start
     * transaction.
     */
  case TBLOCK_DEFAULT:
    StartTransaction();
    s->blockState = TBLOCK_STARTED;
    break;

    /*
     * We are somewhere in a transaction block or subtransaction and
     * about to start a new command.  For now we do nothing, but
     * someday we may do command-local resource initialization. (Note
     * that any needed CommandCounterIncrement was done by the
     * previous CommitTransactionCommand.)
     */
  case TBLOCK_INPROGRESS:
  case TBLOCK_IMPLICIT_INPROGRESS:
  case TBLOCK_SUBINPROGRESS:
    break;

    /*
     * Here we are in a failed transaction block (one of the commands
     * caused an abort) so we do nothing but remain in the abort
     * state.  Eventually we will get a ROLLBACK command which will
     * get us out of this state.  (It is up to other code to ensure
     * that no commands other than ROLLBACK will be processed in these
     * states.)
     */
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
    break;

    /* These cases are invalid. */
  case TBLOCK_STARTED:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(ERROR, "StartTransactionCommand: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  /*
   * We must switch to CurTransactionContext before returning. This is
   * already done if we called StartTransaction, otherwise not.
   */
  Assert(CurTransactionContext != NULL);
  MemoryContextSwitchTo(CurTransactionContext);
}

/*
 * Simple system for saving and restoring transaction characteristics
 * (isolation level, read only, deferrable).  We need this for transaction
 * chaining, so that we can set the characteristics of the new transaction to
 * be the same as the previous one.  (We need something like this because the
 * GUC system resets the characteristics at transaction end, so for example
 * just skipping the reset in StartTransaction() won't work.)
 */
void SaveTransactionCharacteristics(SavedTransactionCharacteristics *s) {
  s->save_XactIsoLevel = XactIsoLevel;
  s->save_XactReadOnly = XactReadOnly;
  s->save_XactDeferrable = XactDeferrable;
}

void RestoreTransactionCharacteristics(
    const SavedTransactionCharacteristics *s) {
  XactIsoLevel = s->save_XactIsoLevel;
  XactReadOnly = s->save_XactReadOnly;
  XactDeferrable = s->save_XactDeferrable;
}

/*
 *	CommitTransactionCommand -- a wrapper function handling the
 *		loop over subtransactions to avoid a potentially dangerous
 *recursion in CommitTransactionCommandInternal().
 */
void CommitTransactionCommand(void) {
  /*
   * Repeatedly call CommitTransactionCommandInternal() until all the work
   * is done.
   */
  while (!CommitTransactionCommandInternal()) {
  }
}

/*
 *	CommitTransactionCommandInternal - a function doing an iteration of work
 *		regarding handling the commit transaction command.  In the case
 *of subtransactions more than one iterations could be required.  Returns true
 *when no more iterations required, false otherwise.
 */
static bool CommitTransactionCommandInternal(void) {
  TransactionState s = CurrentTransactionState;
  SavedTransactionCharacteristics savetc;

  /* Must save in case we need to restore below */
  SaveTransactionCharacteristics(&savetc);

  switch (s->blockState) {
    /*
     * This shouldn't happen.  TBLOCK_DEFAULT means the previous
     * StartTransactionCommand didn't set the STARTED state
     * appropriately.
     */
  case TBLOCK_DEFAULT:
    elog(FATAL, "CommitTransactionCommand: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;

    /*
     * If we aren't in a transaction block, just do our usual
     * transaction commit, and return to the idle state.
     */
  case TBLOCK_STARTED:
    CommitTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * We are completing a "BEGIN TRANSACTION" command, so we change
     * to the "transaction block in progress" state and return.  (We
     * assume the BEGIN did nothing to the database, so we need no
     * CommandCounterIncrement.)
     */
  case TBLOCK_BEGIN:
    s->blockState = TBLOCK_INPROGRESS;
    break;

    /*
     * This is the case when we have finished executing a command
     * someplace within a transaction block.  We increment the command
     * counter and return.
     */
  case TBLOCK_INPROGRESS:
  case TBLOCK_IMPLICIT_INPROGRESS:
  case TBLOCK_SUBINPROGRESS:
    CommandCounterIncrement();
    break;

    /*
     * We are completing a "COMMIT" command.  Do it and return to the
     * idle state.
     */
  case TBLOCK_END:
    CommitTransaction();
    s->blockState = TBLOCK_DEFAULT;
    if (s->chain) {
      StartTransaction();
      s->blockState = TBLOCK_INPROGRESS;
      s->chain = false;
      RestoreTransactionCharacteristics(&savetc);
    }
    break;

    /*
     * Here we are in the middle of a transaction block but one of the
     * commands caused an abort so we do nothing but remain in the
     * abort state.  Eventually we will get a ROLLBACK command.
     */
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
    break;

    /*
     * Here we were in an aborted transaction block and we just got
     * the ROLLBACK command from the user, so clean up the
     * already-aborted transaction and return to the idle state.
     */
  case TBLOCK_ABORT_END:
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    if (s->chain) {
      StartTransaction();
      s->blockState = TBLOCK_INPROGRESS;
      s->chain = false;
      RestoreTransactionCharacteristics(&savetc);
    }
    break;

    /*
     * Here we were in a perfectly good transaction block but the user
     * told us to ROLLBACK anyway.  We have to abort the transaction
     * and then clean up.
     */
  case TBLOCK_ABORT_PENDING:
    AbortTransaction();
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    if (s->chain) {
      StartTransaction();
      s->blockState = TBLOCK_INPROGRESS;
      s->chain = false;
      RestoreTransactionCharacteristics(&savetc);
    }
    break;

    /*
     * The user issued a SAVEPOINT inside a transaction block. Start a
     * subtransaction.  (DefineSavepoint already did PushTransaction,
     * so as to have someplace to put the SUBBEGIN state.)
     */
  case TBLOCK_SUBBEGIN:
    StartSubTransaction();
    s->blockState = TBLOCK_SUBINPROGRESS;
    break;

    /*
     * The user issued a RELEASE command, so we end the current
     * subtransaction and return to the parent transaction. The parent
     * might be ended too, so repeat till we find an INPROGRESS
     * transaction or subtransaction.
     */
  case TBLOCK_SUBRELEASE:
    do {
      CommitSubTransaction();
      s = CurrentTransactionState; /* changed by pop */
    } while (s->blockState == TBLOCK_SUBRELEASE);

    Assert(s->blockState == TBLOCK_INPROGRESS ||
           s->blockState == TBLOCK_SUBINPROGRESS);
    break;

    /*
     * The user issued a COMMIT, so we end the current subtransaction
     * hierarchy and perform final commit. We do this by rolling up
     * any subtransactions into their parent, which leads to O(N^2)
     * operations with respect to resource owners - this isn't that
     * bad until we approach a thousands of savepoints but is
     * necessary for correctness should after triggers create new
     * resource owners.
     */
  case TBLOCK_SUBCOMMIT:
    do {
      CommitSubTransaction();
      s = CurrentTransactionState; /* changed by pop */
    } while (s->blockState == TBLOCK_SUBCOMMIT);
    /* If we had a COMMIT command, finish off the main xact too */
    if (s->blockState == TBLOCK_END) {
      Assert(s->parent == NULL);
      CommitTransaction();
      s->blockState = TBLOCK_DEFAULT;
      if (s->chain) {
        StartTransaction();
        s->blockState = TBLOCK_INPROGRESS;
        s->chain = false;
        RestoreTransactionCharacteristics(&savetc);
      }
    } else
      elog(ERROR, "CommitTransactionCommand: unexpected state %s",
           BlockStateAsString(s->blockState));
    break;

    /*
     * The current already-failed subtransaction is ending due to a
     * ROLLBACK or ROLLBACK TO command, so pop it and recursively
     * examine the parent (which could be in any of several states).
     * As we need to examine the parent, return false to request the
     * caller to do the next iteration.
     */
  case TBLOCK_SUBABORT_END:
    CleanupSubTransaction();
    return false;

    /*
     * As above, but it's not dead yet, so abort first.
     */
  case TBLOCK_SUBABORT_PENDING:
    AbortSubTransaction();
    CleanupSubTransaction();
    return false;

    /*
     * The current subtransaction is the target of a ROLLBACK TO
     * command.  Abort and pop it, then start a new subtransaction
     * with the same name.
     */
  case TBLOCK_SUBRESTART: {
    char *name;
    int savepointLevel;

    /* save name and keep Cleanup from freeing it */
    name = s->name;
    s->name = NULL;
    savepointLevel = s->savepointLevel;

    AbortSubTransaction();
    CleanupSubTransaction();

    DefineSavepoint(NULL);
    s = CurrentTransactionState; /* changed by push */
    s->name = name;
    s->savepointLevel = savepointLevel;

    /* This is the same as TBLOCK_SUBBEGIN case */
    Assert(s->blockState == TBLOCK_SUBBEGIN);
    StartSubTransaction();
    s->blockState = TBLOCK_SUBINPROGRESS;
  } break;

    /*
     * Same as above, but the subtransaction had already failed, so we
     * don't need AbortSubTransaction.
     */
  case TBLOCK_SUBABORT_RESTART: {
    char *name;
    int savepointLevel;

    /* save name and keep Cleanup from freeing it */
    name = s->name;
    s->name = NULL;
    savepointLevel = s->savepointLevel;

    CleanupSubTransaction();

    DefineSavepoint(NULL);
    s = CurrentTransactionState; /* changed by push */
    s->name = name;
    s->savepointLevel = savepointLevel;

    /* This is the same as TBLOCK_SUBBEGIN case */
    Assert(s->blockState == TBLOCK_SUBBEGIN);
    StartSubTransaction();
    s->blockState = TBLOCK_SUBINPROGRESS;
  } break;
  }

  /* Done, no more iterations required */
  return true;
}

/*
 *	AbortCurrentTransaction -- a wrapper function handling the
 *		loop over subtransactions to avoid potentially dangerous
 *recursion in AbortCurrentTransactionInternal().
 */
void AbortCurrentTransaction(void) {
  /*
   * Repeatedly call AbortCurrentTransactionInternal() until all the work is
   * done.
   */
  while (!AbortCurrentTransactionInternal()) {
  }
}

/*
 *	AbortCurrentTransactionInternal - a function doing an iteration of work
 *		regarding handling the current transaction abort.  In the case
 *of subtransactions more than one iterations could be required.  Returns true
 *when no more iterations required, false otherwise.
 */
static bool AbortCurrentTransactionInternal(void) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
  case TBLOCK_DEFAULT:
    if (s->state == TRANS_DEFAULT) {
      /* we are idle, so nothing to do */
    } else {
      /*
       * We can get here after an error during transaction start
       * (state will be TRANS_START).  Need to clean up the
       * incompletely started transaction.  First, adjust the
       * low-level state to suppress warning message from
       * AbortTransaction.
       */
      if (s->state == TRANS_START)
        s->state = TRANS_INPROGRESS;
      AbortTransaction();
      CleanupTransaction();
    }
    break;

    /*
     * If we aren't in a transaction block, we just do the basic abort
     * & cleanup transaction.  For this purpose, we treat an implicit
     * transaction block as if it were a simple statement.
     */
  case TBLOCK_STARTED:
  case TBLOCK_IMPLICIT_INPROGRESS:
    AbortTransaction();
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * If we are in TBLOCK_BEGIN it means something screwed up right
     * after reading "BEGIN TRANSACTION".  We assume that the user
     * will interpret the error as meaning the BEGIN failed to get him
     * into a transaction block, so we should abort and return to idle
     * state.
     */
  case TBLOCK_BEGIN:
    AbortTransaction();
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * We are somewhere in a transaction block and we've gotten a
     * failure, so we abort the transaction and set up the persistent
     * ABORT state.  We will stay in ABORT until we get a ROLLBACK.
     */
  case TBLOCK_INPROGRESS:
    AbortTransaction();
    s->blockState = TBLOCK_ABORT;
    /* CleanupTransaction happens when we exit TBLOCK_ABORT_END */
    break;

    /*
     * Here, we failed while trying to COMMIT.  Clean up the
     * transaction and return to idle state (we do not want to stay in
     * the transaction).
     */
  case TBLOCK_END:
    AbortTransaction();
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * Here, we are already in an aborted transaction state and are
     * waiting for a ROLLBACK, but for some reason we failed again! So
     * we just remain in the abort state.
     */
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
    break;

    /*
     * We are in a failed transaction and we got the ROLLBACK command.
     * We have already aborted, we just need to cleanup and go to idle
     * state.
     */
  case TBLOCK_ABORT_END:
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * We are in a live transaction and we got a ROLLBACK command.
     * Abort, cleanup, go to idle state.
     */
  case TBLOCK_ABORT_PENDING:
    AbortTransaction();
    CleanupTransaction();
    s->blockState = TBLOCK_DEFAULT;
    break;

    /*
     * We got an error inside a subtransaction.  Abort just the
     * subtransaction, and go to the persistent SUBABORT state until
     * we get ROLLBACK.
     */
  case TBLOCK_SUBINPROGRESS:
    AbortSubTransaction();
    s->blockState = TBLOCK_SUBABORT;
    break;

    /*
     * If we failed while trying to create a subtransaction, clean up
     * the broken subtransaction and abort the parent.  The same
     * applies if we get a failure while ending a subtransaction.  As
     * we need to abort the parent, return false to request the caller
     * to do the next iteration.
     */
  case TBLOCK_SUBBEGIN:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
    AbortSubTransaction();
    CleanupSubTransaction();
    return false;

    /*
     * Same as above, except the Abort() was already done.
     */
  case TBLOCK_SUBABORT_END:
  case TBLOCK_SUBABORT_RESTART:
    CleanupSubTransaction();
    return false;
  }

  /* Done, no more iterations required */
  return true;
}

/*
 *	PreventInTransactionBlock
 *
 *	This routine is to be called by statements that must not run inside
 *	a transaction block, typically because they have non-rollback-able
 *	side effects or do internal commits.
 *
 *	If this routine completes successfully, then the calling statement is
 *	guaranteed that if it completes without error, its results will be
 *	committed immediately.
 *
 *	If we have already started a transaction block, issue an error; also
 *issue an error if we appear to be running inside a user-defined function
 *(which could issue more commands and possibly cause a failure after the
 *statement completes).  Subtransactions are verboten too.
 *
 *	We must also set XACT_FLAGS_NEEDIMMEDIATECOMMIT in MyXactFlags, to
 *ensure that postgres.c follows through by committing after the statement is
 *done.
 *
 *	isTopLevel: passed down from ProcessUtility to determine whether we are
 *	inside a function.  (We will always fail if this is false, but it's
 *	convenient to centralize the check here instead of making callers do
 *it.) stmtType: statement type name, for error messages.
 */
void PreventInTransactionBlock(bool isTopLevel, const char *stmtType) {
  /*
   * xact block already started?
   */
  if (IsTransactionBlock())
    ereport(ERROR,
            (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
             /* translator: %s represents an SQL statement name */
             errmsg("%s cannot run inside a transaction block", stmtType)));

  /*
   * subtransaction?
   */
  if (IsSubTransaction())
    ereport(ERROR, (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
                    /* translator: %s represents an SQL statement name */
                    errmsg("%s cannot run inside a subtransaction", stmtType)));

  /*
   * inside a function call?
   */
  if (!isTopLevel)
    ereport(ERROR, (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
                    /* translator: %s represents an SQL statement name */
                    errmsg("%s cannot be executed from a function", stmtType)));

  /* If we got past IsTransactionBlock test, should be in default state */
  if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
      CurrentTransactionState->blockState != TBLOCK_STARTED)
    elog(FATAL, "cannot prevent transaction chain");

  /* All okay.  Set the flag to make sure the right thing happens later. */
  MyXactFlags |= XACT_FLAGS_NEEDIMMEDIATECOMMIT;
}

/*
 *	WarnNoTransactionBlock
 *	RequireTransactionBlock
 *
 *	These two functions allow for warnings or errors if a command is
 *executed outside of a transaction block.  This is useful for commands that
 *have no effects that persist past transaction end (and so calling them outside
 *a transaction block is presumably an error).  DECLARE CURSOR is an example.
 *	While top-level transaction control commands (BEGIN/COMMIT/ABORT) and
 *SET that have no effect issue warnings, all other no-effect commands generate
 *	errors.
 *
 *	If we appear to be running inside a user-defined function, we do not
 *	issue anything, since the function could issue more commands that make
 *	use of the current statement's results.  Likewise subtransactions.
 *	Thus these are inverses for PreventInTransactionBlock.
 *
 *	isTopLevel: passed down from ProcessUtility to determine whether we are
 *	inside a function.
 *	stmtType: statement type name, for warning or error messages.
 */
void WarnNoTransactionBlock(bool isTopLevel, const char *stmtType) {
  CheckTransactionBlock(isTopLevel, false, stmtType);
}

void RequireTransactionBlock(bool isTopLevel, const char *stmtType) {
  CheckTransactionBlock(isTopLevel, true, stmtType);
}

/*
 * This is the implementation of the above two.
 */
static void CheckTransactionBlock(bool isTopLevel, bool throwError,
                                  const char *stmtType) {
  /*
   * xact block already started?
   */
  if (IsTransactionBlock())
    return;

  /*
   * subtransaction?
   */
  if (IsSubTransaction())
    return;

  /*
   * inside a function call?
   */
  if (!isTopLevel)
    return;

  ereport(throwError ? ERROR : WARNING,
          (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
           /* translator: %s represents an SQL statement name */
           errmsg("%s can only be used in transaction blocks", stmtType)));
}

/*
 *	IsInTransactionBlock
 *
 *	This routine is for statements that need to behave differently inside
 *	a transaction block than when running as single commands.  ANALYZE is
 *	currently the only example.
 *
 *	If this routine returns "false", then the calling statement is allowed
 *	to perform internal transaction-commit-and-start cycles; there is not a
 *	risk of messing up any transaction already in progress.  (Note that this
 *	is not the identical guarantee provided by PreventInTransactionBlock,
 *	since we will not force a post-statement commit.)
 *
 *	isTopLevel: passed down from ProcessUtility to determine whether we are
 *	inside a function.
 */
bool IsInTransactionBlock(bool isTopLevel) {
  /*
   * Return true on same conditions that would make
   * PreventInTransactionBlock error out
   */
  if (IsTransactionBlock())
    return true;

  if (IsSubTransaction())
    return true;

  if (!isTopLevel)
    return true;

  if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
      CurrentTransactionState->blockState != TBLOCK_STARTED)
    return true;

  return false;
}

/* ----------------------------------------------------------------
 *					   transaction block support
 * ----------------------------------------------------------------
 */

/*
 *	BeginTransactionBlock
 *		This executes a BEGIN command.
 */
void BeginTransactionBlock(void) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
    /*
     * We are not inside a transaction block, so allow one to begin.
     */
  case TBLOCK_STARTED:
    s->blockState = TBLOCK_BEGIN;
    break;

    /*
     * BEGIN converts an implicit transaction block to a regular one.
     * (Note that we allow this even if we've already done some
     * commands, which is a bit odd but matches historical practice.)
     */
  case TBLOCK_IMPLICIT_INPROGRESS:
    s->blockState = TBLOCK_BEGIN;
    break;

    /*
     * Already a transaction block in progress.
     */
  case TBLOCK_INPROGRESS:
  case TBLOCK_SUBINPROGRESS:
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
    ereport(WARNING, (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
                      errmsg("there is already a transaction in progress")));
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "BeginTransactionBlock: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }
}

/*
 *	PrepareTransactionBlock
 *		This executes a PREPARE command, which is refused: a prepared
 *		transaction's state was kept in WAL, and digres has none.
 */
bool PrepareTransactionBlock(const char *gid) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("PREPARE TRANSACTION is not supported")));
  return false;
}

/*
 *	EndTransactionBlock
 *		This executes a COMMIT command.
 *
 * Since COMMIT may actually do a ROLLBACK, the result indicates what
 * happened: true for COMMIT, false for ROLLBACK.
 *
 * Note that we don't actually do anything here except change blockState.
 * The real work will be done in the upcoming CommitTransactionCommand().
 * We do it this way because it's not convenient to change memory context,
 * resource owner, etc while executing inside a Portal.
 */
bool EndTransactionBlock(bool chain) {
  TransactionState s = CurrentTransactionState;
  bool result = false;

  switch (s->blockState) {
    /*
     * We are in a transaction block, so tell CommitTransactionCommand
     * to COMMIT.
     */
  case TBLOCK_INPROGRESS:
    s->blockState = TBLOCK_END;
    result = true;
    break;

    /*
     * We are in an implicit transaction block.  If AND CHAIN was
     * specified, error.  Otherwise commit, but issue a warning
     * because there was no explicit BEGIN before this.
     */
  case TBLOCK_IMPLICIT_INPROGRESS:
    if (chain)
      ereport(ERROR, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                      /* translator: %s represents an SQL statement name */
                      errmsg("%s can only be used in transaction blocks",
                             "COMMIT AND CHAIN")));
    else
      ereport(WARNING, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                        errmsg("there is no transaction in progress")));
    s->blockState = TBLOCK_END;
    result = true;
    break;

    /*
     * We are in a failed transaction block.  Tell
     * CommitTransactionCommand it's time to exit the block.
     */
  case TBLOCK_ABORT:
    s->blockState = TBLOCK_ABORT_END;
    break;

    /*
     * We are in a live subtransaction block.  Set up to subcommit all
     * open subtransactions and then commit the main transaction.
     */
  case TBLOCK_SUBINPROGRESS:
    while (s->parent != NULL) {
      if (s->blockState == TBLOCK_SUBINPROGRESS)
        s->blockState = TBLOCK_SUBCOMMIT;
      else
        elog(FATAL, "EndTransactionBlock: unexpected state %s",
             BlockStateAsString(s->blockState));
      s = s->parent;
    }
    if (s->blockState == TBLOCK_INPROGRESS)
      s->blockState = TBLOCK_END;
    else
      elog(FATAL, "EndTransactionBlock: unexpected state %s",
           BlockStateAsString(s->blockState));
    result = true;
    break;

    /*
     * Here we are inside an aborted subtransaction.  Treat the COMMIT
     * as ROLLBACK: set up to abort everything and exit the main
     * transaction.
     */
  case TBLOCK_SUBABORT:
    while (s->parent != NULL) {
      if (s->blockState == TBLOCK_SUBINPROGRESS)
        s->blockState = TBLOCK_SUBABORT_PENDING;
      else if (s->blockState == TBLOCK_SUBABORT)
        s->blockState = TBLOCK_SUBABORT_END;
      else
        elog(FATAL, "EndTransactionBlock: unexpected state %s",
             BlockStateAsString(s->blockState));
      s = s->parent;
    }
    if (s->blockState == TBLOCK_INPROGRESS)
      s->blockState = TBLOCK_ABORT_PENDING;
    else if (s->blockState == TBLOCK_ABORT)
      s->blockState = TBLOCK_ABORT_END;
    else
      elog(FATAL, "EndTransactionBlock: unexpected state %s",
           BlockStateAsString(s->blockState));
    break;

    /*
     * The user issued COMMIT when not inside a transaction.  For
     * COMMIT without CHAIN, issue a WARNING, staying in
     * TBLOCK_STARTED state.  The upcoming call to
     * CommitTransactionCommand() will then close the transaction and
     * put us back into the default state.  For COMMIT AND CHAIN,
     * error.
     */
  case TBLOCK_STARTED:
    if (chain)
      ereport(ERROR, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                      /* translator: %s represents an SQL statement name */
                      errmsg("%s can only be used in transaction blocks",
                             "COMMIT AND CHAIN")));
    else
      ereport(WARNING, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                        errmsg("there is no transaction in progress")));
    result = true;
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "EndTransactionBlock: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  Assert(s->blockState == TBLOCK_STARTED || s->blockState == TBLOCK_END ||
         s->blockState == TBLOCK_ABORT_END ||
         s->blockState == TBLOCK_ABORT_PENDING);

  s->chain = chain;

  return result;
}

/*
 *	UserAbortTransactionBlock
 *		This executes a ROLLBACK command.
 *
 * As above, we don't actually do anything here except change blockState.
 */
void UserAbortTransactionBlock(bool chain) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
    /*
     * We are inside a transaction block and we got a ROLLBACK command
     * from the user, so tell CommitTransactionCommand to abort and
     * exit the transaction block.
     */
  case TBLOCK_INPROGRESS:
    s->blockState = TBLOCK_ABORT_PENDING;
    break;

    /*
     * We are inside a failed transaction block and we got a ROLLBACK
     * command from the user.  Abort processing is already done, so
     * CommitTransactionCommand just has to cleanup and go back to
     * idle state.
     */
  case TBLOCK_ABORT:
    s->blockState = TBLOCK_ABORT_END;
    break;

    /*
     * We are inside a subtransaction.  Mark everything up to top
     * level as exitable.
     */
  case TBLOCK_SUBINPROGRESS:
  case TBLOCK_SUBABORT:
    while (s->parent != NULL) {
      if (s->blockState == TBLOCK_SUBINPROGRESS)
        s->blockState = TBLOCK_SUBABORT_PENDING;
      else if (s->blockState == TBLOCK_SUBABORT)
        s->blockState = TBLOCK_SUBABORT_END;
      else
        elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
             BlockStateAsString(s->blockState));
      s = s->parent;
    }
    if (s->blockState == TBLOCK_INPROGRESS)
      s->blockState = TBLOCK_ABORT_PENDING;
    else if (s->blockState == TBLOCK_ABORT)
      s->blockState = TBLOCK_ABORT_END;
    else
      elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
           BlockStateAsString(s->blockState));
    break;

    /*
     * The user issued ABORT when not inside a transaction.  For
     * ROLLBACK without CHAIN, issue a WARNING and go to abort state.
     * The upcoming call to CommitTransactionCommand() will then put
     * us back into the default state.  For ROLLBACK AND CHAIN, error.
     *
     * We do the same thing with ABORT inside an implicit transaction,
     * although in this case we might be rolling back actual database
     * state changes.  (It's debatable whether we should issue a
     * WARNING in this case, but we have done so historically.)
     */
  case TBLOCK_STARTED:
  case TBLOCK_IMPLICIT_INPROGRESS:
    if (chain)
      ereport(ERROR, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                      /* translator: %s represents an SQL statement name */
                      errmsg("%s can only be used in transaction blocks",
                             "ROLLBACK AND CHAIN")));
    else
      ereport(WARNING, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                        errmsg("there is no transaction in progress")));
    s->blockState = TBLOCK_ABORT_PENDING;
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  Assert(s->blockState == TBLOCK_ABORT_END ||
         s->blockState == TBLOCK_ABORT_PENDING);

  s->chain = chain;
}

/*
 * BeginImplicitTransactionBlock
 *		Start an implicit transaction block if we're not already in one.
 *
 * Unlike BeginTransactionBlock, this is called directly from the main loop
 * in postgres.c, not within a Portal.  So we can just change blockState
 * without a lot of ceremony.  We do not expect caller to do
 * CommitTransactionCommand/StartTransactionCommand.
 */
void BeginImplicitTransactionBlock(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * If we are in STARTED state (that is, no transaction block is open),
   * switch to IMPLICIT_INPROGRESS state, creating an implicit transaction
   * block.
   *
   * For caller convenience, we consider all other transaction states as
   * legal here; otherwise the caller would need its own state check, which
   * seems rather pointless.
   */
  if (s->blockState == TBLOCK_STARTED)
    s->blockState = TBLOCK_IMPLICIT_INPROGRESS;
}

/*
 * EndImplicitTransactionBlock
 *		End an implicit transaction block, if we're in one.
 *
 * Like EndTransactionBlock, we just make any needed blockState change here.
 * The real work will be done in the upcoming CommitTransactionCommand().
 */
void EndImplicitTransactionBlock(void) {
  TransactionState s = CurrentTransactionState;

  /*
   * If we are in IMPLICIT_INPROGRESS state, switch back to STARTED state,
   * allowing CommitTransactionCommand to commit whatever happened during
   * the implicit transaction block as though it were a single statement.
   *
   * For caller convenience, we consider all other transaction states as
   * legal here; otherwise the caller would need its own state check, which
   * seems rather pointless.
   */
  if (s->blockState == TBLOCK_IMPLICIT_INPROGRESS)
    s->blockState = TBLOCK_STARTED;
}

/*
 * DefineSavepoint
 *		This executes a SAVEPOINT command.
 */
void DefineSavepoint(const char *name) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
  case TBLOCK_INPROGRESS:
  case TBLOCK_SUBINPROGRESS:
    /* Normal subtransaction start */
    PushTransaction();
    s = CurrentTransactionState; /* changed by push */

    /*
     * Savepoint names, like the TransactionState block itself, live
     * in TopTransactionContext.
     */
    if (name)
      s->name = MemoryContextStrdup(TopTransactionContext, name);
    break;

    /*
     * We disallow savepoint commands in implicit transaction blocks.
     * There would be no great difficulty in allowing them so far as
     * this module is concerned, but a savepoint seems inconsistent
     * with exec_simple_query's behavior of abandoning the whole query
     * string upon error.  Also, the point of an implicit transaction
     * block (as opposed to a regular one) is to automatically close
     * after an error, so it's hard to see how a savepoint would fit
     * into that.
     *
     * The error messages for this are phrased as if there were no
     * active transaction block at all, which is historical but
     * perhaps could be improved.
     */
  case TBLOCK_IMPLICIT_INPROGRESS:
    ereport(ERROR,
            (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
             /* translator: %s represents an SQL statement name */
             errmsg("%s can only be used in transaction blocks", "SAVEPOINT")));
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_STARTED:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "DefineSavepoint: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }
}

/*
 * ReleaseSavepoint
 *		This executes a RELEASE command.
 *
 * As above, we don't actually do anything here except change blockState.
 */
void ReleaseSavepoint(const char *name) {
  TransactionState s = CurrentTransactionState;
  TransactionState target, xact;

  switch (s->blockState) {
    /*
     * We can't release a savepoint if there is no savepoint defined.
     */
  case TBLOCK_INPROGRESS:
    ereport(ERROR, (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
                    errmsg("savepoint \"%s\" does not exist", name)));
    break;

  case TBLOCK_IMPLICIT_INPROGRESS:
    /* See comment about implicit transactions in DefineSavepoint */
    ereport(ERROR, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                    /* translator: %s represents an SQL statement name */
                    errmsg("%s can only be used in transaction blocks",
                           "RELEASE SAVEPOINT")));
    break;

    /*
     * We are in a non-aborted subtransaction.  This is the only valid
     * case.
     */
  case TBLOCK_SUBINPROGRESS:
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_STARTED:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "ReleaseSavepoint: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  for (target = s; PointerIsValid(target); target = target->parent) {
    if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
      break;
  }

  if (!PointerIsValid(target))
    ereport(ERROR, (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
                    errmsg("savepoint \"%s\" does not exist", name)));

  /* disallow crossing savepoint level boundaries */
  if (target->savepointLevel != s->savepointLevel)
    ereport(
        ERROR,
        (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
         errmsg(
             "savepoint \"%s\" does not exist within current savepoint level",
             name)));

  /*
   * Mark "commit pending" all subtransactions up to the target
   * subtransaction.  The actual commits will happen when control gets to
   * CommitTransactionCommand.
   */
  xact = CurrentTransactionState;
  for (;;) {
    Assert(xact->blockState == TBLOCK_SUBINPROGRESS);
    xact->blockState = TBLOCK_SUBRELEASE;
    if (xact == target)
      break;
    xact = xact->parent;
    Assert(PointerIsValid(xact));
  }
}

/*
 * RollbackToSavepoint
 *		This executes a ROLLBACK TO <savepoint> command.
 *
 * As above, we don't actually do anything here except change blockState.
 */
void RollbackToSavepoint(const char *name) {
  TransactionState s = CurrentTransactionState;
  TransactionState target, xact;

  switch (s->blockState) {
    /*
     * We can't rollback to a savepoint if there is no savepoint
     * defined.
     */
  case TBLOCK_INPROGRESS:
  case TBLOCK_ABORT:
    ereport(ERROR, (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
                    errmsg("savepoint \"%s\" does not exist", name)));
    break;

  case TBLOCK_IMPLICIT_INPROGRESS:
    /* See comment about implicit transactions in DefineSavepoint */
    ereport(ERROR, (errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
                    /* translator: %s represents an SQL statement name */
                    errmsg("%s can only be used in transaction blocks",
                           "ROLLBACK TO SAVEPOINT")));
    break;

    /*
     * There is at least one savepoint, so proceed.
     */
  case TBLOCK_SUBINPROGRESS:
  case TBLOCK_SUBABORT:
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_STARTED:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "RollbackToSavepoint: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  for (target = s; PointerIsValid(target); target = target->parent) {
    if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
      break;
  }

  if (!PointerIsValid(target))
    ereport(ERROR, (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
                    errmsg("savepoint \"%s\" does not exist", name)));

  /* disallow crossing savepoint level boundaries */
  if (target->savepointLevel != s->savepointLevel)
    ereport(
        ERROR,
        (errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
         errmsg(
             "savepoint \"%s\" does not exist within current savepoint level",
             name)));

  /*
   * Mark "abort pending" all subtransactions up to the target
   * subtransaction.  The actual aborts will happen when control gets to
   * CommitTransactionCommand.
   */
  xact = CurrentTransactionState;
  for (;;) {
    if (xact == target)
      break;
    if (xact->blockState == TBLOCK_SUBINPROGRESS)
      xact->blockState = TBLOCK_SUBABORT_PENDING;
    else if (xact->blockState == TBLOCK_SUBABORT)
      xact->blockState = TBLOCK_SUBABORT_END;
    else
      elog(FATAL, "RollbackToSavepoint: unexpected state %s",
           BlockStateAsString(xact->blockState));
    xact = xact->parent;
    Assert(PointerIsValid(xact));
  }

  /* And mark the target as "restart pending" */
  if (xact->blockState == TBLOCK_SUBINPROGRESS)
    xact->blockState = TBLOCK_SUBRESTART;
  else if (xact->blockState == TBLOCK_SUBABORT)
    xact->blockState = TBLOCK_SUBABORT_RESTART;
  else
    elog(FATAL, "RollbackToSavepoint: unexpected state %s",
         BlockStateAsString(xact->blockState));
}

/*
 * BeginInternalSubTransaction
 *		This is the same as DefineSavepoint except it allows
 *TBLOCK_STARTED, TBLOCK_IMPLICIT_INPROGRESS,
 *and TBLOCK_END states, and therefore it can safely be used in
 *		functions that might be called when not inside a BEGIN block or
 *when running deferred triggers at COMMIT/PREPARE time.  Also, it automatically
 *does CommitTransactionCommand/StartTransactionCommand instead of expecting the
 *caller to do it.
 */
void BeginInternalSubTransaction(const char *name) {
  TransactionState s = CurrentTransactionState;
  bool save_ExitOnAnyError = ExitOnAnyError;

  /*
   * Errors within this function are improbable, but if one does happen we
   * force a FATAL exit.  Callers generally aren't prepared to handle losing
   * control, and moreover our transaction state is probably corrupted if we
   * fail partway through; so an ordinary ERROR longjmp isn't okay.
   */
  ExitOnAnyError = true;

  switch (s->blockState) {
  case TBLOCK_STARTED:
  case TBLOCK_INPROGRESS:
  case TBLOCK_IMPLICIT_INPROGRESS:
  case TBLOCK_END:
  case TBLOCK_SUBINPROGRESS:
    /* Normal subtransaction start */
    PushTransaction();
    s = CurrentTransactionState; /* changed by push */

    /*
     * Savepoint names, like the TransactionState block itself, live
     * in TopTransactionContext.
     */
    if (name)
      s->name = MemoryContextStrdup(TopTransactionContext, name);
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "BeginInternalSubTransaction: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  CommitTransactionCommand();
  StartTransactionCommand();

  ExitOnAnyError = save_ExitOnAnyError;
}

/*
 * ReleaseCurrentSubTransaction
 *
 * RELEASE (ie, commit) the innermost subtransaction, regardless of its
 * savepoint name (if any).
 * NB: do NOT use CommitTransactionCommand/StartTransactionCommand with this.
 */
void ReleaseCurrentSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  if (s->blockState != TBLOCK_SUBINPROGRESS)
    elog(ERROR, "ReleaseCurrentSubTransaction: unexpected state %s",
         BlockStateAsString(s->blockState));
  Assert(s->state == TRANS_INPROGRESS);
  MemoryContextSwitchTo(CurTransactionContext);
  CommitSubTransaction();
  s = CurrentTransactionState; /* changed by pop */
  Assert(s->state == TRANS_INPROGRESS);
}

/*
 * RollbackAndReleaseCurrentSubTransaction
 *
 * ROLLBACK and RELEASE (ie, abort) the innermost subtransaction, regardless
 * of its savepoint name (if any).
 * NB: do NOT use CommitTransactionCommand/StartTransactionCommand with this.
 */
void RollbackAndReleaseCurrentSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
    /* Must be in a subtransaction */
  case TBLOCK_SUBINPROGRESS:
  case TBLOCK_SUBABORT:
    break;

    /* These cases are invalid. */
  case TBLOCK_DEFAULT:
  case TBLOCK_STARTED:
  case TBLOCK_BEGIN:
  case TBLOCK_IMPLICIT_INPROGRESS:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_INPROGRESS:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
  case TBLOCK_ABORT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    elog(FATAL, "RollbackAndReleaseCurrentSubTransaction: unexpected state %s",
         BlockStateAsString(s->blockState));
    break;
  }

  /*
   * Abort the current subtransaction, if needed.
   */
  if (s->blockState == TBLOCK_SUBINPROGRESS)
    AbortSubTransaction();

  /* And clean it up, too */
  CleanupSubTransaction();

  s = CurrentTransactionState; /* changed by pop */
  Assert(s->blockState == TBLOCK_SUBINPROGRESS ||
         s->blockState == TBLOCK_INPROGRESS ||
         s->blockState == TBLOCK_IMPLICIT_INPROGRESS ||
         s->blockState == TBLOCK_STARTED);
}

/*
 *	AbortOutOfAnyTransaction
 *
 *	This routine is provided for error recovery purposes.  It aborts any
 *	active transaction or transaction block, leaving the system in a known
 *	idle state.
 */
void AbortOutOfAnyTransaction(void) {
  TransactionState s = CurrentTransactionState;

  /* Ensure we're not running in a doomed memory context */
  AtAbort_Memory();

  /*
   * Get out of any transaction or nested transaction
   */
  do {
    switch (s->blockState) {
    case TBLOCK_DEFAULT:
      if (s->state == TRANS_DEFAULT) {
        /* Not in a transaction, do nothing */
      } else {
        /*
         * We can get here after an error during transaction start
         * (state will be TRANS_START).  Need to clean up the
         * incompletely started transaction.  First, adjust the
         * low-level state to suppress warning message from
         * AbortTransaction.
         */
        if (s->state == TRANS_START)
          s->state = TRANS_INPROGRESS;
        AbortTransaction();
        CleanupTransaction();
      }
      break;
    case TBLOCK_STARTED:
    case TBLOCK_BEGIN:
    case TBLOCK_INPROGRESS:
    case TBLOCK_IMPLICIT_INPROGRESS:
    case TBLOCK_END:
    case TBLOCK_ABORT_PENDING:
      /* In a transaction, so clean up */
      AbortTransaction();
      CleanupTransaction();
      s->blockState = TBLOCK_DEFAULT;
      break;
    case TBLOCK_ABORT:
    case TBLOCK_ABORT_END:

      /*
       * AbortTransaction is already done, still need Cleanup.
       * However, if we failed partway through running ROLLBACK,
       * there will be an active portal running that command, which
       * we need to shut down before doing CleanupTransaction.
       */
      AtAbort_Portals();
      CleanupTransaction();
      s->blockState = TBLOCK_DEFAULT;
      break;

      /*
       * In a subtransaction, so clean it up and abort parent too
       */
    case TBLOCK_SUBBEGIN:
    case TBLOCK_SUBINPROGRESS:
    case TBLOCK_SUBRELEASE:
    case TBLOCK_SUBCOMMIT:
    case TBLOCK_SUBABORT_PENDING:
    case TBLOCK_SUBRESTART:
      AbortSubTransaction();
      CleanupSubTransaction();
      s = CurrentTransactionState; /* changed by pop */
      break;

    case TBLOCK_SUBABORT:
    case TBLOCK_SUBABORT_END:
    case TBLOCK_SUBABORT_RESTART:
      /* As above, but AbortSubTransaction already done */
      if (s->curTransactionOwner) {
        /* As in TBLOCK_ABORT, might have a live portal to zap */
        AtSubAbort_Portals(s->subTransactionId, s->parent->subTransactionId,
                           s->curTransactionOwner,
                           s->parent->curTransactionOwner);
      }
      CleanupSubTransaction();
      s = CurrentTransactionState; /* changed by pop */
      break;
    }
  } while (s->blockState != TBLOCK_DEFAULT);

  /* Should be out of all subxacts now */
  Assert(s->parent == NULL);

  /*
   * Revert to TopMemoryContext, to ensure we exit in a well-defined state
   * whether there were any transactions to close or not.  (Callers that
   * don't intend to exit soon should switch to some other context to avoid
   * long-term memory leaks.)
   */
  MemoryContextSwitchTo(TopMemoryContext);
}

/*
 * IsTransactionBlock --- are we within a transaction block?
 */
bool IsTransactionBlock(void) {
  TransactionState s = CurrentTransactionState;

  if (s->blockState == TBLOCK_DEFAULT || s->blockState == TBLOCK_STARTED)
    return false;

  return true;
}

/*
 * IsTransactionOrTransactionBlock --- are we within either a transaction
 * or a transaction block?	(The backend is only really "idle" when this
 * returns false.)
 *
 * This should match up with IsTransactionBlock and IsTransactionState.
 */
bool IsTransactionOrTransactionBlock(void) {
  TransactionState s = CurrentTransactionState;

  if (s->blockState == TBLOCK_DEFAULT)
    return false;

  return true;
}

/*
 * TransactionBlockStatusCode - return status code to send in ReadyForQuery
 */
char TransactionBlockStatusCode(void) {
  TransactionState s = CurrentTransactionState;

  switch (s->blockState) {
  case TBLOCK_DEFAULT:
  case TBLOCK_STARTED:
    return 'I'; /* idle --- not in transaction */
  case TBLOCK_BEGIN:
  case TBLOCK_SUBBEGIN:
  case TBLOCK_INPROGRESS:
  case TBLOCK_IMPLICIT_INPROGRESS:
  case TBLOCK_SUBINPROGRESS:
  case TBLOCK_END:
  case TBLOCK_SUBRELEASE:
  case TBLOCK_SUBCOMMIT:
    return 'T'; /* in transaction */
  case TBLOCK_ABORT:
  case TBLOCK_SUBABORT:
  case TBLOCK_ABORT_END:
  case TBLOCK_SUBABORT_END:
  case TBLOCK_ABORT_PENDING:
  case TBLOCK_SUBABORT_PENDING:
  case TBLOCK_SUBRESTART:
  case TBLOCK_SUBABORT_RESTART:
    return 'E'; /* in failed transaction */
  }

  /* should never get here */
  elog(FATAL, "invalid transaction block state: %s",
       BlockStateAsString(s->blockState));
  return 0; /* keep compiler quiet */
}

/*
 * IsSubTransaction
 */
bool IsSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  if (s->nestingLevel >= 2)
    return true;

  return false;
}

/*
 * StartSubTransaction
 *
 * If you're wondering why this is separate from PushTransaction: it's because
 * we can't conveniently do this stuff right inside DefineSavepoint.  The
 * SAVEPOINT utility command will be executed inside a Portal, and if we
 * muck with CurrentMemoryContext or CurrentResourceOwner then exit from
 * the Portal will undo those settings.  So we make DefineSavepoint just
 * push a dummy transaction block, and when control returns to the main
 * idle loop, CommitTransactionCommand will be called, and we'll come here
 * to finish starting the subtransaction.
 */
static void StartSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  if (s->state != TRANS_DEFAULT)
    elog(WARNING, "StartSubTransaction while in %s state",
         TransStateAsString(s->state));

  s->state = TRANS_START;

  /*
   * Initialize subsystems for new subtransaction
   *
   * must initialize resource-management stuff first
   */
  AtSubStart_Memory();
  AtSubStart_ResourceOwner();
  AfterTriggerBeginSubXact();

  s->state = TRANS_INPROGRESS;

  ShowTransactionState("StartSubTransaction");
}

/*
 * CommitSubTransaction
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void CommitSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  ShowTransactionState("CommitSubTransaction");

  if (s->state != TRANS_INPROGRESS)
    elog(WARNING, "CommitSubTransaction while in %s state",
         TransStateAsString(s->state));

  /* Do the actual "commit", such as it is */
  s->state = TRANS_COMMIT;

  /* Must CCI to ensure commands of subtransaction are seen as done */
  CommandCounterIncrement();

  /*
   * Prior to 8.4 we marked subcommit in clog at this point.  We now only
   * perform that step, if required, as part of the atomic update of the
   * whole transaction tree at top level commit or abort.
   */

  /* Post-commit cleanup */
  AfterTriggerEndSubXact(true);
  AtSubCommit_Portals(s->subTransactionId, s->parent->subTransactionId,
                      s->parent->nestingLevel, s->parent->curTransactionOwner);
  AtEOSubXact_LargeObject(true, s->subTransactionId,
                          s->parent->subTransactionId);

  /* sm keeps the subtransaction's writes. */
  smstore_subcommit();

  ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_BEFORE_LOCKS,
                       true, false);
  AtEOSubXact_RelationCache(true, s->subTransactionId,
                            s->parent->subTransactionId);
  AtEOSubXact_TypeCache();
  AtEOSubXact_Inval(true);
  AtSubCommit_smgr();

  CurrentResourceOwner = s->curTransactionOwner;

  /*
   * Other locks should get transferred to their parent resource owner.
   */
  ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_LOCKS, true,
                       false);
  ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_AFTER_LOCKS,
                       true, false);

  AtEOXact_GUC(true, s->gucNestLevel);
  AtEOSubXact_SPI(true, s->subTransactionId);
  AtEOSubXact_on_commit_actions(true, s->subTransactionId,
                                s->parent->subTransactionId);
  AtEOSubXact_Namespace(true, s->subTransactionId, s->parent->subTransactionId);
  AtEOSubXact_Files(true, s->subTransactionId, s->parent->subTransactionId);
  AtEOSubXact_HashTables(true, s->nestingLevel);
  AtSubCommit_Snapshot(s->nestingLevel);

  /*
   * We need to restore the upper transaction's read-only state, in case the
   * upper is read-write while the child is read-only; GUC will incorrectly
   * think it should leave the child state in place.
   */
  XactReadOnly = s->prevXactReadOnly;

  CurrentResourceOwner = s->parent->curTransactionOwner;
  CurTransactionResourceOwner = s->parent->curTransactionOwner;
  ResourceOwnerDelete(s->curTransactionOwner);
  s->curTransactionOwner = NULL;

  AtSubCommit_Memory();

  s->state = TRANS_DEFAULT;

  PopTransaction();
}

/*
 * AbortSubTransaction
 */
static void AbortSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  /* Prevent cancel/die interrupt while cleaning up */
  HOLD_INTERRUPTS();

  /* Make sure we have a valid memory context and resource owner */
  AtSubAbort_Memory();
  AtSubAbort_ResourceOwner();

  /*
   * Release any LW locks we might be holding as quickly as possible.
   * (Regular locks, however, must be held till we finish aborting.)
   * Releasing LW locks is critical since we might try to grab them again
   * while cleaning up!
   *
   * FIXME This may be incorrect --- Are there some locks we should keep?
   * Buffer locks, for example?  I don't think so but I'm not sure.
   */
  LWLockReleaseAll();

  UnlockBuffers();

  /* Cancel condition variable sleep */
  ConditionVariableCancelSleep();

  /*
   * Also clean up any open wait for lock, since the lock manager will choke
   * if we try to wait for another lock before doing this.
   */
  LockErrorCleanup();

  /*
   * If any timeout events are still active, make sure the timeout interrupt
   * is scheduled.  This covers possible loss of a timeout interrupt due to
   * longjmp'ing out of the SIGINT handler (see notes in handle_sig_alarm).
   * We delay this till after LockErrorCleanup so that we don't uselessly
   * reschedule lock or deadlock check timeouts.
   */
  reschedule_timeouts();

  /*
   * Re-enable signals, in case we got here by longjmp'ing out of a signal
   * handler.  We do this fairly early in the sequence so that the timeout
   * infrastructure will be functional if needed while aborting.
   */
  sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

  /*
   * check the current transaction state
   */
  ShowTransactionState("AbortSubTransaction");

  if (s->state != TRANS_INPROGRESS)
    elog(WARNING, "AbortSubTransaction while in %s state",
         TransStateAsString(s->state));

  s->state = TRANS_ABORT;

  /*
   * Reset user ID which might have been changed transiently.  (See notes in
   * AbortTransaction.)
   */
  SetUserIdAndSecContext(s->prevUser, s->prevSecContext);

  /* Forget about any active REINDEX. */
  ResetReindexState(s->nestingLevel);

  /*
   * No need for SnapBuildResetExportedSnapshotState() here, snapshot
   * exports are not supported in subtransactions.
   */

  /*
   * We can skip all this stuff if the subxact failed before creating a
   * ResourceOwner...
   */
  if (s->curTransactionOwner) {
    AfterTriggerEndSubXact(false);
    AtSubAbort_Portals(s->subTransactionId, s->parent->subTransactionId,
                       s->curTransactionOwner, s->parent->curTransactionOwner);
    AtEOSubXact_LargeObject(false, s->subTransactionId,
                            s->parent->subTransactionId);

    /* sm undoes the subtransaction's writes. */
    smstore_subabort();

    ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_BEFORE_LOCKS,
                         false, false);

    AtEOSubXact_RelationCache(false, s->subTransactionId,
                              s->parent->subTransactionId);
    AtEOSubXact_TypeCache();
    AtEOSubXact_Inval(false);
    ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_LOCKS, false,
                         false);
    ResourceOwnerRelease(s->curTransactionOwner, RESOURCE_RELEASE_AFTER_LOCKS,
                         false, false);
    AtSubAbort_smgr();

    AtEOXact_GUC(false, s->gucNestLevel);
    AtEOSubXact_SPI(false, s->subTransactionId);
    AtEOSubXact_on_commit_actions(false, s->subTransactionId,
                                  s->parent->subTransactionId);
    AtEOSubXact_Namespace(false, s->subTransactionId,
                          s->parent->subTransactionId);
    AtEOSubXact_Files(false, s->subTransactionId, s->parent->subTransactionId);
    AtEOSubXact_HashTables(false, s->nestingLevel);
    AtSubAbort_Snapshot(s->nestingLevel);
  }

  /*
   * Restore the upper transaction's read-only state, too.  This should be
   * redundant with GUC's cleanup but we may as well do it for consistency
   * with the commit case.
   */
  XactReadOnly = s->prevXactReadOnly;

  RESUME_INTERRUPTS();
}

/*
 * CleanupSubTransaction
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void CleanupSubTransaction(void) {
  TransactionState s = CurrentTransactionState;

  ShowTransactionState("CleanupSubTransaction");

  if (s->state != TRANS_ABORT)
    elog(WARNING, "CleanupSubTransaction while in %s state",
         TransStateAsString(s->state));

  AtSubCleanup_Portals(s->subTransactionId);

  CurrentResourceOwner = s->parent->curTransactionOwner;
  CurTransactionResourceOwner = s->parent->curTransactionOwner;
  if (s->curTransactionOwner)
    ResourceOwnerDelete(s->curTransactionOwner);
  s->curTransactionOwner = NULL;

  AtSubCleanup_Memory();

  s->state = TRANS_DEFAULT;

  PopTransaction();
}

/*
 * PushTransaction
 *		Create transaction state stack entry for a subtransaction
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void PushTransaction(void) {
  TransactionState p = CurrentTransactionState;
  TransactionState s;

  /*
   * We keep subtransaction state nodes in TopTransactionContext.
   */
  s = (TransactionState)MemoryContextAllocZero(TopTransactionContext,
                                               sizeof(TransactionStateData));

  /*
   * Assign a subtransaction ID, watching out for counter wraparound.
   */
  currentSubTransactionId += 1;
  if (currentSubTransactionId == InvalidSubTransactionId) {
    currentSubTransactionId -= 1;
    pfree(s);
    ereport(
        ERROR,
        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
         errmsg(
             "cannot have more than 2^32-1 subtransactions in a transaction")));
  }

  /*
   * We can now stack a minimally valid subtransaction without fear of
   * failure.
   */
  s->subTransactionId = currentSubTransactionId;
  s->parent = p;
  s->nestingLevel = p->nestingLevel + 1;
  s->gucNestLevel = NewGUCNestLevel();
  s->savepointLevel = p->savepointLevel;
  s->state = TRANS_DEFAULT;
  s->blockState = TBLOCK_SUBBEGIN;
  GetUserIdAndSecContext(&s->prevUser, &s->prevSecContext);
  s->prevXactReadOnly = XactReadOnly;

  CurrentTransactionState = s;

  /*
   * AbortSubTransaction and CleanupSubTransaction have to be able to cope
   * with the subtransaction from here on out; in particular they should not
   * assume that it necessarily has a transaction context, resource owner,
   * or XID.
   */
}

/*
 * PopTransaction
 *		Pop back to parent transaction state
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void PopTransaction(void) {
  TransactionState s = CurrentTransactionState;

  if (s->state != TRANS_DEFAULT)
    elog(WARNING, "PopTransaction while in %s state",
         TransStateAsString(s->state));

  if (s->parent == NULL)
    elog(FATAL, "PopTransaction with no parent");

  CurrentTransactionState = s->parent;

  /* Let's just make sure CurTransactionContext is good */
  CurTransactionContext = s->parent->curTransactionContext;
  MemoryContextSwitchTo(CurTransactionContext);

  /* Ditto for ResourceOwner links */
  CurTransactionResourceOwner = s->parent->curTransactionOwner;
  CurrentResourceOwner = s->parent->curTransactionOwner;

  /* Free the old child structure */
  if (s->name)
    pfree(s->name);
  pfree(s);
}

/*
 * ShowTransactionState
 *		Debug support
 */
static void ShowTransactionState(const char *str) {
  /* skip work if message will definitely not be printed */
  if (message_level_is_interesting(DEBUG5))
    ShowTransactionStateRec(str, CurrentTransactionState);
}

/*
 * ShowTransactionStateRec
 *		Recursive subroutine for ShowTransactionState
 */
static void ShowTransactionStateRec(const char *str, TransactionState s) {

  if (s->parent) {
    /*
     * Since this function recurses, it could be driven to stack overflow.
     * This is just a debugging aid, so we can leave out some details
     * instead of erroring out with check_stack_depth().
     */
    if (stack_is_too_deep())
      ereport(DEBUG5,
              (errmsg_internal("%s(%d): parent omitted to avoid stack overflow",
                               str, s->nestingLevel)));
    else
      ShowTransactionStateRec(str, s->parent);
  }

  ereport(
      DEBUG5,
      (errmsg_internal(
          "%s(%d) name: %s; blockState: %s; state: %s, subid/cid: %u/%u%s", str,
          s->nestingLevel, PointerIsValid(s->name) ? s->name : "unnamed",
          BlockStateAsString(s->blockState), TransStateAsString(s->state),
          (unsigned int)s->subTransactionId, (unsigned int)currentCommandId,
          currentCommandIdUsed ? " (used)" : "")));
}

/*
 * BlockStateAsString
 *		Debug support
 */
static const char *BlockStateAsString(TBlockState blockState) {
  switch (blockState) {
  case TBLOCK_DEFAULT:
    return "DEFAULT";
  case TBLOCK_STARTED:
    return "STARTED";
  case TBLOCK_BEGIN:
    return "BEGIN";
  case TBLOCK_INPROGRESS:
    return "INPROGRESS";
  case TBLOCK_IMPLICIT_INPROGRESS:
    return "IMPLICIT_INPROGRESS";
  case TBLOCK_END:
    return "END";
  case TBLOCK_ABORT:
    return "ABORT";
  case TBLOCK_ABORT_END:
    return "ABORT_END";
  case TBLOCK_ABORT_PENDING:
    return "ABORT_PENDING";
  case TBLOCK_SUBBEGIN:
    return "SUBBEGIN";
  case TBLOCK_SUBINPROGRESS:
    return "SUBINPROGRESS";
  case TBLOCK_SUBRELEASE:
    return "SUBRELEASE";
  case TBLOCK_SUBCOMMIT:
    return "SUBCOMMIT";
  case TBLOCK_SUBABORT:
    return "SUBABORT";
  case TBLOCK_SUBABORT_END:
    return "SUBABORT_END";
  case TBLOCK_SUBABORT_PENDING:
    return "SUBABORT_PENDING";
  case TBLOCK_SUBRESTART:
    return "SUBRESTART";
  case TBLOCK_SUBABORT_RESTART:
    return "SUBABORT_RESTART";
  }
  return "UNRECOGNIZED";
}

/*
 * TransStateAsString
 *		Debug support
 */
static const char *TransStateAsString(TransState state) {
  switch (state) {
  case TRANS_DEFAULT:
    return "DEFAULT";
  case TRANS_START:
    return "START";
  case TRANS_INPROGRESS:
    return "INPROGRESS";
  case TRANS_COMMIT:
    return "COMMIT";
  case TRANS_ABORT:
    return "ABORT";
  case TRANS_PREPARE:
    return "PREPARE";
  }
  return "UNRECOGNIZED";
}

/*
 * xactGetCommittedChildren
 *
 * Gets the list of committed children of the current transaction.  The return
 * value is the number of child transactions.  *ptr is set to point to an
 * array of TransactionIds.  The array is allocated in TopTransactionContext;
 * the caller should *not* pfree() it (this is a change from pre-8.4 code!).
 * If there are no subxacts, *ptr is set to NULL.
 */
int xactGetCommittedChildren(TransactionId **ptr) {
  /* A transaction has no xid, nor its children: digres assigns none. */
  *ptr = NULL;
  return 0;
}

/*
 *	XLOG support routines
 */
