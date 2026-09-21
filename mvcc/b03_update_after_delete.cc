/*
 * b03 -- w over a committed d.
 *
 *   T1: r(R1) -> 0
 *   T2: d(R1) c
 *   T1: w(R1) -> CONFLICT, not NOTFOUND
 *
 * Expected in every pairing: the row still exists in T1's
 * snapshot, so the failure must be the retryable conflict, not
 * the "no such row" a missing row would give (b13).
 */

#include "mvcc.h"

TEST_CASE("b03: updating a row another transaction deleted conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.d(2, R1) == 0);
    CHECK(s.c(2));

    conflicts(s.w(1, R1, "t1"));
    CHECK(s.final_value(R1) == "-");
  }
}
