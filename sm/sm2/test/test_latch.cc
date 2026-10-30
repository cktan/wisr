/*
 * test_latch.cc -- the writers' latches on B+tree pages (../BTREE.md
 * §13 step 2): one writer of a page at a time, pages apart in
 * parallel, and no entry left behind. Run under the thread sanitizer
 * by `make tsan`.
 */

#include "doctest.h"

#include "../src/tree/latch.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace sm2::tree;

TEST_CASE("tree latch: one writer of a page at a time") {
  // Verify a page's latch excludes its other writers.
  // 1. Eight threads each latch one page 2,000 times, and add to a
  //    count it guards, unguarded otherwise, while holding it.
  // 2. The count is every addition: no two held the latch at once.
  // 3. The table is empty once all are done.
  latch_table_t table;
  page_name_t page{1, 2, 3, 4};
  long count = 0;
  std::atomic<int> inside{0};
  std::atomic<bool> overlap{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < 2000; i++) {
        page_latch_t latch(table, page);
        if (0 != inside.fetch_add(1)) {
          overlap = true;
        }
        count++;
        inside.fetch_sub(1);
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  CHECK_FALSE(overlap.load());
  CHECK(8 * 2000 == count);
  CHECK(0u == table.size());
}

TEST_CASE("tree latch: pages apart are latched apart") {
  // Verify the latches of different pages do not exclude each other,
  // and that a page is named by all four of its parts.
  // 1. Latch a page, then each page differing from it in one part:
  //    database, store, index, id; none waits.
  // 2. Many threads latch many pages, two at once, child then parent,
  //    as a split does, the pages' counts each guarded by its latch.
  // 3. Every count is whole, and the table is empty.
  latch_table_t table;
  page_name_t a{1, 2, 3, 4};
  page_latch_t la(table, a);
  page_latch_t l1(table, page_name_t{9, 2, 3, 4});
  page_latch_t l2(table, page_name_t{1, 9, 3, 4});
  page_latch_t l3(table, page_name_t{1, 2, 9, 4});
  page_latch_t l4(table, page_name_t{1, 2, 3, 9});
  CHECK(5u == table.size());
  la.release();
  l1.release();
  l2.release();
  l3.release();
  l4.release();
  CHECK(0u == table.size());

  constexpr int pages = 16;
  std::vector<long> counts(pages, 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 3000; i++) {
        // A child above its parent's id, so every thread takes the
        // two in one order: higher id first.
        int child = 1 + (t * 7 + i) % (pages - 1);
        int parent = (t + i) % child;
        page_latch_t lc(table, page_name_t{1, 1, 1, uint64_t(child)});
        page_latch_t lp(table, page_name_t{1, 1, 1, uint64_t(parent)});
        counts[child]++;
        counts[parent]++;
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  long total = 0;
  for (long c : counts) {
    total += c;
  }
  CHECK(8 * 3000 * 2 == total);
  CHECK(0u == table.size());
}

TEST_CASE("tree latch: a thread counts the latches it holds") {
  // Verify latches_held counts this thread's latches alone.
  // 1. Hold two latches: two. Another thread holding one sees one.
  // 2. Release them: none.
  latch_table_t table;
  CHECK(0u == latches_held());
  page_latch_t a(table, page_name_t{1, 1, 1, 1});
  page_latch_t b(table, page_name_t{1, 1, 1, 2});
  CHECK(2u == latches_held());
  size_t other = 99;
  std::thread t([&] {
    page_latch_t c(table, page_name_t{1, 1, 1, 3});
    other = latches_held();
  });
  t.join();
  CHECK(1u == other);
  a.release();
  b.release();
  CHECK(0u == latches_held());
}
