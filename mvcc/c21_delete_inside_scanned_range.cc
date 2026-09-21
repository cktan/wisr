/*
 * c21 -- a delete inside a scanned range makes the same edge
 * an insert does.
 *
 *   T1: s(R1..R3) -> {R1, R2}
 *   T2: r(R5) d(R1)            T1 --rw--> T2
 *   T1: w(R5)                  T2 --rw--> T1
 *   T1: c              T2: c
 *
 * Expected:
 *   RR    both commit
 *   RR+V  both commit
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("c21: a delete inside a scanned range is a read-write conflict") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R5});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R2});
    CHECK(s.r(2, R5) == "0");

    REQUIRE(s.d(2, R1) == 0);
    REQUIRE(s.w(1, R5, "t1") == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
    }
  }
}
