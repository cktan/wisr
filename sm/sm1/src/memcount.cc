/*
 * memcount.cc -- the bytes a store's writes put in its memtables, by
 * key prefix; see memcount.h.
 */
#include "memcount.h"

namespace wisr {

/* A key's prefix, its first two bytes big-endian; a shorter key's are 0. */
uint16_t key_prefix(std::string_view key) {
  uint16_t hi = key.size() > 0 ? static_cast<uint8_t>(key[0]) : 0;
  uint16_t lo = key.size() > 1 ? static_cast<uint8_t>(key[1]) : 0;
  return static_cast<uint16_t>(hi << 8 | lo);
}

/* Count a write of bytes under key in the active memtable. */
void memcount_t::add(std::string_view key, uint64_t bytes) {
  uint16_t prefix = key_prefix(key);
  {
    // A prefix written before is counted with the lock shared.
    std::shared_lock lock(m_mu);
    auto it = m_active.find(prefix);
    if (it != m_active.end()) {
      it->second.fetch_add(bytes, std::memory_order_relaxed);
      return;
    }
  }
  // A new prefix is added with it exclusive; another may have added it.
  std::unique_lock lock(m_mu);
  m_active[prefix].fetch_add(bytes, std::memory_order_relaxed);
}

/* Keep the active memtable's count apart, now that it is sealed. */
void memcount_t::seal(uint64_t first_seqno) {
  std::unique_lock lock(m_mu);
  sealed_t sealed{first_seqno, {}};
  for (auto &[prefix, bytes] : m_active) {
    uint64_t n = bytes.exchange(0, std::memory_order_relaxed);
    if (n > 0) {
      sealed.bytes[prefix] = n;
    }
  }
  m_sealed.push_back(std::move(sealed));
}

/* Forget each sealed memtable the flush wrote out: oldest first, in order. */
void memcount_t::flushed(uint64_t largest_seqno) {
  std::unique_lock lock(m_mu);
  while (!m_sealed.empty() && m_sealed.front().first_seqno <= largest_seqno) {
    m_sealed.pop_front();
  }
}

/* The bytes in memtables under the prefixes in [lo, hi). */
uint64_t memcount_t::bytes(uint32_t lo, uint32_t hi, uint64_t unflushed) const {
  std::shared_lock lock(m_mu);
  uint64_t total = 0;
  for (const auto &[prefix, bytes] : m_active) {
    if (prefix >= lo && prefix < hi) {
      total += bytes.load(std::memory_order_relaxed);
    }
  }
  // The newest sealed memtables, those not yet flushed.
  size_t skip = m_sealed.size() > unflushed ? m_sealed.size() - unflushed : 0;
  for (size_t i = skip; i < m_sealed.size(); i++) {
    const sealed_t &s = m_sealed[i];
    for (const auto &[prefix, bytes] : s.bytes) {
      if (prefix >= lo && prefix < hi) {
        total += bytes;
      }
    }
  }
  return total;
}

/* Count cf_name's memtables in counts. */
void memcount_listener_t::add(const std::string &cf_name,
                              std::shared_ptr<memcount_t> counts) {
  std::lock_guard lock(m_mu);
  m_counts[cf_name] = std::move(counts);
}

/* Forget cf_name. */
void memcount_listener_t::remove(const std::string &cf_name) {
  std::lock_guard lock(m_mu);
  m_counts.erase(cf_name);
}

/* The counts of cf_name, or nullptr if it is not a store counted. */
std::shared_ptr<memcount_t>
memcount_listener_t::find(const std::string &cf_name) {
  std::lock_guard lock(m_mu);
  auto it = m_counts.find(cf_name);
  return it == m_counts.end() ? nullptr : it->second;
}

/* A memtable sealed: its store's active count is kept apart. */
void memcount_listener_t::OnMemTableSealed(const rocksdb::MemTableInfo &info) {
  if (std::shared_ptr<memcount_t> c = find(info.cf_name)) {
    c->seal(info.first_seqno);
  }
}

/* A flush done: its store forgets the sealed memtables it wrote out. */
void memcount_listener_t::OnFlushCompleted(rocksdb::DB * /*db*/,
                                           const rocksdb::FlushJobInfo &info) {
  if (std::shared_ptr<memcount_t> c = find(info.cf_name)) {
    c->flushed(info.largest_seqno);
  }
}

} // namespace wisr
