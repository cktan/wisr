/*
 * driver.h -- the scenario language, as the driver sees it.
 *
 * A script is a list of items in file order: comments, blank
 * lines, and steps. Directives (pairings, session, seed) are
 * consumed while parsing and describe the run rather than
 * appearing in it. DRIVER.md §2 is the language reference.
 */
#ifndef WISR_DRIVER_H
#define WISR_DRIVER_H

#include <stdint.h>

#include <string>
#include <vector>

/* The isolation level of each session in one run of a script. */
enum pairing_t {
  PAIR_RR_RR, /* T1 REPEATABLE READ,  T2 SERIALIZABLE    */
  PAIR_RR_V,  /* T1 REPEATABLE READ,  T2 SERIALIZABLE    */
  PAIR_V_RR,  /* T1 SERIALIZABLE,     T2 REPEATABLE READ */
  PAIR_V_V    /* T1 SERIALIZABLE,     T2 SERIALIZABLE    */
};

/* The banner name of a pairing, as the transcript prints it. */
const char *pairing_label(pairing_t p);

/* The pairing group a "when" filter names: rr, rr+v, v+v, all. */
bool pairing_selected(pairing_t p, const std::string &filter);

/* What an item in a script is. */
enum item_kind_t {
  ITEM_TEXT,  /* a comment or a blank line, echoed as it stands */
  ITEM_STEP,  /* an operation run by one session */
  ITEM_CHECK, /* an assertion run by the driver itself */
};

/* One line of a script. */
struct item_t {
  item_kind_t kind = ITEM_TEXT;
  std::string text;             /* the line, for ITEM_TEXT */
  std::string time;             /* "@1", printed with the step */
  std::string session;          /* the session's name, for ITEM_STEP */
  std::string verb;             /* begin, read, check, ... */
  std::vector<std::string> arg; /* the verb's arguments */
  std::string when = "all";     /* the pairings this item runs in */
  bool expect_block = false;    /* the script says this step waits */
  int lineno = 0;
};

/* A session the script declares. */
struct session_decl_t {
  std::string name;
  std::string level; /* "", "rr", or "ser": "" takes the pairing's */
};

/* A parsed script. */
struct script_t {
  std::string path;
  std::string pairings = "all";
  std::vector<session_decl_t> sessions;
  std::vector<std::pair<std::string, std::string>> seeds; /* row, value */
  std::vector<item_t> items;
};

/*
 * Parse the script at path. Returns false and fills *err on a
 * malformed script: a driver that guessed at a typo would make
 * the transcript a lie.
 */
bool script_parse(const std::string &path, script_t *out, std::string *err);

/*
 * Run script once per selected pairing, writing the transcript
 * to out. Returns the number of failed checks, or -1 if the
 * run could not finish.
 */
int script_run(const script_t &script, int block_ms, std::string *out);

#endif /* WISR_DRIVER_H */
