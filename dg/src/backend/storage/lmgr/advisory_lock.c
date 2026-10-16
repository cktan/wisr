/*-------------------------------------------------------------------------
 *
 * advisory_lock.c
 *	  PostgreSQL's advisory locks, in a table of digres's own.
 *
 * In place of the lock manager's user lock method (dg/SHMEM.md section 3):
 * a mutex, a condition variable its waiters sleep on, the holders of each
 * key, and what each waiting session waits for.  A session keeps the keys
 * it holds in its tcb, so a transaction's end that holds none takes no
 * mutex.  A request is granted when no other session holds the key in a
 * conflicting mode: share conflicts with exclusive, exclusive with both.
 * There is no queue, so a stream of share holds can keep an exclusive
 * request waiting.
 *
 * A session about to wait looks for a cycle of advisory lock waits back to
 * itself, and if there is one fails with a deadlock.  Its wait on sm's
 * locks is not seen here, nor sm's of it: a cycle through both is broken
 * by lock_timeout, or a cancel.  sm1's wait-for graph could take an edge
 * of ours to find it (sm/NEXT.md section 3).
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/advisory_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/advisory_lock.h"
#include "storage/proc.h"
#include "storage/session_registry.h"
#include "utils/control_block.h"

/* The modes, as an index. */
enum { SHARE = 0, EXCLUSIVE = 1 };

/* One session's holds of one key, by mode. */
typedef struct advisory_holder_t {
  session_ref_t session;          /* the holder */
  uint32 session_holds[2] = {};   /* its session-scope holds */
  std::vector<int> xact_holds[2]; /* the nest level of each transaction one */
} advisory_holder_t;

/* What a waiting session waits for. */
typedef struct advisory_wait_t {
  advisory_key_t key;
  int mode;
} advisory_wait_t;

/* Hash a key, for the table. */
struct advisory_key_hash_t {
  size_t operator()(const advisory_key_t &k) const {
    uint64 h = ((uint64)k.database << 32 | k.form) * 0x9e3779b97f4a7c15ULL;

    return h ^ ((uint64)k.key1 << 32 | k.key2);
  }
};

/* Compare keys, for the table. */
struct advisory_key_eq_t {
  bool operator()(const advisory_key_t &a, const advisory_key_t &b) const {
    return a.database == b.database && a.key1 == b.key1 && a.key2 == b.key2 &&
           a.form == b.form;
  }
};

/* The table: the pcb's. */
struct advisory_table_t {
  std::mutex mu;              /* guards locks and waits */
  std::condition_variable cv; /* a waiter sleeps on it; a release wakes all */
  std::unordered_map<advisory_key_t, std::vector<advisory_holder_t>,
                     advisory_key_hash_t, advisory_key_eq_t>
      locks; /* the holders of each key held, never empty */
  std::unordered_map<const session_record_t *, advisory_wait_t> waits;
};

/* A session's own: the keys it holds, which only it reads or writes. */
struct advisory_state_t {
  std::vector<advisory_key_t> keys;
};

/* Free the table: never, as the process ends first; for the deleter. */
void advisory_table_free_t::operator()(advisory_table_t *table) const {
  delete table;
}

/* Free a session's state, as its thread ends; for the deleter. */
void advisory_state_free_t::operator()(advisory_state_t *state) const {
  delete state;
}

/*
 * Make the table.  The postmaster, or a standalone backend, calls it
 * before any session thread starts; a session thread's call finds it made.
 */
void advisory_init(void) {
  if (pcb->advisory == nullptr)
    pcb->advisory.reset(new advisory_table_t());
}

/* The session's state, made at its first use. */
static advisory_state_t *state(void) {
  if (tcb->advisory == nullptr)
    tcb->advisory.reset(new advisory_state_t());
  return tcb->advisory.get();
}

/* Whether h holds its key in mode, at either scope. */
static bool holds(const advisory_holder_t &h, int mode) {
  return h.session_holds[mode] > 0 || !h.xact_holds[mode].empty();
}

/* Whether h, another session's holder, conflicts with a request in mode. */
static bool conflicts(const advisory_holder_t &h, int mode) {
  return holds(h, EXCLUSIVE) || (mode == EXCLUSIVE && holds(h, SHARE));
}

/* Whether a request of me for key in mode must wait; the table's mutex held. */
static bool blocked(advisory_table_t *t, const session_record_t *me,
                    const advisory_key_t &key, int mode) {
  auto it = t->locks.find(key);

  if (it == t->locks.end())
    return false;
  for (const advisory_holder_t &h : it->second)
    if (h.session.get() != me && conflicts(h, mode))
      return true;
  return false;
}

/*
 * Whether the waiting session s waits, through holders that wait in turn,
 * on me; seen holds the sessions already followed.  The mutex held.
 */
static bool waits_on(advisory_table_t *t, const session_record_t *s,
                     const session_record_t *me,
                     std::unordered_set<const session_record_t *> &seen) {
  auto w = t->waits.find(s);

  if (w == t->waits.end())
    return false;
  auto it = t->locks.find(w->second.key);
  if (it == t->locks.end())
    return false;
  for (const advisory_holder_t &h : it->second) {
    const session_record_t *b = h.session.get();

    if (b == s || !conflicts(h, w->second.mode))
      continue;
    if (b == me)
      return true;
    if (seen.insert(b).second && waits_on(t, b, me, seen))
      return true;
  }
  return false;
}

/* What a wait's predicate reads. */
typedef struct advisory_wait_arg_t {
  advisory_table_t *t;
  const session_record_t *me;
  const advisory_key_t *key;
  int mode;
} advisory_wait_arg_t;

/* A wait's predicate: the request need not wait any more. */
static bool unblocked(void *arg) {
  advisory_wait_arg_t *a = (advisory_wait_arg_t *)arg;

  return !blocked(a->t, a->me, *a->key, a->mode);
}

/* The session's holder among holders, or their end. */
static std::vector<advisory_holder_t>::iterator
mine(std::vector<advisory_holder_t> &holders) {
  return std::find_if(
      holders.begin(), holders.end(),
      [](const advisory_holder_t &h) { return h.session == tcb->session; });
}

/* Add a hold of key in mode to the session's holder, made if new. */
static void grant(advisory_table_t *t, const advisory_key_t &key, int mode,
                  bool session) {
  std::vector<advisory_holder_t> &holders = t->locks[key];
  auto h = mine(holders);

  if (h == holders.end()) {
    holders.push_back(advisory_holder_t{tcb->session, {}, {}});
    h = holders.end() - 1;
    state()->keys.push_back(key);
  }
  if (session)
    h->session_holds[mode]++;
  else
    h->xact_holds[mode].push_back(GetCurrentTransactionNestLevel());
}

/*
 * Take a hold of key, exclusive or share, at session scope or the
 * transaction's.  Unless dontwait, wait while another session holds it in
 * a conflicting mode, until it does not, an interrupt, lock_timeout, or a
 * deadlock among advisory lock waits, which raise an error.  Returns
 * whether the hold was taken: false only under dontwait.
 */
bool advisory_lock(const advisory_key_t &key, bool exclusive, bool session,
                   bool dontwait) {
  advisory_table_t *t = pcb->advisory.get();
  const session_record_t *me = tcb->session.get();
  int mode = exclusive ? EXCLUSIVE : SHARE;
  int64 deadline =
      LockTimeout > 0 ? sm_now_us() + (int64)LockTimeout * 1000 : SM_FOREVER;
  advisory_wait_arg_t arg = {t, me, &key, mode};

  Assert(me != nullptr);
  /* Hold the table's mutex to read and change the holders. */
  std::unique_lock<std::mutex> guard(t->mu);
  for (;;) {
    if (!blocked(t, me, key, mode)) {
      grant(t, key, mode, session);
      break;
    }
    if (dontwait)
      return false;

    std::unordered_set<const session_record_t *> seen;
    t->waits[me] = advisory_wait_t{key, mode};
    if (waits_on(t, me, me, seen)) {
      t->waits.erase(me);
      guard.unlock();
      ereport(
          ERROR,
          (errcode(ERRCODE_T_R_DEADLOCK_DETECTED), errmsg("deadlock detected"),
           errdetail("Process %d waits for advisory lock [%u,%u,%u,%u] "
                     "in a cycle of advisory lock waits.",
                     MyProcPid, key.database, key.key1, key.key2, key.form)));
    }
    /* Sleep until a release lets the request through, or an interrupt. */
    sm_wait_result_t r =
        sm_wait(&guard, &t->cv, unblocked, &arg, sm_wait_mask(), deadline);
    t->waits.erase(me);
    if (r == SM_WAIT_DONE)
      continue;
    guard.unlock();
    if (r == SM_WAIT_TIMED_OUT)
      ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                      errmsg("canceling statement due to lock timeout")));
    /* Act on the interrupt: an error, or nothing and wait again. */
    CHECK_FOR_INTERRUPTS();
    guard.lock();
  }
  return true;
}

/*
 * Drop holds of the session's, changing each holder of its with drop, then
 * forgetting the holders left with none, and waking the waiters.
 */
template <class F> static void release(F drop) {
  advisory_table_t *t = pcb->advisory.get();
  advisory_state_t *st = tcb->advisory.get();

  /* Nothing held: no mutex to take. */
  if (st == nullptr || st->keys.empty())
    return;
  {
    /* Hold the table's mutex to change the holders. */
    std::lock_guard<std::mutex> guard(t->mu);

    std::erase_if(st->keys, [&](const advisory_key_t &key) {
      std::vector<advisory_holder_t> &holders = t->locks.at(key);
      auto h = mine(holders);

      drop(*h);
      if (holds(*h, SHARE) || holds(*h, EXCLUSIVE))
        return false;
      holders.erase(h);
      if (holders.empty())
        t->locks.erase(key);
      return true;
    });
  }
  /* Wake the waiters to look again: outside the mutex, as they take it. */
  t->cv.notify_all();
}

/*
 * Drop one session-scope hold of key, exclusive or share.  Returns false,
 * with a warning, if the session holds none.
 */
bool advisory_unlock(const advisory_key_t &key, bool exclusive) {
  advisory_table_t *t = pcb->advisory.get();
  int mode = exclusive ? EXCLUSIVE : SHARE;
  bool held = false;

  {
    /* Hold the table's mutex to change the holders. */
    std::lock_guard<std::mutex> guard(t->mu);
    auto it = t->locks.find(key);

    if (it != t->locks.end()) {
      std::vector<advisory_holder_t> &holders = it->second;
      auto h = mine(holders);

      if (h != holders.end() && h->session_holds[mode] > 0) {
        held = true;
        h->session_holds[mode]--;
        if (!holds(*h, SHARE) && !holds(*h, EXCLUSIVE)) {
          holders.erase(h);
          if (holders.empty())
            t->locks.erase(it);
          std::erase_if(state()->keys, [&](const advisory_key_t &k) {
            return advisory_key_eq_t()(k, key);
          });
        }
      }
    }
  }
  if (!held) {
    elog(WARNING, "you don't own a lock of type %s",
         exclusive ? "ExclusiveLock" : "ShareLock");
    return false;
  }
  /* Wake the waiters to look again: outside the mutex, as they take it. */
  t->cv.notify_all();
  return true;
}

/* Drop every session-scope hold: pg_advisory_unlock_all(). */
void advisory_unlock_all(void) {
  release([](advisory_holder_t &h) {
    h.session_holds[SHARE] = h.session_holds[EXCLUSIVE] = 0;
  });
}

/* Drop every hold, at both scopes: as the session ends, or DISCARD ALL. */
void advisory_release_all(void) {
  release([](advisory_holder_t &h) { h = advisory_holder_t{h.session}; });
}

/* Drop every transaction-scope hold, as the transaction ends. */
void advisory_xact_end(void) {
  release([](advisory_holder_t &h) {
    h.xact_holds[SHARE].clear();
    h.xact_holds[EXCLUSIVE].clear();
  });
}

/*
 * As the current subtransaction ends: its transaction-scope holds are its
 * parent's if it commits, and dropped if it aborts.
 */
void advisory_subxact_end(bool commit) {
  int level = GetCurrentTransactionNestLevel();

  release([&](advisory_holder_t &h) {
    for (std::vector<int> &levels : h.xact_holds) {
      if (commit)
        std::replace_if(
            levels.begin(), levels.end(), [&](int l) { return l >= level; },
            level - 1);
      else
        std::erase_if(levels, [&](int l) { return l >= level; });
    }
  });
}

/* The pids of the sessions holding the advisory lock pid waits for. */
std::vector<int> advisory_blockers(int pid) {
  advisory_table_t *t = pcb->advisory.get();
  session_ref_t s = session_by_pid(pid);
  std::vector<int> out;

  if (s == nullptr)
    return out;
  /* Hold the table's mutex to read the waits and the holders. */
  std::lock_guard<std::mutex> guard(t->mu);
  auto w = t->waits.find(s.get());
  if (w == t->waits.end())
    return out;
  auto it = t->locks.find(w->second.key);
  if (it == t->locks.end())
    return out;
  for (const advisory_holder_t &h : it->second)
    if (h.session != s && conflicts(h, w->second.mode))
      out.push_back(h.session->pid);
  return out;
}

/* Every lock of the table: each mode each holder holds, and each wait. */
std::vector<advisory_status_t> advisory_status(void) {
  advisory_table_t *t = pcb->advisory.get();
  std::vector<advisory_status_t> out;

  /* Hold the table's mutex to read the holders and the waits. */
  std::lock_guard<std::mutex> guard(t->mu);
  for (const auto &[key, holders] : t->locks)
    for (const advisory_holder_t &h : holders)
      for (int mode = SHARE; mode <= EXCLUSIVE; mode++)
        if (holds(h, mode))
          out.push_back({key, h.session->pid, mode == EXCLUSIVE, true});
  for (const auto &[record, wait] : t->waits)
    out.push_back({wait.key, record->pid, wait.mode == EXCLUSIVE, false});
  return out;
}
