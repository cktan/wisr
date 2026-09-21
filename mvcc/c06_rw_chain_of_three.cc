/*
 * c06 -- the dangerous structure with three transactions.
 *
 *   T1: r(R1)                          T1 --rw--> T2
 *   T2: r(R2) w(R1)                    T2 --rw--> T3
 *   T3: w(R2) c                        T3 commits first
 *   T2: c
 *   T1: c
 *
 * T2 is the pivot: an incoming edge from T1, an outgoing edge
 * to T3, and T3 committed first. T3 takes T2's level here, so
 * V+V means all three are SERIALIZABLE.
 *
 * Expected:
 *   RR    all three commit
 *   RR+V  all three commit -- the chain needs every link
 *         tracked, and in either mixed pairing one link is an
 *         untracked REPEATABLE READ read
 *   V+V   the pivot T2 fails
 */

#include "mvcc.h"

TEST_CASE("c06: a three-transaction read-write chain fails its pivot") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    s.begin(3, p); // T3 runs at T2's level

    CHECK(s.r(1, R1) == "0");
    CHECK(s.r(2, R2) == "0");
    REQUIRE(s.w(2, R1, "t2") == 0); // T1 --rw--> T2
    REQUIRE(s.w(3, R2, "t3") == 0); // T2 --rw--> T3

    CHECK(s.c(3));
    bool c2 = s.c(2);
    bool c1 = s.c(1);
    if (both_serializable(p)) {
      CHECK_FALSE(c2); // the pivot
      CHECK(c1);
      CHECK(s.final_value(R1) == "0"); // T2 wrote nothing
    } else {
      CHECK(c2);
      CHECK(c1);
      CHECK(s.final_value(R1) == "t2");
    }
    CHECK(s.final_value(R2) == "t3");
  }
}
