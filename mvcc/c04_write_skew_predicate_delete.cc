/*
 * c04 -- write skew over a predicate, resolved by deletes.
 *
 *   T1: s(P) -> {R1, R2}   T2: s(P) -> {R1, R2}
 *   T1: d(R1)              T2: d(R2)
 *   T1: c                  T2: c
 *
 * "Remove one, as long as one remains", run twice, emptying
 * the set.
 *
 * Expected:
 *   RR    both commit -- the set ends up empty
 *   RR+V  both commit
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("c04: write skew through a predicate, by deleting") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.s(1, R1, R3).size() == 2);
    CHECK(s.s(2, R1, R3).size() == 2);

    REQUIRE(s.d(1, R1) == 0);
    REQUIRE(s.d(2, R2) == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
      CHECK(s.final_keys().empty());
    }
  }
}
