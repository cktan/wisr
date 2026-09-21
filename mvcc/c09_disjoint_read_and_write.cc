/*
 * c09 -- reads and writes that do not meet.
 *
 *   T1: r(R1) w(R2)
 *   T2: r(R3) w(R4)
 *   both commit
 *
 * No transaction writes a row the other read, so there is no
 * rw-edge in either direction.
 *
 * Expected: OK in every pairing.
 */

#include "mvcc.h"

TEST_CASE("c09: disjoint reads and writes never conflict") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R3, R4});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.r(1, R1) == "0");
    CHECK(s.r(2, R3) == "0");
    REQUIRE(s.w(1, R2, "t1") == 0);
    REQUIRE(s.w(2, R4, "t2") == 0);
    CHECK(s.c(1));
    CHECK(s.c(2));
  }
}
