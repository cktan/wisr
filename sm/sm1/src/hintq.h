/*
 * hintq.h -- a bounded queue of keys for the GC worker (../GC.md §4.2
 * and §5).
 *
 * The compaction filter finds work it cannot do: a dead chained
 * version to put back, a tombstone to purge. It queues the key, and
 * the worker does the work. The queue is a hint. A key it drops is
 * found again by the next compaction that meets it, so the queue may
 * not grow without bound.
 *
 *   hint_queue_t q;
 *   q.push(storeid, xid, key);        // a compaction thread
 *   hint_queue_t::batch_t b = q.take(); // the worker
 *   b.for_each([&](const record_t &r) { ... });
 *
 * 1. A push sets one bit of a 1-bit Bloom filter with one hash
 *    function, over the store and the key. A bit set means the key is
 *    queued, so the push stops.
 * 2. A push appends the record to one buffer, of a fixed size, made
 *    on the first push. A full buffer drops the key and clears its
 *    bit.
 * 3. A take hands the whole buffer to the caller and clears the
 *    filter. A key met again after it is queued again.
 *
 * One hash function is what makes a clear safe: with two or more,
 * clearing a bit could hide another key (../BTREEINDEX.md §9.4).
 */
#ifndef SM1_HINTQ_H
#define SM1_HINTQ_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>

namespace wisr {

class hint_queue_t {
public:
  /* The buffer's size: 32 MB, about 800,000 records of 40 bytes. */
  static constexpr size_t default_bytes = size_t(32) << 20;
  /* The filter's bits, a power of two: 1 MB. */
  static constexpr size_t default_filter_bits = size_t(1) << 23;

  /* One queued key. */
  struct record_t {
    uint64_t storeid;
    uint64_t xid; /* the dead writer, for a repair; 0 for a purge */
    std::string_view key;
  };

  /* The records a take handed over: packed bytes, freed with it. */
  class batch_t {
  public:
    batch_t() = default;
    batch_t(char *data, size_t used, size_t count)
        : m_data(data), m_used(used), m_count(count) {}
    batch_t(batch_t &&o) noexcept { swap(o); }
    batch_t &operator=(batch_t &&o) noexcept {
      batch_t tmp(std::move(o));
      swap(tmp);
      return *this;
    }
    batch_t(const batch_t &) = delete;
    batch_t &operator=(const batch_t &) = delete;
    ~batch_t() { delete[] m_data; }

    /* The number of records. */
    size_t count() const { return m_count; }

    /* Call fn(record_t) for each record, in the order queued. */
    template <class F> void for_each(F fn) const {
      size_t off = 0;
      while (off < m_used) {
        uint32_t klen;
        uint64_t storeid, xid;
        memcpy(&klen, m_data + off, 4);
        memcpy(&storeid, m_data + off + 4, 8);
        memcpy(&xid, m_data + off + 12, 8);
        fn(record_t{storeid, xid,
                    std::string_view(m_data + off + header, klen)});
        off += header + klen;
      }
    }

  private:
    void swap(batch_t &o) {
      std::swap(m_data, o.m_data);
      std::swap(m_used, o.m_used);
      std::swap(m_count, o.m_count);
    }
    char *m_data = nullptr;
    size_t m_used = 0;
    size_t m_count = 0;
  };

  /* A queue of bytes bytes and filter_bits filter bits (a power of two). */
  explicit hint_queue_t(size_t bytes = default_bytes,
                        size_t filter_bits = default_filter_bits);
  ~hint_queue_t();
  hint_queue_t(const hint_queue_t &) = delete;
  hint_queue_t &operator=(const hint_queue_t &) = delete;

  /* Queue the key: true if queued, false if the filter or a full buffer stopped
   * it. */
  bool push(uint64_t storeid, uint64_t xid, std::string_view key);

  /* Take every queued record, and clear the filter. */
  batch_t take();

  /* The number of records queued. */
  size_t size() const;

  /* A record: the key's length (4), the store (8), the xid (8), the key. */
  static constexpr size_t header = 20;

private:
  const size_t m_bytes;
  const size_t m_filter_bits;
  std::unique_ptr<std::atomic<uint64_t>[]> m_filter;
  mutable std::mutex m_mu; /* guards the buffer; the filter is atomic */
  char *m_buf = nullptr;   /* made by the first push */
  size_t m_used = 0;
  size_t m_count = 0;
};

} // namespace wisr

#endif /* SM1_HINTQ_H */
