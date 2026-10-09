/*
 * scansup.cc -- identifiers folded and cut, and integers read.
 */

#include "scansup.h"

#include <string.h>

#include <cctype>
#include <format>
#include <string>
#include <string_view>

#include "error.h"
#include "scan.h"
#include "tree.h"
#include "utf8.h"

namespace sq {

/* PostgreSQL's truncate_identifier. */
void truncate_identifier(scan_extra_t &extra, char *ident, int len) {
  if (len < namedatalen) {
    return;
  }
  len = utf8_cliplen(ident, len, namedatalen - 1);
  notify(extra, SQ_NOTICE, sqlstate_name_too_long, 0,
         std::format("identifier \"{}\" will be truncated to \"{}\"", ident,
                     std::string_view(ident, len)),
         nullptr);
  ident[len] = '\0';
}

/* PostgreSQL's downcase_truncate_identifier, for a multibyte encoding. */
char *downcase_truncate_identifier(scan_extra_t &extra, const char *ident,
                                   int len) {
  char *result = dup_string(*extra.tree, std::string_view(ident, len));
  // Only ASCII is folded: a byte with the high bit set is part of a
  // multibyte character.
  for (int i = 0; i < len; i++) {
    if (result[i] >= 'A' && result[i] <= 'Z') {
      result[i] += 'a' - 'A';
    }
  }
  truncate_identifier(extra, result, len);
  return result;
}

namespace {

/* The value of digit ch in the given base, or -1 if it is not one. */
int digit_value(char ch, int base) {
  int v;
  if (ch >= '0' && ch <= '9') {
    v = ch - '0';
  } else if (ch >= 'a' && ch <= 'f') {
    v = ch - 'a' + 10;
  } else if (ch >= 'A' && ch <= 'F') {
    v = ch - 'A' + 10;
  } else {
    return -1;
  }
  return v < base ? v : -1;
}

} // namespace

/* PostgreSQL's pg_strtoint32_safe, reporting failure by its result. */
bool parse_int32(const char *text, int32_t *out) {
  const char *p = text;
  bool neg = false;
  int base = 10;
  // Wide enough that a number just out of range does not wrap.
  uint64_t value = 0;

  while (isspace(static_cast<unsigned char>(*p))) {
    p++;
  }
  if (*p == '-') {
    p++;
    neg = true;
  } else if (*p == '+') {
    p++;
  }
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
  } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
    base = 8;
    p += 2;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
    base = 2;
    p += 2;
  }

  const char *firstdigit = p;
  for (;;) {
    int d = digit_value(*p, base);
    if (d >= 0) {
      value = value * base + d;
      if (value > 0x80000000ull) {
        return false;
      }
      p++;
    } else if (*p == '_') {
      // An underscore may not lead a decimal number, and must be
      // followed by a digit.
      if (base == 10 && p == firstdigit) {
        return false;
      }
      p++;
      if (digit_value(*p, base) < 0) {
        return false;
      }
    } else {
      break;
    }
  }
  if (p == firstdigit) {
    return false;
  }
  while (isspace(static_cast<unsigned char>(*p))) {
    p++;
  }
  if (*p != '\0') {
    return false;
  }
  if (!neg && value > 0x7fffffffull) {
    return false;
  }
  *out = neg ? static_cast<int32_t>(-static_cast<int64_t>(value))
             : static_cast<int32_t>(value);
  return true;
}

} // namespace sq
