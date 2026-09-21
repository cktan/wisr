/*
 * c02 -- write skew over two rows read by point read.
 *
 *   T1: r(R1) r(R2)    T2: r(R1) r(R2)
 *   T1: w(R1)          T2: w(R2)
 *   T1: c              T2: c
 *
 * Each transaction writes a row the other read, so
 * T1 --rw--> T2 and T2 --rw--> T1: a cycle.
 *
 * Expected:
 *   RR    both commit -- the anomaly is allowed
 *   RR+V  both commit -- SSI needs both sides tracked
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("c02: write skew on two rows") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    for (int n : {1, 2}) {
      CHECK(s.r(n, R1) == "0");
      CHECK(s.r(n, R2) == "0");
    }
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.w(2, R2, "t2") == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
    }
  }
}
