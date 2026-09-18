/*
 * db.h -- the layout of one database directory: its column
 * families, their options, and CF:meta's keys (DESIGN.md §3).
 */
#ifndef WISR_DB_H
#define WISR_DB_H

#include "sm.h"

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rocksdb/compaction_filter.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

/* Database format version, stored in CF:meta. */
#define WISR_DB_VERSION 1

/* Column family names. */
#define WISR_CF_TXN  "txn"
#define WISR_CF_META "meta"

/* CF:meta keys (DESIGN.md §3.3, §4.2). */
#define WISR_META_VERSION    "version"
#define WISR_META_NEXT_TS    "next_ts"
#define WISR_META_NEXT_RELID "next_relid"

namespace wisr
{

/*
 * One open database, shared by every session on it and held
 * in the environment's registry (DESIGN.md §1).
 */
struct db_t {
    std::string name;
    rocksdb::DB* rdb = nullptr;
    rocksdb::ColumnFamilyHandle* cf_default = nullptr;
    rocksdb::ColumnFamilyHandle* cf_txn = nullptr;
    rocksdb::ColumnFamilyHandle* cf_meta = nullptr;
    int durability_sync = 1;
    int nsessions = 0; /* guarded by the env's mu */

    /* relid -> column family handle, for every live relation CF. */
    std::shared_mutex rel_mu;
    std::map<uint64_t, rocksdb::ColumnFamilyHandle*> rels;

    /* The ts and relid allocators and their leases (§4.2). */
    std::mutex alloc_mu;
    uint64_t lease = 0;
    uint64_t global_ts = 0;  /* next ts to hand out */
    uint64_t ts_limit = 0;   /* persisted; never handed out */
    uint64_t next_relid = 0; /* next relid to hand out */
    uint64_t relid_limit = 0;

    /*
     * The commit sequencer (§4.3): commit_ts values claimed and
     * not yet published. Guarded by alloc_mu, the mutex that
     * hands out timestamps, so no commit_ts is ever outside both
     * this set and the advancer's view of global_ts.
     */
    std::set<uint64_t> committing;

    /* Every commit_ts at or below this is published (§4.3). */
    std::atomic<uint64_t> visible_ts{0};

    /*
     * The active transaction table and the commit cache (§3.4),
     * under one mutex so a committing xid moves from one to the
     * other atomically. The cache evicts the oldest commit_ts.
     */
    std::mutex txn_mu;
    std::unordered_set<uint64_t> active;          /* xids in flight */
    std::unordered_map<uint64_t, uint64_t> cache; /* xid -> commit_ts */
    std::map<uint64_t, uint64_t> cache_by_ts;     /* commit_ts -> xid */
    size_t cache_entries = 0;

    /*
     * Set when a status write fails (§8.2): the outcome of that
     * commit is unknown until a reopen, so writes, commits, and
     * DDL fail with SM_ERR_IO from then on.
     */
    std::atomic<bool> failed{false};

    /*
     * Test hook: called after a commit claims its commit_ts and
     * before it writes the status record. A non-OK status stands
     * in for the write's failure. Empty outside tests.
     */
    std::function<rocksdb::Status(uint64_t commit_ts)> test_before_commit;

    /*
     * Row locks (§5.1): lock key -> the session whose transaction
     * holds it. One mutex covers the table and every session's
     * waiting_for pointer, so a deadlock walk sees a consistent
     * waits-for graph; shard it if a benchmark shows contention.
     * Every release and every doom notifies lock_cv.
     */
    std::mutex lock_mu;
    std::condition_variable lock_cv;
    std::unordered_map<std::string, sm_session_t*> lock_owners;
    int deadlock_ms = 50;

    /* Hands out sm_session_t::begin_seq. */
    std::atomic<uint64_t> begin_seq{0};

    /*
     * The begin snapshots of active transactions, whose minimum
     * is gc_watermark (§8.3). Taking a snapshot and registering
     * it happen under snap_mu, the mutex the watermark is read
     * under, so no snapshot lands below a watermark in use.
     */
    std::mutex snap_mu;
    std::multiset<uint64_t> snapshots;

    /*
     * Garbage collection (§8.3). The compaction filter runs only
     * once gc_ready is set, after the handles above are valid.
     * It cannot write, so it queues tombstones to purge; a
     * committed sm_rmrel queues its relids to drop once the
     * watermark passes its commit. The worker drains both.
     */
    std::atomic<bool> gc_ready{false};
    std::mutex gc_mu;
    std::condition_variable gc_cv;
    bool gc_stopping = false;
    struct purge_t {
        uint64_t relid;
        std::string key; /* the tombstone's version key */
    };
    std::vector<purge_t> purges;
    std::multimap<uint64_t, uint64_t> drops; /* commit_ts -> relid */
    std::thread gc_thread;
    std::mutex gc_run_mu; /* one gc_run_once at a time */

    /*
     * Read epochs, for the CF:txn sweep (§8.3 step 3). Every
     * reader -- a point read, a write's existence check, a
     * cursor for its whole life -- is counted in the current
     * epoch's slot from before it opens an iterator until it is
     * done resolving what it read. The sweep flips the epoch
     * after its compaction pass and deletes records only once
     * the old slot drains: every reader that could still see a
     * pre-pass, unstamped version has finished.
     */
    std::atomic<uint64_t> read_epoch{0};
    std::atomic<int64_t> readers[2] = {};

    /*
     * The CF:txn sweep in progress: after its pass and epoch
     * flip it waits, across GC runs, for the old epoch's readers
     * before deleting COMMITTED records at or below sweep_w0.
     * gc_filter_errors counts versions the filter kept unstamped
     * out of doubt; a pass that saw any may not delete records.
     */
    bool sweep_pending = false;
    uint64_t sweep_w0 = 0;
    uint64_t sweep_epoch = 0;
    std::atomic<uint64_t> gc_filter_errors{0};
    uint64_t txn_sweep_records = 1000000; /* CF:txn keys to trigger */

    /* Destroy the CF handles and close RocksDB. */
    ~db_t();
};

/*
 * Open database dbname at path with every column family it
 * has, and load its allocators from CF:meta (DESIGN.md §9.1
 * steps 1-3). Returns NULL with the error set.
 */
db_t* db_open(const std::string& path, const char* dbname,
              const sm_options_t* opt);

/* Allocate a timestamp from db's lease. Returns 0 or -1. */
int db_alloc_ts(db_t* db, uint64_t* out);

/*
 * Claim a commit_ts: allocate it and enter it into the
 * sequencer, under one lock (§4.3). Returns 0 or -1.
 */
int db_claim_commit_ts(db_t* db, uint64_t* out);

/*
 * Mark commit_ts done: leave the sequencer and advance
 * visible_ts to one below the oldest commit still in progress.
 */
void db_commit_done(db_t* db, uint64_t commit_ts);

/* Allocate a relid from db's lease. Returns 0 or -1. */
int db_alloc_relid(db_t* db, uint64_t* out);

/* The CF handle of relid, or NULL if it has none. */
rocksdb::ColumnFamilyHandle* db_rel_cf(db_t* db, uint64_t relid);

/* Create the CF for relid and register it. Returns 0 or -1. */
int db_create_rel_cf(db_t* db, uint64_t relid);

/* Drop relid's CF and unregister it. Returns 0 or -1. */
int db_drop_rel_cf(db_t* db, uint64_t relid);

/* The column family name of relation relid: rel.<relid>. */
std::string rel_cf_name(uint64_t relid);

/*
 * Options for a relation CF: comparator, prefix extractor,
 * bloom filter, and -- when db is given -- the GC compaction
 * filter for relid.
 */
rocksdb::ColumnFamilyOptions rel_cf_options(db_t* db = nullptr,
                                            uint64_t relid = 0);

/* Enter the current read epoch; returns it for db_read_leave. */
uint64_t db_read_enter(db_t* db);

/* Leave the read epoch returned by db_read_enter. */
void db_read_leave(db_t* db, uint64_t epoch);

/* Take a transaction's begin snapshot and register it (§8.3). */
uint64_t db_take_snapshot(db_t* db);

/* Unregister a snapshot taken by db_take_snapshot. */
void db_release_snapshot(db_t* db, uint64_t snapshot_ts);

/*
 * gc_watermark (§8.3): the oldest registered snapshot, or
 * visible_ts if none. It never moves backwards.
 */
uint64_t db_watermark(db_t* db);

/* The GC compaction filter factory for relation relid. */
std::shared_ptr<rocksdb::CompactionFilterFactory>
gc_filter_factory(db_t* db, uint64_t relid);

/* Start the GC worker thread. */
void gc_start(db_t* db);

/* Stop the GC worker thread and wait for it. */
void gc_stop(db_t* db);

/* Queue relids dropped by a transaction committed at commit_ts. */
void gc_queue_drops(db_t* db, uint64_t commit_ts,
                    const std::vector<uint64_t>& relids);

/*
 * Do the GC work that is due now: purge queued tombstones, drop
 * relations whose drop the watermark has passed, and advance
 * the CF:txn sweep once CF:txn has grown past its threshold.
 * The worker calls this; tests call it to run GC synchronously.
 */
void gc_run_once(db_t* db);

/*
 * Advance the CF:txn sweep (§8.3). With none pending and start
 * set, run the compaction pass and flip the read epoch. With
 * one pending, delete the swept COMMITTED records once the old
 * epoch's readers are gone. Returns the number of records
 * deleted when a sweep completes, 0 if one is still pending,
 * and -1 if the pass failed or could not be trusted.
 */
int64_t gc_sweep_txn(db_t* db, bool start);

/*
 * Apply opt->rocksdb_options, if any, on top of *out. Returns 0,
 * or -1 with SM_ERR_INVALID if the string does not parse.
 */
int db_options(const sm_options_t* opt, rocksdb::DBOptions* out);

} // namespace wisr

#endif /* WISR_DB_H */
