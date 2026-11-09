/*
 * test_hintq.cc -- hint_queue_t (../src/hintq.h), the bounded queue the
 * compaction filter and readers fill and the GC worker empties.
 */
#include "doctest.h"

#include "hintq.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace wisr;

namespace {

/* Every record of a batch, as "storeid/xid/key". */
std::vector<std::string> records_of(const hint_queue_t::batch_t &b) {
  std::vector<std::string> out;
  b.for_each([&](const hint_queue_t::record_t &r) {
    out.push_back(std::to_string(r.storeid) + "/" + std::to_string(r.xid) +
                  "/" + std::string(r.key));
  });
  return out;
}

} // namespace

TEST_CASE("a queue hands back what was pushed, in order") {
  // Verify push and take keep each record's store, xid and key.
  // 1. Push three keys, one empty and one with a NUL byte.
  // 2. Take: the three come back in order, with their fields.
  // 3. Take again: the queue is empty.
  hint_queue_t q(1 << 16, 1 << 12);
  CHECK(q.push(1, 10, "a"));
  CHECK(q.push(2, 20, std::string_view("b\0c", 3)));
  CHECK(q.push(3, 30, ""));
  CHECK(q.size() == 3);
  hint_queue_t::batch_t b = q.take();
  CHECK(b.count() == 3);
  std::vector<std::string> got = records_of(b);
  REQUIRE(got.size() == 3);
  CHECK(got[0] == "1/10/a");
  CHECK(got[1] == std::string("2/20/b\0c", 8));
  CHECK(got[2] == "3/30/");
  CHECK(q.size() == 0);
  CHECK(q.take().count() == 0);
}

TEST_CASE("a queue holds a key once, until it is taken") {
  // Verify the filter stops a key already queued, and a take clears it.
  // 1. Push a key twice and another store's same key: two queued.
  // 2. Take, then push the key again: it is queued again.
  hint_queue_t q(1 << 16, 1 << 20);
  CHECK(q.push(1, 7, "k"));
  CHECK_FALSE(q.push(1, 7, "k"));
  CHECK(q.push(2, 7, "k"));
  CHECK(q.size() == 2);
  CHECK(q.take().count() == 2);
  CHECK(q.push(1, 7, "k"));
  CHECK(q.size() == 1);
}

TEST_CASE("a full queue drops a key, and stays the same size") {
  // Verify the buffer is bounded and a dropped key can be queued later.
  // 1. Make a queue of 1,000 bytes and push 100 distinct keys.
  // 2. Some were dropped, and the bytes queued never pass 1,000.
  // 3. Take: the queue accepts the first dropped key now.
  const size_t bytes = 1000;
  hint_queue_t q(bytes, 1 << 20);
  std::vector<std::string> dropped;
  size_t queued = 0;
  for (int i = 0; i < 100; i++) {
    std::string k = "key" + std::to_string(i);
    if (q.push(1, 0, k)) {
      queued++;
    } else {
      dropped.push_back(k);
    }
  }
  CHECK(queued == q.size());
  CHECK(queued < 100);
  CHECK(queued * hint_queue_t::header <= bytes);
  REQUIRE_FALSE(dropped.empty());
  hint_queue_t::batch_t b = q.take();
  CHECK(b.count() == queued);
  CHECK(q.push(1, 0, dropped[0]));
}

TEST_CASE("a record larger than the buffer is dropped") {
  // Verify a key that cannot fit is refused, not stored.
  // 1. Push a key larger than the buffer.
  // 2. It is dropped, and a small key still queues.
  hint_queue_t q(100, 1 << 12);
  CHECK_FALSE(q.push(1, 0, std::string(200, 'x')));
  CHECK(q.size() == 0);
  CHECK(q.push(1, 0, "ok"));
}

TEST_CASE("pushes and takes in many threads lose no queued key") {
  // Verify a swap under producers: a key whose push returned true is in
  // exactly one taken batch, and no record is torn.
  // 1. Start four producers, each pushing 20,000 keys of its own.
  // 2. Take batches until the producers finish, then once more.
  // 3. Every key a push accepted is in the taken records, intact.
  hint_queue_t q(1 << 20, 1 << 22);
  const int producers = 4, per = 20000;
  std::vector<std::vector<std::string>> accepted(producers);
  std::atomic<int> done{0};
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; p++) {
    threads.emplace_back([&, p] {
      for (int i = 0; i < per; i++) {
        std::string k = "p" + std::to_string(p) + "-" + std::to_string(i);
        if (q.push(p, p * 1000 + 1, k)) {
          accepted[p].push_back(k);
        }
      }
      done++;
    });
  }
  std::multiset<std::string> taken;
  auto drain = [&] {
    hint_queue_t::batch_t b = q.take();
    b.for_each([&](const hint_queue_t::record_t &r) {
      CHECK(r.xid == r.storeid * 1000 + 1);
      taken.insert(std::string(r.key));
    });
  };
  while (done.load() < producers) {
    drain();
  }
  for (std::thread &t : threads) {
    t.join();
  }
  drain();
  for (int p = 0; p < producers; p++) {
    for (const std::string &k : accepted[p]) {
      CHECK(taken.count(k) == 1);
    }
  }
}
