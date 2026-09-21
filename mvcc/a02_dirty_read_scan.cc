/*
 * a02 -- dirty read through a scan, for an insert and a delete.
 *
 *   T1: i(R3) d(R2)    T2: s(R1..R3) -> {R1, R2}
 *   T1: c              T2: s(R1..R3) -> {R1, R2}
 *
 * Expected in every pairing: a scan shows neither an
 * uncommitted insert nor an uncommitted delete, and does not
 * change when the writer commits.
 */

#include "mvcc.h"

TEST_CASE("a02: uncommitted inserts and deletes are invisible to a scan") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.i(1, R3, "t1") == 0);
    REQUIRE(s.d(1, R2) == 0);

    CHECK(s.s(2, R1, R3) == std::vector<std::string>{R1, R2});
    CHECK(s.c(1));
    CHECK(s.s(2, R1, R3) == std::vector<std::string>{R1, R2});
    CHECK(s.c(2));
    CHECK(s.final_keys() == std::vector<std::string>{R1, R3});
  }
}
