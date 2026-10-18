/*
 * test_pg_fixtures.cc -- sm2's datums against the bytes
 * PostgreSQL itself stored (test/pg_fixtures.h, SM2_DESIGN.md §8.3
 * "Testing").
 */

#include "doctest.h"
#include "pg_fixtures.h"

#include "sm2.h"

#include <bit>
#include <cstring>
#include <string>

namespace {

/* The bytes a string of hex digits spells. */
std::string unhex(const char *hex) {
  std::string out;
  for (const char *p = hex; p[0] && p[1]; p += 2) {
    out.push_back(static_cast<char>(std::stoi(std::string(p, 2), nullptr, 16)));
  }
  return out;
}

} // namespace

TEST_CASE("every datum is the bytes PostgreSQL stores") {
  // Verify each fixture both ways through its type.
  // 1. The host is little-endian, which the fixtures' bytes assume.
  // 2. Each fixture's type exists.
  // 3. in on the fixture's text gives PostgreSQL's bytes.
  // 4. out on PostgreSQL's bytes gives back the fixture's text.
  REQUIRE(std::endian::native == std::endian::little);
  for (const pg_fixture_t &f : pg_fixtures) {
    CAPTURE(f.text);
    const sm2::type_t *t = sm2::type_by_code(f.code);
    REQUIRE(t);
    CAPTURE(t->name);
    std::string datum, text;
    REQUIRE(t->in(f.text, -1, datum) == 0);
    CHECK(datum == unhex(f.hex));
    REQUIRE(t->out(unhex(f.hex), -1, text) == 0);
    CHECK(text == f.text);
  }
}

TEST_CASE("every decimal datum is PostgreSQL's value, scaled, with its scale") {
  // Verify each decimal fixture both ways, under its typmod, as
  // sm3 maps numeric(p, s): decimal64 up to 17 digits, decimal128
  // up to 36, and past 36 neither.
  // 1. A fixture's hex is the value times 10^s; its datum is that
  //    integer shifted left past the scale, 5 bits for decimal64
  //    and 6 for decimal128, with s in them, at 8 or 16 bytes.
  // 2. in on the text PostgreSQL printed for numeric(p, s) gives
  //    that datum, and out on it gives back PostgreSQL's text.
  // 3. A precision past 36 is a typmod both decimals refuse.
  for (const pg_decimal_fixture_t &f : pg_decimal_fixtures) {
    CAPTURE(f.typmod);
    CAPTURE(f.text);
    int p = (f.typmod - 4) >> 16, s = (f.typmod - 4) & 0xFFFF;
    if (p > 36) {
      CHECK_FALSE(sm2::type_decimal64.typmod_valid(f.typmod));
      CHECK_FALSE(sm2::type_decimal128.typmod_valid(f.typmod));
      continue;
    }
    const sm2::type_t &t = p <= 17 ? sm2::type_decimal64 : sm2::type_decimal128;
    std::string scaled = unhex(f.hex);
    // Sign-extend the fixture's integer, 8 or 16 bytes, to 128 bits.
    __int128 v = 0;
    memcpy(&v, scaled.data(), scaled.size());
    if (scaled.size() == 8) {
      v = static_cast<int64_t>(v);
    }
    __int128 d = &t == &sm2::type_decimal64 ? v * 32 + s : v * 64 + s;
    std::string expect(reinterpret_cast<const char *>(&d), t.len);
    std::string datum, text;
    REQUIRE(t.in(f.text, f.typmod, datum) == 0);
    CHECK(datum == expect);
    REQUIRE(t.out(expect, f.typmod, text) == 0);
    CHECK(text == f.text);
  }
}
