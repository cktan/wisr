/*
 * build.cc -- the catalog's builder (SM3_DESIGN.md §3): a queue of
 * indexes to build, the workers that build them a batch at a time,
 * and the wait for a build to end.
 */

#include "internal.h"

namespace {

using sm3::builder_t;
using sm3::catalog_t;
using sm3::index_state_t;
using sm3::session_t;

/* Whether the builder has been told to stop. */
bool stopping(catalog_t *cat) {
  std::lock_guard lock(cat->builder.mu);
  return cat->builder.stop;
}

/*
 * Set the index's state in a transaction of ws's own, again after
 * a retryable error; any other error gives up.
 */
void set_state(catalog_t *cat, session_t *ws, uint64_t table, uint64_t index,
               index_state_t state) {
  while (!stopping(cat)) {
    if (sm3::begin(ws, sm1::xctlevel_t::read_committed) != 0) {
      return;
    }
    // The schema lock X: this waits out every user of the table.
    if (sm3::cat_set_index_state(ws, table, index, state) == 0 &&
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
 * Build one index to its end: batches from the first row on, each
 * a transaction of its own, then the index marked valid; or marked
 * failed, on two rows with one key. A table or an index that has
 * gone, or is no longer building, ends the build quietly.
 */
void build_one(catalog_t *cat, session_t *ws, uint64_t table, uint64_t index) {
  uint64_t from = 0;
  for (;;) {
    if (stopping(cat) || sm3::begin(ws, sm1::xctlevel_t::read_committed) != 0) {
      return;
    }
    // The schema lock IX, as any writer takes: DDL waits for the
    // batch, and the batch sees the table as DDL left it.
    sm3::table_t *t = nullptr;
    uint64_t next = from;
    bool done = false, building = false;
    int rc = sm3::tbl_open(ws, table, sm1::lockmode_t::intent_exclusive, t);
    if (rc == 0) {
      for (const sm3::index_def_t &x : sm3::tbl_def(t).indexes) {
        building |= x.oid == index && x.state == index_state_t::building;
      }
      if (building) {
        rc = sm3::tbl_index_backfill(t, index, from, cat->builder.batch_rows,
                                     next, done);
      }
      sm3::tbl_close(t);
    }
    if (rc == 0 && !building) {
      sm3::abort(ws);
      return;
    }
    // Commit the batch, which lets go of its rows.
    if (rc == 0) {
      rc = sm3::commit(ws);
    }
    if (rc != 0) {
      bool again = sm1::retryable() == 1;
      bool duplicate = sm1::errnum() == sm1::SM_ERR_DUPKEY;
      sm3::abort(ws);
      if (again) {
        continue;
      }
      if (duplicate) {
        set_state(cat, ws, table, index, index_state_t::failed);
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
 * A worker: take builds from the queue until it is empty, each
 * with this thread's own session, then end.
 */
void work(catalog_t *cat) {
  builder_t &b = cat->builder;
  sm1::session_t *s = b.open();
  session_t *ws = nullptr;
  if (s && sm3::session_open(cat, s, ws) != 0) {
    ws = nullptr;
  }
  for (;;) {
    std::pair<uint64_t, uint64_t> task;
    {
      std::unique_lock lock(b.mu);
      // No session, nothing queued, or told to stop: end. What is
      // still queued without a session can never be built.
      if (!ws || b.stop || b.queue.empty()) {
        if (!ws) {
          for (const auto &q : b.queue) {
            b.active.erase(q.second);
          }
          b.queue.clear();
        }
        b.running--;
        b.finished.push_back(std::this_thread::get_id());
        b.cv.notify_all();
        break;
      }
      task = b.queue.front();
      b.queue.pop_front();
    }
    try {
      build_one(cat, ws, task.first, task.second);
    } catch (...) {
      // Out of memory, most likely: give the build up, and whoever
      // waits for it is told no build is running.
      sm3::abort(ws);
    }
    std::lock_guard lock(b.mu);
    b.active.erase(task.second);
    b.cv.notify_all();
  }
  sm3::session_close(ws);
  if (s) {
    // The worker's session ends with the worker.
    sm1::closedb(s);
  }
}

/* The committed state of a table's index, or nullopt if there is none. */
std::optional<index_state_t> state_of(const catalog_t *cat, uint64_t table,
                                      uint64_t index) {
  std::shared_ptr<const sm3::table_def_t> d = sm3::cat_table(cat, table);
  if (d) {
    for (const sm3::index_def_t &x : d->indexes) {
      if (x.oid == index) {
        return x.state;
      }
    }
  }
  return std::nullopt;
}

} // namespace

namespace sm3 {

/* Queue a build, and start a worker for it if one may; see internal.h. */
void build_enqueue(catalog_t *cat, uint64_t table, uint64_t index) {
  builder_t &b = cat->builder;
  std::lock_guard lock(b.mu);
  if (!b.open || b.stop || !b.active.insert(index).second) {
    return;
  }
  b.queue.emplace_back(table, index);
  // Join the workers that have ended, so none is left unjoined.
  for (std::thread::id id : b.finished) {
    for (auto it = b.threads.begin(); it != b.threads.end(); ++it) {
      if (it->get_id() == id) {
        it->join();
        b.threads.erase(it);
        break;
      }
    }
  }
  b.finished.clear();
  if (b.running < b.max_workers) {
    b.running++;
    // A worker thread, alive until it finds the queue empty.
    b.threads.emplace_back(work, cat);
  }
}

/* Wake the waiters; see internal.h. */
void build_notify(catalog_t *cat) {
  std::lock_guard lock(cat->builder.mu);
  cat->builder.cv.notify_all();
}

/* Stop the workers and wait for them; see internal.h. */
void build_stop(catalog_t *cat) {
  builder_t &b = cat->builder;
  std::vector<std::thread> threads;
  {
    std::lock_guard lock(b.mu);
    b.stop = true;
    threads.swap(b.threads);
    b.cv.notify_all();
  }
  // Each ends after the batch it is in.
  for (std::thread &t : threads) {
    t.join();
  }
}

/* Start the builder, and queue what a restart left; see sm3.h. */
int cat_builder_start(catalog_t *cat, std::function<sm1::session_t *()> open,
                      size_t max_workers, size_t batch_rows) {
  return catch_nomem([&] {
    if (!cat || !open || max_workers == 0 || batch_rows == 0) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "builder: no catalog, no way to open a session, or "
                         "no workers or rows");
    }
    std::vector<std::pair<uint64_t, uint64_t>> found;
    {
      std::shared_lock cl(cat->mu);
      std::lock_guard lock(cat->builder.mu);
      if (cat->builder.open) {
        return sm1::seterr(sm1::SM_ERR_INVALID, "builder: started already");
      }
      cat->builder.open = std::move(open);
      cat->builder.max_workers = max_workers;
      cat->builder.batch_rows = batch_rows;
      for (const auto &[oid, def] : cat->tables) {
        for (const index_def_t &x : def->indexes) {
          if (x.state == index_state_t::building) {
            found.emplace_back(oid, x.oid);
          }
        }
      }
    }
    for (const auto &[table, index] : found) {
      build_enqueue(cat, table, index);
    }
    return 0;
  });
}

/* Wait for a build to end; see sm3.h. */
int cat_index_wait(session_t *session, uint64_t table, uint64_t oid) {
  return catch_nomem([&] {
    if (session->in_txn) {
      return sm1::seterr(sm1::SM_ERR_INTRANS,
                         "index wait: inside a transaction");
    }
    catalog_t *cat = session->cat;
    builder_t &b = cat->builder;
    for (;;) {
      // The count first: a change after it wakes the wait below.
      uint64_t seq = cat->seq.load();
      std::optional<index_state_t> state = state_of(cat, table, oid);
      if (!state) {
        return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                           "index wait: no index " + std::to_string(oid) +
                               " on table " + std::to_string(table));
      }
      if (*state == index_state_t::valid) {
        return 0;
      }
      if (*state == index_state_t::failed) {
        return sm1::seterr(sm1::SM_ERR_DUPKEY,
                           "index wait: the build failed, on two rows with "
                           "one key");
      }
      std::unique_lock lock(b.mu);
      if (!b.active.count(oid)) {
        // It may have ended between the look above and this one.
        if (cat->seq.load() != seq) {
          continue;
        }
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "index wait: no build of the index is running");
      }
      b.cv.wait(lock,
                [&] { return cat->seq.load() != seq || !b.active.count(oid); });
    }
  });
}

} // namespace sm3
