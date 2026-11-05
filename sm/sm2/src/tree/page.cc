/*
 * page.cc -- a B+tree page's bytes, laid out and read where they lie
 * (../BTREEINDEX.md §4), and a decoded page's insert and split.
 *
 *   header = level (1B) | flags (1B) | max (2B) | top (2B) | version (8B)
 *          | prev (8B) | next (8B) | high key's length (2B) | high key
 *   slots  = max offsets (2B each), the first top of them valid and in
 *            key order, each from the slot array's end; the rest zero
 *   entry  = key's length (2B) | key | value's length (2B) | value
 *
 * Entries lie end to end, in no order, to the end of the page's bytes:
 * growing the slot array moves them up with no slot changing. Integers
 * are little-endian. A page stays under 64 KB, as an offset is two
 * bytes; its limit is far below that.
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

/* The bytes n entries' slots leave unused. */
size_t unused_slots(size_t n) { return 2 * (page_slots(n) - n); }

/*
 * Set page's page_all_visible by its entries: a leaf is all visible if
 * no entry on it is in doubt (../BTREEINDEX.md §9). An inner page never is.
 */
void set_all_visible(page_t &page) {
  bool set = 0 == page.level;
  for (size_t i = 0; set && i < page.entries.size(); i++) {
    set = !entry_in_doubt(page.entries[i].second);
  }
  page.flags = static_cast<uint8_t>(set ? page.flags | page_all_visible
                                        : page.flags & ~page_all_visible);
}

} // namespace

/* The bytes page takes, laid out; see page.h. */
size_t page_size(const page_t &page) {
  size_t n =
      page_header_size + page.high.size() + unused_slots(page.entries.size());
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
  size_t max = page_slots(page.entries.size());
  put_le(out, max, 2);
  put_le(out, page.entries.size(), 2);
  put_le(out, page.version, 8);
  put_le(out, page.prev, 8);
  put_le(out, page.next, 8);
  put_le(out, page.high.size(), 2);
  out += page.high;
  // The slots, then the entries they point at, in the same order.
  size_t at = 0;
  for (const auto &[key, value] : page.entries) {
    put_le(out, at, 2);
    at += 2 + key.size() + 2 + value.size();
  }
  out.append(unused_slots(page.entries.size()), '\0');
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
  size_t n = page.entries.size();
  size_t after = page_size(page) - unused_slots(n) + size + unused_slots(n + 1);
  if (after > limit) {
    return 2;
  }
  page.entries.emplace(at, std::string(key), std::string(value));
  page.flags &= static_cast<uint8_t>(~page_all_visible);
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
  size_t bytes = size - 2; /* the entry's, past its slot */
  size_t slots = v.slots_offset();
  size_t first = v.entries_offset();
  size_t max = (first - slots) / 2;
  size_t grown_max = n < max ? max : max + page_slot_step;
  size_t new_first = slots + 2 * grown_max;
  size_t used = old.size() - first; /* the entries' bytes */
  size_t total = new_first + used + bytes;
  if (total > limit) {
    return 2;
  }
  out.resize(total);
  char *p = out.data();
  auto put = [&](size_t pos, uint64_t x, size_t len) {
    for (size_t b = 0; b < len; b++) {
      p[pos + b] = static_cast<char>((x >> (8 * b)) & 0xFF);
    }
  };
  memcpy(p, old.data(), slots);
  // The header: no longer all visible, as the new entry is not
  // settled; the slots' count, one more in use, the version moved.
  p[1] = static_cast<char>(v.flags() & ~page_all_visible);
  put(2, grown_max, 2);
  put(4, n + 1, 2);
  put(6, v.version() + 1, 8);
  // The slots: those from i up one, and i the new entry, which goes
  // after the others. No offset changes, being from the array's end.
  memcpy(p + slots, old.data() + slots, 2 * i);
  put(slots + 2 * i, used, 2);
  memcpy(p + slots + 2 * (i + 1), old.data() + slots + 2 * i, 2 * (n - i));
  memset(p + slots + 2 * (n + 1), 0, 2 * (grown_max - n - 1));
  memcpy(p + new_first, old.data() + first, used);
  size_t to = new_first + used;
  put(to, key.size(), 2);
  memcpy(p + to + 2, key.data(), key.size());
  put(to + 2 + key.size(), value.size(), 2);
  memcpy(p + to + 4 + key.size(), value.data(), value.size());
  return 0;
}

/* Mark an entry in doubt in a page's bytes; see page.h. */
bool page_mark_in_doubt(const page_view_t &v, size_t i, std::string &out) {
  std::string_view value = i < v.count() ? v.value(i) : std::string_view();
  bool mark = !value.empty() && !entry_in_doubt(value);
  if (!mark && 0 == (v.flags() & page_all_visible)) {
    return false;
  }
  std::string_view old = v.bytes();
  out.assign(old);
  char *p = out.data();
  p[1] = static_cast<char>(v.flags() & ~page_all_visible);
  for (size_t b = 0; b < 8; b++) {
    p[6 + b] = static_cast<char>(((v.version() + 1) >> (8 * b)) & 0xFF);
  }
  if (mark) {
    p[value.data() - old.data()] |= static_cast<char>(entry_indoubt);
  }
  return true;
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
    size_t left = page_header_size + page.entries[i].first.size() + lower +
                  unused_slots(i);
    size_t upper = page_header_size + page.high.size() + total - lower +
                   unused_slots(page.entries.size() - i);
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
  set_all_visible(page);
  set_all_visible(right);
}

/* Check bytes, a page's, and view them; see page.h. */
int page_view_t::open(std::string_view bytes) {
  *this = page_view_t{};
  if (bytes.size() < page_header_size) {
    return corrupt("shorter than its header");
  }
  if (bytes.size() > UINT16_MAX) {
    return corrupt("past what an offset reaches");
  }
  const char *p = bytes.data();
  uint8_t flags = static_cast<uint8_t>(p[1]);
  size_t max = get_le(p + 2, 2);
  size_t count = get_le(p + 4, 2);
  size_t high_len = get_le(p + 30, 2);
  if (count > max) {
    return corrupt("more slots in use than it has");
  }
  if (0 == (flags & page_has_high) && 0 != high_len) {
    return corrupt("a high key with no flag for it");
  }
  size_t slots = page_header_size + high_len;
  size_t entries = slots + 2 * max;
  if (entries > bytes.size()) {
    return corrupt("slots past its end");
  }
  m_bytes = bytes;
  m_level = static_cast<uint8_t>(p[0]);
  m_flags = flags;
  m_count = count;
  m_version = get_le(p + 6, 8);
  m_prev = get_le(p + 14, 8);
  m_next = get_le(p + 22, 8);
  m_high = bytes.substr(page_header_size, high_len);
  m_slots = slots;
  m_entries = entries;
  return 0;
}

/* Check every entry lies within the page; see page.h. */
int page_view_t::check() const {
  const char *p = m_bytes.data();
  size_t area = m_bytes.size() - m_entries;
  // One bit a byte of the entries': a slot's mark, so each entry met
  // below is known to have a slot, and to have only one.
  uint64_t marks[(UINT16_MAX + 1) / 64];
  memset(marks, 0, (area + 63) / 64 * sizeof marks[0]);
  for (size_t i = 0; i < m_count; i++) {
    size_t at = get_le(p + m_slots + 2 * i, 2);
    if (at >= area || 0 != (marks[at / 64] >> (at % 64) & 1)) {
      return corrupt("a slot out of the page, or two of one entry");
    }
    marks[at / 64] |= uint64_t(1) << (at % 64);
  }
  size_t at = m_entries;
  for (size_t i = 0; i < m_count; i++) {
    size_t rel = at - m_entries;
    if (at + 2 > m_bytes.size() || 0 == (marks[rel / 64] >> (rel % 64) & 1)) {
      return corrupt("an entry with no slot");
    }
    size_t key_len = get_le(p + at, 2);
    if (at + 2 + key_len + 2 > m_bytes.size()) {
      return corrupt("an entry's key past its end");
    }
    size_t value_len = get_le(p + at + 2 + key_len, 2);
    at += 2 + key_len + 2 + value_len;
    if (at > m_bytes.size()) {
      return corrupt("an entry's value past its end");
    }
  }
  if (at != m_bytes.size()) {
    return corrupt("bytes past its last entry");
  }
  return 0;
}

/* Where entry i lies, from its slot. */
size_t page_view_t::offset(size_t i) const {
  assert(i < m_count);
  return m_entries + get_le(m_bytes.data() + m_slots + 2 * i, 2);
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

/* Entry i's bytes; see page.h. */
std::string_view page_view_t::entry(size_t i) const {
  size_t at = offset(i);
  std::string_view k = field(at);
  std::string_view v = field(at + 2 + k.size());
  size_t to = at + 2 + k.size() + 2 + v.size();
  if (to > m_bytes.size()) {
    return {};
  }
  return m_bytes.substr(at, to - at);
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
