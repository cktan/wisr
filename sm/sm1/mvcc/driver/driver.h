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
  PAIR_RR_RR, /* T1 REPEATABLE READ,  T2 REPEATABLE READ */
  PAIR_RR_S,  /* T1 REPEATABLE READ,  T2 SERIALIZABLE    */
  PAIR_S_RR,  /* T1 SERIALIZABLE,     T2 REPEATABLE READ */
  PAIR_S_S,   /* T1 SERIALIZABLE,     T2 SERIALIZABLE    */
  /* "pairings levels" adds READ COMMITTED on either side. */
  PAIR_RC_RC, /* T1 READ COMMITTED,   T2 READ COMMITTED  */
  PAIR_RC_RR, /* T1 READ COMMITTED,   T2 REPEATABLE READ */
  PAIR_RC_S,  /* T1 READ COMMITTED,   T2 SERIALIZABLE    */
  PAIR_RR_RC, /* T1 REPEATABLE READ,  T2 READ COMMITTED  */
  PAIR_S_RC   /* T1 SERIALIZABLE,     T2 READ COMMITTED  */
};

/* The level, "rc", "rr" or "ser", pairing p gives T1 (side 0) or T2. */
std::string pairing_level(pairing_t p, int side);

/* The banner name of a pairing, as the transcript prints it. */
const char *pairing_label(pairing_t p);

/*
 * The pairing group a "when" filter names: rr, rr+s, s+s, all,
 * or t1= or t2= with levels joined by "+", as t2=rr+ser.
 */
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
  std::string level;      /* "", "rr", or "ser": "" takes the pairing's */
  bool read_only = false; /* "ro" after the level: begun read-only */
};

/*
 * A named step of a permutation script (DRIVER.md §6): one
 * session runs its operations in order, as PostgreSQL's
 * isolation tester runs one step's SQL.
 */
struct named_step_t {
  std::string session;
  std::string name;
  std::vector<item_t> ops;
  int lineno = 0;
};

/*
 * One permutation: the steps in the order they run, and each
 * session's outcome as PostgreSQL's expected output gives it,
 * with sm1's where it differs on purpose.
 */
struct permutation_t {
  std::vector<std::string> steps;
  std::vector<std::pair<std::string, std::string>>
      expect;                                           /* session, outcome */
  std::vector<std::pair<std::string, std::string>> sm1; /* the same, sm1's */
  int lineno = 0;
};

/* A parsed script. */
struct script_t {
  std::string path;
  std::string pairings = "all";
  std::vector<session_decl_t> sessions;
  std::vector<std::pair<std::string, std::string>> seeds; /* row, value */
  std::vector<item_t> items;
  std::vector<named_step_t> steps;  /* a permutation script's */
  std::vector<permutation_t> perms; /* empty for a schedule script */
};

/*
 * Parse the script at path. Returns false and fills *err on a
 * malformed script: a driver that guessed at a typo would make
 * the transcript a lie.
 */
bool script_parse(const std::string &path, script_t *out, std::string *err);

/*
 * Run script once per selected pairing, writing the transcript
 * to out; with chained, against chained stores through the chain_
 * calls (DRIVER.md §7). Returns the number of failed checks, or -1
 * if the run could not finish.
 */
int script_run(const script_t &script, int block_ms, bool chained,
               std::string *out);

#endif /* WISR_DRIVER_H */
