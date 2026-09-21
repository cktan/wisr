/*
 * b10 -- a write waits for the row lock, then proceeds.
 *
 *   T1: w(R1)          (holds the row lock, uncommitted)
 *   T2: w(R1)          -> blocks, in another thread
 *   T1: a              -> T2 wakes and succeeds
 *
 * Expected in every pairing: an aborted write is no write at
 * all, so the waiter has nothing to lose to. The pair b09/b10
 * is the whole of the write-write rule: wait, then compare.
 */

#include "mvcc.h"

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE("b10: a second writer blocks, then proceeds when the first aborts") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s({R1});
    s.begin(1, p);
    s.begin(2, p);
    REQUIRE(s.w(1, R1, "t1") == 0);

    std::atomic<bool> returned{false};
    int rc = 0;
    std::thread t2([&] {
      rc = s.w(2, R1, "t2");
      returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_FALSE(returned);

    s.a(1);
    t2.join();
    CHECK(rc == 0);
    CHECK(s.c(2));
    CHECK(s.final_value(R1) == "t2");
  }
}
