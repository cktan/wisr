/*
 * b07 -- i over a committed d.
 *
 *   T1: r(R1) -> 0    (present in T1's snapshot)
 *   T2: d(R1) c
 *   T1: i(R1) -> CONFLICT
 *
 * Expected in every pairing: T1 would be inserting a row its
 * own snapshot says already exists, and the row changed after
 * that snapshot. The conflict is reported before the "already
 * exists" rule is reached.
 */

#include "mvcc.h"

TEST_CASE("b07: inserting a row another transaction deleted conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.d(2, R1) == 0);
    CHECK(s.c(2));

    conflicts(s.i(1, R1, "t1"));
    CHECK(s.final_value(R1) == "-");
  }
}
