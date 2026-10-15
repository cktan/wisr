/*
 * memcount.h -- the bytes a store's writes put in its memtables, by
 * the first two bytes of the key (STATISTICS.md §3.1).
 *
 * RocksDB answers exactly for a column family's memtables as a whole,
 * and for a key range within them only by sampling the skip list,
 * whose shape is random: a range among others reads anything from 0
 * to the whole memtable, the same rows differently in each process.
 * sm2 keeps each heap and index of a table under a 2-byte relid of
 * one store, so the range that matters is a prefix's, and sm1, which
 * makes every write, counts them there.
 *
 * A count is kept for the active memtable and for each sealed one
 * until a flush writes it out: a sealed memtable's bytes are still in
 * memory, and the writes after its sealing are in the next.
 */
#ifndef SM1_MEMCOUNT_H
#define SM1_MEMCOUNT_H

#include <rocksdb/listener.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wisr {

/* A key's prefix, its first two bytes big-endian; a shorter key's are 0. */
uint16_t key_prefix(std::string_view key);

/* One store's memtable bytes, by key prefix. */
class memcount_t {
public:
  /* A write of bytes into the store, under key, in the active memtable. */
  void add(std::string_view key, uint64_t bytes);

  /*
   * The active memtable was sealed, holding what was written since
   * first_seqno: its count is kept apart until a flush covers it.
   */
  void seal(uint64_t first_seqno);

  /* A flush wrote out every sealed memtable it covered, to largest_seqno. */
  void flushed(uint64_t largest_seqno);

  /*
   * The bytes in memtables under the prefixes in [lo, hi), hi 0x10000
   * for no bound above, of the active memtable and the newest
   * unflushed sealed ones: as many as RocksDB says are not yet
   * flushed, since it reports a flush to the listener only after a
   * waiting Flush() has returned.
   */
  uint64_t bytes(uint32_t lo, uint32_t hi, uint64_t unflushed) const;

private:
  /* A sealed memtable's count, and the first sequence number it holds. */
  struct sealed_t {
    uint64_t first_seqno;
    std::unordered_map<uint16_t, uint64_t> bytes;
  };

  /*
   * m_mu shared to count a write of a prefix already there, exclusive
   * to add a prefix, to seal and to forget what a flush wrote.
   */
  mutable std::shared_mutex m_mu;
  std::unordered_map<uint16_t, std::atomic<uint64_t>> m_active;
  std::deque<sealed_t> m_sealed;
};

/*
 * A RocksDB instance's listener: each store's counts by its column
 * family's name, told of a memtable sealed and a flush done.
 */
class memcount_listener_t : public rocksdb::EventListener {
public:
  /* Count the column family cf_name's memtables in counts. */
  void add(const std::string &cf_name, std::shared_ptr<memcount_t> counts);

  /* Forget cf_name, dropped. */
  void remove(const std::string &cf_name);

  void OnMemTableSealed(const rocksdb::MemTableInfo &info) override;
  void OnFlushCompleted(rocksdb::DB *db,
                        const rocksdb::FlushJobInfo &info) override;

private:
  /* The counts of cf_name, or nullptr if it is not a store counted. */
  std::shared_ptr<memcount_t> find(const std::string &cf_name);

  std::mutex m_mu; /* guards m_counts */
  std::unordered_map<std::string, std::shared_ptr<memcount_t>> m_counts;
};

} // namespace wisr

#endif /* SM1_MEMCOUNT_H */
