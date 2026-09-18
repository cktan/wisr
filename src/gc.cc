/*
 * gc.cc -- garbage collection of dead versions (DESIGN.md §8.3):
 * gc_watermark, the compaction filter, tombstone purge, and the
 * removal of dropped relations' column families.
 *
 * The filter sees each relation CF's versions during ordinary
 * compaction, newest first within a userkey, and applies §8.3's
 * rules against the watermark taken when the compaction starts.
 * What it cannot do itself -- range-delete a purged key, drop a
 * column family -- it leaves to one worker thread per database.
 *
 * Once CF:txn grows past a threshold, a sweep compacts every
 * relation CF so each committed version is stamped, waits out
 * readers that could still see an unstamped copy, and deletes
 * the COMMITTED records it no longer needs.
 */

#include "codec.h"
#include "db.h"
#include "session.h"
#include "version.h"

#include <chrono>
#include <shared_mutex>

using namespace wisr;

namespace
{

/*
 * One compaction's filter. RocksDB gives each compaction thread
 * its own instance, so the mutable per-key state needs no lock.
 */
class gc_filter_t : public rocksdb::CompactionFilter
{
  public:
    /* A filter for relid, judging against watermark. */
    gc_filter_t(db_t* db, uint64_t relid, uint64_t watermark)
        : m_db(db), m_relid(relid), m_watermark(watermark)
    {
    }

    /* The filter's name, for RocksDB's logs. */
    const char* Name() const override
    {
        return "wisr.gc.v1";
    }

    /*
     * Apply §8.3 to one version: true drops it; false keeps it,
     * with *new_value set when it gains a commit_ts stamp. On
     * any doubt -- an undecodable value, a failed status lookup
     * -- keep the version untouched.
     */
    bool Filter(int, const rocksdb::Slice& key, const rocksdb::Slice& value,
                std::string* new_value, bool* value_changed) const override
    {
        version_t v;
        version_decode_key(key, &v);
        if (!version_decode_value(value, &v)) {
            m_db->gc_filter_errors++;
            return false;
        }
        uint64_t commit_ts = v.commit_ts;
        if (!(v.flags & version_stamped)) {
            switch (xct_status(m_db, v.xid, &commit_ts)) {
            case XCT_DEAD:
                return true; // aborted or crashed
            case XCT_IN_FLIGHT:
                return false; // no commit_ts to stamp
            case XCT_ERROR:
                m_db->gc_filter_errors++; // unstamped out of doubt
                return false;
            case XCT_COMMITTED:
                break;
            }
        }
        if (commit_ts > m_watermark) {
            return false; // some live snapshot may not see it yet
        }
        // Committed at or below the watermark: every live
        // snapshot sees the newest such version of the key, so
        // any older one is garbage.
        if (m_have_newest && v.userkey == rocksdb::Slice(m_newest_key)) {
            return true;
        }
        m_newest_key.assign(v.userkey.data(), v.userkey.size());
        m_have_newest = true;
        if (v.flags & version_tombstone) {
            // Queue the delete for purging; dropping it here could
            // resurrect older versions outside this compaction.
            std::lock_guard<std::mutex> guard(m_db->gc_mu);
            m_db->purges.push_back({m_relid, key.ToString()});
            m_db->gc_cv.notify_one();
        }
        if (!(v.flags & version_stamped)) {
            new_value->clear();
            new_value->push_back(static_cast<char>(v.flags | version_stamped));
            put_be64(*new_value, commit_ts);
            new_value->append(v.payload.data(), v.payload.size());
            *value_changed = true;
        }
        return false;
    }

  private:
    db_t* m_db;
    uint64_t m_relid;
    uint64_t m_watermark;
    mutable std::string m_newest_key; /* newest version <= watermark seen */
    mutable bool m_have_newest = false;
};

/* A filter that keeps everything, for compactions before GC is ready. */
class keep_all_filter_t : public rocksdb::CompactionFilter
{
  public:
    /* The filter's name, for RocksDB's logs. */
    const char* Name() const override
    {
        return "wisr.keep_all";
    }
};

/* Creates gc_filter_t for one relation CF. */
class gc_factory_t : public rocksdb::CompactionFilterFactory
{
  public:
    /* A factory for relid's CF in db. */
    gc_factory_t(db_t* db, uint64_t relid) : m_db(db), m_relid(relid) {}

    /* The factory's name, for RocksDB's logs. */
    const char* Name() const override
    {
        return "wisr.gc_factory.v1";
    }

    /*
     * A filter judging against the watermark as of now. Until
     * the database is fully open (compactions may start inside
     * DB::Open), keep everything.
     */
    std::unique_ptr<rocksdb::CompactionFilter>
    CreateCompactionFilter(const rocksdb::CompactionFilter::Context&) override
    {
        if (!m_db->gc_ready) {
            return std::make_unique<keep_all_filter_t>();
        }
        return std::make_unique<gc_filter_t>(m_db, m_relid, db_watermark(m_db));
    }

  private:
    db_t* m_db;
    uint64_t m_relid;
};

/*
 * Range-delete a purged key's tombstone and everything older:
 * [tombstone, userkey | FF x 12). Newer versions of the key sort
 * before the tombstone and survive. One range per key -- never
 * across keys, which would take other keys' rows with it.
 */
void purge_one(db_t* db, const db_t::purge_t& p,
               std::set<rocksdb::ColumnFamilyHandle*>* touched)
{
    rocksdb::ColumnFamilyHandle* cf = db_rel_cf(db, p.relid);
    if (!cf) {
        return; // the relation's CF is already gone
    }
    std::string end = version_bound(version_userkey(p.key), '\xff');
    rocksdb::Status s =
        db->rdb->DeleteRange(rocksdb::WriteOptions(), cf, p.key, end);
    if (s.ok()) {
        touched->insert(cf);
    }
    // On failure the tombstone stays; the next compaction that
    // meets it queues it again.
}

/*
 * The CF:txn sweep (§8.3), with gc_run_mu held. Phase one, from
 * start: record W0, then run every relation CF through a forced,
 * exclusive, bottommost compaction, so every committed version
 * with commit_ts <= W0 is dropped or stamped; then flip the read
 * epoch. Phase two, on a later call: once the old epoch has no
 * readers, delete the COMMITTED records at or below W0.
 */
int64_t sweep_txn_locked(db_t* db, bool start)
{
    if (!db->sweep_pending) {
        if (!start) {
            return 0;
        }
        uint64_t w0 = db_watermark(db);
        uint64_t errors = db->gc_filter_errors.load();
        std::vector<rocksdb::ColumnFamilyHandle*> cfs;
        {
            std::shared_lock<std::shared_mutex> guard(db->rel_mu);
            for (auto& kv : db->rels) {
                cfs.push_back(kv.second);
            }
        }
        rocksdb::CompactRangeOptions opt;
        // Reach the bottommost level explicitly rather than by the
        // default that happens to cover a filtered CF (§8.3).
        opt.bottommost_level_compaction =
            rocksdb::BottommostLevelCompaction::kForce;
        opt.exclusive_manual_compaction = true;
        for (rocksdb::ColumnFamilyHandle* cf : cfs) {
            // Compact the whole relation, memtable first.
            rocksdb::Status s =
                db->rdb->CompactRange(opt, cf, nullptr, nullptr);
            if (!s.ok()) {
                return -1;
            }
        }
        if (db->gc_filter_errors.load() != errors) {
            return -1; // some version stayed unstamped: keep records
        }
        db->sweep_w0 = w0;
        db->sweep_epoch = db->read_epoch.fetch_add(1);
        db->sweep_pending = true;
    }
    if (db->readers[db->sweep_epoch & 1].load() != 0) {
        return 0; // a pre-pass reader is still at work
    }
    db->sweep_pending = false;
    return xct_drop_committed_records(db, db->sweep_w0);
}

/* The worker: run GC when woken, and at least once a second. */
void gc_main(db_t* db)
{
    std::unique_lock<std::mutex> guard(db->gc_mu);
    while (!db->gc_stopping) {
        db->gc_cv.wait_for(guard, std::chrono::seconds(1));
        if (db->gc_stopping) {
            break;
        }
        guard.unlock();
        gc_run_once(db);
        guard.lock();
    }
}

} // namespace

namespace wisr
{

/* Count ourselves in the current epoch, re-checking it after. */
uint64_t db_read_enter(db_t* db)
{
    for (;;) {
        uint64_t e = db->read_epoch.load();
        db->readers[e & 1]++;
        if (db->read_epoch.load() == e) {
            return e;
        }
        db->readers[e & 1]--; // flipped under us: count in the new one
    }
}

/* Leave the epoch we entered. */
void db_read_leave(db_t* db, uint64_t epoch)
{
    db->readers[epoch & 1]--;
}

/* Read visible_ts and register it under snap_mu. */
uint64_t db_take_snapshot(db_t* db)
{
    std::lock_guard<std::mutex> guard(db->snap_mu);
    uint64_t ts = db->visible_ts.load();
    db->snapshots.insert(ts);
    return ts;
}

/* Remove one registration of snapshot_ts. */
void db_release_snapshot(db_t* db, uint64_t snapshot_ts)
{
    std::lock_guard<std::mutex> guard(db->snap_mu);
    auto it = db->snapshots.find(snapshot_ts);
    if (it != db->snapshots.end()) {
        db->snapshots.erase(it);
    }
}

/* The oldest registered snapshot, or visible_ts. */
uint64_t db_watermark(db_t* db)
{
    std::lock_guard<std::mutex> guard(db->snap_mu);
    return db->snapshots.empty() ? db->visible_ts.load()
                                 : *db->snapshots.begin();
}

/* The GC compaction filter factory for relid. */
std::shared_ptr<rocksdb::CompactionFilterFactory>
gc_filter_factory(db_t* db, uint64_t relid)
{
    return std::make_shared<gc_factory_t>(db, relid);
}

/* Start the worker thread. */
void gc_start(db_t* db)
{
    db->gc_thread = std::thread(gc_main, db);
}

/* Tell the worker to stop, and wait for it. */
void gc_stop(db_t* db)
{
    {
        std::lock_guard<std::mutex> guard(db->gc_mu);
        db->gc_stopping = true;
    }
    db->gc_cv.notify_all();
    if (db->gc_thread.joinable()) {
        db->gc_thread.join();
    }
}

/* Queue relids to drop once the watermark passes commit_ts. */
void gc_queue_drops(db_t* db, uint64_t commit_ts,
                    const std::vector<uint64_t>& relids)
{
    if (relids.empty()) {
        return;
    }
    std::lock_guard<std::mutex> guard(db->gc_mu);
    for (uint64_t relid : relids) {
        db->drops.emplace(commit_ts, relid);
    }
    db->gc_cv.notify_one();
}

/* Purge queued tombstones, then drop relations no snapshot can see. */
void gc_run_once(db_t* db)
{
    std::lock_guard<std::mutex> run(db->gc_run_mu);
    std::vector<db_t::purge_t> purges;
    std::vector<uint64_t> drops;
    uint64_t watermark = db_watermark(db);
    {
        std::lock_guard<std::mutex> guard(db->gc_mu);
        purges.swap(db->purges);
        auto end = db->drops.upper_bound(watermark);
        for (auto it = db->drops.begin(); it != end; ++it) {
            drops.push_back(it->second);
        }
        db->drops.erase(db->drops.begin(), end);
    }

    // Purge first: a key queued from a relation about to be
    // dropped is simply skipped once its CF is gone.
    std::set<rocksdb::ColumnFamilyHandle*> touched;
    for (const db_t::purge_t& p : purges) {
        purge_one(db, p, &touched);
    }
    // Flush the range tombstones into SST files, so reads stop
    // re-examining them in the memtable (§8.3).
    for (rocksdb::ColumnFamilyHandle* cf : touched) {
        db->rdb->Flush(rocksdb::FlushOptions(), cf);
    }

    for (uint64_t relid : drops) {
        // Drop the relation's column family: no snapshot sees it.
        db_drop_rel_cf(db, relid);
    }

    // Free SSI nodes the watermark has passed (§7.1).
    ssi_cleanup(db);

    // Finish a pending sweep, or start one once CF:txn is big.
    uint64_t records = 0;
    if (!db->sweep_pending &&
        db->rdb->GetIntProperty(db->cf_txn, "rocksdb.estimate-num-keys",
                                &records) &&
        records > db->txn_sweep_records) {
        sweep_txn_locked(db, true);
    } else if (db->sweep_pending) {
        sweep_txn_locked(db, false);
    }
}

/* gc_sweep_txn under gc_run_mu, which serializes it with the drops. */
int64_t gc_sweep_txn(db_t* db, bool start)
{
    std::lock_guard<std::mutex> run(db->gc_run_mu);
    return sweep_txn_locked(db, start);
}

} // namespace wisr
