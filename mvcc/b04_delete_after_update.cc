/*
 * b04 -- d over a committed w.
 *
 *   T1: r(R1) -> 0
 *   T2: w(R1) c
 *   T1: d(R1) -> CONFLICT
 *
 * Expected in every pairing: a delete is a write like any
 * other and loses to the earlier committer.
 */

#include "mvcc.h"

TEST_CASE("b04: deleting a row another transaction updated conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    conflicts(s.d(1, R1));
    CHECK(s.final_value(R1) == "t2");
  }
}
