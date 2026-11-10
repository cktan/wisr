/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * float.cc -- float4 and float8 (TYPE.md §3.3): IEEE-754 binary32
 * and binary64, little-endian. The text is PostgreSQL's, printed
 * by its own Ryu code in src/ryu/: nearly always the shortest
 * decimal that reads back to the same value, but not quite always,
 * so no other printer will do.
 */

#include "native.h"
#include "sm2.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

extern "C" {
#include "common/shortest_dec.h"
}

namespace {

/* The unsigned integer as wide as the float F. */
template <typename F>
using bits_t = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;

/* Print v as PostgreSQL does, with its own float4out. */
std::string float_text(float v) {
  char buf[FLOAT_SHORTEST_DECIMAL_LEN];
  return std::string(buf, float_to_shortest_decimal_bufn(v, buf));
}

/* Print v as PostgreSQL does, with its own float8out. */
std::string float_text(double v) {
  char buf[DOUBLE_SHORTEST_DECIMAL_LEN];
  return std::string(buf, double_to_shortest_decimal_bufn(v, buf));
}

/*
 * Parse PostgreSQL's canonical text for F: exactly what
 * float_text prints for the value it reads as, which rules out
 * every other spelling, and values out of F's range.
 */
template <typename F>
int float_in(const char *type, std::string_view text, std::string &datum) {
  F v;
  if (text == "NaN") {
    v = std::numeric_limits<F>::quiet_NaN();
  } else if (text == "Infinity" || text == "-Infinity") {
    v = text[0] == '-' ? -std::numeric_limits<F>::infinity()
                       : std::numeric_limits<F>::infinity();
  } else {
    // Read it, then insist it is what PostgreSQL would print.
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || end != text.data() + text.size() ||
        std::isnan(v) || std::isinf(v) || float_text(v) != text) {
      return sm2::bad_input(type, text);
    }
  }
  sm2::store_le(std::bit_cast<bits_t<F>>(v), datum);
  return 0;
}

/* Print an F datum as PostgreSQL does. */
template <typename F>
int float_out(const char *type, std::string_view datum, std::string &text) {
  if (datum.size() != sizeof(F)) {
    return sm2::bad_datum(type, datum, sizeof(F));
  }
  text = float_text(std::bit_cast<F>(sm2::load_le<bits_t<F>>(datum)));
  return 0;
}

/*
 * Order two F datums as PostgreSQL's float8_cmp_internal does:
 * NaN above everything and equal to every NaN, -0 equal to 0.
 */
template <typename F> int float_cmp(std::string_view a, std::string_view b) {
  F x = std::bit_cast<F>(sm2::load_le<bits_t<F>>(a));
  F y = std::bit_cast<F>(sm2::load_le<bits_t<F>>(b));
  if (std::isnan(x) || std::isnan(y)) {
    return std::isnan(x) - std::isnan(y);
  }
  return (x > y) - (x < y);
}

/*
 * The key of an F datum (TYPE.md §3.3), so memcmp orders it as
 * float8_cmp_internal.
 * 1. Fold -0 into 0, and every NaN into the one quiet NaN, as
 *    PostgreSQL calls them equal.
 * 2. Flip every bit of a negative value, and the sign bit of any
 *    other.
 * 3. Append the bits big-endian.
 */
template <typename F>
int float_key(const char *type, std::string_view datum, std::string &key) {
  using U = bits_t<F>;
  if (datum.size() != sizeof(F)) {
    return sm2::bad_datum(type, datum, sizeof(F));
  }
  F v = std::bit_cast<F>(sm2::load_le<U>(datum));
  U bits = std::isnan(v) ? std::bit_cast<U>(std::numeric_limits<F>::quiet_NaN())
           : v == F{0}   ? U{0}
                         : std::bit_cast<U>(v);
  const U sign = U{1} << (8 * sizeof(U) - 1);
  sm2::put_be(static_cast<U>(bits & sign ? ~bits : bits ^ sign), key);
  return 0;
}

/* Read float_key's bytes back into the datum the key keeps. */
template <typename F>
int float_unkey(const char *type, std::string_view &key, std::string &datum) {
  using U = bits_t<F>;
  U k;
  if (!sm2::get_be(key, k)) {
    return sm2::bad_key(type);
  }
  const U sign = U{1} << (8 * sizeof(U) - 1);
  sm2::store_le(static_cast<U>(k & sign ? k ^ sign : ~k), datum);
  return 0;
}

/* Parse a float4's canonical text. */
int float4_in(std::string_view t, int32_t, std::string &d) {
  return float_in<float>("float4", t, d);
}
/* Print a float4 datum. */
int float4_out(std::string_view d, int32_t, std::string &t) {
  return float_out<float>("float4", d, t);
}
/* Order two float4 datums. */
int float4_cmp(std::string_view a, std::string_view b, int32_t) {
  return float_cmp<float>(a, b);
}

/* A float4's key. */
int float4_key(std::string_view d, int32_t, const sm2::collation_t *,
               std::string &k) {
  return float_key<float>("float4", d, k);
}
/* Read a float4's key. */
int float4_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
                 std::string &d) {
  return float_unkey<float>("float4", k, d);
}

/* Parse a float8's canonical text. */
int float8_in(std::string_view t, int32_t, std::string &d) {
  return float_in<double>("float8", t, d);
}
/* Print a float8 datum. */
int float8_out(std::string_view d, int32_t, std::string &t) {
  return float_out<double>("float8", d, t);
}
/* Order two float8 datums. */
int float8_cmp(std::string_view a, std::string_view b, int32_t) {
  return float_cmp<double>(a, b);
}

/* A float8's key. */
int float8_key(std::string_view d, int32_t, const sm2::collation_t *,
               std::string &k) {
  return float_key<double>("float8", d, k);
}
/* Read a float8's key. */
int float8_unkey(std::string_view &k, int32_t, const sm2::collation_t *,
                 std::string &d) {
  return float_unkey<double>("float8", k, d);
}

} // namespace

namespace sm2 {

const type_t type_float4 = {.code = 5,
                            .name = "float4",
                            .len = 4,
                            .byval = true,
                            .align = 4,
                            .in = float4_in,
                            .out = float4_out,
                            .cmp = float4_cmp,
                            .key_encode = float4_key,
                            .key_decode = float4_unkey};

const type_t type_float8 = {.code = 6,
                            .name = "float8",
                            .len = 8,
                            .byval = true,
                            .align = 8,
                            .in = float8_in,
                            .out = float8_out,
                            .cmp = float8_cmp,
                            .key_encode = float8_key,
                            .key_decode = float8_unkey};

} // namespace sm2
