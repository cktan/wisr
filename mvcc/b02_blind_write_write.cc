/*
 * b02 -- write-write with no read at all.
 *
 *   T1: begins (snapshot taken)
 *   T2: w(R1) c
 *   T1: w(R1) -> CONFLICT
 *
 * Expected in every pairing: a blind write conflicts too. The
 * rule is about the row's newest committed version against the
 * snapshot, not about what the transaction read.
 */

#include "mvcc.h"

TEST_CASE("b02: a blind write over a committed change conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    REQUIRE(s.w(1, R2, "t1") == 0); // fixes T1's snapshot, elsewhere

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    conflicts(s.w(1, R1, "t1"));
    CHECK(s.final_value(R1) == "t2");
    CHECK(s.final_value(R2) == "0"); // T1 was aborted by the conflict
  }
}
