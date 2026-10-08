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
}

#endif /* SM_OPTIONS_H */
