/*
 * latch.h -- the writers' latches on B+tree pages (../BTREE.md §2):
 * a writer latches each page it changes, by its id, so two writers of
 * one page take turns; a reader takes none. A latch is a page's own,
 * never a stripe two pages share, as a split holds a page's and its
 * parent's at once, and two pages sharing a stripe would deadlock it
 * (../../../MAJORDECISION.md D3). Latches are taken child before
 * parent and left before right, so no two writers wait on each other.
 * A latch is held for a page's change alone, never across a wait for
 * a transaction, so its wait is not one sm_wait() cuts short, as
 * sm1's write latch is not.
 */
#ifndef SM2_TREE_LATCH_H
#define SM2_TREE_LATCH_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sm2::tree {

/* A page, named across the process: its database, store, index, id. */
struct page_name_t {
  uint64_t dbid = 0;
  uint64_t storeid = 0;
  uint16_t relid = 0;
  uint64_t pageid = 0;

  bool operator==(const page_name_t &) const = default;
};

/* A table of latches, one a page while it is latched or waited for. */
class latch_table_t {
public:
  /* Latch page, waiting while another writer holds it. */
  void lock(const page_name_t &page);

  /* Release page's latch, which the caller holds. */
  void unlock(const page_name_t &page);

  /* The pages latched or waited for now: for tests. */
  size_t size();

private:
  struct entry_t {
    std::mutex latch;
    size_t users = 0; /* holders and waiters */
  };
  struct hash_t {
    size_t operator()(const page_name_t &p) const;
  };
  struct shard_t {
    std::mutex mutex;
    std::unordered_map<page_name_t, std::unique_ptr<entry_t>, hash_t> map;
  };
  static constexpr size_t shards = 64;

  shard_t &shard_of(const page_name_t &page);

  shard_t m_shards[shards];
};

/* The process's latch table, which every session's writers share. */
latch_table_t &page_latches();

/*
 * How many latches the calling thread holds, of every table: a writer
 * that would take one out of order checks it holds none.
 */
size_t latches_held();

/* A page's latch, held for a scope. */
class page_latch_t {
public:
  page_latch_t() = default;
  /* Latch page in table. */
  page_latch_t(latch_table_t &table, const page_name_t &page)
      : m_table(&table), m_page(page) {
    // Wait for the page's other writer: the latch orders their changes.
    m_table->lock(m_page);
  }
  page_latch_t(const page_latch_t &) = delete;
  page_latch_t &operator=(const page_latch_t &) = delete;
  page_latch_t(page_latch_t &&o) noexcept
      : m_table(o.m_table), m_page(o.m_page) {
    o.m_table = nullptr;
  }
  page_latch_t &operator=(page_latch_t &&o) noexcept {
    if (this != &o) {
      release();
      m_table = o.m_table;
      m_page = o.m_page;
      o.m_table = nullptr;
    }
    return *this;
  }
  ~page_latch_t() { release(); }

  /* Release the latch now, if held. */
  void release() {
    if (nullptr != m_table) {
      m_table->unlock(m_page);
      m_table = nullptr;
    }
  }

  bool held() const { return nullptr != m_table; }
  const page_name_t &page() const { return m_page; }

private:
  latch_table_t *m_table = nullptr;
  page_name_t m_page;
};

} // namespace sm2::tree

#endif
