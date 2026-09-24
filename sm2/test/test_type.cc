/*
 * test_type.cc -- the type descriptors (SM2_DESIGN.md §2.3) and
 * the types built so far: bool, int2, int4 and int8 (TYPE.md
 * §3.1, §3.2).
 */

#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <cstdint>
#include <cstring>
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

} // namespace

TEST_CASE("each type is found by its frozen code, and only by it") {
  // Verify the registry and each descriptor's fixed properties.
  // 1. Codes 1 to 4 find bool, int2, int4 and int8, whose name,
  //    width, by-value flag and alignment are PostgreSQL's.
  // 2. The slots not filled yet are null.
  // 3. Code 0, which is never a type, and codes not assigned yet
  //    find nothing.
  struct {
    uint8_t code;
    const char *name;
    int16_t len;
    uint8_t align;
  } want[] = {{1, "bool", 1, 1},
              {2, "int2", 2, 2},
              {3, "int4", 4, 4},
              {4, "int8", 8, 8}};
  for (auto &w : want) {
    CAPTURE(w.name);
    const sm2::type_t *t = sm2::type_by_code(w.code);
    REQUIRE(t);
    CHECK(t->code == w.code);
    CHECK(std::string(t->name) == w.name);
    CHECK(t->len == w.len);
    CHECK(t->byval);
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
  for (uint8_t code : {0, 5, 64, 255}) {
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
