/*
 * e03 -- contention at RR costs conflicts, never serialization
 * failures.
 *
 * Four threads each add 1 to one row fifty times, reading the
 * value and writing it back, retrying on failure. At
 * REPEATABLE READ the only retryable failures are the
 * write-write conflict and the deadlock victim: a caller
 * choosing RR never has to handle SM_ERR_SERIALIZATION.
 *
 * The total must be exactly 200, which is first-committer-wins
 * (b01) holding under load: no update is lost.
 */

#include "mvcc.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("e03: RR contention gives CONFLICT, never SERIALIZATION") {
  scenario_t s({R1});
  std::atomic<int> serialization{0}, other{0}, retries{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    sm_session_t *sess = s.session(1 + i);
    threads.emplace_back([&, sess] {
      for (int round = 0; round < 50; round++) {
        for (;;) {
          if (sm_begin(sess, SM_XCT_REPEATABLE_READ) != 0) {
            other++;
            return;
          }
          const char *data = nullptr;
          size_t len = 0;
          int rc = sm_read(sess, s.rel(), R1, 2, &data, &len);
          if (rc != 0) {
            other++;
            return;
          }
          std::string next =
              std::to_string(std::stoi(std::string(data, len)) + 1);
          rc = sm_update(sess, s.rel(), R1, 2, next.data(), next.size(),
                         nullptr, nullptr);
          if (rc == 0 && sm_commit(sess) == 0) {
            break;
          }
          // Classify the failure, then retry the whole step.
          if (sm_errno() == SM_ERR_SERIALIZATION) {
            serialization++;
          } else if (sm_errno() != SM_ERR_CONFLICT &&
                     sm_errno() != SM_ERR_DEADLOCK) {
            other++;
          }
          retries++;
          if (rc == 0) {
            sm_abort(sess); // the commit failed, not the write
          }
        }
      }
    });
  }
  for (std::thread &th : threads) {
    th.join();
  }
  CHECK(serialization == 0);
  CHECK(other == 0);
  MESSAGE("retries: " << retries.load());
  CHECK(s.final_value(R1) == "200");
}
