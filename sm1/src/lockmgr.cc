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
 * Deadlocks are found the way PostgreSQL's deadlock_timeout
 * finds them: a blocked transaction sleeps deadlock_ms, then
 * searches the waits-for graph from itself. If it can reach
 * itself, the youngest transaction on that cycle is doomed and
 * woken; every member of a cycle is blocked, so the victim is
 * always waiting in lock_wait or lock_wait_xact and fails from
 * there. A waiter for a shared lock waits on every holder at
 * once, so the search walks a graph, not a chain.
 */

#include "session.h"
#include "sm1_codec.h"

#include <chrono>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace wisr;

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

/* The lock table key of (type, storeid, userkey). */
std::string lock_key(sm1::locktype_t type, uint64_t storeid,
                     rocksdb::Slice key) {
  std::string k;
  // Reserve room for the type byte, the storeid and the key.
  k.reserve(1 + 8 + key.size());
  // Lead with the type, so no two namespaces share a lock.
  k.push_back(static_cast<char>(type));
  // Then the storeid, so equal keys in different stores differ.
  sm1::put_be64(k, storeid);
  // Append the userkey.
  k.append(key.data(), key.size());
  return k;
}

/*
 * Remove s from the holders of k, dropping the entry once the
 * last holder goes. The caller holds lock_mu.
 */
void lock_drop_holder(db_t *db, const std::string &k, sm1::session_t *s) {
  auto it = db->lock_table.find(k);
  // Gone already: a rollback may have released it.
  if (it == db->lock_table.end()) {
    return;
  }
  std::vector<db_t::lock_ent_t::holder_t> &holders = it->second.holders;
  // Drop us from the holder list.
  for (size_t i = 0; i < holders.size(); i++) {
    if (holders[i].session == s) {
      holders.erase(holders.begin() + i);
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
sm1::session_t::lockref_t *find_lockref(sm1::session_t *s, sm1::locktype_t type,
                                        uint64_t storeid, rocksdb::Slice key) {
  std::string k = lock_key(type, storeid, key);
  for (size_t i = s->locks.size(); i-- > 0;) {
    if (s->locks[i].key == k) {
      return &s->locks[i];
    }
  }
  return nullptr;
}

/*
 * Record a conversion in the session's lock list, so release and
 * the prepare record carry the mode actually held. Only a
 * conversion scans; the common write, which holds nothing on the
 * key yet, appends instead.
 */
void note_mode(sm1::session_t *s, const std::string &k, lmode_t mode) {
  // Newest first: a key just converted was likely just taken.
  for (size_t i = s->locks.size(); i-- > 0;) {
    if (s->locks[i].key == k) {
      s->locks[i].mode = mode;
      return;
    }
  }
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
bool conflicting(db_t *db, sm1::session_t *s, const std::string &k,
                 lmode_t mode, db_t::lock_ent_t::holder_t **mine, lmode_t *want,
                 std::vector<sm1::session_t *> *out) {
  out->clear();
  *mine = nullptr;
  *want = mode;
  auto it = db->lock_table.find(k);
  if (it == db->lock_table.end()) {
    return false;
  }
  // Our own hold, if any, and the mode we would hold after.
  for (db_t::lock_ent_t::holder_t &h : it->second.holders) {
    if (h.session == s) {
      *mine = &h;
      break;
    }
  }
  *want = *mine ? cover((*mine)->mode, mode) : mode;
  // The other holders whose modes conflict with that.
  for (const db_t::lock_ent_t::holder_t &h : it->second.holders) {
    if (h.session != s && !compatible(h.mode, *want)) {
      out->push_back(h.session);
    }
  }
  return true;
}

/*
 * Sleep on lock_cv until woken or deadlock_ms after *deadline was
 * set, with lock_mu held in guard; on timeout, look for a cycle
 * through s and re-arm the timer.
 */
void wait_once(db_t *db, sm1::session_t *s, std::unique_lock<std::mutex> &guard,
               std::chrono::steady_clock::time_point *deadline) {
  if (db->lock_cv.wait_until(guard, *deadline) == std::cv_status::timeout) {
    // Timed out: look for a cycle through us.
    check_deadlock(db, s);
    // Re-arm the timer for the next check.
    *deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(db->deadlock_ms);
  }
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
  std::string k = lock_key(type, storeid, key);
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  db_t::lock_ent_t::holder_t *mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  conflicting(db, s, k, mode, &mine, &want, &others);
  // Another holder conflicts: take nothing.
  if (!others.empty()) {
    return 2;
  }
  if (mine) {
    // Ours already, at least as strong: nothing to do.
    if (want != mine->mode) {
      mine->mode = want;
      // The list must show the stronger mode from now on.
      note_mode(s, k, want);
    }
    return 0;
  }
  // Join the holders, or make the entry.
  db->lock_table[k].holders.push_back({s, want});
  // Remember the lock for release at commit or abort.
  s->locks.push_back({std::move(k), want});
  return 0;
}

/* For a write: 2 if another holder conflicts, else convert our own. */
int lock_write_check(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  db_t *db = s->db;
  // A write meets row locks and no other lock type.
  std::string k = lock_key(sm1::LOCKTYPE_ROW, storeid, key);
  // Hold lock_mu for the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  db_t::lock_ent_t::holder_t *mine;
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
  if (mine && mine->mode != sm1::SM_LOCKMODE_EX) {
    mine->mode = sm1::SM_LOCKMODE_EX;
    note_mode(s, k, sm1::SM_LOCKMODE_EX);
  }
  return 0;
}

/* Block until no other holder of (storeid, key) conflicts with mode. */
int lock_wait(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
              rocksdb::Slice key, sm1::lockmode_t mode) {
  db_t *db = s->db;
  std::string k = lock_key(type, storeid, key);
  // Hold lock_mu for the table and the waits-for lists.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // Time the first deadlock check.
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(db->deadlock_ms);
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      s->waiting_for.clear();
      return -1;
    }
    db_t::lock_ent_t::holder_t *mine;
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
    wait_once(db, s, guard, &deadline);
  }
}

/* Block until xid ends, or rolls back to a savepoint. */
int lock_wait_xact(sm1::session_t *s, uint64_t xid) {
  db_t *db = s->db;
  // Hold lock_mu for the waits-for lists; every wake-up takes it.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // A rollback from now on may delete what we wait for.
  uint64_t epoch = db->rollback_epoch;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(db->deadlock_ms);
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
    wait_once(db, s, guard, &deadline);
  }
}

/* Wake every waiter: a transaction ended, or rolled back. */
void lock_wake(db_t *db, bool rollback) {
  std::lock_guard<std::mutex> guard(db->lock_mu);
  if (rollback) {
    db->rollback_epoch++;
  }
  db->lock_cv.notify_all();
}

/* Move every lock s holds to g, which takes its place. */
void lock_transfer(db_t *db, sm1::session_t *s, sm1::session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-point each entry's holder list at g.
  for (const sm1::session_t::lockref_t &l : s->locks) {
    auto it = db->lock_table.find(l.key);
    // Replace s with g among the holders.
    if (it != db->lock_table.end()) {
      for (db_t::lock_ent_t::holder_t &h : it->second.holders) {
        if (h.session == s) {
          h.session = g;
        }
      }
    }
  }
}

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm1::session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-take each lock in the mode the record carries.
  for (const sm1::session_t::lockref_t &l : g->locks) {
    db->lock_table[l.key].holders.push_back({g, l.mode});
  }
}

/* Whether the session holds (storeid, key), and in which mode. */
bool lock_held(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
               sm1::lockmode_t *mode) {
  sm1::session_t::lockref_t *l =
      find_lockref(s, sm1::LOCKTYPE_ROW, storeid, key);
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
