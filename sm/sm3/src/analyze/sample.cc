/*
 * sample.cc -- the sample an analyze estimates from
 * (SM3_DESIGN.md §6.4): a pilot of single rownos, then runs of
 * rownos whose slots Floyd's algorithm draws, read through forward
 * cursors, with a reservoir keeping a uniform share of the rows
 * found. Nothing walks the table: what is read is bounded by the
 * rows wanted, whatever the table's size.
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

/* A run of rownos, first to last, both included. */
struct run_t {
  uint64_t first;
  uint64_t last;
};

/*
 * Read the rows of runs, sorted and disjoint, through one forward
 * cursor over 1..n, calling emit(rowno, tuple) for each row found.
 * A row the cursor returns past one run is held for the next, so no
 * seek ever goes back.
 */
template <typename F>
int read_runs(sm3::table_t *table, uint64_t n, const std::vector<run_t> &runs,
              F emit) {
  sm2::rel_scan_t *c = nullptr;
  if (sm3::tbl_scan(table, 1, n, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::rel_scan_t, decltype(&sm2::rel_scanend)> guard(
      c, &sm2::rel_scanend);
  bool have = false, done = false;
  uint64_t rowno = 0;
  std::string_view tuple;
  // The next row, into rowno and tuple: have it, or be done.
  auto next = [&]() {
    int rc = sm2::rel_scannxt(c, rowno, tuple);
    have = rc == 0;
    done = rc == 1;
    return rc < 0 ? -1 : 0;
  };
  for (const run_t &r : runs) {
    if (done) {
      break;
    }
    // Move to the run, unless the row in hand is in it or past it.
    if (!have || rowno < r.first) {
      if (sm2::rel_scan_seek(c, r.first) != 0 || next() != 0) {
        return -1;
      }
    }
    while (have && rowno <= r.last) {
      emit(rowno, tuple);
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
  uint64_t n = 0;
  if (tbl_rowno_limit(table, n) != 0) {
    return -1;
  }
  out.rowno_limit = n;
  if (n == 0) {
    out.whole = true;
    return 0;
  }
  const uint64_t want = rows_per_target * static_cast<uint64_t>(target);

  // 1. The pilot: single rownos, for the live fraction and width.
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
  // pilot that found nothing live gives single rownos.
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

  // 4. The runs, k slots by Floyd, and more while the rows fall
  // short and the budget lasts. A reservoir (Algorithm R) keeps a
  // uniform want of every row read.
  uint64_t bytes = 0;
  auto keep = [&](uint64_t rowno, std::string_view t) {
    out.rows_read++;
    bytes += t.size();
    if (out.rows.size() < want) {
      out.rows.push_back({rowno, std::string(t)});
      return;
    }
    std::uniform_int_distribution<uint64_t> d(0, out.rows_read - 1);
    uint64_t j = d(rng);
    if (j < want) {
      out.rows[j] = {rowno, std::string(t)};
    }
  };
  std::unordered_set<uint64_t> chosen;
  std::vector<uint64_t> next = floyd(slots, k, rng);
  while (!next.empty()) {
    std::vector<run_t> runs;
    for (uint64_t s : next) {
      chosen.insert(s);
      uint64_t first = s * b + 1;
      uint64_t last = std::min(n, first + b - 1);
      runs.push_back({first, last});
      out.rownos_covered += last - first + 1;
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
                                       static_cast<double>(out.rownos_covered)
                                 : 1.0 / (out.rownos_covered + 1);
    uint64_t more = static_cast<uint64_t>(
        std::ceil(1.2 * static_cast<double>(want - out.rows_read) / (b * f)));
    next = more_slots(slots, chosen, std::clamp<uint64_t>(more, 1, left), rng);
  }

  // 5. The row count: exact if every rowno was covered, else the
  // live fraction of what was, times N.
  out.whole = out.rownos_covered == n;
  out.live_rows = out.whole ? static_cast<double>(out.rows_read)
                            : static_cast<double>(out.rows_read) *
                                  static_cast<double>(n) /
                                  static_cast<double>(out.rownos_covered);
  out.avg_row_bytes =
      out.rows_read > 0 ? static_cast<int32_t>(bytes / out.rows_read) : 0;
  // Rowno order, for the correlation of each column's order with it.
  std::sort(out.rows.begin(), out.rows.end(),
            [](const sample_row_t &x, const sample_row_t &y) {
              return x.rowno < y.rowno;
            });
  return 0;
}

} // namespace sm3
