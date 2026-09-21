/*
 * c07 -- Fekete's read-only anomaly.
 *
 * Savings R1 and checking R2 both hold 0, and a withdrawal
 * from an overdrawn total costs 1 more.
 *
 *   T2: r(R1) r(R2)        decides the withdrawal costs 12
 *   T1: w(R1)=20 c         the deposit lands
 *   T3: r(R1) r(R2)        sees 20 and 0  -- read-only
 *   T2: w(R2)=-12 c
 *
 * T3 sees a state no serial order produces: if T2 ran first,
 * T3 would see the penalty; if T1 ran first, T2 would not have
 * charged one.
 *
 * Expected:
 *   RR    all three commit and T3 observes the anomaly
 *   RR+V  all three commit
 *   V+V   not all three commit -- SSI fails one of them, and
 *         T3 is a full participant even though it only reads
 */

#include "mvcc.h"

TEST_CASE("c07: the read-only anomaly") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1, R2});
    s.begin(2, p);
    int x = std::stoi(s.r(2, R1)), y = std::stoi(s.r(2, R2));
    int withdrawal = x + y >= 11 ? 11 : 12;

    s.begin(1, p);
    REQUIRE(s.w(1, R1, "20") == 0);
    bool c1 = s.c(1);
    CHECK(c1);

    s.begin(3, p);
    std::string seen_x = s.r(3, R1), seen_y = s.r(3, R2);
    bool c3 = s.c(3);

    REQUIRE(s.w(2, R2, std::to_string(-withdrawal)) == 0);
    bool c2 = s.c(2);

    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2 && c3));
    } else {
      CHECK(c2);
      CHECK(c3);
      // The anomaly itself: the deposit without the penalty.
      CHECK(seen_x == "20");
      CHECK(seen_y == "0");
      CHECK(s.final_value(R2) == "-12");
    }
  }
}
