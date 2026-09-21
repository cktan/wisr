/*
 * a11 -- a point read of a row that did not exist yet.
 *
 *   T1: r(R2) -> -
 *   T2: i(R2) c
 *   T1: r(R2) -> -
 *
 * Expected in every pairing: the read keeps saying "no row".
 * Note what is NOT here: T1 only reads, so V+V commits it.
 * The version where T1 then writes is c05.
 */

#include "mvcc.h"

TEST_CASE("a11: a row inserted after the snapshot stays unreadable") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R2) == "-");

    s.begin(2, p);
    REQUIRE(s.i(2, R2, "t2") == 0);
    CHECK(s.c(2));

    CHECK(s.r(1, R2) == "-");
    CHECK(s.c(1));
    CHECK(s.final_value(R2) == "t2");
  }
}
