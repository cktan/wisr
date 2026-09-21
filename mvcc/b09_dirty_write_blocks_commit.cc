/*
 * b09 -- a write waits for the row lock, then conflicts.
 *
 *   T1: w(R1)          (holds the row lock, uncommitted)
 *   T2: w(R1)          -> blocks, in another thread
 *   T1: c              -> T2 wakes and gets CONFLICT
 *
 * Expected in every pairing: no dirty write. The waiter is
 * released by T1's commit and then loses first-committer-wins,
 * because T1 committed after T2's snapshot (DESIGN.md §5.1).
 */

#include "mvcc.h"

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("b09: a second writer blocks, then conflicts when the first "
          "commits") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.w(1, R1, "t1") == 0);

    std::atomic<bool> returned{false};
    int rc = 0, err = 0;
    std::thread t2([&] {
      rc = s.w(2, R1, "t2");
      err = sm_errno();
      returned = true;
    });
    // Give the waiter time to reach the lock and stay there.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_FALSE(returned);

    CHECK(s.c(1));
    t2.join();
    CHECK(rc == -1);
    CHECK(err == SM_ERR_CONFLICT);
    CHECK(s.final_value(R1) == "t1");
  }
}
