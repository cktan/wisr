/*
 * btree_reaper.cc -- the btree reaper (../BTREEINDEX.md §9).
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

/* Make a queue; see btree_reaper.h. */
reap_queue_t::reap_queue_t(size_t slots, size_t filter_bits)
    : m_slots(new slot_t[slots]), m_cap(slots),
      m_filter(new std::atomic<uint64_t>[filter_bits / 64]()),
      m_filter_bits(filter_bits) {
  assert(0 == (filter_bits & (filter_bits - 1)) && filter_bits >= 64);
}

/* The filter's word and mask for page; see btree_reaper.h. */
std::atomic<uint64_t> &reap_queue_t::filter_word(const reap_page_t &page,
                                                 uint64_t &mask) const {
  uint64_t h =
      mix(page.storeid ^ mix(page.pageid ^ (uint64_t(page.relid) << 48)));
  uint64_t bit = h & (m_filter_bits - 1);
  mask = uint64_t(1) << (bit % 64);
  return m_filter[bit / 64];
}

/* Put a slot in at the tail; see btree_reaper.h. */
void reap_queue_t::push(const slot_t &slot) {
  m_slots[(m_head + m_count) % m_cap] = slot;
  m_count++;
  m_waiting += 0 != slot.watermark ? 1 : 0;
}

/* Queue a page; see btree_reaper.h. */
reap_note_t reap_queue_t::note(const reap_page_t &page) {
  uint64_t mask = 0;
  std::atomic<uint64_t> &word = filter_word(page, mask);
  if (0 != (word.load() & mask) || 0 != (word.fetch_or(mask) & mask)) {
    return reap_note_t::known;
  }
  // The queue's mutex is held for a copy of a slot, never across a pin.
  std::lock_guard lock(m_mu);
  if (m_cap == m_count) {
    word.fetch_and(~mask);
    return reap_note_t::full;
  }
  push({page, 0});
  return reap_note_t::queued;
}

/* Take the page at the head; see btree_reaper.h. */
reap_take_t reap_queue_t::take(uint64_t watermark, reap_page_t &out) {
  // Held to move a slot from the head to the tail in one step.
  std::lock_guard lock(m_mu);
  if (0 == m_count) {
    return reap_take_t::empty;
  }
  slot_t slot = m_slots[m_head];
  m_head = (m_head + 1) % m_cap;
  m_count--;
  m_waiting -= 0 != slot.watermark ? 1 : 0;
  if (0 != slot.watermark && slot.watermark == watermark) {
    push(slot);
    return reap_take_t::waits;
  }
  uint64_t mask = 0;
  filter_word(slot.page, mask).fetch_and(~mask);
  out = slot.page;
  return reap_take_t::page;
}

/* Put a page back to wait; see btree_reaper.h. */
void reap_queue_t::wait(const reap_page_t &page, uint64_t watermark) {
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

/* Take a tree's pages off the queue; see btree_reaper.h. */
void reap_queue_t::extract(uint64_t storeid, uint16_t relid,
                           std::vector<reap_page_t> &out) {
  // Held for the whole walk: VACUUM is rare and the queue is an array.
  std::lock_guard lock(m_mu);
  size_t kept = 0;
  for (size_t i = 0; i < m_count; i++) {
    slot_t slot = m_slots[(m_head + i) % m_cap];
    if (slot.page.storeid != storeid || slot.page.relid != relid) {
      m_slots[(m_head + kept) % m_cap] = slot;
      kept++;
      continue;
    }
    uint64_t mask = 0;
    filter_word(slot.page, mask).fetch_and(~mask);
    m_waiting -= 0 != slot.watermark ? 1 : 0;
    out.push_back(slot.page);
  }
  m_count = kept;
}

/* The most pages the first worker's next pass reaps; see btree_reaper.h. */
size_t btree_reaper_budget(const reap_queue_t &queue, uint64_t begun,
                           uint64_t limit) {
  if (0 == queue.size() || (queue.all_wait() && 0 == begun)) {
    return 0;
  }
  return queue.full() || begun <= limit ? SIZE_MAX : btree_reaper_busy_pages;
}

/* Queue a leaf; see btree_reaper.h. */
void btree_reaper_note(void *ctx, uint64_t storeid, uint16_t relid,
                       uint64_t pageid) {
  btree_reaper_t *c = static_cast<btree_reaper_t *>(ctx);
  if (reap_note_t::full == c->queue.note({storeid, pageid, relid})) {
    // The workers look at the queue again now, not at their next tick.
    c->cv.notify_all();
  }
}

/* The pages on the queue; see btree_reaper.h. */
size_t reap_queue_t::size() const {
  std::lock_guard lock(m_mu);
  return m_count;
}

/* Whether no page can be put in; see btree_reaper.h. */
bool reap_queue_t::full() const {
  std::lock_guard lock(m_mu);
  return m_cap == m_count;
}

/* Whether every page on the queue waits; see btree_reaper.h. */
bool reap_queue_t::all_wait() const {
  std::lock_guard lock(m_mu);
  return 0 != m_count && m_count == m_waiting;
}

namespace {

/*
 * Reap the pages on the queue when the pass begins, one at a time, at
 * most budget of them (../BTREEINDEX.md §9); then, with rows, the keys
 * queued for the row reaper (../GC.md §13.5): 0, or -1 with no session
 * to do it with.
 * 1. Open a session with no transaction, and read the watermark, made
 *    above 0, as a slot's 0 is a page that does not wait.
 * 2. Take a page. One that waits is passed over, unread.
 * 3. Note the catalog's publishes, then find the table by the page's
 *    store. A store gone drops the page.
 * 4. Reap the leaf. A page left with an entry in doubt waits for the
 *    watermark to move. A failure drops the page: the queue is a hint,
 *    and the page's bits are still there. A store dropped meanwhile
 *    fails so, and is no error to report.
 * 5. With rows, run the row reaper's pass on the same session. A
 *    failure is logged: its keys are a hint, queued again by a read.
 * 6. Close the session, so the db reaper can close an idle database.
 */
int run_pass(btree_reaper_t *c, size_t budget, bool rows) {
  sm1::session_t *s = c->open();
  if (nullptr == s) {
    return -1;
  }
  defer { sm1::closedb(s); };
  // No transaction: a reap needs no snapshot, and holds none.
  sm1::set_no_transaction(s);
  session_t *ws = nullptr;
  if (0 != session_open(c->cat, s, ws)) {
    return -1;
  }
  defer { session_close(ws); };
  uint64_t watermark = sm1::gc_watermark(s) + 1;
  size_t pages = c->queue.size();
  for (size_t i = 0; i < pages && 0 != budget && !c->stop.load(); i++) {
    reap_page_t page;
    reap_take_t took = c->queue.take(watermark, page);
    if (reap_take_t::empty == took) {
      break;
    }
    if (reap_take_t::waits == took) {
      continue;
    }
    budget--;
    uint64_t publishes = cat_publishes(ws);
    auto def = cat_get_table_by_store(c->cat, page.storeid);
    if (!def) {
      continue;
    }
    bool doubt = false;
    int rc = tbl_reap_leaf(ws, def, publishes, page.relid, page.pageid, doubt);
    if (0 == rc && doubt) {
      c->queue.wait(page, watermark);
    } else if (rc < 0 && sm1::SM_ERR_NOTFOUND != sm1::errnum()) {
      sm0::log_msg(sm1::loglevel_t::warn,
                   "btree reaper: store {} tree {} page {}: {}", page.storeid,
                   page.relid, page.pageid, sm1::errmsg());
    }
  }
  if (rows && !c->stop.load() && sm1::row_reap_pass(s, &c->stop) < 0) {
    sm0::log_msg(sm1::loglevel_t::warn, "row reaper: {}", sm1::errmsg());
  }
  return 0;
}

/*
 * A worker: sleep a second, or until the queue is full or a detach
 * stops it, then run a pass if one is due. The first runs by
 * btree_reaper_budget, and runs the row pass when keys wait for it
 * (../BTREEINDEX.md §9.9). The others, for an overflow, run only on a
 * full queue, and run no row pass.
 */
void worker(btree_reaper_t *c, bool first) {
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
      budget = btree_reaper_budget(c->queue, now - c->begins,
                                   btree_reaper_busy_limit);
      c->begins = now;
    }
    // Asked with no session: a session here would keep the db reaper
    // from closing an idle database.
    bool rows = first && c->row_pending && 0 != c->row_pending();
    if (0 != budget || rows) {
      run_pass(c, budget, rows);
    }
    lock.lock();
  }
}

} // namespace

/* Attach the btree reaper to a catalog; see sm3.h. */
int btree_reaper_attach(catalog_t *cat, std::function<sm1::session_t *()> open,
                        size_t workers, std::function<size_t()> row_pending) {
  if (!cat->btree_reaper) {
    cat->btree_reaper = std::make_unique<btree_reaper_t>();
    cat->btree_reaper->cat = cat;
  }
  btree_reaper_t *c = cat->btree_reaper.get();
  c->open = std::move(open);
  c->row_pending = std::move(row_pending);
  c->stopping = false;
  c->stop = false;
  // The threads are made now, and sleep until there is work: making
  // one when the queue overflows has no caller to fail to.
  for (size_t i = 0; i < workers; i++) {
    c->workers.emplace_back(worker, c, 0 == i);
  }
  return 0;
}

/* Stop the btree reaper's workers; see sm3.h. */
void btree_reaper_detach(catalog_t *cat) {
  btree_reaper_t *c = cat->btree_reaper.get();
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
int btree_reaper_run_once(catalog_t *cat, size_t budget) {
  return run_pass(cat->btree_reaper.get(), budget, true);
}

/* The pages queued, for tests; see sm3.h. */
size_t btree_reaper_queued(const catalog_t *cat) {
  return cat->btree_reaper->queue.size();
}

} // namespace sm3
