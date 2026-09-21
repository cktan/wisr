/*
 * c16 -- a point read that steps past a newer version.
 *
 *   T1: begins
 *   T2: r(R2) w(R1) c                  T2 commits with an edge in
 *   T1: r(R1) -> 0     steps past T2's newer version:
 *                      T1 --rw--> T2
 *   T1: w(R2)          T2 read R2:     T2 --rw--> T1
 *   T1: c
 *
 * T2 is a committed pivot, so the transaction completing the
 * structure -- T1 -- is the one that must fail (DESIGN.md
 * §7.2, §10.1: the reader runs on_skip over versions it steps
 * past).
 *
 * Expected:
 *   RR    both commit
 *   RR+V  both commit
 *   V+V   T1 fails at commit
 */

#include "mvcc.h"

TEST_CASE("c16: reading past a concurrent writer's version makes an edge") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.r(2, R2) == "0");
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    CHECK(s.r(1, R1) == "0"); // steps past T2's version
    REQUIRE(s.w(1, R2, "t1") == 0);
    bool c1 = s.c(1);
    if (both_serializable(p)) {
      CHECK_FALSE(c1);
      CHECK(s.final_value(R2) == "0");
    } else {
      CHECK(c1);
      CHECK(s.final_value(R2) == "t1");
    }
  }
}
