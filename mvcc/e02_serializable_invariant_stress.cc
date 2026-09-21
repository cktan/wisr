/*
 * e02 -- the invariant holds under contention at V+V.
 *
 * Four rows are "doctors", each on call ("1"). Four threads,
 * one per row, repeatedly: scan the four rows, go off call if
 * at least two are on, go back on otherwise, and commit,
 * retrying every retryable failure. A fifth thread checks, in
 * its own SERIALIZABLE transaction, that at least one doctor
 * is always on call.
 *
 * The rule each thread enforces is only correct if the
 * transactions are serializable: c04 shows two of them
 * breaking it at REPEATABLE READ. Here nothing may break it,
 * however the schedules interleave.
 */

#include "mvcc.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("e02: SERIALIZABLE keeps the invariant under contention") {
  const char *const DOCS[] = {"d0", "d1", "d2", "d3"};
  scenario_t s({DOCS[0], DOCS[1], DOCS[2], DOCS[3]});
  // Every doctor starts on call.
  s.begin_at(1, SM_XCT_REPEATABLE_READ);
  for (const char *d : DOCS) {
    REQUIRE(s.w(1, d, "1") == 0);
  }
  CHECK(s.c(1));

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0}, violations{0}, commits{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    sm_session_t *sess = s.session(2 + i);
    threads.emplace_back([&, i, sess] {
      std::string me = DOCS[i];
      for (int round = 0; round < 50; round++) {
        if (sm_begin(sess, SM_XCT_SERIALIZABLE) != 0) {
          errors++;
          return;
        }
        // Count the doctors on call in this snapshot.
        sm_scan_t *c = sm_scan(sess, s.rel(), DOCS[0], 2, 1, DOCS[3], 2, 1);
        int on = 0;
        const char *k, *d;
        size_t kl, dl;
        while (c && sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
          on += std::string(d, dl) == "1";
        }
        sm_scanend(c);
        const char *v = on >= 2 ? "0" : "1";
        if (sm_update(sess, s.rel(), me.data(), me.size(), v, 1) == 0 &&
            sm_commit(sess) == 0) {
          commits++;
        } else if (!sm_retryable()) {
          errors++;
          sm_abort(sess);
          return;
        }
      }
    });
  }
  // The checker reads committed states only.
  sm_session_t *check = s.session(6);
  std::thread checker([&] {
    while (!stop) {
      if (sm_begin(check, SM_XCT_SERIALIZABLE) != 0) {
        errors++;
        return;
      }
      int on = 0;
      for (const char *d : DOCS) {
        const char *data = nullptr;
        size_t len = 0;
        if (sm_read(check, s.rel(), d, 2, &data, &len) == 0) {
          on += std::string(data, len) == "1";
        }
      }
      if (on == 0) {
        violations++;
      }
      if (sm_commit(check) != 0 && !sm_retryable()) {
        errors++;
      }
    }
  });
  for (std::thread &th : threads) {
    th.join();
  }
  stop = true;
  checker.join();
  CHECK(errors == 0);
  CHECK(violations == 0);
  CHECK(commits > 0);
  MESSAGE("commits: " << commits.load());
}
