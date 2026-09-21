/*
 * db.cc -- sm_options_init, sm_mkdb, sm_rmdb, sm_lsdb, and the
 * column family layout every database shares (DESIGN.md §3,
 * §3.5, §11).
 */

#include "db.h"

#include "codec.h"
#include "defer.h"
#include "env.h"
#include "error.h"
#include "log.h"
#include "session.h"
#include "strlist.h"
#include "version.h"

#include <inttypes.h>
#include <stdio.h>

#include <memory>
#include <new>
#include <vector>

#include <rocksdb/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace {

/* The root db key naming dbname: "D" | dbname. */
std::string dbname_key(const char *dbname) {
  return std::string(WISR_ROOT_DBNAME_PREFIX) + dbname;
}

/*
 * Create a database at path: the directory, CF:txn, CF:meta
 * with its initial contents, and the catalog CF:rel.0.
 */
int create_db(const std::string &path, rocksdb::DBOptions dbo) {
  // Create the directory and every column family, fresh only.
  dbo.create_if_missing = true;
  dbo.create_missing_column_families = true;
  dbo.error_if_exists = true;

  // The fixed CFs: default, CF:txn, CF:meta, and the catalog.
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
      {rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()},
      {WISR_CF_TXN, rocksdb::ColumnFamilyOptions()},
      {WISR_CF_META, rocksdb::ColumnFamilyOptions()},
      {rel_cf_name(0), rel_cf_options()},
  };
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::DB *db = nullptr;
  // Create the RocksDB instance with every fixed column family.
  rocksdb::Status s = rocksdb::DB::Open(dbo, path, cfs, &handles, &db);
  // Fail if RocksDB could not create the database.
  if (!s.ok()) {
    return set_status_error(s, path.c_str());
  }
  // On the way out, destroy the handles, then close the database:
  // sm_mkdb leaves it unopened.
  defer {
    for (rocksdb::ColumnFamilyHandle *h : handles) {
      db->DestroyColumnFamilyHandle(h);
    }
    db->Close();
    delete db;
  };

  // Seed CF:meta: format version and the allocators' leases. A
  // persisted lease of 0 resumes both allocators at 1 (§4.2),
  // so relid 0 stays the catalog's.
  std::string version, zero;
  put_be64(version, WISR_DB_VERSION);
  put_be64(zero, 0);
  rocksdb::WriteBatch batch;
  batch.Put(handles[2], WISR_META_VERSION, version);
  batch.Put(handles[2], WISR_META_NEXT_TS, zero);
  batch.Put(handles[2], WISR_META_NEXT_OBJID, zero);
  // Write the seed synced.
  rocksdb::WriteOptions wo;
  wo.sync = true;
  // Write the batch.
  s = db->Write(wo, &batch);
  // Fail if the seed did not make it.
  if (!s.ok()) {
    return set_status_error(s, path.c_str());
  }
  return 0;
}

/* sm_mkdb, minus the out-of-memory guard. */
int mkdb(sm_env_t *env, const char *dbname, const sm_options_t *opt) {
  // Reject a NULL environment.
  if (!env) {
    return set_error(SM_ERR_INVALID, "sm_mkdb: env is NULL");
  }
  // Validate the database name.
  if (check_name(dbname, SM_MAX_DBNAME, "database") != 0) {
    return -1;
  }
  // Parse the pass-through RocksDB options.
  rocksdb::DBOptions dbo;
  if (db_options(opt, &dbo) != 0) {
    return -1;
  }

  // Hold the env mutex for the root db and the registry.
  std::lock_guard<std::mutex> guard(env->mu);
  // Look the name up in the root db.
  std::string dkey = dbname_key(dbname);
  std::string val;
  rocksdb::Status s = env->root->Get(rocksdb::ReadOptions(), dkey, &val);
  // Refuse a name already taken.
  if (s.ok()) {
    return set_error(SM_ERR_EXISTS, "database {} already exists", dbname);
  }
  // Fail on any other root db error.
  if (!s.IsNotFound()) {
    return set_status_error(s, "read root db");
  }

  // Allocate a dbid, durably, before its directory exists, so
  // a dbid is never reused (§3.5).
  s = env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_NEXT_DBID, &val);
  // Fail if the root db has no usable next_dbid.
  if (!s.ok() || val.size() != 8) {
    return set_error(SM_ERR_CORRUPT, "root db has no next_dbid");
  }
  // Take the next dbid.
  uint64_t dbid = get_be64(val.data());
  // Advance next_dbid past it.
  std::string next;
  put_be64(next, dbid + 1);
  // Persist the advance synced.
  rocksdb::WriteOptions wo;
  wo.sync = true;
  s = env->root->Put(wo, WISR_ROOT_NEXT_DBID, next);
  // Fail if the advance did not persist.
  if (!s.ok()) {
    return set_status_error(s, "write root db");
  }

  // Build the database. On failure its directory is unnamed,
  // and the next sm_attach sweeps it.
  if (create_db(env_db_path(env, dbid), dbo) != 0) {
    return -1;
  }

  // Name the database last: the mapping is the truth (§3.5).
  std::string dbid_val;
  // Encode the dbid as the name's value.
  put_be64(dbid_val, dbid);
  // Write the name, synced.
  s = env->root->Put(wo, dkey, dbid_val);
  // Fail if the name did not persist.
  if (!s.ok()) {
    return set_status_error(s, "write root db");
  }
  return 0;
}

/* sm_rmdb, minus the out-of-memory guard. */
int rmdb(sm_env_t *env, const char *dbname) {
  // Reject a NULL environment.
  if (!env) {
    return set_error(SM_ERR_INVALID, "sm_rmdb: env is NULL");
  }
  // Validate the database name.
  if (check_name(dbname, SM_MAX_DBNAME, "database") != 0) {
    return -1;
  }

  // Hold the env mutex for the root db and the registry.
  std::lock_guard<std::mutex> guard(env->mu);
  // Refuse while any session has the database open.
  if (env->dbs.count(dbname)) {
    return set_error(SM_ERR_INUSE, "database {} is open", dbname);
  }
  // Look the name up in the root db.
  std::string dkey = dbname_key(dbname);
  std::string val;
  rocksdb::Status s = env->root->Get(rocksdb::ReadOptions(), dkey, &val);
  // Refuse a name that does not exist.
  if (s.IsNotFound()) {
    return set_error(SM_ERR_NOTFOUND, "database {} does not exist", dbname);
  }
  // Fail on any other root db error.
  if (!s.ok()) {
    return set_status_error(s, "read root db");
  }
  // Reject a malformed dbid.
  if (val.size() != 8) {
    return set_error(SM_ERR_CORRUPT, "bad dbid for database {}", dbname);
  }
  // Decode the dbid.
  uint64_t dbid = get_be64(val.data());

  // Drop the name first: the mapping is the truth (§3.5).
  rocksdb::WriteOptions wo;
  wo.sync = true;
  // Delete the name, synced.
  s = env->root->Delete(wo, dkey);
  // Fail if the delete did not persist.
  if (!s.ok()) {
    return set_status_error(s, "write root db");
  }

  // Destroy the directory. If this fails the database is
  // already gone by name, and the next sm_attach sweeps the
  // directory; the error still reports that it is left.
  return env_destroy_db_dir(env_db_path(env, dbid));
}

/* sm_lsdb, minus the out-of-memory guard. */
int lsdb(sm_env_t *env, sm_strlist_t **out) {
  // Reject NULL arguments.
  if (!env || !out) {
    return set_error(SM_ERR_INVALID, "sm_lsdb: env or out is NULL");
  }
  // Collect every database name.
  std::vector<std::string> names;
  {
    // Hold the env mutex while scanning the root db.
    std::lock_guard<std::mutex> guard(env->mu);
    std::unique_ptr<rocksdb::Iterator> it(
        env->root->NewIterator(rocksdb::ReadOptions()));
    // Walk the "D" rows in bytewise order.
    for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
         it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
         it->Next()) {
      // Strip the "D" prefix from the key.
      rocksdb::Slice k = it->key();
      k.remove_prefix(1);
      // Keep the name.
      names.push_back(k.ToString());
    }
    // Fail if the scan failed.
    if (!it->status().ok()) {
      return set_status_error(it->status(), "scan root db");
    }
  }
  // Pack the names into one list.
  sm_strlist_t *sl = strlist_make(names);
  // Fail if it could not be allocated.
  if (!sl) {
    return set_error(SM_ERR_NOMEM, "sm_lsdb: out of memory");
  }
  // Hand the list to the caller.
  *out = sl;
  return 0;
}

} // namespace

namespace wisr {

/* rel.<relid>, in decimal. */
std::string rel_cf_name(uint64_t relid) {
  return "rel." + std::to_string(relid);
}

/*
 * The comparator and prefix extractor are part of the format
 * (§3.1). The bloom filter keys on the userkey prefix; whole
 * version keys are never looked up, so no whole-key filter. No
 * row_cache: DeleteRange, which the tombstone purge uses, is not
 * supported with one (§8.3).
 */
rocksdb::ColumnFamilyOptions rel_cf_options(db_t *db, uint64_t relid) {
  // Start from default CF options.
  rocksdb::ColumnFamilyOptions cfo;
  // Attach the GC filter when opening a real database.
  if (db) {
    cfo.compaction_filter_factory = gc_filter_factory(db, relid);
  }
  // Order versions by the wisr.version.v1 comparator.
  cfo.comparator = version_comparator();
  // Key bloom filters on the userkey.
  cfo.prefix_extractor = version_prefix_extractor();
  // Build a table with a userkey bloom filter.
  rocksdb::BlockBasedTableOptions bbto;
  // Ten bits per key.
  bbto.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  // Whole version keys are never looked up.
  bbto.whole_key_filtering = false;
  // Install the table factory.
  cfo.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbto));
  return cfo;
}

/* Parse the pass-through RocksDB option string onto *out. */
int db_options(const sm_options_t *opt, rocksdb::DBOptions *out) {
  // No option string: keep the defaults.
  if (!opt || !opt->rocksdb_options) {
    return 0;
  }
  rocksdb::ConfigOptions cfg;
  rocksdb::DBOptions parsed;
  // Parse the string over the current options.
  rocksdb::Status s =
      rocksdb::GetDBOptionsFromString(cfg, *out, opt->rocksdb_options, &parsed);
  // Reject an unparsable string.
  if (!s.ok()) {
    return set_error(SM_ERR_INVALID, "rocksdb_options: {}",
                     s.ToString().c_str());
  }
  // Adopt the parsed options.
  *out = parsed;
  return 0;
}

/* Parse "rel.<relid>"; false if name is not a relation CF's. */
static bool parse_rel_cf_name(const std::string &name, uint64_t *relid) {
  // Require the "rel." prefix, a number, and a canonical spelling.
  return name.compare(0, 4, "rel.") == 0 &&
         sscanf(name.c_str() + 4, "%" SCNu64, relid) == 1 &&
         rel_cf_name(*relid) == name;
}

/*
 * Read an 8-byte CF:meta value; false if missing or malformed.
 * The key is a slice, because a sequence's lease key carries an
 * 8-byte id whose bytes include NUL.
 */
static bool read_meta(db_t *db, rocksdb::Slice key, uint64_t *out) {
  std::string val;
  // Read the key from CF:meta.
  rocksdb::Status s =
      db->rdb->Get(rocksdb::ReadOptions(), db->cf_meta, key, &val);
  // Fail if missing or not 8 bytes.
  if (!s.ok() || val.size() != 8) {
    return false;
  }
  // Decode the value.
  *out = get_be64(val.data());
  return true;
}

/* Open every column family of the database and load CF:meta. */
db_t *db_open(const std::string &path, const char *dbname,
              const sm_options_t *opt) {
  sm_options_t defaults;
  // No options: use the defaults.
  if (!opt) {
    sm_options_init(&defaults);
    opt = &defaults;
  }
  // Reject an empty commit cache.
  if (opt->commit_cache_entries < 1) {
    set_error(SM_ERR_INVALID, "commit_cache_entries must be at least 1");
    return nullptr;
  }
  // Reject a non-positive deadlock timeout.
  if (opt->deadlock_ms <= 0) {
    set_error(SM_ERR_INVALID, "deadlock_ms must be positive");
    return nullptr;
  }
  // Reject a non-positive timestamp lease.
  if (opt->ts_lease <= 0) {
    set_error(SM_ERR_INVALID, "ts_lease must be positive");
    return nullptr;
  }
  rocksdb::DBOptions dbo;
  // Parse the pass-through RocksDB options.
  if (db_options(opt, &dbo) != 0) {
    return nullptr;
  }

  std::vector<std::string> names;
  // List the column families the database has.
  rocksdb::Status s = rocksdb::DB::ListColumnFamilies(dbo, path, &names);
  // Fail if the database cannot be read.
  if (!s.ok()) {
    set_status_error(s, path.c_str());
    return nullptr;
  }
  // Allocate the shared database object.
  std::unique_ptr<db_t> db(new db_t);
  db->name = dbname;
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  // Give each relation CF its options, GC filter included.
  for (const std::string &name : names) {
    uint64_t relid;
    // A relation CF: comparator, prefix extractor, GC filter.
    if (parse_rel_cf_name(name, &relid)) {
      cfs.emplace_back(name, rel_cf_options(db.get(), relid));
    } else {
      // A fixed CF: default options.
      cfs.emplace_back(name, rocksdb::ColumnFamilyOptions());
    }
  }

  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  // Open RocksDB with every column family; this replays the WAL.
  s = rocksdb::DB::Open(dbo, path, cfs, &handles, &db->rdb);
  // Fail if RocksDB could not open it.
  if (!s.ok()) {
    set_status_error(s, path.c_str());
    return nullptr;
  }
  bool bad_name = false;
  // Sort each handle into its slot by name.
  for (size_t i = 0; i < handles.size(); i++) {
    const std::string &name = names[i];
    uint64_t relid;
    // The default CF, unused but always present.
    if (name == rocksdb::kDefaultColumnFamilyName) {
      db->cf_default = handles[i];
      // The status table.
    } else if (name == WISR_CF_TXN) {
      db->cf_txn = handles[i];
      // The bootstrap state.
    } else if (name == WISR_CF_META) {
      db->cf_meta = handles[i];
      // A relation's CF.
    } else if (parse_rel_cf_name(name, &relid)) {
      db->rels[relid] = handles[i];
    } else {
      // Unknown CF: release its handle and fail the open.
      db->rdb->DestroyColumnFamilyHandle(handles[i]);
      bad_name = true;
    }
  }
  // Require the fixed CFs and the catalog.
  if (bad_name || !db->cf_txn || !db->cf_meta || !db->rels.count(0)) {
    set_error(SM_ERR_CORRUPT, "{}: unexpected column families", dbname);
    return nullptr;
  }

  uint64_t version, next_ts, next_objid;
  // Read the format version and both leases.
  if (!read_meta(db.get(), WISR_META_VERSION, &version) ||
      !read_meta(db.get(), WISR_META_NEXT_TS, &next_ts) ||
      !read_meta(db.get(), WISR_META_NEXT_OBJID, &next_objid)) {
    set_error(SM_ERR_CORRUPT, "{}: CF:meta is incomplete", dbname);
    return nullptr;
  }
  // Reject a format this build does not understand.
  if (version != WISR_DB_VERSION) {
    set_error(SM_ERR_VERSION, "{}: format version {}, expected {}", dbname,
              version, WISR_DB_VERSION);
    return nullptr;
  }

  // Resume the allocators one past their persisted leases, and
  // publish everything below the ts lease (§4.2).
  db->lease = opt->ts_lease;
  db->durability_sync = opt->durability_sync;
  // Resume ts one past the lease; that value is never handed out.
  db->global_ts = next_ts + 1;
  db->ts_limit = next_ts + 1;
  // Everything below the lease is published.
  db->visible_ts = next_ts;
  // Resume object ids the same way.
  db->next_objid = next_objid + 1;
  db->objid_limit = next_objid + 1;
  // Size the commit cache.
  db->cache_entries = opt->commit_cache_entries;
  // Arm deadlock detection.
  db->deadlock_ms = opt->deadlock_ms;

  // Restore prepared transactions and sweep orphaned CFs
  // before any session or compaction filter can look (§9.1).
  if (recover(db.get()) != 0) {
    return nullptr;
  }

  // Everything the compaction filter reads is in place: let it
  // collect, and start the worker that purges and drops.
  db->gc_ready = true;
  // Start the GC worker.
  gc_start(db.get());
  return db.release();
}

/*
 * Hand out *next from a lease, with db->alloc_mu held. When the
 * lease is used up, persist a new limit before handing out any
 * value under it, so no value is handed out twice across a
 * restart (§4.2).
 */
static int persist_lease(db_t *db, const char *key, uint64_t new_limit) {
  // Encode it.
  std::string val;
  put_be64(val, new_limit);
  // The lease must be durable before any value under it.
  rocksdb::WriteOptions wo;
  wo.sync = true;
  // Persist the new lease.
  rocksdb::Status s = db->rdb->Put(wo, db->cf_meta, key, val);
  // Fail if the new lease did not persist.
  if (!s.ok()) {
    return set_status_error(s, "persist lease");
  }
  return 0;
}

static int alloc_leased_locked(db_t *db, uint64_t *next, uint64_t *limit,
                               const char *key, uint64_t *out) {
  // Lease used up: persist a new limit first.
  if (*next >= *limit) {
    // Extend the lease from here.
    uint64_t new_limit = *next + db->lease;
    // Persist the new lease.
    if (persist_lease(db, key, new_limit) != 0) {
      return -1;
    }
    // Adopt the new limit.
    *limit = new_limit;
  }
  // Hand out the next value.
  *out = (*next)++;
  return 0;
}

/* alloc_leased_locked under db->alloc_mu. */
static int alloc_leased(db_t *db, uint64_t *next, uint64_t *limit,
                        const char *key, uint64_t *out) {
  // Hold alloc_mu around the allocation.
  std::lock_guard<std::mutex> guard(db->alloc_mu);
  return alloc_leased_locked(db, next, limit, key, out);
}

/*
 * Make sure the persisted lease covers timestamp n, with alloc_mu
 * held. A value the lease does not cover is never handed out, so a
 * failure only burns n.
 */
static int cover_ts_locked(db_t *db, uint64_t n) {
  // Another thread may have extended the lease while we waited.
  if (n < db->ts_limit.load()) {
    return 0;
  }
  // Extend the lease from n.
  uint64_t new_limit = n + db->lease;
  // Persist the new lease before n is handed out.
  if (persist_lease(db, WISR_META_NEXT_TS, new_limit) != 0) {
    return -1;
  }
  // Adopt the new limit.
  db->ts_limit.store(new_limit);
  return 0;
}

/* Allocate a timestamp: a lock-free fetch_add, but for a lease renewal. */
int db_alloc_ts(db_t *db, uint64_t *out) {
  // Take the next value.
  uint64_t n = db->global_ts.fetch_add(1);
  // Past the persisted lease: renew it under alloc_mu first.
  if (n >= db->ts_limit.load()) {
    // Hold alloc_mu around the lease write.
    std::lock_guard<std::mutex> guard(db->alloc_mu);
    if (cover_ts_locked(db, n) != 0) {
      return -1;
    }
  }
  *out = n;
  return 0;
}

/* Allocate commit_ts and enter it into the sequencer atomically. */
int db_claim_commit_ts(db_t *db, uint64_t *out) {
  // Hold alloc_mu, the sequencer's mutex too.
  std::lock_guard<std::mutex> guard(db->alloc_mu);
  // Take the next value.
  uint64_t n = db->global_ts.fetch_add(1);
  // Cover it with the persisted lease.
  if (cover_ts_locked(db, n) != 0) {
    return -1;
  }
  *out = n;
  // Enter it into the sequencer's set in progress.
  db->committing.insert(n);
  return 0;
}

/*
 * Leave the sequencer and advance visible_ts: one below the
 * oldest commit in progress, or everything handed out so far if
 * none is. New values are always above what was handed out, so
 * visible_ts never moves backwards.
 */
void db_commit_done(db_t *db, uint64_t commit_ts) {
  // Hold alloc_mu, the sequencer's mutex.
  std::lock_guard<std::mutex> guard(db->alloc_mu);
  // This commit is no longer in progress.
  db->committing.erase(commit_ts);
  // Publish up to just below the oldest commit still in progress.
  uint64_t v = db->committing.empty() ? db->global_ts.load() - 1
                                      : *db->committing.begin() - 1;
  // Never move visible_ts backwards.
  if (v > db->visible_ts.load()) {
    db->visible_ts.store(v);
  }
}

/* Allocate an object id: a relid or a seqid. */
int db_alloc_objid(db_t *db, uint64_t *out) {
  return alloc_leased(db, &db->next_objid, &db->objid_limit,
                      WISR_META_NEXT_OBJID, out);
}

/* The CF:meta key holding sequence seqid's lease. */
static std::string seq_meta_key(uint64_t seqid) {
  std::string key(WISR_META_SEQ_PREFIX);
  put_be64(key, seqid);
  return key;
}

/* Hand out seqid's next value, persisting a new lease first if needed. */
int db_next_seq(db_t *db, uint64_t seqid, uint64_t start, uint64_t increment,
                uint64_t *out) {
  // Hold alloc_mu, which guards every lease.
  std::lock_guard<std::mutex> guard(db->alloc_mu);
  auto it = db->seqs.find(seqid);
  // First use in this process: recover the sequence's state.
  if (it == db->seqs.end()) {
    db_t::seq_state_t st;
    st.increment = increment;
    uint64_t limit;
    // A persisted lease means values were handed out before, so
    // resume above it; the rest of that block is skipped, as a
    // crash skips one.
    st.next = read_meta(db, seq_meta_key(seqid), &limit) ? limit : start;
    st.limit = st.next;
    it = db->seqs.emplace(seqid, st).first;
  }
  db_t::seq_state_t &st = it->second;
  // Refuse a sequence whose last value is already handed out.
  if (st.exhausted) {
    return set_error(SM_ERR_INVALID, "sequence is exhausted");
  }
  // Lease used up: persist a new limit before handing out more.
  if (st.next >= st.limit) {
    // Extend by a whole block, stopping at the largest value.
    uint64_t room = (UINT64_MAX - st.next) / st.increment;
    uint64_t blocks = room < db->lease ? room : db->lease;
    uint64_t new_limit = st.next + blocks * st.increment;
    // No room left for a whole step: this block is the last one.
    if (new_limit == st.next) {
      new_limit = UINT64_MAX;
    }
    std::string val;
    put_be64(val, new_limit);
    // The lease must be durable before any value under it.
    rocksdb::WriteOptions wo;
    wo.sync = true;
    // Persist the new lease.
    rocksdb::Status s = db->rdb->Put(wo, db->cf_meta, seq_meta_key(seqid), val);
    // Fail if the new lease did not persist.
    if (!s.ok()) {
      return set_status_error(s, "persist sequence lease");
    }
    st.limit = new_limit;
  }
  // Hand out the next value.
  *out = st.next;
  // Stop here if another step would wrap.
  if (st.next > UINT64_MAX - st.increment) {
    st.exhausted = true;
  } else {
    st.next += st.increment;
  }
  return 0;
}

/* Drop seqid's in-memory state and its persisted lease. */
void db_forget_seq(db_t *db, uint64_t seqid) {
  // Hold alloc_mu while dropping the state.
  std::lock_guard<std::mutex> guard(db->alloc_mu);
  db->seqs.erase(seqid);
  // Delete the lease; nobody waits on this, so log a failure.
  rocksdb::Status s = db->rdb->Delete(rocksdb::WriteOptions(), db->cf_meta,
                                      seq_meta_key(seqid));
  if (!s.ok()) {
    log_msg(SM_LOG_ERROR,
            "database {}: deleting sequence {}'s lease failed: {}",
            db->name.c_str(), seqid, s.ToString().c_str());
  }
}

/* Look up relid's CF handle under the shared lock. */
rocksdb::ColumnFamilyHandle *db_rel_cf(db_t *db, uint64_t relid) {
  // Hold rel_mu shared while reading the map.
  std::shared_lock<std::shared_mutex> guard(db->rel_mu);
  // Look up relid's handle.
  auto it = db->rels.find(relid);
  return it == db->rels.end() ? nullptr : it->second;
}

/* Create rel.<relid> and register its handle. */
int db_create_rel_cf(db_t *db, uint64_t relid) {
  rocksdb::ColumnFamilyHandle *h = nullptr;
  // Create the relation's column family; this is not transactional.
  rocksdb::Status s = db->rdb->CreateColumnFamily(rel_cf_options(db, relid),
                                                  rel_cf_name(relid), &h);
  // Fail if RocksDB could not create it.
  if (!s.ok()) {
    return set_status_error(s, "create column family");
  }
  // Hold rel_mu exclusively while registering it.
  std::unique_lock<std::shared_mutex> guard(db->rel_mu);
  // Register the handle.
  db->rels[relid] = h;
  return 0;
}

/* Drop rel.<relid> and unregister its handle. */
int db_drop_rel_cf(db_t *db, uint64_t relid) {
  // Hold rel_mu exclusively while dropping.
  std::unique_lock<std::shared_mutex> guard(db->rel_mu);
  // Look up relid's handle.
  auto it = db->rels.find(relid);
  // Already gone: nothing to do.
  if (it == db->rels.end()) {
    return 0;
  }
  // Drop the relation's column family.
  rocksdb::Status s = db->rdb->DropColumnFamily(it->second);
  // Release the handle either way.
  db->rdb->DestroyColumnFamilyHandle(it->second);
  // Unregister it.
  db->rels.erase(it);
  // Report a failed drop; most callers cannot act on it, so log it.
  if (!s.ok()) {
    log_msg(SM_LOG_ERROR,
            "database {}: dropping column family rel.{} failed: {}",
            db->name.c_str(), relid, s.ToString().c_str());
    return set_status_error(s, "drop column family");
  }
  return 0;
}

} // namespace wisr

/* Destroy every CF handle, then close RocksDB. */
wisr::db_t::~db_t() {
  // Stop the GC worker, then wait out running compactions,
  // whose filters read the handles destroyed below.
  gc_stop(this);
  // Free the SSI nodes still kept for overlapping transactions;
  // with the database closing, none can overlap any more.
  for (ssi_node_t *n : ssi_nodes) {
    delete n;
  }
  ssi_nodes.clear();
  // Free the in-doubt transactions; their PREPARED records
  // restore them at the next open.
  for (auto &kv : prepared) {
    delete kv.second;
  }
  prepared.clear();
  // Nothing more to do if RocksDB never opened.
  if (!rdb) {
    return;
  }
  // Wait out running compactions, whose filters read the handles.
  rocksdb::CancelAllBackgroundWork(rdb, true);
  // Destroy every relation CF handle.
  for (auto &kv : rels) {
    rdb->DestroyColumnFamilyHandle(kv.second);
  }
  // Destroy the fixed CF handles.
  for (rocksdb::ColumnFamilyHandle *h : {cf_default, cf_txn, cf_meta}) {
    if (h) {
      rdb->DestroyColumnFamilyHandle(h);
    }
  }
  // Close the database's RocksDB instance.
  rdb->Close();
  delete rdb;
}

/* Fill opt with the defaults documented in sm.h. */
void sm_options_init(sm_options_t *opt) {
  // Sync every commit record.
  opt->durability_sync = 1;
  // Accepted, unused.
  opt->group_commit_us = 200;
  // Check for deadlock after 50ms of waiting.
  opt->deadlock_ms = 50;
  // Accepted, unused.
  opt->ssi_max_entries = 1 << 20;
  // Persist the ts lease every million values.
  opt->ts_lease = 1000000;
  // No pass-through RocksDB options.
  opt->rocksdb_options = NULL;
  // Cache commit_ts for 64K transactions.
  opt->commit_cache_entries = 65536;
}

/* Create the database dbname. */
int sm_mkdb(sm_env_t *env, const char *dbname, const sm_options_t *opt) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return mkdb(env, dbname, opt);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_mkdb: out of memory");
  }
}

/* Delete the database dbname and all of its data. */
int sm_rmdb(sm_env_t *env, const char *dbname) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return rmdb(env, dbname);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_rmdb: out of memory");
  }
}

/* List the databases in the home directory, in bytewise order. */
int sm_lsdb(sm_env_t *env, sm_strlist_t **out) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return lsdb(env, out);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_lsdb: out of memory");
  }
}
