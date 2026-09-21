/*
 * c17 -- an edge onto a pivot that has already committed.
 *
 *   T3: r(R1)
 *   T2: r(R2) w(R1) c          T3 --rw--> T2, and T2 commits
 *   T1: w(R2)                  T2 --rw--> T1
 *   T1: c
 *
 * T2 gains its second edge after committing, which makes it a
 * pivot; T1, which has no edge of its own out, is the one
 * doomed. This is Cahill's rule for a committed pivot
 * (DESIGN.md §7.2).
 *
 * Expected:
 *   RR    T1 commits
 *   RR+V  T1 commits
 *   V+V   T1 fails at commit
 */

#include "mvcc.h"

TEST_CASE("c17: completing a committed pivot dooms the newcomer") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    s.begin(3, p);
    CHECK(s.r(3, R1) == "0");
    CHECK(s.r(2, R2) == "0");
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    REQUIRE(s.w(1, R2, "t1") == 0);
    bool c1 = s.c(1);
    CHECK(c1 == !both_serializable(p));
    CHECK(s.final_value(R2) == (both_serializable(p) ? "0" : "t1"));
  }
}
