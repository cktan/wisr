/*
 * a10 -- a row deleted and inserted again.
 *
 *   T1: d(R1) i(R1) c
 *   T2 (snapshot before T1's commit): r(R1) -> old
 *   T3 (after):                       r(R1) -> new, once
 *
 * Expected in every pairing: the tombstone and the new version
 * both sit under R1, and each snapshot resolves to exactly one
 * of them.
 */

#include "mvcc.h"

TEST_CASE("a10: a resurrected row is one row with one value per snapshot") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(2, p);
    CHECK(s.r(2, R1) == "0");

    s.begin(1, p);
    REQUIRE(s.d(1, R1) == 0);
    REQUIRE(s.i(1, R1, "new") == 0);
    CHECK(s.r(1, R1) == "new");
    CHECK(s.c(1));

    CHECK(s.r(2, R1) == "0");
    CHECK(s.srows(2, R1, R2) ==
          std::vector<std::pair<std::string, std::string>>{{R1, "0"}});
    CHECK(s.c(2));
    CHECK(s.final_rows() ==
          std::vector<std::pair<std::string, std::string>>{{R1, "new"}});
  }
}
