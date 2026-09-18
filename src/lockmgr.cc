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

namespace
{

/* The lock table key of (relid, userkey). */
std::string lock_key(uint64_t relid, rocksdb::Slice key)
{
    std::string k;
    k.reserve(8 + key.size());
    put_be64(k, relid);
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
void check_deadlock(db_t* db, sm_session_t* s)
{
    std::set<sm_session_t*> seen;
    sm_session_t* victim = s;
    for (sm_session_t* cur = s->waiting_for; cur; cur = cur->waiting_for) {
        if (cur == s) {
            victim->doomed = SM_ERR_DEADLOCK;
            db->lock_cv.notify_all();
            return;
        }
        if (!seen.insert(cur).second) {
            return;
        }
        if (cur->begin_seq > victim->begin_seq) {
            victim = cur;
        }
    }
}

} // namespace

namespace wisr
{

/* Block until (relid, key) is free or ours, checking for deadlock. */
int lock_acquire(sm_session_t* s, uint64_t relid, rocksdb::Slice key)
{
    db_t* db = s->db;
    std::string k = lock_key(relid, key);
    std::unique_lock<std::mutex> guard(db->lock_mu);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(db->deadlock_ms);
    for (;;) {
        if (s->doomed) {
            s->waiting_for = nullptr;
            return -1;
        }
        auto it = db->lock_owners.find(k);
        if (it == db->lock_owners.end()) {
            db->lock_owners.emplace(k, s);
            s->locks.push_back(std::move(k));
            s->waiting_for = nullptr;
            return 0;
        }
        if (it->second == s) {
            s->waiting_for = nullptr;
            return 0; // already ours: nothing new to record
        }
        s->waiting_for = it->second;
        // Wait for a release; after deadlock_ms, look for a cycle.
        if (db->lock_cv.wait_until(guard, deadline) ==
            std::cv_status::timeout) {
            check_deadlock(db, s);
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(db->deadlock_ms);
        }
    }
}

/* Drop every lock the session holds and wake the waiters. */
void lock_release_all(sm_session_t* s)
{
    lock_release_from(s, 0);
}

/* Drop the locks taken after the first n and wake the waiters. */
void lock_release_from(sm_session_t* s, size_t n)
{
    db_t* db = s->db;
    std::lock_guard<std::mutex> guard(db->lock_mu);
    s->waiting_for = nullptr;
    if (s->locks.size() <= n) {
        return;
    }
    for (size_t i = n; i < s->locks.size(); i++) {
        db->lock_owners.erase(s->locks[i]);
    }
    s->locks.resize(n);
    db->lock_cv.notify_all();
}

} // namespace wisr
