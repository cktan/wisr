/*
 * hintq.cc -- hint_queue_t (hintq.h).
 */
#include "hintq.h"

#include <functional>

using namespace wisr;

namespace {

/* The filter's hash of a record: its store and its key. */
uint64_t filter_hash(uint64_t storeid, std::string_view key) {
  uint64_t h = std::hash<std::string_view>{}(key);
  // Mix the store in, as boost::hash_combine does.
  h ^= storeid + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

} // namespace

/* Make an empty queue; the buffer waits for the first push. */
hint_queue_t::hint_queue_t(size_t bytes, size_t filter_bits)
    : m_bytes(bytes), m_filter_bits(filter_bits),
      m_filter(new std::atomic<uint64_t>[(filter_bits + 63) / 64]()) {}

/* Free the buffer. */
hint_queue_t::~hint_queue_t() { delete[] m_buf; }

/*
 * Queue a key, unless the filter says it is queued or the buffer is full.
 * 1. Load the filter's word. If the key's bit is set, stop.
 * 2. Set the bit. If another caller set it first, stop.
 * 3. Under the mutex, make the buffer if this is the first push.
 * 4. If the record does not fit, clear the bit and stop.
 * 5. Append the record.
 */
bool hint_queue_t::push(uint64_t storeid, uint64_t xid, std::string_view key) {
  size_t bit = filter_hash(storeid, key) & (m_filter_bits - 1);
  std::atomic<uint64_t> &word = m_filter[bit / 64];
  uint64_t mask = uint64_t(1) << (bit % 64);
  // The load keeps the shared word clean on the common path: a key met again.
  if (word.load(std::memory_order_relaxed) & mask) {
    return false;
  }
  if (word.fetch_or(mask) & mask) {
    return false;
  }
  // The buffer and its counters are shared with take().
  std::lock_guard<std::mutex> guard(m_mu);
  if (nullptr == m_buf) {
    // Untouched pages of a large block cost no memory, and new aborts
    // the process on failure (../CODE.md §5).
    m_buf = new char[m_bytes];
  }
  size_t need = header + key.size();
  if (key.size() > UINT32_MAX || need > m_bytes - m_used) {
    // A hint: the next compaction that meets the key queues it again.
    word.fetch_and(~mask);
    return false;
  }
  uint32_t klen = static_cast<uint32_t>(key.size());
  memcpy(m_buf + m_used, &klen, 4);
  memcpy(m_buf + m_used + 4, &storeid, 8);
  memcpy(m_buf + m_used + 12, &xid, 8);
  memcpy(m_buf + m_used + header, key.data(), key.size());
  m_used += need;
  m_count++;
  return true;
}

/*
 * Hand every queued record to the caller.
 * 1. If the queue is empty, return an empty batch.
 * 2. Give the buffer away. The next push makes a new one.
 * 3. Clear every filter bit, in the same hold of the mutex: a push
 *    that set its bit before this appends to the new buffer.
 */
hint_queue_t::batch_t hint_queue_t::take() {
  std::lock_guard<std::mutex> guard(m_mu);
  if (0 == m_count) {
    return batch_t();
  }
  batch_t out(m_buf, m_used, m_count);
  m_buf = nullptr;
  m_used = 0;
  m_count = 0;
  for (size_t i = 0; i < (m_filter_bits + 63) / 64; i++) {
    m_filter[i].store(0, std::memory_order_relaxed);
  }
  return out;
}

/* The number of records queued. */
size_t hint_queue_t::size() const {
  std::lock_guard<std::mutex> guard(m_mu);
  return m_count;
}
