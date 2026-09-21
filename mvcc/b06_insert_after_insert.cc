/*
 * b06 -- i over a committed i of the same row.
 *
 *   T1: r(R2) -> -    (absent in T1's snapshot)
 *   T2: i(R2) c
 *   T1: i(R2) -> CONFLICT, not DUPKEY
 *
 * Expected in every pairing: the row is absent in T1's
 * snapshot, so this is not a duplicate key from T1's point of
 * view -- it is a row that changed after the snapshot, which
 * is retryable. DUPKEY is b12.
 */

#include "mvcc.h"

TEST_CASE("b06: inserting a row another transaction inserted conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R2) == "-");

    s.begin(2, p);
    REQUIRE(s.i(2, R2, "t2") == 0);
    CHECK(s.c(2));

    conflicts(s.i(1, R2, "t1"));
    CHECK(s.final_value(R2) == "t2");
  }
}
