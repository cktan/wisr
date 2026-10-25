%top{
/*
 * This file is sq's fork of PostgreSQL 18.6's src/backend/parser/scan.l.
 * The rules are PostgreSQL's. What differs is around them:
 *
 *   - an error is raised with sq::raise, not ereport, and a warning or
 *     notice goes to the options' notice hook;
 *   - identifiers and literals are made in the tree being built, and
 *     the scan's own buffers are std::strings;
 *   - keywords are looked up in sq's table (src/keywords.cc);
 *   - the text is UTF-8, so the encoding is never asked for;
 *   - the entry points are scanner_t's (src/scan.h), below the rules.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <stdexcept>
#include <string>

#include "error.h"
#include "keywords.h"
#include "scan.h"
#include "scansup.h"
#include "tree.h"
#include "utf8.h"

/* The grammar's tokens; its value and location types are not used here. */
#include "gram.hh"
}

%{

using namespace sq;

/* PostgreSQL's names for what the rules below use. */
typedef void *core_yyscan_t;
typedef uint32_t pg_wchar;
#define NAMEDATALEN namedatalen
#define IS_HIGHBIT_SET(ch) ((unsigned char) (ch) & 0x80)

/*
 * A fatal error in flex's own code would print and exit(). Leave the
 * parse instead: sq_parse reports it as SQ_ERR_EXCEPTION.
 */
#define YY_FATAL_ERROR(msg) throw std::runtime_error(msg)

/*
 * Set the type of YYSTYPE.
 */
#undef YYSTYPE
#define YYSTYPE scan_value_t

/*
 * Each call to yylex must set yylloc to the location of the found token
 * (expressed as a byte offset from the start of the input text).
 * When we parse a token that requires multiple lexer rules to process,
 * this should be done in the first such rule, else yylloc will point
 * into the middle of the token.
 */
#undef YYLTYPE
#define YYLTYPE int
#define SET_YYLLOC()  (*(yylloc) = yytext - yyextra->scanbuf.data())

/*
 * Advance yylloc by the given number of bytes.
 */
#define ADVANCE_YYLLOC(delta)  ( *(yylloc) += (delta) )

/*
 * Sometimes, we do want yylloc to point into the middle of a token; this is
 * useful for instance to throw an error about an escape sequence within a
 * string literal.  But if we find no error there, we want to revert yylloc
 * to the token start, so that that's the location reported to the parser.
 * Use PUSH_YYLLOC/POP_YYLLOC to save/restore yylloc around such code.
 * (Currently the implied "stack" is just one location, but someday we might
 * need to nest these.)
 */
#define PUSH_YYLLOC()	(yyextra->save_yylloc = *(yylloc))
#define POP_YYLLOC()	(*(yylloc) = yyextra->save_yylloc)

#define startlit()	( yyextra->literal.clear() )
static void addlit(char *ytext, int yleng, core_yyscan_t yyscanner);
static void addlitchar(unsigned char ychar, core_yyscan_t yyscanner);
static char *litbufdup(core_yyscan_t yyscanner);
static char *scan_strdup(const char *text, core_yyscan_t yyscanner);
static unsigned char unescape_single_char(unsigned char c, core_yyscan_t yyscanner);
static int	process_integer_literal(const char *token, YYSTYPE *lval, int base,
									core_yyscan_t yyscanner);
static void addunicode(pg_wchar c, core_yyscan_t yyscanner);
static void verify_literal(core_yyscan_t yyscanner);

#define yyerror(msg)  scanner_yyerror(msg, yyscanner)

#define lexer_errposition()  scanner_errposition(*(yylloc), yyscanner)

static void check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner);
static void check_escape_warning(core_yyscan_t yyscanner);

%}

%option reentrant
%option bison-bridge
%option bison-locations
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="sq_core_yy"
%option extra-type="sq::scan_extra_t *"

/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 *
 * We use exclusive states for quoted strings, extended comments,
 * and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> bit string literal
 *  <xc> extended C-style comments
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xh> hexadecimal byte string
 *  <xq> standard quoted strings
 *  <xqs> quote stop (detect continued strings)
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xus> quoted string with Unicode escapes
 *  <xeu> Unicode surrogate pair in extended quoted string
 *
 * Remember to add an <<EOF>> case whenever you add a new exclusive state!
 * The default one is probably not the right thing.
 */

%x xb
%x xc
%x xd
%x xh
%x xq
%x xqs
%x xe
%x xdolq
%x xui
%x xus
%x xeu

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  Comments that start with -- and extend to the next
 * newline are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * non_newline_space tracks all the other space characters except newlines.
 *
 * XXX if you change the set of whitespace characters, fix scanner_isspace()
 * to agree.
 */

space				[ \t\n\r\f\v]
non_newline_space	[ \t\f\v]
newline				[\n\r]
non_newline			[^\n\r]

comment			("--"{non_newline}*)

whitespace		({space}+|{comment})

/*
 * SQL requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */

special_whitespace		({space}+|{comment}{newline})
non_newline_whitespace	({non_newline_space}|{comment})
whitespace_with_newline	({non_newline_whitespace}*{newline}{special_whitespace}*)

quote			'
/* If we see {quote} then {quotecontinue}, the quoted string continues */
quotecontinue	{whitespace_with_newline}{quote}

/*
 * {quotecontinuefail} is needed to avoid lexer backup when we fail to match
 * {quotecontinue}.  It might seem that this could just be {whitespace}*,
 * but if there's a dash after {whitespace_with_newline}, it must be consumed
 * to see if there's another dash --- which would start a {comment} and thus
 * allow continuation of the {quotecontinue} token.
 */
quotecontinuefail	{whitespace}*"-"?

/* Bit string
 * It is tempting to scan the string for only those characters
 * which are allowed. However, this leads to silently swallowed
 * characters if illegal characters are included in the string.
 * For example, if xbinside is [01] then B'ABCD' is interpreted
 * as a zero-length string, and the ABCD' is lost!
 * Better to pass the string forward and let the input routines
 * validate the contents.
 */
xbstart			[bB]{quote}
xbinside		[^']*

/* Hexadecimal byte string */
xhstart			[xX]{quote}
xhinside		[^']*

/* National character */
xnstart			[nN]{quote}

/* Quoted string that allows backslash escapes */
xestart			[eE]{quote}
xeinside		[^\\']+
xeescape		[\\][^0-7]
xeoctesc		[\\][0-7]{1,3}
xehexesc		[\\]x[0-9A-Fa-f]{1,2}
xeunicode		[\\](u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})
xeunicodefail	[\\](u[0-9A-Fa-f]{0,3}|U[0-9A-Fa-f]{0,7})

/* Extended quote
 * xqdouble implements embedded quote, ''''
 */
xqstart			{quote}
xqdouble		{quote}{quote}
xqinside		[^']+

/* $foo$ style quotes ("dollar quoting")
 * The quoted string starts with $foo$ where "foo" is an optional string
 * in the form of an identifier, except that it may not contain "$",
 * and extends to the first occurrence of an identical string.
 * There is *no* processing of the quoted text.
 *
 * {dolqfailed} is an error rule to avoid scanner backup when {dolqdelim}
 * fails to match its trailing "$".
 */
dolq_start		[A-Za-z\200-\377_]
dolq_cont		[A-Za-z\200-\377_0-9]
dolqdelim		\$({dolq_start}{dolq_cont}*)?\$
dolqfailed		\${dolq_start}{dolq_cont}*
dolqinside		[^$]+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

/* Quoted identifier with Unicode escapes */
xuistart		[uU]&{dquote}

/* Quoted string with Unicode escapes */
xusstart		[uU]&{quote}

/* error rule to avoid backup */
xufailed		[uU]&


/* C-style comments
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *    {operator} would. Then the tie-breaker (first matching rule of same
 *    length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *    in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *    if found throw it back with yyless().  This handles the plus-slash-star
 *    problem.
 * Dash-dash comments have similar interactions with the operator rule.
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside		[^*/]+

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

/* Assorted special-case operators and operator-like tokens */
typecast		"::"
dot_dot			\.\.
colon_equals	":="

/*
 * These operator-like tokens (unlike the above ones) also match the {operator}
 * rule, which means that they might be overridden by a longer match if they
 * are followed by a comment start or a + or - character. Accordingly, if you
 * add to this list, you must also add corresponding code to the {operator}
 * block to return the correct token in such cases. (This is not needed in
 * psqlscan.l since the token value is ignored there.)
 */
equals_greater	"=>"
less_equals		"<="
greater_equals	">="
less_greater	"<>"
not_equals		"!="

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.  "op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 */
self			[,()\[\].;\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\+\-\*\/\%\<\>\=]
operator_		{op_chars}+

/*
 * Numbers
 *
 * Unary minus is not part of a number here.  Instead we pass it separately to
 * the parser, and there it gets coerced via doNegate().
 *
 * {numericfail} is used because we would like "1..10" to lex as 1, dot_dot, 10.
 *
 * {realfail} is added to prevent the need for scanner
 * backup when the {real} rule fails to match completely.
 */
decdigit		[0-9]
hexdigit		[0-9A-Fa-f]
octdigit		[0-7]
bindigit		[0-1]

decinteger		{decdigit}(_?{decdigit})*
hexinteger		0[xX](_?{hexdigit})+
octinteger		0[oO](_?{octdigit})+
bininteger		0[bB](_?{bindigit})+

hexfail			0[xX]_?
octfail			0[oO]_?
binfail			0[bB]_?

numeric			(({decinteger}\.{decinteger}?)|(\.{decinteger}))
numericfail		{decinteger}\.\.

real			({decinteger}|{numeric})[Ee][-+]?{decinteger}
realfail		({decinteger}|{numeric})[Ee][-+]

/* Positional parameters don't accept underscores. */
param			\${decdigit}+

/*
 * An identifier immediately following an integer literal is disallowed because
 * in some cases it's ambiguous what is meant: for example, 0x1234 could be
 * either a hexinteger or a decinteger "0" and an identifier "x1234".  We can
 * detect such problems by seeing if integer_junk matches a longer substring
 * than any of the XXXinteger patterns (decinteger, hexinteger, octinteger,
 * bininteger).  One "junk" pattern is sufficient because
 * {decinteger}{identifier} will match all the same strings we'd match with
 * {hexinteger}{identifier} etc.
 *
 * Note that the rule for integer_junk must appear after the ones for
 * XXXinteger to make this work correctly: 0x1234 will match both hexinteger
 * and integer_junk, and we need hexinteger to be chosen in that case.
 *
 * Also disallow strings matched by numeric_junk, real_junk and param_junk
 * for consistency.
 */
integer_junk	{decinteger}{identifier}
numeric_junk	{numeric}{identifier}
real_junk		{real}{identifier}
param_junk		\${decdigit}+{identifier}

other			.

/*
 * Dollar quoted strings are totally opaque, and no escaping is done on them.
 * Other quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string.
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 */

%%

{whitespace}	{
					/* ignore */
				}

{xcstart}		{
					/* Set location in case of syntax error in comment */
					SET_YYLLOC();
					yyextra->xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

<xc>{
{xcstart}		{
					(yyextra->xcdepth)++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

{xcstop}		{
					if (yyextra->xcdepth <= 0)
						BEGIN(INITIAL);
					else
						(yyextra->xcdepth)--;
				}

{xcinside}		{
					/* ignore */
				}

{op_chars}		{
					/* ignore */
				}

\*+				{
					/* ignore */
				}

<<EOF>>			{
					yyerror("unterminated /* comment");
				}
} /* <xc> */

{xbstart}		{
					/* Binary bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "b" on the string
					 * to mark it for the input routine as a binary string.
					 */
					SET_YYLLOC();
					BEGIN(xb);
					startlit();
					addlitchar('b', yyscanner);
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xb><<EOF>>		{ yyerror("unterminated bit string literal"); }

{xhstart}		{
					/* Hexadecimal bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "x" on the string
					 * to mark it for the input routine as a hex string.
					 */
					SET_YYLLOC();
					BEGIN(xh);
					startlit();
					addlitchar('x', yyscanner);
				}
<xh><<EOF>>		{ yyerror("unterminated hexadecimal string literal"); }

{xnstart}		{
					/* National character.
					 * We will pass this along as a normal character string,
					 * but preceded with an internally-generated "NCHAR".
					 */
					int		kwnum;

					SET_YYLLOC();
					yyless(1);	/* eat only 'n' this time */

					kwnum = keyword_lookup("nchar", 5);
					if (kwnum >= 0)
					{
						yylval->keyword = keyword_name(kwnum);
						return keyword_token(kwnum);
					}
					else
					{
						/* If NCHAR isn't a keyword, just return "n" */
						yylval->str = scan_strdup("n", yyscanner);
						return IDENT;
					}
				}

{xqstart}		{
					yyextra->warn_on_first_escape = true;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					if (yyextra->options.standard_conforming_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
					startlit();
				}
{xestart}		{
					yyextra->warn_on_first_escape = false;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					BEGIN(xe);
					startlit();
				}
{xusstart}		{
					SET_YYLLOC();
					if (!yyextra->options.standard_conforming_strings)
						raise({.sqlstate = sqlstate_feature_not_supported,
							   .pos = lexer_errposition(),
							   .detail = "String constants with Unicode escapes cannot be used when \"standard_conforming_strings\" is off."},
							  "unsafe use of string constant with Unicode escapes");
					BEGIN(xus);
					startlit();
				}

<xb,xh,xq,xe,xus>{quote} {
					/*
					 * When we are scanning a quoted string and see an end
					 * quote, we must look ahead for a possible continuation.
					 * If we don't see one, we know the end quote was in fact
					 * the end of the string.  To reduce the lexer table size,
					 * we use a single "xqs" state to do the lookahead for all
					 * types of strings.
					 */
					yyextra->state_before_str_stop = YYSTATE;
					BEGIN(xqs);
				}
<xqs>{quotecontinue} {
					/*
					 * Found a quote continuation, so return to the in-quote
					 * state and continue scanning the literal.  Nothing is
					 * added to the literal's contents.
					 */
					BEGIN(yyextra->state_before_str_stop);
				}
<xqs>{quotecontinuefail} |
<xqs>{other} |
<xqs><<EOF>>	{
					/*
					 * Failed to see a quote continuation.  Throw back
					 * everything after the end quote, and handle the string
					 * according to the state we were in previously.
					 */
					yyless(0);
					BEGIN(INITIAL);

					switch (yyextra->state_before_str_stop)
					{
						case xb:
							yylval->str = litbufdup(yyscanner);
							return BCONST;
						case xh:
							yylval->str = litbufdup(yyscanner);
							return XCONST;
						case xq:
						case xe:
							/*
							 * Check that the data remains valid, if it might
							 * have been made invalid by unescaping any chars.
							 */
							if (yyextra->saw_non_ascii)
								verify_literal(yyscanner);
							yylval->str = litbufdup(yyscanner);
							return SCONST;
						case xus:
							yylval->str = litbufdup(yyscanner);
							return USCONST;
						default:
							yyerror("unhandled previous state in xqs");
					}
				}

<xq,xe,xus>{xqdouble} {
					addlitchar('\'', yyscanner);
				}
<xq,xus>{xqinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/*
					 * For consistency with other productions, issue any
					 * escape warning with cursor pointing to start of string.
					 * We might want to change that, someday.
					 */
					check_escape_warning(yyscanner);

					/* Remember start of overall string token ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq */
					SET_YYLLOC();

					if (is_utf16_surrogate_first(c))
					{
						yyextra->utf16_first_part = c;
						BEGIN(xeu);
					}
					else if (is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");
					else
						addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token */
					POP_YYLLOC();
				}
<xeu>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/* Remember start of overall string token ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq */
					SET_YYLLOC();

					if (!is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");

					c = surrogate_pair_to_codepoint(yyextra->utf16_first_part, c);

					addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token */
					POP_YYLLOC();

					BEGIN(xe);
				}
<xeu>. |
<xeu>\n |
<xeu><<EOF>>	{
					/* Set the error cursor to point at missing esc seq */
					SET_YYLLOC();
					yyerror("invalid Unicode surrogate pair");
				}
<xe,xeu>{xeunicodefail}	{
					/* Set the error cursor to point at malformed esc seq */
					SET_YYLLOC();
					raise({.sqlstate = sqlstate_invalid_escape_sequence,
						   .pos = lexer_errposition(),
						   .hint = "Unicode escapes must be \\uXXXX or \\UXXXXXXXX."},
						  "invalid Unicode escape");
				}
<xe>{xeescape}  {
					if (yytext[1] == '\'')
					{
						/*
						 * SAFE_ENCODING refuses \' only in a client-only
						 * encoding, which UTF-8 is not.
						 */
						if (yyextra->options.backslash_quote == SQ_BACKSLASH_QUOTE_OFF)
							raise({.sqlstate = sqlstate_nonstandard_use_of_escape_character,
								   .pos = lexer_errposition(),
								   .hint = "Use '' to write quotes in strings. \\' is insecure in client-only encodings."},
								  "unsafe use of \\' in a string literal");
					}
					check_string_escape_warning(yytext[1], yyscanner);
					addlitchar(unescape_single_char(yytext[1], yyscanner),
							   yyscanner);
				}
<xe>{xeoctesc}  {
					unsigned char c = strtoul(yytext + 1, NULL, 8);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>{xehexesc}  {
					unsigned char c = strtoul(yytext + 2, NULL, 16);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>.			{
					/* This is only needed for \ just before EOF */
					addlitchar(yytext[0], yyscanner);
				}
<xq,xe,xus><<EOF>>		{ yyerror("unterminated quoted string"); }

{dolqdelim}		{
					SET_YYLLOC();
					yyextra->dolqstart = yytext;
					BEGIN(xdolq);
					startlit();
				}
{dolqfailed}	{
					SET_YYLLOC();
					/* throw back all but the initial "$" */
					yyless(1);
					/* and treat it as {other} */
					return yytext[0];
				}
<xdolq>{dolqdelim} {
					if (yyextra->dolqstart == yytext)
					{
						yyextra->dolqstart.clear();
						BEGIN(INITIAL);
						yylval->str = litbufdup(yyscanner);
						return SCONST;
					}
					else
					{
						/*
						 * When we fail to match $...$ to dolqstart, transfer
						 * the $... part to the output, but put back the final
						 * $ for rescanning.  Consider $delim$...$junk$delim$
						 */
						addlit(yytext, yyleng - 1, yyscanner);
						yyless(yyleng - 1);
					}
				}
<xdolq>{dolqinside} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>{dolqfailed} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>.		{
					/* This is only needed for $ inside the quoted text */
					addlitchar(yytext[0], yyscanner);
				}
<xdolq><<EOF>>	{ yyerror("unterminated dollar-quoted string"); }

{xdstart}		{
					SET_YYLLOC();
					BEGIN(xd);
					startlit();
				}
{xuistart}		{
					SET_YYLLOC();
					BEGIN(xui);
					startlit();
				}
<xd>{xdstop}	{
					char	   *ident;

					BEGIN(INITIAL);
					if (yyextra->literal.empty())
						yyerror("zero-length delimited identifier");
					ident = litbufdup(yyscanner);
					truncate_identifier(*yyextra, ident,
										(int) yyextra->literal.size());
					yylval->str = ident;
					return IDENT;
				}
<xui>{dquote}	{
					BEGIN(INITIAL);
					if (yyextra->literal.empty())
						yyerror("zero-length delimited identifier");
					/* can't truncate till after we de-escape the ident */
					yylval->str = litbufdup(yyscanner);
					return UIDENT;
				}
<xd,xui>{xddouble}	{
					addlitchar('"', yyscanner);
				}
<xd,xui>{xdinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xd,xui><<EOF>>		{ yyerror("unterminated quoted identifier"); }

{xufailed}	{
					char	   *ident;

					SET_YYLLOC();
					/* throw back all but the initial u/U */
					yyless(1);
					/* and treat it as {identifier} */
					ident = downcase_truncate_identifier(*yyextra, yytext, yyleng);
					yylval->str = ident;
					return IDENT;
				}

{typecast}		{
					SET_YYLLOC();
					return TYPECAST;
				}

{dot_dot}		{
					SET_YYLLOC();
					return DOT_DOT;
				}

{colon_equals}	{
					SET_YYLLOC();
					return COLON_EQUALS;
				}

{equals_greater} {
					SET_YYLLOC();
					return EQUALS_GREATER;
				}

{less_equals}	{
					SET_YYLLOC();
					return LESS_EQUALS;
				}

{greater_equals} {
					SET_YYLLOC();
					return GREATER_EQUALS;
				}

{less_greater}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{not_equals}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{self}			{
					SET_YYLLOC();
					return yytext[0];
				}

{operator_}		{
					/*
					 * Check for embedded slash-star or dash-dash; those
					 * are comment starts, so operator must stop there.
					 * Note that slash-star or dash-dash at the first
					 * character will match a prior rule, not this one.
					 */
					int			nchars = yyleng;
					char	   *slashstar = strstr(yytext, "/*");
					char	   *dashdash = strstr(yytext, "--");

					if (slashstar && dashdash)
					{
						/* if both appear, take the first one */
						if (slashstar > dashdash)
							slashstar = dashdash;
					}
					else if (!slashstar)
						slashstar = dashdash;
					if (slashstar)
						nchars = slashstar - yytext;

					/*
					 * For SQL compatibility, '+' and '-' cannot be the
					 * last char of a multi-char operator unless the operator
					 * contains chars that are not in SQL operators.
					 * The idea is to lex '=-' as two operators, but not
					 * to forbid operator names like '?-' that could not be
					 * sequences of SQL operators.
					 */
					if (nchars > 1 &&
						(yytext[nchars - 1] == '+' ||
						 yytext[nchars - 1] == '-'))
					{
						int			ic;

						for (ic = nchars - 2; ic >= 0; ic--)
						{
							char c = yytext[ic];
							if (c == '~' || c == '!' || c == '@' ||
								c == '#' || c == '^' || c == '&' ||
								c == '|' || c == '`' || c == '?' ||
								c == '%')
								break;
						}
						if (ic < 0)
						{
							/*
							 * didn't find a qualifying character, so remove
							 * all trailing [+-]
							 */
							do {
								nchars--;
							} while (nchars > 1 &&
								 (yytext[nchars - 1] == '+' ||
								  yytext[nchars - 1] == '-'));
						}
					}

					SET_YYLLOC();

					if (nchars < yyleng)
					{
						/* Strip the unwanted chars from the token */
						yyless(nchars);
						/*
						 * If what we have left is only one char, and it's
						 * one of the characters matching "self", then
						 * return it as a character token the same way
						 * that the "self" rule would have.
						 */
						if (nchars == 1 &&
							strchr(",()[].;:+-*/%^<>=", yytext[0]))
							return yytext[0];
						/*
						 * Likewise, if what we have left is two chars, and
						 * those match the tokens ">=", "<=", "=>", "<>" or
						 * "!=", then we must return the appropriate token
						 * rather than the generic Op.
						 */
						if (nchars == 2)
						{
							if (yytext[0] == '=' && yytext[1] == '>')
								return EQUALS_GREATER;
							if (yytext[0] == '>' && yytext[1] == '=')
								return GREATER_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '=')
								return LESS_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '>')
								return NOT_EQUALS;
							if (yytext[0] == '!' && yytext[1] == '=')
								return NOT_EQUALS;
						}
					}

					/*
					 * Complain if operator is too long.  Unlike the case
					 * for identifiers, we make this an error not a notice-
					 * and-truncate, because the odds are we are looking at
					 * a syntactic mistake anyway.
					 */
					if (nchars >= NAMEDATALEN)
						yyerror("operator too long");

					yylval->str = scan_strdup(yytext, yyscanner);
					return Op;
				}

{param}			{
					int32_t		val;

					SET_YYLLOC();
					if (!parse_int32(yytext + 1, &val))
						yyerror("parameter number too large");
					yylval->ival = val;
					return PARAM;
				}
{param_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after parameter");
				}

{decinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10, yyscanner);
				}
{hexinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 16, yyscanner);
				}
{octinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 8, yyscanner);
				}
{bininteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 2, yyscanner);
				}
{hexfail}		{
					SET_YYLLOC();
					yyerror("invalid hexadecimal integer");
				}
{octfail}		{
					SET_YYLLOC();
					yyerror("invalid octal integer");
				}
{binfail}		{
					SET_YYLLOC();
					yyerror("invalid binary integer");
				}
{numeric}		{
					SET_YYLLOC();
					yylval->str = scan_strdup(yytext, yyscanner);
					return FCONST;
				}
{numericfail}	{
					/* throw back the .., and treat as integer */
					yyless(yyleng - 2);
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10, yyscanner);
				}
{real}			{
					SET_YYLLOC();
					yylval->str = scan_strdup(yytext, yyscanner);
					return FCONST;
				}
{realfail}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{integer_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{numeric_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{real_junk}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}


{identifier}	{
					int			kwnum;
					char	   *ident;

					SET_YYLLOC();

					/* Is it a keyword? */
					kwnum = keyword_lookup(yytext, yyleng);
					if (kwnum >= 0)
					{
						yylval->keyword = keyword_name(kwnum);
						return keyword_token(kwnum);
					}

					/*
					 * No.  Convert the identifier to lower case, and truncate
					 * if necessary.
					 */
					ident = downcase_truncate_identifier(*yyextra, yytext, yyleng);
					yylval->str = ident;
					return IDENT;
				}

{other}			{
					SET_YYLLOC();
					return yytext[0];
				}

<<EOF>>			{
					SET_YYLLOC();
					yyterminate();
				}

%%

/* LCOV_EXCL_STOP */

/*
 * Arrange access to yyextra for subroutines of the main yylex() function.
 * We expect each subroutine to have a yyscanner parameter.  Rather than
 * use the yyget_xxx functions, which might or might not get inlined by the
 * compiler, we cheat just a bit and cast yyscanner to the right type.
 */
#undef yyextra
#define yyextra  (((struct yyguts_t *) yyscanner)->yyextra_r)

/* Likewise for a couple of other things we need. */
#undef yylloc
#define yylloc	(((struct yyguts_t *) yyscanner)->yylloc_r)
#undef yyleng
#define yyleng	(((struct yyguts_t *) yyscanner)->yyleng_r)

/* scanner_extra (scan.h) reads the extra as the handle's first field. */
static_assert(offsetof(struct yyguts_t, yyextra_r) == 0);

namespace sq {

/*
 * scanner_errposition
 *		The position in characters, from 1, of a byte offset in the
 *		text: what PostgreSQL reports as an error's cursor position.
 */
int
scanner_errposition(int location, void *yyscanner)
{
	if (location < 0)
		return 0;				/* no-op if location is unknown */

	/* Convert byte offset to character number */
	return utf8_strlen_with_len(yyextra->scanbuf.data(), location) + 1;
}

/*
 * scanner_yyerror
 *		Report a lexer or grammar error.
 *
 * The message's cursor position is whatever YYLLOC was last set to,
 * ie, the start of the current token if called within yylex(), or the
 * most recently lexed token if called from the grammar.
 * This is OK for syntax error messages from the Bison parser, because Bison
 * parsers report error as soon as the first unparsable token is reached.
 * Beware of using yyerror for other purposes, as the cursor position might
 * be misleading!
 */
void
scanner_yyerror(const char *message, void *yyscanner)
{
	const char *loc = yyextra->scanbuf.data() + *yylloc;

	if (*loc == YY_END_OF_BUFFER_CHAR)
		raise({.sqlstate = sqlstate_syntax_error,
			   .pos = lexer_errposition()},
			  "{} at end of input", message);
	else
		raise({.sqlstate = sqlstate_syntax_error,
			   .pos = lexer_errposition()},
			  "{} at or near \"{}\"", message, loc);
}

/* Hand a notice to the options' hook, if one is set. */
void
notify(const scan_extra_t &extra, sq_notice_level_t level,
	   const char *sqlstate, int pos, const std::string &message,
	   const char *hint)
{
	if (extra.options.notice == nullptr)
		return;

	sq_notice_t notice = {level, sqlstate, message.c_str(), hint, pos};

	extra.options.notice(extra.options.notice_arg, &notice);
}

/* Set up flex to scan a copy of sql. */
scanner_t::scanner_t(std::string_view sql, const sq_options_t &options,
					 sq_tree_t &tree)
{
	m_extra.tree = &tree;
	m_extra.options = options;

	/*
	 * A NUL cannot be in the text: it is what ends it. PostgreSQL refuses
	 * one in what a client sends, as a byte that is no character.
	 */
	if (sql.find('\0') != std::string_view::npos)
		raise({.sqlstate = sqlstate_character_not_in_repertoire},
			  "invalid byte sequence for encoding \"UTF8\": 0x00");

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	m_extra.scanbuf.reserve(sql.size() + 2);
	m_extra.scanbuf.assign(sql);
	m_extra.scanbuf.append(2, YY_END_OF_BUFFER_CHAR);

	if (yylex_init(&m_scanner) != 0)
		throw std::bad_alloc();

	sq_core_yyset_extra(&m_extra, m_scanner);

	try
	{
		yy_scan_buffer(m_extra.scanbuf.data(), m_extra.scanbuf.size(),
					   m_scanner);
	}
	catch (...)
	{
		/* Nothing owns flex's state until the constructor returns. */
		yylex_destroy(m_scanner);
		throw;
	}
}

/* Give flex's state back. */
scanner_t::~scanner_t()
{
	yylex_destroy(m_scanner);
}

/* Scan one token. */
int
scanner_t::lex(scan_value_t *value, int *location)
{
	return sq_core_yylex(value, location, m_scanner);
}

} /* namespace sq */


static void
addlit(char *ytext, int yleng, core_yyscan_t yyscanner)
{
	yyextra->literal.append(ytext, yleng);
}


static void
addlitchar(unsigned char ychar, core_yyscan_t yyscanner)
{
	yyextra->literal.push_back((char) ychar);
}


/*
 * Create a copy of the literal in the tree, adding a trailing null.
 */
static char *
litbufdup(core_yyscan_t yyscanner)
{
	return dup_string(*yyextra->tree, yyextra->literal);
}

/*
 * Create a copy of a string in the tree.
 */
static char *
scan_strdup(const char *text, core_yyscan_t yyscanner)
{
	return dup_string(*yyextra->tree, text);
}

/*
 * Check that a literal is still valid UTF-8 after its escapes have been
 * turned into bytes: PostgreSQL's pg_verifymbstr.
 */
static void
verify_literal(core_yyscan_t yyscanner)
{
	const std::string &lit = yyextra->literal;
	int			len = (int) lit.size();
	int			ok = utf8_verify(lit.data(), len);
	std::string bytes;
	int			n;

	if (ok == len)
		return;

	/* Name the bytes of the character that is not one, as PostgreSQL does. */
	n = utf8_mblen((const unsigned char *) lit.data() + ok);
	if (n > len - ok)
		n = len - ok;
	for (int j = 0; j < n; j++)
	{
		char		hex[8];

		snprintf(hex, sizeof(hex), "0x%02x", (unsigned char) lit[ok + j]);
		if (j > 0)
			bytes += ' ';
		bytes += hex;
	}
	raise({.sqlstate = sqlstate_character_not_in_repertoire},
		  "invalid byte sequence for encoding \"UTF8\": {}", bytes);
}

/*
 * Process {decinteger}, {hexinteger}, etc.  Note this will also do the right
 * thing with {numeric}, ie digits and a decimal point.
 */
static int
process_integer_literal(const char *token, YYSTYPE *lval, int base,
						core_yyscan_t yyscanner)
{
	int32_t		val;

	(void) base;
	if (!parse_int32(token, &val))
	{
		/* integer too large (or contains decimal pt), treat it as a float */
		lval->str = scan_strdup(token, yyscanner);
		return FCONST;
	}
	lval->ival = val;
	return ICONST;
}

static void
addunicode(pg_wchar c, core_yyscan_t yyscanner)
{
	unsigned char buf[utf8_max_len + 1];
	int			len;

	if (!is_valid_unicode_codepoint(c))
		yyerror("invalid Unicode escape value");

	/*
	 * Any valid code point can be written as UTF-8, so we don't have to
	 * set saw_non_ascii.
	 */
	len = unicode_to_utf8(c, buf);
	addlit((char *) buf, len, yyscanner);
}

static unsigned char
unescape_single_char(unsigned char c, core_yyscan_t yyscanner)
{
	switch (c)
	{
		case 'b':
			return '\b';
		case 'f':
			return '\f';
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'v':
			return '\v';
		default:
			/* check for backslash followed by non-7-bit-ASCII */
			if (c == '\0' || IS_HIGHBIT_SET(c))
				yyextra->saw_non_ascii = true;

			return c;
	}
}

static void
check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner)
{
	if (ychar == '\'')
	{
		if (yyextra->warn_on_first_escape && yyextra->options.escape_string_warning)
			notify(*yyextra, SQ_WARNING,
				   sqlstate_nonstandard_use_of_escape_character,
				   lexer_errposition(),
				   "nonstandard use of \\' in a string literal",
				   "Use '' to write quotes in strings, or use the escape string syntax (E'...').");
		yyextra->warn_on_first_escape = false;	/* warn only once per string */
	}
	else if (ychar == '\\')
	{
		if (yyextra->warn_on_first_escape && yyextra->options.escape_string_warning)
			notify(*yyextra, SQ_WARNING,
				   sqlstate_nonstandard_use_of_escape_character,
				   lexer_errposition(),
				   "nonstandard use of \\\\ in a string literal",
				   "Use the escape string syntax for backslashes, e.g., E'\\\\'.");
		yyextra->warn_on_first_escape = false;	/* warn only once per string */
	}
	else
		check_escape_warning(yyscanner);
}

static void
check_escape_warning(core_yyscan_t yyscanner)
{
	if (yyextra->warn_on_first_escape && yyextra->options.escape_string_warning)
		notify(*yyextra, SQ_WARNING,
			   sqlstate_nonstandard_use_of_escape_character,
			   lexer_errposition(),
			   "nonstandard use of escape in a string literal",
			   "Use the escape string syntax for escapes, e.g., E'\\r\\n'.");
	yyextra->warn_on_first_escape = false;	/* warn only once per string */
}

/*
 * Interface functions to make flex use operator new's way of failing:
 * memory that cannot be had is an exception, never a null pointer for
 * flex to print about and exit on.
 */

void *
sq_core_yyalloc(yy_size_t bytes, core_yyscan_t yyscanner)
{
	void	   *p = malloc(bytes);

	(void) yyscanner;
	if (p == NULL)
		throw std::bad_alloc();
	return p;
}

void *
sq_core_yyrealloc(void *ptr, yy_size_t bytes, core_yyscan_t yyscanner)
{
	void	   *p = realloc(ptr, bytes);

	(void) yyscanner;
	if (p == NULL)
		throw std::bad_alloc();
	return p;
}

void
sq_core_yyfree(void *ptr, core_yyscan_t yyscanner)
{
	(void) yyscanner;
	free(ptr);
}
