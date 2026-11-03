/*
 * estimate.cc -- the estimators an analyze runs over its sample
 * (SM3_DESIGN.md §6.5): compute_scalar_stats and analyze_mcv_list,
 * ported from PostgreSQL 18's analyze.c.
 *
 * What changes in the port is how values compare: by their key
 * under the column's collation, as sm2 encodes it, with memcmp,
 * which orders them as the type does and calls them equal where
 * PostgreSQL does, a non-deterministic collation included. Sorting
 * by key, then by place in the sample, puts equal values together,
 * so PostgreSQL's tupnoLink, which saved comparisons its sort had
 * made, is not needed. Everything else is PostgreSQL's: the
 * Haas-Stokes Duj1 distinct estimator and its 10% rule, which
 * values are common enough to list, the histogram over the rest,
 * the correlation, and WIDTH_THRESHOLD.
 */

#include "analyze.h"

#include <algorithm>
#include <cmath>

namespace {

/*
 * A value wider than this, in bytes, is counted as distinct and kept
 * out of the lists: PostgreSQL's WIDTH_THRESHOLD.
 */
constexpr size_t width_threshold = 1024;

/* A sortable value: its key, and its place among them in sample order. */
struct item_t {
  std::string key;
  int tupno;
};

/* A value in the running for the common values: how many, and where. */
struct track_t {
  int count;
  int first; /* its first place among the sorted items */
};

/*
 * How many of the most common values, counts, most common first, are
 * worth listing: those significantly more common than the values not
 * listed would be (analyze.c's analyze_mcv_list).
 */
int analyze_mcv_list(const std::vector<int> &counts, int num_mcv,
                     double stadistinct, double stanullfrac, int samplerows,
                     double totalrows) {
  // The whole table sampled: keep the whole list.
  if (samplerows == totalrows || totalrows <= 1.0) {
    return num_mcv;
  }
  double ndistinct_table = stadistinct;
  if (ndistinct_table < 0) {
    ndistinct_table = -ndistinct_table * totalrows;
  }
  // The count of every listed value but the least common.
  double sumcount = 0.0;
  for (int i = 0; i < num_mcv - 1; i++) {
    sumcount += counts[i];
  }
  while (num_mcv > 0) {
    // The selectivity the least common value would have unlisted.
    double selec = 1.0 - sumcount / samplerows - stanullfrac;
    selec = std::clamp(selec, 0.0, 1.0);
    double otherdistinct = ndistinct_table - (num_mcv - 1);
    if (otherdistinct > 1) {
      selec /= otherdistinct;
    }
    // Kept if its count is over that by two standard deviations of
    // a hypergeometric sample, plus a continuity correction.
    double N = totalrows;
    double n = samplerows;
    double K = N * counts[num_mcv - 1] / n;
    double variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
    double stddev = std::sqrt(variance);
    if (counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5) {
      break;
    }
    num_mcv--;
    if (num_mcv == 0) {
      break;
    }
    sumcount -= counts[num_mcv - 1];
  }
  return num_mcv;
}

} // namespace

namespace sm3 {

/* PostgreSQL's compute_scalar_stats, over keys; see analyze.h. */
int compute_scalar_stats(const sm2::key_column_t &col,
                         std::span<const sm2::datum_t> values,
                         std::span<const bool> isnull, int32_t target,
                         double totalrows, bool &valid, stat_t &out) {
  valid = false;
  const sm2::type_t *type = col.type;
  const int samplerows = static_cast<int>(values.size());
  const bool is_varwidth = !type->byval && type->len < 0;
  int null_cnt = 0, nonnull_cnt = 0, toowide_cnt = 0;
  double total_width = 0;
  std::vector<item_t> items;
  std::vector<int> place; /* tupno -> its index in values */
  items.reserve(values.size());

  // The sortable values, each with its key.
  for (int i = 0; i < samplerows; i++) {
    if (isnull[i]) {
      null_cnt++;
      continue;
    }
    nonnull_cnt++;
    if (is_varwidth) {
      std::string_view bytes;
      if (sm2::datum_bytes(type, col.typmod, values[i], bytes) != 0) {
        return -1;
      }
      // A datum out of line is as wide as its pointer says, and
      // always too wide to sort: it is counted, and never fetched.
      sm2::toast_ptr_t p;
      if (sm2::parse_toast_ptr(bytes, p)) {
        total_width += static_cast<double>(p.len) + 4;
        toowide_cnt++;
        continue;
      }
      total_width += static_cast<double>(bytes.size());
      if (bytes.size() > width_threshold) {
        toowide_cnt++;
        continue;
      }
    }
    std::string key;
    bool notnull = false;
    if (sm2::encode_key(std::span(&col, 1), values.subspan(i, 1),
                        std::span(&notnull, 1), key) != 0) {
      return -1;
    }
    items.push_back({std::move(key), static_cast<int>(place.size())});
    place.push_back(i);
  }
  const int values_cnt = static_cast<int>(items.size());
  // A value as the catalog keeps it: its bytes.
  auto bytes_of = [&](int tupno, std::string &out_bytes) {
    std::string_view v;
    if (sm2::datum_bytes(type, col.typmod, values[place[tupno]], v) != 0) {
      return -1;
    }
    out_bytes.assign(v);
    return 0;
  };

  out.null_frac = 0;
  out.width = 0;
  out.distinct = 0;
  out.has_correlation = false;
  out.correlation = 0;
  out.mcv.clear();
  out.mcv_freq.clear();
  out.bounds.clear();

  if (values_cnt > 0) {
    // Equal keys together, each group in sample order.
    std::sort(items.begin(), items.end(), [](const item_t &a, const item_t &b) {
      int c = a.key.compare(b.key);
      return c != 0 ? c < 0 : a.tupno < b.tupno;
    });
    // The common values, and the correlation of order with the sample's.
    int num_mcv = target, num_bins = target;
    std::vector<track_t> track;
    double corr_xysum = 0;
    int ndistinct = 0, nmultiple = 0, dups_cnt = 0;
    for (int i = 0; i < values_cnt; i++) {
      corr_xysum += static_cast<double>(i) * items[i].tupno;
      dups_cnt++;
      // The last of its group: the next is another value, or none.
      if (i + 1 < values_cnt && items[i + 1].key == items[i].key) {
        continue;
      }
      ndistinct++;
      if (dups_cnt > 1) {
        nmultiple++;
        int track_cnt = static_cast<int>(track.size());
        if (track_cnt < num_mcv || dups_cnt > track[track_cnt - 1].count) {
          // Its place in the list, most common first, bubbling down.
          if (track_cnt < num_mcv) {
            track.push_back({0, 0});
            track_cnt++;
          }
          int j;
          for (j = track_cnt - 1; j > 0; j--) {
            if (dups_cnt <= track[j - 1].count) {
              break;
            }
            track[j] = track[j - 1];
          }
          track[j] = {dups_cnt, i + 1 - dups_cnt};
        }
      }
      dups_cnt = 0;
    }

    valid = true;
    out.null_frac = static_cast<float>(static_cast<double>(null_cnt) /
                                       static_cast<double>(samplerows));
    out.width = is_varwidth ? static_cast<int32_t>(total_width / nonnull_cnt)
                            : type->len;
    double stadistinct;
    if (nmultiple == 0) {
      // No value repeated: a unique column, but for its nulls.
      stadistinct = -1.0 * (1.0 - out.null_frac);
    } else if (toowide_cnt == 0 && nmultiple == ndistinct) {
      // Every value repeated: the column has just these.
      stadistinct = ndistinct;
    } else {
      // Haas and Stokes' Duj1, over the non-null values: n*d / (n -
      // f1 + f1*n/N); a too-wide value is taken as distinct.
      int f1 = ndistinct - nmultiple + toowide_cnt;
      int d = f1 + nmultiple;
      double n = samplerows - null_cnt;
      double N = totalrows * (1.0 - out.null_frac);
      stadistinct = N > 0 ? (n * d) / ((n - f1) + f1 * n / N) : 0;
      stadistinct = std::min(std::max(stadistinct, static_cast<double>(d)), N);
      stadistinct = std::floor(stadistinct + 0.5);
    }
    // Over 10% of the rows, it scales with them.
    if (stadistinct > 0.1 * totalrows) {
      stadistinct = -(stadistinct / totalrows);
    }
    out.distinct = static_cast<float>(stadistinct);

    // How many common values to keep: all, if they are every value
    // and fit; else those significantly more common than the rest.
    int track_cnt = static_cast<int>(track.size());
    if (track_cnt == ndistinct && toowide_cnt == 0 && stadistinct > 0 &&
        track_cnt <= num_mcv) {
      num_mcv = track_cnt;
    } else {
      num_mcv = std::min(num_mcv, track_cnt);
      if (num_mcv > 0) {
        std::vector<int> counts;
        for (int i = 0; i < num_mcv; i++) {
          counts.push_back(track[i].count);
        }
        num_mcv = analyze_mcv_list(counts, num_mcv, stadistinct, out.null_frac,
                                   samplerows, totalrows);
      }
    }
    for (int i = 0; i < num_mcv; i++) {
      std::string v;
      if (bytes_of(items[track[i].first].tupno, v) != 0) {
        return -1;
      }
      out.mcv.push_back(std::move(v));
      out.mcv_freq.push_back(
          static_cast<float>(static_cast<double>(track[i].count) / samplerows));
    }

    // A histogram over the values not listed, if two or more remain.
    int num_hist = ndistinct - num_mcv;
    if (num_hist > num_bins) {
      num_hist = num_bins + 1;
    }
    if (num_hist >= 2) {
      // The listed values' groups, in sorted order, taken out.
      std::vector<track_t> listed(track.begin(), track.begin() + num_mcv);
      std::sort(
          listed.begin(), listed.end(),
          [](const track_t &a, const track_t &b) { return a.first < b.first; });
      std::vector<int> rest; /* tupnos of the values not listed */
      size_t j = 0;
      for (int src = 0; src < values_cnt;) {
        if (j < listed.size() && src >= listed[j].first) {
          src = listed[j].first + listed[j].count;
          j++;
          continue;
        }
        rest.push_back(items[src].tupno);
        src++;
      }
      int nvals = static_cast<int>(rest.size());
      // The first and last, and evenly spaced ones between.
      int delta = (nvals - 1) / (num_hist - 1);
      int deltafrac = (nvals - 1) % (num_hist - 1);
      int pos = 0, posfrac = 0;
      for (int i = 0; i < num_hist; i++) {
        std::string v;
        if (bytes_of(rest[pos], v) != 0) {
          return -1;
        }
        out.bounds.push_back(std::move(v));
        pos += delta;
        posfrac += deltafrac;
        if (posfrac >= num_hist - 1) {
          pos++;
          posfrac -= num_hist - 1;
        }
      }
    }

    // The correlation of the values' order with the sample's, whose x
    // and y are both 0..values_cnt-1.
    if (values_cnt > 1) {
      double n = values_cnt;
      double xsum = (n - 1) * n / 2.0;
      double x2sum = (n - 1) * n * (2 * n - 1) / 6.0;
      out.has_correlation = true;
      out.correlation = static_cast<float>((n * corr_xysum - xsum * xsum) /
                                           (n * x2sum - xsum * xsum));
    }
  } else if (nonnull_cnt > 0) {
    // Every non-null value too wide: a unique column.
    valid = true;
    out.null_frac = static_cast<float>(static_cast<double>(null_cnt) /
                                       static_cast<double>(samplerows));
    out.width = is_varwidth ? static_cast<int32_t>(total_width / nonnull_cnt)
                            : type->len;
    out.distinct = -1.0f * (1.0f - out.null_frac);
  } else if (null_cnt > 0) {
    // Only nulls: a column of nulls, its width and distinct unknown.
    valid = true;
    out.null_frac = 1.0f;
    out.width = is_varwidth ? 0 : type->len;
    out.distinct = 0.0f;
  }
  return 0;
}

} // namespace sm3
