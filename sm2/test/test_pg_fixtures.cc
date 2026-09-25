/*
 * test_pg_fixtures.cc -- sm2's datums against the bytes
 * PostgreSQL itself stored (test/pg_fixtures.h, PLAN.md
 * "Testing").
 */

#include "doctest.h"
#include "pg_fixtures.h"

#include "sm2.h"

#include <bit>
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

TEST_CASE("every number datum is PostgreSQL's value, scaled, as an integer") {
  // Verify each number fixture both ways, under its typmod.
  // 1. in on the text PostgreSQL printed for numeric(p, s) gives
  //    the value times 10^s, as a two's complement integer of 16
  //    or 32 bytes.
  // 2. out on those bytes gives back PostgreSQL's text.
  for (const pg_number_fixture_t &f : pg_number_fixtures) {
    CAPTURE(f.typmod);
    CAPTURE(f.text);
    std::string datum, text;
    REQUIRE(sm2::type_number.in(f.text, f.typmod, datum) == 0);
    CHECK(datum == unhex(f.hex));
    REQUIRE(sm2::type_number.out(unhex(f.hex), f.typmod, text) == 0);
    CHECK(text == f.text);
  }
}
