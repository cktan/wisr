/*
 * a08 -- a transaction reads its own writes.
 *
 *   T1: i(R3) w(R1) d(R2)
 *   T1: r(R1) -> new, r(R2) -> -, r(R3) -> new
 *   T1: s(R1..R3) -> {R1, R3}
 *
 * Expected in every pairing: own writes are visible to the
 * writer before commit, through both read paths, including a
 * row it inserted and then overwrote or deleted.
 */

#include "mvcc.h"

TEST_CASE("a08: a transaction reads its own inserts, updates, and deletes") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    REQUIRE(s.i(1, R3, "t1") == 0);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.d(1, R2) == 0);
    CHECK(s.r(1, R1) == "t1");
    CHECK(s.r(1, R2) == "-");
    CHECK(s.r(1, R3) == "t1");
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R3});

    REQUIRE(s.w(1, R3, "again") == 0);
    CHECK(s.r(1, R3) == "again");
    REQUIRE(s.d(1, R3) == 0);
    CHECK(s.r(1, R3) == "-");
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1});
    CHECK(s.c(1));
  }
}
