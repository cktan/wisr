/*
 * b13 -- updating or deleting a row that never existed.
 *
 *   T1: w(R2) -> NOTFOUND
 *   T1: d(R2) -> NOTFOUND
 *
 * Expected in every pairing: not retryable, and the
 * transaction stays active. Contrast b03 and b05, where the
 * row existed in the snapshot and was removed by someone else:
 * those are CONFLICT.
 */

#include "mvcc.h"

TEST_CASE("b13: writing a row that never existed gives NOTFOUND") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    fails(s.w(1, R2, "t1"), SM_ERR_NOTFOUND);
    CHECK_FALSE(sm_retryable());
    fails(s.d(1, R2), SM_ERR_NOTFOUND);
    CHECK_FALSE(sm_retryable());

    CHECK(s.i(1, R2, "t1") == 0);
    CHECK(s.c(1));
    CHECK(s.final_value(R2) == "t1");
  }
}
