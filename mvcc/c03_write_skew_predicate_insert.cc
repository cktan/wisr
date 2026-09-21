/*
 * c03 -- write skew over a predicate, resolved by inserts.
 *
 *   T1: s(P) -> {}     T2: s(P) -> {}
 *   T1: i(R1)          T2: i(R2)     (both inside P)
 *   T1: c              T2: c
 *
 * "Insert it if nobody else has", run twice. Each insert
 * falsifies the other's predicate.
 *
 * Expected:
 *   RR    both commit -- two rows where the rule allows one
 *   RR+V  both commit
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("c03: write skew through a predicate, by inserting") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s;
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.s(1, R1, R3).empty());
    CHECK(s.s(2, R1, R3).empty());

    REQUIRE(s.i(1, R1, "t1") == 0);
    REQUIRE(s.i(2, R2, "t2") == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
      CHECK(s.final_keys() == std::vector<std::string>{R1, R2});
    }
  }
}
