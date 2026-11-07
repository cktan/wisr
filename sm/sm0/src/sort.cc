/*
 * sort.cc -- a sort of records by key (../SORT_PLAN.md).
 *
 * A record is one array of bytes, the same in memory as in a run:
 * its length, then its key's length, its key and its value. An item
 * points to a record, and holds 8 bytes of its key inline, so that
 * most compares touch no record.
 *
 * Records that overrun the budget go to runs: files of records, each
 * written once and read once, start to end. A merge reads them in
 * step, through a heap of one head record a run.
 */
#include "sm0_sort.h"

#include "sm0_arena.h"
#include "sm0_codec.h"
#include "sm0_error.h"
#include "sm_sys.h"
#include "sm_vfd.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

/* How many records go by between two looks for an interrupt. */
constexpr size_t interrupt_interval = 4096;

/* The fewest runs one merge pass reads, for all a small budget. */
constexpr size_t merge_order_min = 6;

/* The longest range the sort of memory orders by insertion. */
constexpr size_t insertion_sort_max = 16;

/* The items the array starts with, at most. */
constexpr size_t items_first = 1024;

/* The most bytes a size_t takes as a varint. */
constexpr size_t varint_max = 10;

/* The smallest buffer: it must hold a record's length. */
constexpr size_t buffer_bytes_min = 16;

/* Numbers a sort's run files, across the process's sorts. */
std::atomic<uint64_t> sort_numbers{0};

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

/* All the bytes of a record, its length among them. */
size_t record_size(const char *record) {
  size_t length;
  const char *body = get_varint(record, length);
  return (body - record) + length;
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

/* Fail with SM_ERR_DUPKEY: two keys of a unique sort are equal. */
int duplicate_key() {
  return sm0::set_error(SM_ERR_DUPKEY, "key already exists");
}

/*
 * The compare the sort of memory goes by. Unequal abbreviations
 * decide most compares inline. A tie goes out of line, where two equal
 * keys are noted under unique. An item compared with itself, as a
 * pivot may be, is no duplicate.
 */
struct item_order_t {
  size_t offset;
  bool unique;
  bool duplicate = false;
  uint64_t compares = 0; /* counted by the range, off the hot path */

  /* Whether a orders before b. */
  bool less(const item_t &a, const item_t &b) {
    return a.abbrev != b.abbrev ? a.abbrev < b.abbrev : tie_less(a, b);
  }

  /* less, for equal abbreviations. */
  [[gnu::noinline]] bool tie_less(const item_t &a, const item_t &b) {
    int c = compare_items(a, b, offset);
    if (0 == c && unique && a.record != b.record) {
      duplicate = true;
    }
    return c < 0;
  }
};

/* Sort a short range: each item moves down to its place. */
void insertion_sort(item_t *a, size_t n, item_order_t &order) {
  order.compares += n;
  for (size_t i = 1; i < n && !order.duplicate; i++) {
    item_t item = a[i];
    size_t j = i;
    while (j > 0 && order.less(item, a[j - 1])) {
      a[j] = a[j - 1];
      j--;
    }
    a[j] = item;
  }
}

/* The middle of three items, by value. */
item_t median_of_three(const item_t &a, const item_t &b, const item_t &c,
                       item_order_t &order) {
  if (order.less(a, b)) {
    if (order.less(b, c)) {
      return b;
    }
    return order.less(a, c) ? c : a;
  }
  if (order.less(a, c)) {
    return a;
  }
  return order.less(b, c) ? c : b;
}

/*
 * Split a range around pivot: the place after which every item is at
 * or above it, every item up to there being at or below it. Both
 * scans stop on an item equal to pivot, so many equal keys still split
 * evenly. The median of three leaves an item on each side to stop them.
 */
size_t partition(item_t *a, size_t n, const item_t &pivot,
                 item_order_t &order) {
  order.compares += n;
  size_t i = 0, j = n - 1;
  for (;;) {
    while (order.less(a[i], pivot)) {
      i++;
    }
    while (order.less(pivot, a[j])) {
      j--;
    }
    if (i >= j || order.duplicate) {
      return j + 1;
    }
    std::swap(a[i], a[j]);
    i++;
    j--;
  }
}

/*
 * Sort a range, stopping at a duplicate or an interrupt: 0, or -1 with
 * the error set. A duplicate stops it with 0, and order says so.
 * 1. Sort a short range by insertion.
 * 2. Past depth splits the input is a bad case: finish by heap sort,
 *    which has none. It cannot stop early, but it is rare.
 * 3. Split around the median of three.
 * 4. Sort the smaller side by recursion, and go on with the larger,
 *    which bounds the stack.
 */
int quick_sort(item_t *a, size_t n, size_t depth, item_order_t &order) {
  while (n > insertion_sort_max) {
    if (order.duplicate) {
      return 0;
    }
    if (n >= interrupt_interval && 0 != check_interrupt()) {
      return -1;
    }
    if (0 == depth) {
      order.compares += n;
      auto less = [&order](const item_t &x, const item_t &y) {
        return order.less(x, y);
      };
      std::make_heap(a, a + n, less);
      std::sort_heap(a, a + n, less);
      return 0;
    }
    depth--;
    item_t pivot = median_of_three(a[0], a[n / 2], a[n - 1], order);
    size_t left = partition(a, n, pivot, order);
    size_t right = n - left;
    if (left < right) {
      if (0 != quick_sort(a, left, depth, order)) {
        return -1;
      }
      a += left;
      n = right;
    } else {
      if (0 != quick_sort(a + left, right, depth, order)) {
        return -1;
      }
      n = left;
    }
  }
  insertion_sort(a, n, order);
  return 0;
}

/* A run: a file of records, written once and read once. */
struct run_t {
  sm_vfd_t vfd = 0;      /* 0 once the file has gone */
  std::string path;      /* the file */
  uint64_t bytes = 0;    /* the file's size, so a short read shows */
  uint64_t records = 0;  /* the records it holds */
  std::string first_key; /* its least key */
  size_t prefix = 0;     /* what all its keys start with */
};

/* A run being read: a buffer, and how far through the file it is. */
struct reader_t {
  size_t run;          /* the run's place in sort_t::runs */
  uint64_t offset = 0; /* the file's bytes read so far */
  uint64_t left = 0;   /* the records not yet stepped to */
  std::vector<char> buf;
  size_t pos = 0; /* the next record, in buf */
  size_t end = 0; /* the bytes held, in buf */
};

/* A run's head record, in the merge's heap. */
struct head_t {
  item_t item;
  reader_t *reader;
};

/* One pass of the merge: a reader a run, and their heads. */
struct merge_t {
  std::vector<reader_t> readers;
  std::vector<head_t> heap;
  size_t offset = 0; /* what every key of every run starts with */
};

} // namespace

struct sm0::sort_t {
  sort_options_t options;
  std::unique_ptr<arena_t> arena = std::make_unique<arena_t>();
  size_t record_bytes = 0;    /* what the arena's records take */
  std::vector<item_t> items;  /* one a record in memory */
  bool items_final = false;   /* the array has had its last growth */
  std::string_view first_key; /* the first record's, in the arena */
  size_t prefix = 0;          /* what every key in memory starts with */
  bool done = false;          /* the input has ended */
  size_t next = 0;            /* the item sort_next gives next */

  uint64_t number = 0;             /* names its run files */
  uint64_t runs_made = 0;          /* names its run files */
  sm_vfd_cache_t *cache = nullptr; /* made at the first spill */
  std::vector<run_t> runs;         /* a file each, oldest first */
  std::unique_ptr<merge_t> merge;  /* the last pass, read by sort_next */
  bool merge_started = false;      /* sort_next has given a record */
  bool have_last = false;          /* last_key holds a key */
  std::string last_key;            /* under unique, the last key seen */
  uint64_t compares = 0;           /* sort_compares gives it */
};

namespace {

using sm0::sort_t;

/* The memory the records and the array take. */
size_t memory_used(const sort_t *sort) {
  return sort->record_bytes + sort->items.capacity() * sizeof(item_t);
}

/*
 * Make room for one more item, within the budget: false if there is
 * none. The array doubles while that uses at most half the budget.
 * Then it grows once, by what the budget allows at the records' size
 * so far, so that it does not stop with the budget half used.
 */
bool grow_items(sort_t *sort) {
  size_t capacity = sort->items.capacity();
  if (0 == capacity) {
    sort->items.reserve(std::min(items_first, sort->options.budget / 64 + 1));
    return true;
  }
  if (sort->items_final) {
    return false;
  }
  size_t budget = sort->options.budget;
  size_t used = memory_used(sort);
  size_t wanted = capacity * 2;
  if (used > budget / 2) {
    sort->items_final = true;
    wanted = static_cast<size_t>(static_cast<double>(capacity) * budget / used);
    if (wanted <= capacity) {
      return false;
    }
  }
  sort->items.reserve(wanted);
  return true;
}

/*
 * Order the records in memory, past the prefix they all share. Under
 * unique it stops at the first compare that finds two equal keys: a
 * comparison sort must compare each pair of neighbours in the final
 * order, so two equal keys always meet.
 * 1. Take each key's abbreviation, past the prefix, which is exact
 *    for these records, so it can be no bad guess.
 * 2. Leave input already in order as it is, which one pass finds.
 * 3. Otherwise sort the items, which moves no record: by std::sort,
 *    or under unique by quick_sort, which can stop.
 */
int order_items(sort_t *sort) {
  const sm0::sort_options_t &opt = sort->options;
  std::vector<item_t> &items = sort->items;
  for (item_t &item : items) {
    item.abbrev = abbreviate(key_of(item.record), sort->prefix);
  }
  item_order_t order{sort->prefix, opt.unique};
  bool sorted = true;
  for (size_t i = 1; sorted && !order.duplicate && i < items.size(); i++) {
    sorted = !order.less(items[i], items[i - 1]);
    order.compares++;
  }
  int rc = 0;
  if (!sorted && !opt.unique) {
    std::sort(items.begin(), items.end(),
              [&order](const item_t &a, const item_t &b) {
                return order.less(a, b);
              });
    order.compares += items.size();
  } else if (!sorted && !order.duplicate) {
    size_t depth = opt.max_sort_depth;
    if (0 == depth) {
      depth = 2 * std::bit_width(items.size());
    }
    rc = quick_sort(items.data(), items.size(), depth, order);
  }
  sort->compares += order.compares;
  if (0 != rc) {
    return -1;
  }
  return order.duplicate ? duplicate_key() : 0;
}

/* Delete a run's file, if it has one. */
void drop_run(sort_t *sort, run_t &run) {
  if (0 != run.vfd) {
    sm_vfd_close(sort->cache, run.vfd);
    unlink(run.path.c_str());
    run.vfd = 0;
  }
}

/* Add an empty run, with its file; its place in sort->runs, in at. */
int new_run(sort_t *sort, size_t &at) {
  if (nullptr == sort->cache) {
    std::error_code ec;
    // The directory is the caller's to name; the first spill makes it.
    std::filesystem::create_directories(sort->options.dir, ec);
    if (ec) {
      return sm0::set_error(SM_ERR_IO, "sort: cannot make {}: {}",
                            sort->options.dir, ec.message());
    }
    sort->number = sort_numbers.fetch_add(1);
    // Its own cache: a sort has one thread, and a small share of fds.
    sort->cache = sm_vfd_cache_make(sort->options.fd_share);
  }
  run_t run;
  run.path = sort->options.dir + "/sort-" + std::to_string(getpid()) + "-" +
             std::to_string(sort->number) + "-" +
             std::to_string(sort->runs_made++);
  run.vfd = sm_vfd_open(sort->cache, run.path.c_str(),
                        O_RDWR | O_CREAT | O_EXCL, 0600);
  if (run.vfd < 0) {
    run.vfd = 0;
    return -1;
  }
  at = sort->runs.size();
  sort->runs.push_back(std::move(run));
  return 0;
}

/* Write n bytes at the run's end, whole. */
int write_bytes(sort_t *sort, run_t &run, const char *data, size_t n) {
  while (n > 0) {
    struct iovec iov = {const_cast<char *>(data), n};
    ssize_t wrote = sm_vfd_writev(sort->cache, run.vfd, &iov, 1,
                                  static_cast<off_t>(run.bytes));
    if (wrote < 0) {
      return -1;
    }
    run.bytes += static_cast<size_t>(wrote);
    data += wrote;
    n -= static_cast<size_t>(wrote);
  }
  return 0;
}

/* A run's records going out through a buffer, in order. */
class run_writer_t {
public:
  /* Write to the run at place at of sort. */
  run_writer_t(sort_t *sort, size_t at)
      : m_sort(sort), m_at(at), m_buf(sort->options.buffer_bytes) {}

  /* Add a record, whole. */
  int put(const char *record, size_t n) {
    run_t &run = m_sort->runs[m_at];
    run.records++;
    if (n > m_buf.size() - m_used) {
      if (0 != flush()) {
        return -1;
      }
      // A record over the buffer goes out as it is.
      if (n > m_buf.size()) {
        return write_bytes(m_sort, run, record, n);
      }
    }
    memcpy(m_buf.data() + m_used, record, n);
    m_used += n;
    return 0;
  }

  /* Write what the buffer holds. */
  int flush() {
    int rc = write_bytes(m_sort, m_sort->runs[m_at], m_buf.data(), m_used);
    m_used = 0;
    return rc;
  }

private:
  sort_t *m_sort;
  size_t m_at;
  std::vector<char> m_buf;
  size_t m_used = 0;
};

/*
 * Write the records in memory as a run, and empty memory for the
 * next.
 * 1. Order them, which finds a duplicate within the run now.
 * 2. Write them, as they are: a record is the same in a run.
 * 3. Give the arena back. The array stays for the next run.
 */
int spill(sort_t *sort) {
  size_t at;
  if (0 != order_items(sort) || 0 != new_run(sort, at)) {
    return -1;
  }
  run_writer_t writer(sort, at);
  for (const item_t &item : sort->items) {
    if (0 != writer.put(item.record, record_size(item.record))) {
      return -1;
    }
  }
  if (0 != writer.flush()) {
    return -1;
  }
  run_t &run = sort->runs[at];
  run.first_key = key_of(sort->items.front().record);
  run.prefix = sort->prefix;
  sort->items.clear();
  sort->arena = std::make_unique<sm0::arena_t>();
  sort->record_bytes = 0;
  sort->first_key = {};
  sort->prefix = 0;
  return 0;
}

/*
 * Fill reader r's buffer behind what it holds still. A record cut by
 * the buffer's end moves to the start, so it comes back whole.
 */
int refill(sort_t *sort, reader_t &r) {
  if (0 != check_interrupt()) {
    return -1;
  }
  run_t &run = sort->runs[r.run];
  memmove(r.buf.data(), r.buf.data() + r.pos, r.end - r.pos);
  r.end -= r.pos;
  r.pos = 0;
  while (r.end < r.buf.size() && r.offset < run.bytes) {
    struct iovec iov = {r.buf.data() + r.end, r.buf.size() - r.end};
    ssize_t got = sm_vfd_readv(sort->cache, run.vfd, &iov, 1,
                               static_cast<off_t>(r.offset));
    if (got < 0) {
      return -1;
    }
    if (0 == got) {
      return sm0::set_error(SM_ERR_IO, "sort: {} is short", run.path);
    }
    r.offset += static_cast<size_t>(got);
    r.end += static_cast<size_t>(got);
  }
  // The next buffer's worth, while the merge works through this one.
  sm_vfd_prefetch(sort->cache, run.vfd, static_cast<off_t>(r.offset),
                  static_cast<off_t>(sort->options.buffer_bytes));
  return 0;
}

/*
 * Step reader r to its next record: 0 with it in record, good until
 * the next step; 1 past the last, the run's file deleted then.
 */
int step_reader(sort_t *sort, reader_t &r, const char *&record) {
  if (0 == r.left) {
    drop_run(sort, sort->runs[r.run]);
    return 1;
  }
  if (r.end - r.pos < varint_max && 0 != refill(sort, r)) {
    return -1;
  }
  size_t length;
  const char *start = r.buf.data() + r.pos;
  size_t size = (get_varint(start, length) - start) + length;
  if (r.end - r.pos < size) {
    // A record over the buffer: the buffer grows to hold it.
    if (size > r.buf.size()) {
      r.buf.resize(size);
    }
    if (0 != refill(sort, r)) {
      return -1;
    }
    if (r.end - r.pos < size) {
      return sm0::set_error(SM_ERR_IO, "sort: {} is short",
                            sort->runs[r.run].path);
    }
  }
  record = r.buf.data() + r.pos;
  r.pos += size;
  r.left--;
  return 0;
}

/* Move a head down from place i, until neither child is less. */
void sift_down(merge_t *m, size_t i) {
  std::vector<head_t> &heap = m->heap;
  head_t head = heap[i];
  for (;;) {
    size_t child = 2 * i + 1;
    if (child >= heap.size()) {
      break;
    }
    if (child + 1 < heap.size() &&
        compare_items(heap[child + 1].item, heap[child].item, m->offset) < 0) {
      child++;
    }
    if (compare_items(head.item, heap[child].item, m->offset) <= 0) {
      break;
    }
    heap[i] = heap[child];
    i = child;
  }
  heap[i] = head;
}

/*
 * Replace the top of the heap by its run's next record, in one sift.
 * A run at its end leaves the heap, its place taken by the last head.
 */
int advance(sort_t *sort, merge_t *m) {
  head_t &top = m->heap.front();
  const char *record = nullptr;
  int rc = step_reader(sort, *top.reader, record);
  if (rc < 0) {
    return -1;
  }
  if (0 == rc) {
    top.item = {abbreviate(key_of(record), m->offset), record};
  } else {
    top = m->heap.back();
    m->heap.pop_back();
  }
  if (!m->heap.empty()) {
    sift_down(m, 0);
  }
  return 0;
}

/*
 * What every key of the runs from first, count of them, starts with.
 * Each run's keys start with its first key's prefix, so this is what
 * those prefixes share: two runs may each have a long one, and differ
 * in its first byte.
 */
size_t runs_prefix(const sort_t *sort, size_t first, size_t count) {
  const run_t &one = sort->runs[first];
  size_t prefix = one.prefix;
  for (size_t i = first + 1; i < first + count; i++) {
    const run_t &run = sort->runs[i];
    prefix = std::min(prefix, run.prefix);
    prefix = shared_prefix(std::string_view(one.first_key).substr(0, prefix),
                           run.first_key);
  }
  return prefix;
}

/* Start a merge of the runs from first, count of them. */
int merge_open(sort_t *sort, size_t first, size_t count,
               std::unique_ptr<merge_t> &out) {
  auto m = std::make_unique<merge_t>();
  m->offset = runs_prefix(sort, first, count);
  // A head points into its reader, which must not move.
  m->readers.resize(count);
  m->heap.reserve(count);
  for (size_t i = 0; i < count; i++) {
    reader_t &r = m->readers[i];
    r.run = first + i;
    r.left = sort->runs[r.run].records;
    r.buf.resize(sort->options.buffer_bytes);
    const char *record = nullptr;
    int rc = step_reader(sort, r, record);
    if (rc < 0) {
      return -1;
    }
    if (0 == rc) {
      m->heap.push_back({{abbreviate(key_of(record), m->offset), record}, &r});
    }
  }
  for (size_t i = m->heap.size() / 2; i-- > 0;) {
    sift_down(m.get(), i);
  }
  out = std::move(m);
  return 0;
}

/* Under unique, fail if key equals the key before it. */
int check_unique(sort_t *sort, std::string_view key) {
  if (!sort->options.unique) {
    return 0;
  }
  if (sort->have_last && key == sort->last_key) {
    return duplicate_key();
  }
  sort->have_last = true;
  sort->last_key.assign(key);
  return 0;
}

/*
 * Merge the oldest count runs into a new one, which goes last, and
 * forget the runs read.
 */
int merge_pass(sort_t *sort, size_t count) {
  std::unique_ptr<merge_t> m;
  size_t at;
  if (0 != merge_open(sort, 0, count, m) || 0 != new_run(sort, at)) {
    return -1;
  }
  run_writer_t writer(sort, at);
  sort->have_last = false;
  bool first = true;
  while (!m->heap.empty()) {
    const char *record = m->heap.front().item.record;
    std::string_view key = key_of(record);
    if (0 != check_unique(sort, key) ||
        0 != writer.put(record, record_size(record))) {
      return -1;
    }
    if (first) {
      sort->runs[at].first_key = key;
      first = false;
    }
    if (0 != advance(sort, m.get())) {
      return -1;
    }
  }
  if (0 != writer.flush()) {
    return -1;
  }
  sort->runs[at].prefix = m->offset;
  sort->have_last = false;
  sort->runs.erase(sort->runs.begin(), sort->runs.begin() + count);
  return 0;
}

} // namespace

/* Open a sort; see sm0_sort.h. */
int sm0::sort_open(const sort_options_t &options, sort_t *&out) {
  // A buffer must hold a record's length; a pass must shrink the runs.
  if (options.dir.empty() || 0 == options.budget ||
      options.buffer_bytes < buffer_bytes_min || options.max_runs < 2 ||
      options.fd_share < 1) {
    return set_error(SM_ERR_INVALID, "sort: an option out of its range");
  }
  auto sort = std::make_unique<sort_t>();
  sort->options = options;
  out = sort.release();
  return 0;
}

/*
 * Add a record.
 * 1. With memory full, write what it holds as a run.
 * 2. Copy the record into the arena.
 * 3. Note how it shortens the common prefix.
 */
int sm0::sort_put(sort_t *sort, std::string_view key, std::string_view value) {
  if (sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has ended");
  }
  if (0 == sort->items.size() % interrupt_interval && 0 != check_interrupt()) {
    return -1;
  }
  const sort_options_t &opt = sort->options;
  bool full =
      memory_used(sort) >= opt.budget ||
      (0 != opt.max_run_records && sort->items.size() >= opt.max_run_records) ||
      (0 != opt.max_run_size && sort->record_bytes >= opt.max_run_size);
  if (!full && sort->items.size() == sort->items.capacity()) {
    full = !grow_items(sort);
  }
  if (full && !sort->items.empty() && 0 != spill(sort)) {
    return -1;
  }
  size_t body = varint_size(key.size()) + key.size() + value.size();
  size_t size = varint_size(body) + body;
  char *record = sort->arena->alloc(size);
  char *k = put_varint(put_varint(record, body), key.size());
  memcpy(k, key.data(), key.size());
  memcpy(k + key.size(), value.data(), value.size());
  sort->record_bytes += size;
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
 * End the input.
 * 1. With no run, order memory: the sort never touches a file.
 * 2. Otherwise write what memory holds as the last run, and free it.
 * 3. While there are more runs than one pass reads, merge the oldest
 *    into a new run.
 * 4. Open the last pass, which sort_next reads, and which writes
 *    nothing.
 */
int sm0::sort_done(sort_t *sort) {
  if (sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has ended");
  }
  if (0 != check_interrupt()) {
    return -1;
  }
  sort->done = true;
  if (sort->runs.empty()) {
    return order_items(sort);
  }
  if (!sort->items.empty() && 0 != spill(sort)) {
    return -1;
  }
  std::vector<item_t>().swap(sort->items);
  // As many runs as the budget holds buffers, less one for output.
  const sort_options_t &opt = sort->options;
  size_t buffers = opt.budget / opt.buffer_bytes;
  size_t order = std::max(buffers > 0 ? buffers - 1 : 0, merge_order_min);
  order = std::min(order, opt.max_runs);
  while (sort->runs.size() > order) {
    if (0 != merge_pass(sort, order)) {
      return -1;
    }
  }
  return merge_open(sort, 0, sort->runs.size(), sort->merge);
}

/*
 * The next record in key order. A merge steps past the last record
 * only now, so that record stays good until this call.
 */
int sm0::sort_next(sort_t *sort, std::string_view &key,
                   std::string_view &value) {
  if (!sort->done) {
    return set_error(SM_ERR_INVALID, "sort: the input has not ended");
  }
  if (!sort->merge) {
    if (sort->next == sort->items.size()) {
      return 1;
    }
    split_record(sort->items[sort->next++].record, key, value);
    return 0;
  }
  merge_t *m = sort->merge.get();
  if (sort->merge_started && !m->heap.empty() && 0 != advance(sort, m)) {
    return -1;
  }
  sort->merge_started = true;
  if (m->heap.empty()) {
    return 1;
  }
  split_record(m->heap.front().item.record, key, value);
  return check_unique(sort, key);
}

/* The compares of sorts of memory so far; see sm0_sort.h. */
uint64_t sm0::sort_compares(const sort_t *sort) { return sort->compares; }

/* Free a sort, and delete the run files it has left; see sm0_sort.h. */
void sm0::sort_free(sort_t *sort) {
  if (nullptr == sort) {
    return;
  }
  for (run_t &run : sort->runs) {
    drop_run(sort, run);
  }
  // Closes every fd the cache holds.
  sm_vfd_cache_free(sort->cache);
  delete sort;
}
