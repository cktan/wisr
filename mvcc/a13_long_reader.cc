/*
 * a13 -- a snapshot survives a long series of commits.
 *
 *   T1: s(all) -> V
 *   T2: ten transactions of inserts, updates, and a delete
 *   T1: s(all) -> V after every one of them
 *
 * Expected in every pairing: a snapshot is a fixed view, not a
 * moving one, however much is committed around it. T1 reads
 * only, so V+V commits it.
 */

#include "mvcc.h"

TEST_CASE("a13: a read-only transaction sees one state throughout") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    auto before = s.srows(1, R1, R5);
    REQUIRE(before.size() == 2);

    for (int round = 0; round < 10; round++) {
      CAPTURE(round);
      s.begin(2, p);
      REQUIRE(s.i(2, "n" + std::to_string(round), "t2") == 0);
      REQUIRE(s.w(2, R2, std::to_string(round)) == 0);
      if (round == 5) {
        REQUIRE(s.d(2, R1) == 0);
      }
      CHECK(s.c(2));
      CHECK(s.srows(1, R1, R5) == before);
    }
    CHECK(s.c(1));
    CHECK(s.final_value(R2) == "9");
    CHECK(s.final_value(R1) == "-");
  }
}
