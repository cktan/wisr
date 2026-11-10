/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

// duckrun -- run DuckDB's sqllogictest files on PostgreSQL 18 and on
// digres side by side, over libpq, and report where digres differs
// (README.md here, ../../DUCKPLAN.md).
//
//   duckrun [--tree] [--isolation LEVEL] [--known FILE] --root DIR
//           --report DIR --tmp DIR PG_CONNINFO DG_CONNINFO FILE...
//
// Each FILE runs in a new database on each server, dropped after. Every
// statement runs on PostgreSQL 18, then on digres, and the two outcomes
// are compared: PostgreSQL 18 is the reference, and DuckDB's own answers
// are not read. A file stops at its first finding. What each file did is
// appended to REPORT/files.tsv, its finding to REPORT/findings (or to
// REPORT/known when known.txt lists it), and each statement that failed
// the same way on both to REPORT/dialect.raw. The exit status is 0 when
// every file ran, 3 when digres or PostgreSQL 18 dropped a connection or
// stopped answering (duck.sh then restarts both, and runs the files
// files.tsv does not list yet), 2 when the run could not start.
#include <libpq-fe.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// What every file is run with.
struct options_t {
  bool tree = false; // --tree: digres indexes are btree
  std::string isolation = "repeatable read";
  std::string root;   // the corpus: report paths below it
  std::string report; // where the report files are appended
  std::string tmp;    // the base of {TEST_DIR} for each server
  std::string pg_conninfo;
  std::string dg_conninfo;
  std::map<std::string, std::string> known; // FILE:LINE -> reason
};

// One line of a file that starts a record, with the SQL under it.
struct record_t {
  size_t line = 0;
  std::vector<std::string> words;              // the directive's words
  std::string sql;                             // a statement's or query's
  std::vector<std::vector<std::string>> conds; // onlyif and skipif before it
};

// A record, or a loop over records: loop and foreach both become a list
// of values that VAR takes in turn.
struct node_t {
  record_t rec;
  bool is_loop = false;
  std::string var;
  std::vector<std::string> values;
  std::vector<node_t> body;
};

// A file as parsed: its records, or why it is skipped whole.
struct parsed_t {
  std::vector<node_t> nodes;
  std::string skip_reason;          // empty when the file runs
  std::vector<size_t> record_lines; // the line of each statement and query
};

// One result of a statement: its command tag, columns and rows. A NULL
// is held as a string no server returns.
struct result_t {
  std::string tag;
  int ncols = 0;
  std::vector<std::vector<std::string>> rows;
};

// What one server did with one statement.
struct outcome_t {
  bool dropped = false; // the connection is gone: the server crashed
  bool stuck = false;   // no answer in the time allowed, even to a cancel
  std::string sqlstate; // empty when it succeeded
  std::string message;
  std::vector<result_t> results;
};

// One session of a file: a connection to each server.
struct session_t {
  PGconn *pg = nullptr;
  PGconn *dg = nullptr;
};

// The state of one file's run.
struct file_run_t {
  std::string rel; // the file's path below the corpus
  std::map<std::string, session_t> sessions;
  int run = 0;         // statements and queries run
  int both_failed = 0; // of those, the ones that failed alike on both
  size_t stop_line = 0;
  bool stopped = false;
  bool halted = false;
  bool broken = false; // a server dropped or stuck: restart both
  std::string kind;    // the finding's kind, empty if none
  std::string status;  // clean, finding, known, crash, error
  std::string detail;  // why it stopped, for files.tsv
};

const char *null_value = "\x01NULL";
const int hard_limit_ms = 60000; // statement_timeout is 10 s: well past it

// The words of S, split at white space.
std::vector<std::string> split_words(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> words;
  for (std::string w; in >> w;)
    words.push_back(w);
  return words;
}

// True if the line is empty or only white space.
bool is_blank(const std::string &l) {
  return l.find_first_not_of(" \t") == std::string::npos;
}

// S with every FROM replaced by TO.
std::string replace_all(std::string s, const std::string &from,
                        const std::string &to) {
  for (size_t at = 0; (at = s.find(from, at)) != std::string::npos;
       at += to.size())
    s.replace(at, from.size(), to);
  return s;
}

// The types a foreach group such as <integral> names, as DuckDB's runner
// expands them; any other word is itself.
std::vector<std::string> expand_group(const std::string &w) {
  static const std::vector<std::string> sig = {"tinyint", "smallint", "integer",
                                               "bigint", "hugeint"};
  static const std::vector<std::string> uns = {
      "utinyint", "usmallint", "uinteger", "ubigint", "uhugeint"};
  std::vector<std::string> out;
  if (w == "<signed>" || w == "<integral>" || w == "<numeric>")
    out.insert(out.end(), sig.begin(), sig.end());
  if (w == "<unsigned>" || w == "<integral>" || w == "<numeric>")
    out.insert(out.end(), uns.begin(), uns.end());
  if (w == "<numeric>") {
    out.push_back("float");
    out.push_back("double");
  }
  if (out.empty())
    out.push_back(w);
  return out;
}

// The require words that concern DuckDB's build alone, so a file that
// needs one runs here anyway.
bool require_ignored(const std::string &w) {
  static const std::set<std::string> ok = {
      "skip_reload",    "no_alternative_verify",
      "noforcestorage", "no_force_storage",
      "vector_size",    "exact_vector_size",
      "64bit"};
  return ok.count(w) > 0;
}

// Read the lines of one record from line I: up to a blank line, or to
// "----" when STOP_AT_DASHES; then skip what "----" heads, DuckDB's
// answer, up to the next blank line. I ends on the blank line.
std::string read_body(const std::vector<std::string> &lines, size_t &i,
                      bool stop_at_dashes) {
  std::string body;
  while (i < lines.size() && !is_blank(lines[i])) {
    if (stop_at_dashes && lines[i] == "----") {
      while (i < lines.size() && !is_blank(lines[i]))
        i++;
      break;
    }
    if (!body.empty())
      body += "\n";
    body += lines[i++];
  }
  return body;
}

// Parse the records from line I up to an endloop (when IN_LOOP) or the
// end; set P.skip_reason when the file cannot run here.
std::vector<node_t> parse_nodes(const std::vector<std::string> &lines,
                                size_t &i, bool in_loop, bool &skipping,
                                parsed_t &p) {
  std::vector<node_t> nodes;
  std::vector<std::vector<std::string>> conds;
  while (i < lines.size() && p.skip_reason.empty()) {
    const std::string &l = lines[i];
    std::vector<std::string> w = split_words(l);
    if (w.empty() || w[0][0] == '#') {
      i++;
      continue;
    }
    size_t line = i + 1;
    const std::string &d = w[0];
    // Between "mode skip" and "mode unskip", only the unskip counts.
    if (d == "mode") {
      i++;
      if (w.size() > 1 && w[1] == "skip")
        skipping = true;
      else if (w.size() > 1 && w[1] == "unskip")
        skipping = false;
      continue;
    }
    if (skipping) {
      i++;
      read_body(lines, i, false);
      continue;
    }
    if (d == "statement" || d == "query") {
      i++;
      node_t n;
      n.rec.line = line;
      n.rec.words = w;
      n.rec.conds = conds;
      conds.clear();
      n.rec.sql = read_body(lines, i, true);
      p.record_lines.push_back(line);
      nodes.push_back(n);
    } else if (d == "onlyif" || d == "skipif") {
      i++;
      conds.push_back(w);
    } else if (d == "loop" || d == "foreach") {
      i++;
      node_t n;
      n.is_loop = true;
      n.rec.line = line;
      n.rec.conds = conds;
      conds.clear();
      if (w.size() < 3 || (d == "loop" && w.size() < 4)) {
        p.skip_reason = "bad " + d + " at line " + std::to_string(line);
        break;
      }
      n.var = w[1];
      if (d == "loop") {
        for (long v = std::stol(w[2]); v < std::stol(w[3]); v++)
          n.values.push_back(std::to_string(v));
      } else {
        for (size_t k = 2; k < w.size(); k++)
          for (const auto &v : expand_group(w[k]))
            n.values.push_back(v);
      }
      n.body = parse_nodes(lines, i, true, skipping, p);
      nodes.push_back(n);
    } else if (d == "endloop") {
      i++;
      if (in_loop)
        return nodes;
      p.skip_reason = "endloop with no loop at line " + std::to_string(line);
    } else if (d == "require") {
      i++;
      read_body(lines, i, false);
      if (w.size() < 2 || !require_ignored(w[1]))
        p.skip_reason = "require " + (w.size() > 1 ? w[1] : "");
    } else if (d == "load" || d == "set" || d == "reset" ||
               d == "hash-threshold") {
      // A DuckDB database file, or one of its runner's options.
      i++;
      read_body(lines, i, false);
    } else if (d == "halt") {
      i++;
      node_t n;
      n.rec.line = line;
      n.rec.words = w;
      nodes.push_back(n);
    } else if (d == "restart" || d == "concurrentloop" ||
               d == "concurrentforeach" || d == "unzip") {
      p.skip_reason = d;
    } else {
      p.skip_reason = "unknown " + d;
    }
  }
  if (in_loop && p.skip_reason.empty())
    p.skip_reason = "loop with no endloop";
  return nodes;
}

// Parse the file PATH; a file that cannot be read is skipped.
parsed_t parse_file(const std::string &path) {
  parsed_t p;
  std::ifstream in(path);
  if (!in) {
    p.skip_reason = "cannot read";
    return p;
  }
  std::vector<std::string> lines;
  std::string all;
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r')
      l.pop_back();
    lines.push_back(l);
    all += l + "\n";
  }
  // Their data is DuckDB's, and not fetched.
  if (all.find("{DATA_DIR}") != std::string::npos)
    p.skip_reason = "{DATA_DIR}";
  else if (all.find("{UNITTEST_OBJECT_FILES}") != std::string::npos)
    p.skip_reason = "{UNITTEST_OBJECT_FILES}";
  if (!p.skip_reason.empty())
    return p;
  size_t i = 0;
  bool skipping = false;
  p.nodes = parse_nodes(lines, i, false, skipping, p);
  return p;
}

// Evaluate one onlyif or skipif, VAR=VALUE, VAR<VALUE or VAR<>VALUE, on
// the loop variables VARS: numbers compare as numbers. A condition with
// no operator names an engine, and neither server is DuckDB.
bool cond_holds(const std::vector<std::string> &c,
                const std::map<std::string, std::string> &vars) {
  std::string e = c[1];
  bool holds = false;
  size_t at;
  std::string op;
  if ((at = e.find("<>")) != std::string::npos)
    op = "<>";
  else if ((at = e.find('=')) != std::string::npos)
    op = "=";
  else if ((at = e.find('<')) != std::string::npos)
    op = "<";
  if (!op.empty()) {
    std::string var = e.substr(0, at), want = e.substr(at + op.size());
    auto v = vars.find(var);
    std::string have = v == vars.end() ? "" : v->second;
    char *end1, *end2;
    double a = strtod(have.c_str(), &end1), b = strtod(want.c_str(), &end2);
    bool numeric = !have.empty() && !want.empty() && *end1 == 0 && *end2 == 0;
    int cmp = numeric ? (a < b ? -1 : a > b) : have.compare(want);
    holds = op == "=" ? cmp == 0 : op == "<>" ? cmp != 0 : cmp < 0;
  }
  return c[0] == "onlyif" ? holds : !holds;
}

// Rewrite DuckDB's integer types and DOUBLE, which neither server has, as
// the nearest of PostgreSQL's wide enough for every value, on both
// servers alike (DUCKPLAN.md "The format"): a table of them could
// otherwise not be made, and every statement on it fails on both.
std::string rewrite_types(const std::string &sql) {
  static const std::pair<std::regex, const char *> types[] = {
      {std::regex(R"(\b(u?tinyint)\b)", std::regex::icase), "int2"},
      {std::regex(R"(\busmallint\b)", std::regex::icase), "int4"},
      {std::regex(R"(\buinteger\b)", std::regex::icase), "int8"},
      {std::regex(R"(\b(ubigint|u?hugeint)\b)", std::regex::icase),
       "numeric(36, 0)"},
      {std::regex(R"(\bdouble\b(?!\s+precision))", std::regex::icase),
       "float8"}};
  std::string out = sql;
  for (const auto &[re, to] : types)
    out = std::regex_replace(out, re, to);
  return out;
}

// Rewrite DuckDB's table function range(), which neither server has, as
// generate_series(), whose end is inclusive (DUCKPLAN.md "The format").
// Only a range( after FROM or JOIN is rewritten, the one form that is a
// table; one with no alias is named range(range), as DuckDB's column is.
// 1. Find "range(" after FROM or JOIN.
// 2. Split its arguments at the commas outside parentheses.
// 3. Write range(n) as generate_series(0, (n) - 1), and range(a, b[, s])
//    as generate_series(a, (b) - 1 [, s]), +1 for a negative step.
// 4. Add AS range(range) when no alias follows.
std::string rewrite_range(const std::string &sql) {
  static const std::regex head(R"(\b(from|join)\s+range\s*\()",
                               std::regex::icase);
  static const std::set<std::string> not_alias = {
      "where", "group",  "order",  "limit",  "union", "except",    "intersect",
      "join",  "inner",  "left",   "right",  "full",  "cross",     "natural",
      "on",    "having", "window", "offset", "using", "returning", "qualify"};
  std::string out;
  size_t last = 0;
  for (auto m = std::sregex_iterator(sql.begin(), sql.end(), head);
       m != std::sregex_iterator(); ++m) {
    size_t open = m->position() + m->length(); // just past "("
    if (open < last)
      continue;
    std::vector<std::string> args(1);
    int depth = 0;
    size_t i = open;
    for (; i < sql.size(); i++) {
      char c = sql[i];
      if (c == '(')
        depth++;
      else if (c == ')' && depth-- == 0)
        break;
      if (c == ',' && depth == 0)
        args.emplace_back();
      else
        args.back() += c;
    }
    if (i >= sql.size() || args.size() > 3 || args[0].empty())
      continue;
    std::string call;
    if (args.size() == 1) {
      call = "generate_series(0, (" + args[0] + ") - 1)";
    } else {
      std::string end = "(" + args[1] + ") - 1";
      if (args.size() == 3)
        end = "(" + args[1] + ") - (CASE WHEN (" + args[2] +
              ") < 0 THEN -1 ELSE 1 END)";
      call = "generate_series(" + args[0] + ", " + end +
             (args.size() == 3 ? ", " + args[2] : "") + ")";
    }
    // The next word, if it is an identifier and no clause's, is an alias.
    size_t j = i + 1;
    while (j < sql.size() && isspace((unsigned char)sql[j]))
      j++;
    std::string word;
    while (j < sql.size() && (isalnum((unsigned char)sql[j]) || sql[j] == '_'))
      word += (char)tolower((unsigned char)sql[j++]);
    bool aliased = !word.empty() && !not_alias.count(word);
    out += sql.substr(last, m->position() - last);
    out += m->str().substr(0, m->str().find_first_of(" \t\n")) + " " + call +
           (aliased ? "" : " AS range(range)");
    last = i + 1;
  }
  return out + sql.substr(last);
}

// Rewrite the kind of each CREATE INDEX for one server: USING art, which
// neither has, becomes btree on PostgreSQL 18 and lsm on digres, and with
// TREE every digres index is btree.
std::string rewrite_index(const std::string &sql, bool digres, bool tree) {
  static const std::regex art(R"(\busing\s+art\b)", std::regex::icase);
  std::string kind = digres && !tree ? "USING lsm" : "USING btree";
  std::string out = std::regex_replace(sql, art, kind);
  if (!(digres && tree))
    return out;
  // A CREATE INDEX with no USING, up to its column list, gains one.
  static const std::regex head(
      R"((create\s+(unique\s+)?index\b[^(;]*?\bon\s+(only\s+)?("[^"]*"|[^\s(;]+))\s*\()",
      std::regex::icase);
  std::string res;
  auto begin = std::sregex_iterator(out.begin(), out.end(), head);
  size_t last = 0;
  for (auto m = begin; m != std::sregex_iterator(); ++m) {
    res += out.substr(last, m->position() - last);
    std::string h = (*m)[1].str();
    static const std::regex using_word(R"(\busing\b)", std::regex::icase);
    if (std::regex_search(h, using_word))
      res += m->str();
    else
      res += h + " USING btree (";
    last = m->position() + m->length();
  }
  res += out.substr(last);
  return res;
}

// Ignore a server's notices, which are not compared.
void no_notice(void *, const char *) {}

// Run CMD on CONN, ignoring its result; true if it succeeded.
bool exec_ok(PGconn *conn, const std::string &cmd, std::string *err) {
  PGresult *res = PQexec(conn, cmd.c_str());
  bool ok = PQresultStatus(res) == PGRES_COMMAND_OK ||
            PQresultStatus(res) == PGRES_TUPLES_OK;
  if (!ok && err)
    *err = PQerrorMessage(conn);
  PQclear(res);
  return ok;
}

// Connect through CONNINFO to DBNAME, or to CONNINFO's own database if
// it is empty, and set what every session runs with; NULL if it fails.
PGconn *connect_to(const std::string &conninfo, const std::string &dbname,
                   const options_t &opt, std::string *err) {
  const char *keys[] = {"dbname", dbname.empty() ? nullptr : "dbname", nullptr};
  const char *values[] = {conninfo.c_str(), dbname.c_str(), nullptr};
  PGconn *conn = PQconnectdbParams(keys, values, 1);
  if (PQstatus(conn) != CONNECTION_OK) {
    if (err)
      *err = PQerrorMessage(conn);
    PQfinish(conn);
    return nullptr;
  }
  PQsetNoticeProcessor(conn, no_notice, nullptr);
  // The runner is one thread: a wait for another session's lock must end
  // in an error, not a hang (DUCKPLAN.md "The servers").
  std::string setup = "set lock_timeout = '500ms'; "
                      "set statement_timeout = '10s'; "
                      "set default_transaction_isolation = '" +
                      opt.isolation + "'";
  if (!dbname.empty() && !exec_ok(conn, setup, err)) {
    PQfinish(conn);
    return nullptr;
  }
  return conn;
}

// Wait until CONN has a whole result to read, or LIMIT_MS pass; false
// on the time limit or a lost connection.
bool wait_ready(PGconn *conn, int limit_ms) {
  auto start = std::chrono::steady_clock::now();
  while (PQisBusy(conn)) {
    int left =
        limit_ms - (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (left <= 0)
      return false;
    pollfd fd = {PQsocket(conn), POLLIN, 0};
    if (fd.fd < 0 || poll(&fd, 1, left) < 0)
      return false;
    if (!PQconsumeInput(conn))
      return false;
  }
  return true;
}

// Run SQL on CONN and gather every result, as text; a statement that
// does not answer in the time allowed is cancelled, and called stuck if
// even the cancel goes unanswered.
outcome_t run_sql(PGconn *conn, const std::string &sql) {
  outcome_t o;
  if (!PQsendQuery(conn, sql.c_str())) {
    o.dropped = PQstatus(conn) == CONNECTION_BAD;
    o.sqlstate = o.dropped ? "" : "XX000";
    o.message = PQerrorMessage(conn);
    return o;
  }
  for (;;) {
    if (!wait_ready(conn, hard_limit_ms)) {
      if (PQstatus(conn) == CONNECTION_BAD) {
        o.dropped = true;
        return o;
      }
      // Ask the server to stop the statement, and give it 10 s more.
      char errbuf[256];
      PGcancel *c = PQgetCancel(conn);
      if (c) {
        PQcancel(c, errbuf, sizeof errbuf);
        PQfreeCancel(c);
      }
      o.stuck = true;
      if (!wait_ready(conn, 10000))
        return o;
    }
    PGresult *res = PQgetResult(conn);
    if (res == nullptr)
      break;
    ExecStatusType st = PQresultStatus(res);
    if (st == PGRES_COPY_IN) {
      // No file of DuckDB's is sent from the client.
      PQputCopyEnd(conn, "duckrun sends no COPY data");
    } else if (st == PGRES_COPY_OUT) {
      char *buf;
      while (PQgetCopyData(conn, &buf, 0) > 0)
        PQfreemem(buf);
    } else if (st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR ||
               st == PGRES_BAD_RESPONSE) {
      const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
      const char *msg = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
      if (o.sqlstate.empty()) {
        o.sqlstate = state ? state : "";
        o.message = msg ? msg : PQerrorMessage(conn);
      }
      // An error with no SQLSTATE is libpq's own: the connection is gone.
      if (state == nullptr && PQstatus(conn) == CONNECTION_BAD)
        o.dropped = true;
    } else {
      result_t r;
      r.tag = PQcmdStatus(res);
      r.ncols = PQnfields(res);
      for (int i = 0; i < PQntuples(res); i++) {
        std::vector<std::string> row;
        for (int j = 0; j < r.ncols; j++)
          row.push_back(PQgetisnull(res, i, j) ? null_value
                                               : PQgetvalue(res, i, j));
        r.rows.push_back(row);
      }
      o.results.push_back(r);
    }
    PQclear(res);
  }
  if (PQstatus(conn) == CONNECTION_BAD)
    o.dropped = true;
  // A cancel that worked leaves a 57014 from a server that answers.
  if (o.stuck && !o.dropped && o.sqlstate == "57014")
    o.stuck = false;
  return o;
}

// True if a SQLSTATE is one of the two timeouts every session sets.
bool is_timeout(const std::string &s) { return s == "57014" || s == "55P03"; }

// True if SQL is an EXPLAIN: a plan is each engine's own, so only whether
// it succeeded is compared.
bool is_explain(const std::string &sql) {
  static const std::regex ex(R"(^\s*explain\b)", std::regex::icase);
  return std::regex_search(sql, ex);
}

// True if SQL has an ORDER BY, so its rows' order is to be compared.
bool has_order_by(const std::string &sql) {
  static const std::regex ob(R"(\border\s+by\b)", std::regex::icase);
  return std::regex_search(sql, ob);
}

// Compare the results of a statement that succeeded on both: "" if they
// agree, else the finding's kind.
std::string compare_results(const outcome_t &pg, const outcome_t &dg,
                            bool ordered) {
  if (pg.results.size() != dg.results.size())
    return "result-differs";
  bool order_only = false;
  for (size_t k = 0; k < pg.results.size(); k++) {
    const result_t &a = pg.results[k], &b = dg.results[k];
    if (a.tag != b.tag || a.ncols != b.ncols)
      return "result-differs";
    if (ordered && a.rows == b.rows)
      continue;
    // With no ORDER BY, or to tell a difference in order from one in the
    // rows, compare them sorted.
    auto ra = a.rows, rb = b.rows;
    std::sort(ra.begin(), ra.end());
    std::sort(rb.begin(), rb.end());
    if (ra != rb)
      return "result-differs";
    if (ordered)
      order_only = true;
  }
  return order_only ? "order-only" : "";
}

// Compare what the two servers did with one statement: "" if they agree,
// else the finding's kind, worst first as DUCKPLAN.md orders them. An
// EXPLAIN's rows are not compared.
std::string compare(const outcome_t &pg, const outcome_t &dg, bool ordered,
                    bool explain) {
  if (dg.dropped)
    return "crash";
  if (dg.stuck)
    return "hang";
  bool pe = !pg.sqlstate.empty(), de = !dg.sqlstate.empty();
  if ((pe && !de && is_timeout(pg.sqlstate)) ||
      (de && !pe && is_timeout(dg.sqlstate)))
    return "hang";
  if (pe && de)
    return pg.sqlstate == dg.sqlstate ? "" : "sqlstate-differs";
  if (de)
    return "digres-only-error";
  if (pe)
    return "pg-only-error";
  return explain ? "" : compare_results(pg, dg, ordered);
}

// One server's outcome, as the findings file shows it: its error, or
// each result's tag and its first rows.
std::string show(const outcome_t &o) {
  if (o.dropped)
    return "    connection lost\n";
  if (o.stuck)
    return "    no answer, even to a cancel\n";
  if (!o.sqlstate.empty())
    return "    ERROR " + o.sqlstate + ": " + o.message + "\n";
  std::string s;
  for (const auto &r : o.results) {
    s += "    " + r.tag + ", " + std::to_string(r.ncols) + " columns, " +
         std::to_string(r.rows.size()) + " rows\n";
    for (size_t i = 0; i < r.rows.size() && i < 10; i++) {
      s += "      ";
      for (size_t j = 0; j < r.rows[i].size(); j++)
        s += (j ? "\t" : "") +
             (r.rows[i][j] == null_value ? std::string("NULL") : r.rows[i][j]);
      s += "\n";
    }
    if (r.rows.size() > 10)
      s += "      ...\n";
  }
  return s;
}

// Append TEXT to the report file NAME.
void append(const options_t &opt, const std::string &name,
            const std::string &text) {
  std::ofstream out(opt.report + "/" + name, std::ios::app);
  out << text;
}

// The first six words of a message, which group like failures together:
// for a syntax error, its words and the token it is at.
std::string first_words(const std::string &msg) {
  std::vector<std::string> w = split_words(msg);
  std::string s;
  for (size_t i = 0; i < w.size() && i < 6; i++)
    s += (i ? " " : "") + w[i];
  return s;
}

// The session NAME of the file, both connections made at its first use;
// false, with why in ERR, if either could not be made.
bool get_session(file_run_t &fr, const std::string &name, const options_t &opt,
                 session_t *&s, std::string &err) {
  s = &fr.sessions[name];
  if (s->pg == nullptr)
    s->pg = connect_to(opt.pg_conninfo, "duck_test", opt, &err);
  if (s->pg == nullptr) {
    err = "PostgreSQL 18: " + err;
    return false;
  }
  if (s->dg == nullptr)
    s->dg = connect_to(opt.dg_conninfo, "duck_test", opt, &err);
  if (s->dg == nullptr) {
    err = "digres: " + err;
    return false;
  }
  return true;
}

// Stop the file at LINE with the finding KIND, and write it to the
// findings file, or to the known file when known.txt lists it.
void report_finding(file_run_t &fr, const options_t &opt, size_t line,
                    const std::string &kind, const std::string &session,
                    const std::string &sql, const std::string &pg_text,
                    const std::string &dg_text) {
  std::string where = fr.rel + ":" + std::to_string(line);
  auto k = opt.known.find(where);
  fr.stopped = true;
  fr.stop_line = line;
  fr.kind = kind;
  fr.status = k == opt.known.end() ? "finding" : "known";
  std::string text = kind + " " + where + " session " + session + "\n";
  if (k != opt.known.end())
    text += "  known: " + k->second + "\n";
  std::string shown = sql.size() > 2000 ? sql.substr(0, 2000) + "..." : sql;
  text += "  sql:\n    " + replace_all(shown, "\n", "\n    ") + "\n";
  text += "  pg18:\n" + pg_text + "  digres:\n" + dg_text + "\n";
  append(opt, fr.status == "finding" ? "findings" : "known", text);
}

// Run one statement or query record, with the loop variables VARS, on
// both servers, and compare what they did.
void run_record(file_run_t &fr, const record_t &rec,
                const std::map<std::string, std::string> &vars,
                const options_t &opt) {
  const auto &w = rec.words;
  // The session: the third word of a statement, or of a query when it is
  // not a sort mode.
  std::string session = "default";
  if (w.size() > 2 &&
      (w[0] == "statement" ||
       (w[2] != "rowsort" && w[2] != "nosort" && w[2] != "valuesort")))
    session = w[2];
  std::string sql = rec.sql;
  // A loop variable is written {var}, or ${var} as older files have it.
  for (const auto &[var, value] : vars) {
    sql = replace_all(sql, "${" + var + "}", value);
    sql = replace_all(sql, "{" + var + "}", value);
  }
  // Each server writes its files in a directory of its own.
  std::string sqls[2];
  for (int k = 0; k < 2; k++) {
    std::string dir = opt.tmp + (k ? "/dg" : "/pg");
    std::string s = replace_all(sql, "__TEST_DIR__", dir);
    s = replace_all(s, "{TEST_DIR}", dir);
    s = replace_all(s, "{TEMP_DIR}", dir);
    sqls[k] = rewrite_index(rewrite_range(rewrite_types(s)), k == 1, opt.tree);
  }
  session_t *s;
  std::string err;
  if (!get_session(fr, session, opt, s, err)) {
    bool dg_side = err.rfind("digres", 0) == 0;
    report_finding(fr, opt, rec.line,
                   dg_side ? "digres-only-error" : "pg-only-error", session,
                   "(connect)", dg_side ? "    connected\n" : "    " + err,
                   dg_side ? "    " + err : "    not tried\n");
    if (PQping(opt.dg_conninfo.c_str()) != PQPING_OK) {
      fr.kind = "crash";
      fr.broken = true;
    }
    return;
  }
  fr.run++;
  outcome_t pg = run_sql(s->pg, sqls[0]);
  if (pg.dropped || pg.stuck) {
    // PostgreSQL 18 is the reference: without it there is nothing to
    // compare, so the run restarts both.
    fr.status = "error";
    fr.detail = "PostgreSQL 18 lost at line " + std::to_string(rec.line);
    fr.stopped = fr.broken = true;
    fr.stop_line = rec.line;
    return;
  }
  outcome_t dg = run_sql(s->dg, sqls[1]);
  std::string kind = compare(pg, dg, has_order_by(sql), is_explain(sql));
  if (kind.empty()) {
    if (!pg.sqlstate.empty()) {
      fr.both_failed++;
      append(opt, "dialect.raw",
             pg.sqlstate + "\t" + first_words(pg.message) + "\n");
    }
    return;
  }
  report_finding(fr, opt, rec.line, kind, session, sqls[1], show(pg), show(dg));
  if (dg.dropped || dg.stuck)
    fr.broken = true;
}

// Run NODES in order, loops expanded, with the loop variables VARS,
// until the end or the file stops.
void run_nodes(file_run_t &fr, const std::vector<node_t> &nodes,
               std::map<std::string, std::string> &vars, const options_t &opt) {
  for (const auto &n : nodes) {
    if (fr.stopped || fr.halted)
      return;
    bool runs = true;
    for (const auto &c : n.rec.conds)
      runs = runs && cond_holds(c, vars);
    if (!runs)
      continue;
    if (n.is_loop) {
      for (const auto &v : n.values) {
        vars[n.var] = v;
        run_nodes(fr, n.body, vars, opt);
        if (fr.stopped || fr.halted)
          break;
      }
      vars.erase(n.var);
    } else if (n.rec.words[0] == "halt") {
      fr.halted = true;
    } else {
      run_record(fr, n.rec, vars, opt);
    }
  }
}

// Make the database duck_test anew on CONN; false, with why in ERR, if
// it could not be.
bool make_db(PGconn *conn, std::string &err) {
  return exec_ok(conn, "DROP DATABASE IF EXISTS duck_test", &err) &&
         exec_ok(conn, "CREATE DATABASE duck_test", &err);
}

// Run one file and append its line to files.tsv: path, status, detail,
// records run, records not run, records that failed alike on both. False
// if a server must be restarted before the next file.
bool run_file(const std::string &path, PGconn *&pg_admin, PGconn *&dg_admin,
              const options_t &opt) {
  file_run_t fr;
  fr.rel = path.rfind(opt.root + "/", 0) == 0 ? path.substr(opt.root.size() + 1)
                                              : path;
  parsed_t p = parse_file(path);
  int not_run = 0;
  if (!p.skip_reason.empty()) {
    fr.status = "skipped";
    fr.detail = p.skip_reason;
  } else {
    std::string err;
    if (!make_db(pg_admin, err)) {
      fr.status = "error";
      fr.detail = "PostgreSQL 18 CREATE DATABASE: " + first_words(err);
    } else if (!make_db(dg_admin, err)) {
      fr.status = "error";
      fr.detail = "digres CREATE DATABASE: " + first_words(err);
      fr.broken = PQstatus(dg_admin) == CONNECTION_BAD;
    } else {
      fr.status = "clean";
      std::map<std::string, std::string> vars;
      run_nodes(fr, p.nodes, vars, opt);
      for (auto &[name, s] : fr.sessions) {
        if (s.pg)
          PQfinish(s.pg);
        if (s.dg)
          PQfinish(s.dg);
      }
      if (fr.stopped)
        for (size_t l : p.record_lines)
          if (l > fr.stop_line)
            not_run++;
      if (!fr.kind.empty())
        fr.detail = fr.kind + " at line " + std::to_string(fr.stop_line);
      if (fr.kind == "crash")
        fr.status = "crash";
      // The databases go, so the next file starts with none of this one's.
      if (!fr.broken) {
        exec_ok(pg_admin, "DROP DATABASE IF EXISTS duck_test", nullptr);
        exec_ok(dg_admin, "DROP DATABASE IF EXISTS duck_test", nullptr);
      }
    }
  }
  if (fr.detail.empty())
    fr.detail = "-";
  append(opt, "files.tsv",
         fr.rel + "\t" + fr.status + "\t" + fr.detail + "\t" +
             std::to_string(fr.run) + "\t" + std::to_string(not_run) + "\t" +
             std::to_string(fr.both_failed) + "\n");
  printf("%s: %s %s, %d run, %d not run\n", fr.rel.c_str(), fr.status.c_str(),
         fr.detail.c_str(), fr.run, not_run);
  fflush(stdout);
  return !fr.broken && PQstatus(dg_admin) == CONNECTION_OK &&
         PQstatus(pg_admin) == CONNECTION_OK;
}

// Read known.txt: a line "FILE:LINE reason"; # starts a comment.
std::map<std::string, std::string> read_known(const std::string &path) {
  std::map<std::string, std::string> known;
  std::ifstream in(path);
  for (std::string l; std::getline(in, l);) {
    if (is_blank(l) || l[0] == '#')
      continue;
    size_t sp = l.find_first_of(" \t");
    known[l.substr(0, sp)] = sp == std::string::npos ? "" : l.substr(sp + 1);
  }
  return known;
}

} // namespace

// Parse the arguments, then run each file on both servers.
int main(int argc, char **argv) {
  options_t opt;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--tree")
      opt.tree = true;
    else if (a == "--isolation" && i + 1 < argc)
      opt.isolation = argv[++i];
    else if (a == "--known" && i + 1 < argc)
      opt.known = read_known(argv[++i]);
    else if (a == "--root" && i + 1 < argc)
      opt.root = argv[++i];
    else if (a == "--report" && i + 1 < argc)
      opt.report = argv[++i];
    else if (a == "--tmp" && i + 1 < argc)
      opt.tmp = argv[++i];
    else if (opt.pg_conninfo.empty())
      opt.pg_conninfo = a;
    else if (opt.dg_conninfo.empty())
      opt.dg_conninfo = a;
    else
      files.push_back(a);
  }
  if (files.empty() || opt.report.empty() || opt.tmp.empty()) {
    fprintf(stderr, "usage: duckrun [--tree] [--isolation LEVEL] [--known "
                    "FILE] --root DIR --report DIR --tmp DIR PG_CONNINFO "
                    "DG_CONNINFO FILE...\n");
    return 2;
  }
  std::string err;
  PGconn *pg_admin = connect_to(opt.pg_conninfo, "", opt, &err);
  PGconn *dg_admin =
      pg_admin ? connect_to(opt.dg_conninfo, "", opt, &err) : nullptr;
  if (!pg_admin || !dg_admin) {
    fprintf(stderr, "duckrun: %s", err.c_str());
    return 2;
  }
  int status = 0;
  for (const auto &f : files)
    if (!run_file(f, pg_admin, dg_admin, opt)) {
      status = 3;
      break;
    }
  PQfinish(pg_admin);
  PQfinish(dg_admin);
  return status;
}
