/*-------------------------------------------------------------------------
 *
 * parse_tool.c
 *		"digres --parse": parse standard input, and print the trees
 *
 *   digres --parse < file.sql              one tree a statement, or
 *                                            the error
 *   digres --parse --type-name < file      the text parsed as one
 *                                            type name
 *   digres --parse --locations < file.sql  each statement whole, with
 *                                            every location shown
 *   digres --parse --analyze < file.sql    each statement analyzed: the
 *                                            Query, or the error
 *   digres --parse --plan < file.sql       each statement planned: the
 *                                            PlannedStmt, or the error
 *   digres --parse --run < file.sql        each statement run, and its
 *                                            rows printed
 *   digres --parse ... -c NAME=VALUE       the same with a setting set,
 *                                            as the server's -c sets it
 *   digres --parse ... --threads N         the same in N threads at
 *                                            once, each a session; what
 *                                            the first printed, and a
 *                                            line for each that printed
 *                                            otherwise; the status is a
 *                                            session's that failed
 *
 * This runs raw_parser() with no server around it. A tree is printed
 * as nodeToString() writes it, and an error as its message, position,
 * detail and hint: the form ../sq's tests keep PostgreSQL's own
 * answers in. Given this program as their parser, those tests check
 * that digres's raw_parser() still builds what PostgreSQL's built.
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_tool.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "executor/executor.h"
#include "parser/analyze.h"
#include "parser/parse_tool.h"
#include "parser/parser.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/resowner.h"
#include "utils/memutils.h"
#include "utils/session_thread.h"

/* One session: what it is to do, and what it printed. */
typedef struct tool_session_t {
  const char *sql;
  bool type_name, locations, analyze, plan, run;
  FILE *out;
  char *printed; /* what out holds, when it is a memory stream */
  size_t printed_len;
  int status;
} tool_session_t;

/* Read all of standard input into a string. */
static char *read_stdin(void) {
  StringInfoData buf;
  char chunk[8192];
  size_t n;

  initStringInfo(&buf);
  while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0)
    appendBinaryStringInfo(&buf, chunk, n);
  return buf.data;
}

/* Print the error just caught as ../sq's tests expect one, and clear it. */
static void print_error(FILE *out) {
  ErrorData *err;

  MemoryContextSwitchTo(TopMemoryContext);
  err = CopyErrorData();
  FlushErrorState();
  fprintf(out, "ERROR: %s", err->message);
  if (err->cursorpos > 0)
    fprintf(out, " at character %d", err->cursorpos);
  fprintf(out, "\n");
  if (err->detail)
    fprintf(out, "DETAIL: %s\n", err->detail);
  if (err->hint)
    fprintf(out, "HINT: %s\n", err->hint);
}

/*
 * Take one statement through the analyzer, the rewriter and the planner,
 * and print each plan; or, if run is set, execute each and print its rows.
 */
static void plan_or_run(RawStmt *raw, const char *sql, bool run, FILE *out) {
  List *queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
  ListCell *lc;

  foreach (lc, queries) {
    PlannedStmt *stmt = pg_plan_query((Query *)lfirst(lc), sql, 0, NULL);
    QueryDesc *desc;

    if (!run) {
      fprintf(out, "%s\n", nodeToString(stmt));
      continue;
    }

    /* The executor keeps what it opens in the current resource owner. */
    if (CurrentResourceOwner == NULL)
      CurrentResourceOwner = ResourceOwnerCreate(NULL, "parse tool");

    desc = CreateQueryDesc(stmt, sql, InvalidSnapshot, InvalidSnapshot,
                           CreateDestReceiver(DestDebug), NULL, NULL, 0);
    ExecutorStart(desc, 0);
    ExecutorRun(desc, ForwardScanDirection, 0);
    ExecutorFinish(desc);
    ExecutorEnd(desc);
    FreeQueryDesc(desc);
  }
}

/* Run one session over s->sql, printing to s->out. */
static void run_session(void *arg) {
  tool_session_t *s = (tool_session_t *)arg;

  /* The parser asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  /*
   * A statement that is planned or run is checked against a user. With
   * no server there is none, so it is the bootstrap superuser, as it is
   * in a standalone backend.
   */
  if (s->plan || s->run)
    InitializeSessionUserIdStandalone();
  debug_out = s->out;

  PG_TRY();
  {
    List *trees = raw_parser(s->sql, s->type_name ? RAW_PARSE_TYPE_NAME
                                                  : RAW_PARSE_DEFAULT);
    ListCell *lc;

    foreach (lc, trees) {
      Node *node = (Node *)lfirst(lc);

      /* A statement comes wrapped in where it was written. */
      if (s->analyze)
        fprintf(s->out, "%s\n",
                nodeToString(parse_analyze_fixedparams((RawStmt *)node, s->sql,
                                                       NULL, 0, NULL)));
      else if (s->plan || s->run)
        plan_or_run((RawStmt *)node, s->sql, s->run, s->out);
      else if (s->locations)
        fprintf(s->out, "%s\n", nodeToStringWithLocations(node));
      else {
        if (IsA(node, RawStmt))
          node = ((RawStmt *)node)->stmt;
        fprintf(s->out, "%s\n", nodeToString(node));
      }
    }
  }
  PG_CATCH();
  {
    print_error(s->out);
    s->status = 1;
  }
  PG_END_TRY();
}

/*
 * Run n sessions of what proto says at once, each in a thread, and print
 * what the first printed; the status is 1 if any printed otherwise.
 */
static int run_threads(const tool_session_t *proto, int n) {
  tool_session_t *s = (tool_session_t *)palloc0(n * sizeof(*s));
  pthread_t *threads = (pthread_t *)palloc(n * sizeof(*threads));
  int status = 0;

  for (int i = 0; i < n; i++) {
    s[i] = *proto;
    s[i].out = open_memstream(&s[i].printed, &s[i].printed_len);
    threads[i] = session_thread_start(run_session, &s[i]);
  }
  for (int i = 0; i < n; i++) {
    void *code;

    /* A session that ended by FATAL gives its exit code, not a status. */
    pthread_join(threads[i], &code);
    if (code != NULL)
      s[i].status = (int)(intptr_t)code;
    fclose(s[i].out);
  }

  fwrite(s[0].printed, 1, s[0].printed_len, stdout);
  for (int i = 0; i < n; i++) {
    if (i > 0 && (s[i].printed_len != s[0].printed_len ||
                  memcmp(s[i].printed, s[0].printed, s[0].printed_len))) {
      printf("thread %d printed otherwise:\n", i);
      fwrite(s[i].printed, 1, s[i].printed_len, stdout);
      status = 1;
    }
    status |= s[i].status;
  }
  for (int i = 0; i < n; i++)
    free(s[i].printed);
  return status;
}

/* Parse standard input and print it: see the head of this file. */
int parse_tool_main(int argc, char **argv) {
  tool_session_t s = {};
  int threads = 0;

  s.type_name = argc > 2 && strcmp(argv[2], "--type-name") == 0;
  s.locations = argc > 2 && strcmp(argv[2], "--locations") == 0;
  s.analyze = argc > 2 && strcmp(argv[2], "--analyze") == 0;
  s.plan = argc > 2 && strcmp(argv[2], "--plan") == 0;
  s.run = argc > 2 && strcmp(argv[2], "--run") == 0;
  /* The settings come first: a session's user is one, and each thread
   * is given this thread's. */
  InitializeGUCOptions();

  for (int i = 2; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--threads") == 0)
      threads = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "-c") == 0) {
      char *name, *value;

      ParseLongOption(argv[i + 1], &name, &value);
      if (value == NULL)
        elog(FATAL, "-c %s: no value given", name);
      SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
    }
  }

  s.sql = read_stdin();
  if (threads > 0)
    return run_threads(&s, threads);
  s.out = stdout;
  run_session(&s);
  return s.status;
}
