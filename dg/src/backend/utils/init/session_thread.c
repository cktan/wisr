/*-------------------------------------------------------------------------
 *
 * session_thread.c
 *	  Start a session in a thread of its own.
 *
 * A new thread's thread_local variables hold what the source wrote, not
 * what its parent has set since (c.h). So the thread is given, as
 * PostgreSQL's EXEC_BACKEND child is given by its postmaster, what it
 * cannot work out for itself: the SHARED values the parent set, and the
 * settings that are not at their defaults. The rest it sets up itself,
 * memory contexts first; nothing of the parent's private memory is
 * copied.
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/session_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/session_thread.h"

/* What a parent hands its new thread, in malloc'd memory the thread frees. */
typedef struct session_start_t {
  void (*fn)(void *);
  void *arg;
  const char *progname;
  char *settings; /* the non-default settings, as GUC writes them */
  size_t settings_len;
} session_start_t;

/* The new thread: set up as its parent was, then run the session. */
static void *session_thread_main(void *p) {
  session_start_t start = *(session_start_t *)p;

  free(p);
  MyProcPid = getpid();
  /* Errors and allocations need the memory contexts, so they come first. */
  MemoryContextInit();
  progname = start.progname;

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

  start.fn(start.arg);
  return NULL;
}

/*
 * Start a thread that runs fn(arg) as a session, given what this thread
 * has set that it needs. fn must catch its own errors.
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
  start->progname = progname;
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
