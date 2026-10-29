/*
 * lockmgr.cc -- the lock table: locks in Gray's modes, the write
 * latches, and deadlock detection (SM1_DESIGN.md §5.1).
 *
 * A lock is keyed exactly by (locktype, storeid, userkey) -- a row
 * lock names a key in a store, an OID lock an object in none -- and
 * is held by the
 * sessions whose transactions took it, each in a mode of its own,
 * every pair of them compatible (sm1.h, lockmode_t). sm1::lock
 * takes any mode, held until the transaction commits or aborts,
 * and a plain read takes none. A write takes no lock: it checks
 * and puts its key under a short latch, and its uncommitted
 * version stands for the lock, a second writer waiting for the
 * transaction that wrote it (WRITELOCK.md).
 *
 * Deadlocks are found as a transaction begins to wait: it
 * searches the waits-for graph from itself, so a cycle its wait
 * closes is found at once, as PostgreSQL finds one a waiter's
 * place in a queue closes. One that forms while it waits is
 * found when it has slept deadlock_ms, PostgreSQL's
 * deadlock_timeout, and each deadlock_ms after. If it can reach
 * itself, the youngest transaction on that cycle is doomed and
 * woken; every member of a cycle is blocked, so the victim is
 * always waiting in lock_wait or lock_wait_xact and fails from
 * there. A waiter for a shared lock waits on every holder at
 * once, so the search walks a graph, not a chain.
 */

#include "error.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"
#include "sm_sys.h"

#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace wisr;

namespace wisr {

std::atomic<bool> lock_test_rows_collide{false};

} // namespace wisr

namespace {

using lmode_t = sm1::lockmode_t;

/* A mode as an index into the tables below: IS, IX, S, SIX, X. */
int rank(lmode_t m) {
  switch (m) {
  case sm1::SM_LOCKMODE_IS:
    return 0;
  case sm1::SM_LOCKMODE_IX:
    return 1;
  case sm1::SM_LOCKMODE_SH:
    return 2;
  case sm1::SM_LOCKMODE_SIX:
    return 3;
  case sm1::SM_LOCKMODE_EX:
    return 4;
  }
  return 4;
}

/* Gray's compatibility of two modes held by different sessions. */
bool compatible(lmode_t a, lmode_t b) {
  static const bool ok[5][5] = {
      /*        IS     IX     S      SIX    X    */
      /* IS  */ {true, true, true, true, false},
      /* IX  */ {true, true, false, false, false},
      /* S   */ {true, false, true, false, false},
      /* SIX */ {true, false, false, false, false},
      /* X   */ {false, false, false, false, false},
  };
  return ok[rank(a)][rank(b)];
}

/*
 * The weakest mode covering both a and b, which a holder of a that
 * asks for b converts to: IS < IX, S < SIX < X, and IX and S meet
 * at SIX.
 */
lmode_t cover(lmode_t a, lmode_t b) {
  static const lmode_t sup[5][5] = {
      {sm1::SM_LOCKMODE_IS, sm1::SM_LOCKMODE_IX, sm1::SM_LOCKMODE_SH,
       sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_EX},
      {sm1::SM_LOCKMODE_IX, sm1::SM_LOCKMODE_IX, sm1::SM_LOCKMODE_SIX,
       sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_EX},
      {sm1::SM_LOCKMODE_SH, sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_SH,
       sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_EX},
      {sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_SIX,
       sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_EX},
      {sm1::SM_LOCKMODE_EX, sm1::SM_LOCKMODE_EX, sm1::SM_LOCKMODE_EX,
       sm1::SM_LOCKMODE_EX, sm1::SM_LOCKMODE_EX},
  };
  return sup[rank(a)][rank(b)];
}

using holder_t = db_t::lock_ent_t::holder_t;
using lockref_t = sm1::session_t::lockref_t;

/* The top bit of a lock key's hi: set for an OID lock. */
uint64_t type_bit(sm1::locktype_t type) { return uint64_t(type) << 63; }

/* The lock table key of (type, storeid, userkey); see lock_key_t. */
lock_key_t lock_key(sm1::locktype_t type, uint64_t storeid,
                    rocksdb::Slice key) {
  // The type flips the top bit, so no two namespaces share a lock.
  uint64_t hi = storeid ^ type_bit(type);
  // An oid is the key exactly; a row's key is hashed into 64 bits.
  if (sm1::LOCKTYPE_OID == type) {
    return {hi, sm0::get_be64(key.data())};
  }
  // A test makes every row's key hash alike.
  if (lock_test_rows_collide.load(std::memory_order_relaxed)) {
    return {hi, 0};
  }
  return {hi, std::hash<std::string_view>{}(
                  std::string_view(key.data(), key.size()))};
}

/* The name of (type, storeid, userkey); see lock_name. */
std::string name_of(sm1::locktype_t type, uint64_t storeid,
                    rocksdb::Slice key) {
  std::string k;
  // Reserve room for the type byte, the storeid and the key.
  k.reserve(1 + 8 + key.size());
  // Lead with the type, then the storeid, then the userkey.
  k.push_back(static_cast<char>(type));
  sm0::put_be64(k, storeid);
  k.append(key.data(), key.size());
  return k;
}

/* A lock list entry for (type, userkey), at k, held in mode. */
lockref_t make_ref(const lock_key_t &k, sm1::locktype_t type,
                   rocksdb::Slice key, lmode_t mode) {
  lockref_t l;
  l.key = k;
  // Only a row's key is not in k exactly.
  if (sm1::LOCKTYPE_ROW == type) {
    l.row.assign(key.data(), key.size());
  }
  l.type = type;
  l.mode = mode;
  return l;
}

/* Whether the list entry l is the lock (type, userkey) at k. */
bool is_ref(const lockref_t &l, const lock_key_t &k, sm1::locktype_t type,
            rocksdb::Slice key) {
  return l.key == k && l.type == type &&
         (sm1::LOCKTYPE_OID == type || rocksdb::Slice(l.row) == key);
}

/*
 * s's list entry for exactly (type, userkey), whose table entry s
 * holds as h, or null: h's first entry, unless another row key that
 * hashes alike made the hold. The caller holds lock_mu.
 */
lockref_t *exact_ref(sm1::session_t *s, const holder_t &h, sm1::locktype_t type,
                     rocksdb::Slice key) {
  lockref_t &first = s->locks[h.idx];
  if (is_ref(first, first.key, type, key)) {
    return &first;
  }
  // Keys that hash alike: look among the later entries, newest first.
  for (size_t i = s->locks.size(); i-- > h.idx + 1;) {
    if (is_ref(s->locks[i], first.key, type, key)) {
      return &s->locks[i];
    }
  }
  return nullptr;
}

/*
 * Remove one of s's references to k, and s from its holders with
 * the last, dropping the entry once the last holder goes. The
 * caller holds lock_mu.
 */
void lock_drop_holder(db_t *db, const lock_key_t &k, sm1::session_t *s) {
  auto it = db->lock_table.find(k);
  // Gone already: a rollback may have released it.
  if (it == db->lock_table.end()) {
    return;
  }
  std::vector<holder_t> &holders = it->second.holders;
  // Drop us from the holder list, with our last reference.
  for (size_t i = 0; i < holders.size(); i++) {
    if (holders[i].session == s) {
      if (0 == --holders[i].refs) {
        holders.erase(holders.begin() + i);
      }
      break;
    }
  }
  // The last holder out removes the entry.
  if (holders.empty()) {
    db->lock_table.erase(it);
  }
}

/*
 * The session's lock list entry for (storeid, key), or null. The
 * list is the session's own, so no lock_mu is taken. Newest
 * first: the lock in question was likely just taken.
 */
lockref_t *find_lockref(sm1::session_t *s, sm1::locktype_t type,
                        uint64_t storeid, rocksdb::Slice key) {
  lock_key_t k = lock_key(type, storeid, key);
  for (size_t i = s->locks.size(); i-- > 0;) {
    if (is_ref(s->locks[i], k, type, key)) {
      return &s->locks[i];
    }
  }
  return nullptr;
}

/*
 * Search the waits-for graph from cur for a path back to s,
 * with lock_mu held. path holds the transactions between s and
 * cur, so a hit has the whole cycle to choose a victim from.
 * Returns true once a cycle through s is found.
 */
bool find_cycle(sm1::session_t *s, sm1::session_t *cur,
                std::unordered_set<sm1::session_t *> *seen,
                std::vector<sm1::session_t *> *path) {
  // Follow every holder this transaction waits on.
  for (sm1::session_t *next : cur->waiting_for) {
    // Back at s: path holds the rest of the cycle.
    if (next == s) {
      return true;
    }
    // Skip a transaction already searched; its cycles are not
    // ours, and this keeps the search linear.
    if (!seen->insert(next).second) {
      continue;
    }
    // Walk on, remembering how we got here.
    path->push_back(next);
    if (find_cycle(s, next, seen, path)) {
      return true;
    }
    path->pop_back();
  }
  return false;
}

/*
 * Search the waits-for graph from s, with lock_mu held. If s
 * can reach itself, doom the youngest transaction on the cycle
 * and wake everyone so the victim sees it. A path that ends, or
 * a cycle that does not pass through s, is not s's to resolve:
 * the members of that other cycle run their own check.
 */
void check_deadlock(db_t *db, sm1::session_t *s) {
  std::unordered_set<sm1::session_t *> seen;
  std::vector<sm1::session_t *> path;
  // No cycle through us: nothing to resolve here.
  if (!find_cycle(s, s, &seen, &path)) {
    return;
  }
  // The youngest transaction on the cycle is the victim.
  sm1::session_t *victim = s;
  for (sm1::session_t *m : path) {
    // Younger: began later.
    if (m->begin_seq > victim->begin_seq) {
      victim = m;
    }
  }
  // Doom it.
  victim->doomed = sm1::SM_ERR_DEADLOCK;
  // Wake every waiter so the victim sees its doom.
  db->lock_cv.notify_all();
}

/*
 * The other holders of k that conflict with mode for s, into
 * out, with lock_mu held; sets *mine to s's own hold, if any, and
 * *want to the mode s would hold. Returns false if k has no entry.
 */
bool conflicting(db_t *db, sm1::session_t *s, const lock_key_t &k, lmode_t mode,
                 holder_t **mine, lmode_t *want,
                 std::vector<sm1::session_t *> *out) {
  out->clear();
  *mine = nullptr;
  *want = mode;
  auto it = db->lock_table.find(k);
  if (it == db->lock_table.end()) {
    return false;
  }
  // Our own hold, if any, and the mode we would hold after.
  for (holder_t &h : it->second.holders) {
    if (h.session == s) {
      *mine = &h;
      break;
    }
  }
  *want = *mine ? cover((*mine)->mode, mode) : mode;
  // The other holders whose modes conflict with that.
  for (const holder_t &h : it->second.holders) {
    if (h.session != s && !compatible(h.mode, *want)) {
      out->push_back(h.session);
    }
  }
  return true;
}

/*
 * The condition of one sleep in sm_wait(): false before it
 * sleeps, true once it wakes. The wait sleeps once, and the caller
 * looks at the lock table again.
 */
bool woke(void *arg) {
  bool *slept = static_cast<bool *>(arg);
  bool was = *slept;
  *slept = true;
  return was;
}

/*
 * How long s waits before it looks for a cycle: its own setting
 * (sm1::set_deadlock_ms), else its database's.
 */
int deadlock_wait_ms(const sm1::session_t *s) {
  return s->deadlock_ms > 0 ? s->deadlock_ms : s->db->deadlock_ms;
}

/*
 * Sleep on lock_cv, with lock_mu held in guard, until woken, until
 * a bit in the thread's wait mask is raised (sm_sys.h), or until
 * *deadline_us; at the deadline, look for a cycle through s and
 * re-arm it. Returns false if interrupted: the bit stays pending,
 * for the caller's caller to take.
 */
bool wait_once(db_t *db, sm1::session_t *s, std::unique_lock<std::mutex> &guard,
               int64_t *deadline_us) {
  bool slept = false;

  // An interrupt raised before this wait ends it at once.
  switch (sm_wait(&guard, &db->lock_cv, woke, &slept, sm_wait_mask(),
                  *deadline_us)) {
  case SM_WAIT_DONE:
    return true;
  case SM_WAIT_INTERRUPTED:
    return false;
  case SM_WAIT_TIMED_OUT:
    // Timed out: look for a cycle through us.
    check_deadlock(db, s);
    // Re-arm the timer for the next check.
    *deadline_us = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
    return true;
  }
  return true;
}

} // namespace

namespace wisr {

/* The write latch (storeid, key) hashes to. */
std::mutex &lock_latch(db_t *db, uint64_t storeid, rocksdb::Slice key) {
  size_t h =
      std::hash<std::string_view>{}(std::string_view(key.data(), key.size()));
  // Mix the storeid in, so equal keys of two stores spread too.
  h ^= std::hash<uint64_t>{}(storeid) + 0x9e3779b97f4a7c15ULL + (h << 6) +
       (h >> 2);
  return db->write_latch[h % db_t::write_latches];
}

/* Take (type, storeid, key) in mode unless another holder conflicts. */
int lock_try(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
             rocksdb::Slice key, sm1::lockmode_t mode) {
  db_t *db = s->db;
  lock_key_t k = lock_key(type, storeid, key);
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  holder_t *mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  conflicting(db, s, k, mode, &mine, &want, &others);
  // Another holder conflicts: take nothing.
  if (!others.empty()) {
    return 2;
  }
  if (mine) {
    mine->mode = want;
    // Ours already: the list shows the stronger mode from now on.
    lockref_t *l = exact_ref(s, *mine, type, key);
    if (l) {
      l->mode = cover(l->mode, mode);
      return 0;
    }
    // Another row's key hashed alike: this one joins the hold.
    mine->refs++;
    s->locks.push_back(make_ref(k, type, key, mode));
    return 0;
  }
  // Join the holders, or make the entry.
  db->lock_table[k].holders.push_back({s, want, 1, s->locks.size()});
  // Remember the lock for release at commit or abort.
  s->locks.push_back(make_ref(k, type, key, want));
  return 0;
}

/* For a write: 2 if another holder conflicts, else convert our own. */
int lock_write_check(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  db_t *db = s->db;
  // A write meets row locks and no other lock type.
  lock_key_t k = lock_key(sm1::LOCKTYPE_ROW, storeid, key);
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  holder_t *mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  // No entry: the common write, which takes no lock.
  if (!conflicting(db, s, k, sm1::SM_LOCKMODE_EX, &mine, &want, &others)) {
    return 0;
  }
  if (!others.empty()) {
    return 2;
  }
  // A lock of our own on the key is held exclusive from now on,
  // as a write converts it (SM1_DESIGN.md §5.1).
  if (mine) {
    mine->mode = sm1::SM_LOCKMODE_EX;
    // Our own lock on exactly this key, not one hashed alike.
    lockref_t *l = exact_ref(s, *mine, sm1::LOCKTYPE_ROW, key);
    if (l) {
      l->mode = sm1::SM_LOCKMODE_EX;
    }
  }
  return 0;
}

/* Block until no other holder of (storeid, key) conflicts with mode. */
int lock_wait(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
              rocksdb::Slice key, sm1::lockmode_t mode) {
  db_t *db = s->db;
  lock_key_t k = lock_key(type, storeid, key);
  // Hold lock_mu for the table and the waits-for lists.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // However it returns, it no longer waits for k: still under lock_mu.
  defer { s->waiting_key.clear(); };
  // Time the deadlock check after the first, made as the wait begins.
  int64_t deadline = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
  bool checked = false;
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      s->waiting_for.clear();
      return -1;
    }
    holder_t *mine;
    lmode_t want;
    // Wait on exactly the holders that conflict.
    conflicting(db, s, k, mode, &mine, &want, &s->waiting_for);
    if (s->waiting_for.empty()) {
      return 0;
    }
    // A qa_nowait session is told, and waits on nobody.
    if (s->qa_nowait) {
      s->waiting_for.clear();
      return 1;
    }
    // As the wait begins, a cycle it closes is found at once.
    if (!checked) {
      checked = true;
      // What it waits for, for lock_status.
      s->waiting_key = name_of(type, storeid, key);
      s->waiting_mode = mode;
      check_deadlock(db, s);
      if (s->doomed) {
        continue;
      }
    }
    // Interrupted, before the wait or during it: stop waiting.
    if (!wait_once(db, s, guard, &deadline)) {
      s->waiting_for.clear();
      return 2;
    }
  }
}

/* Block until xid ends, or rolls back to a savepoint. */
int lock_wait_xact(sm1::session_t *s, uint64_t xid) {
  db_t *db = s->db;
  // Hold lock_mu for the waits-for lists; every wake-up takes it.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // A rollback from now on may delete what we wait for.
  uint64_t epoch = db->rollback_epoch;
  // Time the deadlock check after the first, made as the wait begins.
  int64_t deadline = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
  bool checked = false;
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      s->waiting_for.clear();
      return -1;
    }
    sm1::session_t *owner = nullptr;
    bool running;
    {
      // Hold txn_mu while reading the active table.
      std::lock_guard<std::mutex> t(db->txn_mu);
      running = db->active.count(xid) != 0;
      auto it = db->xid_owner.find(xid);
      if (it != db->xid_owner.end()) {
        owner = it->second;
      }
    }
    // Ended, or rolled back: the caller looks again.
    if (!running || db->rollback_epoch != epoch) {
      s->waiting_for.clear();
      return 0;
    }
    // A qa_nowait session is told, and waits on nobody.
    if (s->qa_nowait) {
      s->waiting_for.clear();
      return 1;
    }
    // Our edge in the waits-for graph: to the transaction itself.
    s->waiting_for.clear();
    if (owner && owner != s) {
      s->waiting_for.push_back(owner);
    }
    // As the wait begins, a cycle it closes is found at once.
    if (!checked) {
      checked = true;
      check_deadlock(db, s);
      if (s->doomed) {
        continue;
      }
    }
    // Interrupted, before the wait or during it: stop waiting.
    if (!wait_once(db, s, guard, &deadline)) {
      s->waiting_for.clear();
      return 2;
    }
  }
}

/* Refuse a lock on a row the statement changed since; see session.h. */
int lock_self_modified(sm1::session_t *s, uint32_t own_seq) {
  if (s->stmt_marked && own_seq > s->stmt_seq) {
    return set_error(sm1::SM_ERR_SELFMODIFIED,
                     "the row was changed after this statement began");
  }
  return 0;
}

/* Fail the call for a wait that did not succeed; see session.h. */
int lock_wait_error(sm1::session_t *s, int waited) {
  if (waited == 2) {
    return set_error(sm1::SM_ERR_INTERRUPTED, "the wait was interrupted");
  }
  if (waited > 0) {
    return set_error(sm1::SM_ERR_BLOCKED, "the row is locked by another");
  }
  return xct_fail_retryable(s, sm1::SM_ERR_DEADLOCK,
                            "chosen as a deadlock victim");
}

/* Wake every waiter: a transaction ended, or rolled back. */
void lock_wake(db_t *db, bool rollback) {
  std::lock_guard<std::mutex> guard(db->lock_mu);
  if (rollback) {
    db->rollback_epoch++;
  }
  db->lock_cv.notify_all();
}

/* Move every lock s holds, and its list, to g, which takes its place. */
void lock_transfer(db_t *db, sm1::session_t *s, sm1::session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-point each entry's holder list at g.
  for (const lockref_t &l : s->locks) {
    auto it = db->lock_table.find(l.key);
    // Replace s with g among the holders.
    if (it != db->lock_table.end()) {
      for (holder_t &h : it->second.holders) {
        if (h.session == s) {
          h.session = g;
        }
      }
    }
  }
  // Move the list with them: a holder's idx points into it.
  g->locks.swap(s->locks);
  // Waiters re-read their holders.
  db->lock_cv.notify_all();
}

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm1::session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-take each lock in the mode the record carries.
  for (size_t i = 0; i < g->locks.size(); i++) {
    const lockref_t &l = g->locks[i];
    std::vector<holder_t> &holders = db->lock_table[l.key].holders;
    holder_t *mine = nullptr;
    for (holder_t &h : holders) {
      if (h.session == g) {
        mine = &h;
      }
    }
    // A second row key hashed alike joins the first's hold.
    if (mine) {
      mine->refs++;
      mine->mode = cover(mine->mode, l.mode);
    } else {
      holders.push_back({g, l.mode, 1, i});
    }
  }
}

/* A lock's name; see session.h. */
std::string lock_name(const lockref_t &l) {
  uint64_t storeid = l.key.hi ^ type_bit(l.type);
  // An OID lock's key is its oid, big-endian.
  if (sm1::LOCKTYPE_OID == l.type) {
    std::string oid;
    sm0::put_be64(oid, l.key.lo);
    return name_of(l.type, storeid, oid);
  }
  return name_of(l.type, storeid, l.row);
}

/* The entry a name stands for; see session.h. */
bool lock_from_name(std::string_view name, lockref_t *l) {
  // The type byte and the storeid at least.
  if (name.size() < 9) {
    return false;
  }
  uint8_t type = static_cast<uint8_t>(name[0]);
  rocksdb::Slice key(name.data() + 9, name.size() - 9);
  // A type there is, and an OID lock's key is an oid.
  if (type > sm1::LOCKTYPE_OID ||
      (sm1::LOCKTYPE_OID == type && 8 != key.size())) {
    return false;
  }
  sm1::locktype_t t = static_cast<sm1::locktype_t>(type);
  *l = make_ref(lock_key(t, sm0::get_be64(name.data() + 1), key), t, key,
                l->mode);
  return true;
}

/* Whether the session holds (storeid, key), and in which mode. */
bool lock_held(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
               sm1::lockmode_t *mode) {
  lockref_t *l = find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  // Not held.
  if (!l) {
    return false;
  }
  *mode = l->mode;
  return true;
}

/* Mark (storeid, key)'s exclusive lock: the next write may go ahead. */
void lock_set_latest(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  sm1::session_t::lockref_t *l =
      find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  // Count a mark once.
  if (l && !l->latest) {
    l->latest = true;
    s->latest_marks++;
  }
}

/* Whether (storeid, key)'s lock carries the mark. */
bool lock_has_latest(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // No mark anywhere: skip the scan.
  if (s->latest_marks == 0) {
    return false;
  }
  sm1::session_t::lockref_t *l =
      find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  return l && l->latest;
}

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1::session_t *s, uint64_t storeid,
                       rocksdb::Slice key) {
  // No mark anywhere: skip the scan, so a plain write pays nothing.
  if (s->latest_marks == 0) {
    return;
  }
  sm1::session_t::lockref_t *l =
      find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  if (l && l->latest) {
    l->latest = false;
    s->latest_marks--;
  }
}

/* Drop every lock the session holds and wake the waiters. */
void lock_release_all(sm1::session_t *s) {
  // Release everything, from the first lock on.
  lock_release_from(s, 0);
}

/* Drop the locks taken after the first n and wake the waiters. */
void lock_release_from(sm1::session_t *s, size_t n) {
  db_t *db = s->db;
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // We no longer wait on anyone.
  s->waiting_for.clear();
  // Nothing past n to release.
  if (s->locks.size() <= n) {
    return;
  }
  // Drop our hold on every lock past the first n.
  for (size_t i = n; i < s->locks.size(); i++) {
    lock_drop_holder(db, s->locks[i].key, s);
    // A mark goes with its lock.
    if (s->locks[i].latest) {
      s->latest_marks--;
    }
  }
  // Forget them in the session's list.
  s->locks.resize(n);
  // Wake the waiters to retry.
  db->lock_cv.notify_all();
}

} // namespace wisr
