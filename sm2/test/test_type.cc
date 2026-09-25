/*
 * test_type.cc -- the type descriptors (SM2_DESIGN.md §2.3) and
 * the types built so far: bool, int2, int4 and int8 (TYPE.md
 * §3.1, §3.2).
 */

#include "alloc.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

/* v's little-endian bytes: what its datum must be. */
template <typename T> std::string le_bytes(T v) {
  std::string out(sizeof(T), '\0');
  for (size_t i = 0; i < sizeof(T); i++) {
    out[i] = static_cast<char>(static_cast<uint64_t>(v) >> (8 * i));
  }
  return out;
}

/* t's in on text, which must fail with SM_ERR_INVALID. */
void expect_bad_input(const sm2::type_t &t, std::string_view text) {
  CAPTURE(text);
  std::string datum;
  CHECK(t.in(text, -1, datum) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

/*
 * Check v's text and datum against each other through t: in
 * gives v's little-endian bytes, and out gives back the text.
 */
template <typename T> void check_round_trip(const sm2::type_t &t, T v) {
  std::string text = std::to_string(v), datum, back;
  CAPTURE(text);
  REQUIRE(t.in(text, -1, datum) == 0);
  CHECK(datum == le_bytes(v));
  REQUIRE(t.out(datum, -1, back) == 0);
  CHECK(back == text);
}

/* The unsigned integer as wide as the float F. */
template <typename F>
using bits_t = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;

/* The sign of an int, for comparing orders. */
int sign(int v) { return (v > 0) - (v < 0); }

/*
 * The integer-type checks: round trips over the boundaries and
 * generated values, the order over every pair of boundaries and
 * over generated pairs, and the non-canonical texts refused.
 */
template <typename T> void check_int_type(const sm2::type_t &t) {
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  for (T v : int_boundaries<T>()) {
    check_round_trip(t, v);
  }
  for (int i = 0; i < property_runs; i++) {
    check_round_trip(t, gen_int<T>(rng));
  }
  std::vector<T> b = int_boundaries<T>();
  for (size_t i = 0; i < b.size(); i++) {
    for (size_t j = 0; j < b.size(); j++) {
      CHECK(sign(t.cmp(le_bytes(b[i]), le_bytes(b[j]), -1)) ==
            sign(static_cast<int>(i) - static_cast<int>(j)));
    }
  }
  for (int i = 0; i < property_runs; i++) {
    T x = gen_int<T>(rng), y = gen_int<T>(rng);
    CHECK(sign(t.cmp(le_bytes(x), le_bytes(y), -1)) == (x > y) - (x < y));
  }
  for (std::string_view text :
       {"", "-", "+1", "01", "-0", "-01", " 1", "1 ", "1.0", "1e3", "abc"}) {
    expect_bad_input(t, text);
  }
  std::string text;
  CHECK(t.out(std::string(sizeof(T) + 1, '\0'), -1, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

/* PostgreSQL's order on floats: NaN last and equal to NaN. */
template <typename F> int pg_float_order(F x, F y) {
  if (std::isnan(x) || std::isnan(y)) {
    return std::isnan(x) - std::isnan(y);
  }
  return (x > y) - (x < y);
}

/*
 * The float-type checks: every boundary and generated value
 * prints and reads back to the same datum -- every NaN to the
 * one quiet NaN -- the order is PostgreSQL's, and the texts
 * PostgreSQL never prints are refused.
 */
template <typename F> void check_float_type(const sm2::type_t &t) {
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  std::vector<F> values = float_boundaries<F>();
  for (int i = 0; i < property_runs; i++) {
    values.push_back(gen_float<F>(rng));
  }
  for (F v : values) {
    std::string datum = le_bytes(std::bit_cast<bits_t<F>>(v)), text, back;
    REQUIRE(t.out(datum, -1, text) == 0);
    CAPTURE(text);
    REQUIRE(t.in(text, -1, back) == 0);
    F nan = std::numeric_limits<F>::quiet_NaN();
    CHECK(back ==
          (std::isnan(v) ? le_bytes(std::bit_cast<bits_t<F>>(nan)) : datum));
  }
  for (size_t i = 0; i + 1 < values.size(); i++) {
    F x = values[i], y = values[i + 1];
    CHECK(sign(t.cmp(le_bytes(std::bit_cast<bits_t<F>>(x)),
                     le_bytes(std::bit_cast<bits_t<F>>(y)), -1)) ==
          pg_float_order(x, y));
  }
  for (std::string_view text :
       {"",         "+1",        "1.0",   "01",   ".5",
        "5.",       "1e5",       "1E+16", "1e+5", " 1",
        "1 ",       "nan",       "NAN",   "-NaN", "inf",
        "infinity", "+Infinity", "0.10",  "-0.0", "0.1000000000000000055",
        "1e+400",   "1e-400",    "abc"}) {
    expect_bad_input(t, text);
  }
  std::string text;
  CHECK(t.out(std::string(sizeof(F) + 1, '\0'), -1, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

} // namespace

TEST_CASE("each type is found by its frozen code, and only by it") {
  // Verify the registry and each descriptor's fixed properties.
  // 1. Codes 1 to 6 and 8 to 16 find bool, the integers, the
  //    floats, text, varchar, bytea, date, time, the timestamps,
  //    interval and uuid, whose name, width (-1 for variable),
  //    by-value flag and alignment are PostgreSQL's.
  // 2. The slots not filled yet are null.
  // 3. Code 0, which is never a type, and codes not assigned yet
  //    find nothing.
  struct {
    uint8_t code;
    const char *name;
    int16_t len;
    bool byval;
    uint8_t align;
  } want[] = {{1, "bool", 1, true, 1},         {2, "int2", 2, true, 2},
              {3, "int4", 4, true, 4},         {4, "int8", 8, true, 8},
              {5, "float4", 4, true, 4},       {6, "float8", 8, true, 8},
              {8, "text", -1, false, 4},       {9, "varchar", -1, false, 4},
              {10, "bytea", -1, false, 4},     {11, "date", 4, true, 4},
              {12, "time", 8, true, 8},        {13, "timestamp", 8, true, 8},
              {14, "timestamptz", 8, true, 8}, {15, "interval", 16, false, 8},
              {16, "uuid", 16, false, 1}};
  for (auto &w : want) {
    CAPTURE(w.name);
    const sm2::type_t *t = sm2::type_by_code(w.code);
    REQUIRE(t);
    CHECK(t->code == w.code);
    CHECK(std::string(t->name) == w.name);
    CHECK(t->len == w.len);
    CHECK(t->byval == w.byval);
    CHECK(t->align == w.align);
    CHECK(t->in);
    CHECK(t->out);
    CHECK(t->cmp);
    CHECK_FALSE(t->hash);
    CHECK_FALSE(t->key_encode);
    CHECK_FALSE(t->key_decode);
    CHECK_FALSE(t->recv);
    CHECK_FALSE(t->send);
  }
  for (uint8_t code : {0, 7, 64, 255}) {
    CAPTURE(code);
    CHECK(sm2::type_by_code(code) == nullptr);
  }
}

TEST_CASE("bool is one byte, 0 or 1, printed t or f") {
  // Verify bool's representation, text and order.
  // 1. "t" and "f" parse to 1 and 0 and print back.
  // 2. false orders before true.
  // 3. Any other text, and a datum of another width or byte, is
  //    SM_ERR_INVALID.
  const sm2::type_t &t = sm2::type_bool;
  std::string d, text;
  REQUIRE(t.in("t", -1, d) == 0);
  CHECK(d == std::string(1, '\1'));
  REQUIRE(t.out(d, -1, text) == 0);
  CHECK(text == "t");
  REQUIRE(t.in("f", -1, d) == 0);
  CHECK(d == std::string(1, '\0'));
  REQUIRE(t.out(d, -1, text) == 0);
  CHECK(text == "f");

  CHECK(t.cmp(std::string(1, '\0'), std::string(1, '\1'), -1) < 0);
  CHECK(t.cmp(std::string(1, '\1'), std::string(1, '\0'), -1) > 0);
  CHECK(t.cmp(std::string(1, '\1'), std::string(1, '\1'), -1) == 0);

  for (std::string_view bad : {"", "true", "false", "T", "1", "0", "t "}) {
    expect_bad_input(t, bad);
  }
  CHECK(t.out(std::string(1, '\2'), -1, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(t.out(std::string(2, '\0'), -1, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("int2, int4 and int8 are little-endian two's complement") {
  // Verify each integer type by property, from test_seed().
  // 1. Every boundary and generated value parses to its
  //    little-endian bytes and prints back as the same decimal.
  // 2. The order agrees with the integers' own: over every pair
  //    of boundaries, and over generated pairs.
  // 3. Text PostgreSQL never prints -- a plus, a leading zero,
  //    "-0", spaces, a fraction -- and a datum of the wrong width
  //    are SM_ERR_INVALID.
  check_int_type<int16_t>(sm2::type_int2);
  check_int_type<int32_t>(sm2::type_int4);
  check_int_type<int64_t>(sm2::type_int8);
}

TEST_CASE("an integer outside its type's range is refused") {
  // Verify the range checks, one past each end of each type.
  // 1. int2, int4 and int8 each refuse one past their minimum and
  //    one past their maximum with SM_ERR_INVALID.
  expect_bad_input(sm2::type_int2, "-32769");
  expect_bad_input(sm2::type_int2, "32768");
  expect_bad_input(sm2::type_int4, "-2147483649");
  expect_bad_input(sm2::type_int4, "2147483648");
  expect_bad_input(sm2::type_int8, "-9223372036854775809");
  expect_bad_input(sm2::type_int8, "9223372036854775808");
}

TEST_CASE("float4 and float8 are IEEE-754, printed as PostgreSQL does") {
  // Verify each float type by property, from test_seed().
  // 1. Every boundary and generated datum prints, and its text
  //    reads back to the same bytes; a NaN reads back as the one
  //    quiet NaN PostgreSQL stores.
  // 2. The order is PostgreSQL's: NaN above everything and equal
  //    to any NaN, -0 equal to 0.
  // 3. Text PostgreSQL never prints -- other spellings of the
  //    specials, a plus, trailing zeros, digits beyond the
  //    shortest, an exponent it would print in fixed point -- and
  //    values out of range are SM_ERR_INVALID.
  check_float_type<float>(sm2::type_float4);
  check_float_type<double>(sm2::type_float8);
}

TEST_CASE("-0 and 0 are distinct datums that compare equal") {
  // Verify the zeros keep their sign but order together.
  // 1. "-0" and "0" parse to different bytes.
  // 2. cmp calls them equal, for both widths.
  for (const sm2::type_t *t : {&sm2::type_float4, &sm2::type_float8}) {
    CAPTURE(t->name);
    std::string neg, pos;
    REQUIRE(t->in("-0", -1, neg) == 0);
    REQUIRE(t->in("0", -1, pos) == 0);
    CHECK(neg != pos);
    CHECK(t->cmp(neg, pos, -1) == 0);
  }
}

TEST_CASE("running out of memory in a type's in or out is SM_ERR_NOMEM") {
  // Verify every type's in and out turn a bad_alloc into an error
  // (SM2_DESIGN.md §5), taking the allocation that reports bad
  // input, since a fixed-width datum and its text fit without one;
  // for bytea, whose every datum prints, the allocation of a long
  // text.
  // 1. With no allocation left, in on invalid text -- bytes that
  //    are not UTF-8, for text and varchar -- fails with
  //    SM_ERR_NOMEM, not SM_ERR_INVALID, and throws nothing.
  // 2. So does out on a datum of the wrong width or not UTF-8, or
  //    on a 16-byte bytea.
  // 3. With the budget restored, the same calls report
  //    SM_ERR_INVALID again, and the bytea prints.
  for (int code = 1; code < 256; code++) {
    const sm2::type_t *t = sm2::type_by_code(code);
    if (!t) {
      continue;
    }
    CAPTURE(t->name);
    bool utf8 = t == &sm2::type_text || t == &sm2::type_varchar;
    bool bytea = t == &sm2::type_bytea;
    std::string bad_text = utf8 ? "\xff" : "not a value";
    std::string bad_datum = utf8    ? "\xff"
                            : bytea ? std::string(16, '\0')
                                    : std::string(t->len + 1, '\0');
    std::string datum, text;
    t_alloc_budget = 0;
    int in_rc = t->in(bad_text, -1, datum);
    sm1::errnum_t in_err = sm1::errnum();
    int out_rc = t->out(bad_datum, -1, text);
    sm1::errnum_t out_err = sm1::errnum();
    t_alloc_budget = -1;
    CHECK(in_rc == -1);
    CHECK(in_err == sm1::SM_ERR_NOMEM);
    CHECK(out_rc == -1);
    CHECK(out_err == sm1::SM_ERR_NOMEM);
    CHECK(t->in(bad_text, -1, datum) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    if (bytea) {
      CHECK(t->out(bad_datum, -1, text) == 0);
    } else {
      CHECK(t->out(bad_datum, -1, text) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
}

namespace {

/* PostgreSQL's datetime bounds, as datums (TYPE.md §3.7-§3.9). */
constexpr int32_t date_min = -2451545;          /* 4714-11-24 BC */
constexpr int32_t date_max = 2145031948;        /* 5874897-12-31 */
constexpr int64_t ts_min = -211813488000000000; /* 4714-11-24 BC */
constexpr int64_t ts_max = 9223371331199999999; /* 294276-12-31 ... */
constexpr int64_t usecs_per_day = 86400000000;

/*
 * Check a datetime type by property: every datum in [lo, hi], and
 * the extra ones given, prints and reads back to itself, and cmp
 * is the integers' order. Datums are drawn at the bounds, near
 * them, and anywhere between.
 */
template <typename T>
void check_datetime(const sm2::type_t &t, T lo, T hi, std::vector<T> extra) {
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  std::vector<T> values = extra;
  for (T v : {lo, static_cast<T>(lo + 1), T{0}, static_cast<T>(hi - 1), hi}) {
    values.push_back(v);
  }
  uint64_t span = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1;
  for (int i = 0; i < property_runs; i++) {
    values.push_back(static_cast<T>(static_cast<uint64_t>(lo) + rng() % span));
  }
  for (T v : values) {
    std::string text, back;
    REQUIRE(t.out(le_bytes(v), -1, text) == 0);
    CAPTURE(text);
    REQUIRE(t.in(text, -1, back) == 0);
    CHECK(back == le_bytes(v));
  }
  for (size_t i = 0; i + 1 < values.size(); i++) {
    T x = values[i], y = values[i + 1];
    CHECK(sign(t.cmp(le_bytes(x), le_bytes(y), -1)) == (x > y) - (x < y));
  }
}

/* t's out on the datum v, which must fail with SM_ERR_INVALID. */
template <typename T> void expect_bad_datum(const sm2::type_t &t, T v) {
  CAPTURE(v);
  std::string text;
  CHECK(t.out(le_bytes(v), -1, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

} // namespace

TEST_CASE("date, time and the timestamps round trip over their ranges") {
  // Verify each datetime type by property, from test_seed().
  // 1. Every datum in PostgreSQL's range, the infinities included
  //    where the type has them, prints and reads back to itself.
  // 2. cmp is the order of the underlying integers, so the
  //    infinities sort at the ends.
  const int32_t dinf = std::numeric_limits<int32_t>::max();
  const int64_t tinf = std::numeric_limits<int64_t>::max();
  check_datetime<int32_t>(sm2::type_date, date_min, date_max,
                          {-dinf - 1, dinf});
  check_datetime<int64_t>(sm2::type_time, 0, usecs_per_day, {});
  check_datetime<int64_t>(sm2::type_timestamp, ts_min, ts_max,
                          {-tinf - 1, tinf});
  check_datetime<int64_t>(sm2::type_timestamptz, ts_min, ts_max,
                          {-tinf - 1, tinf});
}

TEST_CASE("datetime text and datums outside PostgreSQL's range are refused") {
  // Verify the range checks, one step past each end.
  // 1. in refuses the day before the first date and after the
  //    last, the microsecond before the first timestamp and the
  //    first one past the last, and a time past 24:00:00.
  // 2. out refuses the datums one past each end.
  expect_bad_input(sm2::type_date, "4714-11-23 BC");
  expect_bad_input(sm2::type_date, "5874898-01-01");
  expect_bad_input(sm2::type_time, "24:00:00.000001");
  expect_bad_input(sm2::type_timestamp, "4714-11-23 23:59:59.999999 BC");
  expect_bad_input(sm2::type_timestamp, "294277-01-01 00:00:00");
  expect_bad_input(sm2::type_timestamptz, "294277-01-01 00:00:00+00");

  expect_bad_datum<int32_t>(sm2::type_date, date_min - 1);
  expect_bad_datum<int32_t>(sm2::type_date, date_max + 1);
  expect_bad_datum<int64_t>(sm2::type_time, -1);
  expect_bad_datum<int64_t>(sm2::type_time, usecs_per_day + 1);
  expect_bad_datum<int64_t>(sm2::type_timestamp, ts_min - 1);
  expect_bad_datum<int64_t>(sm2::type_timestamp, ts_max + 1);
  expect_bad_datum<int64_t>(sm2::type_timestamptz, ts_max + 1);
}

TEST_CASE("only PostgreSQL's ISO text is datetime input") {
  // Verify in refuses every spelling PostgreSQL does not print
  // under DateStyle ISO and, for timestamptz, TimeZone UTC.
  // 1. Dates: impossible days, year 0, unpadded or overpadded
  //    fields, other words for infinity, stray text.
  // 2. Times: trailing zeros, missing seconds, an hour past 24.
  // 3. Timestamps: a T, 24:00:00, BC in the wrong place, and a
  //    zone on a timestamp.
  // 4. Timestamptz: no zone, or any zone but +00.
  for (std::string_view text :
       {"", "2000-02-30", "2001-02-29", "0000-01-01", "2000-1-01", "2000-01-1",
        "02000-01-01", "200-01-01", " 2000-01-01", "2000-01-01 ",
        "2000-01-01 AD", "2000-01-01 bc", "Infinity", "+infinity", "-Infinity",
        "today", "2000-01-01 00:00:00"}) {
    CAPTURE(text);
    expect_bad_input(sm2::type_date, text);
  }
  for (std::string_view text :
       {"", "12:00:00.10", "12:00", "12:00:00.", "25:00:00", "12:60:00",
        "12:00:60", "1:00:00", "12:00:00.1234567", "allballs", "12:00:00+00"}) {
    CAPTURE(text);
    expect_bad_input(sm2::type_time, text);
  }
  for (std::string_view text :
       {"2000-01-01T00:00:00", "2000-01-01 24:00:00", "2000-01-01 00:00",
        "2000-01-01", "2000-01-01 BC 00:00:00", "2000-01-01 00:00:00+00",
        "2000-01-01 00:00:00.500"}) {
    CAPTURE(text);
    expect_bad_input(sm2::type_timestamp, text);
  }
  for (std::string_view text :
       {"2000-01-01 00:00:00", "2000-01-01 00:00:00+01",
        "2000-01-01 00:00:00-00", "2000-01-01 00:00:00Z",
        "2000-01-01 00:00:00+00:00", "2000-01-01 00:00:00 BC+00"}) {
    CAPTURE(text);
    expect_bad_input(sm2::type_timestamptz, text);
  }
}

TEST_CASE("a precision typmod bounds the fraction digits a text carries") {
  // Verify time(p), timestamp(p) and timestamptz(p) input.
  // 1. With p = 3, text with three fraction digits is accepted and
  //    text with four is refused.
  // 2. With p = 0, only whole seconds are accepted.
  // 3. A typmod outside -1..6 is SM_ERR_INVALID, whatever the text.
  struct {
    const sm2::type_t *t;
    const char *three, *four, *whole;
  } cases[] = {
      {&sm2::type_time, "12:00:00.123", "12:00:00.1234", "12:00:00"},
      {&sm2::type_timestamp, "2000-01-01 12:00:00.123",
       "2000-01-01 12:00:00.1234", "2000-01-01 12:00:00"},
      {&sm2::type_timestamptz, "2000-01-01 12:00:00.123+00",
       "2000-01-01 12:00:00.1234+00", "2000-01-01 12:00:00+00"},
  };
  for (auto &c : cases) {
    CAPTURE(c.t->name);
    std::string datum;
    CHECK(c.t->in(c.three, 3, datum) == 0);
    CHECK(c.t->in(c.four, 3, datum) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(c.t->in(c.whole, 0, datum) == 0);
    CHECK(c.t->in(c.three, 0, datum) == -1);
    for (int32_t typmod : {-2, 7, 1000}) {
      CAPTURE(typmod);
      CHECK(c.t->in(c.whole, typmod, datum) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
}

namespace {

/* An interval datum from its components. */
std::string interval_datum(int64_t time, int32_t day, int32_t month) {
  return le_bytes(time) + le_bytes(day) + le_bytes(month);
}

/* PostgreSQL's interval order, flattened at 30 days to the month. */
__int128 interval_span(int64_t time, int32_t day, int32_t month) {
  return static_cast<__int128>(time) +
         static_cast<__int128>(int64_t{month} * 30 + day) * 86400000000LL;
}

/* An interval typmod: a field range and a precision. */
int32_t interval_typmod(int range, int precision) {
  return (range << 16) | precision;
}

} // namespace

TEST_CASE("every interval datum round trips, ordered as PostgreSQL orders") {
  // Verify interval by property, from test_seed().
  // 1. Every datum -- the infinities, the components' extremes,
  //    small components and any 16 bytes at all -- prints and
  //    reads back to itself.
  // 2. cmp agrees with PostgreSQL's flattened order.
  // 3. '1 mon' and '30 days' are distinct datums that cmp calls
  //    equal, as PostgreSQL does.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  using i32 = std::numeric_limits<int32_t>;
  using i64 = std::numeric_limits<int64_t>;
  struct parts_t {
    int64_t time;
    int32_t day, month;
  };
  std::vector<parts_t> values = {{i64::max(), i32::max(), i32::max()},
                                 {i64::min(), i32::min(), i32::min()},
                                 {0, 0, 0},
                                 {i64::max(), 0, 0},
                                 {i64::min(), 0, 0},
                                 {0, i32::max(), i32::min()},
                                 {i64::max() - 1, i32::max(), i32::max()},
                                 {-500000, 0, 0},
                                 {1, -1, 1}};
  for (int i = 0; i < property_runs; i++) {
    if (i % 2) {
      values.push_back({gen_int<int64_t>(rng), gen_int<int32_t>(rng),
                        gen_int<int32_t>(rng)});
    } else {
      values.push_back({static_cast<int64_t>(rng() % 7200000001) - 3600000000,
                        static_cast<int32_t>(rng() % 61) - 30,
                        static_cast<int32_t>(rng() % 49) - 24});
    }
  }
  const sm2::type_t &t = sm2::type_interval;
  for (const parts_t &v : values) {
    std::string datum = interval_datum(v.time, v.day, v.month), text, back;
    REQUIRE(t.out(datum, -1, text) == 0);
    CAPTURE(text);
    REQUIRE(t.in(text, -1, back) == 0);
    CHECK(back == datum);
  }
  for (size_t i = 0; i + 1 < values.size(); i++) {
    const parts_t &x = values[i], &y = values[i + 1];
    __int128 sx = interval_span(x.time, x.day, x.month);
    __int128 sy = interval_span(y.time, y.day, y.month);
    CHECK(sign(t.cmp(interval_datum(x.time, x.day, x.month),
                     interval_datum(y.time, y.day, y.month), -1)) ==
          (sx > sy) - (sx < sy));
  }
  std::string mon, days;
  REQUIRE(t.in("P1M", -1, mon) == 0);
  REQUIRE(t.in("P30D", -1, days) == 0);
  CHECK(mon != days);
  CHECK(t.cmp(mon, days, -1) == 0);
}

TEST_CASE("only PostgreSQL's ISO 8601 text is interval input") {
  // Verify in refuses what PostgreSQL does not print under
  // IntervalStyle iso_8601.
  // 1. Other styles, lower case, and empty or zero spellings but
  //    PT0S.
  // 2. Fields out of order, repeated, or that PostgreSQL would
  //    carry: 12 months, 60 minutes, a year and month of opposite
  //    signs.
  // 3. Seconds with trailing zeros or seven digits, a sign
  //    written as plus, and weeks.
  // 4. Components beyond their fields' ranges.
  for (std::string_view text : {"",
                                "P",
                                "PT",
                                "P0D",
                                "PT-0S",
                                "0",
                                "1 mon",
                                "@ 1 mon",
                                "p1d",
                                "P1d",
                                "INFINITY",
                                "+infinity",
                                "P1D1M",
                                "P1M1Y",
                                "P1Y1Y",
                                "PT1S1M",
                                "P1Y12M",
                                "PT1H60M",
                                "PT60S",
                                "P1Y-2M",
                                "PT1.50S",
                                "PT1.1234567S",
                                "P+1D",
                                "P1W",
                                "P1DT",
                                "PT.5S",
                                "P1.5D",
                                "PT1.5H",
                                "P178956970Y8M",
                                "P2147483648D",
                                "PT2562047788H54.775808S",
                                "P1D "}) {
    CAPTURE(text);
    expect_bad_input(sm2::type_interval, text);
  }
}

TEST_CASE("an interval typmod keeps only what it would not change") {
  // Verify interval(p) and the field ranges, as PostgreSQL's
  // AdjustIntervalForTypmod applies them.
  // 1. YEAR accepts whole years only.
  // 2. HOUR and DAY TO MINUTE refuse what lies below their last
  //    field.
  // 3. DAY TO SECOND(3) accepts three fraction digits, not four.
  // 4. A typmod with no precision takes six digits.
  // 5. The infinities pass any valid typmod.
  // 6. An unknown range, a precision past 6 and a typmod below -1
  //    are SM_ERR_INVALID, even for infinity.
  const sm2::type_t &t = sm2::type_interval;
  const int year = 1 << 2, month = 1 << 1, day = 1 << 3, hour = 1 << 10,
            minute = 1 << 11, second = 1 << 12, full = 0xFFFF;
  struct {
    int32_t typmod;
    const char *text;
    int rc;
  } cases[] = {
      {interval_typmod(year, full), "P2Y", 0},
      {interval_typmod(year, full), "P2Y1M", -1},
      {interval_typmod(year, full), "P2Y1D", -1},
      {interval_typmod(year | month, full), "P2Y1M", 0},
      {interval_typmod(year | month, full), "P2Y1MT1H", -1},
      {interval_typmod(hour, full), "P1DT2H", 0},
      {interval_typmod(hour, full), "PT2H1M", -1},
      {interval_typmod(day | hour | minute, full), "P1DT2H3M", 0},
      {interval_typmod(day | hour | minute, full), "PT3M1S", -1},
      {interval_typmod(day | hour | minute | second, 3), "P1DT1.123S", 0},
      {interval_typmod(day | hour | minute | second, 3), "P1DT1.1234S", -1},
      {interval_typmod(0x7FFF, full), "PT1.123456S", 0},
      {interval_typmod(0x7FFF, 0), "PT1S", 0},
      {interval_typmod(0x7FFF, 0), "PT1.5S", -1},
      {interval_typmod(year, full), "infinity", 0},
      {interval_typmod(hour, 0), "-infinity", 0},
  };
  for (auto &c : cases) {
    CAPTURE(c.typmod);
    CAPTURE(c.text);
    std::string datum;
    CHECK(t.in(c.text, c.typmod, datum) == c.rc);
    if (c.rc != 0) {
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
  for (int32_t typmod :
       {interval_typmod(0x1234, full), interval_typmod(0x7FFF, 7), -2}) {
    CAPTURE(typmod);
    std::string datum;
    for (const char *text : {"PT1S", "infinity"}) {
      CHECK(t.in(text, typmod, datum) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
}

TEST_CASE("a uuid is its 16 bytes, ordered bytewise") {
  // Verify uuid by property, from test_seed().
  // 1. All-zero, all-one and random datums print and read back
  //    to themselves.
  // 2. cmp is memcmp's order, the high bit included.
  // 3. in refuses what uuid_out never prints: upper case, braces,
  //    no or misplaced hyphens, a wrong length, non-hex digits.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  const sm2::type_t &t = sm2::type_uuid;
  std::vector<std::string> values = {std::string(16, '\0'),
                                     std::string(16, '\xff')};
  for (int i = 0; i < property_runs; i++) {
    std::string v = le_bytes(rng()) + le_bytes(rng());
    if (i % 4 == 0) {
      v.replace(8, 8, values[i % 2 == 0].substr(0, 8));
    }
    values.push_back(v);
  }
  for (const std::string &v : values) {
    std::string text, back;
    REQUIRE(t.out(v, -1, text) == 0);
    CAPTURE(text);
    REQUIRE(t.in(text, -1, back) == 0);
    CHECK(back == v);
  }
  for (size_t i = 0; i + 1 < values.size(); i++) {
    int want = sign(memcmp(values[i].data(), values[i + 1].data(), 16));
    CHECK(sign(t.cmp(values[i], values[i + 1], -1)) == want);
  }
  for (std::string_view text : {"", "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11",
                                "{a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11}",
                                "a0eebc999c0b4ef8bb6d6bb9bd380a11",
                                "a0eebc99-9c0b4ef8-bb6d-6bb9bd380a11",
                                "a0eebc9-99c0b-4ef8-bb6d-6bb9bd380a11",
                                "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a1",
                                "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a111",
                                "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a1g",
                                " a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"}) {
    CAPTURE(text);
    expect_bad_input(t, text);
  }
}

namespace {

/* A code point's UTF-8 bytes. */
std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | cp >> 6);
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | cp >> 12);
    s += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | cp >> 18);
    s += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
    s += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}

/*
 * A random valid UTF-8 string of up to 20 code points: mostly
 * ASCII, with every length of sequence, the edges of each range
 * included.
 */
std::string gen_utf8(std::mt19937_64 &rng) {
  static const uint32_t edges[] = {0x01,   0x7F,   0x80,   0x7FF,   0x800,
                                   0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF};
  std::string s;
  for (size_t n = rng() % 21; n > 0; n--) {
    uint32_t cp;
    switch (rng() % 4) {
    case 0:
      cp = edges[rng() % std::size(edges)];
      break;
    case 1:
      do {
        cp = 1 + rng() % 0x10FFFF;
      } while (cp >= 0xD800 && cp <= 0xDFFF);
      break;
    default:
      cp = 0x20 + rng() % 0x5F;
    }
    s += utf8(cp);
  }
  return s;
}

/* memcmp's order on byte strings, the shorter first on a tie. */
int bytes_order(const std::string &a, const std::string &b) {
  return sign(a.compare(b));
}

} // namespace

TEST_CASE("text and varchar hold valid UTF-8, ordered bytewise") {
  // Verify text and varchar by property, from test_seed().
  // 1. Every generated string, the empty one included, is its own
  //    text and datum, both ways.
  // 2. cmp is bytewise, the shorter first on a tie, which is
  //    codepoint order.
  // 3. in and out refuse bytes that are not UTF-8 -- overlong
  //    forms, surrogates, past U+10FFFF, truncated sequences,
  //    stray continuation bytes -- and a NUL.
  INFO("seed ", test_seed());
  const std::string_view bad[] = {"\xff",
                                  "\x80",
                                  "\xc0\x80",
                                  "\xc1\xbf",
                                  "\xe0\x80\x80",
                                  "\xed\xa0\x80",
                                  "\xed\xbf\xbf",
                                  "\xf0\x80\x80\x80",
                                  "\xf4\x90\x80\x80",
                                  "\xf5\x80\x80\x80",
                                  "\xe2\x82",
                                  "a\xe2\x82",
                                  "\xe2\x28\xa1",
                                  std::string_view("a\0b", 3),
                                  std::string_view("\0", 1)};
  for (const sm2::type_t *t : {&sm2::type_text, &sm2::type_varchar}) {
    CAPTURE(t->name);
    std::mt19937_64 rng(test_seed());
    std::vector<std::string> values = {"", "a", "ab", "b"};
    for (int i = 0; i < property_runs; i++) {
      values.push_back(gen_utf8(rng));
    }
    for (const std::string &v : values) {
      std::string datum, text;
      REQUIRE(t->in(v, -1, datum) == 0);
      CHECK(datum == v);
      REQUIRE(t->out(datum, -1, text) == 0);
      CHECK(text == v);
    }
    for (size_t i = 0; i + 1 < values.size(); i++) {
      CHECK(sign(t->cmp(values[i], values[i + 1], -1)) ==
            bytes_order(values[i], values[i + 1]));
    }
    for (std::string_view b : bad) {
      std::string datum, text;
      CHECK(t->in(b, -1, datum) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
      CHECK(t->out(b, -1, text) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
}

TEST_CASE("bytea is any bytes, printed as hex, ordered bytewise") {
  // Verify bytea by property, from test_seed().
  // 1. Every generated byte string, empty and NUL-bearing ones
  //    included, prints and reads back to itself.
  // 2. cmp is bytewise, the shorter first on a tie.
  // 3. in refuses what bytea_out never prints: upper case, the
  //    escape format, a missing or odd hex digit, spaces.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  const sm2::type_t &t = sm2::type_bytea;
  std::vector<std::string> values = {"", std::string(1, '\0'), "\xff",
                                     std::string(2, '\0')};
  for (int i = 0; i < property_runs; i++) {
    std::string v;
    for (size_t n = rng() % 24; n > 0; n--) {
      v += static_cast<char>(rng() % 4 == 0 ? 0 : rng());
    }
    values.push_back(v);
  }
  for (const std::string &v : values) {
    std::string text, back;
    REQUIRE(t.out(v, -1, text) == 0);
    CAPTURE(text);
    REQUIRE(t.in(text, -1, back) == 0);
    CHECK(back == v);
  }
  for (size_t i = 0; i + 1 < values.size(); i++) {
    CHECK(sign(t.cmp(values[i], values[i + 1], -1)) ==
          bytes_order(values[i], values[i + 1]));
  }
  for (std::string_view text :
       {"", "x00", "\\X00", "\\x0", "\\x0g", "\\xFF", "\\x 00", "\\x00 ",
        "\\000", "abc", "\\\\x00"}) {
    CAPTURE(text);
    expect_bad_input(t, text);
  }
}
