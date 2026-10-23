/*
 * analyze.cc -- tbl_analyze (SM3_DESIGN.md §6.4 to §6.7): the
 * sample, each column's estimates over it, each partial or
 * expression index's from the host's evaluator, and the sizes,
 * recorded as the table's statistics.
 */

#include "analyze.h"
#include "catalog.h"
#include "internal.h"

#include <memory>

namespace {

/*
 * Ask the host's evaluator about a sampled row, as a write does
 * (table.cc): noexcept, so one that throws ends the process rather
 * than unwinding through the analyze.
 */
int ask_evaluator(const sm3::index_eval_fn &eval, const sm3::table_def_t &def,
                  const sm3::index_def_t &x, std::span<const sm2::datum_t> row,
                  std::span<const bool> row_null, bool &belongs,
                  std::vector<std::string> &out,
                  std::span<bool> out_null) noexcept {
  return eval(def, x, row, row_null, belongs, out, out_null);
}

/*
 * The statistics of a partial or an expression index x over the
 * sample s (SM3_DESIGN.md §6.5): the fraction of the rows that
 * belong to it, into ix, and each expression's statistics over
 * those rows, appended to stats.
 */
int analyze_index(sm3::table_t *table, const sm3::table_def_t &def,
                  const sm3::index_def_t &x, const sm3::sample_t &s,
                  int32_t target, sm3::stats_index_t &ix,
                  std::vector<sm3::stat_t> &stats) {
  const sm3::index_eval_fn &eval = sm3::tbl_session(table)->cat->eval;
  if (!eval) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "analyze: index " + x.name +
                           " is evaluated by the host, and the catalog has "
                           "no evaluator");
  }
  std::vector<size_t> exprs; /* the key places that are expressions */
  for (size_t p = 0; p < x.key_attnums.size(); p++) {
    if (x.key_attnums[p] < 0) {
      exprs.push_back(p);
    }
  }
  // Each expression's values over the rows that belong.
  std::vector<std::vector<std::string>> vals(exprs.size());
  std::vector<std::vector<char>> nulls(exprs.size());
  size_t ncol = def.columns.size(), in = 0;
  std::vector<sm2::datum_t> row(ncol);
  std::unique_ptr<bool[]> row_null(new bool[ncol]);
  std::unique_ptr<bool[]> out_null(new bool[exprs.size() + 1]);
  for (const sm3::sample_row_t &r : s.rows) {
    // The row's every column, as the evaluator is shown one.
    for (size_t c = 0; c < ncol; c++) {
      if (sm2::tup_decode(def.desc, r.tuple, c, row[c], row_null[c]) != 0) {
        return -1;
      }
    }
    // What the evaluator reads of it is fetched, if out of line.
    std::vector<std::string> own;
    if (sm3::tbl_fetch_cols(table, x.reads, row,
                            std::span<const bool>(row_null.get(), ncol),
                            own) != 0) {
      return -1;
    }
    bool belongs = true;
    std::vector<std::string> out(exprs.size());
    std::fill(out_null.get(), out_null.get() + exprs.size(), false);
    if (ask_evaluator(eval, def, x, row, std::span(row_null.get(), ncol),
                      belongs, out,
                      std::span(out_null.get(), exprs.size())) != 0) {
      return -1;
    }
    if (!belongs) {
      continue;
    }
    in++;
    for (size_t e = 0; e < exprs.size(); e++) {
      vals[e].push_back(std::move(out[e]));
      nulls[e].push_back(out_null[e]);
    }
  }
  ix.belongs =
      s.rows.empty()
          ? 1.0f
          : static_cast<float>(static_cast<double>(in) / s.rows.size());
  ix.entries = s.live_rows * ix.belongs;
  for (size_t e = 0; e < exprs.size(); e++) {
    const sm2::key_column_t &key = x.key[exprs[e]];
    std::vector<sm2::datum_t> d(in);
    std::unique_ptr<bool[]> d_null(new bool[in]);
    for (size_t i = 0; i < in; i++) {
      d_null[i] = nulls[e][i] != 0;
      d[i] = d_null[i] ? 0 : sm3::datum_of_bytes(key.type, vals[e][i]);
    }
    sm3::stat_t stat;
    bool valid = false;
    if (sm3::compute_scalar_stats(key, d, std::span(d_null.get(), in), target,
                                  ix.entries, valid, stat) != 0) {
      return -1;
    }
    if (valid) {
      stat.index = x.oid;
      stat.position = static_cast<int32_t>(exprs[e]);
      stats.push_back(std::move(stat));
    }
  }
  return 0;
}

} // namespace

namespace sm3 {

/* Analyze a table with rng's draws; see analyze.h. */
int analyze_table(table_t *table, int32_t target, std::mt19937_64 &rng,
                  bool wait) {
  if (target < 1) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "analyze: a statistics target below 1");
  }
  const table_def_t &def = tbl_def(table);
  session_t *session = tbl_session(table);
  stats_t st;
  if (tbl_size(table, st.heap_disk, st.heap_mem) != 0) {
    return -1;
  }
  // A second analyze of the table waits here, before it samples, or,
  // not waiting, returns 1.
  int held = wait ? cat_lock_stats(session, def.oid)
                  : cat_trylock_stats(session, def.oid);
  if (held != 0) {
    return held;
  }
  sample_t s;
  if (sample_table(table, target, rng, s) != 0) {
    return -1;
  }
  // A heap of no bytes and no row read is a new store, a new or
  // truncated table's: the host plans it as PostgreSQL plans one
  // truncated (§6.7). The bytes in sorted files are RocksDB's
  // estimate, so the sample decides.
  if (st.heap_disk == 0 && st.heap_mem == 0 && s.rows_read == 0) {
    return 0;
  }
  st.target = target;
  st.run_length = s.run_length;
  st.rows_sampled = s.rows.size();
  st.rowno_limit = s.rowno_limit;
  st.live_rows = s.live_rows;
  st.avg_row_bytes = s.avg_row_bytes;
  // Each index's sizes; its entries are the rows, one each.
  for (const index_def_t &x : def.indexes) {
    stats_index_t ix{x.oid, 0, 0, s.live_rows, 1.0f};
    if (tbl_idx_size(table, x.oid, ix.disk, ix.mem) != 0) {
      return -1;
    }
    st.indexes.push_back(ix);
  }
  // Each live column's values over the sample, in its order; the
  // datums point into the sample's tuples.
  size_t n = s.rows.size();
  std::vector<sm2::datum_t> values(n);
  std::unique_ptr<bool[]> isnull(new bool[n]);
  for (size_t c = 0; c < def.columns.size(); c++) {
    const column_def_t &col = def.columns[c];
    if (col.dropped) {
      continue;
    }
    for (size_t i = 0; i < n; i++) {
      if (sm2::tup_decode(def.desc, s.rows[i].tuple, c, values[i], isnull[i]) !=
          0) {
        return -1;
      }
    }
    sm2::key_column_t key{col.type, col.typmod, col.collation, false, false};
    stat_t stat;
    bool valid = false;
    if (compute_scalar_stats(key, values, std::span(isnull.get(), n), target,
                             s.live_rows, valid, stat) != 0) {
      return -1;
    }
    if (valid) {
      stat.position = static_cast<int32_t>(c);
      st.stats.push_back(std::move(stat));
    }
  }
  // A partial or an expression index: its share of the rows, and
  // its expressions' statistics, from the host's evaluator.
  for (size_t i = 0; i < def.indexes.size(); i++) {
    if (!def.indexes[i].eval.empty() &&
        analyze_index(table, def, def.indexes[i], s, target, st.indexes[i],
                      st.stats) != 0) {
      return -1;
    }
  }
  return cat_put_stats(session, def.oid, st);
}

namespace {

/* A fresh seed each time: an analyze is a sample, not a replay. */
std::mt19937_64 fresh_rng() {
  std::random_device rd;
  return std::mt19937_64((static_cast<uint64_t>(rd()) << 32) | rd());
}

} // namespace

/* Analyze a table; see sm3.h. */
int tbl_analyze(table_t *table, int32_t target) {
  std::mt19937_64 rng = fresh_rng();
  return analyze_table(table, target, rng);
}

/* As tbl_analyze, if no other analyze of the table holds it; see sm3.h. */
int tbl_tryanalyze(table_t *table, int32_t target) {
  std::mt19937_64 rng = fresh_rng();
  return analyze_table(table, target, rng, false);
}

} // namespace sm3
