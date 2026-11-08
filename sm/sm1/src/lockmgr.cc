/*
 * lockmgr.cc -- the lock table: locks in Gray's modes, the write
 * latches, and deadlock detection (../LOCKMGR.md §3.2).
 *
 * A lock is keyed by its id, made from (locktype, storeid, userkey)
 * (lockid_t) -- a row lock names a key in a store, an OID lock an
 * object in none -- and is held by the
 * sessions whose transactions took it, each in a mode of its own,
 * every pair of them compatible (sm1.h, lockmode_t). sm1::lock_row
 * and sm1::lock_oid take any mode, held until the transaction
 * commits or aborts, and a plain read takes none. A write takes
 * no lock: it checks and puts its key under a short latch, and its
 * uncommitted version stands for the lock, a second writer waiting for the
 * transaction that wrote it (../LOCKMGR.md).
 *
 * The table is in 16 partitions (locktab.h), each with a mutex
 * and a condition of its own: a lock call takes its partition's
 * mutex alone, and a release wakes the waiters of the partitions
 * it released in. A wait for a transaction takes xact_mu and
 * sleeps on xact_cv, which the transaction's end and every
 * rollback to a savepoint notify.
 *
 * Deadlocks are found as PostgreSQL finds them: a waiter that has
 * slept deadlock_ms, PostgreSQL's deadlock_timeout, searches the
 * waits-for graph from itself, and again each deadlock_ms after.
 * A wait to upgrade a lock it holds searches once as it begins, as
 * PostgreSQL's ProcSleep checks a waiter's early: two upgraders of
 * one lock need no timer to run out. The search holds every partition's mutex,
 * in order, then xact_mu (lock_all_t), so it sees every waiter and holder
 * still. If it can reach itself, the youngest transaction on that cycle is
 * doomed and woken; every member of a cycle is blocked, so the victim is always
 * waiting in lock_wait or lock_wait_xact and fails from there. A waiter for a
 * shared lock waits on every holder at once, so the search walks a graph, not a
 * chain, its edges read from the table as it goes (lock_blockers): a waiter's
 * own list goes stale as its holders release, before it wakes to look.
 */

#include "error.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"
#include "sm_sys.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace wisr;

namespace wisr {

std::atomic<bool> lock_test_rows_collide{false};

/* The row hash of key: the same in every build, as a record may keep it. */
uint64_t lock_row_hash(rocksdb::Slice key) {
  const char *p = key.data();
  size_t n = key.size();
  // The length goes in first, so a key and its zero-padded self differ.
  uint64_t h = n;
  // Each 8 bytes are read big-endian, so every host hashes alike.
  for (; n >= 8; p += 8, n -= 8) {
    h = lock_fmix64(h ^ sm0::get_be64(p));
  }
  uint64_t tail = 0;
  for (size_t i = 0; i < n; i++) {
    tail = tail << 8 | static_cast<unsigned char>(p[i]);
  }
  return lock_fmix64(h ^ tail);
}

} // namespace wisr

namespace {

using lmode_t = sm1::lockmode_t;

using holder_t = locktab_t::holder_t;
using entry_t = locktab_t::entry_t;

/* The top bit of a lock key's hi: set for an OID lock. */
uint64_t type_bit(sm1::locktype_t type) { return uint64_t(type) << 63; }

/* The lock id of an oid; see lockid_t. */
lockid_t oid_id(uint64_t oid) { return {type_bit(sm1::LOCKTYPE_OID), oid}; }

/* A lock list entry for the lock k, held in mode, with the caller's kind. */
lockref_t make_ref(const lockid_t &k, lmode_t mode, uint16_t kind) {
  lockref_t l;
  l.id = k;
  l.type = lock_type(k);
  l.mode = mode;
  l.kind = kind;
  l.part = static_cast<uint8_t>(locktab_t::part_of(k));
  return l;
}

/*
 * Release s's list entry l from the table: s's hold on l.id goes
 * with the entry that made it, and the table entry with its last
 * holder. The caller holds l's partition's mutex.
 */
void lock_drop_holder(db_t *db, const lockref_t &l, sm1::session_t *s) {
  db->lock_table.part(l.part).drop(l.id, s, &l);
}

/*
 * The session's lock list entry for (storeid, key), or nullptr, in
 * the list of its id's partition. The list is the session's own, so
 * no mutex is taken. The newest first: the lock in question was
 * likely just taken.
 */
lockref_t *find_lockref(sm1::session_t *s, uint64_t storeid,
                        rocksdb::Slice key) {
  // The id holds the type, so comparing ids is enough.
  lockid_t k = lock_row_id(storeid, key);
  lock_list_t &list = s->locks.part(locktab_t::part_of(k));
  return list.find([&](const lockref_t &l) { return l.id == k; });
}

/*
 * Search the waits-for graph from cur for a path back to s,
 * with every lock mutex held (lock_all_t). path holds the transactions between
 * s and cur, so a hit has the whole cycle to choose a victim from. Returns true
 * once a cycle through s is found.
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
 * Search the waits-for graph from s for a deadlock. The caller holds
 * no lock mutex.
 * 1. Take every lock mutex (lock_all_t).
 * 2. Search for a path from s back to s. Return if there is none. A
 *    path that ends, or a cycle that does not pass through s, is not
 *    s's to resolve: the members of that other cycle run their own
 *    check.
 * 3. Doom the youngest transaction on the cycle.
 * 4. Wake everyone, so the victim sees it.
 */
void check_deadlock(db_t *db, sm1::session_t *s) {
  // Hold every partition and xact_mu: the graph stays still.
  lock_all_t all(db);
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
  // Wake every waiter so the victim sees its doom, wherever it waits.
  for (std::condition_variable &cv : db->lock_cv) {
    cv.notify_all();
  }
  db->xact_cv.notify_all();
}

/*
 * The other holders of e that conflict with mode for s, into out,
 * with e's partition's mutex held; sets *mine to s's own hold among them, or
 * nullptr, and *want to the mode s would hold. Returns false if
 * there is no entry, e being nullptr.
 */
bool conflicting(entry_t *e, sm1::session_t *s, lmode_t mode, holder_t **mine,
                 lmode_t *want, std::vector<sm1::session_t *> *out) {
  out->clear();
  *mine = nullptr;
  *want = mode;
  if (nullptr == e) {
    return false;
  }
  // Our own hold, if any, the mode we would hold after, and whether
  // another's conflicts, in one walk; the others are named only then.
  if (locktab_t::probe(e, s, mode, mine, want)) {
    locktab_t::each_conflict(e, s, *want, [&](void *owner) {
      out->push_back(static_cast<sm1::session_t *>(owner));
    });
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
 * Sleep once on cv, with its mutex held in guard. Returns false if
 * interrupted: the bit stays pending, for the caller's caller to take.
 * 1. Sleep until woken, until a bit in the thread's wait mask is
 *    raised (sm_sys.h), or until *deadline_us.
 * 2. At the deadline, look for a cycle through s.
 * 3. Re-arm *deadline_us.
 */
bool wait_once(db_t *db, sm1::session_t *s, std::unique_lock<std::mutex> &guard,
               std::condition_variable *cv, int64_t *deadline_us) {
  bool slept = false;

  // An interrupt raised before this wait ends it at once.
  switch (sm_wait(&guard, cv, woke, &slept, sm_wait_mask(), *deadline_us)) {
  case SM_WAIT_DONE:
    return true;
  case SM_WAIT_INTERRUPTED:
    return false;
  case SM_WAIT_TIMED_OUT:
    // Timed out: look for a cycle through us, with every lock mutex,
    // taken in their order, so ours is let go first.
    guard.unlock();
    check_deadlock(db, s);
    guard.lock();
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
 * one s has there already. The caller holds s->fp_mu, and no
 * partition's mutex.
 */
void fp_move(db_t *db, sm1::session_t *s, int i) {
  sm1::session_t::fp_slot_t &f = s->fp[i];
  {
    locktab_t::part_t &p = db->lock_table.part(f.ref->part);
    // Hold the partition's mutex while editing it.
    std::lock_guard<std::mutex> guard(p.mu);
    // A hold of ours already was made before this slot: a slot for
    // k is moved to the table before a hold there is made, so the
    // hold's first entry stays the first, and is released last.
    p.join(f.id, s, f.mode, f.ref);
    // Its list entry is the table's from now on.
    f.ref->slot = -1;
  }
  f.used = false;
  s->fp_used--;
}

/*
 * Move every session's slot for k into the lock table, where a
 * strong locker, counted already, meets them (../LOCKMGR.md §3.2).
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
 * Take the weak OID lock k in mode by a slot of s's: false if it
 * must go to the table, with s's slot for k moved there first when
 * a strong lock hashes with it.
 */
bool fp_take(db_t *db, sm1::session_t *s, const lockid_t &k, lmode_t mode) {
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
    f.mode = lock_cover(f.mode, mode);
    f.ref->mode = f.mode;
    return true;
  }
  // Every slot taken: the table holds it.
  if (free < 0) {
    return false;
  }
  lockref_t l = make_ref(k, mode, 0);
  l.slot = static_cast<int8_t>(free);
  s->fp[free] = {k, mode, s->locks.part(l.part).emplace(l), true};
  s->fp_used++;
  return true;
}

/* s's first list entry for the OID lock k, or nullptr. */
lockref_t *first_ref(sm1::session_t *s, const lockid_t &k) {
  return s->locks.part(locktab_t::part_of(k)).find([&](const lockref_t &l) {
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

/* Take the lock k in mode unless another holder conflicts. */
int lock_try(sm1::session_t *s, const lockid_t &k, sm1::lockmode_t mode,
             uint16_t kind) {
  db_t *db = s->db;
  size_t pi = locktab_t::part_of(k);
  lock_list_t &list = s->locks.part(pi);
  // Room for the entry now, so none is allocated under the mutex.
  list.reserve();
  locktab_t::part_t &p = db->lock_table.part(pi);
  // Hold k's partition's mutex for its entry.
  std::lock_guard<std::mutex> guard(p.mu);
  entry_t *e = p.find(k);
  holder_t *mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  conflicting(e, s, mode, &mine, &want, &others);
  // Another holder conflicts: take nothing.
  if (!others.empty()) {
    return 2;
  }
  // Ours already, a row's key or another hashed alike: one id is
  // one hold, and its first entry shows the stronger mode from now on.
  if (nullptr != mine) {
    mine->mode = want;
    static_cast<lockref_t *>(mine->first)->mode = want;
    return 0;
  }
  // Remember the lock for release at commit or abort.
  p.add(e, k, s, want, list.emplace(make_ref(k, want, kind)));
  return 0;
}

/*
 * Take the OID lock (storeid, key) in mode; see session.h.
 *
 * The fast path (../LOCKMGR.md §3.2) keeps a table's weak locks,
 * IS and IX, out of the lock table and its mutexes. IS and IX never
 * conflict with each other, so only a strong lock, S, SIX or X,
 * must see them. A weak lock goes in a slot of the session's own,
 * under its fp_mu, unless a strong lock is held or asked on a key
 * that hashes with it: its count in fp_strong is not 0. A strong
 * locker raises that count first, then moves every session's slot
 * for its key into the table (fp_sweep), and locks as any lock does.
 *
 * Skipping the table: a SELECT on t1, with no DDL on t1.
 *   1. sm3 asks IS on t1's oid. fp_take takes our fp_mu, finds
 *      t1's count 0 and a slot free, fills it, and adds the entry
 *      to our lock list.
 *   2. The commit clears the slot under fp_mu alone, in
 *      lock_release_from: our locks all in slots, it takes no
 *      partition's mutex either.
 *
 * Not skipping it: an ALTER TABLE t1 while that SELECT runs.
 *   1. The ALTER asks X on t1's oid. It raises t1's count, then
 *      sweeps: under the SELECT session's fp_mu, it moves that
 *      session's IS slot into the table, as a holder.
 *   2. lock_try, under t1's partition, finds the IS, which conflicts with
 *      X, and lock_wait sleeps until the SELECT commits.
 *   3. A second SELECT asks IS on t1 meanwhile. fp_take sees the
 *      count and declines, and the IS goes through lock_try under
 *      the partition: granted, the ALTER not yet a holder, as sm1 keeps
 *      no queue.
 *   4. The ALTER commits, its count goes back to 0, and the next
 *      SELECT on t1 takes a slot again.
 *
 * Why no lock is missed: both sides meet at the weak session's
 * fp_mu. fp_take reads the count under it, and a sweep takes it
 * after raising the count. If fp_take ran first, the sweep finds
 * its slot; if after, it sees the count and goes to the table.
 */
int lock_oid(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode,
             bool nowait) {
  db_t *db = s->db;
  lockid_t k = oid_id(oid);
  bool counted = false;
  if (is_weak(mode)) {
    if (fp_take(db, s, k, mode)) {
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
    if (0 == lock_try(s, k, mode)) {
      rc = 0;
      break;
    }
    if (nowait) {
      rc = 1;
      break;
    }
    // Wait for the holders that conflict.
    int waited = lock_wait(s, k, mode);
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
  lockid_t k = lock_row_id(storeid, key);
  locktab_t::part_t &p = db->lock_table.part(k);
  // Hold k's partition's mutex for its entry.
  std::lock_guard<std::mutex> guard(p.mu);
  entry_t *e = p.find(k);
  holder_t *mine;
  lmode_t want;
  std::vector<sm1::session_t *> others;
  // No entry: the common write, which takes no lock.
  if (!conflicting(e, s, sm1::SM_LOCKMODE_EX, &mine, &want, &others)) {
    return 0;
  }
  if (!others.empty()) {
    return 2;
  }
  // A lock of our own on the key is held exclusive from now on,
  // as a write converts it (../LOCKMGR.md §3.2).
  if (nullptr != mine) {
    mine->mode = sm1::SM_LOCKMODE_EX;
    static_cast<lockref_t *>(mine->first)->mode = sm1::SM_LOCKMODE_EX;
  }
  return 0;
}

/* Pair a host's wait notifications across wakeups and every exit. */
class lock_wait_notice_t {
public:
  /* Keep the session's hook for the duration of this wait. */
  explicit lock_wait_notice_t(sm1::session_t *s)
      : m_fn(s->lock_wait_hook), m_arg(s->lock_wait_arg) {}

  /* End the notification after the wait's mutex has been released. */
  ~lock_wait_notice_t() {
    if (m_paired) {
      m_fn(m_arg, false);
    }
  }

  /* Whether the first conflict has already been reported. */
  bool started() const { return m_started; }

  /* Start the notification once, after a conflict requires a wait. */
  void start() {
    if (!m_started) {
      m_started = true;
      m_paired = nullptr != m_fn && m_fn(m_arg, true);
    }
  }

private:
  sm_lock_wait_fn m_fn;
  void *m_arg;
  bool m_started = false;
  bool m_paired = false;
};

/* Block until no other holder of (storeid, key) conflicts with mode. */
int lock_wait(sm1::session_t *s, const lockid_t &k, sm1::lockmode_t mode,
              uint16_t kind) {
  db_t *db = s->db;
  lock_wait_notice_t notice(s);
  size_t pi = locktab_t::part_of(k);
  locktab_t::part_t &p = db->lock_table.part(pi);
  // Hold k's partition's mutex for its entry and our wait.
  std::unique_lock<std::mutex> guard(p.mu);
  // However it returns, it no longer waits for k: still under the
  // partition's mutex.
  defer { s->waiting_lock = false; };
  // Look for a cycle once we have slept deadlock_ms.
  int64_t deadline = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
  bool searched = false;
  for (;;) {
    // A doomed transaction stops waiting and fails.
    if (s->doomed) {
      s->waiting_for.clear();
      return -1;
    }
    holder_t *mine;
    lmode_t want;
    // Wait on exactly the holders that conflict.
    entry_t *e = p.find(k);
    conflicting(e, s, mode, &mine, &want, &s->waiting_for);
    if (s->waiting_for.empty()) {
      return 0;
    }
    // A qa_nowait session is told, and waits on nobody.
    if (s->qa_nowait) {
      s->waiting_for.clear();
      return 1;
    }
    if (!notice.started()) {
      // Name no holder while unlocked: a cycle search must not see an
      // edge of a wait that has not begun. The next pass names them again.
      s->waiting_for.clear();
      // Timer cancellation may wait for a wakeup that needs this mutex.
      guard.unlock();
      notice.start();
      guard.lock();
      continue;
    }
    // What it waits for, for lock_status and the cycle search.
    if (!s->waiting_lock) {
      s->waiting_lock = true;
      s->waiting_id = k;
      // A write waits with no kind of its own: it reports the kind
      // of the lock it waits on, its first holder's.
      s->waiting_kind =
          0 != kind
              ? kind
              : static_cast<lockref_t *>(locktab_t::oldest(e).first)->kind;
      s->waiting_mode = mode;
    }
    // An upgrade may close a cycle with another of this lock's holders,
    // so it searches now, once: every lock mutex, taken in their order,
    // so ours is let go first. Then it looks again, as a victim or not.
    if (nullptr != mine && !searched) {
      searched = true;
      guard.unlock();
      check_deadlock(db, s);
      guard.lock();
      continue;
    }
    // Interrupted, before the wait or during it: stop waiting.
    if (!wait_once(db, s, guard, &db->lock_cv[pi], &deadline)) {
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
    holder_t *mine;
    lmode_t want;
    conflicting(db->lock_table.part(s->waiting_id).find(s->waiting_id), s,
                s->waiting_mode, &mine, &want, out);
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
  lock_wait_notice_t notice(s);
  // Hold xact_mu for our wait; every wake-up takes it.
  std::unique_lock<std::mutex> guard(db->xact_mu);
  // A rollback from now on may delete what we wait for.
  uint64_t epoch = db->rollback_epoch;
  // What we wait for, for the cycle search; however it returns,
  // still under xact_mu, we no longer do.
  s->waiting_xid = xid;
  s->waiting_epoch = epoch;
  defer { s->waiting_xid = 0; };
  // Look for a cycle once we have slept deadlock_ms.
  int64_t deadline = sm_now_us() + int64_t(deadlock_wait_ms(s)) * 1000;
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
    if (!notice.started()) {
      // Timer cancellation may wait for a wakeup that needs this mutex.
      guard.unlock();
      notice.start();
      guard.lock();
      continue;
    }
    // Our edge in the waits-for graph: to the transaction itself.
    s->waiting_for.clear();
    if (owner && owner != s) {
      s->waiting_for.push_back(owner);
    }
    // Interrupted, before the wait or during it: stop waiting.
    if (!wait_once(db, s, guard, &db->xact_cv, &deadline)) {
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

/* Wake every waiter for a transaction: one ended, or rolled back. */
void lock_wake(db_t *db, bool rollback) {
  // Hold xact_mu: a waiter reads the epoch and sleeps under it.
  std::lock_guard<std::mutex> guard(db->xact_mu);
  if (rollback) {
    db->rollback_epoch++;
  }
  db->xact_cv.notify_all();
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
  // Hold every lock mutex while the holders and the list move.
  lock_all_t all(db);
  // Re-point each entry's holder list at g.
  s->locks.each([&](const lockref_t &l) {
    // Replace s with g among the holders.
    db->lock_table.part(l.part).move_owner(l.id, s, g);
  });
  // Move the list with them: a holder's first points into it.
  g->locks.swap(s->locks);
  // Waiters re-read their holders.
  for (std::condition_variable &cv : db->lock_cv) {
    cv.notify_all();
  }
}

/* Put a restored transaction's locks back in the table (../DBMGR.md §9.1). */
void lock_restore(db_t *db, sm1::session_t *g) {
  // Hold every lock mutex while editing the table.
  lock_all_t all(db);
  // Re-take each lock in the mode the record carries.
  g->locks.each([&](lockref_t &l) {
    // A strong OID lock keeps the fast path off its key, as when
    // it was taken.
    if (sm1::LOCKTYPE_OID == l.type && !is_weak(l.mode)) {
      strong_count(db, l.id).fetch_add(1);
      l.strong = true;
    }
    // An OID lock's slot, moved to the table before the prepare,
    // joins the hold its first entry made.
    db->lock_table.part(l.part).join(l.id, g, l.mode, &l);
  });
}

/* A row's lock id; see session.h. */
lockid_t lock_row_id(uint64_t storeid, rocksdb::Slice key) {
  // A test makes every row's key hash alike.
  if (lock_test_rows_collide.load(std::memory_order_relaxed)) {
    return {storeid, 0};
  }
  return {storeid, lock_row_hash(key)};
}

/* A lock's type; see session.h. */
sm1::locktype_t lock_type(const lockid_t &k) {
  return static_cast<sm1::locktype_t>(k.hi >> 63);
}

/* A lock's storeid; see session.h. */
uint64_t lock_storeid(const lockid_t &k) {
  return k.hi ^ type_bit(lock_type(k));
}

/* Append a lock to a record; see session.h. */
void lock_put_record(std::string *out, const lockref_t &l) {
  // The type, the id's two words, the mode, then the kind.
  out->push_back(static_cast<char>(l.type));
  sm0::put_be64(*out, l.id.hi);
  sm0::put_be64(*out, l.id.lo);
  out->push_back(static_cast<char>(l.mode));
  out->push_back(static_cast<char>(l.kind >> 8));
  out->push_back(static_cast<char>(l.kind & 0xff));
}

/* The lock a record's entry at p stands for; see session.h. */
bool lock_from_record(const char *p, lockref_t *l) {
  uint8_t type = static_cast<uint8_t>(p[0]);
  lockid_t k{sm0::get_be64(p + 1), sm0::get_be64(p + 9)};
  uint8_t mode = static_cast<uint8_t>(p[17]);
  // A type there is, the one the id's top bit says, and a mode.
  if (type > sm1::LOCKTYPE_OID ||
      lock_type(k) != static_cast<sm1::locktype_t>(type) ||
      mode > sm1::SM_LOCKMODE_SIX) {
    return false;
  }
  uint16_t kind = static_cast<uint16_t>(static_cast<uint8_t>(p[18]) << 8 |
                                        static_cast<uint8_t>(p[19]));
  *l = make_ref(k, static_cast<sm1::lockmode_t>(mode), kind);
  return true;
}

/* Whether the session holds (storeid, key), and in which mode. */
bool lock_held(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key,
               sm1::lockmode_t *mode) {
  lockref_t *l = find_lockref(s, storeid, key);
  // Not held.
  if (nullptr == l) {
    return false;
  }
  *mode = l->mode;
  return true;
}

/* Mark (storeid, key)'s exclusive lock: the next write may go ahead. */
void lock_set_latest(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  lockref_t *l = find_lockref(s, storeid, key);
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
  lockref_t *l = find_lockref(s, storeid, key);
  return nullptr != l && l->latest;
}

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1::session_t *s, uint64_t storeid,
                       rocksdb::Slice key) {
  // No mark anywhere: skip the scan, so a plain write pays nothing.
  if (s->latest_marks == 0) {
    return;
  }
  lockref_t *l = find_lockref(s, storeid, key);
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

/*
 * Drop the locks taken after m and wake the waiters, as PostgreSQL's
 * LockReleaseAll does. For each partition, in their order:
 * 1. Walk our list for the partition, past the mark.
 * 2. Free each lock's slot, or drop its hold from the table. The
 *    partition's mutex is taken once, at the first hold, and not at
 *    all for a partition we hold nothing past the mark in.
 * 3. Wake the partition's waiters, if its mutex was taken.
 * 4. Cut the list back to the mark.
 */
void lock_release_from(sm1::session_t *s, const lock_mark_t &m) {
  db_t *db = s->db;
  // Hold our fp_mu: a sweep may be moving our slots.
  std::lock_guard<std::mutex> fp(s->fp_mu);
  // We no longer wait on anyone. Every wait empties the list as it
  // returns, so this is for one that did not; the driver reads it
  // under every lock mutex.
  if (!s->waiting_for.empty()) {
    lock_all_t all(db);
    s->waiting_for.clear();
  }
  for (size_t pi = 0; pi < locktab_t::parts; pi++) {
    lock_list_t &list = s->locks.part(pi);
    // The partition's mutex, taken at our first lock in its table: a
    // lock in a slot is released without it.
    std::unique_lock<std::mutex> guard(db->lock_table.part(pi).mu,
                                       std::defer_lock);
    list.each_from(m.at[pi], [&](const lockref_t &l) {
      if (l.slot >= 0) {
        s->fp[l.slot].used = false;
        s->fp_used--;
      } else {
        if (!guard.owns_lock()) {
          // Hold the partition's mutex while editing it.
          guard.lock();
        }
        lock_drop_holder(db, l, s);
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
    // Wake the partition's waiters to retry: no one waits on a slot.
    if (guard.owns_lock()) {
      db->lock_cv[pi].notify_all();
    }
    // Forget them, keeping the blocks for the locks to come.
    list.cut(m.at[pi]);
  }
}

/* Take every partition's mutex in order, then xact_mu; see session.h. */
lock_all_t::lock_all_t(db_t *db) : m_db(db) {
  for (size_t i = 0; i < locktab_t::parts; i++) {
    m_db->lock_table.part(i).mu.lock();
  }
  m_db->xact_mu.lock();
}

/* Let them go, in the reverse order. */
lock_all_t::~lock_all_t() {
  m_db->xact_mu.unlock();
  for (size_t i = locktab_t::parts; i-- > 0;) {
    m_db->lock_table.part(i).mu.unlock();
  }
}

} // namespace wisr
