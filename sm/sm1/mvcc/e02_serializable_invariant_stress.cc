/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * e02 -- the invariant holds under contention at S+S.
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
  s.begin_at(1, sm1::xctlevel_t::repeatable_read);
  for (const char *d : DOCS) {
    REQUIRE(s.w(1, d, "1") == 0);
  }
  CHECK(s.c(1));

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0}, violations{0}, commits{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    sm1::session_t *sess = s.session(2 + i);
    threads.emplace_back([&, i, sess] {
      std::string me = DOCS[i];
      for (int round = 0; round < 50; round++) {
        if (sm1::begin(sess, sm1::xctlevel_t::serializable) != 0) {
          errors++;
          return;
        }
        // Count the doctors on call in this snapshot.
        sm1::scan_t *c = nullptr;
        sm1::scan(sess, s.store(), DOCS[0], 2, 1, DOCS[3], 2, 1, c);
        int on = 0;
        std::string_view k, d;
        while (c && sm1::scannxt(c, k, d) == 0) {
          on += std::string(d) == "1";
        }
        sm1::scanend(c);
        const char *v = on >= 2 ? "0" : "1";
        if (sm1::update(sess, s.store(), me, std::string_view(v, 1)) == 0 &&
            sm1::commit(sess) == 0) {
          commits++;
        } else if (!sm1::retryable()) {
          errors++;
          sm1::abort(sess);
          return;
        }
      }
    });
  }
  // The checker reads committed states only.
  sm1::session_t *check = s.session(6);
  std::thread checker([&] {
    while (!stop) {
      if (sm1::begin(check, sm1::xctlevel_t::serializable) != 0) {
        errors++;
        return;
      }
      int on = 0;
      for (const char *d : DOCS) {
        std::string_view data;
        if (sm1::read(check, s.store(), std::string_view(d, 2), data) == 0) {
          on += std::string(data) == "1";
        }
      }
      if (on == 0) {
        violations++;
      }
      if (sm1::commit(check) != 0 && !sm1::retryable()) {
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
