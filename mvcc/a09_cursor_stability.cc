/*
 * a09 -- a cursor does not see writes made after it opened.
 *
 *   T1: open cursor over R1..R3
 *   T1: for each row the cursor returns: w(that row), i(R4)
 *   T1: the cursor still returns R1, R2, R3 with their old
 *       values, and never R4
 *
 * Expected in every pairing: Halloween protection. This is
 * what makes "scan a range and update every row in it"
 * terminate.
 */

#include "mvcc.h"

TEST_CASE("a09: a cursor keeps the view it opened with") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2, R3});
    s.begin(1, p);
    sm_scan_t *cur = sm_scan(s.session(1), s.rel(), R1, 2, 1, R3, 2, 1);
    REQUIRE_MESSAGE(cur, sm_errmsg());

    std::vector<std::string> seen;
    const char *k, *d;
    size_t kl, dl;
    while (sm_scannxt(cur, &k, &kl, &d, &dl) == 0) {
      std::string row(k, kl);
      CHECK(std::string(d, dl) == "0"); // never a value written below
      seen.push_back(row);
      REQUIRE(s.w(1, row, "t1") == 0);
      if (seen.size() == 1) {
        REQUIRE(s.i(1, R4, "t1") == 0);
      }
    }
    sm_scanend(cur);
    CHECK(seen == std::vector<std::string>{R1, R2, R3});
    CHECK(s.s(1, R1, R4) == std::vector<std::string>{R1, R2, R3, R4});
    CHECK(s.c(1));
  }
}
