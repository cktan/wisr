/*
 * lockmgr.cc -- exclusive row locks and deadlock detection
 * (DESIGN.md §5.1).
 *
 * A lock is keyed exactly by (relid, userkey) and owned by the
 * session whose transaction took it; it is held until that
 * transaction commits or aborts. Readers never lock.
 *
 * Deadlocks are found the way PostgreSQL's deadlock_timeout
 * finds them: a blocked writer sleeps deadlock_ms, then walks
 * the waits-for chain from itself. If the chain returns to it,
 * the youngest transaction on the cycle is doomed and woken;
 * every member of a cycle is blocked, so the victim is always
 * waiting somewhere in lock_acquire and fails from there.
 */

#include "codec.h"
#include "session.h"

#include <chrono>
#include <set>

using namespace wisr;

namespace {

/* The lock table key of (relid, userkey). */
std::string lock_key(uint64_t relid, rocksdb::Slice key) {
  std::string k;
  // Reserve room for the relid prefix and the key.
  k.reserve(8 + key.size());
  // Prefix the relid, so equal keys in different relations differ.
  put_be64(k, relid);
  // Append the userkey.
  k.append(key.data(), key.size());
  return k;
}

/*
 * Walk the waits-for chain from s, with lock_mu held. If it
 * leads back to s, doom the youngest transaction on the cycle
 * and wake everyone so the victim sees it. A chain that ends,
 * or loops without reaching s, is not s's to resolve: the
 * members of that other cycle run their own check.
 */
void check_deadlock(db_t *db, sm_session_t *s) {
  std::set<sm_session_t *> seen;
  sm_session_t *victim = s;
  // Follow each blocked transaction to the holder it waits on.
  for (sm_session_t *cur = s->waiting_for; cur; cur = cur->waiting_for) {
    // Back at s: the chain is a cycle through s.
    if (cur == s) {
      // Doom the youngest member as the victim.
      victim->doomed = SM_ERR_DEADLOCK;
      // Wake every waiter so the victim sees its doom.
      db->lock_cv.notify_all();
      return;
    }
    // Stop at a cycle that does not pass through s.
    if (!seen.insert(cur).second) {
      return;
    }
    // Track the youngest member seen so far.
    if (cur->begin_seq > victim->begin_seq) {
      victim = cur;
    }
  }
}

} // namespace

namespace wisr {

/* Block until (relid, key) is free or ours, checking for deadlock. */
int lock_acquire(sm_session_t *s, uint64_t relid, rocksdb::Slice key) {
  db_t *db = s->db;
  // Build the lock table key.
  std::string k = lock_key(relid, key);
  // Hold lock_mu for the table and the waits-for pointers.
  std::unique_lock<std::mutex> guard(db->lock_mu);
  // Time the first deadlock check.
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(db->deadlock_ms);
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      // Clear its wait edge before leaving.
      s->waiting_for = nullptr;
      return -1;
    }
    // Look up the lock's current holder.
    auto it = db->lock_owners.find(k);
    // Free: take it.
    if (it == db->lock_owners.end()) {
      // Record us as the owner.
      db->lock_owners.emplace(k, s);
      // Remember the lock for release at commit or abort.
      s->locks.push_back(std::move(k));
      // We no longer wait on anyone.
      s->waiting_for = nullptr;
      return 0;
    }
    // Already ours: re-acquiring changes nothing.
    if (it->second == s) {
      // We no longer wait on anyone.
      s->waiting_for = nullptr;
      return 0; // already ours: nothing new to record
    }
    // Record the wait edge for deadlock detection.
    s->waiting_for = it->second;
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
  s->waiting_for = nullptr;
  // Nothing past n to release.
  if (s->locks.size() <= n) {
    return;
  }
  // Drop ownership of every lock past the first n.
  for (size_t i = n; i < s->locks.size(); i++) {
    db->lock_owners.erase(s->locks[i]);
  }
  // Forget them in the session's list.
  s->locks.resize(n);
  // Wake the waiters to retry.
  db->lock_cv.notify_all();
}

} // namespace wisr
