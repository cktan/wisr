/*
 * sm_stats.h -- a table's statistics, as sm3 records and keeps them.
 *
 * sm3.h names these types stats_t, stats_index_t and stat_t, and
 * sm.h hands them out as they are: sm3 keeps each table's
 * statistics in memory once loaded (sm3/SM3_DESIGN.md section 6),
 * and every client reads that one copy, never a translation of
 * it (OBJECTIVE.md). It belongs to no level, so it lives at the
 * root, as sm_options.h does.
 *
 * A value -- a most common value or a histogram bound -- is its
 * bytes as a datum of the column's or expression's type, a
 * varlena for a variable-width one.
 */
#ifndef SM_STATS_H
#define SM_STATS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/* The version of the parts below; 1 for now. */
constexpr int32_t sm_stats_format = 1;

/*
 * The statistics of one column, or of one expression of an index:
 * PostgreSQL's six.
 */
struct sm_stat_t {
    std::optional<uint64_t>  index;       /* nullopt: a column's */
    int32_t                  position;    /* the column's attnum, 0-based,
                                             or the expression's place
                                             among the index's keys */
    float                    null_frac;
    int32_t                  width;       /* average bytes of a value */
    float                    distinct;    /* a count, or, below 0, minus
                                             a fraction of the rows */
    std::optional<float>     correlation;
    std::vector<std::string> mcv;         /* most common values */
    std::vector<float>       mcv_freq;    /* each one's frequency */
    std::vector<std::string> bounds;      /* histogram bounds */
};

/* An index's sizes and entries as the analyze found them. */
struct sm_stats_index_t {
    uint64_t oid;
    uint64_t disk;
    uint64_t mem;
    double   entries;
    float    belongs;  /* the fraction of rows in it; 1 if not partial */
};

/* A table's statistics, as its last analyze recorded them. */
struct sm_stats_t {
    int32_t                        format = sm_stats_format;
    int32_t                        target;        /* the statistics target */
    int32_t                        run_length;    /* B, rownos a run */
    uint64_t                       rows_sampled;
    uint64_t                       rowno_limit;   /* N when analyzed */
    double                         live_rows;
    uint64_t                       heap_disk;
    uint64_t                       heap_mem;
    int32_t                        avg_row_bytes;
    std::vector<sm_stats_index_t>  indexes;
    std::vector<sm_stat_t>         stats;
};

#endif /* SM_STATS_H */
