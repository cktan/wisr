/*
 * c05 -- reading a row that is not there counts as a read.
 *
 *   T1: r(R2) -> -     T2: r(R1) -> -
 *   T1: i(R1)          T2: i(R2)
 *   T1: c              T2: c
 *
 * Each transaction inserts exactly the row the other looked
 * for and did not find: the rw-edges run both ways even
 * though neither read returned a row.
 *
 * Expected:
 *   RR    both commit
 *   RR+V  both commit
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("c05: a point read of a missing row takes part in conflicts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s;
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.r(1, R2) == "-");
    CHECK(s.r(2, R1) == "-");

    REQUIRE(s.i(1, R1, "t1") == 0);
    REQUIRE(s.i(2, R2, "t2") == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
    }
  }
}
