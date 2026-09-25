/*
 * uuid.cc -- uuid (TYPE.md §3.11): the 16 bytes as they are,
 * ordered by memcmp, as PostgreSQL orders them. The text is
 * PostgreSQL's: lower-case hex in groups of 8, 4, 4, 4 and 12,
 * joined by hyphens.
 */

#include "native.h"
#include "sm2.h"

#include <cstring>

namespace {

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

/* True where a hyphen goes in the 36-character text. */
bool is_hyphen_at(size_t i) { return i == 8 || i == 13 || i == 18 || i == 23; }

/* Parse a uuid's canonical text: exactly what uuid_out prints. */
int uuid_in(std::string_view text, int32_t, std::string &datum) {
  return sm2::catch_nomem([&] {
    std::string bytes;
    bool ok = text.size() == 36;
    for (size_t i = 0; ok && i < 36; i++) {
      if (is_hyphen_at(i)) {
        ok = text[i] == '-';
        continue;
      }
      int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
      ok = hi >= 0 && lo >= 0;
      bytes.push_back(static_cast<char>(hi << 4 | lo));
      i++;
    }
    if (!ok) {
      return sm2::bad_input("uuid", text);
    }
    datum = bytes;
    return 0;
  });
}

/* Print a uuid datum as uuid_out does. */
int uuid_out(std::string_view datum, int32_t, std::string &text) {
  return sm2::catch_nomem([&] {
    if (datum.size() != 16) {
      return sm2::bad_datum("uuid", datum, 16);
    }
    static const char digits[] = "0123456789abcdef";
    text.clear();
    for (size_t i = 0; i < 16; i++) {
      if (i == 4 || i == 6 || i == 8 || i == 10) {
        text += '-';
      }
      unsigned char b = static_cast<unsigned char>(datum[i]);
      text += digits[b >> 4];
      text += digits[b & 0x0F];
    }
    return 0;
  });
}

/* Order two uuids bytewise, as uuid_internal_cmp does. */
int uuid_cmp(std::string_view a, std::string_view b, int32_t) {
  return memcmp(a.data(), b.data(), 16);
}

/* A uuid's key: its 16 bytes, which memcmp already orders. */
int uuid_key(std::string_view datum, int32_t, const sm2::collation_t *,
             std::string &key) {
  return sm2::catch_nomem([&] {
    if (datum.size() != 16) {
      return sm2::bad_datum("uuid", datum, 16);
    }
    key += datum;
    return 0;
  });
}

/* Read a uuid's key. */
int uuid_unkey(std::string_view &key, int32_t, const sm2::collation_t *,
               std::string &datum) {
  return sm2::catch_nomem([&] {
    if (key.size() < 16) {
      return sm2::bad_key("uuid");
    }
    datum.assign(key.substr(0, 16));
    key.remove_prefix(16);
    return 0;
  });
}

} // namespace

namespace sm2 {

const type_t type_uuid = {.code = 16,
                          .name = "uuid",
                          .len = 16,
                          .byval = false,
                          .align = 1,
                          .in = uuid_in,
                          .out = uuid_out,
                          .cmp = uuid_cmp,
                          .key_encode = uuid_key,
                          .key_decode = uuid_unkey};

} // namespace sm2
