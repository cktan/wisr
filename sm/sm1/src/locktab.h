/*
 * locktab.h -- the lock table (../LOCKMGR.md §3.2): a lock's id ->
 * the owners holding it, each in a mode of its own.
 *
 * It knows ids, Gray's modes and opaque owners; nothing of
 * sessions, waits, the fast path or RocksDB, which stay in
 * lockmgr.cc, its one caller. So it is measured alone
 * (bench/locktab_bench.cc, ../NEXT.md §1 item 4).
 *
 * It is in 16 partitions, as PostgreSQL's lock table is, an id's
 * by its hash, each with a mutex of its own, so lockers of ids in
 * different partitions do not meet. The caller holds a
 * partition's mutex across every call to it, and a pointer one
 * returns holds until the partition is next changed; what reads
 * the whole table holds every partition's, in their order.
 */
#ifndef WISR_LOCKTAB_H
#define WISR_LOCKTAB_H

#include "sm_lock.h"

#include <stddef.h>
#include <stdint.h>

#include <mutex>

namespace wisr {

/*
 * A lock's id in the lock table: hi is the storeid, its top bit flipped
 * for an OID lock so the two types never meet; lo is the oid of an
 * OID lock, exact, or a hash of a row lock's key. Two rows of a
 * store whose keys hash alike are one lock: a false conflict,
 * never a missed one.
 */
struct lockid_t {
  uint64_t hi;
  uint64_t lo;
  bool operator==(const lockid_t &o) const { return hi == o.hi && lo == o.lo; }
};

/* MurmurHash3's 64-bit finalizer: each bit of h reaches every bit out. */
inline uint64_t lock_fmix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

/*
 * The lock table's hash of an id: the two words combined, then
 * finalized, so every bit of both reaches every bit of the hash
 * and the fast path's count of a table, its hash modulo 1,024,
 * is not its oid modulo 1,024. Never persisted. Not noexcept,
 * so the map keeps each node's hash: without it, a lookup
 * hashes every node of a bucket it walks, 7% more instructions
 * a lock, measured.
 */
struct lockid_hash_t {
  size_t operator()(const lockid_t &k) const {
    return lock_fmix64(k.lo ^ (k.hi * 0x9e3779b97f4a7c15ULL));
  }
};

/* Gray's compatibility of two modes held by different owners. */
bool lock_compatible(sm_lockmode_t a, sm_lockmode_t b);

/*
 * The weakest mode covering both a and b, which a holder of a that
 * asks for b converts to: IS < IX, S < SIX < X, and IX and S meet
 * at SIX.
 */
sm_lockmode_t lock_cover(sm_lockmode_t a, sm_lockmode_t b);

/* The lock table; see the top of the file. */
class locktab_t {
public:
  /* The partitions, and so the mutexes, a table has. */
  static constexpr size_t parts = 16;

  /*
   * An owner's hold on a lock, in mode; first is the caller's, a
   * word for what made the hold, which the table only compares.
   */
  struct holder_t {
    void *owner;
    sm_lockmode_t mode;
    void *first;
  };

  /*
   * One id's entry: its holders, never none, in the order they came,
   * the first three inline and the rest in an array of its own.
   * Only the table reads its fields.
   */
  struct entry_t {
    static constexpr uint32_t inline_holders = 3;
    lockid_t id;
    size_t hash;    /* lockid_hash_t's of id */
    entry_t *next;  /* in its bucket, or the free list */
    uint32_t n;     /* holders */
    uint32_t cap;   /* room in more */
    holder_t *more; /* holders past the first three */
    holder_t first3[inline_holders];

    /* Holder i, the oldest 0. */
    holder_t &at(uint32_t i) {
      return i < inline_holders ? first3[i] : more[i - inline_holders];
    }
    const holder_t &at(uint32_t i) const {
      return i < inline_holders ? first3[i] : more[i - inline_holders];
    }
  };

  /*
   * One partition: its mutex, and the entries of its ids, a cache
   * line of its own so lockers of two partitions share none.
   *
   * Its own hash table, as PostgreSQL keeps its own: a power-of-two
   * bucket array, indexed by the low bits of an id's hash (the top
   * ones choose the partition), no division. It doubles past one
   * entry a bucket and goes to a quarter under one in eight, into a
   * new array that each add and drop moves a few buckets to, so no
   * call rehashes the partition. A dropped entry goes on a free list, up
   * to free_max, so a steady workload allocates no entry under the
   * mutex, and a transaction of a million locks leaves no more than
   * that behind.
   */
  class alignas(64) part_t {
  public:
    /* Held across every call below. */
    std::mutex mu;

    part_t() = default;
    ~part_t();
    part_t(const part_t &) = delete;
    part_t &operator=(const part_t &) = delete;

    /* The entry of id, or nullptr if no owner holds it. */
    entry_t *find(const lockid_t &id);

    /*
     * A new hold of owner's on id, in mode, made with first: in e,
     * id's entry, or in a new one when e is nullptr. owner holds
     * none on id.
     */
    void add(entry_t *e, const lockid_t &id, void *owner, sm_lockmode_t mode,
             void *first);

    /*
     * owner's hold on id covers mode from now on; made with first
     * if owner has none.
     */
    void join(const lockid_t &id, void *owner, sm_lockmode_t mode, void *first);

    /*
     * Drop owner's hold on id if first made it, and the entry with
     * its last holder; a hold made with another, or none, is left.
     */
    void drop(const lockid_t &id, const void *owner, const void *first);

    /* to holds what from held on id, in its place. */
    void move_owner(const lockid_t &id, const void *from, void *to);

    /* Call fn(id, holder) for every hold in the partition. */
    template <class F> void each(F &&fn) const {
      for (entry_t *const *b : {m_buckets, m_old}) {
        size_t n = b == m_buckets ? m_mask + 1 : m_old_mask + 1;
        for (size_t i = 0; nullptr != b && i < n; i++) {
          for (const entry_t *e = b[i]; nullptr != e; e = e->next) {
            for (uint32_t j = 0; j < e->n; j++) {
              fn(e->id, e->at(j));
            }
          }
        }
      }
    }

    /* The ids some owner holds here. */
    size_t size() const { return m_size; }

    /* The buckets now, the array a resize moves to; for tests. */
    size_t buckets() const { return nullptr != m_buckets ? m_mask + 1 : 0; }

    /* Whether a resize is moving buckets; for tests. */
    bool resizing() const { return nullptr != m_old; }

    /* The entries kept for reuse; for tests. */
    size_t spares() const { return m_nfree; }

    /*
     * The fewest buckets, the most spare entries, and the buckets
     * holding entries a call moves while a resize runs, looking at
     * eight times as many.
     */
    static constexpr size_t min_buckets = 64;
    static constexpr size_t free_max = 1024;
    static constexpr size_t move_step = 8;

  private:
    /* The link to id's entry, in either array, or nullptr. */
    entry_t **link_of(const lockid_t &id, size_t h);
    /* An entry for id, from the free list or new. */
    entry_t *take_entry(const lockid_t &id, size_t h);
    /* Give e back: to the free list, or freed. */
    void give_entry(entry_t *e);
    /* Begin moving every bucket into an array of n. */
    void resize(size_t n);
    /* Grow or shrink if the load says to, then move a few buckets. */
    void rebalance();

    entry_t **m_buckets = nullptr; /* m_mask + 1 of them */
    size_t m_mask = 0;
    entry_t **m_old = nullptr; /* being moved from, or nullptr */
    size_t m_old_mask = 0;
    size_t m_moved = 0;        /* m_old's buckets moved so far */
    entry_t *m_free = nullptr; /* entries kept for reuse */
    size_t m_nfree = 0;
    size_t m_size = 0;
  };

  /* The partition of id: the top bits of its hash. */
  static size_t part_of(const lockid_t &id) {
    return lockid_hash_t{}(id) >> (8 * sizeof(size_t) - 4);
  }

  /* Partition i, and the partition of id. */
  part_t &part(size_t i) { return m_parts[i]; }
  part_t &part(const lockid_t &id) { return m_parts[part_of(id)]; }

  /* owner's hold in e, or nullptr. */
  static holder_t *held(entry_t *e, const void *owner);

  /* The hold e was made with, its oldest. */
  static const holder_t &oldest(const entry_t *e) { return e->at(0); }

  /*
   * One walk of e's holders for owner asking mode: *mine is owner's
   * hold, or nullptr, and *want the mode owner would hold; true if
   * another holder's mode conflicts with *want. The others' modes
   * are gathered as they pass, so the check after is one AND.
   */
  static bool probe(entry_t *e, const void *owner, sm_lockmode_t mode,
                    holder_t **mine, sm_lockmode_t *want);

  /* Call fn(owner) for each holder of e but owner not compatible with mode. */
  template <class F>
  static void each_conflict(const entry_t *e, const void *owner,
                            sm_lockmode_t mode, F &&fn) {
    for (uint32_t i = 0; i < e->n; i++) {
      const holder_t &h = e->at(i);
      if (h.owner != owner && !lock_compatible(h.mode, mode)) {
        fn(h.owner);
      }
    }
  }

  /*
   * The whole table, with every partition's mutex held: fn(id,
   * holder) for every hold, the ids held, and whether none is.
   */
  template <class F> void each(F &&fn) const {
    for (const part_t &p : m_parts) {
      p.each(fn);
    }
  }
  size_t size() const {
    size_t n = 0;
    for (const part_t &p : m_parts) {
      n += p.size();
    }
    return n;
  }
  bool empty() const { return 0 == size(); }

private:
  part_t m_parts[parts];
};

} // namespace wisr

#endif /* WISR_LOCKTAB_H */
