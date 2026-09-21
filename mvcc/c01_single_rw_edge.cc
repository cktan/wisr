/*
 * c01 -- one read-write edge is not a cycle.
 *
 *   T1: r(R1)
 *   T2: w(R1) c        -> T1 --rw--> T2
 *   T1: w(R2) c        (R2 is read by nobody)
 *
 * Expected: OK in every pairing, V+V included. SSI fails a
 * transaction only for a dangerous structure -- two adjacent
 * rw-edges with the last one committing first -- and a lone
 * edge is serializable as T1 then T2.
 */

#include "mvcc.h"

TEST_CASE("c01: a single read-write edge commits in every pairing") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    REQUIRE(s.w(1, R2, "t1") == 0);
    CHECK(s.c(1));
    CHECK(s.final_value(R1) == "t2");
    CHECK(s.final_value(R2) == "t1");
  }
}
