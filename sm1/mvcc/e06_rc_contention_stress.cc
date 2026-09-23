/*
 * e06 -- read committed increments on hot counters are never
 * lost and never applied twice, with compaction running.
 *
 * Four threads each add 1 to a hot counter, forty rounds each,
 * choosing among three, through the whole exchange: read, then
 * update; on `updated`, sm1_read_latest and update again. A
 * compaction thread runs meanwhile, so versions are stamped and
 * dropped under the writers' feet. The sum of the counters must
 * be exactly the number of increments (f01, f08, f07 under
 * load).
 */

#include "mvcc.h"

#include "db.h"
#include "session.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <rocksdb/db.h>

TEST_CASE("e06: RC contention loses no increment, with compaction running") {
  const char *rows[] = {R1, R2, R3};
  scenario_t s({R1, R2, R3});
  std::atomic<int> failures{0}, updated{0};
  std::atomic<bool> stop{false};

  // A compaction thread forces the filter to stamp and drop.
  sm1_session_t *csess = s.session(5);
  std::thread compactor([&] {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    while (!stop) {
      wisr::db_t *db = csess->db;
      rocksdb::ColumnFamilyHandle *cf = wisr::db_store_cf(db, s.store());
      if (cf) {
        db->rdb->CompactRange(opt, cf, nullptr, nullptr);
      }
    }
  });

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    sm1_session_t *sess = s.session(1 + i);
    threads.emplace_back([&, sess, i] {
      for (int round = 0; round < 40; round++) {
        const char *row = rows[(round + i) % 3];
        // Read the counter, then add one.
        if (sm1_begin(sess, SM1_XCT_READ_COMMITTED) != 0) {
          failures++;
          return;
        }
        const char *data = nullptr;
        size_t len = 0;
        if (sm1_read(sess, s.store(), row, 2, &data, &len) != 0) {
          failures++;
          return;
        }
        std::string next =
            std::to_string(std::stoi(std::string(data, len)) + 1);
        int rc = sm1_update(sess, s.store(), row, 2, next.data(), next.size(),
                            nullptr, nullptr);
        // The counter moved: read the newest value, add one to that.
        if (rc != 0 && sm1_errno() == SM1_ERR_UPDATED) {
          updated++;
          if (sm1_read_latest(sess, s.store(), row, 2, &data, &len) != 0) {
            failures++;
            return;
          }
          next = std::to_string(std::stoi(std::string(data, len)) + 1);
          rc = sm1_update(sess, s.store(), row, 2, next.data(), next.size(),
                          nullptr, nullptr);
        }
        // Only a deadlock may be retried, and no row order here makes one.
        if (rc != 0 || sm1_commit(sess) != 0) {
          failures++;
          return;
        }
      }
    });
  }
  for (std::thread &th : threads) {
    th.join();
  }
  stop = true;
  compactor.join();

  CHECK(failures == 0);
  MESSAGE("updated: " << updated.load());
  int sum = 0;
  for (const char *row : rows) {
    sum += std::stoi(s.final_value(row));
  }
  CHECK(sum == 160);
}
