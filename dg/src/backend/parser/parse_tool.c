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
static void print_error(void) {
  ErrorData *err;

  MemoryContextSwitchTo(TopMemoryContext);
  err = CopyErrorData();
  FlushErrorState();
  printf("ERROR: %s", err->message);
  if (err->cursorpos > 0)
    printf(" at character %d", err->cursorpos);
  printf("\n");
  if (err->detail)
    printf("DETAIL: %s\n", err->detail);
  if (err->hint)
    printf("HINT: %s\n", err->hint);
}

/*
 * Take one statement through the analyzer, the rewriter and the planner,
 * and print each plan; or, if run is set, execute each and print its rows.
 */
static void plan_or_run(RawStmt *raw, const char *sql, bool run) {
  List *queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
  ListCell *lc;

  foreach (lc, queries) {
    PlannedStmt *stmt = pg_plan_query((Query *)lfirst(lc), sql, 0, NULL);
    QueryDesc *desc;

    if (!run) {
      printf("%s\n", nodeToString(stmt));
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

/* Parse standard input and print it: see the head of this file. */
int parse_tool_main(int argc, char **argv) {
  bool type_name = argc > 2 && strcmp(argv[2], "--type-name") == 0;
  bool locations = argc > 2 && strcmp(argv[2], "--locations") == 0;
  bool analyze = argc > 2 && strcmp(argv[2], "--analyze") == 0;
  bool plan = argc > 2 && strcmp(argv[2], "--plan") == 0;
  bool run = argc > 2 && strcmp(argv[2], "--run") == 0;
  char *sql;
  int status = 0;

  /* The parser asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  /*
   * A statement that is planned or run is checked against a user. With
   * no server there is none, so it is the bootstrap superuser, as it is
   * in a standalone backend.
   */
  if (plan || run) {
    /* Setting the user sets a setting: the settings come first. */
    InitializeGUCOptions();
    InitializeSessionUserIdStandalone();
  }

  sql = read_stdin();

  PG_TRY();
  {
    List *trees =
        raw_parser(sql, type_name ? RAW_PARSE_TYPE_NAME : RAW_PARSE_DEFAULT);
    ListCell *lc;

    foreach (lc, trees) {
      Node *node = (Node *)lfirst(lc);

      /* A statement comes wrapped in where it was written. */
      if (analyze)
        printf("%s\n", nodeToString(parse_analyze_fixedparams(
                           (RawStmt *)node, sql, NULL, 0, NULL)));
      else if (plan || run)
        plan_or_run((RawStmt *)node, sql, run);
      else if (locations)
        printf("%s\n", nodeToStringWithLocations(node));
      else {
        if (IsA(node, RawStmt))
          node = ((RawStmt *)node)->stmt;
        printf("%s\n", nodeToString(node));
      }
    }
  }
  PG_CATCH();
  {
    print_error();
    status = 1;
  }
  PG_END_TRY();

  return status;
}
