/*
 * c08 -- an insert outside the scanned range is no conflict.
 *
 *   T1: s(R1..R2) -> {R1}
 *   T2: i(R4) r(R3)
 *   T1: w(R3)
 *   both commit
 *
 * T2's insert lands outside T1's predicate, so there is no
 * T1 --rw--> T2 edge; T1's write of a row T2 read gives only
 * T2 --rw--> T1.
 *
 * Expected: OK in every pairing. A level that failed here
 * would be tracking ranges too coarsely.
 */

#include "mvcc.h"

TEST_CASE("c08: an insert outside the predicate creates no edge") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R3});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.s(1, R1, R2) == std::vector<std::string>{R1});

    REQUIRE(s.i(2, R4, "t2") == 0);
    CHECK(s.r(2, R3) == "0");
    REQUIRE(s.w(1, R3, "t1") == 0);

    CHECK(s.c(1));
    CHECK(s.c(2));
    CHECK(s.final_value(R3) == "t1");
    CHECK(s.final_value(R4) == "t2");
  }
}
