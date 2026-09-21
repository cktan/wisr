/*
 * b08 -- writes to different rows never conflict.
 *
 *   T1: w(R1) d(R3) i(R4)
 *   T2: w(R2) d(R5) i(R6-as-R4's neighbour)
 *   both commit
 *
 * Expected in every pairing: the conflict check is per row.
 * Two transactions writing disjoint rows of one relation are
 * independent, whatever their levels.
 */

#include "mvcc.h"

TEST_CASE("b08: concurrent writes to different rows both commit") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R3, R5});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.w(2, R2, "t2") == 0);
    REQUIRE(s.d(1, R3) == 0);
    REQUIRE(s.d(2, R5) == 0);
    REQUIRE(s.i(1, R4, "t1") == 0);
    CHECK(s.c(1));
    CHECK(s.c(2));

    CHECK(s.final_value(R1) == "t1");
    CHECK(s.final_value(R2) == "t2");
    CHECK(s.final_keys() == std::vector<std::string>{R1, R2, R4});
  }
}
