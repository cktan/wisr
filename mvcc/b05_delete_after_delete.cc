/*
 * b05 -- d over a committed d.
 *
 *   T1: r(R1) -> 0
 *   T2: d(R1) c
 *   T1: d(R1) -> CONFLICT, not NOTFOUND
 *
 * Expected in every pairing: two transactions deleting the
 * same row is a write-write conflict; the loser must retry
 * rather than believe the row was never there.
 */

#include "mvcc.h"

TEST_CASE("b05: deleting a row another transaction deleted conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.d(2, R1) == 0);
    CHECK(s.c(2));

    conflicts(s.d(1, R1));
  }
}
