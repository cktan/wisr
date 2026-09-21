/*
 * a04 -- read-write-read with an update: no non-repeatable read.
 *
 *   T1: r(R1) -> 0
 *   T2: w(R1) c
 *   T1: r(R1) -> 0     (not T2's value)
 *
 * Expected in every pairing: T1 reads the same value twice.
 * T1 is read-only, so even at V+V it commits: one rw-edge out
 * of T1 is not a dangerous structure.
 */

#include "mvcc.h"

TEST_CASE("a04: a repeated read is unaffected by a committed update") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    CHECK(s.r(1, R1) == "0");
    CHECK(s.c(1));
    CHECK(s.final_value(R1) == "t2");
  }
}
