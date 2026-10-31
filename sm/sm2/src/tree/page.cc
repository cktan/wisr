/*
 * page.cc -- a B+tree page's bytes, laid out and read where they lie
 * (../BTREE.md §3), and a decoded page's insert and split.
 *
 *   header = level (1B) | flags (1B) | n (2B) | version (8B)
 *          | prev (8B) | next (8B) | high key's length (2B) | high key
 *   slots  = n offsets (2B each), of the entries in key order
 *   entry  = key's length (2B) | key | value's length (2B) | value
 *
 * Integers are little-endian. A page stays under 64 KB, as an offset
 * is two bytes; its limit is far below that.
 */

#include "page.h"

#include "sm1.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace sm2::tree {

namespace {

/* Append v to out, little-endian, in n bytes. */
void put_le(std::string &out, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

/* The n-byte little-endian integer at p. */
uint64_t get_le(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return v;
}

/* Fail as a page that is not one. */
int corrupt(const char *what) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, std::string("tree page: ") + what);
}

/* Order entries by key alone. */
bool key_less(const std::pair<std::string, std::string> &e,
              std::string_view key) {
  return std::string_view(e.first) < key;
}

} // namespace

/* The bytes page takes, laid out; see page.h. */
size_t page_size(const page_t &page) {
  size_t n = page_header_size + page.high.size();
  for (const auto &[key, value] : page.entries) {
    n += entry_size(key.size(), value.size());
  }
  return n;
}

/* page, laid out as its bytes; see page.h. */
std::string page_bytes(const page_t &page) {
  assert(page.entries.size() <= UINT16_MAX);
  assert(0 != (page.flags & page_has_high) || page.high.empty());
  std::string out;
  out.reserve(page_size(page));
  out.push_back(static_cast<char>(page.level));
  out.push_back(static_cast<char>(page.flags));
  put_le(out, page.entries.size(), 2);
  put_le(out, page.version, 8);
  put_le(out, page.prev, 8);
  put_le(out, page.next, 8);
  put_le(out, page.high.size(), 2);
  out += page.high;
  // The slots, then the entries they point at, in the same order.
  size_t at = out.size() + 2 * page.entries.size();
  for (const auto &[key, value] : page.entries) {
    put_le(out, at, 2);
    at += 2 + key.size() + 2 + value.size();
  }
  for (const auto &[key, value] : page.entries) {
    put_le(out, key.size(), 2);
    out += key;
    put_le(out, value.size(), 2);
    out += value;
  }
  assert(out.size() <= UINT16_MAX);
  return out;
}

/* Insert key with value in key order, room allowing; see page.h. */
int page_insert(page_t &page, std::string_view key, std::string_view value,
                size_t limit) {
  size_t size = entry_size(key.size(), value.size());
  if (size > entry_max(limit)) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "tree page: an entry over a third of its page");
  }
  auto at =
      std::lower_bound(page.entries.begin(), page.entries.end(), key, key_less);
  if (page.entries.end() != at && at->first == key) {
    return 1;
  }
  if (page_size(page) + size > limit) {
    return 2;
  }
  page.entries.emplace(at, std::string(key), std::string(value));
  return 0;
}

/* Insert an entry by splicing a page's bytes; see page.h. */
int page_insert_bytes(const page_view_t &v, std::string_view key,
                      std::string_view value, size_t limit, std::string &out) {
  size_t size = entry_size(key.size(), value.size());
  if (size > entry_max(limit)) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "tree page: an entry over a third of its page");
  }
  size_t n = v.count();
  size_t i = v.lower_bound(key);
  if (i < n && v.key(i) == key) {
    return 1;
  }
  std::string_view old = v.bytes();
  if (old.size() + size > limit) {
    return 2;
  }
  // The new entry goes where entry i begins, the entries being end to
  // end in slot order (check()); its slot goes in slot i's place.
  size_t slots = v.slots_offset();
  size_t first = slots + 2 * n;
  size_t at = i < n ? v.offset(i) : old.size();
  size_t grown = size - 2; /* the entry's bytes, past its slot */
  out.resize(old.size() + size);
  char *p = out.data();
  memcpy(p, old.data(), slots);
  // The header: one more entry, the version moved.
  uint64_t count = n + 1, version = v.version() + 1;
  for (int b = 0; b < 2; b++) {
    p[2 + b] = static_cast<char>((count >> (8 * b)) & 0xFF);
  }
  for (int b = 0; b < 8; b++) {
    p[4 + b] = static_cast<char>((version >> (8 * b)) & 0xFF);
  }
  auto put16 = [&](size_t pos, size_t x) {
    p[pos] = static_cast<char>(x & 0xFF);
    p[pos + 1] = static_cast<char>(x >> 8);
  };
  for (size_t j = 0; j < n; j++) {
    size_t o = v.offset(j) + 2 + (j < i ? 0 : grown);
    put16(slots + 2 * (j < i ? j : j + 1), o);
  }
  put16(slots + 2 * i, at + 2);
  // The entries before it, it, and those after.
  size_t to = first + 2;
  memcpy(p + to, old.data() + first, at - first);
  to += at - first;
  put16(to, key.size());
  memcpy(p + to + 2, key.data(), key.size());
  put16(to + 2 + key.size(), value.size());
  memcpy(p + to + 4 + key.size(), value.data(), value.size());
  to += grown;
  memcpy(p + to, old.data() + at, old.size() - at);
  return 0;
}

/* Insert an entry into a full page and split it; see page.h. */
void page_split(page_t &page, page_t &right, std::string_view key,
                std::string_view value, size_t limit) {
  assert(entry_size(key.size(), value.size()) <= entry_max(limit));
  auto ins =
      std::lower_bound(page.entries.begin(), page.entries.end(), key, key_less);
  assert(page.entries.end() == ins || ins->first != key);
  page.entries.emplace(ins, std::string(key), std::string(value));
  size_t total = 0;
  for (const auto &[k, v] : page.entries) {
    total += entry_size(k.size(), v.size());
  }
  // The first entry of the upper half: of the points where both
  // halves fit, the one nearest half the bytes. entry_max keeps one
  // (page.h).
  size_t at = 0;
  size_t best = SIZE_MAX;
  size_t lower = 0;
  for (size_t i = 1; i < page.entries.size(); i++) {
    lower += entry_size(page.entries[i - 1].first.size(),
                        page.entries[i - 1].second.size());
    size_t left = page_header_size + page.entries[i].first.size() + lower;
    size_t upper = page_header_size + page.high.size() + total - lower;
    if (left > limit || upper > limit) {
      continue;
    }
    size_t skew = left > upper ? left - upper : upper - left;
    if (skew < best) {
      best = skew;
      at = i;
    }
  }
  assert(0 != at);
  right = page_t{};
  right.level = page.level;
  right.flags = page.flags & page_has_high;
  right.next = page.next;
  right.high = std::move(page.high);
  right.entries.assign(std::make_move_iterator(page.entries.begin() + at),
                       std::make_move_iterator(page.entries.end()));
  page.entries.resize(at);
  page.flags |= page_has_high;
  page.high = right.entries.front().first;
  page.next = 0;
}

/* Check bytes, a page's, and view them; see page.h. */
int page_view_t::open(std::string_view bytes) {
  *this = page_view_t{};
  if (bytes.size() < page_header_size) {
    return corrupt("shorter than its header");
  }
  const char *p = bytes.data();
  uint8_t flags = static_cast<uint8_t>(p[1]);
  size_t count = get_le(p + 2, 2);
  size_t high_len = get_le(p + 28, 2);
  if (0 == (flags & page_has_high) && 0 != high_len) {
    return corrupt("a high key with no flag for it");
  }
  size_t slots = page_header_size + high_len;
  size_t entries = slots + 2 * count;
  if (entries > bytes.size()) {
    return corrupt("slots past its end");
  }
  m_bytes = bytes;
  m_level = static_cast<uint8_t>(p[0]);
  m_flags = flags;
  m_count = count;
  m_version = get_le(p + 4, 8);
  m_prev = get_le(p + 12, 8);
  m_next = get_le(p + 20, 8);
  m_high = bytes.substr(page_header_size, high_len);
  m_slots = slots;
  return 0;
}

/* Check every entry lies within the page; see page.h. */
int page_view_t::check() const {
  const char *p = m_bytes.data();
  size_t entries = m_slots + 2 * m_count;
  size_t end = entries;
  for (size_t i = 0; i < m_count; i++) {
    size_t at = get_le(p + m_slots + 2 * i, 2);
    if (at != end || at + 2 > m_bytes.size()) {
      return corrupt("an entry's offset out of the page, or out of order");
    }
    size_t key_len = get_le(p + at, 2);
    if (at + 2 + key_len + 2 > m_bytes.size()) {
      return corrupt("an entry's key past its end");
    }
    size_t value_len = get_le(p + at + 2 + key_len, 2);
    end = at + 2 + key_len + 2 + value_len;
    if (end > m_bytes.size()) {
      return corrupt("an entry's value past its end");
    }
  }
  return 0;
}

/* Entry i's offset, from its slot. */
size_t page_view_t::offset(size_t i) const {
  assert(i < m_count);
  return get_le(m_bytes.data() + m_slots + 2 * i, 2);
}

/*
 * The length-prefixed bytes at at, empty if they run past the page:
 * open() checked no entry, so each read stays within the bytes.
 */
std::string_view page_view_t::field(size_t at) const {
  if (at + 2 > m_bytes.size()) {
    return {};
  }
  size_t len = get_le(m_bytes.data() + at, 2);
  if (at + 2 + len > m_bytes.size()) {
    return {};
  }
  return std::string_view(m_bytes.data() + at + 2, len);
}

/* Entry i's key; see page.h. */
std::string_view page_view_t::key(size_t i) const { return field(offset(i)); }

/* Entry i's value; see page.h. */
std::string_view page_view_t::value(size_t i) const {
  size_t at = offset(i);
  return field(at + 2 + field(at).size());
}

/* Entries i to j - 1, their bytes in order; see page.h. */
std::string_view page_view_t::span(size_t i, size_t j) const {
  if (i >= j) {
    return {};
  }
  size_t from = offset(i);
  size_t last = offset(j - 1);
  std::string_view k = field(last);
  std::string_view v = field(last + 2 + k.size());
  size_t to = last + 2 + k.size() + 2 + v.size();
  if (from > to || to > m_bytes.size()) {
    return {};
  }
  return m_bytes.substr(from, to - from);
}

/* The first entry whose key is at or past key; see page.h. */
size_t page_view_t::lower_bound(std::string_view key) const {
  size_t lo = 0;
  size_t hi = m_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (this->key(mid) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* An inner page's entry to descend by for key; see page.h. */
size_t page_view_t::child(std::string_view key) const {
  size_t at = lower_bound(key);
  if (at < m_count && this->key(at) == key) {
    return at;
  }
  return 0 == at ? 0 : at - 1;
}

/* The page decoded into out; see page.h. */
void page_view_t::load(page_t &out) const {
  out.level = m_level;
  out.flags = m_flags;
  out.version = m_version;
  out.prev = m_prev;
  out.next = m_next;
  out.high = std::string(m_high);
  out.entries.clear();
  out.entries.reserve(m_count);
  for (size_t i = 0; i < m_count; i++) {
    out.entries.emplace_back(std::string(key(i)), std::string(value(i)));
  }
}

} // namespace sm2::tree
