/*
 * b12 -- inserting a row that existed before the snapshot.
 *
 *   T1: i(R1) -> DUPKEY        (R1 committed long ago)
 *
 * Expected in every pairing: this is not a conflict and not
 * retryable -- nothing changed after T1's snapshot. It is the
 * ordinary "the key is taken" answer, and the transaction
 * stays active, as b06's conflict does not.
 */

#include "mvcc.h"

TEST_CASE("b12: inserting an existing row gives DUPKEY and keeps the "
          "transaction") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    fails(s.i(1, R1, "t1"), SM_ERR_DUPKEY);
    CHECK_FALSE(sm_retryable());

    // The transaction is untouched and may carry on.
    CHECK(s.w(1, R1, "t1") == 0);
    CHECK(s.c(1));
    CHECK(s.final_value(R1) == "t1");
  }
}
