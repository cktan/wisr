/*
 * a01 -- dirty read of an update.
 *
 *   T1: w(R1)          T2: r(R1) -> old
 *   T1: c              T2: r(R1) -> old   (T2's snapshot predates c)
 *
 * Expected in every pairing: T2 never sees T1's uncommitted
 * value, and keeps its own snapshot even after T1 commits.
 */

#include "mvcc.h"

TEST_CASE("a01: an uncommitted update is invisible to another transaction") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.w(1, R1, "t1") == 0);

    CHECK(s.r(2, R1) == "0"); // not "t1"
    CHECK(s.c(1));
    CHECK(s.r(2, R1) == "0"); // still the snapshot's value
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t1");
  }
}
