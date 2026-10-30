/*
 * page.h -- a B+tree page of a secondary index (../BTREE.md §3): its
 * bytes as RocksDB and the page cache hold them, read where they lie,
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
 * writes its page whole (../../MAJORDECISION.md D12).
 */
constexpr size_t page_limit = 4096;

/* A high key follows the header: none on a level's rightmost page. */
constexpr uint8_t page_has_high = 0x01;
/* Split, and its parent's entry for the new page not yet written. */
constexpr uint8_t page_incomplete = 0x02;

/* The bytes an entry adds to a page: its key, its value, its slot. */
constexpr size_t entry_size(size_t key_len, size_t value_len) {
  return 2 + key_len + 2 + value_len + 2;
}

/* The bytes of a page's header before its high key. */
constexpr size_t page_header_size = 1 + 1 + 2 + 8 + 8 + 2;

/*
 * The most an entry may take of a page of limit bytes: a third of
 * what the header leaves, as a half of a split holds its high key,
 * no longer than an entry, and two entries; so a split always finds
 * a point where both halves fit, with the entry that caused it.
 */
constexpr size_t entry_max(size_t limit) {
  return (limit - page_header_size) / 3;
}

/*
 * A page decoded to be changed: its header, and its entries in key
 * order, each a key and a value. A leaf's value is the entry's own
 * (its cut flag); an inner page's is its child's page id.
 */
struct page_t {
  uint8_t level = 0; /* 0 a leaf */
  uint8_t flags = 0; /* page_has_high, page_incomplete */
  uint64_t version = 0;
  uint64_t right = 0; /* the right sibling's page id, 0 none */
  std::string high;   /* the least key the page may not hold */
  std::vector<std::pair<std::string, std::string>> entries;
};

/* The bytes page takes, laid out. */
size_t page_size(const page_t &page);

/* page, laid out as its bytes (../BTREE.md §3). */
std::string page_bytes(const page_t &page);

/*
 * Insert key, with value, in key order, if page has room under limit:
 * 0; 1 if an entry has the key already, and nothing is changed; 2 if
 * there is no room, so the page is to be split first; -1 with
 * SM_ERR_KEYTOOLONG if the entry is over a third of limit.
 */
int page_insert(page_t &page, std::string_view key, std::string_view value,
                size_t limit = page_limit);

/*
 * Insert key, with value, into page, which page_insert found full,
 * and split it into itself and right, each under limit, their bytes
 * as near half as that allows: right takes the upper entries, page's
 * high key and right link, and page's high key becomes right's first
 * key. key is not on page, and its entry is at most entry_max(limit).
 * The caller gives right its page id, links page to it, and sets the
 * flags the split is written with.
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
   * Check every entry lies within the page, after the slots, in slot
   * order and end to end, as page_bytes() lays them: 0, or -1 with
   * SM_ERR_CORRUPT.
   */
  int check() const;

  uint8_t level() const { return m_level; }
  uint8_t flags() const { return m_flags; }
  uint64_t version() const { return m_version; }
  uint64_t right() const { return m_right; }
  bool has_high() const { return 0 != (m_flags & page_has_high); }
  std::string_view high() const { return m_high; }
  size_t count() const { return m_count; }
  /* The page's bytes, as open() was given them. */
  std::string_view bytes() const { return m_bytes; }

  /* Entry i's key and value, i below count(). */
  std::string_view key(size_t i) const;
  std::string_view value(size_t i) const;

  /*
   * The bytes of entries i to j - 1, end to end as check() finds
   * them, each a key's length, the key, a value's length, the value;
   * empty if none, or if they run past the page.
   */
  std::string_view span(size_t i, size_t j) const;

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
  /* Entry i's offset, from its slot. */
  size_t offset(size_t i) const;

  /* The length-prefixed bytes at at, empty past the page. */
  std::string_view field(size_t at) const;

  std::string_view m_bytes;
  std::string_view m_high;
  uint8_t m_level = 0;
  uint8_t m_flags = 0;
  uint64_t m_version = 0;
  uint64_t m_right = 0;
  size_t m_count = 0;
  size_t m_slots = 0; /* where the slots start */
};

} // namespace sm2::tree

#endif
