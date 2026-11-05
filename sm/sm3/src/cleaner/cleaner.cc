/*
 * cleaner.cc -- the btree cleaner (../../../BTREE.md §7.1).
 */

#include "../catalog/internal.h"

#include "sm0_defer.h"
#include "sm0_log.h"

#include <cassert>
#include <chrono>

namespace sm3 {

namespace {

/* A 64-bit finalizer: every input bit reaches every output bit. */
uint64_t mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

} // namespace

/* Make a queue; see cleaner.h. */
clean_queue_t::clean_queue_t(size_t slots, size_t filter_bits)
    : m_slots(new slot_t[slots]), m_cap(slots),
      m_filter(new std::atomic<uint64_t>[filter_bits / 64]()),
      m_filter_bits(filter_bits) {
  assert(0 == (filter_bits & (filter_bits - 1)) && filter_bits >= 64);
}

/* The filter's word and mask for page; see cleaner.h. */
std::atomic<uint64_t> &clean_queue_t::filter_word(const clean_page_t &page,
                                                  uint64_t &mask) const {
  uint64_t h =
      mix(page.storeid ^ mix(page.pageid ^ (uint64_t(page.relid) << 48)));
  uint64_t bit = h & (m_filter_bits - 1);
  mask = uint64_t(1) << (bit % 64);
  return m_filter[bit / 64];
}

/* Put a slot in at the tail; see cleaner.h. */
void clean_queue_t::push(const slot_t &slot) {
  m_slots[(m_head + m_count) % m_cap] = slot;
  m_count++;
  m_waiting += 0 != slot.watermark ? 1 : 0;
}

/* Queue a page; see cleaner.h. */
clean_note_t clean_queue_t::note(const clean_page_t &page) {
  uint64_t mask = 0;
  std::atomic<uint64_t> &word = filter_word(page, mask);
  if (0 != (word.load() & mask) || 0 != (word.fetch_or(mask) & mask)) {
    return clean_note_t::known;
  }
  // The queue's mutex is held for a copy of a slot, never across a pin.
  std::lock_guard lock(m_mu);
  if (m_cap == m_count) {
    word.fetch_and(~mask);
    return clean_note_t::full;
  }
  push({page, 0});
  return clean_note_t::queued;
}

/* Take the page at the head; see cleaner.h. */
clean_take_t clean_queue_t::take(uint64_t watermark, clean_page_t &out) {
  // Held to move a slot from the head to the tail in one step.
  std::lock_guard lock(m_mu);
  if (0 == m_count) {
    return clean_take_t::empty;
  }
  slot_t slot = m_slots[m_head];
  m_head = (m_head + 1) % m_cap;
  m_count--;
  m_waiting -= 0 != slot.watermark ? 1 : 0;
  if (0 != slot.watermark && slot.watermark == watermark) {
    push(slot);
    return clean_take_t::waits;
  }
  uint64_t mask = 0;
  filter_word(slot.page, mask).fetch_and(~mask);
  out = slot.page;
  return clean_take_t::page;
}

/* Put a page back to wait; see cleaner.h. */
void clean_queue_t::wait(const clean_page_t &page, uint64_t watermark) {
  uint64_t mask = 0;
  std::atomic<uint64_t> &word = filter_word(page, mask);
  // Held so the bit and the slot agree against a take.
  std::lock_guard lock(m_mu);
  if (m_cap == m_count) {
    word.fetch_and(~mask);
    return;
  }
  word.fetch_or(mask);
  push({page, watermark});
}

/* The most pages the first worker's next pass cleans; see cleaner.h. */
size_t cleaner_budget(const clean_queue_t &queue, uint64_t begun,
                      uint64_t limit) {
  if (0 == queue.size() || (queue.all_wait() && 0 == begun)) {
    return 0;
  }
  return queue.full() || begun <= limit ? SIZE_MAX : cleaner_busy_pages;
}

/* Queue a leaf; see cleaner.h. */
void cleaner_note(void *ctx, uint64_t storeid, uint16_t relid,
                  uint64_t pageid) {
  cleaner_t *c = static_cast<cleaner_t *>(ctx);
  if (clean_note_t::full == c->queue.note({storeid, pageid, relid})) {
    // The workers look at the queue again now, not at their next tick.
    c->cv.notify_all();
  }
}

/* The pages on the queue; see cleaner.h. */
size_t clean_queue_t::size() const {
  std::lock_guard lock(m_mu);
  return m_count;
}

/* Whether no page can be put in; see cleaner.h. */
bool clean_queue_t::full() const {
  std::lock_guard lock(m_mu);
  return m_cap == m_count;
}

/* Whether every page on the queue waits; see cleaner.h. */
bool clean_queue_t::all_wait() const {
  std::lock_guard lock(m_mu);
  return 0 != m_count && m_count == m_waiting;
}

namespace {

/*
 * Clean the pages on the queue when the pass begins, one at a time, at
 * most budget of them (../../../BTREE.md §7.1): 0, or -1 with no session
 * to do it with.
 * 1. Open a session with no transaction, and read the watermark, made
 *    above 0, as a slot's 0 is a page that does not wait.
 * 2. Take a page. One that waits is passed over, unread.
 * 3. Note the catalog's publishes, then find the table by the page's
 *    store. A store gone drops the page.
 * 4. Clean the leaf. A page left with an entry in doubt waits for the
 *    watermark to move. A failure drops the page: the queue is a hint,
 *    and the page's bits are still there. A store dropped meanwhile
 *    fails so, and is no error to report.
 * 5. Close the session, so the reaper can close an idle database.
 */
int run_pass(cleaner_t *c, size_t budget) {
  sm1::session_t *s = c->open();
  if (nullptr == s) {
    return -1;
  }
  defer { sm1::closedb(s); };
  // No transaction: a cleaning needs no snapshot, and holds none.
  sm1::set_no_transaction(s);
  session_t *ws = nullptr;
  if (0 != session_open(c->cat, s, ws)) {
    return -1;
  }
  defer { session_close(ws); };
  uint64_t watermark = sm1::gc_watermark(s) + 1;
  size_t pages = c->queue.size();
  for (size_t i = 0; i < pages && 0 != budget && !c->stop.load(); i++) {
    clean_page_t page;
    clean_take_t took = c->queue.take(watermark, page);
    if (clean_take_t::empty == took) {
      break;
    }
    if (clean_take_t::waits == took) {
      continue;
    }
    budget--;
    uint64_t publishes = cat_publishes(ws);
    auto def = cat_get_table_by_store(c->cat, page.storeid);
    if (!def) {
      continue;
    }
    bool doubt = false;
    int rc = tbl_clean_leaf(ws, def, publishes, page.relid, page.pageid, doubt);
    if (0 == rc && doubt) {
      c->queue.wait(page, watermark);
    } else if (rc < 0 && sm1::SM_ERR_NOTFOUND != sm1::errnum()) {
      sm0::log_msg(sm1::loglevel_t::warn,
                   "cleaner: store {} tree {} page {}: {}", page.storeid,
                   page.relid, page.pageid, sm1::errmsg());
    }
  }
  return 0;
}

/*
 * A worker: sleep a second, or until the queue is full or a detach
 * stops it, then run a pass if one is due. The first runs by
 * cleaner_budget. The others, for an overflow, run only on a full queue.
 */
void worker(cleaner_t *c, bool first) {
  std::unique_lock lock(c->mu);
  while (!c->stopping) {
    c->cv.wait_for(lock, std::chrono::seconds(1));
    if (c->stopping) {
      break;
    }
    lock.unlock();
    size_t budget = c->queue.full() ? SIZE_MAX : 0;
    if (first) {
      uint64_t now = c->cat->begins.load(std::memory_order_relaxed);
      budget = cleaner_budget(c->queue, now - c->begins, cleaner_busy_limit);
      c->begins = now;
    }
    if (0 != budget) {
      run_pass(c, budget);
    }
    lock.lock();
  }
}

} // namespace

/* Attach the cleaner to a catalog; see sm3.h. */
int cleaner_attach(catalog_t *cat, std::function<sm1::session_t *()> open,
                   size_t workers) {
  if (!cat->cleaner) {
    cat->cleaner = std::make_unique<cleaner_t>();
    cat->cleaner->cat = cat;
  }
  cleaner_t *c = cat->cleaner.get();
  c->open = std::move(open);
  c->stopping = false;
  c->stop = false;
  // The threads are made now, and sleep until there is work: making
  // one when the queue overflows has no caller to fail to.
  for (size_t i = 0; i < workers; i++) {
    c->workers.emplace_back(worker, c, 0 == i);
  }
  return 0;
}

/* Stop the cleaner's workers; see sm3.h. */
void cleaner_detach(catalog_t *cat) {
  cleaner_t *c = cat->cleaner.get();
  if (nullptr == c) {
    return;
  }
  {
    std::lock_guard lock(c->mu);
    c->stopping = true;
  }
  c->stop = true;
  c->cv.notify_all();
  // Joined here, as a worker holds an sm1 session on the database.
  for (std::thread &t : c->workers) {
    t.join();
  }
  c->workers.clear();
}

/* One pass, for tests; see sm3.h. */
int cleaner_run_once(catalog_t *cat, size_t budget) {
  return run_pass(cat->cleaner.get(), budget);
}

/* The pages queued, for tests; see sm3.h. */
size_t cleaner_queued(const catalog_t *cat) {
  return cat->cleaner->queue.size();
}

} // namespace sm3
