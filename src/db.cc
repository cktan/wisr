/*
 * db.cc -- sm_options_init, sm_mkdb, sm_rmdb, sm_lsdb, and the
 * column family layout every database shares (DESIGN.md §3,
 * §3.5, §11).
 */

#include "db.h"

#include "codec.h"
#include "env.h"
#include "error.h"
#include "strlist.h"
#include "version.h"

#include <memory>
#include <new>
#include <vector>

#include <rocksdb/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace
{

/* The root db key naming dbname: "D" | dbname. */
std::string dbname_key(const char* dbname)
{
    return std::string(WISR_ROOT_DBNAME_PREFIX) + dbname;
}

/*
 * Create a database at path: the directory, CF:txn, CF:meta
 * with its initial contents, and the catalog CF:rel.0.
 */
int create_db(const std::string& path, rocksdb::DBOptions dbo)
{
    dbo.create_if_missing = true;
    dbo.create_missing_column_families = true;
    dbo.error_if_exists = true;

    std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
        {rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()},
        {WISR_CF_TXN, rocksdb::ColumnFamilyOptions()},
        {WISR_CF_META, rocksdb::ColumnFamilyOptions()},
        {rel_cf_name(0), rel_cf_options()},
    };
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    rocksdb::DB* db = nullptr;
    // Create the RocksDB instance with every fixed column family.
    rocksdb::Status s = rocksdb::DB::Open(dbo, path, cfs, &handles, &db);
    if (!s.ok()) {
        return set_status_error(s, path.c_str());
    }

    // Seed CF:meta: format version and the allocators' leases. A
    // persisted lease of 0 resumes both allocators at 1 (§4.2),
    // so relid 0 stays the catalog's.
    std::string version, zero;
    put_be64(version, WISR_DB_VERSION);
    put_be64(zero, 0);
    rocksdb::WriteBatch batch;
    batch.Put(handles[2], WISR_META_VERSION, version);
    batch.Put(handles[2], WISR_META_NEXT_TS, zero);
    batch.Put(handles[2], WISR_META_NEXT_RELID, zero);
    rocksdb::WriteOptions wo;
    wo.sync = true;
    s = db->Write(wo, &batch);

    for (rocksdb::ColumnFamilyHandle* h : handles) {
        db->DestroyColumnFamilyHandle(h);
    }
    db->Close();
    delete db;
    if (!s.ok()) {
        return set_status_error(s, path.c_str());
    }
    return 0;
}

/* sm_mkdb, minus the out-of-memory guard. */
int mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt)
{
    if (!env) {
        return set_error(SM_ERR_INVALID, "sm_mkdb: env is NULL");
    }
    if (check_name(dbname, SM_MAX_DBNAME, "database") != 0) {
        return -1;
    }
    rocksdb::DBOptions dbo;
    if (db_options(opt, &dbo) != 0) {
        return -1;
    }

    std::lock_guard<std::mutex> guard(env->mu);
    std::string dkey = dbname_key(dbname);
    std::string val;
    rocksdb::Status s = env->root->Get(rocksdb::ReadOptions(), dkey, &val);
    if (s.ok()) {
        return set_error(SM_ERR_EXISTS, "database %s already exists", dbname);
    }
    if (!s.IsNotFound()) {
        return set_status_error(s, "read root db");
    }

    // Allocate a dbid, durably, before its directory exists, so
    // a dbid is never reused (§3.5).
    s = env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_NEXT_DBID, &val);
    if (!s.ok() || val.size() != 8) {
        return set_error(SM_ERR_CORRUPT, "root db has no next_dbid");
    }
    uint64_t dbid = get_be64(val.data());
    std::string next;
    put_be64(next, dbid + 1);
    rocksdb::WriteOptions wo;
    wo.sync = true;
    s = env->root->Put(wo, WISR_ROOT_NEXT_DBID, next);
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
    put_be64(dbid_val, dbid);
    s = env->root->Put(wo, dkey, dbid_val);
    if (!s.ok()) {
        return set_status_error(s, "write root db");
    }
    return 0;
}

/* sm_rmdb, minus the out-of-memory guard. */
int rmdb(sm_env_t* env, const char* dbname)
{
    if (!env) {
        return set_error(SM_ERR_INVALID, "sm_rmdb: env is NULL");
    }
    if (check_name(dbname, SM_MAX_DBNAME, "database") != 0) {
        return -1;
    }

    // Sessions do not exist yet (build order step 1, task 5
    // adds them), so there is no open database to refuse on.
    std::lock_guard<std::mutex> guard(env->mu);
    std::string dkey = dbname_key(dbname);
    std::string val;
    rocksdb::Status s = env->root->Get(rocksdb::ReadOptions(), dkey, &val);
    if (s.IsNotFound()) {
        return set_error(SM_ERR_NOTFOUND, "database %s does not exist", dbname);
    }
    if (!s.ok()) {
        return set_status_error(s, "read root db");
    }
    if (val.size() != 8) {
        return set_error(SM_ERR_CORRUPT, "bad dbid for database %s", dbname);
    }
    uint64_t dbid = get_be64(val.data());

    // Drop the name first: the mapping is the truth (§3.5).
    rocksdb::WriteOptions wo;
    wo.sync = true;
    s = env->root->Delete(wo, dkey);
    if (!s.ok()) {
        return set_status_error(s, "write root db");
    }

    // Destroy the directory. If this fails the database is
    // already gone by name, and the next sm_attach sweeps the
    // directory; the error still reports that it is left.
    return env_destroy_db_dir(env_db_path(env, dbid));
}

/* sm_lsdb, minus the out-of-memory guard. */
int lsdb(sm_env_t* env, sm_strlist_t** out)
{
    if (!env || !out) {
        return set_error(SM_ERR_INVALID, "sm_lsdb: env or out is NULL");
    }
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> guard(env->mu);
        std::unique_ptr<rocksdb::Iterator> it(
            env->root->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek(WISR_ROOT_DBNAME_PREFIX);
             it->Valid() && it->key().starts_with(WISR_ROOT_DBNAME_PREFIX);
             it->Next()) {
            rocksdb::Slice k = it->key();
            k.remove_prefix(1);
            names.push_back(k.ToString());
        }
        if (!it->status().ok()) {
            return set_status_error(it->status(), "scan root db");
        }
    }
    sm_strlist_t* sl = strlist_make(names);
    if (!sl) {
        return set_error(SM_ERR_NOMEM, "sm_lsdb: out of memory");
    }
    *out = sl;
    return 0;
}

} // namespace

namespace wisr
{

/* rel.<relid>, in decimal. */
std::string rel_cf_name(uint64_t relid)
{
    return "rel." + std::to_string(relid);
}

/*
 * The comparator and prefix extractor are part of the format
 * (§3.1). The bloom filter keys on the userkey prefix; whole
 * version keys are never looked up, so no whole-key filter.
 */
rocksdb::ColumnFamilyOptions rel_cf_options()
{
    rocksdb::ColumnFamilyOptions cfo;
    cfo.comparator = version_comparator();
    cfo.prefix_extractor = version_prefix_extractor();
    rocksdb::BlockBasedTableOptions bbto;
    bbto.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
    bbto.whole_key_filtering = false;
    cfo.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbto));
    return cfo;
}

/* Parse the pass-through RocksDB option string onto *out. */
int db_options(const sm_options_t* opt, rocksdb::DBOptions* out)
{
    if (!opt || !opt->rocksdb_options) {
        return 0;
    }
    rocksdb::ConfigOptions cfg;
    rocksdb::DBOptions parsed;
    rocksdb::Status s = rocksdb::GetDBOptionsFromString(
        cfg, *out, opt->rocksdb_options, &parsed);
    if (!s.ok()) {
        return set_error(SM_ERR_INVALID, "rocksdb_options: %s",
                         s.ToString().c_str());
    }
    *out = parsed;
    return 0;
}

} // namespace wisr

/* Fill opt with the defaults documented in sm.h. */
void sm_options_init(sm_options_t* opt)
{
    opt->durability_sync = 1;
    opt->group_commit_us = 200;
    opt->deadlock_ms = 50;
    opt->ssi_max_entries = 1 << 20;
    opt->ts_lease = 1000000;
    opt->rocksdb_options = NULL;
}

/* Create the database dbname. */
int sm_mkdb(sm_env_t* env, const char* dbname, const sm_options_t* opt)
{
    try {
        return mkdb(env, dbname, opt);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_mkdb: out of memory");
    }
}

/* Delete the database dbname and all of its data. */
int sm_rmdb(sm_env_t* env, const char* dbname)
{
    try {
        return rmdb(env, dbname);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_rmdb: out of memory");
    }
}

/* List the databases in the home directory, in bytewise order. */
int sm_lsdb(sm_env_t* env, sm_strlist_t** out)
{
    try {
        return lsdb(env, out);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_lsdb: out of memory");
    }
}
