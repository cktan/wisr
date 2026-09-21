/*
 * c13 -- where a doomed transaction finds out.
 *
 *   T1: r(R1) r(R2)    T2: r(R1) r(R2)
 *   T1: w(R1)          T2: w(R2)
 *   every call so far returns 0, doomed or not
 *   T1: c              T2: c        -- the failure is here
 *
 * Expected in every pairing: no read or write fails. At V+V
 * one of the commits reports SM_ERR_SERIALIZATION, which is
 * retryable, and a retry with nobody concurrent commits.
 */

#include "mvcc.h"

TEST_CASE("c13: a doomed transaction fails at commit, not before") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    for (int n : {1, 2}) {
      CHECK(s.r(n, R1) == "0");
      CHECK(s.r(n, R2) == "0");
    }
    CHECK(s.w(1, R1, "t1") == 0); // succeeds even when doomed
    CHECK(s.w(2, R2, "t2") == 0);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
      // Retry each loser on its own; nothing is concurrent now.
      if (!c1) {
        s.begin(1, p);
        CHECK(s.r(1, R1) == "0"); // its earlier write is gone
        REQUIRE(s.w(1, R1, "t1") == 0);
        CHECK(s.c(1));
      }
      if (!c2) {
        s.begin(2, p);
        REQUIRE(s.w(2, R2, "t2") == 0);
        CHECK(s.c(2));
      }
    } else {
      CHECK(c1);
      CHECK(c2);
    }
    CHECK(s.final_value(R1) == "t1");
    CHECK(s.final_value(R2) == "t2");
  }
}
