/*
 * test_type.cc -- the type descriptors (TYPE.md §5.3) and
 * the types built so far: bool, int2, int4 and int8 (TYPE.md
 * §3.1, §3.2).
 */

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
#include <tuple>
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
  // 1. Codes 1 to 18 find bool, the integers, the floats, text,
  //    varchar, bytea, date, time, the timestamps, interval, uuid,
  //    decimal64 and decimal128, whose name, width (-1 for
  //    variable) and by-value flag are PostgreSQL's, and whose
  //    alignment is too, the decimals' aside, which PostgreSQL
  //    does not have.
  // 2. Each has its key functions, only the decimals check their
  //    typmod, and the slots not filled yet -- hash, recv, send --
  //    are null.
  // 3. Code 0, which is never a type, code 7, decimal's before it
  //    was split and now retired, and codes not assigned yet find
  //    nothing.
  struct {
    uint8_t code;
    const char *name;
    int16_t len;
    bool byval;
    uint8_t align;
  } want[] = {{1, "bool", 1, true, 1},          {2, "int2", 2, true, 2},
              {3, "int4", 4, true, 4},          {4, "int8", 8, true, 8},
              {5, "float4", 4, true, 4},        {6, "float8", 8, true, 8},
              {8, "text", -1, false, 4},        {9, "varchar", -1, false, 4},
              {10, "bytea", -1, false, 4},      {11, "date", 4, true, 4},
              {12, "time", 8, true, 8},         {13, "timestamp", 8, true, 8},
              {14, "timestamptz", 8, true, 8},  {15, "interval", 16, false, 8},
              {16, "uuid", 16, false, 1},       {17, "decimal64", 8, true, 8},
              {18, "decimal128", 16, false, 8}, {19, "bpchar", -1, false, 4},
              {20, "name", 64, false, 1}};
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
    CHECK(t->key_encode);
    // A bpchar's key gives no value back: the row has its padding.
    CHECK((t->key_decode != nullptr) == (t != &sm2::type_bpchar));
    CHECK((t->typmod_valid != nullptr) ==
          (t == &sm2::type_decimal64 || t == &sm2::type_decimal128));
    CHECK_FALSE(t->hash);
    CHECK_FALSE(t->recv);
    CHECK_FALSE(t->send);
  }
  for (uint8_t code : {0, 7, 21, 64, 255}) {
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

namespace {

/* PostgreSQL's datetime bounds, as datums (TYPE.md §3.7, §3.9). */
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

TEST_CASE("bpchar is text whose trailing spaces count for nothing") {
  // Verify bpchar, PostgreSQL's char(n) (TYPE.md §3.5).
  // 1. A value is kept and printed as given, its padding and all.
  // 2. cmp leaves trailing spaces out: 'a' and 'a  ' are equal, and
  //    'a ' orders before 'ab' as 'a' does.
  // 3. 'a' and 'a  ' have one key; keys order as the values without
  //    their trailing spaces, under no collation.
  // 4. A key gives no value back, having no key_decode: an index scan
  //    reads the row, which has the padding.
  const sm2::type_t *t = &sm2::type_bpchar;
  std::string datum, text;
  REQUIRE(t->in("ab   ", -1, datum) == 0);
  CHECK(datum == "ab   ");
  REQUIRE(t->out(datum, -1, text) == 0);
  CHECK(text == "ab   ");
  CHECK(t->cmp("a", "a  ", -1) == 0);
  CHECK(t->cmp("a ", "ab", -1) < 0);
  CHECK(t->cmp("b", "a   ", -1) > 0);
  CHECK(t->cmp(" a", "a", -1) < 0);
  auto key = [&](std::string_view v) {
    std::string k;
    REQUIRE(t->key_encode(v, -1, nullptr, k) == 0);
    return k;
  };
  CHECK(key("a") == key("a  "));
  CHECK(key("") == key("   "));
  CHECK(key("a ") < key("ab"));
  CHECK(key("ab  ") < key("b"));
  CHECK(t->key_decode == nullptr);
  sm2::key_column_t col{};
  col.type = t;
  col.typmod = -1;
  CHECK_FALSE(sm2::key_decodable(std::span<const sm2::key_column_t>(&col, 1)));
}

TEST_CASE("name is PostgreSQL's 64 bytes, NUL-padded, ordered bytewise") {
  // Verify name, PostgreSQL's NameData (TYPE.md §3.5).
  // 1. A value's datum is its bytes and NULs to 64, as PostgreSQL
  //    stores it; out prints the bytes before the NULs.
  // 2. The empty name and one of 63 bytes are taken; one of 64, and
  //    one not UTF-8, are refused, as is a datum not 64 bytes.
  // 3. cmp orders by the bytes before the NULs, a prefix first.
  // 4. Keys order as the values, and decode to the 64-byte datum.
  const sm2::type_t *t = &sm2::type_name;
  std::string datum, text;
  REQUIRE(t->in("pg_class", -1, datum) == 0);
  CHECK(datum == std::string("pg_class") + std::string(56, '\0'));
  REQUIRE(t->out(datum, -1, text) == 0);
  CHECK(text == "pg_class");
  REQUIRE(t->in("", -1, datum) == 0);
  CHECK(datum == std::string(64, '\0'));
  REQUIRE(t->in(std::string(63, 'x'), -1, datum) == 0);
  CHECK(datum == std::string(63, 'x') + '\0');
  CHECK(t->in(std::string(64, 'x'), -1, datum) != 0);
  CHECK(t->in("\xff", -1, datum) != 0);
  CHECK(t->out("short", -1, text) != 0);
  auto name = [&](std::string_view v) {
    std::string d;
    REQUIRE(t->in(v, -1, d) == 0);
    return d;
  };
  CHECK(t->cmp(name("a"), name("a"), -1) == 0);
  CHECK(t->cmp(name("a"), name("ab"), -1) < 0);
  CHECK(t->cmp(name("B"), name("a"), -1) < 0);
  CHECK(t->cmp(name("é"), name("z"), -1) > 0);
  auto key = [&](std::string_view v) {
    std::string k;
    REQUIRE(t->key_encode(name(v), -1, nullptr, k) == 0);
    return k;
  };
  CHECK(key("") < key("a"));
  CHECK(key("a") < key("ab"));
  CHECK(key("ab") < key("b"));
  std::string k = key("tenk1"), back;
  std::string_view rest = k;
  REQUIRE(t->key_decode(rest, -1, nullptr, back) == 0);
  CHECK(rest.empty());
  CHECK(back == name("tenk1"));
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

namespace {

/* decimal's typmod for (p, s), as PostgreSQL encodes numeric's. */
int32_t decimal_typmod(int p, int s) { return ((p << 16) | s) + 4; }

/*
 * A random decimal text at scale s with up to p digits: its
 * sign, integer part and exactly s fraction digits.
 */
std::string gen_decimal_text(std::mt19937_64 &rng, int p, int s) {
  std::string digits;
  for (size_t n = 1 + rng() % p; n > 0; n--) {
    digits += static_cast<char>('0' + rng() % 10);
  }
  digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size()));
  digits.insert(
      0, s + 1 > static_cast<int>(digits.size()) ? s + 1 - digits.size() : 0,
      '0');
  std::string text = digits.substr(0, digits.size() - s);
  if (s > 0) {
    text += "." + digits.substr(digits.size() - s);
  }
  bool zero = text.find_first_not_of("0.") == std::string::npos;
  return !zero && rng() % 2 ? "-" + text : text;
}

/* The order of two decimal texts at one scale, as values. */
int decimal_text_order(const std::string &a, const std::string &b) {
  bool na = a[0] == '-', nb = b[0] == '-';
  if (na != nb) {
    return na ? -1 : 1;
  }
  std::string ma = a.substr(na), mb = b.substr(nb);
  int mag = ma.size() != mb.size() ? (ma.size() < mb.size() ? -1 : 1)
                                   : sign(ma.compare(mb));
  return na ? -mag : mag;
}

} // namespace

TEST_CASE("decimal64 and decimal128 round trip, in integer order") {
  // Verify both decimals by property, from test_seed(), for
  // precisions up to each one's limit, decimal128's also below 17.
  // 1. Every generated text of at most p digits at scale s reads
  //    into a datum of the type's width and prints back.
  // 2. cmp orders the datums as their values.
  // 3. The largest magnitude, 10^p - 1 at scale s, round trips,
  //    and one more digit is refused.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  for (auto [t, p, s] : {std::tuple{&sm2::type_decimal64, 1, 0},
                         {&sm2::type_decimal64, 5, 2},
                         {&sm2::type_decimal64, 17, 3},
                         {&sm2::type_decimal64, 17, 17},
                         {&sm2::type_decimal128, 5, 2},
                         {&sm2::type_decimal128, 18, 0},
                         {&sm2::type_decimal128, 36, 10},
                         {&sm2::type_decimal128, 36, 36}}) {
    CAPTURE(t->name);
    CAPTURE(p);
    CAPTURE(s);
    int32_t typmod = decimal_typmod(p, s);
    size_t width = t->len;
    std::vector<std::string> texts;
    std::vector<std::string> datums;
    for (int i = 0; i < property_runs / 10; i++) {
      std::string text = gen_decimal_text(rng, p, s), datum, back;
      CAPTURE(text);
      REQUIRE(t->in(text, typmod, datum) == 0);
      CHECK(datum.size() == width);
      REQUIRE(t->out(datum, typmod, back) == 0);
      CHECK(back == text);
      texts.push_back(text);
      datums.push_back(datum);
    }
    for (size_t i = 0; i + 1 < texts.size(); i++) {
      CHECK(sign(t->cmp(datums[i], datums[i + 1], typmod)) ==
            decimal_text_order(texts[i], texts[i + 1]));
    }
    std::string nines(p, '9');
    std::string max = s == p ? "0." + nines
                      : s > 0
                          ? nines.substr(0, p - s) + "." + nines.substr(p - s)
                          : nines;
    std::string datum, back;
    for (const std::string &m : {max, "-" + max}) {
      CAPTURE(m);
      REQUIRE(t->in(m, typmod, datum) == 0);
      REQUIRE(t->out(datum, typmod, back) == 0);
      CHECK(back == m);
    }
    std::string over =
        s == p ? "1." + std::string(s, '0') : "1" + max.substr(s == p ? 2 : 0);
    CAPTURE(over);
    CHECK(t->in(over, typmod, datum) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
}

TEST_CASE("a decimal's key refuses a datum whose scale is not its column's") {
  // Verify key_encode checks a decimal datum's scale bits against the
  // typmod's, as out does, since a key at another scale sorts wrong.
  // 1. Read "1.25" as numeric(5, 2), for each decimal width.
  // 2. Its key under numeric(5, 2) is made.
  // 3. Under numeric(5, 3) it is SM_ERR_INVALID, as out is.
  for (const sm2::type_t *t : {&sm2::type_decimal64, &sm2::type_decimal128}) {
    CAPTURE(t->name);
    std::string datum, key, text;
    REQUIRE(t->in("1.25", decimal_typmod(5, 2), datum) == 0);

    // 2.
    CHECK(t->key_encode(datum, decimal_typmod(5, 2), nullptr, key) == 0);

    // 3.
    key.clear();
    CHECK(t->key_encode(datum, decimal_typmod(5, 3), nullptr, key) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(t->out(datum, decimal_typmod(5, 3), text) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
}

TEST_CASE("only numeric(p, s)'s own text is decimal input") {
  // Verify in refuses what PostgreSQL never prints for numeric(p, s).
  // 1. At (5, 2): the wrong number of fraction digits, leading
  //    zeros, a plus, "-0.00", a bare point, exponents, NaN and
  //    infinity, spaces, a separator, and 1000.00, which needs six
  //    digits.
  // 2. There is no bare decimal: typmod -1 is refused, since sm3
  //    resolves a bare decimal to a declared one.
  const sm2::type_t &t = sm2::type_decimal64;
  int32_t typmod = decimal_typmod(5, 2);
  std::string datum;
  CHECK(t.in("123.45", typmod, datum) == 0);
  for (std::string_view text :
       {"",        "1.5",      "1.500",     "1",     "01.50", "00.50",
        "+1.50",   "-0.00",    ".50",       "1.",    "1e2",   "1.50e0",
        "NaN",     "Infinity", "-Infinity", " 1.50", "1.50 ", "1,000.00",
        "1000.00", "-",        "-.50",      "--1.50"}) {
    CAPTURE(text);
    CHECK(t.in(text, typmod, datum) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  CHECK(t.in("1.000", -1, datum) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a decimal typmod is numeric(p, s) within the type's digits") {
  // Verify the typmod checks on in, out and typmod_valid.
  // 1. A typmod with p of 0, s greater than p, below the +4
  //    offset, or -1 is refused by both decimals.
  // 2. p up to 17 is decimal64's, and 18 is refused; p up to 36
  //    is decimal128's, 37 refused, and 17 or fewer accepted too.
  // 3. out refuses a datum of the other type's width, and one
  //    whose value needs more than p digits.
  std::string datum, text;
  for (const sm2::type_t *t : {&sm2::type_decimal64, &sm2::type_decimal128}) {
    CAPTURE(t->name);
    for (int32_t typmod :
         {decimal_typmod(0, 0), decimal_typmod(5, 6), 0, 3, -1, -2}) {
      CAPTURE(typmod);
      CHECK_FALSE(t->typmod_valid(typmod));
      CHECK(sm2::datum_width(t, typmod) == 0);
      CHECK(t->in("0", typmod, datum) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
      CHECK(t->out(std::string(t->len, '\0'), typmod, text) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
  const sm2::type_t &d64 = sm2::type_decimal64, &d128 = sm2::type_decimal128;
  CHECK(d64.typmod_valid(decimal_typmod(17, 0)));
  CHECK_FALSE(d64.typmod_valid(decimal_typmod(18, 0)));
  CHECK(d128.typmod_valid(decimal_typmod(1, 0)));
  CHECK(d128.typmod_valid(decimal_typmod(36, 0)));
  CHECK_FALSE(d128.typmod_valid(decimal_typmod(37, 0)));
  CHECK(sm2::datum_width(&d64, decimal_typmod(17, 0)) == 8);
  CHECK(sm2::datum_width(&d128, decimal_typmod(5, 0)) == 16);
  CHECK(d64.out(std::string(16, '\0'), decimal_typmod(17, 0), text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(d128.out(std::string(8, '\0'), decimal_typmod(5, 0), text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  // 100000, which needs six digits, under decimal(5, 0).
  std::string six = le_bytes(int64_t{100000 * 32});
  CHECK(d64.out(six, decimal_typmod(5, 0), text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  six = le_bytes(int64_t{-99999 * 32});
  CHECK(d64.out(six, decimal_typmod(5, 0), text) == 0);
  CHECK(text == "-99999");
}

TEST_CASE("a decimal datum carries its scale below its value") {
  // Verify the datum's layout and its scale check.
  // 1. in under (5, 2) makes 123.45 and -123.45 the integer 12345
  //    shifted past 5 scale bits for decimal64, 6 for decimal128,
  //    with 2 in them, at 8 and 16 bytes.
  // 2. The largest magnitudes, 17 and 36 digits at scale 0, fit
  //    their words and print back.
  // 3. out refuses a datum whose scale is not its typmod's.
  const sm2::type_t &d64 = sm2::type_decimal64, &d128 = sm2::type_decimal128;
  int32_t typmod = decimal_typmod(5, 2);
  std::string datum, text;
  REQUIRE(d64.in("123.45", typmod, datum) == 0);
  CHECK(datum == le_bytes(int64_t{12345 * 32 + 2}));
  REQUIRE(d64.in("-123.45", typmod, datum) == 0);
  CHECK(datum == le_bytes(int64_t{-12345 * 32 + 2}));
  REQUIRE(d128.in("-123.45", typmod, datum) == 0);
  CHECK(datum == le_bytes(int64_t{-12345 * 64 + 2}) + le_bytes(int64_t{-1}));
  for (auto [t, p] : {std::pair{&d64, 17}, {&d128, 36}}) {
    CAPTURE(t->name);
    for (const std::string &m :
         {std::string(p, '9'), "-" + std::string(p, '9')}) {
      REQUIRE(t->in(m, decimal_typmod(p, 0), datum) == 0);
      REQUIRE(t->out(datum, decimal_typmod(p, 0), text) == 0);
      CHECK(text == m);
    }
  }
  REQUIRE(d64.in("123.45", typmod, datum) == 0);
  CHECK(d64.out(datum, decimal_typmod(5, 3), text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(d64.out(le_bytes(int64_t{12345 * 32 + 3}), typmod, text) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}
