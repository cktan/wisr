/*
 * sm0_sort.h -- a sort of records by key (../../SORT.md).
 *
 *   sm0::sort_options_t opt;
 *   opt.dir = dir;
 *   sm0::sort_t *s = nullptr;
 *   sm0::sort_open(opt, s);
 *   sm0::sort_put(s, key, value);     // any order, any number
 *   sm0::sort_done(s);
 *   while (0 == sm0::sort_next(s, key, value)) { ... }
 *   sm0::sort_free(s);
 *
 * A key is bytes, ordered as memcmp orders them, a shorter key before
 * a longer one it starts. The sort knows no types: sm's encoded keys
 * already order so.
 *
 * A sort has one owner, on one thread. It belongs to no session and
 * no transaction. A call returns 0, or -1 with the thread's error set.
 * After an error the owner can only free the sort.
 *
 * Records over the budget go to files in the sort's directory, which
 * the sort deletes. It makes the directory if it must.
 */
#ifndef SM0_SORT_H
#define SM0_SORT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sm0 {

struct sort_t;

/* How a sort runs. Only dir has no default a caller can leave. */
struct sort_options_t {
  /* The directory of its runs, made at the first spill. */
  std::string dir;
  /* The memory it may use for records, in bytes. */
  size_t budget = 256 << 20;
  /*
   * Two equal keys fail with SM_ERR_DUPKEY, as soon as a compare finds
   * them, with the rest not sorted. Without, they come in any order,
   * and the sort of memory is std::sort, which is faster but cannot
   * stop part way, for an interrupt either.
   */
  bool unique = false;
  /* A run's read or write buffer: at least 16 bytes. */
  size_t buffer_bytes = 256 << 10;
  /*
   * The most runs one merge pass reads, at least 2. Fewer are read
   * if the budget holds fewer buffers, down to 6.
   */
  size_t max_runs = 500;
  /*
   * The real fds the sort holds: a run closed for want of one reopens
   * at its next read. At least 1.
   */
  int fd_share = 4;
  /* Under unique, splits before heap sort; 0 by the input's size. */
  size_t max_sort_depth = 0;
  /*
   * The caller decides when memory becomes a run, by sort_spill: for
   * a budget shared by many sorts. budget then only sizes the merge.
   */
  bool manual_spill = false;
  /* A run ends at this many records; 0 for no limit. */
  size_t max_run_records = 0;
  /* A run ends once its records reach this many bytes; 0 for no limit. */
  size_t max_run_size = 0;
  /*
   * sort_put and sort_spill stop for an interrupt (sm_should_stop()),
   * with SM_ERR_INTERRUPTED. Off for a caller that cannot retry one
   * part way, as a bulk load's row that has gone into some sorts and
   * not others: it checks for an interrupt itself, between rows.
   * sort_done and sort_next stop for one either way.
   */
  bool interruptible_input = true;
};

/*
 * Open a sort.
 *
 * Errors: SM_ERR_INVALID (options out of their range).
 */
int sort_open(const sort_options_t &options, sort_t *&out);

/*
 * Add a record, which the sort copies. A duplicate within one run
 * shows here, when the run is written.
 *
 * Errors: SM_ERR_INVALID (the input has ended), SM_ERR_DUPKEY,
 * SM_ERR_DISKFULL, SM_ERR_IO, SM_ERR_INTERRUPTED.
 */
int sort_put(sort_t *sort, std::string_view key, std::string_view value);

/*
 * Write the records in memory as a run; none is no error.
 *
 * Errors: as sort_put's.
 */
int sort_spill(sort_t *sort);

/*
 * End the input, and order the records.
 *
 * Errors: SM_ERR_INVALID (the input has ended), SM_ERR_DUPKEY,
 * SM_ERR_DISKFULL, SM_ERR_IO, SM_ERR_INTERRUPTED.
 */
int sort_done(sort_t *sort);

/*
 * The next record in key order: 0 with its key and value, good until
 * the next call; 1 past the last. Keys in different runs meet here,
 * so a unique sort's duplicate may show only here.
 *
 * Errors: SM_ERR_INVALID (the input has not ended), SM_ERR_DUPKEY,
 * SM_ERR_IO, SM_ERR_INTERRUPTED.
 */
int sort_next(sort_t *sort, std::string_view &key, std::string_view &value);

/* About the compares the sort of memory has made so far: for tests. */
uint64_t sort_compares(const sort_t *sort);

/* Free a sort at any point, deleting its files; nullptr is a no-op. */
void sort_free(sort_t *sort);

} // namespace sm0

#endif /* SM0_SORT_H */
