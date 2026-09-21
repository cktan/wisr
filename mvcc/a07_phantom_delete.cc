/*
 * a07 -- no phantom from a delete.
 *
 *   T1: s(R1..R3) -> {R1, R2, R3}
 *   T2: d(R2) c
 *   T1: s(R1..R3) -> {R1, R2, R3}
 *
 * Expected in every pairing: a range keeps its membership for
 * the whole of T1, disappearances included.
 */

#include "mvcc.h"

TEST_CASE("a07: a row deleted after the snapshot stays in the range") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R3});
    s.begin(1, p);
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R2, R3});

    s.begin(2, p);
    REQUIRE(s.d(2, R2) == 0);
    CHECK(s.c(2));

    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R2, R3});
    CHECK(s.c(1));
    CHECK(s.final_keys() == std::vector<std::string>{R1, R3});
  }
}
