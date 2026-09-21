/*
 * parse.cc -- turning a script into items (PLAN.md §3).
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
  return *when == "rr" || *when == "rr+v" || *when == "v+v" || *when == "all";
}

} // namespace

/* The banner name of a pairing. */
const char *pairing_label(pairing_t p) {
  switch (p) {
  case PAIR_RR_RR:
    return "RR/RR";
  case PAIR_RR_V:
    return "RR/V";
  case PAIR_V_RR:
    return "V/RR";
  default:
    return "V/V";
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
  if (filter == "rr+v") {
    return p == PAIR_RR_V || p == PAIR_V_RR;
  }
  return p == PAIR_V_V; /* "v+v" */
}

/* Parse the script at path (PLAN.md §3). */
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
          !(w[1] == "all" || w[1] == "rr" || w[1] == "rr+v" || w[1] == "v+v")) {
        *err = where(path, lineno, "pairings takes all, rr, rr+v, or v+v");
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
      if (!d.level.empty() && d.level != "rr" && d.level != "ser") {
        *err = where(path, lineno, "a session's level is rr or ser");
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
      *err = where(path, lineno, "when takes rr, rr+v, v+v, or all");
      return false;
    }
    // "blocks" may also precede "when", as in "... blocks when rr".
    if (!it.arg.empty() && it.arg.back() == "blocks") {
      it.expect_block = true;
      it.arg.pop_back();
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
