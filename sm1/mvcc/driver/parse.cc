/*
 * parse.cc -- turning a script into items (DRIVER.md §2).
 *
 * The grammar is one line long: a line is blank, a comment, a
 * directive, or a step. Anything else is an error, because a
 * script the driver half-understood would produce a transcript
 * that looks fine and proves nothing.
 */

#include "driver.h"

#include <fstream>
#include <sstream>

namespace {

/* Split a line into whitespace-separated words. */
std::vector<std::string> words(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string w;
  while (in >> w) {
    out.push_back(w);
  }
  return out;
}

/* True if the line has nothing but whitespace. */
bool blank(const std::string &line) {
  return line.find_first_not_of(" \t\r") == std::string::npos;
}

/* Format "path:line: message" for an error. */
std::string where(const std::string &path, int lineno, const std::string &msg) {
  std::ostringstream out;
  out << path << ":" << lineno << ": " << msg;
  return out.str();
}

/* True if name is one of the script's declared sessions. */
bool declared(const script_t &s, const std::string &name) {
  for (const session_decl_t &d : s.sessions) {
    if (d.name == name) {
      return true;
    }
  }
  return false;
}

/*
 * The verbs a step may use, with the number of arguments each
 * takes. A verb the driver does not know, or one given the
 * wrong number of arguments, is a broken script: catching it
 * here keeps it out of a transcript, where it would look like
 * a result.
 */
struct verb_t {
  const char *name;
  size_t min_args;
  size_t max_args;
};

const verb_t STEP_VERBS[] = {
    {"begin", 0, 0},      {"commit", 0, 0},  {"commit?", 0, 0},
    {"abort", 0, 0},      {"read", 1, 1},    {"write", 2, 2},
    {"insert", 2, 2},     {"delete", 1, 1},  {"scan", 2, 4},
    {"open", 3, 3},       {"next", 1, 1},    {"close", 1, 1},
    {"mkstore", 0, 0},    {"lsstore", 0, 0}, {"savepoint", 0, 0},
    {"rollback", 1, 1},   {"lock", 2, 2},    {"seek", 2, 2},
    {"stmt", 0, 0},       {"lookup", 1, 1},  {"rmstore", 0, 0},
    {"readlatest", 1, 1},
};

const verb_t CHECK_VERBS[] = {
    {"rows", 0, 0},
    {"row", 2, 2},
    {"committed", 1, 8},
    {"failed", 1, 8},
    {"exactly-one-committed", 2, 8},
    {"at-most-one-committed", 2, 8},
    {"not-all-committed", 2, 8},
    {"last", 2, 2},
};

/*
 * Check a verb and its argument count against the table.
 * Returns an empty string if it is well formed.
 */
std::string check_verb(const item_t &it, const verb_t *table, size_t n,
                       const char *what) {
  for (size_t i = 0; i < n; i++) {
    if (it.verb != table[i].name) {
      continue;
    }
    if (it.arg.size() < table[i].min_args ||
        it.arg.size() > table[i].max_args) {
      return std::string(what) + " " + it.verb + " takes " +
             std::to_string(table[i].min_args) + " to " +
             std::to_string(table[i].max_args) + " arguments, given " +
             std::to_string(it.arg.size());
    }
    // scan takes its bounds, and optionally "limit <n>".
    if (it.verb == "scan" && it.arg.size() != 2 &&
        (it.arg.size() != 4 || it.arg[2] != "limit")) {
      return "scan takes <lo> <hi> or <lo> <hi> limit <n>";
    }
    return "";
  }
  return std::string("unknown ") + what + ": " + it.verb;
}

/*
 * Pull a trailing "when <filter>" off a step's arguments.
 * Returns false if the filter names no known pairing group.
 */
bool take_when(std::vector<std::string> *arg, std::string *when) {
  size_t n = arg->size();
  if (n < 2 || (*arg)[n - 2] != "when") {
    return true;
  }
  *when = (*arg)[n - 1];
  arg->resize(n - 2);
  return *when == "rr" || *when == "rr+s" || *when == "s+s" || *when == "all";
}

} // namespace

/* The banner name of a pairing. */
const char *pairing_label(pairing_t p) {
  switch (p) {
  case PAIR_RR_RR:
    return "RR/RR";
  case PAIR_RR_S:
    return "RR/S";
  case PAIR_S_RR:
    return "S/RR";
  default:
    return "S/S";
  }
}

/* True if pairing p belongs to the group filter names. */
bool pairing_selected(pairing_t p, const std::string &filter) {
  if (filter == "all") {
    return true;
  }
  if (filter == "rr") {
    return p == PAIR_RR_RR;
  }
  if (filter == "rr+s") {
    return p == PAIR_RR_S || p == PAIR_S_RR;
  }
  return p == PAIR_S_S; /* "s+s" */
}

/* Parse the script at path (DRIVER.md §2). */
bool script_parse(const std::string &path, script_t *out, std::string *err) {
  std::ifstream in(path);
  // Report a script that is not there rather than running none.
  if (!in) {
    *err = path + ": cannot open";
    return false;
  }
  out->path = path;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    lineno++;
    // Comments and blank lines are echoed where they stand.
    if (blank(line) || line[line.find_first_not_of(" \t")] == '#') {
      item_t it;
      it.kind = ITEM_TEXT;
      it.text = blank(line) ? "" : line;
      it.lineno = lineno;
      out->items.push_back(it);
      continue;
    }
    std::vector<std::string> w = words(line);
    // Directives describe the run and are not echoed.
    if (w[0] == "pairings") {
      if (w.size() != 2 ||
          !(w[1] == "all" || w[1] == "rr" || w[1] == "rr+s" || w[1] == "s+s")) {
        *err = where(path, lineno, "pairings takes all, rr, rr+s, or s+s");
        return false;
      }
      out->pairings = w[1];
      continue;
    }
    if (w[0] == "session") {
      if (w.size() < 2 || w.size() > 3) {
        *err =
            where(path, lineno, "session takes a name and an optional level");
        return false;
      }
      session_decl_t d;
      d.name = w[1];
      d.level = w.size() == 3 ? w[2] : "";
      if (!d.level.empty() && d.level != "rr" && d.level != "ser" &&
          d.level != "rc") {
        *err = where(path, lineno, "a session's level is rc, rr, or ser");
        return false;
      }
      out->sessions.push_back(d);
      continue;
    }
    if (w[0] == "seed") {
      if (w.size() != 3) {
        *err = where(path, lineno, "seed takes a row and a value");
        return false;
      }
      out->seeds.emplace_back(w[1], w[2]);
      continue;
    }
    // Everything else is a step: @<time> <session|check> <verb> ...
    if (w[0].empty() || w[0][0] != '@' || w.size() < 3) {
      *err = where(path, lineno, "expected @<time> <session> <verb> ...");
      return false;
    }
    item_t it;
    it.time = w[0];
    it.lineno = lineno;
    it.arg.assign(w.begin() + 3, w.end());
    if (w[1] == "check") {
      it.kind = ITEM_CHECK;
      it.verb = w[2];
    } else {
      it.kind = ITEM_STEP;
      it.session = w[1];
      it.verb = w[2];
      // A step must name a session the script declared.
      if (!declared(*out, it.session)) {
        *err = where(path, lineno, "no such session: " + it.session);
        return false;
      }
    }
    // A trailing "blocks" says the step must wait on a lock.
    if (!it.arg.empty() && it.arg.back() == "blocks") {
      it.expect_block = true;
      it.arg.pop_back();
    }
    // A trailing "when <group>" limits the step to some pairings.
    if (!take_when(&it.arg, &it.when)) {
      *err = where(path, lineno, "when takes rr, rr+s, s+s, or all");
      return false;
    }
    // "blocks" may also precede "when", as in "... blocks when rr".
    if (!it.arg.empty() && it.arg.back() == "blocks") {
      it.expect_block = true;
      it.arg.pop_back();
    }
    // Reject a verb the driver does not know, or one given the
    // wrong arguments, before it can reach a transcript.
    std::string bad =
        it.kind == ITEM_CHECK
            ? check_verb(it, CHECK_VERBS,
                         sizeof(CHECK_VERBS) / sizeof(CHECK_VERBS[0]), "check")
            : check_verb(it, STEP_VERBS,
                         sizeof(STEP_VERBS) / sizeof(STEP_VERBS[0]), "verb");
    if (!bad.empty()) {
      *err = where(path, lineno, bad);
      return false;
    }
    out->items.push_back(it);
  }
  // A script with no session can run nothing.
  if (out->sessions.empty()) {
    *err = path + ": no session declared";
    return false;
  }
  return true;
}
