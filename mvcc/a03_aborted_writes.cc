/*
 * a03 -- an aborted transaction leaves nothing behind.
 *
 *   T1: w(R1) d(R2) i(R3) a
 *   T2: r(R1) -> old, r(R2) -> old, r(R3) -> -
 *   T2: w(R1) d(R2) i(R3) c
 *
 * Expected in every pairing: the abort is invisible, and it
 * leaves no conflict behind -- T2 may write the same rows.
 */

#include "mvcc.h"

TEST_CASE("a03: an aborted transaction is invisible and blocks nobody") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.d(1, R2) == 0);
    REQUIRE(s.i(1, R3, "t1") == 0);
    s.a(1);

    s.begin(2, p);
    CHECK(s.r(2, R1) == "0");
    CHECK(s.r(2, R2) == "0");
    CHECK(s.r(2, R3) == "-");
    CHECK(s.w(2, R1, "t2") == 0);
    CHECK(s.d(2, R2) == 0);
    CHECK(s.i(2, R3, "t2") == 0);
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t2");
  }
}
