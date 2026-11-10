/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * gc.cc -- garbage collection of dead versions (../GC.md §1):
 * gc_watermark, the compaction filter, tombstone purge, and the
 * removal of dropped stores' column families.
 *
 * The filter sees each store CF's versions during ordinary
 * compaction, newest first within a userkey, and applies ../GC.md §1's
 * rules against the watermark taken when the compaction starts.
 * What it cannot do itself -- range-delete a purged key, drop a
 * column family -- it leaves to one worker thread per database.
 *
 * Once CF:txn grows past a threshold, a sweep compacts every
 * store CF so each committed version is stamped, waits out
 * readers that could still see an unstamped copy, and deletes
 * the COMMITTED records it no longer needs.
 */

#include "chain.h"
#include "db.h"
#include "log.h"
#include "session.h"
#include "sm0_codec.h"
#include "version.h"

#include <chrono>
#include <shared_mutex>

using namespace wisr;

namespace {

/*
 * One compaction's filter. RocksDB gives each compaction thread
 * its own instance, so the mutable per-key state needs no lock.
 */
class gc_filter_t : public rocksdb::CompactionFilter {
public:
  /* A filter for storeid, judging against watermark. */
  gc_filter_t(db_t *db, uint64_t storeid, uint64_t watermark)
      : m_db(db), m_storeid(storeid), m_watermark(watermark) {}

  /* Report, once per compaction, the versions it could not judge. */
  ~gc_filter_t() override {
    // Warn if any version was kept unstamped out of doubt.
    if (m_doubts > 0) {
      log_msg(sm1::loglevel_t::warn,
              "database {}: compaction kept {} version(s) in column "
              "family store.{} unstamped: undecodable value or unknown writer "
              "status",
              m_db->name.c_str(), m_doubts, m_storeid);
    }
  }

  /* The filter's name, for RocksDB's logs. */
  const char *Name() const override { return "wisr.gc.v1"; }

  /*
   * Judge one version by judge(), and say how RocksDB is to remove one
   * it drops (../PURGE_PLAN.md §3): a versioned key, written once, by a
   * SingleDelete, as every deletion of one is; a chained key, written
   * more than once, by a Delete. Only a plain value is judged.
   */
  Decision FilterV2(int level, const rocksdb::Slice &key, ValueType type,
                    const rocksdb::Slice &value, std::string *new_value,
                    std::string *) const override {
    if (ValueType::kValue != type) {
      return Decision::kKeep;
    }
    bool changed = false;
    if (judge(level, key, value, new_value, &changed)) {
      return !key.empty() && key[0] == key_class_versioned ? Decision::kPurge
                                                           : Decision::kRemove;
    }
    return changed ? Decision::kChangeValue : Decision::kKeep;
  }

private:
  /*
   * Apply ../GC.md §1 to one version: true drops it; false keeps it,
   * with *new_value set when it gains a commit_ts stamp. On
   * any doubt -- an undecodable value, a failed status lookup
   * -- keep the version untouched.
   */
  bool judge(int, const rocksdb::Slice &key, const rocksdb::Slice &value,
             std::string *new_value, bool *value_changed) const {
    // A page is no version: it is the level above's, and kept as it
    // is (../BTREEINDEX.md §9).
    if (!key.empty() && key[0] == key_class_page) {
      return false;
    }
    // A chained key's versions have rules of their own (../ROWSTORE.md
    // ../GC.md §4.2).
    if (!key.empty() && key[0] != key_class_versioned) {
      return filter_chained(key, value, key[0] == key_class_old, new_value,
                            value_changed);
    }
    // Decode the version's userkey, xid, and seq.
    version_t v;
    version_decode_key(key, &v);
    // Keep an undecodable value, and count it as an error.
    if (!version_decode_value(value, &v)) {
      // Count the doubt: a sweep may not trust this pass.
      m_db->gc_filter_errors++;
      m_doubts++;
      return false;
    }
    // Take the stamp, if the version has one.
    uint64_t commit_ts = v.commit_ts;
    // Resolve an unstamped version's writer.
    if (!(v.flags & version_stamped)) {
      // Branch on the writer's fate.
      switch (xct_status(m_db, v.xid, &commit_ts)) {
      case XCT_DEAD:
        return true; // aborted or crashed
      case XCT_IN_FLIGHT:
        return false; // no commit_ts to stamp
      case XCT_ERROR:
        // Count the doubt: a sweep may not trust this pass.
        m_db->gc_filter_errors++; // unstamped out of doubt
        m_doubts++;
        return false;
      case XCT_COMMITTED:
        break;
      }
    }
    // Keep versions some live snapshot may not see yet.
    if (commit_ts > m_watermark) {
      return false; // some live snapshot may not see it yet
    }
    // Committed at or below the watermark: every live
    // snapshot sees the newest such version of the key, so
    // any older one is garbage.
    if (m_have_newest && v.userkey == rocksdb::Slice(m_newest_key)) {
      return true;
    }
    // This is the key's newest version at or below the watermark.
    m_newest_key.assign(v.userkey.data(), v.userkey.size());
    // Later versions of this key are older, and garbage.
    m_have_newest = true;
    // A tombstone is queued for purging, not dropped here.
    if (v.flags & version_tombstone) {
      // Queue the delete for purging; dropping it here could
      // resurrect older versions outside this compaction. The queue
      // takes a key once: each compaction that meets it before the
      // purge would queue it again (../PURGE_PLAN.md §2.4).
      if (m_db->purges.push(m_storeid, 0,
                            std::string_view(key.data(), key.size()))) {
        // Wake the worker.
        m_db->gc_cv.notify_one();
      }
    }
    // Stamp commit_ts into an unstamped version's header.
    if (!(v.flags & version_stamped)) {
      // Rebuild the value from scratch.
      new_value->clear();
      // Set the stamped flag.
      new_value->push_back(static_cast<char>(v.flags | version_stamped));
      // Write the commit_ts after the flags.
      sm0::put_be64(*new_value, commit_ts);
      // Keep the payload as it was.
      new_value->append(v.payload.data(), v.payload.size());
      // Tell RocksDB the value changed.
      *value_changed = true;
    }
    return false;
  }

  /*
   * Judge a chained key's version, a current one or with old an old
   * one (../GC.md §4.2): true drops it (RocksDB writes a delete
   * over the key, so no older entry of it comes back); false keeps
   * it, with *new_value set when it gains a stamp.
   */
  bool filter_chained(const rocksdb::Slice &key, const rocksdb::Slice &value,
                      bool old, std::string *new_value,
                      bool *value_changed) const {
    chain_version_t v;
    if (!chain_decode(value, &v, old)) {
      return doubt();
    }
    if (old) {
      return filter_old(v, new_value, value_changed);
    }
    uint64_t commit_ts;
    switch (writer(v, &commit_ts)) {
    case XCT_DEAD:
      // Left by an abort or a crash: the worker puts the version
      // before it back. The key less its class byte is the userkey.
      chain_report(m_db, m_storeid,
                   rocksdb::Slice(key.data() + 1, key.size() - 1), v.xid);
      return false;
    case XCT_IN_FLIGHT:
      return false;
    case XCT_ERROR:
      return doubt();
    case XCT_COMMITTED:
      break;
    }
    if (commit_ts > m_watermark) {
      return false; // some live snapshot may not see it yet
    }
    // A delete every snapshot sees: the key goes, and the chain
    // behind it goes by its displacer's rule.
    if (v.flags & version_tombstone) {
      return true;
    }
    stamp(&v, commit_ts, false, new_value, value_changed);
    return false;
  }

  /* Count a version kept out of doubt; returns false, to keep it. */
  bool doubt() const {
    m_db->gc_filter_errors++;
    m_doubts++;
    return false;
  }

  /* The fate of v's own writer, its commit_ts into *commit_ts. */
  xct_state_t writer(const chain_version_t &v, uint64_t *commit_ts) const {
    if (v.flags & version_stamped) {
      *commit_ts = v.commit_ts;
      return XCT_COMMITTED;
    }
    return xct_status(m_db, v.xid, commit_ts);
  }

  /* Stamp v's own writer's commit_ts, if it is not yet. */
  void stamp(chain_version_t *v, uint64_t commit_ts, bool old,
             std::string *new_value, bool *value_changed) const {
    if (v->flags & version_stamped) {
      return;
    }
    v->flags |= version_stamped;
    v->commit_ts = commit_ts;
    *new_value = chain_value(*v, old);
    *value_changed = true;
  }

  /* Judge an old version; see filter_chained. */
  bool filter_old(chain_version_t &v, std::string *new_value,
                  bool *value_changed) const {
    // Unlinked: no chain reaches it; a reader on its way is older
    // than the ts it was unlinked at.
    if (v.flags & chain_unlinked) {
      return v.displaced <= m_watermark;
    }
    // When its displacer committed, or that it has not.
    uint64_t displaced_ts = v.displaced;
    if (!(v.flags & chain_displaced_stamped)) {
      switch (xct_status(m_db, v.displaced, &displaced_ts)) {
      case XCT_DEAD:
        // Its displacer aborted: the dead version still points to it
        // until the repair unlinks it.
        return stamp_own(v, new_value, value_changed);
      case XCT_IN_FLIGHT:
        return stamp_own(v, new_value, value_changed);
      case XCT_ERROR:
        return doubt();
      case XCT_COMMITTED:
        break;
      }
    }
    // Displaced by a version every snapshot sees: no one's, nor is
    // anything older in its chain.
    if (displaced_ts <= m_watermark) {
      return true;
    }
    // Kept: stamp the displacer, and its own writer, so the sweep may
    // delete their status records.
    bool changed = !(v.flags & chain_displaced_stamped);
    v.flags |= chain_displaced_stamped;
    v.displaced = displaced_ts;
    uint64_t own_ts;
    xct_state_t own = writer(v, &own_ts);
    if (XCT_ERROR == own) {
      return doubt();
    }
    if (XCT_COMMITTED == own && !(v.flags & version_stamped)) {
      v.flags |= version_stamped;
      v.commit_ts = own_ts;
      changed = true;
    }
    if (changed) {
      *new_value = chain_value(v, true);
      *value_changed = true;
    }
    return false;
  }

  /* Keep an old version, stamping its own writer if it committed. */
  bool stamp_own(chain_version_t &v, std::string *new_value,
                 bool *value_changed) const {
    uint64_t ts;
    xct_state_t own = writer(v, &ts);
    if (XCT_ERROR == own) {
      return doubt();
    }
    if (XCT_COMMITTED == own) {
      stamp(&v, ts, true, new_value, value_changed);
    }
    return false;
  }

  db_t *m_db;
  uint64_t m_storeid;
  uint64_t m_watermark;
  mutable std::string m_newest_key; /* newest version <= watermark seen */
  mutable bool m_have_newest = false;
  mutable uint64_t m_doubts = 0; /* versions kept out of doubt */
};

/* A filter that keeps everything, for compactions before GC is ready. */
class keep_all_filter_t : public rocksdb::CompactionFilter {
public:
  /* The filter's name, for RocksDB's logs. */
  const char *Name() const override { return "wisr.keep_all"; }
};

/* Creates gc_filter_t for one store CF. */
class gc_factory_t : public rocksdb::CompactionFilterFactory {
public:
  /* A factory for storeid's CF in db. */
  gc_factory_t(db_t *db, uint64_t storeid) : m_db(db), m_storeid(storeid) {}

  /* The factory's name, for RocksDB's logs. */
  const char *Name() const override { return "wisr.gc_factory.v1"; }

  /*
   * A filter judging against the watermark as of now. Until
   * the database is fully open (compactions may start inside
   * DB::Open), keep everything.
   */
  std::unique_ptr<rocksdb::CompactionFilter>
  CreateCompactionFilter(const rocksdb::CompactionFilter::Context &) override {
    // Until the database is fully open, keep everything.
    if (!m_db->gc_ready) {
      return std::make_unique<keep_all_filter_t>();
    }
    // Judge against the watermark as of this compaction's start.
    return std::make_unique<gc_filter_t>(m_db, m_storeid, db_watermark(m_db));
  }

private:
  db_t *m_db;
  uint64_t m_storeid;
};

/*
 * Erase a purged key's tombstone and every older version of the key,
 * each by a SingleDelete (../PURGE_PLAN.md §3): every version key is
 * written once, and every deletion of one is a SingleDelete, so the
 * pair goes at the first compaction that meets both.
 * 1. Skip a store already dropped.
 * 2. Read the versions in [tombstone, userkey | FF x 12), with no
 *    snapshot: newer versions of the key sort before the tombstone and
 *    survive; one already erased is not read.
 * 3. Write a SingleDelete of each in one batch, at low priority, so
 *    RocksDB slows it, not the sessions' writes, when compaction is
 *    behind.
 * A version a compaction erases meanwhile gets a second SingleDelete,
 * which a compaction skips (db/compaction/compaction_iterator.cc).
 */
void purge_one(db_t *db, uint64_t storeid, rocksdb::Slice tombstone) {
  // Find the store's CF.
  store_t st = db_store(db, storeid);
  // Skip a store already dropped.
  if (!st.cf) {
    return; // the store's CF is already gone
  }
  // End the range just past the key's oldest version.
  std::string end = version_bound(version_userkey(tombstone), '\xff');
  rocksdb::Slice upper(end);
  rocksdb::ReadOptions ro;
  ro.iterate_upper_bound = &upper;
  // Read the tombstone and every older version still there.
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
  rocksdb::WriteBatch batch;
  uint64_t bytes = 0;
  for (it->Seek(tombstone); it->Valid(); it->Next()) {
    batch.SingleDelete(st.cf, it->key());
    bytes += it->key().size();
  }
  rocksdb::Status s = it->status();
  if (s.ok() && batch.Count() > 0) {
    rocksdb::WriteOptions wo = store_write_options(st);
    // Behind the sessions' writes when compaction is behind.
    wo.low_pri = true;
    s = st.rdb->Write(wo, &batch);
  }
  if (s.ok()) {
    rocksdb::Slice userkey = version_userkey(tombstone);
    count_write(st, std::string_view(userkey.data(), userkey.size()), bytes);
    return;
  }
  // On failure the tombstone stays; the next compaction that
  // meets it queues it again. Nobody waits on this, so log it.
  log_msg(sm1::loglevel_t::error,
          "database {}: purge in column family store.{} failed: {}",
          db->name.c_str(), storeid, s.ToString().c_str());
}

/*
 * The CF:txn sweep (../GC.md §1), with gc_run_mu held: the records deleted
 * when the sweep completes, 0 if it is pending, -1 if the pass failed
 * or could not be trusted.
 * 1. With no sweep pending: return 0 unless start is set.
 * 2. Record W0, the watermark, and the size of CF:txn.
 * 3. Run every store CF through a forced, exclusive, bottommost
 *    compaction, so every committed version with commit_ts <= W0 is
 *    dropped or stamped. Return -1 if a compaction fails, or if the
 *    filter met a version it could not stamp.
 * 4. Flip the read epoch, and mark the sweep pending.
 * 5. If the old epoch still has readers, return 0: a later call
 *    goes on from here. Else, in this call or a later one, delete
 *    the COMMITTED records at or below W0, and end the sweep.
 * A sweep that ends, deleted or not, sets sweep_floor to the records
 * it left, so the next one waits for txn_sweep_records more (back-off).
 */
int64_t sweep_txn_locked(db_t *db, bool start) {
  // Start a sweep only when none is pending.
  if (!db->sweep_pending) {
    // Nothing pending and not asked to start.
    if (!start) {
      return 0;
    }
    // Record W0: the pass stamps everything at or below it.
    uint64_t w0 = db_watermark(db);
    // Remember the size of CF:txn: the back-off floor starts from it.
    uint64_t records = 0;
    db->rdb->GetIntProperty(db->cf_txn, "rocksdb.estimate-num-keys", &records);
    db->sweep_records = records;
    // Remember the filter's error count before the pass.
    uint64_t errors = db->gc_filter_errors.load();
    // Collect every store CF.
    std::vector<store_t> cfs;
    {
      // Hold store_mu while reading the CF map.
      std::shared_lock<std::shared_mutex> guard(db->store_mu);
      // Take each store's handle.
      for (auto &kv : db->stores) {
        cfs.push_back(kv.second);
      }
    }
    // Build the pass's compaction options.
    rocksdb::CompactRangeOptions opt;
    // Reach the bottommost level explicitly rather than by the
    // default that happens to cover a filtered CF (../GC.md §1).
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    // Keep automatic compactions from running alongside.
    opt.exclusive_manual_compaction = true;
    // Run every store through the filter.
    for (const store_t &st : cfs) {
      // Compact the whole store, memtable first.
      rocksdb::Status s = st.rdb->CompactRange(opt, st.cf, nullptr, nullptr);
      // A failed compaction leaves versions unstamped.
      if (!s.ok()) {
        log_msg(sm1::loglevel_t::error,
                "database {}: CF:txn sweep abandoned: compaction failed: {}",
                db->name.c_str(), s.ToString().c_str());
        // Back off: nothing was deleted.
        db->sweep_floor = db->sweep_records;
        return -1;
      }
    }
    // Any filter doubt during the pass forbids deleting records.
    if (db->gc_filter_errors.load() != errors) {
      log_msg(sm1::loglevel_t::warn,
              "database {}: CF:txn sweep abandoned: some versions stayed "
              "unstamped, so their status records are kept",
              db->name.c_str());
      // Back off: nothing was deleted.
      db->sweep_floor = db->sweep_records;
      return -1; // some version stayed unstamped: keep records
    }
    // The pass succeeded: remember W0.
    db->sweep_w0 = w0;
    // Flip the read epoch; pre-pass readers count in the old one.
    db->sweep_epoch = db->read_epoch.fetch_add(1);
    // Wait for them before deleting records.
    db->sweep_pending = true;
  }
  // A pre-pass reader may still see unstamped copies.
  if (db->readers[db->sweep_epoch & 1].load() != 0) {
    return 0; // a pre-pass reader is still at work
  }
  // The old epoch drained: the sweep completes now.
  db->sweep_pending = false;
  // Delete the records every version has outgrown.
  int64_t n = xct_drop_committed_records(db, db->sweep_w0);
  // Back off: the records left, short of what the sweep deleted.
  uint64_t gone = n > 0 ? static_cast<uint64_t>(n) : 0;
  db->sweep_floor = db->sweep_records > gone ? db->sweep_records - gone : 0;
  // Report the outcome: nobody waits on a background sweep.
  if (n < 0) {
    log_msg(sm1::loglevel_t::error, "database {}: CF:txn sweep failed: {}",
            db->name.c_str(), sm1::errmsg());
  } else {
    log_msg(sm1::loglevel_t::info,
            "database {}: CF:txn sweep deleted {} record(s)", db->name.c_str(),
            n);
  }
  return n;
}

/*
 * Warn, once per snapshot, about a snapshot that has held back
 * garbage collection for over stuck_warn_secs: an idle
 * transaction, a forgotten cursor, or a transaction in doubt.
 */
void warn_stuck(db_t *db) {
  auto now = std::chrono::steady_clock::now();
  std::vector<std::pair<uint64_t, long long>> stuck;
  {
    // Hold snap_mu while reading the snapshots' ages.
    std::lock_guard<std::mutex> guard(db->snap_mu);
    // Find snapshots past the threshold not yet warned about.
    for (auto &kv : db->snap_since) {
      long long age =
          std::chrono::duration_cast<std::chrono::seconds>(now - kv.second)
              .count();
      if (age >= db->stuck_warn_secs && !db->snap_warned.count(kv.first)) {
        // Warn once per snapshot.
        db->snap_warned.insert(kv.first);
        stuck.emplace_back(kv.first, age);
      }
    }
  }
  // Log outside snap_mu.
  for (auto &st : stuck) {
    log_msg(sm1::loglevel_t::warn,
            "database {}: a transaction with snapshot {} has held back "
            "garbage collection for {}s",
            db->name.c_str(), st.first, st.second);
  }
}

/* The worker: run GC when woken, and at least once a second. */
void gc_main(db_t *db) {
  // Hold gc_mu while waiting for work.
  std::unique_lock<std::mutex> guard(db->gc_mu);
  // Run until told to stop.
  while (!db->gc_stopping) {
    // Sleep until woken or a second passes.
    db->gc_cv.wait_for(guard, std::chrono::seconds(1));
    // Stop promptly when told to.
    if (db->gc_stopping) {
      break;
    }
    // Run GC without holding gc_mu.
    guard.unlock();
    gc_run_once(db);
    // Retake gc_mu before waiting again.
    guard.lock();
  }
}

} // namespace

namespace wisr {

/* Count ourselves in the current epoch, re-checking it after. */
uint64_t db_read_enter(db_t *db) {
  // Retry until counted in a stable epoch.
  for (;;) {
    // Read the current epoch.
    uint64_t e = db->read_epoch.load();
    // Count ourselves in it.
    db->readers[e & 1]++;
    // Still current: we are counted in the right epoch.
    if (db->read_epoch.load() == e) {
      return e;
    }
    db->readers[e & 1]--; // flipped under us: count in the new one
  }
}

/* Leave the epoch we entered. */
void db_read_leave(db_t *db, uint64_t epoch) {
  // Leave the epoch we entered.
  db->readers[epoch & 1]--;
}

/* Read visible_ts and register it under snap_mu. */
uint64_t db_take_snapshot(db_t *db) {
  // Hold snap_mu so no watermark reads between the two steps.
  std::lock_guard<std::mutex> guard(db->snap_mu);
  // The snapshot is everything published so far.
  uint64_t ts = db->visible_ts.load();
  // Register it for the watermark.
  db->snapshots.insert(ts);
  // Note when, for the stuck-snapshot warning.
  db->snap_since.emplace(ts, std::chrono::steady_clock::now());
  // Publish the watermark for readers that take no mutex.
  db->watermark_cache.store(*db->snapshots.begin(), std::memory_order_relaxed);
  return ts;
}

/* Register snapshot_ts under snap_mu. */
void db_register_snapshot(db_t *db, uint64_t snapshot_ts) {
  // Hold snap_mu while registering.
  std::lock_guard<std::mutex> guard(db->snap_mu);
  // Register the snapshot for the watermark.
  db->snapshots.insert(snapshot_ts);
  // Note when, for the stuck-snapshot warning.
  db->snap_since.emplace(snapshot_ts, std::chrono::steady_clock::now());
  // Publish the watermark for readers that take no mutex.
  db->watermark_cache.store(*db->snapshots.begin(), std::memory_order_relaxed);
}

/* Remove one registration of snapshot_ts. */
void db_release_snapshot(db_t *db, uint64_t snapshot_ts) {
  // Hold snap_mu while unregistering.
  std::lock_guard<std::mutex> guard(db->snap_mu);
  // Find one registration of the snapshot.
  auto it = db->snapshots.find(snapshot_ts);
  // Remove it, if present.
  if (it != db->snapshots.end()) {
    db->snapshots.erase(it);
  }
  // Forget when it was taken.
  auto since = db->snap_since.find(snapshot_ts);
  if (since != db->snap_since.end()) {
    db->snap_since.erase(since);
  }
  // With the last snapshot at this ts gone, a new one may warn again.
  if (!db->snapshots.count(snapshot_ts)) {
    db->snap_warned.erase(snapshot_ts);
  }
  // Publish the watermark for readers that take no mutex.
  db->watermark_cache.store(db->snapshots.empty() ? db->visible_ts.load()
                                                  : *db->snapshots.begin(),
                            std::memory_order_relaxed);
}

/* The oldest registered snapshot, or visible_ts. */
uint64_t db_watermark(db_t *db) {
  // Hold snap_mu while reading the snapshots.
  std::lock_guard<std::mutex> guard(db->snap_mu);
  // The oldest snapshot, or everything published if none.
  return db->snapshots.empty() ? db->visible_ts.load() : *db->snapshots.begin();
}

/* The GC compaction filter factory for storeid. */
std::shared_ptr<rocksdb::CompactionFilterFactory>
gc_filter_factory(db_t *db, uint64_t storeid) {
  // Create a factory bound to the database and store.
  return std::make_shared<gc_factory_t>(db, storeid);
}

/* Start the worker thread. */
void gc_start(db_t *db) {
  // Start the worker thread.
  db->gc_thread = std::thread(gc_main, db);
}

/* Tell the worker to stop, and wait for it. */
void gc_stop(db_t *db) {
  {
    // Hold gc_mu while setting the stop flag.
    std::lock_guard<std::mutex> guard(db->gc_mu);
    // Tell the worker to stop.
    db->gc_stopping = true;
  }
  // Wake it.
  db->gc_cv.notify_all();
  // Wait for it to finish, if it was started.
  if (db->gc_thread.joinable()) {
    db->gc_thread.join();
  }
}

/* Queue storeids to drop once the watermark passes commit_ts. */
void gc_queue_drops(db_t *db, uint64_t commit_ts,
                    const std::vector<uint64_t> &storeids) {
  // Nothing dropped, nothing to queue.
  if (storeids.empty()) {
    return;
  }
  // Hold gc_mu while queueing.
  std::lock_guard<std::mutex> guard(db->gc_mu);
  // Queue each storeid under the drop's commit_ts.
  for (uint64_t storeid : storeids) {
    db->drops.emplace(commit_ts, storeid);
  }
  // Wake the worker.
  db->gc_cv.notify_one();
}

/* Do the GC work that is due now; see db.h. */
void gc_run_once(db_t *db) {
  // One GC run at a time.
  std::lock_guard<std::mutex> run(db->gc_run_mu);
  std::vector<uint64_t> drops;
  // Take the watermark once, for the whole run.
  uint64_t watermark = db_watermark(db);
  {
    // Hold gc_mu while taking work from the queues.
    std::lock_guard<std::mutex> guard(db->gc_mu);
    // Take the drops the watermark has passed.
    auto end = db->drops.upper_bound(watermark);
    for (auto it = db->drops.begin(); it != end; ++it) {
      drops.push_back(it->second);
    }
    // Remove them from the queue.
    db->drops.erase(db->drops.begin(), end);
  }

  // Take every queued repair and purge, outside gc_mu: a queue has its
  // own lock. A key met again from now is queued again.
  hint_queue_t::batch_t repairs = db->repairs.take();
  hint_queue_t::batch_t purges = db->purges.take();

  // Put back the chained keys compaction found dead.
  chain_repair(db, repairs);

  // Purge first: a key queued from a store about to be
  // dropped is simply skipped once its CF is gone.
  purges.for_each([&](const hint_queue_t::record_t &p) {
    purge_one(db, p.storeid, rocksdb::Slice(p.key.data(), p.key.size()));
  });

  // Drop each store no snapshot can see.
  for (uint64_t storeid : drops) {
    // Drop the store's column family: no snapshot sees it.
    db_drop_store_cf(db, storeid);
  }

  // Free SSI nodes the watermark has passed (../XCTMGR.md §7.2).
  ssi_cleanup(db);

  // Warn about snapshots holding back GC for too long.
  warn_stuck(db);

  // Finish a pending sweep, or start one once CF:txn is big.
  uint64_t records = 0;
  if (!db->sweep_pending &&
      db->rdb->GetIntProperty(db->cf_txn, "rocksdb.estimate-num-keys",
                              &records) &&
      records > db->sweep_floor + db->txn_sweep_records) {
    // Big enough: start a sweep.
    sweep_txn_locked(db, true);
  } else if (db->sweep_pending) {
    // A sweep is pending: try to finish it.
    sweep_txn_locked(db, false);
  }
}

/* gc_sweep_txn under gc_run_mu, which serializes it with the drops. */
int64_t gc_sweep_txn(db_t *db, bool start) {
  // Hold gc_run_mu, as a GC run does.
  std::lock_guard<std::mutex> run(db->gc_run_mu);
  return sweep_txn_locked(db, start);
}

} // namespace wisr
