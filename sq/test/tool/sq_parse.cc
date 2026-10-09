/*
 * sq_parse.cc -- parse SQL with sq, and print the parse tree.
 *
 *   sq_parse < file.sql          one tree a statement, or the error
 *   sq_parse --type-name < file  the text parsed as one type name
 *   sq_parse --locations < file.sql
 *                                each statement whole, with where it
 *                                was written, and every location shown
 *
 * What it prints is what test/run.sh and test/corpus.py compare with
 * PostgreSQL's parser's answer for the same text.
 */

#include <stdio.h>
#include <string.h>

#include <iostream>
#include <iterator>
#include <string>

#include "sq.h"

/* Print one tree, in PostgreSQL's nodeToString() text. */
static int print_tree(const sq_node_t *node, bool locations = false) {
  std::string text;
  int rc = locations ? sq_to_string_with_locations(node, text)
                     : sq_to_string(node, text);
  if (rc != 0) {
    printf("ERROR: %s\n", sq_errmsg());
    return 1;
  }
  printf("%s\n", text.c_str());
  return 0;
}

/* Parse standard input and print each statement's tree, or the error. */
int main(int argc, char **argv) {
  std::string sql((std::istreambuf_iterator<char>(std::cin)),
                  std::istreambuf_iterator<char>());
  bool type_name = argc == 2 && strcmp(argv[1], "--type-name") == 0;
  bool locations = argc == 2 && strcmp(argv[1], "--locations") == 0;
  sq_tree_t tree;
  sq_type_name_t *type = nullptr;
  int rc = type_name ? sq_parse_type_name(sql, nullptr, tree, &type)
                     : sq_parse(sql, nullptr, tree);

  if (rc != 0) {
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
  if (type_name) {
    return print_tree(type);
  }
  for (const sq_raw_stmt_t *raw : tree.stmts) {
    // With locations, the statement's own place in the text is shown too.
    if (print_tree(locations ? raw : raw->stmt, locations) != 0) {
      return 1;
    }
  }
  return 0;
}
