/*
 * b11 -- two inserts of the same absent row.
 *
 *   T1: i(R2)          (holds the row lock, uncommitted)
 *   T2: i(R2)          -> blocks, in another thread
 *   T1: c              -> T2 wakes and gets CONFLICT
 *
 * Expected in every pairing: an insert takes the row lock on a
 * key that does not exist yet, so two inserters serialize on
 * it rather than both succeeding.
 */

#include "mvcc.h"

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("b11: two transactions inserting one row serialize on its lock") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.i(1, R2, "t1") == 0);

    std::atomic<bool> returned{false};
    int rc = 0, err = 0;
    std::thread t2([&] {
      rc = s.i(2, R2, "t2");
      err = sm_errno();
      returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_FALSE(returned);

    CHECK(s.c(1));
    t2.join();
    CHECK(rc == -1);
    CHECK(err == SM_ERR_CONFLICT);
    CHECK(s.final_value(R2) == "t1");
  }
}
