/*
 * e05 -- many RR transactions reading everything and writing
 * one row apiece.
 *
 *   T1..T4: each s(all), then w(its own row), then c
 *
 * Every pair of them has rw-edges in both directions, which is
 * exactly the shape SERIALIZABLE refuses (c02, c03). At
 * REPEATABLE READ all four must commit: this is the cost RR
 * declines to pay, and the reason a caller picks it.
 */

#include "mvcc.h"

#include <string>
#include <vector>

TEST_CASE("e05: four RR transactions that read everything all commit") {
  scenario_t s({R1, R2, R3, R4});
  const std::string rows[] = {R1, R2, R3, R4};
  for (int n = 1; n <= 4; n++) {
    s.begin_at(n, SM_XCT_REPEATABLE_READ);
    CHECK(s.s(n, R1, R4).size() == 4);
  }
  for (int n = 1; n <= 4; n++) {
    CAPTURE(n);
    REQUIRE(s.w(n, rows[n - 1], "t" + std::to_string(n)) == 0);
    CHECK(s.c(n));
  }
  CHECK(s.final_value(R1) == "t1");
  CHECK(s.final_value(R4) == "t4");
}
