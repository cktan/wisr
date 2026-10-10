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
 * The thread's result is the exit code. A session that returns ends the
 * same way, with code 0.
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
 * freed: its wait sets and signalfd, its files, the snapshots' arrays, then
 * all its memory contexts.
 */
static void session_thread_end(void) {
  if (FeBeWaitSet != NULL) {
    FreeWaitEventSet(FeBeWaitSet);
    FeBeWaitSet = NULL;
  }
  ShutdownLatchWaitSet();
  ShutdownWaitEventSupport();
  ShutdownFileAccess();
  FreeStaticSnapshots();

  /* Nothing allocates from here on. */
  MemoryContextSwitchTo(NULL);
  MemoryContextDelete(TopMemoryContext);
  TopMemoryContext = NULL;
  ErrorContext = NULL;
  AllocSetFreeFreeLists();
}

/* The new thread: set up as its parent was, then run the session. */
static void *session_thread_main(void *p) {
  session_start_t start = *(session_start_t *)p;

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

  /* check_stack_depth() measures from here: this thread's stack is its own. */
  (void)set_stack_base();

  /*
   * A session thread takes no signal but SIGALRM, from its own timer, and
   * the faults it causes itself: the rest are the postmaster's, or come as
   * flags (SignalProc()).  Every mask the backend sets is this one.
   */
  sigfillset(&BlockSig);
  sigdelset(&BlockSig, SIGALRM);
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
  } catch (const proc_exit_t &e) {
    session_thread_end();
    return (void *)(intptr_t)e.code;
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
 * has set that it needs. fn must catch its own errors. Joined, the thread
 * gives the session's exit code: 0 if fn returned.
 */
pthread_t session_thread_start(void (*fn)(void *), void *arg) {
  session_start_t *start = (session_start_t *)malloc(sizeof(*start));
  FILE *fp;
  pthread_t thread;
  int rc;

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

  rc = pthread_create(&thread, NULL, session_thread_main, start);
  if (rc != 0)
    elog(ERROR, "could not start a thread: %s", strerror(rc));
  return thread;
}
