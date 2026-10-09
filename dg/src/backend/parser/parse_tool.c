/*-------------------------------------------------------------------------
 *
 * parse_tool.c
 *		"postgres --parse": parse standard input, and print the trees
 *
 *   postgres --parse < file.sql              one tree a statement, or
 *                                            the error
 *   postgres --parse --type-name < file      the text parsed as one
 *                                            type name
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
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/parse_tool.h"
#include "parser/parser.h"
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

/* Parse standard input and print it: see the head of this file. */
int parse_tool_main(int argc, char **argv) {
  bool type_name = argc > 2 && strcmp(argv[2], "--type-name") == 0;
  char *sql;
  int status = 0;

  /* The parser asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  sql = read_stdin();

  PG_TRY();
  {
    List *trees =
        raw_parser(sql, type_name ? RAW_PARSE_TYPE_NAME : RAW_PARSE_DEFAULT);
    ListCell *lc;

    foreach (lc, trees) {
      Node *node = (Node *)lfirst(lc);

      /* A statement comes wrapped in where it was written. */
      if (IsA(node, RawStmt))
        node = ((RawStmt *)node)->stmt;
      printf("%s\n", nodeToString(node));
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
