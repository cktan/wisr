/*
 * locktab_bench.cc -- the lock table alone (src/locktab.h), as
 * threads lock and release through it, with no session, wait or
 * database (../NEXT.md §1 item 4).
 *
 * Each thread runs transactions: it takes a transaction's locks
 * one at a time, each under its partition's mutex as lockmgr.cc's
 * lock_try does (find, its own hold, the conflicts, then add or
 * convert), then releases them all as commit does: a list of its
 * holds for each partition, as a session keeps, each walked under
 * its partition's mutex, taken once. A lock that conflicts is refused, not
 * waited for, and counted.
 *
 * The workloads:
 *
 *   distinct   100 row locks of keys at random, X (FOR UPDATE)
 *   hot        10 row locks of 8 hot keys, S (foreign-key parents)
 *   big        thread 0 takes 1,000,000 row locks, then releases
 *              them; the others run distinct beside it (a rehash's
 *              pause shows in their maximum)
 *   mixed      16 table locks of 64 tables, IS or IX, one in 50
 *              S or X (strong and weak locks mixed, no fast path)
 *
 * Each workload runs twice at each thread count. The first reads
 * no clock: lock calls a second, the allocations a lock call
 * makes (operator new, counted), and the share refused. The
 * second reads the clock around every lock call and every
 * release: a lock's latency, the mutex included, and a
 * transaction's release, at p50, p99 and the maximum, the
 * clock's own cost taken off each. A clock read can cost far
 * more than a lock (a system call, as on WSL2), so the two are
 * never measured in one run.
 *
 * Run it on the optimized build (make -j4 without DEBUG=1):
 *
 *   build/bench/locktab_bench [-s seconds] [-t 1,2,4,...]
 *                             [-w distinct,hot,big,mixed]
 */

#include "locktab.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using wisr::lockid_t;
using wisr::locktab_t;

/* The allocations this thread made, counted by operator new. */
static thread_local uint64_t t_allocs = 0;

/*
 * Count each allocation, then make it; out of memory ends the run.
 * Never inlined, so the compiler does not pair its malloc with
 * the library's operator delete.
 */
__attribute__((noinline)) void *operator new(size_t n) {
  t_allocs++;
  void *p = malloc(0 != n ? n : 1);
  if (nullptr == p) {
    abort();
  }
  return p;
}

/* Free what operator new made. */
__attribute__((noinline)) void operator delete(void *p) noexcept { free(p); }

/* Free what operator new made, given its size. */
__attribute__((noinline)) void operator delete(void *p, size_t) noexcept {
  free(p);
}

namespace {

/* Nanoseconds since some fixed point. */
uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* What two clock reads around nothing measure, the median of many. */
uint64_t clock_cost() {
  std::vector<uint64_t> v(1001);
  for (uint64_t &d : v) {
    uint64_t a = now_ns();
    d = now_ns() - a;
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

/* clock_cost(), taken once in main. */
uint64_t g_clock_ns = 0;

/* The time from a to b, less the clock's own cost. */
uint64_t span(uint64_t a, uint64_t b) {
  return b - a > g_clock_ns ? b - a - g_clock_ns : 0;
}

/*
 * A histogram of nanoseconds, log-linear: exact below 64, else 32
 * buckets an octave, about 3% wide.
 */
class hist_t {
public:
  /* Count one value. */
  void add(uint64_t v) {
    m_count[index(v)]++;
    m_max = std::max(m_max, v);
    m_n++;
  }

  /* Add o's counts to ours. */
  void merge(const hist_t &o) {
    for (size_t i = 0; i < m_buckets; i++) {
      m_count[i] += o.m_count[i];
    }
    m_max = std::max(m_max, o.m_max);
    m_n += o.m_n;
  }

  /* The value at fraction q of the counts, as its bucket's floor. */
  uint64_t at(double q) const {
    uint64_t want = uint64_t(q * double(m_n));
    uint64_t seen = 0;
    for (size_t i = 0; i < m_buckets; i++) {
      seen += m_count[i];
      if (seen > want) {
        return floor_of(i);
      }
    }
    return m_max;
  }

  uint64_t max() const { return m_max; }

private:
  static constexpr size_t m_buckets = 64 + 58 * 32;

  /* The bucket of v. */
  static size_t index(uint64_t v) {
    if (v < 64) {
      return v;
    }
    int e = 63 - __builtin_clzll(v);
    return 64 + size_t(e - 6) * 32 + ((v >> (e - 5)) & 31);
  }

  /* The least value in bucket i. */
  static uint64_t floor_of(size_t i) {
    if (i < 64) {
      return i;
    }
    size_t e = (i - 64) / 32 + 6;
    return (uint64_t(32 + (i - 64) % 32)) << (e - 5);
  }

  uint64_t m_count[m_buckets] = {};
  uint64_t m_max = 0;
  uint64_t m_n = 0;
};

/* A workload: its name, and how a transaction's locks are chosen. */
struct workload_t {
  const char *name;
  size_t locks;  /* a transaction's */
  uint64_t keys; /* the ids drawn from, 0 for any */
  bool table;    /* OID locks, IS or IX, one in 50 strong */
  bool big;      /* thread 0 takes big_locks, the others distinct */
};

const workload_t g_workloads[] = {
    {"distinct", 100, 0, false, false},
    {"hot", 10, 8, false, false},
    {"big", 100, 0, false, true},
    {"mixed", 16, 64, true, false},
};

constexpr size_t big_locks = 1000000;

/* The top bit of an OID lock's hi, as lockmgr.cc's type_bit. */
constexpr uint64_t oid_bit = uint64_t(1) << 63;

/* What one thread measured. */
struct result_t {
  hist_t lock;
  hist_t release;
  uint64_t calls = 0;
  uint64_t refused = 0;
  uint64_t allocs = 0;
  uint64_t big_lock_ns = 0; /* thread 0's big transaction, if one */
  uint64_t big_release_ns = 0;
};

/* xorshift64*, a thread's own. */
uint64_t next_rand(uint64_t *s) {
  *s ^= *s >> 12;
  *s ^= *s << 25;
  *s ^= *s >> 27;
  return *s * 0x2545f4914f6cdd1dULL;
}

/* The shared table, its partitions' mutexes its own. */
struct shared_t {
  locktab_t table;
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
};

/* No hold: the end of a partition's list of a thread's holds. */
constexpr uint32_t none = UINT32_MAX;

/*
 * A thread's holds, a list for each partition as a session's lock
 * lists are: the firsts, never moved, as a holder's first points
 * into them, and each partition's chained through next.
 */
struct holds_t {
  std::vector<lockid_t> first;
  std::vector<uint32_t> next;
  uint32_t head[locktab_t::parts];
  size_t n = 0;

  /* Room for cap holds, made before the clock starts. */
  explicit holds_t(size_t cap) : first(cap), next(cap) {
    std::fill(head, head + locktab_t::parts, none);
  }
};

/*
 * Take id in mode for owner, as lock_try does, with its partition's
 * mutex held for the call: false if another holder conflicts.
 */
bool take(shared_t *sh, const lockid_t &id, sm_lockmode_t mode, void *owner,
          holds_t *h) {
  size_t pi = locktab_t::part_of(id);
  locktab_t::part_t &p = sh->table.part(pi);
  // Hold the partition's mutex, as lockmgr.cc does.
  std::lock_guard<std::mutex> guard(p.mu);
  locktab_t::entry_t *e = p.find(id);
  locktab_t::holder_t *mine = nullptr;
  sm_lockmode_t want = mode;
  bool conflict = false;
  if (nullptr != e) {
    conflict = locktab_t::probe(e, owner, mode, &mine, &want);
  }
  if (conflict) {
    return false;
  }
  if (nullptr != mine) {
    mine->mode = want;
    return true;
  }
  // Into the partition's list, as a lock list entry goes.
  uint32_t i = static_cast<uint32_t>(h->n++);
  h->first[i] = id;
  h->next[i] = h->head[pi];
  h->head[pi] = i;
  p.add(e, id, owner, want, &h->first[i]);
  return true;
}

/*
 * Release owner's holds, as commit does: each partition's list
 * walked under its mutex, taken once, and not taken for a list
 * that is empty.
 */
void release(shared_t *sh, void *owner, holds_t *h) {
  for (size_t pi = 0; pi < locktab_t::parts; pi++) {
    if (none == h->head[pi]) {
      continue;
    }
    locktab_t::part_t &p = sh->table.part(pi);
    // Hold the partition's mutex once for all of its holds.
    std::lock_guard<std::mutex> guard(p.mu);
    for (uint32_t i = h->head[pi]; none != i; i = h->next[i]) {
      p.drop(h->first[i], owner, &h->first[i]);
    }
    h->head[pi] = none;
  }
  h->n = 0;
}

/*
 * One thread's transactions until told to stop; timed, it reads the
 * clock around each lock call and each release.
 */
void run_thread(shared_t *sh, const workload_t &w, int me, bool timed,
                result_t *r) {
  bool big = w.big && 0 == me;
  size_t cap = big ? big_locks : w.locks;
  // Room for every hold, made before the clock starts.
  holds_t holds(cap);
  uint64_t seed = 0x9e3779b97f4a7c15ULL * uint64_t(me + 1);
  int owner = 0;
  sh->ready.fetch_add(1);
  while (!sh->go.load()) {
    std::this_thread::yield();
  }
  uint64_t allocs0 = t_allocs;
  bool did_big = false;
  while (!sh->stop.load(std::memory_order_relaxed) || (big && !did_big)) {
    // The big transaction is timed in both runs: three reads in a
    // million locks.
    uint64_t t0 = big ? now_ns() : 0;
    for (size_t i = 0; i < cap; i++) {
      uint64_t x = next_rand(&seed);
      lockid_t id;
      sm_lockmode_t mode;
      if (w.table) {
        id = {oid_bit, x % w.keys};
        bool strong = 0 == (x >> 32) % 50;
        mode = strong ? ((x >> 40) & 1 ? SM_LOCKMODE_EX : SM_LOCKMODE_SH)
                      : ((x >> 40) & 1 ? SM_LOCKMODE_IX : SM_LOCKMODE_IS);
      } else {
        id = {1, 0 != w.keys ? x % w.keys : x};
        mode = 0 != w.keys ? SM_LOCKMODE_SH : SM_LOCKMODE_EX;
      }
      bool ok;
      if (timed) {
        uint64_t a = now_ns();
        ok = take(sh, id, mode, &owner, &holds);
        r->lock.add(span(a, now_ns()));
      } else {
        ok = take(sh, id, mode, &owner, &holds);
      }
      r->calls++;
      if (!ok) {
        r->refused++;
      }
    }
    uint64_t t1 = timed || big ? now_ns() : 0;
    release(sh, &owner, &holds);
    uint64_t t2 = timed || big ? now_ns() : 0;
    if (timed) {
      r->release.add(span(t1, t2));
    }
    if (big) {
      r->big_lock_ns = t1 - t0;
      r->big_release_ns = t2 - t1;
      did_big = true;
    }
  }
  r->allocs = t_allocs - allocs0;
}

/* Run w at nthreads for secs, timed or not, into *all; its seconds. */
double run_once(const workload_t &w, int nthreads, double secs, bool timed,
                result_t *all) {
  shared_t sh;
  std::vector<result_t> results(nthreads);
  std::vector<std::thread> threads;
  for (int i = 0; i < nthreads; i++) {
    // A thread a client, each its own transactions.
    threads.emplace_back(run_thread, &sh, std::cref(w), i, timed, &results[i]);
  }
  while (sh.ready.load() < nthreads) {
    std::this_thread::yield();
  }
  uint64_t t0 = now_ns();
  sh.go.store(true);
  std::this_thread::sleep_for(std::chrono::duration<double>(secs));
  sh.stop.store(true);
  for (std::thread &t : threads) {
    t.join();
  }
  uint64_t t1 = now_ns();
  for (const result_t &r : results) {
    all->lock.merge(r.lock);
    all->release.merge(r.release);
    all->calls += r.calls;
    all->refused += r.refused;
    all->allocs += r.allocs;
  }
  all->big_lock_ns = results[0].big_lock_ns;
  all->big_release_ns = results[0].big_release_ns;
  return double(t1 - t0) / 1e9;
}

/* Run w at nthreads, untimed then timed, and print its line. */
void run(const workload_t &w, int nthreads, double secs) {
  result_t fast, slow;
  double elapsed = run_once(w, nthreads, secs, false, &fast);
  run_once(w, nthreads, secs, true, &slow);
  double calls = double(fast.calls ? fast.calls : 1);
  printf("%-8s %3d %10.0f %7llu %7llu %9llu %8.1f %8.1f %9.1f %6.2f %6.2f%%",
         w.name, nthreads, double(fast.calls) / elapsed,
         (unsigned long long)slow.lock.at(0.5),
         (unsigned long long)slow.lock.at(0.99),
         (unsigned long long)slow.lock.max(), slow.release.at(0.5) / 1e3,
         slow.release.at(0.99) / 1e3, slow.release.max() / 1e3,
         double(fast.allocs) / calls, 100.0 * double(fast.refused) / calls);
  if (w.big) {
    printf("  big: lock %.1f ms, release %.1f ms", fast.big_lock_ns / 1e6,
           fast.big_release_ns / 1e6);
  }
  printf("\n");
  fflush(stdout);
}

/* The comma-separated list s, split. */
std::vector<std::string> split(const char *s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char *p = s;; p++) {
    if (',' == *p || '\0' == *p) {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
      if ('\0' == *p) {
        break;
      }
    } else {
      cur += *p;
    }
  }
  return out;
}

} // namespace

/* Parse the options, then run each workload at each thread count. */
int main(int argc, char **argv) {
  double secs = 1;
  std::vector<std::string> threads = {"1", "2", "4", "8", "16", "32"};
  std::vector<std::string> names;
  for (int i = 1; i < argc; i++) {
    if (0 == strcmp(argv[i], "-s") && i + 1 < argc) {
      secs = atof(argv[++i]);
    } else if (0 == strcmp(argv[i], "-t") && i + 1 < argc) {
      threads = split(argv[++i]);
    } else if (0 == strcmp(argv[i], "-w") && i + 1 < argc) {
      names = split(argv[++i]);
    } else {
      fprintf(stderr,
              "usage: %s [-s seconds] [-t 1,2,4,...] "
              "[-w distinct,hot,big,mixed]\n",
              argv[0]);
      return 2;
    }
  }
  g_clock_ns = clock_cost();
  printf("clock read: %llu ns, taken off each latency\n",
         (unsigned long long)g_clock_ns);
  printf("%-8s %3s %10s %7s %7s %9s %8s %8s %9s %6s %7s\n", "workload", "thr",
         "locks/s", "p50ns", "p99ns", "maxns", "rel50us", "rel99us", "relmaxus",
         "alloc", "refused");
  for (const workload_t &w : g_workloads) {
    if (!names.empty() &&
        names.end() == std::find(names.begin(), names.end(), w.name)) {
      continue;
    }
    for (const std::string &t : threads) {
      run(w, atoi(t.c_str()), secs);
    }
  }
  return 0;
}
