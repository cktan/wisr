/*
 * test_parse.cc -- sq_parse as a caller sees it. That each statement
 * parses to PostgreSQL's tree is test/trees.sh's to show, against the
 * oracle; here are the call's own promises (sq.h).
 */

#include "doctest.h"

#include "sq.h"

#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/* The text sq_to_string writes for a statement of a tree. */
std::string text_of(const sq_raw_stmt_t *raw) {
  std::string out;
  REQUIRE(sq_to_string(raw->stmt, out) == 0);
  return out;
}

/* A notice hook that counts into the int at arg. */
void count_notice(void *arg, const sq_notice_t *) {
  ++*static_cast<int *>(arg);
}

} // namespace

TEST_CASE("each statement comes back with where it was written") {
  // Verify the tree has one entry a statement, with its place in the text.
  // 1. Parse two statements and a stray semicolon.
  // 2. Confirm two entries, each of the right kind.
  // 3. Confirm each one's offset and length cut its own text out.
  const std::string sql = "SELECT 1;  INSERT INTO t VALUES (2) ; ;";
  sq_tree_t tree;
  REQUIRE(sq_parse(sql, nullptr, tree) == 0);
  REQUIRE(tree.stmts.size() == 2);
  CHECK(tree.stmts[0]->tag == SQ_T_RAW_STMT);
  CHECK(tree.stmts[0]->stmt->tag == SQ_T_SELECT_STMT);
  CHECK(tree.stmts[1]->stmt->tag == SQ_T_INSERT_STMT);
  CHECK(sql.substr(tree.stmts[0]->stmt_location, tree.stmts[0]->stmt_len) ==
        "SELECT 1");
  CHECK(sql.substr(tree.stmts[1]->stmt_location, tree.stmts[1]->stmt_len) ==
        "INSERT INTO t VALUES (2) ");
}

TEST_CASE("text with no statement parses to an empty tree") {
  // Verify nothing is not an error.
  // 1. Parse empty text, and text of comments and semicolons.
  // 2. Confirm success and no statements.
  sq_tree_t tree;
  CHECK(sq_parse("", nullptr, tree) == 0);
  CHECK(tree.stmts.empty());
  CHECK(sq_parse(" -- nothing\n ;; /* here */ ", nullptr, tree) == 0);
  CHECK(tree.stmts.empty());
}

TEST_CASE("a syntax error is reported, and the tree is left empty") {
  // Verify a parse is all or nothing, and says what was wrong and where.
  // 1. Parse into a tree that already holds a statement.
  // 2. Give it a good statement followed by a bad one.
  // 3. Confirm -1, PostgreSQL's message, SQLSTATE and position.
  // 4. Confirm the tree is empty: not the old statement, not the good one.
  sq_tree_t tree;
  REQUIRE(sq_parse("SELECT 0", nullptr, tree) == 0);
  CHECK(sq_parse("SELECT 1; SELECT FROM WHERE;", nullptr, tree) == -1);
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_sqlstate()) == "42601");
  CHECK(std::string(sq_errmsg()) == "syntax error at or near \"WHERE\"");
  CHECK(sq_errpos() == 23);
  CHECK(tree.stmts.empty());
}

TEST_CASE("an error raised by an action carries PostgreSQL's hint") {
  // Verify errors from the grammar's own code, not bison's, arrive whole.
  // 1. Parse a statement the grammar accepts and its action refuses.
  // 2. Confirm the message, SQLSTATE, position and hint.
  sq_tree_t tree;
  CHECK(sq_parse("CREATE ROLE r UNENCRYPTED PASSWORD 'x'", nullptr, tree) ==
        -1);
  CHECK(std::string(sq_sqlstate()) == "0A000");
  CHECK(std::string(sq_errmsg()) ==
        "UNENCRYPTED PASSWORD is no longer supported");
  CHECK(std::string(sq_errhint()) ==
        "Remove UNENCRYPTED to store the password in encrypted form instead.");
  CHECK(sq_errpos() == 15);
}

TEST_CASE("a successful parse leaves the last error alone") {
  // Verify the error state is only ever written by a failure.
  // 1. Fail a parse.
  // 2. Succeed at another.
  // 3. Confirm the first one's error is still what is read.
  sq_tree_t tree;
  REQUIRE(sq_parse("SELECT FROM WHERE", nullptr, tree) == -1);
  REQUIRE(sq_parse("SELECT 1", nullptr, tree) == 0);
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_errmsg()) == "syntax error at or near \"WHERE\"");
}

TEST_CASE("the options reach the scanner, and the grammar's own warning") {
  // Verify sq_parse passes its options on, and the notice hook is called.
  // 1. Parse 'a\nb' with standard_conforming_strings off and a hook.
  // 2. Confirm the string has a newline, and one notice was given.
  // 3. Parse CREATE GLOBAL TEMPORARY TABLE, which the grammar warns of.
  // 4. Confirm one more notice.
  int notices = 0;
  sq_options_t opt;
  opt.standard_conforming_strings = false;
  opt.notice = count_notice;
  opt.notice_arg = &notices;

  sq_tree_t tree;
  REQUIRE(sq_parse("SELECT 'a\\nb'", &opt, tree) == 0);
  CHECK(text_of(tree.stmts[0]).find(":val \"a\\\nb\"") != std::string::npos);
  CHECK(notices == 1);

  REQUIRE(sq_parse("CREATE GLOBAL TEMPORARY TABLE t (a int)", &opt, tree) == 0);
  CHECK(notices == 2);
}

TEST_CASE("a tree outlives the text it was parsed from") {
  // Verify a tree holds copies, not pointers into the caller's text.
  // 1. Parse text held in a string, then overwrite and free the string.
  // 2. Confirm the tree still prints the names that were in it.
  sq_tree_t tree;
  {
    std::string sql = "SELECT col_a FROM tab_b";
    REQUIRE(sq_parse(sql, nullptr, tree) == 0);
    sql.assign(sql.size(), 'X');
  }
  std::string out = text_of(tree.stmts[0]);
  CHECK(out.find("\"col_a\"") != std::string::npos);
  CHECK(out.find(":relname tab_b") != std::string::npos);
}

TEST_CASE("threads parse at once, each with its own error") {
  // Verify nothing is shared between parses on different threads.
  // 1. Start threads that parse in a loop: even ones a good statement,
  //    odd ones a bad one of their own.
  // 2. Confirm every good parse prints the same tree.
  // 3. Confirm every bad parse reads back its own thread's error.
  std::string want;
  {
    sq_tree_t tree;
    REQUIRE(sq_parse("SELECT a, b FROM t WHERE c = 1", nullptr, tree) == 0);
    want = text_of(tree.stmts[0]);
  }
  std::vector<int> wrong(8, 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([t, &want, &wrong] {
      for (int i = 0; i < 200; i++) {
        sq_tree_t tree;
        if (t % 2 == 0) {
          std::string out;
          if (sq_parse("SELECT a, b FROM t WHERE c = 1", nullptr, tree) != 0 ||
              sq_to_string(tree.stmts[0]->stmt, out) != 0 || out != want) {
            wrong[t]++;
          }
        } else {
          std::string bad = "SELECT 1 FROM t" + std::to_string(t) + " WHERE";
          if (sq_parse(bad, nullptr, tree) != -1 ||
              std::string(sq_errmsg()) != "syntax error at end of input" ||
              sq_errpos() != static_cast<int>(bad.size()) + 1) {
            wrong[t]++;
          }
        }
      }
    });
  }
  for (std::thread &th : threads) {
    th.join();
  }
  for (int t = 0; t < 8; t++) {
    CHECK(wrong[t] == 0);
  }
}

TEST_CASE("a parse that fails deep in a statement frees what it built") {
  // Verify nothing leaks when the grammar gives up after much work.
  // 1. Parse a long, deeply nested statement with an error at its end.
  // 2. Confirm it fails; the leak checker the tests are linked with sees
  //    the rest.
  std::string sql = "SELECT ";
  for (int i = 0; i < 500; i++) {
    sql += "f(a" + std::to_string(i) + ", 'text', (1 + 2) * 3), ";
  }
  sql += "FROM";
  sq_tree_t tree;
  CHECK(sq_parse(sql, nullptr, tree) == -1);
  CHECK(tree.stmts.empty());
}

TEST_CASE("a type name is handed back, owned by its tree") {
  // Verify sq_parse_type_name's own promises.
  // 1. Parse a type name into a tree that holds a statement.
  // 2. Confirm the type comes back, printable, and the statement is gone.
  // 3. Move the tree, and confirm the type is still valid: the tree owns
  //    it.
  sq_tree_t tree;
  REQUIRE(sq_parse("SELECT 1", nullptr, tree) == 0);
  sq_type_name_t *type = nullptr;
  REQUIRE(sq_parse_type_name("varchar(20)[]", nullptr, tree, &type) == 0);
  REQUIRE(type != nullptr);
  CHECK(type->tag == SQ_T_TYPE_NAME);
  CHECK(tree.stmts.empty());

  sq_tree_t moved(std::move(tree));
  std::string out;
  REQUIRE(sq_to_string(type, out) == 0);
  CHECK(out.starts_with("{TYPENAME :names (\"pg_catalog\" \"varchar\")"));
  CHECK(type->array_bounds.size() == 1);
}

TEST_CASE("a refused type name leaves the caller's pointer alone") {
  // Verify the out parameter is set only on success.
  // 1. Point the out parameter at a marker.
  // 2. Parse text that is no type name.
  // 3. Confirm -1, the error, and that the pointer still holds the marker.
  sq_tree_t tree;
  sq_type_name_t marker;
  sq_type_name_t *type = &marker;
  CHECK(sq_parse_type_name("int4 extra", nullptr, tree, &type) == -1);
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_errmsg()) == "syntax error at or near \"extra\"");
  CHECK(sq_errpos() == 6);
  CHECK(type == &marker);
}
