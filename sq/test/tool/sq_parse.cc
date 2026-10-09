/*
 * sq_parse.cc -- parse SQL with sq, and print the parse tree.
 *
 *   sq_parse < file.sql     one tree a statement, or the error
 *
 * What it prints is what `oracle` prints for PostgreSQL's parser, and
 * the tests compare the two.
 */

#include <stdio.h>

#include <iostream>
#include <iterator>
#include <string>

#include "sq.h"

/* Parse standard input and print each statement's tree, or the error. */
int main(void) {
  std::string sql((std::istreambuf_iterator<char>(std::cin)),
                  std::istreambuf_iterator<char>());
  sq_tree_t tree;

  if (sq_parse(sql, nullptr, tree) != 0) {
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
    return 1;
  }
  for (const sq_raw_stmt_t *raw : tree.stmts) {
    std::string text;
    if (sq_to_string(raw->stmt, text) != 0) {
      printf("ERROR: %s\n", sq_errmsg());
      return 1;
    }
    printf("%s\n", text.c_str());
  }
  return 0;
}
