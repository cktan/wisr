/*
 * a06 -- no phantom from an insert.
 *
 *   T1: s(R1..R3) -> {R1, R3}
 *   T2: i(R2) c
 *   T1: s(R1..R3) -> {R1, R3},  r(R2) -> -
 *
 * Expected in every pairing: the insert is outside T1's
 * snapshot, so no repeated read of the range can show it.
 * Whether T1 may then WRITE is scenario c03.
 */

#include "mvcc.h"

TEST_CASE("a06: a row inserted after the snapshot is no phantom") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R3});
    s.begin(1, p);
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R3});

    s.begin(2, p);
    REQUIRE(s.i(2, R2, "t2") == 0);
    CHECK(s.c(2));

    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R3});
    CHECK(s.s(1, R2, R2) == std::vector<std::string>{});
    CHECK(s.r(1, R2) == "-");
    CHECK(s.c(1));
    CHECK(s.final_keys() == std::vector<std::string>{R1, R2, R3});
  }
}
