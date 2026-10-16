/*-------------------------------------------------------------------------
 *
 * session_thread.c
 *	  Start a session in a thread of its own.
 *
 * A new thread's thread_local variables hold what the source wrote, not
 * what its parent has set since (c.h). So the thread is given, as
 * PostgreSQL's EXEC_BACKEND child is given by its postmaster, what it
 * cannot work out for itself: a copy of every SHARED variable the parent
 * has, found by the table src/tools/shared_tls.py writes, and the
 * settings that are not at their defaults. The rest it sets up itself,
 * memory contexts first; nothing of the parent's private memory is
 * copied.
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
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/control_block.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/session_thread.h"
#include "utils/snapmgr.h"

/* What a parent hands its new thread, in malloc'd memory the thread frees. */
typedef struct session_start_t {
  void (*fn)(void *);
  void *arg;
  char *shared;   /* the SHARED variables, one after another */
  char *settings; /* the non-default settings, as GUC writes them */
  size_t settings_len;
} session_start_t;

/*
 * This thread's thread-local block: where progname is, less its offset.
 * Through an integer, so _FORTIFY_SOURCE does not take the block for
 * progname alone.
 */
static char *tls_block(void) {
  return (char *)((uintptr_t)&progname - shared_tls_anchor);
}

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
  ShutdownProcessLocalLatch();
  ShutdownFileAccess();
  FreeStaticSnapshots();

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

  /* The parent's SHARED variables, before anything reads one. */
  for (int i = 0, at = 0; i < shared_tls_count; i++) {
    memcpy(tls_block() + shared_tls_table[i].offset, start.shared + at,
           shared_tls_table[i].size);
    at += shared_tls_table[i].size;
  }
  free(start.shared);

  IsSessionThread = true;
  MyProcPid = gettid();

  /* This thread's control block: what it has its own of. */
  tcb_init();

  /* check_stack_depth() measures from here: this thread's stack is its own. */
  (void)set_stack_base();

  /*
   * A session thread takes no signal but the faults it causes itself: the
   * rest are the postmaster's, or come as interrupt bits
   * (session_interrupt()), its timeouts among them.  Every mask the backend
   * sets is this one.
   */
  sigfillset(&BlockSig);
  sigdelset(&BlockSig, SIGSEGV);
  sigdelset(&BlockSig, SIGBUS);
  sigdelset(&BlockSig, SIGFPE);
  sigdelset(&BlockSig, SIGILL);
  sigdelset(&BlockSig, SIGABRT);
  UnBlockSig = BlockSig;
  StartupBlockSig = BlockSig;
  pthread_sigmask(SIG_SETMASK, &BlockSig, NULL);
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

/* The bytes of all the SHARED variables together. */
static size_t shared_len(void) {
  size_t len = 0;

  for (int i = 0; i < shared_tls_count; i++)
    len += shared_tls_table[i].size;
  return len;
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
  start->shared = (char *)malloc(shared_len());
  if (start->shared == NULL)
    elog(ERROR, "out of memory");
  for (int i = 0, at = 0; i < shared_tls_count; i++) {
    memcpy(start->shared + at, tls_block() + shared_tls_table[i].offset,
           shared_tls_table[i].size);
    at += shared_tls_table[i].size;
  }
  fp = open_memstream(&start->settings, &start->settings_len);
  if (fp == NULL)
    elog(ERROR, "could not write the settings: %m");
  write_nondefault_variables_to(fp);
  fclose(fp);

  /* Start the session's thread: it frees start. */
  try {
    return std::thread(session_thread_main, start);
  } catch (const std::system_error &e) {
    free(start->shared);
    free(start->settings);
    free(start);
    elog(ERROR, "could not start a thread: %s", e.what());
  }
  pg_unreachable();
}
