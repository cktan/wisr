/*
 * test_cleaner.cc -- the btree cleaner (../BTREE.md §7.1): its queue
 * of pages and their filter.
 */

#include "doctest.h"

#include "../src/cleaner/cleaner.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using sm3::clean_note_t;
using sm3::clean_page_t;
using sm3::clean_queue_t;
using sm3::clean_take_t;

/* Page n of store 7, tree 3. */
clean_page_t page_of(uint64_t n) { return {7, n, 3}; }

} // namespace

TEST_CASE("cleaner queue: pages come out in order, and the queue wraps") {
  // Verify the queue is first in, first out, across its array's end.
  // 1. Fill a queue of four slots.
  // 2. Take two, put in two more, so the tail wraps.
  // 3. All four come out in the order they went in, then none.
  clean_queue_t q(4, 1 << 16);
  clean_page_t p;
  for (uint64_t n = 1; n <= 4; n++) {
    REQUIRE(clean_note_t::queued == q.note(page_of(n)));
  }
  CHECK(q.full());
  for (uint64_t n = 1; n <= 2; n++) {
    REQUIRE(clean_take_t::page == q.take(1, p));
    CHECK(n == p.pageid);
  }
  REQUIRE(clean_note_t::queued == q.note(page_of(5)));
  REQUIRE(clean_note_t::queued == q.note(page_of(6)));
  for (uint64_t n = 3; n <= 6; n++) {
    REQUIRE(clean_take_t::page == q.take(1, p));
    CHECK(n == p.pageid);
    CHECK(7u == p.storeid);
    CHECK(3u == p.relid);
  }
  CHECK(clean_take_t::empty == q.take(1, p));
  CHECK(0u == q.size());
}

TEST_CASE("cleaner queue: the filter keeps a page off the queue once") {
  // Verify a page on the queue is not queued again, and that a take
  // lets it in again.
  // 1. Queue a page twice: the second is known, and one slot is used.
  // 2. Take it: the same page queues again.
  clean_queue_t q(4, 1 << 16);
  clean_page_t p;
  CHECK(clean_note_t::queued == q.note(page_of(1)));
  CHECK(clean_note_t::known == q.note(page_of(1)));
  CHECK(1u == q.size());
  REQUIRE(clean_take_t::page == q.take(1, p));
  CHECK(clean_note_t::queued == q.note(page_of(1)));
}

TEST_CASE("cleaner queue: a full queue drops a new page and clears its bit") {
  // Verify a page refused leaves no bit, so a later try gets in.
  // 1. Fill a queue of two slots; a third page is refused.
  // 2. Refused again, not known: its bit was cleared.
  // 3. After a take it goes in.
  clean_queue_t q(2, 1 << 16);
  clean_page_t p;
  REQUIRE(clean_note_t::queued == q.note(page_of(1)));
  REQUIRE(clean_note_t::queued == q.note(page_of(2)));
  CHECK(clean_note_t::full == q.note(page_of(3)));
  CHECK(clean_note_t::full == q.note(page_of(3)));
  REQUIRE(clean_take_t::page == q.take(1, p));
  CHECK(clean_note_t::queued == q.note(page_of(3)));
}

TEST_CASE("cleaner queue: a page waits until the watermark moves") {
  // Verify a page put back is not handed out at its own watermark.
  // 1. Queue a page, take it, and put it back to wait at watermark 5.
  // 2. Its filter bit is set: a reader does not queue it again.
  // 3. Every slot waits; a take at 5 leaves it, unread.
  // 4. A take at 6 hands it out, and no slot waits.
  // 5. On a full queue a page put back is dropped, its bit clear.
  clean_queue_t q(2, 1 << 16);
  clean_page_t p;
  REQUIRE(clean_note_t::queued == q.note(page_of(1)));
  REQUIRE(clean_take_t::page == q.take(5, p));
  CHECK_FALSE(q.all_wait());
  q.wait(p, 5);
  CHECK(clean_note_t::known == q.note(page_of(1)));
  CHECK(q.all_wait());
  CHECK(clean_take_t::waits == q.take(5, p));
  CHECK(1u == q.size());
  CHECK(q.all_wait());
  REQUIRE(clean_take_t::page == q.take(6, p));
  CHECK(1u == p.pageid);
  CHECK_FALSE(q.all_wait());
  CHECK(0u == q.size());

  REQUIRE(clean_note_t::queued == q.note(page_of(2)));
  REQUIRE(clean_note_t::queued == q.note(page_of(3)));
  q.wait(page_of(1), 7);
  CHECK(2u == q.size());
  REQUIRE(clean_take_t::page == q.take(9, p));
  CHECK(clean_note_t::queued == q.note(page_of(1)));
}

TEST_CASE("cleaner queue: writers and cleaners on many threads lose no page") {
  // Verify the queue under concurrent notes and takes.
  // 1. Four threads queue 20,000 pages each, again until each is known
  //    or queued, while two take and keep what they get.
  // 2. Queue again what no take saw: a bit that collided is healed by
  //    the next finder, as the design says.
  // 3. Every page was taken at least once.
  constexpr uint64_t per_thread = 20000;
  constexpr int writers = 4;
  clean_queue_t q(1024, 1 << 22);
  std::mutex mu;
  std::set<uint64_t> seen;
  std::atomic<int> running{writers};
  auto taker = [&] {
    std::vector<uint64_t> mine;
    clean_page_t p;
    for (;;) {
      clean_take_t t = q.take(1, p);
      if (clean_take_t::page == t) {
        mine.push_back(p.pageid);
      } else if (clean_take_t::empty == t && 0 == running) {
        break;
      }
    }
    std::lock_guard lock(mu);
    seen.insert(mine.begin(), mine.end());
  };
  std::vector<std::thread> threads;
  threads.emplace_back(taker);
  threads.emplace_back(taker);
  for (int w = 0; w < writers; w++) {
    threads.emplace_back([&, w] {
      for (uint64_t n = 0; n < per_thread; n++) {
        while (clean_note_t::full == q.note(page_of(w * per_thread + n))) {
          std::this_thread::yield();
        }
      }
      running--;
    });
  }
  for (std::thread &t : threads) {
    t.join();
  }
  clean_page_t p;
  for (uint64_t n = 0; n < writers * per_thread; n++) {
    if (0 == seen.count(n)) {
      REQUIRE(clean_note_t::queued == q.note(page_of(n)));
      REQUIRE(clean_take_t::page == q.take(1, p));
      seen.insert(p.pageid);
    }
  }
  CHECK(writers * per_thread == seen.size());
}

TEST_CASE("cleaner: a busy system is still cleaned, at a low rate") {
  // Verify the first worker's budget (../BTREE.md §7.1).
  // 1. An empty queue has no pass, however idle.
  // 2. A queue with room is cleaned whole below the limit.
  // 3. Above the limit it gets the busy budget, not none: steady load
  //    must not stop the cleaning.
  // 4. A full queue is cleaned whole above the limit too.
  // 5. Pages that all wait get no pass with no transaction begun, so no
  //    session keeps an idle database from closing; they get one once a
  //    transaction begins.
  clean_queue_t q(2, 1 << 16);
  clean_page_t p;
  CHECK(0u == sm3::cleaner_budget(q, 0, 100));
  REQUIRE(clean_note_t::queued == q.note(page_of(1)));
  CHECK(SIZE_MAX == sm3::cleaner_budget(q, 0, 100));
  CHECK(SIZE_MAX == sm3::cleaner_budget(q, 100, 100));
  CHECK(sm3::cleaner_busy_pages == sm3::cleaner_budget(q, 101, 100));
  REQUIRE(clean_note_t::queued == q.note(page_of(2)));
  CHECK(SIZE_MAX == sm3::cleaner_budget(q, 101, 100));
  REQUIRE(clean_take_t::page == q.take(1, p));
  REQUIRE(clean_take_t::page == q.take(1, p));
  q.wait(page_of(1), 5);
  CHECK(0u == sm3::cleaner_budget(q, 0, 100));
  CHECK(SIZE_MAX == sm3::cleaner_budget(q, 1, 100));
}
