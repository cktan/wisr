/*
 * lockmgr.cc -- row locks in Gray's modes, and deadlock detection
 * (SM1_DESIGN.md §5.1).
 *
 * A lock is keyed exactly by (storeid, userkey) and held by the
 * sessions whose transactions took it, each in a mode of its own,
 * every pair of them compatible (sm1.h, lockmode_t). Writes take
 * it exclusive, sm1::lock takes any mode, and a plain read still
 * takes none. It is held until the transaction commits or
 * aborts.
 *
 * Deadlocks are found the way PostgreSQL's deadlock_timeout
 * finds them: a blocked transaction sleeps deadlock_ms, then
 * searches the waits-for graph from itself. If it can reach
 * itself, the youngest transaction on that cycle is doomed and
 * woken; every member of a cycle is blocked, so the victim is
 * always waiting somewhere in lock_acquire and fails from
 * there. A waiter for a shared lock waits on every holder at
 * once, so the search walks a graph, not a chain.
 */

#include "session.h"
#include "sm1_codec.h"

#include <chrono>
#include <set>

using namespace wisr;

namespace {

using lmode_t = sm1::lockmode_t;

/* A mode as an index into the tables below: IS, IX, S, SIX, X. */
int rank(lmode_t m) {
  switch (m) {
  case lmode_t::intent_shared:
    return 0;
  case lmode_t::intent_exclusive:
    return 1;
  case lmode_t::shared:
    return 2;
  case lmode_t::shared_intent_exclusive:
    return 3;
  case lmode_t::exclusive:
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
      {lmode_t::intent_shared, lmode_t::intent_exclusive, lmode_t::shared,
       lmode_t::shared_intent_exclusive, lmode_t::exclusive},
      {lmode_t::intent_exclusive, lmode_t::intent_exclusive,
       lmode_t::shared_intent_exclusive, lmode_t::shared_intent_exclusive,
       lmode_t::exclusive},
      {lmode_t::shared, lmode_t::shared_intent_exclusive, lmode_t::shared,
       lmode_t::shared_intent_exclusive, lmode_t::exclusive},
      {lmode_t::shared_intent_exclusive, lmode_t::shared_intent_exclusive,
       lmode_t::shared_intent_exclusive, lmode_t::shared_intent_exclusive,
       lmode_t::exclusive},
      {lmode_t::exclusive, lmode_t::exclusive, lmode_t::exclusive,
       lmode_t::exclusive, lmode_t::exclusive},
  };
  return sup[rank(a)][rank(b)];
}

/* The lock table key of (storeid, userkey). */
std::string lock_key(uint64_t storeid, rocksdb::Slice key) {
  std::string k;
  // Reserve room for the storeid prefix and the key.
  k.reserve(8 + key.size());
  // Prefix the storeid, so equal keys in different stores differ.
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
sm1::session_t::lockref_t *find_lockref(sm1::session_t *s, uint64_t storeid,
                                        rocksdb::Slice key) {
  std::string k = lock_key(storeid, key);
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
                std::set<sm1::session_t *> *seen,
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
  std::set<sm1::session_t *> seen;
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

} // namespace

namespace wisr {

/*
 * Block until (storeid, key) is ours in mode, checking for deadlock;
 * a qa_nowait session gets 1 instead of blocking.
 */
int lock_acquire(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
                 sm1::lockmode_t mode) {
  db_t *db = s->db;
  // Build the lock table key.
  std::string k = lock_key(storeid, key);
  // Hold lock_mu for the table and the waits-for lists.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // Time the first deadlock check.
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(db->deadlock_ms);
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      // Clear its wait edges before leaving.
      s->waiting_for.clear();
      return -1;
    }
    // Look up the lock's current holders.
    auto it = db->lock_table.find(k);
    // Free: take it in the mode asked.
    if (it == db->lock_table.end()) {
      // Record us as the only holder.
      db->lock_table[k].holders.push_back({s, mode});
      // Remember the lock for release at commit or abort.
      s->locks.push_back({std::move(k), mode});
      // We no longer wait on anyone.
      s->waiting_for.clear();
      return 0;
    }
    db_t::lock_ent_t &ent = it->second;
    // Our own hold, if any, and the mode we would hold after.
    db_t::lock_ent_t::holder_t *mine = nullptr;
    for (db_t::lock_ent_t::holder_t &h : ent.holders) {
      if (h.session == s) {
        mine = &h;
        break;
      }
    }
    lmode_t want = mine ? cover(mine->mode, mode) : mode;
    // Ours already, in a mode at least as strong: nothing to do.
    if (mine && want == mine->mode) {
      s->waiting_for.clear();
      return 0;
    }
    // Collect the other holders whose modes conflict with want.
    s->waiting_for.clear();
    for (const db_t::lock_ent_t::holder_t &h : ent.holders) {
      if (h.session != s && !compatible(h.mode, want)) {
        s->waiting_for.push_back(h.session);
      }
    }
    // None: convert our hold in place, or join the holders.
    if (s->waiting_for.empty()) {
      if (mine) {
        mine->mode = want;
        // The list must show the stronger mode from now on.
        note_mode(s, k, want);
      } else {
        ent.holders.push_back({s, want});
        s->locks.push_back({std::move(k), want});
      }
      return 0;
    }
    // A qa_nowait session is told, and waits on nobody.
    if (s->qa_nowait) {
      s->waiting_for.clear();
      return 1;
    }
    // Blocked, on exactly the holders that conflict: a converter
    // waits on the others, which is how two converters on one key
    // form a cycle. After deadlock_ms, look for one.
    if (db->lock_cv.wait_until(guard, deadline) == std::cv_status::timeout) {
      // Timed out: look for a cycle through us.
      check_deadlock(db, s);
      // Re-arm the timer for the next check.
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(db->deadlock_ms);
    }
  }
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
  sm1::session_t::lockref_t *l = find_lockref(s, storeid, key);
  // Not held.
  if (!l) {
    return false;
  }
  *mode = l->mode;
  return true;
}

/* Mark (storeid, key)'s exclusive lock: the next write may go ahead. */
void lock_set_latest(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  sm1::session_t::lockref_t *l = find_lockref(s, storeid, key);
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
  sm1::session_t::lockref_t *l = find_lockref(s, storeid, key);
  return l && l->latest;
}

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1::session_t *s, uint64_t storeid,
                       rocksdb::Slice key) {
  // No mark anywhere: skip the scan, so a plain write pays nothing.
  if (s->latest_marks == 0) {
    return;
  }
  sm1::session_t::lockref_t *l = find_lockref(s, storeid, key);
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
