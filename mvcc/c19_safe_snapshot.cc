/*
 * c19 -- reads alone never doom a transaction.
 *
 *   T1: r(R1)
 *   T2: w(R2) c        T2 wrote nothing T1 read
 *   T1: w(R1) c
 *   then T1 again, on an idle database: r, s, w, c
 *
 * T1 has no incoming edge, so however long it lives and
 * whatever it writes afterwards, it commits (DESIGN.md §7.4).
 *
 * Expected: OK in every pairing.
 */

#include "mvcc.h"

TEST_CASE("c19: a reader that outlives its concurrent writers can write") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R2, "t2") == 0);
    CHECK(s.c(2));

    REQUIRE(s.w(1, R1, "t1") == 0);
    CHECK(s.c(1));

    // Again, with nothing else running at all.
    s.begin(1, p);
    CHECK(s.r(1, R1) == "t1");
    CHECK(s.s(1, R1, R2).size() == 2);
    REQUIRE(s.w(1, R1, "again") == 0);
    CHECK(s.c(1));
  }
}
