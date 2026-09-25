/*
 * text.cc -- text, varchar and bytea (TYPE.md §3.5, §3.6): the
 * payload bytes alone, with no varlena header.
 *
 * text and varchar are the same bytes under two codes: valid
 * UTF-8 with no NUL, as a PostgreSQL UTF8 database holds, and
 * their own text. varchar(n)'s limit is a constraint sm3 enforces
 * (SM2_DESIGN.md §2), so its typmod does nothing here. Until the
 * collation work of step 2, cmp orders them bytewise, which is
 * codepoint order.
 *
 * bytea's text is PostgreSQL's with bytea_output hex: "\x" and
 * two lower-case hex digits a byte. It orders by memcmp, as
 * PostgreSQL does.
 */

#include "native.h"
#include "sm2.h"

#include <algorithm>
#include <cstring>

namespace {

/*
 * True if s is well-formed UTF-8 with no NUL: no overlong form,
 * no surrogate, nothing past U+10FFFF, as PostgreSQL's
 * pg_utf8_verifystr requires.
 */
bool valid_utf8(std::string_view s) {
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
  const unsigned char *end = p + s.size();
  while (p < end) {
    unsigned char c = *p;
    // One byte: ASCII, but not NUL.
    if (c < 0x80) {
      if (c == 0) {
        return false;
      }
      p++;
      continue;
    }
    // The lead byte fixes the length and the second byte's range.
    int n;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      n = 3;
      lo = c == 0xE0 ? 0xA0 : 0x80; // no overlong
      hi = c == 0xED ? 0x9F : 0xBF; // no surrogate
    } else if (c >= 0xF0 && c <= 0xF4) {
      n = 4;
      lo = c == 0xF0 ? 0x90 : 0x80; // no overlong
      hi = c == 0xF4 ? 0x8F : 0xBF; // nothing past U+10FFFF
    } else {
      return false;
    }
    if (end - p < n || p[1] < lo || p[1] > hi) {
      return false;
    }
    for (int i = 2; i < n; i++) {
      if (p[i] < 0x80 || p[i] > 0xBF) {
        return false;
      }
    }
    p += n;
  }
  return true;
}

/* Order two byte strings as memcmp would, the shorter first on a tie. */
int bytes_cmp(std::string_view a, std::string_view b, int32_t) {
  int c = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
  if (c != 0) {
    return c;
  }
  return (a.size() > b.size()) - (a.size() < b.size());
}

/* Parse type's text: the text itself, if it is valid UTF-8. */
int string_in(const char *type, std::string_view text, std::string &datum) {
  return sm2::catch_nomem([&] {
    // Not echoed into the message, which must stay valid text.
    if (!valid_utf8(text)) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         std::string("invalid input for type ") + type +
                             ": not valid UTF-8, or holds a NUL");
    }
    datum.assign(text);
    return 0;
  });
}

/* Print type's datum: the datum itself, if it is valid UTF-8. */
int string_out(const char *type, std::string_view datum, std::string &text) {
  return sm2::catch_nomem([&] {
    if (!valid_utf8(datum)) {
      return sm1::seterr(sm1::SM_ERR_INVALID, std::string("a ") + type +
                                                  " datum is not valid UTF-8");
    }
    text.assign(datum);
    return 0;
  });
}

/*
 * Fail with SM_ERR_INVALID unless collation is byte order: the
 * collations of step 2b are not built yet.
 */
int check_collation(const char *type, const sm2::collation_t *collation) {
  if (collation == nullptr) {
    return 0;
  }
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("type ") + type +
                         ": only byte order is supported yet, no collation");
}

/*
 * A byte string's key (TYPE.md §3.5, §3.6): the bytes escaped and
 * terminated, so memcmp orders keys as the strings and no key is a
 * prefix of another.
 */
int bytes_key(const char *type, std::string_view datum,
              const sm2::collation_t *collation, std::string &key) {
  return sm2::catch_nomem([&] {
    if (check_collation(type, collation) != 0) {
      return -1;
    }
    sm2::put_escaped(datum, key);
    return 0;
  });
}

/* Read bytes_key's bytes back into the datum. */
int bytes_unkey(const char *type, std::string_view &key,
                const sm2::collation_t *collation, std::string &datum) {
  return sm2::catch_nomem([&] {
    if (check_collation(type, collation) != 0) {
      return -1;
    }
    if (!sm2::get_escaped(key, datum)) {
      return sm2::bad_key(type);
    }
    return 0;
  });
}

/* Parse a text's text. */
int text_in(std::string_view t, int32_t, std::string &d) {
  return string_in("text", t, d);
}
/* Print a text datum. */
int text_out(std::string_view d, int32_t, std::string &t) {
  return string_out("text", d, t);
}

/* A text's key, under its collation. */
int text_key(std::string_view d, int32_t, const sm2::collation_t *c,
             std::string &k) {
  return bytes_key("text", d, c, k);
}
/* Read a text's key. */
int text_unkey(std::string_view &k, int32_t, const sm2::collation_t *c,
               std::string &d) {
  return bytes_unkey("text", k, c, d);
}

/* Parse a varchar's text; the typmod's limit is sm3's. */
int varchar_in(std::string_view t, int32_t, std::string &d) {
  return string_in("varchar", t, d);
}
/* Print a varchar datum. */
int varchar_out(std::string_view d, int32_t, std::string &t) {
  return string_out("varchar", d, t);
}

/* A varchar's key, under its collation. */
int varchar_key(std::string_view d, int32_t, const sm2::collation_t *c,
                std::string &k) {
  return bytes_key("varchar", d, c, k);
}
/* Read a varchar's key. */
int varchar_unkey(std::string_view &k, int32_t, const sm2::collation_t *c,
                  std::string &d) {
  return bytes_unkey("varchar", k, c, d);
}

/* A bytea's key: its bytes, escaped; a bytea has no collation. */
int bytea_key(std::string_view d, int32_t, const sm2::collation_t *,
              std::string &k) {
  return bytes_key("bytea", d, nullptr, k);
}
/* Read a bytea's key. */
int bytea_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
                std::string &d) {
  return bytes_unkey("bytea", k, nullptr, d);
}

/* The hex digit's value, or -1 if c is not a lower-case hex digit. */
int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

/* Parse a bytea's canonical text: "\x" and lower-case hex pairs. */
int bytea_in(std::string_view text, int32_t, std::string &datum) {
  return sm2::catch_nomem([&] {
    if (!text.starts_with("\\x") || text.size() % 2 != 0) {
      return sm2::bad_input("bytea", text);
    }
    std::string bytes;
    bytes.reserve(text.size() / 2 - 1);
    for (size_t i = 2; i < text.size(); i += 2) {
      int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
      if (hi < 0 || lo < 0) {
        return sm2::bad_input("bytea", text);
      }
      bytes.push_back(static_cast<char>(hi << 4 | lo));
    }
    datum = std::move(bytes);
    return 0;
  });
}

/* Print a bytea datum as "\x" and lower-case hex. */
int bytea_out(std::string_view datum, int32_t, std::string &text) {
  return sm2::catch_nomem([&] {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 + 2 * datum.size());
    out += "\\x";
    for (char c : datum) {
      unsigned char b = static_cast<unsigned char>(c);
      out += digits[b >> 4];
      out += digits[b & 0x0F];
    }
    text = std::move(out);
    return 0;
  });
}

} // namespace

namespace sm2 {

const type_t type_text = {.code = 8,
                          .name = "text",
                          .len = -1,
                          .byval = false,
                          .align = 4,
                          .in = text_in,
                          .out = text_out,
                          .cmp = bytes_cmp,
                          .key_encode = text_key,
                          .key_decode = text_unkey};

const type_t type_varchar = {.code = 9,
                             .name = "varchar",
                             .len = -1,
                             .byval = false,
                             .align = 4,
                             .in = varchar_in,
                             .out = varchar_out,
                             .cmp = bytes_cmp,
                             .key_encode = varchar_key,
                             .key_decode = varchar_unkey};

const type_t type_bytea = {.code = 10,
                           .name = "bytea",
                           .len = -1,
                           .byval = false,
                           .align = 4,
                           .in = bytea_in,
                           .out = bytea_out,
                           .cmp = bytes_cmp,
                           .key_encode = bytea_key,
                           .key_decode = bytea_unkey};

} // namespace sm2
