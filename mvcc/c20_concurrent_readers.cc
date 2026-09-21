/*
 * c20 -- readers do not block or conflict with each other.
 *
 *   T1: r(R1) s(R1..R2)
 *   T2: r(R1) s(R1..R2)
 *   T3: r(R1) s(R1..R2)
 *   all commit, then one of them writes R1 and commits
 *
 * Reads take no locks (DESIGN.md §5.1), and an SIREAD is not
 * a lock: it never blocks and never fails a reader.
 *
 * Expected: OK in every pairing.
 */

#include "mvcc.h"

TEST_CASE("c20: concurrent readers never block or conflict") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    for (int n : {1, 2, 3}) {
      s.begin(n, p);
      CHECK(s.r(n, R1) == "0");
      CHECK(s.s(n, R1, R2) == std::vector<std::string>{R1, R2});
    }
    CHECK(s.c(1));
    CHECK(s.c(2));
    CHECK(s.c(3));

    s.begin(1, p);
    REQUIRE(s.w(1, R1, "t1") == 0);
    CHECK(s.c(1));
  }
}
