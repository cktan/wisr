/*
 * oom.h -- marking a thread as inside sm1, where running out of
 * memory is fatal (the top-level DESIGN.md §5).
 *
 * The new-handler sm1::attach installs aborts when an allocation
 * fails on a thread marked inside sm1, before anything unwinds
 * through RocksDB, and throws std::bad_alloc otherwise, for the
 * levels above to report as SM_ERR_NOMEM. Every sm1 entry point,
 * the GC thread and the compaction filter hold an in_sm1_t for as
 * long as they run.
 */
#ifndef WISR_OOM_H
#define WISR_OOM_H

namespace wisr {

/* True while the calling thread runs sm1's code. */
extern thread_local bool t_in_sm1;

/* Marks the calling thread as inside sm1 for the guard's lifetime. */
class in_sm1_t {
public:
  in_sm1_t() : m_saved(t_in_sm1) { t_in_sm1 = true; }
  ~in_sm1_t() { t_in_sm1 = m_saved; }
  in_sm1_t(const in_sm1_t &) = delete;
  in_sm1_t &operator=(const in_sm1_t &) = delete;

private:
  bool m_saved; /* the flag as the guard found it */
};

} // namespace wisr

#endif /* WISR_OOM_H */
