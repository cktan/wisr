/*
 * sqparse.c -- parse SQL, and print the parse tree.
 *
 *   sqparse < file.sql      one tree a statement, or the error
 *
 * This is the whole of what sq is tested through: text in, the raw
 * parse tree out, as nodeToString() writes it. No catalog is read:
 * the raw parser knows the grammar and nothing about what a name
 * names.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
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

/* Parse the input and print each statement's tree, or the error. */
int main(int argc, char **argv) {
  char *sql;
  int status = 0;

  /* The parser allocates in memory contexts and raises through them. */
  MemoryContextInit();
  /* The scanner asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  sql = read_stdin();

  PG_TRY();
  {
    List *stmts = raw_parser(sql, RAW_PARSE_DEFAULT);
    ListCell *lc;

    foreach (lc, stmts) {
      RawStmt *raw = lfirst_node(RawStmt, lc);

      printf("%s\n", nodeToString(raw->stmt));
    }
  }
  PG_CATCH();
  {
    ErrorData *err;

    MemoryContextSwitchTo(TopMemoryContext);
    err = CopyErrorData();
    FlushErrorState();
    printf("ERROR: %s", err->message);
    if (err->cursorpos > 0)
      printf(" at character %d", err->cursorpos);
    printf("\n");
    status = 1;
  }
  PG_END_TRY();

  return status;
}
