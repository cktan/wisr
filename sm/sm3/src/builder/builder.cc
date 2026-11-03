/*
 * builder.cc -- the builder (SM3_DESIGN.md §3): one queue of
 * pending indexes for the process, the workers that fill them a
 * batch at a time, and the wait for a build to end. It does not
 * look for work: it is given a table and an index, checks the
 * index is still pending, and fills it. It is no part of the
 * catalog, which it reads and changes through the catalog's own
 * calls.
 */

#include "../catalog/internal.h"
#include "sm_sys.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace {

using sm3::catalog_t;

/* The interrupt bit a detach raises in a worker (sm_sys.h). */
constexpr uint32_t stop_bit = 1;
using sm3::index_state_t;
using sm3::session_t;

/* A catalog attached to the builder. */
struct target_t {
  std::function<sm1::session_t *()> open;
  size_t batch_rows = 0;
  bool stop = false; /* detaching: its builds end */
  size_t busy = 0;   /* workers building one of its indexes */
};

/* A build to run: an index of a table of a catalog. */
struct task_t {
  catalog_t *cat;
  uint64_t table;
  uint64_t index;
};

using index_key_t = std::pair<const catalog_t *, uint64_t>;

/*
 * The builder. mu guards all of it. A commit queues an index while
 * it holds its catalog's mutex, so mu is taken after a catalog's
 * mutex and no call into a catalog is made while mu is held.
 */
struct builder_t {
  std::mutex mu;
  std::condition_variable cv; /* gen changed */
  uint64_t gen = 0;           /* counts what a waiter may wait for */
  size_t max_workers = 4;
  std::unordered_map<const catalog_t *, target_t> catalogs;
  std::deque<task_t> queue;
  std::set<index_key_t> active;   /* queued or being built */
  std::set<index_key_t> building; /* being built */
  /* Each running build's worker, for a detach to interrupt. */
  std::map<index_key_t, sm_thread_ref_t> workers;
  size_t running = 0; /* workers alive */
  std::vector<std::thread> threads;
  std::vector<std::thread::id> finished; /* workers to join */
};

builder_t g_builder;

/* Tell the waiters something changed. The caller holds mu. */
void changed(builder_t &b) {
  b.gen++;
  b.cv.notify_all();
}

/* Whether cat is detaching, or gone from the builder. */
bool stopping(const catalog_t *cat) {
  builder_t &b = g_builder;
  std::lock_guard lock(b.mu);
  auto it = b.catalogs.find(cat);
  return it == b.catalogs.end() || it->second.stop;
}

/*
 * Set the index's state in a transaction of ws's own. A failed index
 * is given why: the error its build met, that error's message, and
 * the rows of a duplicate key.
 * 1. Begin a READ COMMITTED transaction.
 * 2. Set the state and commit. Return on success.
 * 3. Abort. After a retryable error, start again at step 1; any
 *    other error gives up.
 */
void set_state(catalog_t *cat, session_t *ws, uint64_t table, uint64_t index,
               index_state_t state, sm1::errnum_t error = sm1::SM_OK,
               const std::string &reason = {}, const sm1::dupkey_t &dup = {}) {
  while (!stopping(cat)) {
    if (sm3::begin(ws, sm1::xctlevel_t::read_committed) != 0) {
      return;
    }
    // The table-oid lock IX, as a batch takes: no reader or writer
    // of the table is waited for, only DDL on it.
    if (sm3::set_index_state(ws, table, index, state, error, reason, dup) ==
            0 &&
        sm3::commit(ws) == 0) {
      return;
    }
    bool again = sm1::retryable() == 1;
    sm3::abort(ws);
    if (!again) {
      return;
    }
  }
}

/*
 * Build one index to its end, in batches from the first row on.
 * 1. Begin a transaction of its own for the batch.
 * 2. Open the table and fill one batch of the index. A table or an
 *    index that has gone, or is no longer pending, ends the build
 *    quietly.
 * 3. Commit the batch.
 * 4. If the batch meets an error that is not retryable -- two rows
 *    with one key, a key too long -- mark the index failed, with the
 *    error kept, and end. After a retryable one, take the batch again.
 * 5. Go on from step 1 with the next batch until the rows end.
 * 6. Mark the index valid.
 */
void build_one(catalog_t *cat, session_t *ws, uint64_t table, uint64_t index,
               size_t batch_rows) {
  std::string from;
  for (;;) {
    if (stopping(cat) || sm3::begin(ws, sm1::xctlevel_t::read_committed) != 0) {
      return;
    }
    // The table-oid lock IX, as any writer takes: DDL waits for the
    // batch, and the batch sees the table as DDL left it.
    sm3::table_t *t = nullptr;
    std::string next = from;
    bool done = false, pending = false;
    int rc = sm3::tbl_open(ws, table, sm1::SM_LOCKMODE_IX, t);
    bool opened = rc == 0;
    if (rc == 0) {
      for (const sm3::index_def_t &x : sm3::tbl_def(t).indexes) {
        pending |=
            x.oid == index &&
            sm3::cat_get_index_state(ws, index).state == index_state_t::pending;
      }
      if (pending) {
        rc = sm3::tbl_index_backfill(t, index, from, batch_rows, next, done);
      }
      sm3::tbl_close(t);
    }
    if (rc == 0 && !pending) {
      sm3::abort(ws);
      return;
    }
    // Commit the batch, which lets go of its rows.
    if (rc == 0) {
      rc = sm3::commit(ws);
    }
    if (rc != 0) {
      // Keep the error as it was met: the abort below may not.
      bool again = sm1::retryable() == 1;
      sm1::errnum_t error = sm1::errnum();
      std::string reason = sm1::errmsg();
      sm1::dupkey_t dup = sm1::errdup();
      sm3::abort(ws);
      if (again) {
        continue;
      }
      // Cut short by a detach, which interrupts a build's wait: the
      // index stays pending, for the next attach.
      if (error == sm1::SM_ERR_INTERRUPTED || stopping(cat)) {
        return;
      }
      // A table that would not open has gone; anything else is the
      // build's failure, kept for whoever asks why.
      if (opened) {
        set_state(cat, ws, table, index, index_state_t::failed, error, reason,
                  dup);
      }
      return;
    }
    from = next;
    if (done) {
      break;
    }
  }
  // In a transaction of its own, holding no row: marking the index
  // valid waits for every user of the table, who may want a row.
  set_state(cat, ws, table, index, index_state_t::valid);
}

/*
 * A worker: run builds from the queue until it is empty.
 * 1. Take the next build from the queue. If it is empty, end.
 * 2. Open a session of its own on the build's database.
 * 3. Run the build on it (build_one), then close the session.
 * 4. Start again at step 1.
 */
void work() {
  builder_t &b = g_builder;
  for (;;) {
    task_t task;
    std::function<sm1::session_t *()> open;
    size_t batch_rows;
    {
      std::lock_guard lock(b.mu);
      if (b.queue.empty()) {
        b.running--;
        b.finished.push_back(std::this_thread::get_id());
        break;
      }
      task = b.queue.front();
      b.queue.pop_front();
      // Queued, so attached: a detach drops what is queued for it.
      target_t &target = b.catalogs.at(task.cat);
      target.busy++;
      open = target.open;
      batch_rows = target.batch_rows;
      b.building.emplace(task.cat, task.index);
      // Known to a detach while the build runs, so that a wait on
      // another transaction does not hold the detach up.
      b.workers[{task.cat, task.index}] = sm_thread_self();
      changed(b);
    }
    // Without a session the build is given up, and whoever waits
    // for it is told no build is running.
    sm1::session_t *s = open();
    session_t *ws = nullptr;
    if (s && sm3::session_open(task.cat, s, ws) != 0) {
      ws = nullptr;
    }
    if (ws) {
      try {
        build_one(task.cat, ws, task.table, task.index, batch_rows);
      } catch (...) {
        // The worker has no caller to pass it to: give the build up.
        sm3::abort(ws);
      }
    }
    sm3::session_close(ws);
    if (s) {
      // The build's session ends with the build.
      sm1::closedb(s);
    }
    std::lock_guard lock(b.mu);
    // Unknown to a detach from here, so a stop it raised is for
    // this build alone: take it, lest it end the next one's waits.
    b.workers.erase({task.cat, task.index});
    sm_take(stop_bit);
    b.active.erase({task.cat, task.index});
    b.building.erase({task.cat, task.index});
    b.catalogs.at(task.cat).busy--;
    changed(b);
  }
}

/* Queue a build, and start a worker for it if one may; see sm3.h. */
void enqueue(catalog_t *cat, uint64_t table, uint64_t index) {
  builder_t &b = g_builder;
  std::lock_guard lock(b.mu);
  auto it = b.catalogs.find(cat);
  if (it == b.catalogs.end() || it->second.stop ||
      !b.active.emplace(cat, index).second) {
    return;
  }
  b.queue.push_back({cat, table, index});
  // Join the workers that have ended, so none is left unjoined.
  for (std::thread::id id : b.finished) {
    for (auto t = b.threads.begin(); t != b.threads.end(); ++t) {
      if (t->get_id() == id) {
        t->join();
        b.threads.erase(t);
        break;
      }
    }
  }
  b.finished.clear();
  if (b.running < b.max_workers) {
    b.running++;
    // A worker thread, alive until it finds the queue empty.
    b.threads.emplace_back(work);
  }
}

} // namespace

namespace sm3 {

/* Attach a catalog; see sm3.h. */
int builder_attach(catalog_t *cat, std::function<sm1::session_t *()> open,
                   size_t batch_rows) {
  if (!cat || !open || batch_rows == 0) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "builder: no catalog, no way to open a session, or "
                       "no rows");
  }
  {
    std::lock_guard lock(g_builder.mu);
    if (g_builder.catalogs.count(cat)) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "builder: attached already");
    }
    g_builder.catalogs[cat] = {std::move(open), batch_rows};
  }
  return 0;
}

/*
 * Detach a catalog and wait for its builds to end; see sm3.h.
 *
 * Its wait is NOT cut short by an interrupt, unlike every other wait in
 * sm (../../CODE.md §7): a detach that returned first would leave a
 * build using a catalog its caller is closing. It is bounded: each of
 * the catalog's builds is interrupted below, and ends after its batch.
 */
void builder_detach(catalog_t *cat) {
  builder_t &b = g_builder;
  std::vector<std::thread> threads;
  {
    std::unique_lock lock(b.mu);
    auto it = b.catalogs.find(cat);
    if (it == b.catalogs.end()) {
      return;
    }
    // Queue no more for it: a commit on its way in is turned away.
    it->second.stop = true;
    std::erase_if(b.queue, [&](const task_t &q) {
      if (q.cat != cat) {
        return false;
      }
      b.active.erase({q.cat, q.index});
      return true;
    });
    changed(b);
    // Each of its builds ends after the batch it is in. One that
    // waits on another transaction, a prepared one perhaps, is
    // interrupted; the bit stays until its worker takes it, so one
    // raised between two waits ends the next. Raised under mu, so
    // never after the worker took it: no worker waits on mu.
    for (const auto &[key, t] : b.workers) {
      if (key.first == cat) {
        sm_interrupt(t, stop_bit);
      }
    }
    b.cv.wait(lock, [&] { return b.catalogs.at(cat).busy == 0; });
    b.catalogs.erase(cat);
    // With no catalog left the queue is empty and every worker is
    // ending: join them, so none outlives the process's builder.
    if (b.catalogs.empty()) {
      threads.swap(b.threads);
      b.finished.clear();
    }
  }
  for (std::thread &t : threads) {
    t.join();
  }
}

/* Queue a pending index, never throwing; see sm3.h. */
void builder_enqueue(catalog_t *cat, uint64_t table, uint64_t index) {
  try {
    enqueue(cat, table, index);
  } catch (...) {
    // A worker that cannot start: the index stays pending, for the
    // next attach to find.
  }
}

/* Set the most builds at a time; see sm3.h. */
void builder_set_max_workers(size_t max_workers) {
  std::lock_guard lock(g_builder.mu);
  g_builder.max_workers = max_workers;
}

/* Whether an index is being built now; see sm3.h. */
bool builder_building(const catalog_t *cat, uint64_t oid) {
  std::lock_guard lock(g_builder.mu);
  return g_builder.building.count({cat, oid}) != 0;
}

/* Wait for a build to end; see sm3.h. */
int builder_wait(session_t *session, uint64_t table, uint64_t oid) {
  if (session->in_txn) {
    return sm1::seterr(sm1::SM_ERR_INTRANS, "index wait: inside a transaction");
  }
  const catalog_t *cat = session->cat;
  builder_t &b = g_builder;
  for (;;) {
    // The count first: a change after it wakes the wait below.
    uint64_t gen;
    bool active;
    {
      std::lock_guard lock(b.mu);
      gen = b.gen;
      active = b.active.count({cat, oid}) != 0;
    }
    std::shared_ptr<const table_def_t> d = cat_get_table(cat, table);
    const index_def_t *x = nullptr;
    if (d) {
      for (const index_def_t &i : d->indexes) {
        if (i.oid == oid) {
          x = &i;
        }
      }
    }
    if (!x) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "index wait: no index " + std::to_string(oid) +
                             " on table " + std::to_string(table));
    }
    index_status_t st = cat_get_index_state(cat, oid);
    if (st.state == index_state_t::valid) {
      return 0;
    }
    if (st.state == index_state_t::failed) {
      // The build's own error, as the index's state keeps it.
      if (st.error != sm1::SM_OK) {
        sm1::seterr(st.error, st.reason);
        // With the rows of a duplicate key, the index this one.
        if (!st.dup.row.empty()) {
          sm1::dupkey_t dup = st.dup;
          dup.index = oid;
          sm1::setdup(dup);
        }
        return -1;
      }
      return sm1::seterr(sm1::SM_ERR_INVALID, "index wait: the build failed");
    }
    std::unique_lock lock(b.mu);
    // An index is queued before it is published, so one seen
    // pending with no build has none coming.
    if (b.gen == gen && !active) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "index wait: no build of the index is running");
    }
    // Until something changes, or an interrupt to this thread
    // ends the wait; the bit stays, for the caller to take.
    std::pair<builder_t *, uint64_t> at{&b, gen};
    auto moved = [](void *arg) {
      auto *p = static_cast<std::pair<builder_t *, uint64_t> *>(arg);
      return p->first->gen != p->second;
    };
    if (sm_wait(&lock, &b.cv, moved, &at, sm_wait_mask(), SM_FOREVER) ==
        SM_WAIT_INTERRUPTED) {
      return sm1::seterr(sm1::SM_ERR_INTERRUPTED,
                         "index wait: the wait was interrupted");
    }
  }
}

} // namespace sm3
