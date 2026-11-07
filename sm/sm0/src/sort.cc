/*
 * sort.cc -- a sort of records by key (../SORT_PLAN.md).
 *
 * A record is one array of bytes, the same in memory as in a run:
 * its length, then its key's length, its key and its value. An item
 * points to a record, and holds 8 bytes of its key inline, so that
 * most compares touch no record.
 */
#include "sm0_sort.h"

#include "sm0_arena.h"
#include "sm0_codec.h"
#include "sm0_error.h"
#include "sm_sys.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* How many records go by between two looks for an interrupt. */
constexpr size_t interrupt_interval = 4096;

/* A record in memory, as the sort moves it. */
struct item_t {
  uint64_t abbrev;    /* 8 bytes of its key, past the common prefix */
  const char *record; /* its bytes, in the arena */
};

/* The bytes v takes as a varint. */
size_t varint_size(size_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

/* Write v at p as a varint, seven bits a byte; past what it wrote. */
char *put_varint(char *p, size_t v) {
  while (v >= 0x80) {
    *p++ = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<char>(v);
  return p;
}

/* Read a varint at p into v; past what it read. */
const char *get_varint(const char *p, size_t &v) {
  v = 0;
  for (int shift = 0;; shift += 7) {
    uint8_t b = static_cast<uint8_t>(*p++);
    v |= static_cast<size_t>(b & 0x7F) << shift;
    if (0 == (b & 0x80)) {
      return p;
    }
  }
}

/* The key and the value of a record. */
void split_record(const char *record, std::string_view &key,
                  std::string_view &value) {
  size_t length, key_length;
  const char *body = get_varint(record, length);
  const char *k = get_varint(body, key_length);
  key = {k, key_length};
  value = {k + key_length, length - (k - body) - key_length};
}

/* The key of a record. */
std::string_view key_of(const char *record) {
  std::string_view key, value;
  split_record(record, key, value);
  return key;
}

/*
 * 8 bytes of key from offset, as an integer that orders as they do. A
 * key that ends sooner is padded with zeros, so an equal abbreviation
 * says nothing of two keys' lengths.
 */
uint64_t abbreviate(std::string_view key, size_t offset) {
  uint64_t word = 0;
  if (offset < key.size()) {
    memcpy(&word, key.data() + offset,
           std::min(sizeof(word), key.size() - offset));
  }
  return sm0::to_be64(word);
}

/*
 * Order two items, by the bytes their keys share past offset, then
 * by length.
 * 1. Unequal abbreviations decide.
 * 2. Equal ones say the keys agree through offset + 8, or to the end
 *    of the shorter. Compare what is left of that.
 * 3. A key that is a prefix of the other sorts first.
 */
int compare_items(const item_t &a, const item_t &b, size_t offset) {
  if (a.abbrev != b.abbrev) {
    return a.abbrev < b.abbrev ? -1 : 1;
  }
  std::string_view ka = key_of(a.record), kb = key_of(b.record);
  size_t common = std::min(ka.size(), kb.size());
  size_t from = std::min(offset + sizeof(a.abbrev), common);
  int c = memcmp(ka.data() + from, kb.data() + from, common - from);
  if (0 != c) {
    return c;
  }
  return ka.size() == kb.size() ? 0 : ka.size() < kb.size() ? -1 : 1;
}

/* The length of the start of key that also starts first. */
size_t shared_prefix(std::string_view first, std::string_view key) {
  size_t n = std::min(first.size(), key.size());
  if (0 == memcmp(first.data(), key.data(), n)) {
    return n;
  }
  return std::mismatch(first.begin(), first.begin() + n, key.begin()).first -
         first.begin();
}

/* Fail if the calling thread has an interrupt to act on. */
int check_interrupt() {
  if (0 != (sm_pending() & sm_wait_mask())) {
    return sm0::set_error(SM_ERR_INTERRUPTED, "sort: interrupted");
  }
  return 0;
}

} // namespace

struct sm0::sort_t {
  std::string dir;
  size_t budget;
  bool unique;
  arena_t arena;              /* the records' bytes */
  std::vector<item_t> items;  /* one a record */
  std::string_view first_key; /* the first record's, in the arena */
  size_t prefix = 0;          /* what every key so far starts with */
  bool done = false;          /* the input has ended */
  size_t next = 0;            /* the item sort_next gives next */
};

/* Open a sort; see sm0_sort.h. */
int sm0::sort_open(const char *dir, size_t budget, bool unique, sort_t *&out) {
  if (nullptr == dir || 0 == budget) {
    return set_error(SM_ERR_INVALID, "sort: a directory and a budget");
  }
  out = new sort_t{dir, budget, unique, {}, {}, {}};
  return 0;
}

/* Copy a record into the arena, and note how it shortens the prefix. */
int sm0::sort_put(sort_t *sort, std::string_view key, std::string_view value) {
  if (sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has ended");
  }
  if (0 == sort->items.size() % interrupt_interval && 0 != check_interrupt()) {
    return -1;
  }
  size_t body = varint_size(key.size()) + key.size() + value.size();
  char *record = sort->arena.alloc(varint_size(body) + body);
  char *k = put_varint(put_varint(record, body), key.size());
  memcpy(k, key.data(), key.size());
  memcpy(k + key.size(), value.data(), value.size());
  if (sort->items.empty()) {
    sort->first_key = {k, key.size()};
    sort->prefix = key.size();
  } else {
    sort->prefix = shared_prefix(sort->first_key.substr(0, sort->prefix), key);
  }
  sort->items.push_back({0, record});
  return 0;
}

/*
 * Order the records.
 * 1. Take each key's abbreviation, past the prefix they all share,
 *    which is exact for these records, so it can be no bad guess.
 * 2. Leave input already in order as it is, which one pass finds.
 * 3. Otherwise sort the items, which moves no record.
 * 4. Under unique, look for two equal neighbours.
 */
int sm0::sort_done(sort_t *sort) {
  if (sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has ended");
  }
  if (0 != check_interrupt()) {
    return -1;
  }
  sort->done = true;
  size_t offset = sort->prefix;
  for (item_t &item : sort->items) {
    item.abbrev = abbreviate(key_of(item.record), offset);
  }
  auto less = [offset](const item_t &a, const item_t &b) {
    return compare_items(a, b, offset) < 0;
  };
  if (!std::is_sorted(sort->items.begin(), sort->items.end(), less)) {
    std::sort(sort->items.begin(), sort->items.end(), less);
  }
  if (sort->unique) {
    for (size_t i = 1; i < sort->items.size(); i++) {
      if (0 == compare_items(sort->items[i - 1], sort->items[i], offset)) {
        return set_error(SM_ERR_DUPKEY, "key already exists");
      }
    }
  }
  return 0;
}

/* The next record in key order; see sm0_sort.h. */
int sm0::sort_next(sort_t *sort, std::string_view &key,
                   std::string_view &value) {
  if (!sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has not ended");
  }
  if (sort->next == sort->items.size()) {
    return 1;
  }
  split_record(sort->items[sort->next++].record, key, value);
  return 0;
}

/* Free a sort; see sm0_sort.h. */
void sm0::sort_free(sort_t *sort) { delete sort; }
