/*
 * lockmgr.cc -- the lock table: locks in Gray's modes, the write
 * latches, and deadlock detection (SM1_DESIGN.md §5.1).
 *
 * A lock is keyed exactly by (locktype, storeid, userkey) -- a row
 * lock names a key in a store, an OID lock an object in none -- and
 * is held by the
 * sessions whose transactions took it, each in a mode of its own,
 * every pair of them compatible (sm1.h, lockmode_t). sm1::lock_row
 * and sm1::lock_oid take any mode, held until the transaction
 * commits or aborts, and a plain read takes none. A write takes
 * no lock: it checks and puts its key under a short latch, and its
 * uncommitted version stands for the lock, a second writer waiting for the
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
 * once, so the search walks a graph, not a chain, its edges read
 * from the table as it goes (lock_blockers): a waiter's own list
 * goes stale as its holders release, before it wakes to look.
 */

#include "error.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"
#include "sm_sys.h"

#include <algorithm>
#include <cstring>
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

/* The top bit of a lock key's hi: set for an OID lock. */
uint64_t type_bit(sm1::locktype_t type) { return uint64_t(type) << 63; }

/* The lock id of (type, storeid, userkey); see lockid_t. */
lockid_t lockid(sm1::locktype_t type, uint64_t storeid, rocksdb::Slice key) {
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

/*
 * A lock list entry of s's for (type, userkey), at k, held in mode:
 * a row's key, which k does not hold exactly, copied into s's
 * lock_keys.
 */
lockref_t make_ref(sm1::session_t *s, const lockid_t &k, sm1::locktype_t type,
                   rocksdb::Slice key, lmode_t mode) {
  lockref_t l;
  l.id = k;
  if (sm1::LOCKTYPE_ROW == type) {
    char *row = s->lock_keys.alloc(key.size());
    memcpy(row, key.data(), key.size());
    l.row = row;
    l.row_len = static_cast<uint32_t>(key.size());
  }
  l.type = type;
  l.mode = mode;
  return l;
}

/* The key of row lock l. */
rocksdb::Slice row_of(const lockref_t &l) {
  return rocksdb::Slice(l.row, l.row_len);
}

/* Whether list entry l is the lock (type, userkey) at k. */
bool is_ref(const lockref_t &l, const lockid_t &k, sm1::locktype_t type,
            rocksdb::Slice key) {
  return l.id == k && l.type == type &&
         (sm1::LOCKTYPE_OID == type || row_of(l) == key);
}

/*
 * s's list entry for exactly (type, userkey), whose table entry s
 * holds as h, or nullptr: h's first entry, unless another row key
 * that hashes alike made the hold. The caller holds lock_mu.
 */
lockref_t *exact_ref(sm1::session_t *s, const holder_t &h, sm1::locktype_t type,
                     rocksdb::Slice key) {
  const lockid_t &k = h.first->id;
  if (is_ref(*h.first, k, type, key)) {
    return h.first;
  }
  // Keys that hash alike share the hold: look through the list.
  return s->locks.find(
      [&](const lockref_t &l) { return is_ref(l, k, type, key); });
}

/*
 * Remove one of s's references to k, and s from its holders with
 * the last, dropping the entry once the last holder goes. The
 * caller holds lock_mu.
 */
void lock_drop_holder(db_t *db, const lockid_t &k, sm1::session_t *s) {
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
 * The session's lock list entry for (storeid, key), or nullptr. The
 * list is the session's own, so no lock_mu is taken. The newest
 * first, the lock in question likely just taken, then the rest from
 * the oldest: the list links only forward.
 */
lockref_t *find_lockref(sm1::session_t *s, sm1::locktype_t type,
                        uint64_t storeid, rocksdb::Slice key) {
  lockref_t *last = s->locks.last();
  if (nullptr == last) {
    return nullptr;
  }
  lockid_t k = lockid(type, storeid, key);
  if (is_ref(*last, k, type, key)) {
    return last;
  }
  return s->locks.find(
      [&](const lockref_t &l) { return is_ref(l, k, type, key); });
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
  // Follow every session this transaction waits on now: an edge
  // its wait recorded may be gone, the holder released, the
  // waiter not yet awake to look again.
  std::vector<sm1::session_t *> blockers;
  lock_blockers(s->db, cur, &blockers);
  for (sm1::session_t *next : blockers) {
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

/* No holder of a lock: what a search finds of none. */
constexpr size_t no_ref = SIZE_MAX;

/* The index of s among holders, or no_ref. */
size_t holder_of(const std::vector<holder_t> &holders,
                 const sm1::session_t *s) {
  for (size_t i = 0; i < holders.size(); i++) {
    if (holders[i].session == s) {
      return i;
    }
  }
  return no_ref;
}

/* An entry of the lock table, as a lookup finds it, or its end. */
using entry_it = decltype(db_t::lock_table)::iterator;

/*
 * The other holders of the entry at it that conflict with mode for
 * s, into out, with lock_mu held; sets *mine to the index of s's own
 * hold among its holders, or no_ref, and *want to the mode s would
 * hold. Returns false if there is no entry, it being the table's end.
 */
bool conflicting(db_t *db, entry_it it, sm1::session_t *s, lmode_t mode,
                 size_t *mine, lmode_t *want,
                 std::vector<sm1::session_t *> *out) {
  out->clear();
  *mine = no_ref;
  *want = mode;
  if (it == db->lock_table.end()) {
    return false;
  }
  const std::vector<holder_t> &holders = it->second.holders;
  // Our own hold, if any, and the mode we would hold after.
  *mine = holder_of(holders, s);
  *want = no_ref != *mine ? cover(holders[*mine].mode, mode) : mode;
  // The other holders whose modes conflict with that.
  for (const holder_t &h : holders) {
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

/* Whether an OID lock in mode may take the fast path: IS or IX. */
bool is_weak(lmode_t mode) {
  return sm1::SM_LOCKMODE_IS == mode || sm1::SM_LOCKMODE_IX == mode;
}

/* The count of strong locks on keys that hash with k. */
std::atomic<uint32_t> &strong_count(db_t *db, const lockid_t &k) {
  return db->fp_strong[lockid_hash_t{}(k) % db_t::fp_strong_slots];
}

/*
 * Move s's slot i into the lock table, as a hold of s's, joining
 * one s has there already. The caller holds s->fp_mu, not lock_mu.
 */
void fp_move(db_t *db, sm1::session_t *s, int i) {
  sm1::session_t::fp_slot_t &f = s->fp[i];
  {
    // Hold lock_mu while editing the table.
    std::lock_guard<std::mutex> guard(db->lock_mu);
    std::vector<holder_t> &holders = db->lock_table[f.id].holders;
    size_t mine = holder_of(holders, s);
    if (no_ref != mine) {
      // A hold of ours already, made before this slot: a slot for k
      // is moved to the table before a hold there is made, so the
      // hold's first entry stays the first.
      holders[mine].refs++;
      holders[mine].mode = cover(holders[mine].mode, f.mode);
    } else {
      holders.push_back({s, f.mode, 1, f.ref});
    }
    // Its list entry is the table's from now on.
    f.ref->slot = -1;
  }
  f.used = false;
  s->fp_used--;
}

/*
 * Move every session's slot for k into the lock table, where a
 * strong locker, counted already, meets them (§5.1).
 */
void fp_sweep(db_t *db, const lockid_t &k) {
  // Hold fp_reg_mu, so no session in the list is freed meanwhile.
  std::lock_guard<std::mutex> reg(db->fp_reg_mu);
  for (sm1::session_t *o : db->fp_sessions) {
    // Hold its fp_mu, so it takes no slot as we look.
    std::lock_guard<std::mutex> fp(o->fp_mu);
    for (int i = 0; o->fp_used > 0 && i < sm1::session_t::fp_slots; i++) {
      if (o->fp[i].used && o->fp[i].id == k) {
        fp_move(db, o, i);
      }
    }
  }
}

/*
 * Take the weak OID lock (key, at k) in mode by a slot of s's:
 * false if it must go to the table, with s's slot for k moved
 * there first when a strong lock hashes with it.
 */
bool fp_take(db_t *db, sm1::session_t *s, const lockid_t &k, rocksdb::Slice key,
             lmode_t mode) {
  // Hold our fp_mu: a sweep reads our slots under it.
  std::lock_guard<std::mutex> fp(s->fp_mu);
  int mine = -1, free = -1;
  for (int i = 0; i < sm1::session_t::fp_slots; i++) {
    if (!s->fp[i].used) {
      free = free < 0 ? i : free;
    } else if (s->fp[i].id == k) {
      mine = i;
      break;
    }
  }
  // Read under fp_mu: a strong locker counts itself before its
  // sweep takes it, so either it sees our slot or we see its count.
  if (0 != strong_count(db, k).load()) {
    if (mine >= 0) {
      fp_move(db, s, mine);
    }
    return false;
  }
  if (mine >= 0) {
    // Ours already: IS and IX cover each other as IX, still weak.
    sm1::session_t::fp_slot_t &f = s->fp[mine];
    f.mode = cover(f.mode, mode);
    f.ref->mode = f.mode;
    return true;
  }
  // Every slot taken: the table holds it.
  if (free < 0) {
    return false;
  }
  lockref_t l = make_ref(s, k, sm1::LOCKTYPE_OID, key, mode);
  l.slot = static_cast<int8_t>(free);
  s->fp[free] = {k, mode, s->locks.emplace(l), true};
  s->fp_used++;
  return true;
}

/* s's first list entry for the OID lock k, or nullptr. */
lockref_t *first_ref(sm1::session_t *s, const lockid_t &k) {
  return s->locks.find([&](const lockref_t &l) {
    return sm1::LOCKTYPE_OID == l.type && l.id == k;
  });
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
  lockid_t k = lockid(type, storeid, key);
  // Room for the entry and its key now, so none is allocated under
  // lock_mu.
  s->locks.reserve();
  if (sm1::LOCKTYPE_ROW == type) {
    s->lock_keys.reserve(key.size());
  }
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  entry_it it = db->lock_table.find(k);
  size_t mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  conflicting(db, it, s, mode, &mine, &want, &others);
  // Another holder conflicts: take nothing.
  if (!others.empty()) {
    return 2;
  }
  if (no_ref != mine) {
    std::vector<holder_t> &holders = it->second.holders;
    holders[mine].mode = want;
    // Ours already: the list shows the stronger mode from now on.
    lockref_t *l = exact_ref(s, holders[mine], type, key);
    if (nullptr != l) {
      l->mode = cover(l->mode, mode);
      return 0;
    }
    // Another row's key hashed alike: this one joins the hold.
    holders[mine].refs++;
    s->locks.emplace(make_ref(s, k, type, key, mode));
    return 0;
  }
  // Join the holders, or make the entry: one lookup either way.
  std::vector<holder_t> &holders = db->lock_table.end() != it
                                       ? it->second.holders
                                       : db->lock_table[k].holders;
  // Remember the lock for release at commit or abort.
  holders.push_back(
      {s, want, 1, s->locks.emplace(make_ref(s, k, type, key, want))});
  return 0;
}

/*
 * Take the OID lock (storeid, key) in mode; see session.h.
 *
 * The fast path (SM1_DESIGN.md §5.1) keeps a table's weak locks,
 * IS and IX, out of the lock table and its lock_mu. IS and IX never
 * conflict with each other, so only a strong lock, S, SIX or X,
 * must see them. A weak lock goes in a slot of the session's own,
 * under its fp_mu, unless a strong lock is held or asked on a key
 * that hashes with it: its count in fp_strong is not 0. A strong
 * locker raises that count first, then moves every session's slot
 * for its key into the table (fp_sweep), and locks as any lock does.
 *
 * Skipping lock_mu: a SELECT on t1, with no DDL on t1.
 *   1. sm3 asks IS on t1's oid. fp_take takes our fp_mu, finds
 *      t1's count 0 and a slot free, fills it, and adds the entry
 *      to our lock list.
 *   2. The commit clears the slot under fp_mu alone, in
 *      lock_release_from: our locks all in slots, it takes no
 *      lock_mu either.
 *
 * Not skipping it: an ALTER TABLE t1 while that SELECT runs.
 *   1. The ALTER asks X on t1's oid. It raises t1's count, then
 *      sweeps: under the SELECT session's fp_mu, it moves that
 *      session's IS slot into the table, as a holder.
 *   2. lock_try, under lock_mu, finds the IS, which conflicts with
 *      X, and lock_wait sleeps until the SELECT commits.
 *   3. A second SELECT asks IS on t1 meanwhile. fp_take sees the
 *      count and declines, and the IS goes through lock_try under
 *      lock_mu: granted, the ALTER not yet a holder, as sm1 keeps
 *      no queue.
 *   4. The ALTER commits, its count goes back to 0, and the next
 *      SELECT on t1 takes a slot again.
 *
 * Why no lock is missed: both sides meet at the weak session's
 * fp_mu. fp_take reads the count under it, and a sweep takes it
 * after raising the count. If fp_take ran first, the sweep finds
 * its slot; if after, it sees the count and goes to the table.
 */
int lock_oid(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
             sm1::lockmode_t mode, bool nowait) {
  db_t *db = s->db;
  lockid_t k = lockid(sm1::LOCKTYPE_OID, storeid, key);
  bool counted = false;
  if (is_weak(mode)) {
    if (fp_take(db, s, k, key, mode)) {
      return 0;
    }
  } else {
    lockref_t *f = first_ref(s, k);
    // A strong lock of ours on k counts already, and keeps slots off it.
    if (nullptr == f || !f->strong) {
      // Count ourselves, then meet every weak holder in the table.
      strong_count(db, k).fetch_add(1);
      counted = true;
      fp_sweep(db, k);
    }
  }
  int rc;
  for (;;) {
    // No writer of this name can exist, so no latch is needed.
    if (0 == lock_try(s, sm1::LOCKTYPE_OID, storeid, key, mode)) {
      rc = 0;
      break;
    }
    if (nowait) {
      rc = 1;
      break;
    }
    // Wait for the holders that conflict.
    int waited = lock_wait(s, sm1::LOCKTYPE_OID, storeid, key, mode);
    if (0 != waited) {
      rc = lock_wait_error(s, waited);
      break;
    }
  }
  if (counted) {
    if (0 == rc) {
      // The count goes with the first entry, released last: a
      // conversion outlives a rollback past the later ones.
      first_ref(s, k)->strong = true;
    } else {
      strong_count(db, k).fetch_sub(1);
    }
  }
  return rc;
}

/* Add the session to its database's sweep list; see session.h. */
void lock_register(sm1::session_t *s) {
  std::lock_guard<std::mutex> reg(s->db->fp_reg_mu);
  s->db->fp_sessions.push_back(s);
}

/* Take the session out of its database's sweep list; see session.h. */
void lock_unregister(sm1::session_t *s) {
  std::lock_guard<std::mutex> reg(s->db->fp_reg_mu);
  std::vector<sm1::session_t *> &v = s->db->fp_sessions;
  v.erase(std::find(v.begin(), v.end(), s));
}

/* For a write: 2 if another holder conflicts, else convert our own. */
int lock_write_check(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  db_t *db = s->db;
  // A write meets row locks and no other lock type.
  lockid_t k = lockid(sm1::LOCKTYPE_ROW, storeid, key);
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  entry_it it = db->lock_table.find(k);
  size_t mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  // No entry: the common write, which takes no lock.
  if (!conflicting(db, it, s, sm1::SM_LOCKMODE_EX, &mine, &want, &others)) {
    return 0;
  }
  if (!others.empty()) {
    return 2;
  }
  // A lock of our own on the key is held exclusive from now on,
  // as a write converts it (SM1_DESIGN.md §5.1).
  if (no_ref != mine) {
    holder_t &h = it->second.holders[mine];
    h.mode = sm1::SM_LOCKMODE_EX;
    // Our own lock on exactly this key, not one hashed alike.
    lockref_t *l = exact_ref(s, h, sm1::LOCKTYPE_ROW, key);
    if (nullptr != l) {
      l->mode = sm1::SM_LOCKMODE_EX;
    }
  }
  return 0;
}

/* Block until no other holder of (storeid, key) conflicts with mode. */
int lock_wait(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
              rocksdb::Slice key, sm1::lockmode_t mode) {
  db_t *db = s->db;
  lockid_t k = lockid(type, storeid, key);
  // Hold lock_mu for the table and the waits-for lists.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // However it returns, it no longer waits for k: still under lock_mu.
  defer { s->waiting_lock = false; };
  // Time the deadlock check after the first, made as the wait begins.
  int64_t deadline = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
  bool checked = false;
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      s->waiting_for.clear();
      return -1;
    }
    size_t mine;
    lmode_t want;
    // Wait on exactly the holders that conflict.
    conflicting(db, db->lock_table.find(k), s, mode, &mine, &want,
                &s->waiting_for);
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
      // What it waits for, for lock_status and the cycle search.
      s->waiting_lock = true;
      s->waiting_type = type;
      s->waiting_storeid = storeid;
      s->waiting_row = key;
      s->waiting_id = k;
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

/* The sessions s waits for now; see session.h. */
void lock_blockers(db_t *db, sm1::session_t *s,
                   std::vector<sm1::session_t *> *out) {
  out->clear();
  // A lock: the holders now in the table whose modes conflict, the
  // weak ones in slots moved there by the strong lock it waits for.
  if (s->waiting_lock) {
    size_t mine;
    lmode_t want;
    conflicting(db, db->lock_table.find(s->waiting_id), s, s->waiting_mode,
                &mine, &want, out);
    return;
  }
  // A transaction, still running and not rolled back since: its owner.
  if (0 != s->waiting_xid && db->rollback_epoch == s->waiting_epoch) {
    // Hold txn_mu while reading the active table.
    std::lock_guard<std::mutex> t(db->txn_mu);
    auto it = db->xid_owner.find(s->waiting_xid);
    if (db->active.count(s->waiting_xid) && it != db->xid_owner.end() &&
        it->second != s) {
      out->push_back(it->second);
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
  // What we wait for, for the cycle search; however it returns,
  // still under lock_mu, we no longer do.
  s->waiting_xid = xid;
  s->waiting_epoch = epoch;
  defer { s->waiting_xid = 0; };
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
  // Hold our fp_mu: our slots go to the table first, g being in
  // no sweep's list.
  std::lock_guard<std::mutex> fp(s->fp_mu);
  for (int i = 0; s->fp_used > 0 && i < sm1::session_t::fp_slots; i++) {
    if (s->fp[i].used) {
      fp_move(db, s, i);
    }
  }
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-point each entry's holder list at g.
  s->locks.each([&](const lockref_t &l) {
    auto it = db->lock_table.find(l.id);
    // Replace s with g among the holders.
    if (it != db->lock_table.end()) {
      for (holder_t &h : it->second.holders) {
        if (h.session == s) {
          h.session = g;
        }
      }
    }
  });
  // Move the list with them, and its keys: a holder's first and an
  // entry's row point into them.
  g->locks.swap(s->locks);
  g->lock_keys.swap(s->lock_keys);
  // Waiters re-read their holders.
  db->lock_cv.notify_all();
}

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm1::session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-take each lock in the mode the record carries.
  g->locks.each([&](lockref_t &l) {
    // A strong OID lock keeps the fast path off its key, as when
    // it was taken.
    if (sm1::LOCKTYPE_OID == l.type && !is_weak(l.mode)) {
      strong_count(db, l.id).fetch_add(1);
      l.strong = true;
    }
    std::vector<holder_t> &holders = db->lock_table[l.id].holders;
    size_t mine = holder_of(holders, g);
    // A second row key hashed alike joins the first's hold.
    if (no_ref != mine) {
      holders[mine].refs++;
      holders[mine].mode = cover(holders[mine].mode, l.mode);
    } else {
      holders.push_back({g, l.mode, 1, &l});
    }
  });
}

/* Append a lock's name; see session.h. */
void lock_put_name(std::string *out, const lockref_t &l) {
  // The type, then the storeid, then the userkey.
  out->push_back(static_cast<char>(l.type));
  sm0::put_be64(*out, l.id.hi ^ type_bit(l.type));
  // An OID lock's key is its oid, big-endian.
  if (sm1::LOCKTYPE_OID == l.type) {
    sm0::put_be64(*out, l.id.lo);
    return;
  }
  out->append(l.row, l.row_len);
}

/* The length of a lock's name; see session.h. */
size_t lock_name_size(const lockref_t &l) {
  return 1 + 8 + (sm1::LOCKTYPE_OID == l.type ? 8 : l.row_len);
}

/* The entry a name stands for; see session.h. */
bool lock_from_name(std::string_view name, sm1::session_t *g, lockref_t *l) {
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
  *l = make_ref(g, lockid(t, sm0::get_be64(name.data() + 1), key), t, key,
                l->mode);
  return true;
}

/* Whether the session holds (storeid, key), and in which mode. */
bool lock_held(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
               sm1::lockmode_t *mode) {
  lockref_t *l = find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  // Not held.
  if (nullptr == l) {
    return false;
  }
  *mode = l->mode;
  return true;
}

/* Mark (storeid, key)'s exclusive lock: the next write may go ahead. */
void lock_set_latest(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  lockref_t *l = find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  // Count a mark once.
  if (nullptr != l && !l->latest) {
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
  lockref_t *l = find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  return nullptr != l && l->latest;
}

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1::session_t *s, uint64_t storeid,
                       rocksdb::Slice key) {
  // No mark anywhere: skip the scan, so a plain write pays nothing.
  if (s->latest_marks == 0) {
    return;
  }
  lockref_t *l = find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
  if (nullptr != l && l->latest) {
    l->latest = false;
    s->latest_marks--;
  }
}

/* Drop every lock the session holds and wake the waiters. */
void lock_release_all(sm1::session_t *s) {
  // Release everything, from the first lock on.
  lock_release_from(s, lock_mark_t{});
}

/* Drop the locks taken after m and wake the waiters. */
void lock_release_from(sm1::session_t *s, const lock_mark_t &m) {
  db_t *db = s->db;
  // Hold our fp_mu: a sweep may be moving our slots.
  std::lock_guard<std::mutex> fp(s->fp_mu);
  // The table is ours to edit only for a lock in it, or to stop a
  // wait; a lock in a slot is released without it.
  bool table = !s->waiting_for.empty() ||
               nullptr != s->locks.find_from(m.locks, [](const lockref_t &l) {
                 return l.slot < 0;
               });
  std::unique_lock<std::mutex> guard(db->lock_mu, std::defer_lock);
  if (table) {
    // Hold lock_mu while editing the table.
    guard.lock();
    // We no longer wait on anyone.
    s->waiting_for.clear();
  }
  // Drop our hold on every lock past the mark.
  s->locks.each_from(m.locks, [&](const lockref_t &l) {
    if (l.slot >= 0) {
      s->fp[l.slot].used = false;
      s->fp_used--;
    } else {
      lock_drop_holder(db, l.id, s);
      // Only once the hold is gone may a weak lock take a slot.
      if (l.strong) {
        strong_count(db, l.id).fetch_sub(1);
      }
    }
    // A mark goes with its lock.
    if (l.latest) {
      s->latest_marks--;
    }
  });
  // Forget them in the session's list, and their row keys, keeping
  // the blocks for the locks to come.
  s->locks.cut(m.locks);
  s->lock_keys.cut(m.keys);
  // Wake the waiters to retry: no one waits on a slot.
  if (table) {
    db->lock_cv.notify_all();
  }
}

} // namespace wisr
