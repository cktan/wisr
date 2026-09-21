/*
 * b14 -- what the loser of a conflict leaves behind.
 *
 *   T1: w(R1) w(R2)
 *   T2: w(R3) c
 *   T1: w(R3) -> CONFLICT, which aborts T1
 *
 * Expected in every pairing: T1's earlier writes to R1 and R2
 * are discarded with it, and the next call on that session
 * reports no transaction. Only T2's change survives.
 */

#include "mvcc.h"

TEST_CASE("b14: a conflict discards the whole losing transaction") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R3});
    s.begin(1, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.w(1, R2, "t1") == 0);

    s.begin(2, p);
    REQUIRE(s.w(2, R3, "t2") == 0);
    CHECK(s.c(2));

    conflicts(s.w(1, R3, "t1"));
    // The error aborted T1: there is nothing left to commit.
    fails(sm_commit(s.session(1)), SM_ERR_NOTRANS);

    CHECK(s.final_value(R1) == "0");
    CHECK(s.final_value(R2) == "0");
    CHECK(s.final_value(R3) == "t2");
  }
}
