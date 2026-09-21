/*
 * c12 -- a read-only transaction alongside a writer.
 *
 *   T1: r(R1) r(R2)    (read-only for its whole life)
 *   T2: w(R1) c
 *   T1: r(R1) c
 *
 * T1 --rw--> T2 is the only edge, and T1 writes nothing, so
 * nobody can have an edge into T1.
 *
 * Expected: OK in every pairing. A read-only transaction that
 * failed here would make SERIALIZABLE reporting unusable.
 */

#include "mvcc.h"

TEST_CASE("c12: a read-only transaction commits beside a concurrent writer") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");
    CHECK(s.r(1, R2) == "0");

    s.begin(2, p);
    REQUIRE(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));

    CHECK(s.r(1, R1) == "0");
    CHECK(s.c(1));
  }
}
