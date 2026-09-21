/*
 * b15 -- a write after the reader is done.
 *
 *   T1: r(R1) c
 *   T2: w(R1) c
 *
 * Expected in every pairing: nothing to detect. This is the
 * serial history T1, T2, and the write must not be refused --
 * a level that conflicted here would make read-then-write
 * impossible.
 */

#include "mvcc.h"

TEST_CASE("b15: writing a row after its reader committed is no conflict") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    CHECK(s.r(1, R1) == "0");
    CHECK(s.c(1));

    s.begin(2, p);
    CHECK(s.w(2, R1, "t2") == 0);
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t2");
  }
}
