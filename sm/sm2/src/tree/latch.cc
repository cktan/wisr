/*
 * latch.cc -- the writers' latches on B+tree pages, one a page while
 * it is latched or waited for, in a sharded map (../BTREE.md §2).
 */

#include "latch.h"

#include <cassert>

namespace sm2::tree {

namespace {

/* The latches this thread holds. */
thread_local size_t t_held = 0;

} // namespace

/* Mix a page's name into a hash. */
size_t latch_table_t::hash_t::operator()(const page_name_t &p) const {
  uint64_t h = p.pageid * 0x9E3779B97F4A7C15ULL;
  h ^= (p.storeid + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2));
  h ^= (p.dbid * 0xC2B2AE3D27D4EB4FULL + p.relid + (h << 6) + (h >> 2));
  return static_cast<size_t>(h ^ (h >> 29));
}

/* The shard page's entry is in. */
latch_table_t::shard_t &latch_table_t::shard_of(const page_name_t &page) {
  return m_shards[hash_t{}(page) % shards];
}

/* Latch page; see latch.h. */
void latch_table_t::lock(const page_name_t &page) {
  shard_t &sh = shard_of(page);
  entry_t *e;
  {
    // Find or make the page's entry, counted so it outlives the wait.
    std::lock_guard<std::mutex> guard(sh.mutex);
    auto &slot = sh.map[page];
    if (nullptr == slot) {
      slot = std::make_unique<entry_t>();
    }
    e = slot.get();
    e->users++;
  }
  // Wait for the page's other writer, if there is one.
  e->latch.lock();
  t_held++;
}

/* Release page's latch; see latch.h. */
void latch_table_t::unlock(const page_name_t &page) {
  shard_t &sh = shard_of(page);
  // The shard's mutex keeps the entry from being dropped under a
  // writer about to count itself in.
  std::lock_guard<std::mutex> guard(sh.mutex);
  auto it = sh.map.find(page);
  assert(sh.map.end() != it);
  entry_t *e = it->second.get();
  e->latch.unlock();
  t_held--;
  if (0 == --e->users) {
    sh.map.erase(it);
  }
}

/* The pages latched or waited for now; see latch.h. */
size_t latch_table_t::size() {
  size_t n = 0;
  for (shard_t &sh : m_shards) {
    std::lock_guard<std::mutex> guard(sh.mutex);
    n += sh.map.size();
  }
  return n;
}

/* The latches the calling thread holds; see latch.h. */
size_t latches_held() { return t_held; }

/* The process's latch table; see latch.h. */
latch_table_t &page_latches() {
  static latch_table_t table;
  return table;
}

} // namespace sm2::tree
