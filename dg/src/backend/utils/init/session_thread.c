/*-------------------------------------------------------------------------
 *
 * session_thread.c
 *	  Start a session in a thread of its own.
 *
 * A new thread's thread_local variables hold what the source wrote, not
 * what its parent has set since (c.h). So the thread is given, as
 * PostgreSQL's EXEC_BACKEND child is given by its postmaster, what it
 * cannot work out for itself: the settings that are not at their
 * defaults, and its parent's hooks and loaded libraries. What the main
 * thread owns it reads through pcb (utils/control_block.h). The rest it
 * sets up itself, memory contexts first; nothing of the parent's private
 * memory is copied.
 *
 * A session ends where a process would exit: proc_exit() runs the
 * thread's exit callbacks and throws, and the thread's top catches it.
 * A session that returns ends the same way.
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/session_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "libpq/pqsignal.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/control_block.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/pg_authid_d.h"
#include "utils/session_thread.h"
#include "utils/hooks.h"
#include "utils/snapmgr.h"

/* What a parent hands its new thread, in malloc'd memory the thread frees. */
typedef struct session_start_t {
  void (*fn)(void *);
  void *arg;
  hooks_t *hooks; /* the parent's hooks, copied */
  dfmgr_state_t *dfmgr;        /* the parent's loaded libraries, copied */
  explain_registry_t *explain; /* the parent's EXPLAIN registry, copied */
  char *settings; /* the non-default settings, as GUC writes them */
  size_t settings_len;
} session_start_t;

/*
 * Free what the session's exit left, which a process's exit would have
 * freed: its wait sets and local latch, its files, the snapshots' arrays, all
 * its memory contexts, then its control block.
 */
static void session_thread_end(void) {
  if (FeBeWaitSet != NULL) {
    FreeWaitEventSet(FeBeWaitSet);
    FeBeWaitSet = NULL;
  }
  ShutdownLatchWaitSet();
  ShutdownWaitEventSupport();
  ShutdownFileAccess();

  /*
   * Nothing allocates from here on.  TopMemoryContext is cleared before it
   * is deleted: deleting it is refused otherwise, as no process would.
   */
  MemoryContextSwitchTo(NULL);
  MemoryContext top = TopMemoryContext;
  TopMemoryContext = NULL;
  ErrorContext = NULL;
  MemoryContextDelete(top);
  AllocSetFreeFreeLists();

  /* Last: what the thread owns goes with its control block. */
  tcb_free();
}

/* The new thread: set up as its parent was, then run the session. */
static void session_thread_main(session_start_t *p) {
  session_start_t start = *p;

  free(p);

  IsSessionThread = true;
  MyProcPid = gettid();

  /* This thread's control block: what it has its own of. */
  tcb_init();

  /*
   * Its parent's libraries, shared_preload_libraries', and the hooks and
   * EXPLAIN options they set.
   */
  *tcb->hooks = *start.hooks;
  delete start.hooks;
  tcb->dfmgr.reset(start.dfmgr);
  tcb->explain.reset(start.explain);

  /* check_stack_depth() measures from here: this thread's stack is its own. */
  (void)set_stack_base();

  /*
   * A session thread keeps its parent's signal mask: the signals the process
   * takes are the postmaster's to read (PostmasterMain()), and a session is
   * interrupted by bits (session_interrupt()), its timeouts among them.
   */

  /* Errors and allocations need the memory contexts, so they come first. */
  MemoryContextInit();

  /* The settings are built for this thread, then given the parent's. */
  InitializeGUCOptions();
  if (start.settings_len > 0) {
    FILE *fp = fmemopen(start.settings, start.settings_len, "r");

    if (fp == NULL)
      elog(FATAL, "could not read the settings: %m");
    read_nondefault_variables_from(fp);
    fclose(fp);
  }
  free(start.settings);

  /* A session that returns ends as one that exits: its callbacks run. */
  try {
    start.fn(start.arg);
    proc_exit(0);
  } catch (const proc_exit_t &) {
    session_thread_end();
  }
}

/*
 * A thread digres did not start, adopted (session_thread_adopt()): what it
 * was given is freed as the thread ends, as a session's end frees it.
 */
struct adopted_thread_t {
  bool adopted = false;
  ~adopted_thread_t();
};

static thread_local /*LOCAL*/ adopted_thread_t adopted_thread;

/* Free an adopted thread's memory contexts and control block, at its end. */
adopted_thread_t::~adopted_thread_t() {
  if (!adopted)
    return;
  /* Nothing allocates from here on, as in session_thread_end(). */
  MemoryContextSwitchTo(NULL);
  MemoryContext top = TopMemoryContext;
  TopMemoryContext = NULL;
  ErrorContext = NULL;
  MemoryContextDelete(top);
  AllocSetFreeFreeLists();
  tcb_free();
}

/*
 * Make this thread, one digres did not start -- one of sm's index builders,
 * which runs digres's evaluator (smstore_index_eval()) -- able to run
 * PostgreSQL's executor and read the compiled-in catalogs, with no session:
 * set up as session_thread_main() sets a session up, with the settings at
 * their defaults, the database's collation and the superuser, and freed as
 * the thread ends.  A thread that has a control block is left alone.
 */
void session_thread_adopt(void) {
  if (tcb != NULL)
    return;
  MyProcPid = gettid();

  /* This thread's control block: what it has its own of. */
  tcb_init();
  adopted_thread.adopted = true;

  /* check_stack_depth() measures from here. */
  (void)set_stack_base();

  /* Errors and allocations need the memory contexts, so they come first. */
  MemoryContextInit();
  InitializeGUCOptions();

  /* The caches the catalogs are read through, empty; then whose reads. */
  RelationCacheInitialize();
  InitCatalogCache();
  /* A transaction's commit visits the portals, of which it has none. */
  EnablePortalManager();
  init_database_collation();
  SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, 0);
}

/*
 * Start a thread that runs fn(arg) as a session, given what this thread
 * has set that it needs. fn must catch its own errors. The caller joins
 * or detaches the thread.
 */
std::thread session_thread_start(void (*fn)(void *), void *arg) {
  session_start_t *start = (session_start_t *)malloc(sizeof(*start));
  FILE *fp;

  if (start == NULL)
    elog(ERROR, "out of memory");
  start->fn = fn;
  start->arg = arg;
  start->hooks = new hooks_t(*tcb->hooks);
  start->dfmgr = dfmgr_state_copy(tcb->dfmgr.get());
  start->explain = explain_registry_copy(tcb->explain.get());
  fp = open_memstream(&start->settings, &start->settings_len);
  if (fp == NULL)
    elog(ERROR, "could not write the settings: %m");
  write_nondefault_variables_to(fp);
  fclose(fp);

  /* Start the session's thread: it frees start. */
  try {
    return std::thread(session_thread_main, start);
  } catch (const std::system_error &e) {
    delete start->hooks;
    dfmgr_state_free_t()(start->dfmgr);
    explain_registry_free_t()(start->explain);
    free(start->settings);
    free(start);
    elog(ERROR, "could not start a thread: %s", e.what());
  }
  pg_unreachable();
}
