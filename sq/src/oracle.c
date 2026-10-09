/*
 * oracle.c -- parse SQL with PostgreSQL's own parser, and print the
 * parse tree.
 *
 *   oracle < file.sql              one tree a statement, or the error
 *   oracle --split DIR < file.sql  the same, and each statement's text
 *                                  to DIR/NNNNN.sql, its tree to
 *                                  DIR/NNNNN.out
 *   oracle --tokens < file.sql     one token a line from the scanner
 *                                  alone: its number, the byte offset
 *                                  it begins at, and its value
 *
 * This is what sq's own parser is checked against (PLAN.md step 0):
 * text in, the raw parse tree out, as nodeToString() writes it. No
 * catalog is read: the raw parser knows the grammar and nothing about
 * what a name names.
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/parser.h"
#include "gramparse.h"
#include "common/keywords.h"
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

/* Open DIR/NNNNN.EXT for writing, or end the process saying why not. */
static FILE *open_split(const char *dir, int n, const char *ext) {
  char path[MAXPGPATH];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%05d.%s", dir, n, ext);
  f = fopen(path, "w");
  if (f == NULL) {
    fprintf(stderr, "oracle: %s: %s\n", path, strerror(errno));
    exit(2);
  }
  return f;
}

/* Write one statement's text and its tree as a pair of files in dir. */
static void write_split(const char *dir, int n, const char *sql, RawStmt *raw,
                        const char *tree) {
  int len =
      raw->stmt_len > 0 ? raw->stmt_len : (int)strlen(sql) - raw->stmt_location;
  FILE *f;

  f = open_split(dir, n, "sql");
  fwrite(sql + raw->stmt_location, 1, len, f);
  fputs(";\n", f);
  fclose(f);

  f = open_split(dir, n, "out");
  fprintf(f, "%s\n", tree);
  fclose(f);
}

/* Scan sql with PostgreSQL's scanner alone and print each token. */
static void print_tokens(const char *sql) {
  core_yy_extra_type extra;
  core_yyscan_t scanner;

  scanner = scanner_init(sql, &extra, &ScanKeywords, ScanKeywordTokens);
  for (;;) {
    core_YYSTYPE value;
    YYLTYPE location = 0;
    int token = core_yylex(&value, &location, scanner);

    if (token == 0)
      break;
    printf("%d %d", token, location);
    switch (token) {
    case IDENT:
    case UIDENT:
    case FCONST:
    case SCONST:
    case USCONST:
    case BCONST:
    case XCONST:
    case Op:
      printf(" %s", value.str);
      break;
    case ICONST:
    case PARAM:
      printf(" %d", value.ival);
      break;
    default:
      if (token >= ABORT_P && token <= ZONE)
        printf(" %s", value.keyword);
      break;
    }
    printf("\n");
  }
  scanner_finish(scanner);
}

/* Parse the input and print each statement's tree, or the error. */
int main(int argc, char **argv) {
  const char *split_dir = NULL;
  bool tokens = false;
  char *sql;
  int status = 0;

  if (argc == 3 && strcmp(argv[1], "--split") == 0)
    split_dir = argv[2];
  else if (argc == 2 && strcmp(argv[1], "--tokens") == 0)
    tokens = true;
  else if (argc != 1) {
    fprintf(stderr, "usage: oracle [--split DIR | --tokens] < file.sql\n");
    return 2;
  }

  /* The parser allocates in memory contexts and raises through them. */
  MemoryContextInit();
  /* The scanner asks the encoding how long a character is. */
  SetDatabaseEncoding(PG_UTF8);

  sql = read_stdin();

  PG_TRY();
  {
    List *stmts = tokens ? NIL : raw_parser(sql, RAW_PARSE_DEFAULT);
    ListCell *lc;
    int n = 0;

    if (tokens)
      print_tokens(sql);
    foreach (lc, stmts) {
      RawStmt *raw = lfirst_node(RawStmt, lc);
      char *tree = nodeToString(raw->stmt);

      printf("%s\n", tree);
      if (split_dir != NULL)
        write_split(split_dir, ++n, sql, raw, tree);
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
    if (tokens && err->detail)
      printf("DETAIL: %s\n", err->detail);
    if (tokens && err->hint)
      printf("HINT: %s\n", err->hint);
    status = 1;
  }
  PG_END_TRY();

  return status;
}
