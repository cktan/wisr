/*
 * sm_options.h -- sm1's database options, in C.
 *
 * sm1.h builds sm1::options_t on this struct, and sm.h takes it
 * from C callers, so both see one layout and one set of
 * defaults. It belongs to no level, so it lives at the root.
 *
 * Database options, taken by mkdb and by the opendb that first
 * opens a database. NULL means all defaults; sm_options_init()
 * fills a struct with them, and a default-constructed
 * sm1::options_t holds them too.
 *
 * durability_sync = 0 trades a bounded window of committed-
 * transaction loss on power failure for throughput. The
 * database stays crash-CONSISTENT; it is no longer crash-
 * DURABLE. It does not weaken isolation.
 *
 * group_commit_us is accepted but currently unused: concurrent
 * commits already share one synced WAL write through RocksDB's
 * write group. commit_cache_entries is the number of slots,
 * rounded up to even, in the in-memory cache from transaction
 * id to commit timestamp that spares readers a status-table
 * lookup; a slot is 16 bytes, and it must be at least 1.
 *
 * savepoint_undo_bytes caps what one transaction keeps in
 * memory to roll back to its savepoints. A transaction that
 * writes more than that while a savepoint is set keeps
 * writing, but those savepoints can no longer be rolled back
 * to: sm1::rollback fails with SM_ERR_UNDOLIMIT and the
 * transaction can only be aborted. It must be at least 1.
 *
 * idle_close_ms is how long a database stays open after its last
 * session closes, so a session that comes in meanwhile finds it
 * open; 0 closes it at once. It must not be negative.
 *
 * The three load_ options are a bulk load's (BULKLOAD.md section
 * 5), each at least 1. load_small_bytes is the size below which a
 * load is written as one batch and not as files; load_sort_bytes
 * is the memory a load sorts in before it writes a run to disk;
 * load_file_bytes is the size at which a sorted file is closed and
 * the next begun.
 */
#ifndef SM_OPTIONS_H
#define SM_OPTIONS_H

typedef struct sm_options_t {
    int         durability_sync;
    int         group_commit_us;       /* unused */
    int         deadlock_ms;
    int         ssi_max_entries;       /* unused */
    long long   ts_lease;
    const char* rocksdb_options;       /* NULL = defaults */
    int         commit_cache_entries;
    long long   savepoint_undo_bytes;
    long long   idle_close_ms;
    long long   load_small_bytes;
    long long   load_sort_bytes;
    long long   load_file_bytes;
} sm_options_t;

/* Fill opt with the defaults. */
static inline void sm_options_init(sm_options_t* opt) {
    opt->durability_sync      = 1;
    opt->group_commit_us      = 200;
    opt->deadlock_ms          = 50;
    opt->ssi_max_entries      = 1 << 20;
    opt->ts_lease             = 1000000;
    opt->rocksdb_options      = 0;
    opt->commit_cache_entries = 65536;
    opt->savepoint_undo_bytes = 64LL << 20;
    opt->idle_close_ms        = 60000;
    opt->load_small_bytes     = 4LL << 20;
    opt->load_sort_bytes      = 256LL << 20;
    opt->load_file_bytes      = 256LL << 20;
}

/*
 * Environment options, taken by attach: what every database of
 * the home directory shares. NULL means all defaults, which
 * sm_env_options_init() fills a struct with.
 *
 * block_cache_bytes is the size of the one block cache every
 * RocksDB instance of the environment reads its sorted files
 * through, every column family of each, as PostgreSQL's
 * shared_buffers is the one pool of a cluster (sm1/SM1_DESIGN.md
 * section 12.2). It must be at least 1.
 */
typedef struct sm_env_options_t {
    long long   block_cache_bytes;
} sm_env_options_t;

/* Fill opt with the defaults. */
static inline void sm_env_options_init(sm_env_options_t* opt) {
    opt->block_cache_bytes = 128LL << 20;
}

#endif /* SM_OPTIONS_H */
