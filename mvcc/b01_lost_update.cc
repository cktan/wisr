/*
 * b01 -- read-write-write: the lost update.
 *
 *   T1: r(R1) -> 0
 *   T2: w(R1) c
 *   T1: w(R1) -> CONFLICT
 *
 * Expected in every pairing: first-committer-wins (DESIGN.md
 * §5.2). The check is against T1's begin snapshot, so it does
 * not matter that T1 read the row first -- b02 shows the same
 * conflict without the read.
 */

#include "mvcc.h"

TEST_CASE("b01: an update over a committed change conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    conflicts(s.w(1, R1, "t1"));
    CHECK(s.final_value(R1) == "t2");
  }
}
