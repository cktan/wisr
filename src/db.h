/*
 * db.h -- the layout of one database directory: its column
 * families, their options, and CF:meta's keys (DESIGN.md §3).
 */
#ifndef WISR_DB_H
#define WISR_DB_H

#include "sm.h"

#include <stdint.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

/* Options for a relation CF: comparator, prefix extractor, bloom. */
rocksdb::ColumnFamilyOptions rel_cf_options();

/*
 * Apply opt->rocksdb_options, if any, on top of *out. Returns 0,
 * or -1 with SM_ERR_INVALID if the string does not parse.
 */
int db_options(const sm_options_t* opt, rocksdb::DBOptions* out);

} // namespace wisr

#endif /* WISR_DB_H */
