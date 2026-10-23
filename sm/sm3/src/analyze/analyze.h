/*
 * analyze.h -- what tbl_analyze is built from (SM3_DESIGN.md §6.4,
 * §6.5): the sample of a table's rows, and the estimators run over
 * it. Internal to sm3.
 */
#ifndef SM3_ANALYZE_H
#define SM3_ANALYZE_H

#include "sm3.h"

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace sm3 {

/* The rows a sample is after: 300 a unit of statistics target. */
constexpr uint64_t rows_per_target = 300;

/*
 * The most _ids a sample covers, as a multiple of the rows it is
 * after: what bounds its reads, whatever the table's size.
 */
constexpr uint64_t budget_factor = 10;

/*
 * One sampled row: its _id and its tuple, as sm2 encodes it; in a
 * table with a primary key, its key too, the sample's order.
 */
struct sample_row_t {
  uint64_t id;
  std::string tuple;
  std::string key;
};

/* A table's sample, and what reading it found. */
struct sample_t {
  std::vector<sample_row_t> rows; /* kept, uniform, in key order */
  uint64_t rowno_limit = 0;       /* N, the last _id handed out */
  int32_t run_length = 0;         /* B, _ids a run; rows a slot, keyed */
  uint64_t runs = 0;              /* runs read; slots, keyed */
  uint64_t ids_covered = 0;       /* _ids the runs spanned */
  uint64_t bytes_covered = 0;     /* bytes the slots spanned, keyed */
  uint64_t rows_read = 0;         /* live rows found in them */
  double live_rows = 0;           /* the estimate of the table's */
  int32_t avg_row_bytes = 0;      /* over the rows read */
  bool whole = false;             /* every _id 1..N was covered */
};

/*
 * Sample the table, open in a transaction, for target: up to
 * rows_per_target x target rows, read through forward cursors at
 * the transaction's snapshot, in runs of B _ids whose slots are
 * chosen by Floyd's algorithm, never by walking the table. It
 * reads at most budget_factor x the rows it is after, in _ids;
 * a sparse table may then give fewer rows. rng makes the draws.
 *
 * A table with a primary key has its slots in key order instead
 * (../../ROWKEY.md §2.4): each a page's share of its bytes, the
 * rows between the keys at its two ends (tbl_keys_at), chosen by
 * Floyd's algorithm as runs are, after a pilot of a few that
 * measures the rows a slot. Its rows are estimated as the rows
 * read per byte the slots read span, by sm1's estimate, times the
 * bytes of all of them: a slot spans whole data blocks, as the
 * estimate resolves its ends, not a page's share exactly.
 */
int sample_table(table_t *table, int32_t target, std::mt19937_64 &rng,
                 sample_t &out);

/*
 * k distinct numbers drawn uniformly from 0..n-1, sorted: Floyd's
 * algorithm, which draws k numbers and looks at no others.
 */
std::vector<uint64_t> floyd(uint64_t n, uint64_t k, std::mt19937_64 &rng);

/*
 * The statistics of one column or expression, from its values over a
 * sample, in the sample's order -- _id order, or key order in a
 * table with a primary key -- with their null
 * flags (SM3_DESIGN.md §6.5): PostgreSQL's compute_scalar_stats,
 * comparing values by their key under col. totalrows is the table's
 * estimated live rows. valid is false if the sample had no rows, and
 * out then says nothing; out's index and position are the caller's
 * to set.
 */
int compute_scalar_stats(const sm2::key_column_t &col,
                         std::span<const sm2::datum_t> values,
                         std::span<const bool> isnull, int32_t target,
                         double totalrows, bool &valid, stat_t &out);

/*
 * tbl_analyze, with the sample's draws made by rng: sample the table,
 * estimate every live column's statistics, and record them with its
 * sizes and its indexes', in the table's transaction. With wait false
 * it is tbl_tryanalyze: 1, having sampled nothing, if another analyze
 * of the table holds its statistics' lock.
 */
int analyze_table(table_t *table, int32_t target, std::mt19937_64 &rng,
                  bool wait = true);

} // namespace sm3

#endif /* SM3_ANALYZE_H */
