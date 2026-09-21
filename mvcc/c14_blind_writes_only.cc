/*
 * c14 -- writes with no reads build no edges.
 *
 *   T1: w(R1)          T2: w(R2)
 *   both commit
 *
 * Neither transaction reads anything, so no rw-edge can exist
 * in either direction, whatever the levels.
 *
 * Expected: OK in every pairing.
 */

#include "mvcc.h"

TEST_CASE("c14: concurrent blind writes commit in every pairing") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.w(2, R2, "t2") == 0);
    CHECK(s.c(1));
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t1");
    CHECK(s.final_value(R2) == "t2");
  }
}
