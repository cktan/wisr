/*
 * parser.cc -- sq_parse and sq_parse_type_name: the scanner, the
 * filter that looks one token ahead, and the grammar, run over one
 * text.
 *
 * This is PostgreSQL 18.6's src/backend/parser/parser.c. The filter,
 * sq_base_yylex, and the Unicode de-escaping below it are PostgreSQL's
 * line for line; the two calls stand where raw_parser() does, one for
 * each of its modes that digres uses.
 */

#include <ctype.h>
#include <string.h>

#include <cassert>
#include <exception>
#include <new>
#include <string>

#include "error.h"
#include "gram.hh"
#include "gramparse.h"
#include "scansup.h"
#include "sq.h"
#include "tree.h"
#include "utf8.h"

namespace sq {

/* The parse that the scan behind yyscanner is part of. */
parse_state_t *parse_state(core_yyscan_t yyscanner) {
  return scanner_extra(yyscanner)->parse;
}

/* Take size bytes for bison from the parse, which frees them at its end. */
void *parse_alloc(core_yyscan_t yyscanner, size_t size) {
  parse_state_t *state = parse_state(yyscanner);
  state->scratch.push_back(std::make_unique<char[]>(size));
  return state->scratch.back().get();
}

} // namespace sq

using namespace sq;

/* The value of a hexadecimal digit, which c is known to be. */
static unsigned int hexval(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 0xA;
  return c - 'A' + 0xA;
}

/* Is 'escape' acceptable as Unicode escape character (UESCAPE syntax) ? */
static bool check_uescapechar(unsigned char escape) {
  if (isxdigit(escape) || escape == '+' || escape == '\'' || escape == '"' ||
      escape == ' ' || escape == '\t' || escape == '\n' || escape == '\r' ||
      escape == '\v' || escape == '\f')
    return false;
  else
    return true;
}

/*
 * Process Unicode escapes in "str", producing a string in the tree
 *
 * escape: the escape character to use
 * position: start position of U&'' or U&"" string token
 * yyscanner: context information needed for error reports
 */
static char *str_udeescape(const char *str, char escape, int position,
                           core_yyscan_t yyscanner) {
  const char *in;
  std::string out;
  uint32_t pair_first = 0;

  in = str;
  while (*in) {
    if (in[0] == escape) {
      /*
       * Any errors reported while processing this escape sequence will
       * have an error cursor pointing at the escape.
       */
      int errpos = scanner_errposition(in - str + position + 3,
                                       yyscanner); /* 3 for U&" */
      uint32_t unicode;
      int len;

      if (in[1] == escape) {
        if (pair_first)
          goto invalid_pair;
        out += escape;
        in += 2;
        continue;
      } else if (isxdigit((unsigned char)in[1]) &&
                 isxdigit((unsigned char)in[2]) &&
                 isxdigit((unsigned char)in[3]) &&
                 isxdigit((unsigned char)in[4])) {
        unicode = (hexval(in[1]) << 12) + (hexval(in[2]) << 8) +
                  (hexval(in[3]) << 4) + hexval(in[4]);
        len = 5;
      } else if (in[1] == '+' && isxdigit((unsigned char)in[2]) &&
                 isxdigit((unsigned char)in[3]) &&
                 isxdigit((unsigned char)in[4]) &&
                 isxdigit((unsigned char)in[5]) &&
                 isxdigit((unsigned char)in[6]) &&
                 isxdigit((unsigned char)in[7])) {
        unicode = (hexval(in[2]) << 20) + (hexval(in[3]) << 16) +
                  (hexval(in[4]) << 12) + (hexval(in[5]) << 8) +
                  (hexval(in[6]) << 4) + hexval(in[7]);
        len = 8;
      } else
        raise({.sqlstate = sqlstate_syntax_error,
               .pos = errpos,
               .hint = "Unicode escapes must be \\XXXX or \\+XXXXXX."},
              "invalid Unicode escape");

      if (!is_valid_unicode_codepoint(unicode))
        raise({.sqlstate = sqlstate_syntax_error, .pos = errpos},
              "invalid Unicode escape value");
      if (pair_first) {
        if (is_utf16_surrogate_second(unicode)) {
          unicode = surrogate_pair_to_codepoint(pair_first, unicode);
          pair_first = 0;
        } else
          goto invalid_pair;
      } else if (is_utf16_surrogate_second(unicode))
        goto invalid_pair;

      if (is_utf16_surrogate_first(unicode))
        pair_first = unicode;
      else {
        unsigned char buf[utf8_max_len + 1];

        out.append(reinterpret_cast<char *>(buf),
                   unicode_to_utf8(unicode, buf));
      }
      in += len;
    } else {
      if (pair_first)
        goto invalid_pair;

      out += *in++;
    }
  }

  /* unfinished surrogate pair? */
  if (pair_first)
    goto invalid_pair;

  return tree_strdup(out);

invalid_pair:
  raise({.sqlstate = sqlstate_syntax_error,
         .pos = scanner_errposition(in - str + position + 3, /* 3 for U&" */
                                    yyscanner)},
        "invalid Unicode surrogate pair");
}

/*
 * Intermediate filter between parser and core lexer (core_yylex in scan.l).
 *
 * This filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.  We reduce these cases to one-token
 * lookahead by replacing tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do that without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 *
 * We also use this filter to convert UIDENT and USCONST sequences into
 * plain IDENT and SCONST tokens.  While that could be handled by additional
 * productions in the main grammar, it's more efficient to do it like this.
 *
 * The filter also provides a convenient place to translate between
 * the core_YYSTYPE and YYSTYPE representations (which are really the
 * same thing anyway, but notationally they're different).
 */
int sq_base_yylex(SQ_BASE_YYSTYPE *lvalp, int *llocp, core_yyscan_t yyscanner) {
  parse_state_t *yyextra = parse_state(yyscanner);
  char *scanbuf = scanner_extra(yyscanner)->scanbuf.data();
  int cur_token;
  int next_token;
  int cur_token_length;
  int cur_yylloc;

  /* Get next token --- we might already have it */
  if (yyextra->have_lookahead) {
    cur_token = yyextra->lookahead_token;
    lvalp->core_yystype = yyextra->lookahead_yylval;
    *llocp = yyextra->lookahead_yylloc;
    if (yyextra->lookahead_end)
      *(yyextra->lookahead_end) = yyextra->lookahead_hold_char;
    yyextra->have_lookahead = false;
  } else
    cur_token = sq_core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

  /*
   * If this token isn't one that requires lookahead, just return it.  If it
   * does, determine the token length.  (We could get that via strlen(), but
   * since we have such a small set of possibilities, hardwiring seems
   * feasible and more efficient --- at least for the fixed-length cases.)
   */
  switch (cur_token) {
  case FORMAT:
    cur_token_length = 6;
    break;
  case NOT:
    cur_token_length = 3;
    break;
  case NULLS_P:
    cur_token_length = 5;
    break;
  case WITH:
    cur_token_length = 4;
    break;
  case UIDENT:
  case USCONST:
    cur_token_length = strlen(scanbuf + *llocp);
    break;
  case WITHOUT:
    cur_token_length = 7;
    break;
  default:
    return cur_token;
  }

  /*
   * Identify end+1 of current token.  core_yylex() has temporarily stored a
   * '\0' here, and will undo that when we call it again.  We need to redo
   * it to fully revert the lookahead call for error reporting purposes.
   */
  yyextra->lookahead_end = scanbuf + *llocp + cur_token_length;
  assert(*(yyextra->lookahead_end) == '\0');

  /*
   * Save and restore *llocp around the call.  It might look like we could
   * avoid this by just passing &lookahead_yylloc to core_yylex(), but that
   * does not work because flex actually holds onto the last-passed pointer
   * internally, and will use that for error reporting.  We need any error
   * reports to point to the current token, not the next one.
   */
  cur_yylloc = *llocp;

  /* Get next token, saving outputs into lookahead variables */
  next_token = sq_core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);
  yyextra->lookahead_token = next_token;
  yyextra->lookahead_yylloc = *llocp;

  *llocp = cur_yylloc;

  /* Now revert the un-truncation of the current token */
  yyextra->lookahead_hold_char = *(yyextra->lookahead_end);
  *(yyextra->lookahead_end) = '\0';

  yyextra->have_lookahead = true;

  /* Replace cur_token if needed, based on lookahead */
  switch (cur_token) {
  case FORMAT:
    /* Replace FORMAT by FORMAT_LA if it's followed by JSON */
    switch (next_token) {
    case JSON:
      cur_token = FORMAT_LA;
      break;
    }
    break;

  case NOT:
    /* Replace NOT by NOT_LA if it's followed by BETWEEN, IN, etc */
    switch (next_token) {
    case BETWEEN:
    case IN_P:
    case LIKE:
    case ILIKE:
    case SIMILAR:
      cur_token = NOT_LA;
      break;
    }
    break;

  case NULLS_P:
    /* Replace NULLS_P by NULLS_LA if it's followed by FIRST or LAST */
    switch (next_token) {
    case FIRST_P:
    case LAST_P:
      cur_token = NULLS_LA;
      break;
    }
    break;

  case WITH:
    /* Replace WITH by WITH_LA if it's followed by TIME or ORDINALITY */
    switch (next_token) {
    case TIME:
    case ORDINALITY:
      cur_token = WITH_LA;
      break;
    }
    break;

  case WITHOUT:
    /* Replace WITHOUT by WITHOUT_LA if it's followed by TIME */
    switch (next_token) {
    case TIME:
      cur_token = WITHOUT_LA;
      break;
    }
    break;

  case UIDENT:
  case USCONST:
    /* Look ahead for UESCAPE */
    if (next_token == UESCAPE) {
      /* Yup, so get third token, which had better be SCONST */
      const char *escstr;

      /* Again save and restore *llocp */
      cur_yylloc = *llocp;

      /* Un-truncate current token so errors point to third token */
      *(yyextra->lookahead_end) = yyextra->lookahead_hold_char;

      /* Get third token */
      next_token =
          sq_core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);

      /* If we throw error here, it will point to third token */
      if (next_token != SCONST)
        scanner_yyerror("UESCAPE must be followed by a simple string literal",
                        yyscanner);

      escstr = yyextra->lookahead_yylval.str;
      if (strlen(escstr) != 1 || !check_uescapechar(escstr[0]))
        scanner_yyerror("invalid Unicode escape character", yyscanner);

      /* Now restore *llocp; errors will point to first token */
      *llocp = cur_yylloc;

      /* Apply Unicode conversion */
      lvalp->core_yystype.str =
          str_udeescape(lvalp->core_yystype.str, escstr[0], *llocp, yyscanner);

      /*
       * We don't need to revert the un-truncation of UESCAPE.  What
       * we do want to do is clear have_lookahead, thereby consuming
       * all three tokens.
       */
      yyextra->have_lookahead = false;
    } else {
      /* No UESCAPE, so convert using default escape character */
      lvalp->core_yystype.str =
          str_udeescape(lvalp->core_yystype.str, '\\', *llocp, yyscanner);
    }

    if (cur_token == UIDENT) {
      /* It's an identifier, so truncate as appropriate */
      truncate_identifier(*scanner_extra(yyscanner), lvalp->core_yystype.str,
                          strlen(lvalp->core_yystype.str));
      cur_token = IDENT;
    } else if (cur_token == USCONST) {
      cur_token = SCONST;
    }
    break;
  }

  return cur_token;
}

/*
 * Run the scanner and the grammar over text, making nodes in tree.
 * mode_token is 0 for a list of statements, or the token that tells
 * the grammar what else the text is. Returns what the grammar built.
 */
static list_ref_t run_grammar(std::string_view text,
                              const sq_options_t &options, sq_tree_t &tree,
                              int mode_token) {
  // The grammar's actions make their nodes in the current tree.
  current_tree_t current(tree);
  parse_state_t state;
  scanner_t scanner(text, options, tree);
  scanner.extra().parse = &state;

  // The grammar is told what to parse by a token put before the text.
  if (mode_token != 0) {
    state.have_lookahead = true;
    state.lookahead_token = mode_token;
    state.lookahead_yylloc = 0;
    state.lookahead_end = nullptr;
  }

  // The grammar raises on anything it refuses, so a result other than
  // success is bison's own failure, which is memory.
  if (sq_base_yyparse(scanner.handle()) != 0) {
    throw std::bad_alloc();
  }
  return state.parsetree;
}

/*
 * Run fill, which parses into tree, as a call of the API: tree is
 * emptied first and again if fill fails, and whatever fill throws
 * becomes the calling thread's error and -1.
 */
template <typename Fill> static int parse_call(sq_tree_t &tree, Fill fill) {
  try {
    // A parse is all or nothing, so what the tree held goes first.
    tree = sq_tree_t();
    try {
      fill();
      return 0;
    } catch (...) {
      // Whatever was made before the error goes with it.
      tree = sq_tree_t();
      throw;
    }
  } catch (const parse_error_t &) {
    // raise() has recorded the error.
    return -1;
  } catch (const std::bad_alloc &) {
    return set_error(SQ_ERR_NOMEM, sqlstate_out_of_memory, 0, "out of memory");
  } catch (const std::exception &e) {
    return set_error(SQ_ERR_EXCEPTION, sqlstate_internal_error, 0,
                     "unexpected exception in sq: {}", e.what());
  } catch (...) {
    return set_error(SQ_ERR_EXCEPTION, sqlstate_internal_error, 0,
                     "unexpected exception in sq");
  }
}

/* Parse sql into out, or leave out empty and say why. */
int sq_parse(std::string_view sql, const sq_options_t *opt, sq_tree_t &out) {
  return parse_call(out, [&] {
    list_ref_t stmts = run_grammar(sql, opt ? *opt : sq_options_t(), out, 0);
    list_cell_t cell;
    foreach (cell, stmts) {
      out.stmts.push_back(static_cast<sq_raw_stmt_t *>(lfirst(cell)));
    }
  });
}

/* Parse text as one type name, into tree, and hand the type back. */
int sq_parse_type_name(std::string_view text, const sq_options_t *opt,
                       sq_tree_t &tree, sq_type_name_t **out) {
  return parse_call(tree, [&] {
    list_ref_t result =
        run_grammar(text, opt ? *opt : sq_options_t(), tree, MODE_TYPE_NAME);
    *out = static_cast<sq_type_name_t *>(linitial(result));
  });
}
