/*
 * c11 -- a cursor closed early covers only what it read.
 *
 *   T1: open cursor over R1..R5, take R1, close
 *   T2: i(R4)          (past where T1 stopped)
 *   T2: r(R2)
 *   T1: w(R2)          -> T2 --rw--> T1 only
 *   both commit
 *
 * The predicate T1 registered at sm_scan is shrunk at
 * sm_scanend to the part the cursor actually traversed, so
 * T2's insert past that point creates no edge back.
 *
 * Expected: OK in every pairing, V+V included.
 */

#include "mvcc.h"

TEST_CASE("c11: a predicate shrinks to the part the cursor traversed") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(1, p);
    s.begin(2, p);

    sm_scan_t *cur = sm_scan(s.session(1), s.rel(), R1, 2, 1, R5, 2, 1);
    REQUIRE_MESSAGE(cur, sm_errmsg());
    const char *k, *d;
    size_t kl, dl;
    REQUIRE(sm_scannxt(cur, &k, &kl, &d, &dl) == 0);
    CHECK(std::string(k, kl) == R1);
    sm_scanend(cur); // stopped at R1

    REQUIRE(s.i(2, R4, "t2") == 0);
    CHECK(s.r(2, R2) == "0");
    REQUIRE(s.w(1, R2, "t1") == 0);

    CHECK(s.c(1));
    CHECK(s.c(2));
  }
}
