// slt -- run sqllogictest files on a PostgreSQL server, over libpq.
//
//   slt [--engine NAME]... [--rewrite] [--times FILE] CONNINFO FILE...
//
// Each FILE runs in a database of its own, made for it through CONNINFO
// and dropped after. The format is SQLite's (README.md here), with
// "statement error" taking a pattern for the message. --engine names
// what "skipif" and "onlyif" match; --rewrite writes each query's
// results back into its file instead of comparing them; --times writes
// how long each record took to FILE, the slowest first. The exit status
// is 1 if any record failed, 2 if a file could not be run.
#include <libpq-fe.h>
#include <openssl/evp.h>

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
  std::set<std::string> engines;
  bool rewrite = false;
  std::string conninfo;
  std::string times_path; // where --times writes each record's time
};

// How long one record took, where it is, and its SQL's first line.
struct record_time_t {
  double ms;
  std::string where;
  std::string sql;
};

// What one file's run counts, and how long it and each record took.
struct tally_t {
  int run = 0;
  int failed = 0;
  int skipped = 0;
  double seconds = 0;
  std::vector<record_time_t> times;
};

// Milliseconds since START.
double ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// A file being run: its lines, the next one to read, and what it
// holds across records.
struct script_t {
  std::string path;
  std::vector<std::string> lines;
  size_t next = 0;
  size_t hash_threshold = 0;
  std::map<std::string, std::string> labels;
  std::string out; // the file as --rewrite writes it
};

// Split S at runs of spaces.
std::vector<std::string> split_words(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> words;
  std::string w;
  while (in >> w)
    words.push_back(w);
  return words;
}

// Split S at each tab.
std::vector<std::string> split_tabs(const std::string &s) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t tab; (tab = s.find('\t', start)) != std::string::npos;
       start = tab + 1)
    parts.push_back(s.substr(start, tab - start));
  parts.push_back(s.substr(start));
  return parts;
}

// The MD5 of each value followed by a newline, in hex, as sqllogictest
// hashes a result.
std::string md5_values(const std::vector<std::string> &values) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
  for (const auto &v : values) {
    EVP_DigestUpdate(ctx, v.data(), v.size());
    EVP_DigestUpdate(ctx, "\n", 1);
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_DigestFinal_ex(ctx, digest, &len);
  EVP_MD_CTX_free(ctx);
  std::string hex;
  char buf[3];
  for (unsigned int i = 0; i < len; i++) {
    snprintf(buf, sizeof buf, "%02x", digest[i]);
    hex += buf;
  }
  return hex;
}

// A value as sqllogictest prints it for a column of type TYPE: I an
// integer, truncated as SQLite's driver does; R three decimals; T the
// text, "(empty)" if empty and a control character as '@'.
std::string format_value(const char *v, bool is_null, char type) {
  if (is_null)
    return "NULL";
  char buf[64];
  if (type == 'I') {
    char *end;
    long long i = strtoll(v, &end, 10);
    if (*end != '\0')
      i = static_cast<long long>(strtod(v, nullptr));
    snprintf(buf, sizeof buf, "%lld", i);
    return buf;
  }
  if (type == 'R') {
    snprintf(buf, sizeof buf, "%.3f", strtod(v, nullptr));
    return buf;
  }
  std::string s = v;
  if (s.empty())
    return "(empty)";
  for (auto &c : s)
    if (static_cast<unsigned char>(c) < ' ')
      c = '@';
  return s;
}

// Whether a record under the conditions COND (its "skipif" and
// "onlyif" lines) runs on ENGINES.
bool runs_here(const std::vector<std::vector<std::string>> &cond,
               const std::set<std::string> &engines) {
  for (const auto &c : cond) {
    bool named = engines.count(c[1]) > 0;
    if (c[0] == "skipif" && named)
      return false;
    if (c[0] == "onlyif" && !named)
      return false;
  }
  return true;
}

// Read lines from S up to a blank line, or a line equal to STOP, and
// return them joined; the stopping line is consumed and copied to the
// rewrite only if it is STOP.
std::string read_block(script_t &s, const std::string &stop, bool *stopped) {
  std::string text;
  *stopped = false;
  while (s.next < s.lines.size()) {
    const std::string &l = s.lines[s.next++];
    if (l.empty())
      break;
    if (!stop.empty() && l == stop) {
      *stopped = true;
      break;
    }
    s.out += l + "\n";
    if (!text.empty())
      text += "\n";
    text += l;
  }
  return text;
}

// Print where a record of S failed, from line LINE, and why.
void report(const script_t &s, size_t line, const std::string &sql,
            const std::string &why) {
  fprintf(stderr, "%s:%zu: %s\n%s\n\n", s.path.c_str(), line, why.c_str(),
          sql.c_str());
}

// Print up to twenty of VALUES, under HEAD, to show a mismatch.
std::string show_values(const char *head,
                        const std::vector<std::string> &values) {
  std::string r = std::string(head) + ":\n";
  for (size_t i = 0; i < values.size() && i < 20; i++)
    r += "  " + values[i] + "\n";
  if (values.size() > 20)
    r += "  ... " + std::to_string(values.size()) + " values\n";
  return r;
}

// Run the "statement" record whose header is WORDS, at line LINE of S.
bool run_statement(script_t &s, PGconn *conn,
                   const std::vector<std::string> &words, size_t line,
                   bool run) {
  bool stopped;
  std::string sql = read_block(s, "", &stopped);
  s.out += "\n";
  if (!run)
    return true;
  bool want_error = words.size() > 1 && words[1] == "error";
  PGresult *res = PQexec(conn, sql.c_str());
  ExecStatusType st = PQresultStatus(res);
  bool failed = st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR;
  std::string msg = failed ? PQresultErrorMessage(res) : "";
  const char *field = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
  std::string primary = field ? field : "";
  PQclear(res);
  if (!want_error) {
    if (failed)
      report(s, line, sql, "statement failed: " + msg);
    return !failed;
  }
  if (!failed) {
    report(s, line, sql, "statement succeeded; an error was expected");
    return false;
  }
  // The pattern is the rest of the header line, after "statement error".
  const std::string &header = s.lines[line - 1];
  size_t at = header.find("error") + 5;
  std::string pattern = header.substr(at);
  pattern.erase(0, pattern.find_first_not_of(' '));
  if (pattern.empty())
    return true;
  if (!std::regex_search(primary, std::regex(pattern))) {
    report(s, line, sql,
           "error \"" + msg + "\" does not match /" + pattern + "/");
    return false;
  }
  return true;
}

// Run the "query" record whose header is WORDS, at line LINE of S, and
// compare what it returns with the results under its "----".
bool run_query(script_t &s, PGconn *conn, const std::vector<std::string> &words,
               size_t line, bool run, const options_t &opt) {
  bool has_results;
  std::string sql = read_block(s, "----", &has_results);
  std::vector<std::string> expected;
  size_t results_at = s.next + 1;
  if (has_results) {
    while (s.next < s.lines.size() && !s.lines[s.next].empty())
      expected.push_back(s.lines[s.next++]);
    if (s.next < s.lines.size())
      s.next++;
  }
  // The results, copied as they were unless this run rewrites them.
  auto keep_results = [&] {
    if (!has_results)
      return;
    s.out += "----\n";
    for (const auto &e : expected)
      s.out += e + "\n";
  };
  if (!run || words.size() < 2) {
    keep_results();
    s.out += "\n";
    if (words.size() < 2)
      report(s, line, sql, "a query with no types");
    return words.size() >= 2;
  }
  const std::string &types = words[1];
  std::string sort = words.size() > 2 ? words[2] : "nosort";
  std::string label = words.size() > 3 ? words[3] : "";

  PGresult *res = PQexec(conn, sql.c_str());
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string msg = PQresultErrorMessage(res);
    PQclear(res);
    keep_results();
    s.out += "\n";
    report(s, line, sql, "query failed: " + msg);
    return false;
  }
  int nrows = PQntuples(res), ncols = PQnfields(res);
  if (ncols != static_cast<int>(types.size())) {
    PQclear(res);
    keep_results();
    s.out += "\n";
    report(s, line, sql,
           "query returned " + std::to_string(ncols) + " columns, not " +
               std::to_string(types.size()));
    return false;
  }
  std::vector<std::vector<std::string>> rows(nrows);
  for (int r = 0; r < nrows; r++)
    for (int c = 0; c < ncols; c++)
      rows[r].push_back(format_value(PQgetvalue(res, r, c),
                                     PQgetisnull(res, r, c), types[c]));
  PQclear(res);
  if (sort == "rowsort")
    std::sort(rows.begin(), rows.end());
  std::vector<std::string> actual;
  for (const auto &row : rows)
    for (const auto &v : row)
      actual.push_back(v);
  if (sort == "valuesort")
    std::sort(actual.begin(), actual.end());
  std::string hash = md5_values(actual);
  std::string hash_line =
      std::to_string(actual.size()) + " values hashing to " + hash;

  bool ok = true;
  if (!label.empty()) {
    auto [it, fresh] = s.labels.emplace(label, hash_line);
    if (!fresh && it->second != hash_line) {
      report(s, line, sql,
             "label " + label + ": " + hash_line + ", not " + it->second);
      ok = false;
    }
  }
  if (opt.rewrite) {
    s.out += "----\n";
    if (s.hash_threshold > 0 && actual.size() > s.hash_threshold) {
      s.out += hash_line + "\n";
    } else {
      for (const auto &row : rows) {
        for (size_t c = 0; c < row.size(); c++)
          s.out += (c ? "\t" : "") + row[c];
        s.out += "\n";
      }
    }
    s.out += "\n";
    return ok;
  }
  keep_results();
  s.out += "\n";
  if (!has_results)
    return ok;

  static const std::regex hashed("^([0-9]+) values hashing to ([0-9a-f]{32})$");
  std::smatch m;
  if (expected.size() == 1 && std::regex_match(expected[0], m, hashed)) {
    if (m[0] != hash_line) {
      report(s, results_at, sql,
             "expected " + expected[0] + ", got " + hash_line);
      ok = false;
    }
    return ok;
  }
  // One value a line, as SQLite writes them, or one row a line, its
  // values separated by tabs.
  std::vector<std::string> want;
  if (expected.size() == actual.size()) {
    want = expected;
  } else {
    for (const auto &e : expected)
      for (auto &v : split_tabs(e))
        want.push_back(v);
  }
  if (sort == "valuesort" && want.size() != actual.size())
    std::sort(want.begin(), want.end());
  if (want != actual) {
    report(s, results_at, sql,
           show_values("expected", want) + show_values("got", actual));
    ok = false;
  }
  return ok;
}

// Run the records of S on CONN, counting them in T.
void run_script(script_t &s, PGconn *conn, const options_t &opt, tally_t &t) {
  std::vector<std::vector<std::string>> cond;
  while (s.next < s.lines.size()) {
    size_t line = ++s.next;
    const std::string &l = s.lines[line - 1];
    std::vector<std::string> words = split_words(l);
    if (words.empty() || words[0][0] == '#') {
      s.out += l + "\n";
      continue;
    }
    s.out += l + "\n";
    if ((words[0] == "skipif" || words[0] == "onlyif") && words.size() > 1) {
      cond.push_back(words);
      continue;
    }
    bool run = runs_here(cond, opt.engines);
    cond.clear();
    if (words[0] == "hash-threshold" && words.size() > 1) {
      s.hash_threshold = std::stoul(words[1]);
    } else if (words[0] == "halt") {
      if (run)
        break;
    } else if (words[0] == "statement" || words[0] == "query") {
      std::string sql = line < s.lines.size() ? s.lines[line] : "";
      auto start = std::chrono::steady_clock::now();
      bool ok = words[0] == "statement"
                    ? run_statement(s, conn, words, line, run)
                    : run_query(s, conn, words, line, run, opt);
      if (!run) {
        t.skipped++;
        continue;
      }
      t.times.push_back(
          {ms_since(start), s.path + ":" + std::to_string(line), sql});
      t.run++;
      if (!ok)
        t.failed++;
    } else {
      report(s, line, l, "not a record");
      t.failed++;
    }
  }
  // What a halt left unread is copied as it was.
  while (s.next < s.lines.size())
    s.out += s.lines[s.next++] + "\n";
}

// Run CMD on CONN, and say so on stderr if it fails.
bool exec_ok(PGconn *conn, const std::string &cmd) {
  PGresult *res = PQexec(conn, cmd.c_str());
  bool ok = PQresultStatus(res) == PGRES_COMMAND_OK;
  if (!ok)
    fprintf(stderr, "slt: %s: %s", cmd.c_str(), PQerrorMessage(conn));
  PQclear(res);
  return ok;
}

// Ignore a server's notices, which a test does not compare.
void no_notice(void *, const char *) {}

// Connect through CONNINFO to the database DBNAME, or to CONNINFO's own
// if DBNAME is empty; NULL, and why on stderr, if it fails.
PGconn *connect_to(const std::string &conninfo, const std::string &dbname) {
  const char *keys[] = {"dbname", dbname.empty() ? nullptr : "dbname", nullptr};
  const char *values[] = {conninfo.c_str(), dbname.c_str(), nullptr};
  PGconn *conn = PQconnectdbParams(keys, values, 1);
  if (PQstatus(conn) != CONNECTION_OK) {
    fprintf(stderr, "slt: %s", PQerrorMessage(conn));
    PQfinish(conn);
    return nullptr;
  }
  PQsetNoticeProcessor(conn, no_notice, nullptr);
  return conn;
}

// Run the file PATH in a database made for it through ADMIN; false if
// it could not be run at all.
bool run_file(const std::string &path, PGconn *admin, const options_t &opt,
              tally_t &t) {
  std::ifstream in(path);
  if (!in) {
    fprintf(stderr, "slt: cannot read %s\n", path.c_str());
    return false;
  }
  script_t s;
  s.path = path;
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r')
      l.pop_back();
    s.lines.push_back(l);
  }
  // A database of the file's own, so no table of one file meets another.
  const std::string db = "slt_test";
  if (!exec_ok(admin, "DROP DATABASE IF EXISTS " + db) ||
      !exec_ok(admin, "CREATE DATABASE " + db))
    return false;
  PGconn *conn = connect_to(opt.conninfo, db);
  if (!conn)
    return false;
  run_script(s, conn, opt, t);
  PQfinish(conn);
  if (!exec_ok(admin, "DROP DATABASE " + db))
    return false;
  if (opt.rewrite) {
    std::ofstream out(path);
    out << s.out.substr(0, s.out.find_last_not_of('\n') + 1) << "\n";
  }
  return true;
}

} // namespace

// Parse the arguments, then run each file and print what it counted.
int main(int argc, char **argv) {
  options_t opt;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--engine" && i + 1 < argc)
      opt.engines.insert(argv[++i]);
    else if (a == "--rewrite")
      opt.rewrite = true;
    else if (a == "--times" && i + 1 < argc)
      opt.times_path = argv[++i];
    else if (opt.conninfo.empty())
      opt.conninfo = a;
    else
      files.push_back(a);
  }
  if (files.empty()) {
    fprintf(stderr, "usage: slt [--engine NAME]... [--rewrite] "
                    "[--times FILE] CONNINFO FILE...\n");
    return 2;
  }
  PGconn *admin = connect_to(opt.conninfo, "");
  if (!admin)
    return 2;
  int status = 0;
  std::vector<record_time_t> times;
  for (const auto &f : files) {
    tally_t t;
    auto start = std::chrono::steady_clock::now();
    if (!run_file(f, admin, opt, t)) {
      status = 2;
      continue;
    }
    t.seconds = ms_since(start) / 1000;
    printf("%s: %d run, %d failed, %d skipped, in %.1f s\n", f.c_str(), t.run,
           t.failed, t.skipped, t.seconds);
    if (t.failed && status == 0)
      status = 1;
    times.insert(times.end(), t.times.begin(), t.times.end());
  }
  PQfinish(admin);
  // Every record's time, the slowest first.
  if (!opt.times_path.empty()) {
    std::sort(times.begin(), times.end(),
              [](const auto &a, const auto &b) { return a.ms > b.ms; });
    std::ofstream out(opt.times_path);
    char ms[32];
    for (const auto &r : times) {
      snprintf(ms, sizeof ms, "%10.1f", r.ms);
      out << ms << " ms  " << r.where << "  " << r.sql << "\n";
    }
  }
  return status;
}
