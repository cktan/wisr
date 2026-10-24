/*
 * sample.cc -- the sample an analyze estimates from
 * (SM3_DESIGN.md §6.4): a pilot of single _ids, then runs of
 * _ids whose slots Floyd's algorithm draws, read through forward
 * cursors, with a reservoir keeping a uniform share of the rows
 * found; in a table with a primary key, slots of its bytes in key
 * order instead (../../ROWKEY.md §2.4). Nothing walks the table:
 * what is read is bounded by the rows wanted, whatever the table's
 * size.
 */

#include "analyze.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace {

/* The bytes of a PostgreSQL page, whose rows a run is as many as. */
constexpr uint64_t page_bytes = 8192;

/* The slots of a keyed table's bytes a pilot reads first. */
constexpr uint64_t pilot_slots = 30;

/*
 * A uniform want of the rows offered (Algorithm R), into out.rows,
 * with the count and the bytes of every row offered.
 */
struct reservoir_t {
  sm3::sample_t &out;
  uint64_t want;
  std::mt19937_64 &rng;
  uint64_t bytes = 0;

  /* Offer the row with _id id, key and tuple t. */
  void offer(uint64_t id, std::string_view key, std::string_view t) {
    out.rows_read++;
    bytes += t.size();
    if (out.rows.size() < want) {
      out.rows.push_back({id, std::string(t), std::string(key)});
      return;
    }
    std::uniform_int_distribution<uint64_t> d(0, out.rows_read - 1);
    uint64_t j = d(rng);
    if (j < want) {
      out.rows[j] = {id, std::string(t), std::string(key)};
    }
  }
};

/* A run of _ids, first to last, both included. */
struct run_t {
  uint64_t first;
  uint64_t last;
};

/*
 * Read the rows of runs, sorted and disjoint, through one forward
 * cursor over 1..n, calling emit(id, tuple) for each row found.
 * A row the cursor returns past one run is held for the next, so no
 * seek ever goes back.
 */
template <typename F>
int read_runs(sm3::table_t *table, uint64_t n, const std::vector<run_t> &runs,
              F emit) {
  sm3::scan_t *c = nullptr;
  if (sm3::tbl_scan(table, 1, n, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm3::scan_t, decltype(&sm3::tbl_scanend)> guard(
      c, &sm3::tbl_scanend);
  bool have = false, done = false;
  uint64_t id = 0;
  std::string_view tuple;
  // The next row, into id and tuple: have it, or be done.
  auto next = [&]() {
    int rc = sm3::tbl_scannxt(c, id, tuple);
    have = rc == 0;
    done = rc == 1;
    return rc < 0 ? -1 : 0;
  };
  for (const run_t &r : runs) {
    if (done) {
      break;
    }
    // Move to the run, unless the row in hand is in it or past it.
    if (!have || id < r.first) {
      if (sm3::tbl_scan_seek(c, r.first) != 0 || next() != 0) {
        return -1;
      }
    }
    while (have && id <= r.last) {
      emit(id, tuple);
      if (next() != 0) {
        return -1;
      }
    }
  }
  return 0;
}

/*
 * k more slots of 0..slots-1, none in chosen, drawn uniformly and
 * sorted. When they are most of what is left, what is left is
 * listed and drawn from; otherwise a draw that hits a slot taken is
 * drawn again, which takes under two draws a slot on average.
 */
std::vector<uint64_t> more_slots(uint64_t slots,
                                 const std::unordered_set<uint64_t> &chosen,
                                 uint64_t k, std::mt19937_64 &rng) {
  std::vector<uint64_t> out;
  if ((chosen.size() + k) * 2 > slots) {
    // Few slots: the budget bounds them, so listing them is cheap.
    std::vector<uint64_t> left;
    for (uint64_t s = 0; s < slots; s++) {
      if (!chosen.count(s)) {
        left.push_back(s);
      }
    }
    for (uint64_t i : sm3::floyd(left.size(), k, rng)) {
      out.push_back(left[i]);
    }
    return out;
  }
  std::unordered_set<uint64_t> drawn;
  std::uniform_int_distribution<uint64_t> d(0, slots - 1);
  while (drawn.size() < k) {
    uint64_t s = d(rng);
    if (!chosen.count(s)) {
      drawn.insert(s);
    }
  }
  out.assign(drawn.begin(), drawn.end());
  std::sort(out.begin(), out.end());
  return out;
}

/*
 * Read the rows of slots, sorted and distinct, of a table with a
 * primary key whose bytes are cut into s slots, through one cursor
 * in key order, calling emit(key, tuple) for each row found. A
 * slot's rows are those from the key at its start up to the key at
 * its end, the next slot's start, or the table's end for the last
 * (tbl_keys_at). A row the cursor returns past one slot is held for
 * the next, so no seek ever goes back. The bytes the slots span, by
 * sm1's estimate, are added to bytes.
 */
template <typename F>
int read_slots(sm3::table_t *table, uint64_t s,
               const std::vector<uint64_t> &slots, uint64_t &bytes, F emit) {
  // Each slot's two ends as fractions of the bytes, ascending; the
  // last slot's end is the table's.
  std::vector<double> fs;
  for (uint64_t x : slots) {
    fs.push_back(static_cast<double>(x) / s);
    if (x + 1 < s) {
      fs.push_back(static_cast<double>(x + 1) / s);
    }
  }
  std::vector<std::string> keys;
  if (0 != sm3::tbl_keys_at(table, fs, keys)) {
    return -1;
  }
  sm3::scan_t *c = nullptr;
  if (0 != sm3::tbl_scan_rows(table, c)) {
    return -1;
  }
  std::unique_ptr<sm3::scan_t, decltype(&sm3::tbl_scanend)> guard(
      c, &sm3::tbl_scanend);
  bool have = false, done = false;
  uint64_t id = 0;
  std::string_view tuple;
  // The next row, into tuple: have it, or be done.
  auto next = [&]() {
    int rc = sm3::tbl_scannxt(c, id, tuple);
    have = 0 == rc;
    done = 1 == rc;
    return rc < 0 ? -1 : 0;
  };
  size_t j = 0;
  for (uint64_t x : slots) {
    if (done) {
      break;
    }
    const std::string &start = keys[j++];
    const std::string *end = x + 1 < s ? &keys[j++] : nullptr;
    uint64_t span;
    if (0 != sm3::tbl_range_size(table, start, end, span)) {
      return -1;
    }
    bytes += span;
    // Move to the slot, unless the row in hand is in it or past it.
    if (!have || sm3::tbl_scan_key(c) < start) {
      if (0 != sm3::tbl_scan_seek_key(c, start) || 0 != next()) {
        return -1;
      }
    }
    while (have && (!end || sm3::tbl_scan_key(c) < *end)) {
      emit(sm3::tbl_scan_key(c), tuple);
      if (0 != next()) {
        return -1;
      }
    }
  }
  return 0;
}

/*
 * Sample a table with a primary key for want rows into out, by
 * slots of its bytes in key order (sample_table, analyze.h).
 */
int sample_by_key(sm3::table_t *table, uint64_t want, std::mt19937_64 &rng,
                  sm3::sample_t &out) {
  uint64_t disk, mem;
  if (0 != sm3::tbl_size(table, disk, mem)) {
    return -1;
  }
  // 1. The slots, a page of the bytes each, and a pilot of a few,
  // for the rows a slot.
  const uint64_t s =
      std::max<uint64_t>(1, (disk + mem + page_bytes - 1) / page_bytes);
  reservoir_t res{out, want, rng};
  auto keep = [&](std::string_view key, std::string_view t) {
    res.offer(0, key, t);
  };
  std::vector<uint64_t> next = sm3::floyd(s, std::min(s, pilot_slots), rng);
  std::unordered_set<uint64_t> chosen(next.begin(), next.end());
  if (0 != read_slots(table, s, next, out.bytes_covered, keep)) {
    return -1;
  }
  // None found is taken as fewer than one in the pilot.
  auto per_slot = [&]() {
    return out.rows_read > 0 ? static_cast<double>(out.rows_read) /
                                   static_cast<double>(chosen.size())
                             : 1.0 / static_cast<double>(chosen.size() + 1);
  };

  // 2. The budget, in slots: a table it covers is read whole, every
  // row then in the reservoir's draw and counted.
  uint64_t max_slots = std::min<uint64_t>(
      s, std::max<uint64_t>(chosen.size(),
                            static_cast<uint64_t>(std::ceil(
                                sm3::budget_factor * want / per_slot()))));
  if (max_slots == s) {
    out.rows.clear();
    out.rows_read = 0;
    res.bytes = 0;
    sm3::scan_t *c = nullptr;
    if (0 != sm3::tbl_scan_rows(table, c)) {
      return -1;
    }
    std::unique_ptr<sm3::scan_t, decltype(&sm3::tbl_scanend)> guard(
        c, &sm3::tbl_scanend);
    uint64_t id;
    std::string_view t;
    int rc;
    while (0 == (rc = sm3::tbl_scannxt(c, id, t))) {
      keep(sm3::tbl_scan_key(c), t);
    }
    if (rc < 0) {
      return -1;
    }
    out.whole = true;
    out.live_rows = static_cast<double>(out.rows_read);
    out.runs = s;
  } else {
    // 3. More slots by Floyd while the rows fall short and the
    // budget lasts, as many as the rows a slot says.
    while (out.rows_read < want && chosen.size() < max_slots) {
      uint64_t more = static_cast<uint64_t>(std::ceil(
          1.2 * static_cast<double>(want - out.rows_read) / per_slot()));
      next = more_slots(
          s, chosen, std::clamp<uint64_t>(more, 1, max_slots - chosen.size()),
          rng);
      chosen.insert(next.begin(), next.end());
      if (0 != read_slots(table, s, next, out.bytes_covered, keep)) {
        return -1;
      }
    }
    // 4. The row count: the rows a byte the slots spanned, times
    // the bytes; with no byte spanned, the rows a slot, times the
    // slots.
    out.live_rows = out.bytes_covered > 0
                        ? static_cast<double>(out.rows_read) *
                              static_cast<double>(disk + mem) /
                              static_cast<double>(out.bytes_covered)
                        : static_cast<double>(out.rows_read) *
                              static_cast<double>(s) /
                              static_cast<double>(chosen.size());
    out.runs = chosen.size();
  }
  out.run_length = static_cast<int32_t>(std::lround(per_slot()));
  if (out.whole) {
    out.run_length = static_cast<int32_t>(
        std::lround(static_cast<double>(out.rows_read) / s));
  }
  out.avg_row_bytes =
      out.rows_read > 0 ? static_cast<int32_t>(res.bytes / out.rows_read) : 0;
  // Key order, the rows' own, for the correlation of each column's
  // order with it.
  std::sort(out.rows.begin(), out.rows.end(),
            [](const sm3::sample_row_t &x, const sm3::sample_row_t &y) {
              return x.key < y.key;
            });
  return 0;
}

} // namespace

namespace sm3 {

/* Floyd's algorithm; see analyze.h. */
std::vector<uint64_t> floyd(uint64_t n, uint64_t k, std::mt19937_64 &rng) {
  k = std::min(k, n);
  std::unordered_set<uint64_t> s;
  s.reserve(k * 2);
  // Each j adds one number of 0..j: t if new, else j itself, which
  // no earlier step could have drawn.
  for (uint64_t j = n - k; j < n; j++) {
    std::uniform_int_distribution<uint64_t> d(0, j);
    uint64_t t = d(rng);
    s.insert(s.count(t) ? j : t);
  }
  std::vector<uint64_t> v(s.begin(), s.end());
  std::sort(v.begin(), v.end());
  return v;
}

/* Sample a table; see analyze.h. */
int sample_table(table_t *table, int32_t target, std::mt19937_64 &rng,
                 sample_t &out) {
  out = sample_t{};
  if (target < 1) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "analyze: a statistics target below 1");
  }
  const uint64_t want = rows_per_target * static_cast<uint64_t>(target);
  const table_def_t &def = tbl_def(table);
  // A table with a primary key has no _ids (../../ROWKEY.md §2.1).
  if (std::any_of(def.indexes.begin(), def.indexes.end(),
                  [](const index_def_t &x) { return x.primary; })) {
    return sample_by_key(table, want, rng, out);
  }
  uint64_t n = 0;
  if (tbl_id_limit(table, n) != 0) {
    return -1;
  }
  out.rowno_limit = n;
  if (n == 0) {
    out.whole = true;
    return 0;
  }

  // 1. The pilot: single _ids, for the live fraction and width.
  uint64_t m = std::min(rows_per_target, n);
  std::vector<run_t> pilot;
  for (uint64_t r : floyd(n, m, rng)) {
    pilot.push_back({r + 1, r + 1});
  }
  uint64_t live0 = 0, bytes0 = 0;
  if (read_runs(table, n, pilot, [&](uint64_t, std::string_view t) {
        live0++;
        bytes0 += t.size();
      }) != 0) {
    return -1;
  }

  // 2. The run length: an 8 KB page's rows at the pilot's width; a
  // pilot that found nothing live gives single _ids.
  uint64_t w0 = live0 > 0 ? std::max<uint64_t>(1, bytes0 / live0) : page_bytes;
  uint64_t b = std::min(n, std::max<uint64_t>(1, page_bytes / w0));
  out.run_length = static_cast<int32_t>(b);

  // 3. The slots, and how many of them the budget allows read.
  uint64_t slots = (n + b - 1) / b;
  uint64_t max_slots =
      std::min(slots, std::max<uint64_t>(1, budget_factor * want / b));
  // Dividing by the live fraction is the sparse-table rule; none
  // found live is taken as fewer than one in the pilot.
  double f0 = live0 > 0 ? static_cast<double>(live0) / m : 1.0 / (m + 1);
  uint64_t k = static_cast<uint64_t>(
      std::ceil(1.2 * static_cast<double>(want) / (b * f0)));
  k = std::clamp<uint64_t>(k, 1, max_slots);
  // A table the budget covers is read whole: every row is then in
  // the reservoir's draw, not only those of k runs, so a column
  // that follows the _ids is not estimated from a few clusters.
  if (max_slots == slots) {
    k = slots;
  }

  // 4. The runs, k slots by Floyd, and more while the rows fall
  // short and the budget lasts. A reservoir (Algorithm R) keeps a
  // uniform want of every row read.
  reservoir_t res{out, want, rng};
  auto keep = [&](uint64_t id, std::string_view t) { res.offer(id, {}, t); };
  std::unordered_set<uint64_t> chosen;
  std::vector<uint64_t> next = floyd(slots, k, rng);
  while (!next.empty()) {
    std::vector<run_t> runs;
    for (uint64_t s : next) {
      chosen.insert(s);
      uint64_t first = s * b + 1;
      uint64_t last = std::min(n, first + b - 1);
      runs.push_back({first, last});
      out.ids_covered += last - first + 1;
    }
    out.runs += runs.size();
    // A cursor a round, since this round's slots fall among the last.
    if (read_runs(table, n, runs, keep) != 0) {
      return -1;
    }
    uint64_t left = max_slots - chosen.size();
    if (out.rows_read >= want || left == 0) {
      break;
    }
    // Short: as many more slots as the live fraction found says.
    double f = out.rows_read > 0 ? static_cast<double>(out.rows_read) /
                                       static_cast<double>(out.ids_covered)
                                 : 1.0 / (out.ids_covered + 1);
    uint64_t more = static_cast<uint64_t>(
        std::ceil(1.2 * static_cast<double>(want - out.rows_read) / (b * f)));
    next = more_slots(slots, chosen, std::clamp<uint64_t>(more, 1, left), rng);
  }

  // 5. The row count: exact if every _id was covered, else the
  // live fraction of what was, times N.
  out.whole = out.ids_covered == n;
  out.live_rows = out.whole ? static_cast<double>(out.rows_read)
                            : static_cast<double>(out.rows_read) *
                                  static_cast<double>(n) /
                                  static_cast<double>(out.ids_covered);
  out.avg_row_bytes =
      out.rows_read > 0 ? static_cast<int32_t>(res.bytes / out.rows_read) : 0;
  // Rowno order, for the correlation of each column's order with it.
  std::sort(
      out.rows.begin(), out.rows.end(),
      [](const sample_row_t &x, const sample_row_t &y) { return x.id < y.id; });
  return 0;
}

} // namespace sm3
