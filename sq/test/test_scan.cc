/*
 * test_scan.cc -- what test/tokens.sh cannot show of the scanner: it
 * compares sq's tokens with PostgreSQL's, which covers every rule,
 * but not the notice hook, the options, or what is freed when a scan
 * ends in an error.
 */

#include "doctest.h"

#include "sq.h"

#include "error.h"
#include "gram.hh"
#include "keywords.h"
#include "scan.h"
#include "scansup.h"
#include "utf8.h"

#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

/* One token: its number and, for the tokens that have one, its text. */
struct token_t {
  int token;
  std::string text;
};

/* A notice, copied out of the hook's call. */
struct seen_notice_t {
  sq_notice_level_t level;
  std::string sqlstate;
  std::string message;
  std::string hint;
  int pos;
};

/* A notice hook that appends to the std::vector<seen_notice_t> at arg. */
void collect_notice(void *arg, const sq_notice_t *n) {
  static_cast<std::vector<seen_notice_t> *>(arg)->push_back(
      {n->level, n->sqlstate, n->message, n->hint ? n->hint : "", n->pos});
}

/*
 * Scan sql to its end. Returns the tokens, or raises as the scanner
 * does.
 */
std::vector<token_t> scan(const std::string &sql, const sq_options_t &opt) {
  sq_tree_t tree;
  sq::scanner_t scanner(sql, opt, tree);
  std::vector<token_t> out;
  for (;;) {
    sq::scan_value_t value;
    int location = 0;
    int token = scanner.lex(&value, &location);
    if (token == 0) {
      return out;
    }
    bool has_text = token == IDENT || token == SCONST || token == USCONST ||
                    token == UIDENT;
    out.push_back({token, has_text ? value.str : ""});
  }
}

/* Whether scanning sql raises; the thread's error then says why. */
bool scan_fails(const std::string &sql, const sq_options_t &opt) {
  try {
    scan(sql, opt);
    return false;
  } catch (const sq::parse_error_t &) {
    return true;
  }
}

} // namespace

TEST_CASE("an identifier too long is cut, and the hook is told") {
  // Verify PostgreSQL's truncation to 63 bytes and its NOTICE.
  // 1. Scan an identifier of 70 letters, upper case, with a notice hook.
  // 2. Confirm the token is the first 63 letters, lowered.
  // 3. Confirm one NOTICE, with PostgreSQL's SQLSTATE and message.
  std::vector<seen_notice_t> seen;
  sq_options_t opt;
  opt.notice = collect_notice;
  opt.notice_arg = &seen;

  std::vector<token_t> tokens = scan(std::string(70, 'A'), opt);
  REQUIRE(tokens.size() == 1);
  CHECK(tokens[0].token == IDENT);
  CHECK(tokens[0].text == std::string(63, 'a'));
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].level == SQ_NOTICE);
  CHECK(seen[0].sqlstate == "42622");
  CHECK(seen[0].message == "identifier \"" + std::string(70, 'a') +
                               "\" will be truncated to \"" +
                               std::string(63, 'a') + "\"");
  CHECK(seen[0].hint.empty());
}

TEST_CASE("an identifier is cut between characters, not inside one") {
  // Verify the cut never leaves part of a multibyte character.
  // 1. Scan a quoted identifier of 62 letters and then a 2-byte character.
  // 2. Confirm the token is the 62 letters: the character did not fit.
  sq_options_t opt;
  std::string id = std::string(62, 'a') + "\xc3\xa9";
  std::vector<token_t> tokens = scan("\"" + id + "\"", opt);
  REQUIRE(tokens.size() == 1);
  CHECK(tokens[0].text == std::string(62, 'a'));
}

TEST_CASE("with no hook, a notice is dropped and the scan goes on") {
  // Verify the hook is optional.
  // 1. Scan an over-long identifier with default options.
  // 2. Confirm the token is still cut.
  sq_options_t opt;
  std::vector<token_t> tokens = scan(std::string(70, 'b'), opt);
  REQUIRE(tokens.size() == 1);
  CHECK(tokens[0].text == std::string(63, 'b'));
}

TEST_CASE("standard_conforming_strings off reads backslashes, and warns") {
  // Verify the setting changes what a plain string means, with a WARNING.
  // 1. Scan 'a\nb' with the setting on: the backslash is itself.
  // 2. Scan it with the setting off: it is a newline, and the hook gets
  //    one WARNING with PostgreSQL's hint, at the string.
  // 3. Turn escape_string_warning off: the same token, no warning.
  std::vector<seen_notice_t> seen;
  sq_options_t opt;
  opt.notice = collect_notice;
  opt.notice_arg = &seen;
  const std::string sql = "SELECT 'a\\nb'";

  std::vector<token_t> on = scan(sql, opt);
  REQUIRE(on.size() == 2);
  CHECK(on[1].text == "a\\nb");
  CHECK(seen.empty());

  opt.standard_conforming_strings = false;
  std::vector<token_t> off = scan(sql, opt);
  REQUIRE(off.size() == 2);
  CHECK(off[1].text == "a\nb");
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].level == SQ_WARNING);
  CHECK(seen[0].sqlstate == "22P06");
  CHECK(seen[0].message == "nonstandard use of escape in a string literal");
  CHECK(seen[0].hint ==
        "Use the escape string syntax for escapes, e.g., E'\\r\\n'.");
  CHECK(seen[0].pos == 8);

  seen.clear();
  opt.escape_string_warning = false;
  CHECK(scan(sql, opt)[1].text == "a\nb");
  CHECK(seen.empty());
}

TEST_CASE("the warnings for \\' and \\\\ are PostgreSQL's, once a string") {
  // Verify the other two escape warnings, and that a string warns once.
  // 1. With standard_conforming_strings off, scan 'a\'b\'c'.
  // 2. Confirm one WARNING about \', though the string has two.
  // 3. Scan 'a\\b' and confirm the WARNING about \\.
  std::vector<seen_notice_t> seen;
  sq_options_t opt;
  opt.standard_conforming_strings = false;
  opt.backslash_quote = SQ_BACKSLASH_QUOTE_ON;
  opt.notice = collect_notice;
  opt.notice_arg = &seen;

  CHECK(scan("SELECT 'a\\'b\\'c'", opt)[1].text == "a'b'c");
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].message == "nonstandard use of \\' in a string literal");
  CHECK(seen[0].hint == "Use '' to write quotes in strings, or use the "
                        "escape string syntax (E'...').");

  seen.clear();
  CHECK(scan("SELECT 'a\\\\b'", opt)[1].text == "a\\b");
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].message == "nonstandard use of \\\\ in a string literal");
  CHECK(seen[0].hint ==
        "Use the escape string syntax for backslashes, e.g., E'\\\\'.");
}

TEST_CASE("backslash_quote off refuses \\' with PostgreSQL's hint") {
  // Verify the setting, and that safe_encoding allows \' in UTF-8.
  // 1. Scan E'a\'b' with the default, safe_encoding: it is a'b.
  // 2. Scan it with backslash_quote off: an error with a hint, at the \'.
  sq_options_t opt;
  const std::string sql = "SELECT E'a\\'b'";
  CHECK(scan(sql, opt)[1].text == "a'b");

  opt.backslash_quote = SQ_BACKSLASH_QUOTE_OFF;
  REQUIRE(scan_fails(sql, opt));
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_sqlstate()) == "22P06");
  CHECK(std::string(sq_errmsg()) == "unsafe use of \\' in a string literal");
  CHECK(std::string(sq_errhint()) ==
        "Use '' to write quotes in strings. \\' is insecure in client-only "
        "encodings.");
  CHECK(std::string(sq_errdetail()).empty());
  CHECK(sq_errpos() == 8);
}

TEST_CASE("a Unicode string is refused, with a detail, when unsafe") {
  // Verify an error's detail reaches the caller.
  // 1. With standard_conforming_strings off, scan a U&'' string.
  // 2. Confirm the error, its SQLSTATE and PostgreSQL's detail.
  sq_options_t opt;
  opt.standard_conforming_strings = false;
  REQUIRE(scan_fails("SELECT U&'x'", opt));
  CHECK(std::string(sq_sqlstate()) == "0A000");
  CHECK(std::string(sq_errmsg()) ==
        "unsafe use of string constant with Unicode escapes");
  CHECK(std::string(sq_errdetail()) ==
        "String constants with Unicode escapes cannot be used when "
        "\"standard_conforming_strings\" is off.");
  CHECK(std::string(sq_errhint()).empty());
}

TEST_CASE("an error's position counts characters, not bytes") {
  // Verify the position PostgreSQL reports to a client.
  // 1. Scan text with a 2-byte character before an unterminated string.
  // 2. Confirm the position is the string's character, one less than its
  //    byte.
  sq_options_t opt;
  REQUIRE(scan_fails("SELECT '\xc3\xa9', 'open", opt));
  CHECK(std::string(sq_errmsg()) ==
        "unterminated quoted string at or near \"'open\"");
  CHECK(sq_errpos() == 13);
}

TEST_CASE("a NUL in the text is refused before anything is scanned") {
  // Verify text with a NUL byte cannot be mistaken for shorter text.
  // 1. Scan text with a NUL in the middle.
  // 2. Confirm PostgreSQL's error for a byte that is no character.
  sq_options_t opt;
  REQUIRE(scan_fails(std::string("SELECT 1\0; DROP TABLE t", 23), opt));
  CHECK(std::string(sq_sqlstate()) == "22021");
  CHECK(std::string(sq_errmsg()) ==
        "invalid byte sequence for encoding \"UTF8\": 0x00");
}

TEST_CASE("a scan that ends in an error frees what it made") {
  // Verify nothing is left behind by a scan abandoned halfway.
  // 1. Scan long literals, a dollar quote left open, into a tree.
  // 2. Let the error unwind the scanner and the tree; the leak checker,
  //    which the tests are linked with, sees the rest.
  sq_options_t opt;
  std::string sql = "SELECT '" + std::string(100000, 'x') + "', \"" +
                    std::string(50, 'y') + "\", $q$ never closed";
  CHECK(scan_fails(sql, opt));
  CHECK(std::string(sq_errmsg())
            .starts_with("unterminated dollar-quoted string at or near"));
}

TEST_CASE("every keyword is found at its own number, whatever its case") {
  // Verify the perfect hash is perfect, and folds ASCII only.
  // 1. Look up each keyword as spelled, and in upper case: each is found
  //    at its own number.
  // 2. Look up each keyword with a letter added or its last one dropped:
  //    it is that keyword no longer.
  // 3. Look up a keyword in mixed case.
  // 4. Confirm an over-long word is not found.
  // 5. Confirm a letter outside ASCII is not folded into a keyword, and
  //    nor is a character that differs from a letter only in bit 0x20.
  REQUIRE(sq::keyword_count() > 400);
  for (int i = 0; i < sq::keyword_count(); i++) {
    std::string name = sq::keyword_name(i);
    std::string upper = name;
    for (char &c : upper) {
      c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    CHECK(sq::keyword_lookup(name.data(), name.size()) == i);
    CHECK(sq::keyword_lookup(upper.data(), upper.size()) == i);
    std::string longer = name + "q";
    CHECK(sq::keyword_lookup(longer.data(), longer.size()) != i);
    CHECK(sq::keyword_lookup(name.data(), name.size() - 1) != i);
  }
  int kw = sq::keyword_lookup("SeLeCt", 6);
  REQUIRE(kw >= 0);
  CHECK(std::string(sq::keyword_name(kw)) == "select");
  CHECK(sq::keyword_token(kw) == SELECT);
  CHECK(sq::keyword_lookup("select", 6) == kw);
  CHECK(sq::keyword_lookup("selec", 5) == -1);
  CHECK(sq::keyword_lookup("selectx", 7) == -1);
  std::string longer(200, 'a');
  CHECK(sq::keyword_lookup(longer.data(), longer.size()) == -1);
  // U+212A KELVIN SIGN lowers to 'k' in Unicode; it is not ASCII.
  CHECK(sq::keyword_lookup("\xe2\x84\xaa"
                           "ey",
                           5) == -1);
  // '@' is 'A' with bit 0x20 cleared from '`'; the hash folds it as it
  // folds a letter, and the comparison must still tell them apart.
  CHECK(sq::keyword_lookup("s\x05lect", 6) == -1);
  CHECK(sq::keyword_lookup("SELEC\x14", 6) == -1);
}

TEST_CASE("an integer is read as PostgreSQL reads it") {
  // Verify parse_int32 at its edges: range, bases and underscores.
  // 1. The largest and smallest 32-bit values, and one past each.
  // 2. Each base's prefix, in either case.
  // 3. Underscores: between digits only, and after a prefix.
  // 4. Things that are no integer.
  int32_t v = 0;
  CHECK((sq::parse_int32("2147483647", &v) && v == 2147483647));
  CHECK(!sq::parse_int32("2147483648", &v));
  CHECK((sq::parse_int32("-2147483648", &v) && v == -2147483647 - 1));
  CHECK(!sq::parse_int32("-2147483649", &v));
  CHECK(!sq::parse_int32("99999999999999999999999999", &v));

  CHECK((sq::parse_int32("0x7FffFFff", &v) && v == 2147483647));
  CHECK((sq::parse_int32("0X1f", &v) && v == 31));
  CHECK((sq::parse_int32("0o17", &v) && v == 15));
  CHECK((sq::parse_int32("0b101", &v) && v == 5));
  CHECK(!sq::parse_int32("0o8", &v));
  CHECK(!sq::parse_int32("0b2", &v));

  CHECK((sq::parse_int32("1_000_000", &v) && v == 1000000));
  CHECK((sq::parse_int32("0x_ff", &v) && v == 255));
  CHECK(!sq::parse_int32("_1", &v));
  CHECK(!sq::parse_int32("1__0", &v));
  CHECK(!sq::parse_int32("1_", &v));

  CHECK(!sq::parse_int32("", &v));
  CHECK(!sq::parse_int32("0x", &v));
  CHECK(!sq::parse_int32("1.5", &v));
  CHECK(!sq::parse_int32("12a", &v));
}

TEST_CASE("UTF-8 is verified as PostgreSQL verifies it") {
  // Verify utf8_verify stops at the first byte that begins no character.
  // 1. Valid text of every length of character.
  // 2. A stray continuation byte, an overlong form, a surrogate, a cut
  //    character and a NUL: each stops the count where it stands.
  auto ok = [](const std::string &s) {
    return sq::utf8_verify(s.data(), static_cast<int>(s.size()));
  };
  CHECK(ok("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 10);
  CHECK(ok("ab\x80") == 2);
  CHECK(ok("a\xc0\xaf") == 1);
  CHECK(ok("\xed\xa0\x80") == 0);
  CHECK(ok("abc\xe2\x82") == 3);
  CHECK(ok(std::string("ab\0cd", 5)) == 2);
}
