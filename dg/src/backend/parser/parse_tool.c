/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

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

#include "catalog/index.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/parse_tool.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "utils/pg_locale.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/* One session: what it is to do, and how it ended. */
typedef struct tool_session_t {
  const char *sql;
  bool type_name, locations, analyze, plan;
  FILE *out;
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
 * and print each plan.
 */
static void plan(RawStmt *raw, const char *sql, FILE *out) {
  List *queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
  ListCell *lc;

  foreach (lc, pg_plan_queries(queries, sql, 0, NULL))
    fprintf(out, "%s\n", nodeToString(lfirst_node(PlannedStmt, lc)));
}

/* Run one session over s->sql, printing to s->out. */
static void run_session(tool_session_t *s) {

  /* The parser asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  /*
   * A statement that is planned is checked against a user. With no
   * server there is none, so it is the bootstrap superuser, as it is in a
   * standalone backend.
   */
  if (s->plan)
    InitializeSessionUserIdStandalone();

  /* Text compares by the default collation, C, as in a server session. */
  database_ctype_is_c = true;
  init_database_collation();

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
      else if (s->plan)
        plan((RawStmt *)node, s->sql, s->out);
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

/* Parse standard input and print it: see the head of this file. */
int parse_tool_main(int argc, char **argv) {
  tool_session_t s = {};

  s.type_name = argc > 2 && strcmp(argv[2], "--type-name") == 0;
  s.locations = argc > 2 && strcmp(argv[2], "--locations") == 0;
  s.analyze = argc > 2 && strcmp(argv[2], "--analyze") == 0;
  s.plan = argc > 2 && strcmp(argv[2], "--plan") == 0;
  /*
   * The process's locale is C, as main() sets a server's: before any
   * thread, since setlocale() changes every thread's (postinit.c).
   */
  if (pg_perm_setlocale(LC_COLLATE, "C") == NULL ||
      pg_perm_setlocale(LC_CTYPE, "C") == NULL)
    elog(FATAL, "could not set the C locale");
  /* The settings come first: a session's user is one. */
  InitializeGUCOptions();

  /*
   * An index that names no access method gets PostgreSQL's, btree, not
   * sm's default kind, so the trees are PostgreSQL's (the head of this
   * file).
   */
  raw_parser_index_type = DEFAULT_INDEX_TYPE;
  s.sql = read_stdin();
  s.out = stdout;
  run_session(&s);
  return s.status;
}
