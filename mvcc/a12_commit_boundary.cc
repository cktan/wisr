/*
 * a12 -- the commit boundary decides what a snapshot sees.
 *
 *   T1: w(R1) i(R2)
 *   T2 begins BEFORE T1 commits -> sees neither
 *   T1: c
 *   T3 begins AFTER  T1 commits -> sees both
 *
 * Expected in every pairing: visibility is by commit order
 * against each transaction's snapshot, not by wall-clock or
 * by who asked first.
 */

#include "mvcc.h"

TEST_CASE("a12: a snapshot taken before a commit never sees it") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.i(1, R2, "t1") == 0);

    s.begin(2, p);
    CHECK(s.r(2, R1) == "0");
    CHECK(s.r(2, R2) == "-");
    CHECK(s.c(1));
    CHECK(s.r(2, R1) == "0");
    CHECK(s.r(2, R2) == "-");
    CHECK(s.c(2));

    s.begin_at(3, SM_XCT_REPEATABLE_READ);
    CHECK(s.r(3, R1) == "t1");
    CHECK(s.r(3, R2) == "t1");
    CHECK(s.c(3));
  }
}
