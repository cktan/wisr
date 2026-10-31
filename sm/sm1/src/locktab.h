/*
 * locktab.h -- the lock table (SM1_DESIGN.md §5.1): a lock's id ->
 * the owners holding it, each in a mode of its own.
 *
 * It knows ids, Gray's modes and opaque owners; nothing of
 * sessions, waits, the fast path or RocksDB, which stay in
 * lockmgr.cc, its one caller. So it is measured alone
 * (bench/locktab_bench.cc, ../NEXT.md §1 item 4). It takes no
 * mutex: its caller holds lock_mu across every call, and a
 * pointer one returns holds until the table is next changed.
 */
#ifndef WISR_LOCKTAB_H
#define WISR_LOCKTAB_H

#include "sm_lock.h"

#include <stddef.h>
#include <stdint.h>

#include <unordered_map>
#include <vector>

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
  /*
   * An owner's hold on a lock, in mode; first is the caller's, a
   * word for what made the hold, which the table only compares.
   */
  struct holder_t {
    void *owner;
    sm_lockmode_t mode;
    void *first;
  };

  /* The holders of one id, never none. */
  struct entry_t {
    std::vector<holder_t> holders;
  };

  /* The entry of id, or nullptr if no owner holds it. */
  entry_t *find(const lockid_t &id);

  /* owner's hold in e, or nullptr. */
  static holder_t *held(entry_t *e, const void *owner);

  /* The hold e was made with, its oldest. */
  static const holder_t &oldest(const entry_t *e) { return e->holders[0]; }

  /* Call fn(owner) for each holder of e but owner not compatible with mode. */
  template <class F>
  static void each_conflict(const entry_t *e, const void *owner,
                            sm_lockmode_t mode, F &&fn) {
    for (const holder_t &h : e->holders) {
      if (h.owner != owner && !lock_compatible(h.mode, mode)) {
        fn(h.owner);
      }
    }
  }

  /*
   * A new hold of owner's on id, in mode, made with first: in e,
   * id's entry, or in a new one when e is nullptr. owner holds
   * none on id.
   */
  void add(entry_t *e, const lockid_t &id, void *owner, sm_lockmode_t mode,
           void *first);

  /*
   * owner's hold on id covers mode from now on; made with first if
   * owner has none.
   */
  void join(const lockid_t &id, void *owner, sm_lockmode_t mode, void *first);

  /*
   * Drop owner's hold on id if first made it, and the entry with its
   * last holder; a hold made with another, or none, is left.
   */
  void drop(const lockid_t &id, const void *owner, const void *first);

  /* to holds what from held on id, in its place. */
  void move_owner(const lockid_t &id, const void *from, void *to);

  /* Call fn(id, holder) for every hold in the table. */
  template <class F> void each(F &&fn) const {
    for (const auto &[id, e] : m_map) {
      for (const holder_t &h : e.holders) {
        fn(id, h);
      }
    }
  }

  /* The ids some owner holds. */
  size_t size() const { return m_map.size(); }

  /* Whether no owner holds any. */
  bool empty() const { return m_map.empty(); }

private:
  std::unordered_map<lockid_t, entry_t, lockid_hash_t> m_map;
};

} // namespace wisr

#endif /* WISR_LOCKTAB_H */
