/*
 * c10 -- the reader finishes before the writer starts.
 *
 *   T1: r(R1) w(R2) c
 *   T2: r(R2) w(R1) c
 *
 * T2 begins only after T1 committed, so the two do not
 * overlap: whatever rows they share, the history is the serial
 * one T1, T2.
 *
 * Expected: OK in every pairing. This is the shape of c02 with
 * the concurrency removed, and it must never fail.
 */

#include "mvcc.h"

TEST_CASE("c10: non-overlapping transactions with the skew shape commit") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");
    REQUIRE(s.w(1, R2, "t1") == 0);
    CHECK(s.c(1));

    s.begin(2, p);
    CHECK(s.r(2, R2) == "t1");
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t2");
    CHECK(s.final_value(R2) == "t1");
  }
}
