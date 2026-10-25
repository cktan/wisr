/*
 * scan.h -- the scanner: SQL text in, tokens out.
 *
 * It is PostgreSQL's scan.l, with its errors raised sq's way and its
 * strings made in the tree. A scanner_t is one scan of one text, and
 * releases flex's state when it goes, however the scan ends.
 */
#ifndef SQ_SRC_SCAN_H
#define SQ_SRC_SCAN_H

#include <cstdint>
#include <string>
#include <string_view>

#include "sq.h"

/* flex's entry point, which the parser's token filter calls. */
namespace sq {
union scan_value_t;
struct parse_state_t;
} // namespace sq
int sq_core_yylex(sq::scan_value_t *value, int *location, void *yyscanner);

namespace sq {

/*
 * What the scanner hands back with a token: PostgreSQL's
 * core_YYSTYPE. Which member is set depends on the token.
 */
union scan_value_t {
  int ival;            /* for integer literals */
  char *str;           /* for identifiers and other literals; the tree's */
  const char *keyword; /* the canonical spelling of a keyword */
};

/* The state of one scan: PostgreSQL's core_yy_extra_type. */
struct scan_extra_t {
  /* Where identifiers and literals are made. */
  sq_tree_t *tree = nullptr;

  /* The parse this scan is part of, if it is part of one. */
  parse_state_t *parse = nullptr;

  /* How to scan, and whom to tell of a notice. */
  sq_options_t options;

  /* The text, with the two NUL bytes flex wants after it. */
  std::string scanbuf;

  /* A literal, gathered across the rules that make it up. */
  std::string literal;

  int state_before_str_stop = 0; /* start condition before an end quote */
  int xcdepth = 0;               /* depth of nesting in slash-star comments */
  std::string dolqstart;         /* the $foo$ that opened a dollar quote */
  int save_yylloc = 0;           /* one-element stack for PUSH_YYLLOC() */

  /* The first half of a UTF-16 surrogate pair in a Unicode escape. */
  int32_t utf16_first_part = 0;

  /* For the warnings about escapes in ordinary string literals. */
  bool warn_on_first_escape = false;
  bool saw_non_ascii = false;
};

/* One scan of one text. */
class scanner_t {
public:
  /* Begin scanning sql, making strings in tree. */
  scanner_t(std::string_view sql, const sq_options_t &options, sq_tree_t &tree);

  /* Release the scan's state. */
  ~scanner_t();

  scanner_t(const scanner_t &) = delete;
  scanner_t &operator=(const scanner_t &) = delete;

  /*
   * The next token, 0 at the end of the text: its value in *value and
   * the byte offset it begins at in *location. Raises on text that is
   * no token.
   */
  int lex(scan_value_t *value, int *location);

  /* flex's handle on this scan, for the grammar to pass around. */
  void *handle() const { return m_scanner; }

  /* The state of this scan. */
  scan_extra_t &extra() { return m_extra; }

private:
  scan_extra_t m_extra;
  void *m_scanner = nullptr;
};

/*
 * The state of the scan behind flex's handle: the first field of
 * flex's reentrant state, as PostgreSQL's pg_yyget_extra reads it.
 */
inline scan_extra_t *scanner_extra(void *yyscanner) {
  return *static_cast<scan_extra_t **>(yyscanner);
}

/* The position in characters, from 1, of byte offset location. */
int scanner_errposition(int location, void *yyscanner);

/*
 * Raise a syntax error at the token being scanned: "message at or
 * near ..." or "message at end of input".
 */
[[noreturn]] void scanner_yyerror(const char *message, void *yyscanner);

/* Tell the options' notice hook, if there is one. */
void notify(const scan_extra_t &extra, sq_notice_level_t level,
            const char *sqlstate, int pos, const std::string &message,
            const char *hint);

} // namespace sq

#endif /* SQ_SRC_SCAN_H */
