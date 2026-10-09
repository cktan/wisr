/*
 * sq_tokens.cc -- scan SQL with sq's scanner, and print the tokens.
 *
 *   sq_tokens < file.sql     one token a line, or the error
 *
 * A line is the token's number, the byte offset it begins at, and its
 * value if it has one. `oracle --tokens` prints the same for
 * PostgreSQL's scanner, and test/tokens.sh compares the two.
 */

#include <stdio.h>

#include <exception>
#include <iostream>
#include <iterator>
#include <string>

#include "error.h"
#include "gram.hh"
#include "scan.h"
#include "sq.h"

/* Print one token as `oracle --tokens` prints it. */
static void print_token(int token, int location, const sq::scan_value_t &v) {
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
    printf(" %s", v.str);
    break;
  case ICONST:
  case PARAM:
    printf(" %d", v.ival);
    break;
  default:
    if (token >= ABORT_P && token <= ZONE) {
      printf(" %s", v.keyword);
    }
    break;
  }
  printf("\n");
}

/* Print the thread's error as the oracle prints PostgreSQL's. */
static void print_error(void) {
  printf("ERROR: %s", sq_errmsg());
  if (sq_errpos() > 0) {
    printf(" at character %d", sq_errpos());
  }
  printf("\n");
  if (sq_errdetail()[0] != '\0') {
    printf("DETAIL: %s\n", sq_errdetail());
  }
  if (sq_errhint()[0] != '\0') {
    printf("HINT: %s\n", sq_errhint());
  }
}

/* Scan standard input and print each token, or the error. */
int main(void) {
  std::string sql((std::istreambuf_iterator<char>(std::cin)),
                  std::istreambuf_iterator<char>());
  sq_tree_t tree;
  sq_options_t options;

  try {
    sq::scanner_t scanner(sql, options, tree);
    for (;;) {
      sq::scan_value_t value;
      int location = 0;
      int token = scanner.lex(&value, &location);
      if (token == 0) {
        break;
      }
      print_token(token, location, value);
    }
  } catch (const sq::parse_error_t &) {
    print_error();
    return 1;
  } catch (const std::exception &e) {
    printf("EXCEPTION: %s\n", e.what());
    return 2;
  }
  return 0;
}
