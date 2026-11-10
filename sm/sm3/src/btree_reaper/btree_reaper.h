/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * btree_reaper.h -- the queue of btree leaves that need reaping
 * (../BTREEINDEX.md §9). A hint: a leaf's in-doubt bits are in the
 * page, so a page lost here is queued again by its next reader or
 * writer. A catalog has one queue.
 */
#ifndef SM3_BTREE_REAPER_H
#define SM3_BTREE_REAPER_H

#include "sm1.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sm3 {

struct catalog_t;

/* A btree leaf: its store, its tree's relid, and its page id. */
struct reap_page_t {
  uint64_t storeid = 0;
  uint64_t pageid = 0;
  uint16_t relid = 0;
};

/* What reap_queue_t::note did with a page. */
enum class reap_note_t {
  queued, /* the page is on the queue */
  known,  /* its filter bit was set: on the queue, or a collision */
  full,   /* the queue is full: the page is dropped */
};

/* What reap_queue_t::take found at the head. */
enum class reap_take_t {
  page,  /* out holds a page to reap, its filter bit clear */
  waits, /* a page that waits: back at the tail, unread */
  empty, /* nothing on the queue */
};

/*
 * A circular queue of pages behind a 1-bit Bloom filter of one hash
 * function. One hash function lets a take clear a page's bit: a page
 * sharing that bit is then only queued twice. With two or more,
 * clearing a bit could hide another page.
 */
class reap_queue_t {
public:
  /* A queue of slots pages, filtered by filter_bits, a power of two. */
  explicit reap_queue_t(size_t slots = 262144, size_t filter_bits = size_t(1)
                                                                    << 23);

  /*
   * Queue page, as a reader or writer that finds it not all visible:
   * 1. Load its filter word, and stop if the bit is set. The load
   *    keeps the shared word clean on the read path.
   * 2. Set the bit, and stop if another caller set it first.
   * 3. Under the mutex, if the queue is full, clear the bit and stop.
   * 4. Put the page in at the tail.
   */
  reap_note_t note(const reap_page_t &page);

  /*
   * Take the page at the head, for a pass at watermark: a page whose
   * slot holds watermark waits, and goes back to the tail; any other
   * has its filter bit cleared, before the caller reads it, so a
   * change after that queues it again.
   */
  reap_take_t take(uint64_t watermark, reap_page_t &out);

  /*
   * Put page in at the tail, its filter bit set so its readers do not
   * queue it again, to wait until the watermark moves past watermark.
   * If the queue is full the page is dropped, and its bit cleared.
   */
  void wait(const reap_page_t &page, uint64_t watermark);

  /*
   * Take every page of tree relid of store storeid off the queue, for
   * VACUUM, the waiting ones too, into out, in queue order:
   * 1. Under the mutex, walk the slots head to tail.
   * 2. Move a page of another tree up, keeping its order.
   * 3. Put a page of this tree in out, and clear its filter bit, as
   *    take does, so a change after this queues it again.
   */
  void extract(uint64_t storeid, uint16_t relid, std::vector<reap_page_t> &out);

  /* The pages on the queue. */
  size_t size() const;

  /* Whether no page can be put in. */
  bool full() const;

  /* Whether the queue holds pages, and every one waits. */
  bool all_wait() const;

private:
  /* A page on the queue; watermark 0 is a page that does not wait. */
  struct slot_t {
    reap_page_t page;
    uint64_t watermark = 0;
  };

  /* The filter's word and mask for page. */
  std::atomic<uint64_t> &filter_word(const reap_page_t &page,
                                     uint64_t &mask) const;

  /* Put a slot in at the tail. The caller holds m_mu, and there is room. */
  void push(const slot_t &slot);

  mutable std::mutex m_mu;
  std::unique_ptr<slot_t[]> m_slots;
  size_t m_cap;
  size_t m_head = 0;
  size_t m_count = 0;
  size_t m_waiting = 0;
  std::unique_ptr<std::atomic<uint64_t>[]> m_filter;
  size_t m_filter_bits;
};

/* A catalog's btree reaper: its queue, and its worker threads. */
struct btree_reaper_t {
  catalog_t *cat = nullptr;
  reap_queue_t queue;
  std::function<sm1::session_t *()> open; /* a session for a pass */
  std::function<size_t()> row_pending;    /* keys the row pass waits on */
  std::mutex mu;                          /* guards stopping */
  std::condition_variable cv;             /* stopping, or the queue full */
  bool stopping = false;
  std::atomic<bool> stop{false}; /* stopping, read by a pass */
  std::vector<std::thread> workers;
  uint64_t begins = 0; /* cat->begins at worker 0's last look */
};

/* The transactions a second above which the system is busy. */
constexpr uint64_t btree_reaper_busy_limit = 100;

/* The pages a second the first worker reaps on a busy system. */
constexpr size_t btree_reaper_busy_pages = 100;

/*
 * The most pages the first worker's next pass reaps
 * (../BTREEINDEX.md §9), given the transactions begun since its last
 * look, at most limit if the system is not busy.
 * 1. An empty queue: 0.
 * 2. Every slot waiting, and no transaction begun: 0. Opening a session
 *    for it would keep an idle database from closing.
 * 3. A full queue, or a system not busy: all of them.
 * 4. A busy system: btree_reaper_busy_pages, so steady load is still reaped,
 *    at a rate it barely feels.
 */
size_t btree_reaper_budget(const reap_queue_t &queue, uint64_t begun,
                           uint64_t limit);

/* Queue a leaf, as sm2::leaf_note_fn; ctx is the btree_reaper_t. */
void btree_reaper_note(void *ctx, uint64_t storeid, uint16_t relid,
                       uint64_t pageid);

} // namespace sm3

#endif
