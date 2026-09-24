/*
 * db.h -- the layout of one database directory: its column
 * families, their options, and CF:meta's keys (SM1_DESIGN.md §3).
 */
#ifndef WISR_DB_H
#define WISR_DB_H

#include "sm1.h"

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
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

/* Database format version, stored in CF:meta. Version 4 added
 * the mode byte on each lock key of a PREPARED record (§8.4);
 * version 6 renamed each store's column family from rel.<id>
 * to store.<id>. */
#define WISR_DB_VERSION 6

/* Column family names. */
#define WISR_CF_TXN "txn"
#define WISR_CF_META "meta"

/* CF:meta keys (SM1_DESIGN.md §3.3, §4.2). */
#define WISR_META_VERSION "version"
#define WISR_META_NEXT_TS "next_ts"
#define WISR_META_NEXT_OBJID "next_objid"

/* CF:meta key prefix of a sequence's lease: "seq." | be64(seqid). */
#define WISR_META_SEQ_PREFIX "seq."

namespace wisr {

struct ssi_range_t;

/*
 * A SERIALIZABLE transaction in the SSI tracker (§7, Cahill et
 * al. 2008). It outlives its transaction's commit, as its SIREAD
 * entries must, and is freed once nothing concurrent remains.
 */
struct ssi_node_t {
  uint64_t snapshot_ts = 0;
  uint64_t xid = 0;                /* 0 while it has written nothing */
  bool committed = false;          /* passed its commit check */
  uint64_t end_ts = 0;             /* once committed: when it ended */
  bool in_conflict = false;        /* some rw-edge points into it */
  bool out_conflict = false;       /* it has an rw-edge out */
  bool doomed = false;             /* must fail at its commit */
  std::vector<std::string> points; /* its SIREAD point keys */
  std::vector<std::pair<uint64_t, std::list<ssi_range_t>::iterator>>
      ranges; /* its SIREAD ranges, by storeid */
};

/* A range SIREAD entry: [lo, hi] with inclusive flags; empty = open. */
struct ssi_range_t {
  ssi_node_t *owner;
  bool has_lo, lo_incl;
  std::string lo;
  bool has_hi, hi_incl;
  std::string hi;
};

/*
 * One open database, shared by every session on it and held
 * in the environment's registry (SM1_DESIGN.md §1).
 */
struct db_t {
  std::string name;
  rocksdb::DB *rdb = nullptr;
  rocksdb::ColumnFamilyHandle *cf_default = nullptr;
  rocksdb::ColumnFamilyHandle *cf_txn = nullptr;
  rocksdb::ColumnFamilyHandle *cf_meta = nullptr;
  int durability_sync = 1;
  int nsessions = 0; /* guarded by the env's mu */

  /* storeid -> column family handle, for every live store CF. */
  std::shared_mutex store_mu;
  std::map<uint64_t, rocksdb::ColumnFamilyHandle *> stores;

  /* The ts and object-id allocators and their leases (§4.2). */
  std::mutex alloc_mu;
  uint64_t lease = 0;
  /*
   * The timestamp counter. Values are taken with fetch_add and
   * no lock, except a commit_ts, which is taken under alloc_mu
   * with its insert into committing (§4.3). ts_limit is written
   * only under alloc_mu.
   */
  std::atomic<uint64_t> global_ts{0}; /* next ts to hand out */
  std::atomic<uint64_t> ts_limit{0};  /* persisted; never handed out */
  uint64_t next_objid = 0;            /* next storeid or seqid to hand out */
  uint64_t objid_limit = 0;

  /*
   * A sequence in use, by seqid (§11). The state is built on
   * the first sm1_nextseq and kept until the database closes;
   * it is guarded by alloc_mu, like the leases above.
   */
  struct seq_state_t {
    uint64_t increment = 1;
    uint64_t next = 0;      /* next value to hand out */
    uint64_t limit = 0;     /* persisted; never handed out */
    bool exhausted = false; /* the last value is handed out */
  };
  std::map<uint64_t, seq_state_t> seqs;

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
   * DDL fail with SM1_ERR_IO from then on.
   */
  std::atomic<bool> failed{false};

  /*
   * Test hook: called after a commit claims its commit_ts and
   * before it writes the status record. A non-OK status stands
   * in for the write's failure. Empty outside tests.
   */
  std::function<rocksdb::Status(uint64_t commit_ts)> test_before_commit;

  /*
   * Test hook: called inside enter_cache just before the commit
   * cache insert, after the commit point. Throwing std::bad_alloc
   * stands in for that insert running out of memory. Empty
   * outside tests.
   */
  std::function<void()> test_cache_insert;

  /*
   * Row locks (§5.1): lock key -> the transactions holding it.
   * A lock is held SHARED by any number of them or EXCLUSIVE by
   * one. One mutex covers the table and every session's
   * waits-for list, so a deadlock walk sees a consistent
   * waits-for graph; shard it if a benchmark shows contention.
   * Every release and every doom notifies lock_cv.
   */
  struct lock_ent_t {
    bool exclusive = false;               /* holders is then one */
    std::vector<sm1_session_t *> holders; /* never empty */
  };
  std::mutex lock_mu;
  std::condition_variable lock_cv;
  std::unordered_map<std::string, lock_ent_t> lock_table;
  int deadlock_ms = 50;

  /* Hands out sm1_session_t::begin_seq. */
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
   * When each registered snapshot was taken, and those already
   * warned about, for the warning about a snapshot holding back
   * GC for over stuck_warn_secs. Guarded by snap_mu.
   */
  std::multimap<uint64_t, std::chrono::steady_clock::time_point> snap_since;
  std::set<uint64_t> snap_warned;
  int stuck_warn_secs = 60;

  /*
   * Garbage collection (§8.3). The compaction filter runs only
   * once gc_ready is set, after the handles above are valid.
   * It cannot write, so it queues tombstones to purge; a
   * committed sm1_rmstore queues its storeids to drop once the
   * watermark passes its commit. The worker drains both.
   */
  std::atomic<bool> gc_ready{false};
  std::mutex gc_mu;
  std::condition_variable gc_cv;
  bool gc_stopping = false;
  struct purge_t {
    uint64_t storeid;
    std::string key; /* the tombstone's version key */
  };
  std::vector<purge_t> purges;
  std::multimap<uint64_t, uint64_t> drops; /* commit_ts -> storeid */
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

  /*
   * The SSI tracker (§7), for SERIALIZABLE transactions only:
   * their nodes, SIREAD entries on points and ranges, and the
   * map from a writer's xid to its node. One mutex covers all
   * of it. A committed node stays until the GC watermark
   * passes its end, when no transaction concurrent with it
   * remains.
   */
  std::mutex ssi_mu;
  std::set<ssi_node_t *> ssi_nodes;
  std::unordered_map<std::string, std::vector<ssi_node_t *>> ssi_points;
  std::map<uint64_t, std::list<ssi_range_t>> ssi_ranges; /* by storeid */
  std::unordered_map<uint64_t, ssi_node_t *> ssi_by_xid;

  /*
   * Restored prepared transactions, xid -> commit_ts (0 while
   * in doubt), for §8.4's rule: a serializable reader that
   * steps past one of their versions is doomed. An entry stays
   * after commit until the watermark passes its commit_ts.
   * Guarded by ssi_mu.
   */
  std::unordered_map<uint64_t, uint64_t> ssi_restored;

  /*
   * Prepared transactions by gid (§8.4, §8.5). Each value is a
   * database-owned sm1_session_t holding the transaction; a
   * null value reserves a gid while sm1_prepare runs.
   */
  std::mutex prepared_mu;
  std::map<std::string, sm1_session_t *> prepared;

  /* Test hook, as test_before_commit, for the PREPARED record. */
  std::function<rocksdb::Status()> test_before_prepare;

  /*
   * Test hook: called right after the PREPARED record is written,
   * on the preparing thread. Empty outside tests.
   */
  std::function<void()> test_after_prepare;

  /* Destroy the CF handles and close RocksDB. */
  ~db_t();
};

/*
 * Open database dbname at path with every column family it
 * has, and load its allocators from CF:meta (SM1_DESIGN.md §9.1
 * steps 1-3). Returns NULL with the error set.
 */
db_t *db_open(const std::string &path, const char *dbname,
              const sm1_options_t *opt);

/* Allocate a timestamp: lock-free within the lease. Returns 0 or -1. */
int db_alloc_ts(db_t *db, uint64_t *out);

/*
 * Claim a commit_ts: allocate it and enter it into the
 * sequencer, under one lock (§4.3). Returns 0 or -1.
 */
int db_claim_commit_ts(db_t *db, uint64_t *out);

/*
 * Mark commit_ts done: leave the sequencer and advance
 * visible_ts to one below the oldest commit still in progress.
 */
void db_commit_done(db_t *db, uint64_t commit_ts);

/* Allocate an object id -- a storeid or a seqid. Returns 0 or -1. */
int db_alloc_objid(db_t *db, uint64_t *out);

/*
 * Hand out sequence seqid's next value, extending and
 * persisting its lease when it runs out. increment and start
 * describe the sequence, as its catalog row does. Returns 0
 * or -1.
 */
int db_next_seq(db_t *db, uint64_t seqid, uint64_t start, uint64_t increment,
                uint64_t *out);

/* Forget sequence seqid and delete its lease from CF:meta. */
void db_forget_seq(db_t *db, uint64_t seqid);

/* The CF handle of storeid, or NULL if it has none. */
rocksdb::ColumnFamilyHandle *db_store_cf(db_t *db, uint64_t storeid);

/* Create the CF for storeid and register it. Returns 0 or -1. */
int db_create_store_cf(db_t *db, uint64_t storeid);

/* Drop storeid's CF and unregister it. Returns 0 or -1. */
int db_drop_store_cf(db_t *db, uint64_t storeid);

/* The column family name of store storeid: store.<storeid>. */
std::string store_cf_name(uint64_t storeid);

/*
 * Options for a store CF: comparator, prefix extractor,
 * bloom filter, and -- when db is given -- the GC compaction
 * filter for storeid.
 */
rocksdb::ColumnFamilyOptions store_cf_options(db_t *db = nullptr,
                                              uint64_t storeid = 0);

/* Enter the current read epoch; returns it for db_read_leave. */
uint64_t db_read_enter(db_t *db);

/* Leave the read epoch returned by db_read_enter. */
void db_read_leave(db_t *db, uint64_t epoch);

/* Take a transaction's begin snapshot and register it (§8.3). */
uint64_t db_take_snapshot(db_t *db);

/* Register a snapshot taken earlier, such as a restored one's. */
void db_register_snapshot(db_t *db, uint64_t snapshot_ts);

/* Unregister a snapshot taken by db_take_snapshot. */
void db_release_snapshot(db_t *db, uint64_t snapshot_ts);

/*
 * gc_watermark (§8.3): the oldest registered snapshot, or
 * visible_ts if none. It never moves backwards.
 */
uint64_t db_watermark(db_t *db);

/* The GC compaction filter factory for store storeid. */
std::shared_ptr<rocksdb::CompactionFilterFactory>
gc_filter_factory(db_t *db, uint64_t storeid);

/* Start the GC worker thread. */
void gc_start(db_t *db);

/* Stop the GC worker thread and wait for it. */
void gc_stop(db_t *db);

/* Queue storeids dropped by a transaction committed at commit_ts. */
void gc_queue_drops(db_t *db, uint64_t commit_ts,
                    const std::vector<uint64_t> &storeids);

/*
 * Do the GC work that is due now: purge queued tombstones, drop
 * stores whose drop the watermark has passed, and advance
 * the CF:txn sweep once CF:txn has grown past its threshold.
 * The worker calls this; tests call it to run GC synchronously.
 */
void gc_run_once(db_t *db);

/*
 * Advance the CF:txn sweep (§8.3). With none pending and start
 * set, run the compaction pass and flip the read epoch. With
 * one pending, delete the swept COMMITTED records once the old
 * epoch's readers are gone. Returns the number of records
 * deleted when a sweep completes, 0 if one is still pending,
 * and -1 if the pass failed or could not be trusted.
 */
int64_t gc_sweep_txn(db_t *db, bool start);

/*
 * Apply opt->rocksdb_options, if any, on top of *out. Returns 0,
 * or -1 with SM1_ERR_INVALID if the string does not parse.
 */
int db_options(const sm1_options_t *opt, rocksdb::DBOptions *out);

} // namespace wisr

#endif /* WISR_DB_H */
