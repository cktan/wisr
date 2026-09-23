/*
 * lockmgr.cc -- shared and exclusive row locks and deadlock
 * detection (DESIGN.md §5.1).
 *
 * A lock is keyed exactly by (storeid, userkey) and held by the
 * sessions whose transactions took it: SHARED by any number of
 * them, EXCLUSIVE by one. Writes take it exclusive, sm_lock
 * takes either, and a plain read still takes none. It is held
 * until the transaction commits or aborts.
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

#include "codec.h"
#include "session.h"

#include <chrono>
#include <set>

using namespace wisr;

namespace {

/* The lock table key of (storeid, userkey). */
std::string lock_key(uint64_t storeid, rocksdb::Slice key) {
  std::string k;
  // Reserve room for the storeid prefix and the key.
  k.reserve(8 + key.size());
  // Prefix the storeid, so equal keys in different stores differ.
  put_be64(k, storeid);
  // Append the userkey.
  k.append(key.data(), key.size());
  return k;
}

/*
 * Remove s from the holders of k, dropping the entry once the
 * last holder goes. The caller holds lock_mu.
 */
void lock_drop_holder(db_t *db, const std::string &k, sm_session_t *s) {
  auto it = db->lock_table.find(k);
  // Gone already: a rollback may have released it.
  if (it == db->lock_table.end()) {
    return;
  }
  std::vector<sm_session_t *> &holders = it->second.holders;
  // Drop us from the holder list.
  for (size_t i = 0; i < holders.size(); i++) {
    if (holders[i] == s) {
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
sm_session_t::lockref_t *find_lockref(sm_session_t *s, uint64_t storeid,
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
 * Record a mode upgrade in the session's lock list, so release
 * and the prepare record carry the mode actually held. Only an
 * upgrade scans; the common write, which holds nothing on the
 * key yet, appends instead.
 */
void note_upgrade(sm_session_t *s, const std::string &k) {
  // Newest first: a key just upgraded was likely just taken.
  for (size_t i = s->locks.size(); i-- > 0;) {
    if (s->locks[i].key == k) {
      s->locks[i].exclusive = true;
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
bool find_cycle(sm_session_t *s, sm_session_t *cur,
                std::set<sm_session_t *> *seen,
                std::vector<sm_session_t *> *path) {
  // Follow every holder this transaction waits on.
  for (sm_session_t *next : cur->waiting_for) {
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
void check_deadlock(db_t *db, sm_session_t *s) {
  std::set<sm_session_t *> seen;
  std::vector<sm_session_t *> path;
  // No cycle through us: nothing to resolve here.
  if (!find_cycle(s, s, &seen, &path)) {
    return;
  }
  // The youngest transaction on the cycle is the victim.
  sm_session_t *victim = s;
  for (sm_session_t *m : path) {
    // Younger: began later.
    if (m->begin_seq > victim->begin_seq) {
      victim = m;
    }
  }
  // Doom it.
  victim->doomed = SM_ERR_DEADLOCK;
  // Wake every waiter so the victim sees its doom.
  db->lock_cv.notify_all();
}

} // namespace

namespace wisr {

/* Block until (storeid, key) is ours in mode, checking for deadlock. */
int lock_acquire(sm_session_t *s, uint64_t storeid, rocksdb::Slice key,
                 bool exclusive) {
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
      db_t::lock_ent_t &ent = db->lock_table[k];
      ent.exclusive = exclusive;
      ent.holders.push_back(s);
      // Remember the lock for release at commit or abort.
      s->locks.push_back({std::move(k), exclusive});
      // We no longer wait on anyone.
      s->waiting_for.clear();
      return 0;
    }
    db_t::lock_ent_t &ent = it->second;
    // Are we already a holder?
    bool ours = false;
    for (sm_session_t *h : ent.holders) {
      if (h == s) {
        ours = true;
        break;
      }
    }
    // Ours already, in a mode at least as strong: nothing to do.
    if (ours && (ent.exclusive || !exclusive)) {
      s->waiting_for.clear();
      return 0;
    }
    // Ours shared, and we are the only holder: upgrade in place.
    if (ours && ent.holders.size() == 1) {
      ent.exclusive = true;
      // The list must show the stronger mode from now on.
      note_upgrade(s, k);
      s->waiting_for.clear();
      return 0;
    }
    // Compatible with the holders: join them. Shared and shared
    // is the only pair that shares a lock.
    if (!ours && !exclusive && !ent.exclusive) {
      ent.holders.push_back(s);
      s->locks.push_back({std::move(k), false});
      s->waiting_for.clear();
      return 0;
    }
    // Blocked: we wait on every holder but ourselves. An
    // upgrade waits on the other shared holders, which is how
    // two upgraders on one key form a cycle.
    s->waiting_for.clear();
    for (sm_session_t *h : ent.holders) {
      if (h != s) {
        s->waiting_for.push_back(h);
      }
    }
    // Wait for a release; after deadlock_ms, look for a cycle.
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
void lock_transfer(db_t *db, sm_session_t *s, sm_session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-point each entry's holder list at g.
  for (const sm_session_t::lockref_t &l : s->locks) {
    auto it = db->lock_table.find(l.key);
    // Replace s with g among the holders.
    if (it != db->lock_table.end()) {
      for (sm_session_t *&h : it->second.holders) {
        if (h == s) {
          h = g;
        }
      }
    }
  }
}

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm_session_t *g) {
  // Hold lock_mu while editing the table.
  std::lock_guard<std::mutex> guard(db->lock_mu);
  // Re-take each lock in the mode the record carries.
  for (const sm_session_t::lockref_t &l : g->locks) {
    db_t::lock_ent_t &ent = db->lock_table[l.key];
    // An exclusive lock is the only one its entry can hold.
    ent.exclusive = l.exclusive;
    ent.holders.push_back(g);
  }
}

/* The mode the session holds (storeid, key) in: 0, 1 shared, 2 exclusive. */
int lock_mode_held(sm_session_t *s, uint64_t storeid, rocksdb::Slice key) {
  sm_session_t::lockref_t *l = find_lockref(s, storeid, key);
  // Not held.
  if (!l) {
    return 0;
  }
  return l->exclusive ? 2 : 1;
}

/* Mark (storeid, key)'s exclusive lock: the next write may go ahead. */
void lock_set_latest(sm_session_t *s, uint64_t storeid, rocksdb::Slice key) {
  sm_session_t::lockref_t *l = find_lockref(s, storeid, key);
  // Count a mark once.
  if (l && !l->latest) {
    l->latest = true;
    s->latest_marks++;
  }
}

/* Whether (storeid, key)'s lock carries the mark. */
bool lock_has_latest(sm_session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // No mark anywhere: skip the scan.
  if (s->latest_marks == 0) {
    return false;
  }
  sm_session_t::lockref_t *l = find_lockref(s, storeid, key);
  return l && l->latest;
}

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm_session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // No mark anywhere: skip the scan, so a plain write pays nothing.
  if (s->latest_marks == 0) {
    return;
  }
  sm_session_t::lockref_t *l = find_lockref(s, storeid, key);
  if (l && l->latest) {
    l->latest = false;
    s->latest_marks--;
  }
}

/* Drop every lock the session holds and wake the waiters. */
void lock_release_all(sm_session_t *s) {
  // Release everything, from the first lock on.
  lock_release_from(s, 0);
}

/* Drop the locks taken after the first n and wake the waiters. */
void lock_release_from(sm_session_t *s, size_t n) {
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
