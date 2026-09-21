/*
 * c15 -- writing inside a range you scanned yourself.
 *
 *   T1: s(R1..R3) -> {R1, R2}
 *   T1: w(R1) i(R3)
 *   T1: c
 *
 * A transaction's own writes into its own predicate are not a
 * conflict: an rw-edge needs two different transactions.
 *
 * Expected: OK in every pairing, with a concurrent T2 reading
 * elsewhere to prove the tracking is on and simply has nothing
 * to report.
 */

#include "mvcc.h"

TEST_CASE("c15: a transaction may write inside the range it scanned") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R5});
    s.begin(1, p);
    s.begin(2, p);
    CHECK(s.s(1, R1, R3) == std::vector<std::string>{R1, R2});
    CHECK(s.r(2, R5) == "0"); // T2 is tracked, but elsewhere

    REQUIRE(s.w(1, R1, "t1") == 0);
    REQUIRE(s.i(1, R3, "t1") == 0);
    CHECK(s.c(1));
    CHECK(s.c(2));
    CHECK(s.final_keys() == std::vector<std::string>{R1, R2, R3, R5});
  }
}
