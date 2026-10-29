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

#include <rocksdb/cache.h>
#include <rocksdb/compaction_filter.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "memcount.h"

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

/*
 * The first object id handed out: 1 to 999 are reserved for
 * objects a database is made with (SM1_DESIGN.md §4.2).
 */
#define WISR_FIRST_OBJID 1000

/* CF:meta key prefix of a sequence's lease: "seq." | be64(seqid). */
#define WISR_META_SEQ_PREFIX "seq."

namespace wisr {

struct ssi_range_t;

/*
 * A store as the engine reaches it: the RocksDB instance that holds
 * it, and its column family there (TEMPTABLE.md §3.1). cf is NULL
 * where there is no store.
 */
struct store_t {
  rocksdb::DB *rdb = nullptr;
  rocksdb::ColumnFamilyHandle *cf = nullptr;
  /* The session a temporary store belongs to; NULL for any other. */
  sm1::session_t *owner = nullptr;
  /*
   * The bytes its writes put in its memtables, by key prefix
   * (memcount.h): shared, so a writer holding this copy may count
   * into it though the store is dropped meanwhile.
   */
  std::shared_ptr<memcount_t> mem;
  /* Its sorted files' bytes as last estimated (memcount.h): shared. */
  std::shared_ptr<filesize_t> files;
  /*
   * Made chained (sm1::mkstore, ../VCHAIN.md §3): its CF is named
   * chain.<storeid>. next_old is the next id of an old version,
   * started at open past the store's last one: shared, as store_t
   * is copied.
   */
  bool chained = false;
  std::shared_ptr<std::atomic<uint64_t>> next_old;
};

/*
 * Count a write that put bytes into store's memtable under the user
 * key userkey: its version's key and value, or a tombstone's key
 * (memcount.h).
 */
inline void count_write(const store_t &store, std::string_view userkey,
                        uint64_t bytes) {
  if (store.mem) {
    store.mem->add(userkey, bytes);
  }
}

/*
 * The options of a write to store: a temporary store's writes are
 * never logged (TEMPTABLE.md §2).
 */
inline rocksdb::WriteOptions store_write_options(const store_t &store) {
  rocksdb::WriteOptions wo;
  wo.disableWAL = store.owner != nullptr;
  return wo;
}

/*
 * A SERIALIZABLE transaction in the SSI tracker (§7, Cahill et
 * al. 2008). It outlives its transaction's commit, as its SIREAD
 * entries must, and is freed once nothing concurrent remains.
 */
struct ssi_node_t {
  uint64_t snapshot_ts = 0;
  uint64_t xid = 0;       /* 0 while it has written nothing */
  bool committed = false; /* passed its commit check */
  uint64_t end_ts = 0;    /* once committed: when it ended */
  /* Declared read-only, or committed having written nothing. */
  bool read_only = false;
  /* Its rw-edges, by the node at the other end (§7.2): readers
     of what it wrote, and writers of what it read. */
  std::vector<ssi_node_t *> in_edges;
  std::vector<ssi_node_t *> out_edges;
  /* The safe-snapshot watch (§7.4): a declared read-only node
     waits on the writers concurrent with its snapshot, each of
     which lists it among its watchers. Once safe, the node holds
     no SIREAD entry and makes no edge; once unsafe, never safe. */
  std::vector<ssi_node_t *> watching;
  std::vector<ssi_node_t *> watchers;
  bool safe = false;
  bool unsafe = false;
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
 * A lock table key (§5.1): hi is the storeid, its top bit flipped
 * for an OID lock so the two types never meet; lo is the oid of an
 * OID lock, exact, or a hash of a row lock's key. Two rows of a
 * store whose keys hash alike share an entry: a false conflict,
 * never a missed one. The exact key is in the holder's lock list.
 */
struct lock_key_t {
  uint64_t hi;
  uint64_t lo;
  bool operator==(const lock_key_t &o) const {
    return hi == o.hi && lo == o.lo;
  }
};

/* The lock table's hash of a key: lo is an oid or a hash already. */
struct lock_key_hash_t {
  size_t operator()(const lock_key_t &k) const {
    return k.lo ^ (k.hi * 0x9e3779b97f4a7c15ULL);
  }
};

/*
 * One open database, shared by every session on it and held
 * in the environment's registry (SM1_DESIGN.md §1).
 */
struct db_t {
  std::string name;
  rocksdb::DB *rdb = nullptr;
  /* The environment's block cache, every CF of rdb's reads through. */
  std::shared_ptr<rocksdb::Cache> block_cache;
  /*
   * The environment's temporary instance and this database's dbid,
   * which names its stores there (TEMPTABLE.md §2). Set once a
   * session opens the database; a database opened only to be
   * recovered has neither.
   */
  rocksdb::DB *tmp = nullptr;
  uint64_t dbid = 0;
  /*
   * The listeners of rdb's and tmp's memtables, which keep each
   * store's counts in step with RocksDB's sealing and flushing
   * (memcount.h). tmp's is the environment's, set with tmp.
   */
  std::shared_ptr<memcount_listener_t> listener;
  memcount_listener_t *tmp_listener = nullptr;
  /* Whether a temporary store was ever made; guarded by store_mu. */
  bool had_temp = false;
  rocksdb::ColumnFamilyHandle *cf_default = nullptr;
  rocksdb::ColumnFamilyHandle *cf_txn = nullptr;
  rocksdb::ColumnFamilyHandle *cf_meta = nullptr;
  int durability_sync = 1;
  int nsessions = 0; /* guarded by the env's mu */
  /*
   * How long the database stays open with no session, and since when
   * it has had none (sm1::closedb), both read under the env's mu.
   */
  long long idle_close_ms = 0;
  std::chrono::steady_clock::time_point idle_since;
  /*
   * The options it was opened with, its RocksDB options string apart,
   * so a session asking for others reopens it once it is idle.
   */
  sm1::options_t opts;
  std::string rocksdb_options;

  /* storeid -> its instance and CF handle, for every live store CF. */
  std::shared_mutex store_mu;
  std::unordered_map<uint64_t, store_t> stores;
  /*
   * Raised by one at every change to stores, before a dropped
   * store's handle is destroyed: a session's handles read at
   * another are read again (STORECACHE.md). Loaded, not locked.
   */
  std::atomic<uint64_t> stores_version{0};

  /* The ts and object-id allocators and their leases (§4.2). */
  std::mutex alloc_mu;
  /*
   * Signalled, under alloc_mu, when visible_ts rises or the
   * database fails stop: a commit waits on it until its own
   * commit_ts is visible (db_commit_wait(), COMMITVISIBLE.md).
   */
  std::condition_variable visible_cv;
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
   * the first sm1::nextseq and kept until the database closes;
   * it is guarded by alloc_mu, like the leases above.
   */
  struct seq_state_t {
    uint64_t increment = 1;
    uint64_t next = 0;      /* next value to hand out */
    uint64_t limit = 0;     /* persisted; never handed out */
    bool exhausted = false; /* the last value is handed out */
  };
  std::unordered_map<uint64_t, seq_state_t> seqs;

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
   * The largest commit_ts of a transaction that made or dropped a
   * store, raised before that commit_ts is published: a session's
   * store cache trusts an entry only from below it (§11).
   */
  std::atomic<uint64_t> last_ddl_ts{0};

  /*
   * The active transaction table and the commit cache (§3.4),
   * under one mutex so a committing xid moves from one to the
   * other atomically. The cache holds commits as they happen
   * and commits read back from CF:txn, in sets of two slots: an
   * xid hashes to a set, and a new entry takes the slot of the
   * smaller xid (COMMITCACHE.md §2). An empty slot has xid 0,
   * which no transaction has.
   */
  std::mutex txn_mu;
  std::unordered_set<uint64_t> active; /* xids in flight */
  /* The transaction object running each xid in flight, for a
     writer that waits for it (WRITELOCK.md §3.4). */
  std::unordered_map<uint64_t, sm1::session_t *> xid_owner;
  struct cache_slot_t {
    uint64_t xid = 0;
    uint64_t commit_ts = 0;
  };
  std::vector<cache_slot_t> cache; /* cache_sets sets, two slots each */
  size_t cache_sets = 0;

  /* The most one transaction's undo list may hold (§6.5). */
  size_t undo_max_bytes = 0;

  /* A bulk load's sizes (BULKLOAD.md §5; sm_options.h). */
  size_t load_small_bytes = 0;
  size_t load_sort_bytes = 0;
  size_t load_file_bytes = 0;

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
   * The lock table (§5.1): lock key -> the transactions holding it.
   * Each holder holds it in a mode of its own, and every pair of
   * holders' modes is compatible (sm1.h, lockmode_t). One mutex
   * covers the table and every session's
   * waits-for list, so a deadlock walk sees a consistent
   * waits-for graph; shard it if a benchmark shows contention.
   * Every release and every doom notifies lock_cv.
   */
  struct lock_ent_t {
    /*
     * A session's hold: refs of its lock list's entries name this
     * key, the first at index idx; more than one only when row
     * keys hash alike. mode covers all of them.
     */
    struct holder_t {
      sm1::session_t *session;
      sm1::lockmode_t mode;
      uint32_t refs;
      size_t idx;
    };
    std::vector<holder_t> holders; /* never empty */
  };
  std::mutex lock_mu;
  std::condition_variable lock_cv;
  std::unordered_map<lock_key_t, lock_ent_t, lock_key_hash_t> lock_table;
  int deadlock_ms = 50;
  /*
   * Bumped, under lock_mu, by every rollback to a savepoint: it
   * deletes versions, so a writer waiting for the transaction
   * looks again (WRITELOCK.md §3.4).
   */
  uint64_t rollback_epoch = 0;

  /*
   * Write latches (WRITELOCK.md §3.1): a write holds the one its
   * store and key hash to while it checks the key and puts its
   * version, and never while it waits.
   */
  static constexpr size_t write_latches = 4096;
  std::mutex write_latch[write_latches];

  /* Hands out sm1::session_t::begin_seq. */
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
  std::unordered_set<uint64_t> snap_warned;
  int stuck_warn_secs = 60;

  /*
   * Garbage collection (§8.3). The compaction filter runs only
   * once gc_ready is set, after the handles above are valid.
   * It cannot write, so it queues tombstones to purge; a
   * committed sm1::rmstore queues its storeids to drop once the
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
  /*
   * Chained keys whose newest version is a dead transaction's (xid),
   * which the compaction filter found, for the worker to put back
   * (../VCHAIN.md §7). A key queued twice finds nothing to do the
   * second time.
   */
  struct repair_t {
    uint64_t storeid;
    std::string key;
    uint64_t xid;
  };
  std::vector<repair_t> repairs;
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
  std::unordered_set<ssi_node_t *> ssi_nodes;
  std::unordered_map<std::string, std::vector<ssi_node_t *>> ssi_points;
  std::unordered_map<uint64_t, std::list<ssi_range_t>>
      ssi_ranges; /* by storeid */
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
   * database-owned sm1::session_t holding the transaction; a
   * null value reserves a gid while sm1::prepare runs.
   */
  std::mutex prepared_mu;
  std::map<std::string, sm1::session_t *> prepared;

  /* Test hook, as test_before_commit, for the PREPARED record. */
  std::function<rocksdb::Status()> test_before_prepare;

  /* Destroy the CF handles and close RocksDB. */
  ~db_t();
};

/*
 * Open database dbname at path with every column family it
 * has, and load its allocators from CF:meta (SM1_DESIGN.md §9.1
 * steps 1-3), every column family reading through cache, the
 * environment's block cache, and its files written through renv,
 * the environment's Env, or RocksDB's default if NULL
 * (WALPREALLOC.md). Returns NULL with the error set.
 */
db_t *db_open(const std::string &path, const char *dbname,
              const sm1::options_t *opt,
              const std::shared_ptr<rocksdb::Cache> &cache,
              rocksdb::Env *renv = nullptr);

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

/*
 * Wait until commit_ts, done, is visible: visible_ts at or past
 * it, or the database failed stop (COMMITVISIBLE.md).
 */
void db_commit_wait(db_t *db, uint64_t commit_ts);

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

/*
 * Set *out to the last value sequence seqid handed out, without
 * handing one out or extending its lease. Before its first
 * sm1::nextseq in this process that is the value below its
 * persisted lease, an upper bound. Returns 0, 1 if it never
 * handed one out, or -1.
 */
int db_last_seq(db_t *db, uint64_t seqid, uint64_t start, uint64_t increment,
                uint64_t *out);

/* Forget sequence seqid and delete its lease from CF:meta. */
void db_forget_seq(db_t *db, uint64_t seqid);

/* The store storeid, its cf NULL if there is none. */
store_t db_store(db_t *db, uint64_t storeid);

/*
 * The CF handle of storeid alone, or NULL: for the isolation
 * scenarios in mvcc/, which are not edited to follow a rename.
 */
inline rocksdb::ColumnFamilyHandle *db_store_cf(db_t *db, uint64_t storeid) {
  return db_store(db, storeid).cf;
}

/*
 * Create the CF for storeid and register it: in the database's
 * instance, or with owner given, a temporary store's in the
 * temporary instance; with chained, a chained store's
 * (../VCHAIN.md §3). Returns 0 or -1.
 */
int db_create_store_cf(db_t *db, uint64_t storeid,
                       sm1::session_t *owner = nullptr, bool chained = false);

/* Drop storeid's CF and unregister it. Returns 0 or -1. */
int db_drop_store_cf(db_t *db, uint64_t storeid);

/*
 * The column family name of store storeid: store.<storeid>, or for a
 * chained store chain.<storeid>.
 */
std::string store_cf_name(uint64_t storeid, bool chained = false);

/*
 * How many range tombstones a store CF's memtable holds before
 * RocksDB flushes it: the tombstone purge's DeleteRanges cost
 * every overlapping read while they sit there (§8.3).
 */
constexpr uint32_t store_max_range_deletions = 1000;

/*
 * The size of a store CF's data blocks, as MyRocks sets it: four
 * times RocksDB's 4 KB, so a scan looks up a quarter as many
 * blocks in the block cache, and a point read is no slower (§12.2).
 */
constexpr size_t store_block_size = 16 << 10;

/*
 * Entries between a store CF's data block restart points, not
 * RocksDB's 16: a Get that the hash index lands on a restart point
 * reads a quarter as many entries to find its key (§12.2).
 */
constexpr int store_restart_interval = 4;

/*
 * Options for a fixed CF: RocksDB's defaults, LZ4, its
 * blocks read through cache when one is given.
 */
rocksdb::ColumnFamilyOptions
fixed_cf_options(const std::shared_ptr<rocksdb::Cache> &cache = nullptr);

/*
 * Options for a store CF: a fixed CF's, with the comparator,
 * prefix extractor, bloom filter, range-tombstone flush limit,
 * and -- when db is given -- the GC compaction filter for
 * storeid.
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
 * Queue storeid's chained key, whose newest version is xid's, a dead
 * transaction's, for the worker to put back (../VCHAIN.md §7;
 * chain.cc).
 */
void chain_report(db_t *db, uint64_t storeid, rocksdb::Slice key, uint64_t xid);

/* Put back each queued chained key (chain.cc). */
void chain_repair(db_t *db, const std::vector<db_t::repair_t> &repairs);

/*
 * The id after the store's last old version, for next_old: 1 if it
 * has none (chain.cc).
 */
uint64_t chain_next_old(rocksdb::DB *rdb, rocksdb::ColumnFamilyHandle *cf);

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
 * or -1 with SM_ERR_INVALID if the string does not parse.
 */
int db_options(const sm1::options_t *opt, rocksdb::DBOptions *out);

} // namespace wisr

#endif /* WISR_DB_H */
