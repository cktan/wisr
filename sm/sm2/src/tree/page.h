/*
 * page.h -- a B+tree page of a secondary index (../BTREE.md §3): its
 * bytes as RocksDB and the buffer pool hold them, read where they lie,
 * and a page decoded to be changed, an entry inserted or the page
 * split. On bytes alone: no RocksDB, no cache, no latch.
 */
#ifndef SM2_TREE_PAGE_H
#define SM2_TREE_PAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sm2::tree {

/*
 * The bytes past which an insert splits a page: 4 KB, as each insert
 * writes its page whole.
 */
constexpr size_t page_limit = 4096;

/* A high key follows the header: none on a level's rightmost page. */
constexpr uint8_t page_has_high = 0x01;
/* Split, and its parent's entry for the new page not yet written. */
constexpr uint8_t page_incomplete = 0x02;
/*
 * A leaf whose every entry vacuum found settled and live: its row's
 * newest version committed at or below the GC watermark with the
 * entry's key, so every snapshot sees it (../BTREE.md §4.1). Set by
 * vacuum alone; an insert into the page clears it.
 */
constexpr uint8_t page_all_visible = 0x04;

/* The bytes an entry adds to a page: its key, its value, its slot. */
constexpr size_t entry_size(size_t key_len, size_t value_len) {
  return 2 + key_len + 2 + value_len + 2;
}

/* The bytes of a page's header before its high key. */
constexpr size_t page_header_size = 1 + 1 + 2 + 2 + 8 + 8 + 8 + 2;

/* A page's slot array grows by this many slots at a time. */
constexpr size_t page_slot_step = 16;

/* The slots a page of n entries is laid out with. */
constexpr size_t page_slots(size_t n) {
  return (n + page_slot_step - 1) / page_slot_step * page_slot_step;
}

/*
 * The most an entry may take of a page of limit bytes: a third of
 * what the header and a half's unused slots leave, as a half of a
 * split holds its high key, no longer than an entry, and two entries;
 * so a split always finds a point where both halves fit, with the
 * entry that caused it.
 */
constexpr size_t entry_max(size_t limit) {
  return (limit - page_header_size - 2 * (page_slot_step - 1)) / 3;
}

/*
 * A page decoded to be changed: its header, and its entries in key
 * order, each a key and a value. A leaf's value is the entry's own
 * (its cut flag); an inner page's is its child's page id.
 */
struct page_t {
  uint8_t level = 0; /* 0 a leaf */
  uint8_t flags = 0; /* page_has_high, page_incomplete, page_all_visible */
  uint64_t version = 0;
  uint64_t prev = 0; /* the left sibling's page id, 0 none */
  uint64_t next = 0; /* the right sibling's page id, 0 none */
  std::string high;  /* the least key the page may not hold */

  /* Whether the page has a high key: none on a level's rightmost. */
  bool has_high_key() const { return 0 != (flags & page_has_high); }
  std::vector<std::pair<std::string, std::string>> entries;
};

/* The bytes page takes, laid out. */
size_t page_size(const page_t &page);

/* page, laid out as its bytes (../BTREE.md §3). */
std::string page_bytes(const page_t &page);

/*
 * Insert key, with value, in key order, if page has room under limit,
 * and clear page_all_visible, as the new entry is not settled: 0; 1 if
 * an entry has the key already, and nothing is changed; 2 if there is
 * no room, so the page is to be split first; -1 with
 * SM_ERR_KEYTOOLONG if the entry is over a third of limit.
 */
int page_insert(page_t &page, std::string_view key, std::string_view value,
                size_t limit = page_limit);

class page_view_t;

/*
 * Insert key, with value, into the page v views, which check() has
 * passed, laid out anew in out with its version moved and
 * page_all_visible cleared, as page_insert: 0; 1 if an
 * entry has the key already, out untouched; 2 if there is no room
 * under limit, so the page is to be decoded and split; -1 with
 * SM_ERR_KEYTOOLONG as page_insert. No entry is decoded: the bytes
 * are spliced, an insert's common case.
 */
int page_insert_bytes(const page_view_t &v, std::string_view key,
                      std::string_view value, size_t limit, std::string &out);

/*
 * Insert key, with value, into page, which page_insert found full,
 * and split it into itself and right, each under limit, their bytes
 * as near half as that allows: right takes the upper entries, page's
 * high key and next link, and page's high key becomes right's first
 * key. key is not on page, and its entry is at most entry_max(limit).
 * The caller gives right its page id, links the two, and sets the
 * flags the split is written with. Neither half is all visible.
 */
void page_split(page_t &page, page_t &right, std::string_view key,
                std::string_view value, size_t limit = page_limit);

/*
 * A page read where its bytes lie. open() checks its header and its
 * slots alone, as every read of a page opens one; an entry read stays
 * within the bytes, empty if it would run past them; check() checks
 * every entry, as a writer does before it changes a page.
 */
class page_view_t {
public:
  /* Check bytes' header and slots, and view them: 0, or -1 with SM_ERR_CORRUPT.
   */
  int open(std::string_view bytes);

  /*
   * Check the entries lie end to end from the slots to the page's end,
   * each named by one slot: 0, or -1 with SM_ERR_CORRUPT.
   */
  int check() const;

  uint8_t level() const { return m_level; }
  uint8_t flags() const { return m_flags; }
  uint64_t version() const { return m_version; }
  uint64_t prev() const { return m_prev; }
  uint64_t next() const { return m_next; }
  bool has_high() const { return 0 != (m_flags & page_has_high); }
  std::string_view high() const { return m_high; }
  size_t count() const { return m_count; }
  /* The page's bytes, as open() was given them. */
  std::string_view bytes() const { return m_bytes; }
  /* Where its slots begin, past the header and the high key. */
  size_t slots_offset() const { return m_slots; }
  /* Where its slots end, and its entries begin. */
  size_t entries_offset() const { return m_entries; }
  /* Where entry i lies in the page, from its slot, i below count(). */
  size_t offset(size_t i) const;

  /* Entry i's key and value, i below count(). */
  std::string_view key(size_t i) const;
  std::string_view value(size_t i) const;

  /*
   * Entry i's bytes, i below count(): a key's length, the key, a
   * value's length, the value; empty if they run past the page.
   */
  std::string_view entry(size_t i) const;

  /* The first entry whose key is at or past key: count() if none. */
  size_t lower_bound(std::string_view key) const;

  /*
   * An inner page's entry to descend by for key: the last whose key
   * is at or below it, 0 if none is, as the first's is empty.
   */
  size_t child(std::string_view key) const;

  /* Whether key is at or past the page's high key: move right. */
  bool past_high(std::string_view key) const {
    return has_high() && key >= m_high;
  }

  /* The page decoded into out, to be changed. */
  void load(page_t &out) const;

private:
  /* The length-prefixed bytes at at, empty past the page. */
  std::string_view field(size_t at) const;

  std::string_view m_bytes;
  std::string_view m_high;
  uint8_t m_level = 0;
  uint8_t m_flags = 0;
  uint64_t m_version = 0;
  uint64_t m_prev = 0;
  uint64_t m_next = 0;
  size_t m_count = 0;   /* top: the slots in use */
  size_t m_slots = 0;   /* where the slots start */
  size_t m_entries = 0; /* where they end, and the entries start */
};

} // namespace sm2::tree

#endif
