/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * int.cc -- int2, int4 and int8 (TYPE.md §3.2): two's complement,
 * little-endian, 2, 4 and 8 bytes; PostgreSQL prints them in
 * decimal.
 */

#include "native.h"
#include "sm2.h"

#include <charconv>
#include <cstdint>
#include <limits>

namespace {

/*
 * Parse PostgreSQL's canonical decimal for T: an optional minus
 * and digits, with no plus, no leading zero, no "-0" and no
 * spaces, in T's range.
 */
template <typename T>
int int_in(const char *type, std::string_view text, std::string &datum) {
  // The canonical form only: what PostgreSQL itself prints.
  size_t digits = text.size() - (text.starts_with('-') ? 1 : 0);
  std::string_view body = text.substr(text.size() - digits);
  if (digits == 0 || (body[0] == '0' && (digits > 1 || text[0] == '-'))) {
    return sm2::bad_input(type, text);
  }
  // Parse into T, which also rejects anything out of range.
  T v;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return sm2::bad_input(type, text);
  }
  sm2::store_le(v, datum);
  return 0;
}

/* Print a T datum in decimal. */
template <typename T>
int int_out(const char *type, std::string_view datum, std::string &text) {
  if (datum.size() != sizeof(T)) {
    return sm2::bad_datum(type, datum, sizeof(T));
  }
  text = std::to_string(sm2::load_le<T>(datum));
  return 0;
}

/* Order two T datums numerically. */
template <typename T> int int_cmp(std::string_view a, std::string_view b) {
  T x = sm2::load_le<T>(a), y = sm2::load_le<T>(b);
  return (x > y) - (x < y);
}

/* Parse an int2's canonical text. */
int int2_in(std::string_view t, int32_t, std::string &d) {
  return int_in<int16_t>("int2", t, d);
}
/* Print an int2 datum. */
int int2_out(std::string_view d, int32_t, std::string &t) {
  return int_out<int16_t>("int2", d, t);
}
/* Order two int2 datums. */
int int2_cmp(std::string_view a, std::string_view b, int32_t) {
  return int_cmp<int16_t>(a, b);
}
/* An int2's key. */
int int2_key(std::string_view d, int32_t, const sm2::collation_t *,
             std::string &k) {
  return sm2::int_key<int16_t, uint16_t>("int2", d, k);
}
/* Read an int2's key. */
int int2_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
               std::string &d) {
  return sm2::int_unkey<int16_t, uint16_t>("int2", k, d);
}

/* Parse an int4's canonical text. */
int int4_in(std::string_view t, int32_t, std::string &d) {
  return int_in<int32_t>("int4", t, d);
}
/* Print an int4 datum. */
int int4_out(std::string_view d, int32_t, std::string &t) {
  return int_out<int32_t>("int4", d, t);
}
/* Order two int4 datums. */
int int4_cmp(std::string_view a, std::string_view b, int32_t) {
  return int_cmp<int32_t>(a, b);
}
/* An int4's key. */
int int4_key(std::string_view d, int32_t, const sm2::collation_t *,
             std::string &k) {
  return sm2::int_key<int32_t, uint32_t>("int4", d, k);
}
/* Read an int4's key. */
int int4_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
               std::string &d) {
  return sm2::int_unkey<int32_t, uint32_t>("int4", k, d);
}

/* Parse an int8's canonical text. */
int int8_in(std::string_view t, int32_t, std::string &d) {
  return int_in<int64_t>("int8", t, d);
}
/* Print an int8 datum. */
int int8_out(std::string_view d, int32_t, std::string &t) {
  return int_out<int64_t>("int8", d, t);
}
/* Order two int8 datums. */
int int8_cmp(std::string_view a, std::string_view b, int32_t) {
  return int_cmp<int64_t>(a, b);
}
/* An int8's key. */
int int8_key(std::string_view d, int32_t, const sm2::collation_t *,
             std::string &k) {
  return sm2::int_key<int64_t, uint64_t>("int8", d, k);
}
/* Read an int8's key. */
int int8_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
               std::string &d) {
  return sm2::int_unkey<int64_t, uint64_t>("int8", k, d);
}

} // namespace

namespace sm2 {

const type_t type_int2 = {.code = 2,
                          .name = "int2",
                          .len = 2,
                          .byval = true,
                          .align = 2,
                          .in = int2_in,
                          .out = int2_out,
                          .cmp = int2_cmp,
                          .key_encode = int2_key,
                          .key_decode = int2_unkey};

const type_t type_int4 = {.code = 3,
                          .name = "int4",
                          .len = 4,
                          .byval = true,
                          .align = 4,
                          .in = int4_in,
                          .out = int4_out,
                          .cmp = int4_cmp,
                          .key_encode = int4_key,
                          .key_decode = int4_unkey};

const type_t type_int8 = {.code = 4,
                          .name = "int8",
                          .len = 8,
                          .byval = true,
                          .align = 8,
                          .in = int8_in,
                          .out = int8_out,
                          .cmp = int8_cmp,
                          .key_encode = int8_key,
                          .key_decode = int8_unkey};

} // namespace sm2
