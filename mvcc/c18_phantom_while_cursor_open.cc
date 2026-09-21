/*
 * c18 -- an insert into a range whose cursor is still open.
 *
 *   T1: open cursor over R1..R5, take the first row
 *   T2: r(R3) i(R2) c          the insert lands inside T1's
 *                              open range: T1 --rw--> T2
 *   T1: finish the scan (R2 is not in it), w(R3)
 *                              T2 read R3:  T2 --rw--> T1
 *   T1: c
 *
 * The range is registered when the cursor opens, before its
 * snapshot, so a row inserted while it is open is caught even
 * though the cursor never returns it (DESIGN.md §10.2).
 *
 * Expected:
 *   RR    both commit
 *   RR+V  both commit
 *   V+V   T1 fails at commit
 */

#include "mvcc.h"

TEST_CASE("c18: a phantom inserted while the cursor is open is caught") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R3});
    s.begin(1, p);
    s.begin(2, p);

    sm_scan_t *cur = sm_scan(s.session(1), s.rel(), R1, 2, 1, R5, 2, 1);
    REQUIRE_MESSAGE(cur, sm_errmsg());
    const char *k, *d;
    size_t kl, dl;
    REQUIRE(sm_scannxt(cur, &k, &kl, &d, &dl) == 0);
    CHECK(std::string(k, kl) == R1);

    CHECK(s.r(2, R3) == "0");
    REQUIRE(s.i(2, R2, "t2") == 0);
    CHECK(s.c(2));

    int rest = 0;
    while (sm_scannxt(cur, &k, &kl, &d, &dl) == 0) {
      CHECK(std::string(k, kl) != R2); // the phantom stays invisible
      rest++;
    }
    CHECK(rest == 1); // R3
    sm_scanend(cur);

    REQUIRE(s.w(1, R3, "t1") == 0);
    bool c1 = s.c(1);
    CHECK(c1 == !both_serializable(p));
  }
}
