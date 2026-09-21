/*
 * a05 -- read-write-read with a delete.
 *
 *   T1: r(R1) -> 0
 *   T2: d(R1) c
 *   T1: r(R1) -> 0, s(R1..R2) -> {R1}
 *
 * Expected in every pairing: a row deleted after T1's snapshot
 * stays visible to T1, through both read paths.
 */

#include "mvcc.h"

TEST_CASE("a05: a row deleted after the snapshot stays visible") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");

    s.begin(2, p);
    REQUIRE(s.d(2, R1) == 0);
    CHECK(s.c(2));

    CHECK(s.r(1, R1) == "0");
    CHECK(s.s(1, R1, R2) == std::vector<std::string>{R1});
    CHECK(s.c(1));
    CHECK(s.final_value(R1) == "-");
  }
}
