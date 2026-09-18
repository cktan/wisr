/*
 * recover.cc -- what opening a database does beyond RocksDB's
 * own WAL replay (DESIGN.md §9.1 steps 4-6): restore prepared
 * transactions in doubt, and drop orphaned relation CFs.
 *
 * There is no redo or undo pass. A transaction running at the
 * crash has no status record and is absent from the fresh
 * active table, so it resolves as dead (§3.4); GC drops its
 * versions.
 */

#include "codec.h"
#include "error.h"
#include "session.h"
#include "version.h"

#include <memory>
#include <set>
#include <vector>

using namespace wisr;

namespace
{

/*
 * Rebuild each PREPARED record as an in-doubt transaction: in
 * the active table, holding the row locks of its write set,
 * pinning the watermark at its snapshot, and marked restored
 * for SSI (§8.4, §9.1 step 5).
 */
int restore_prepared(db_t* db)
{
    std::unique_ptr<rocksdb::Iterator> it(
        db->rdb->NewIterator(rocksdb::ReadOptions(), db->cf_txn));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        if (it->key().size() != 8) {
            continue;
        }
        uint64_t xid = get_be64(it->key().data());
        sm_session_t* g = nullptr;
        if (!xct_decode_prepared(db, xid, it->value(), &g)) {
            continue; // not a PREPARED record
        }
        g->restored = true;
        g->begin_seq = ++db->begin_seq;
        {
            std::lock_guard<std::mutex> guard(db->txn_mu);
            db->active.insert(xid);
        }
        for (const std::string& k : g->locks) {
            db->lock_owners[k] = g;
        }
        db_register_snapshot(db, g->snapshot_ts);
        g->snapshot_registered = true;
        ssi_restore(db, xid);
        db->prepared[g->prepared_gid] = g;
    }
    if (!it->status().ok()) {
        return set_status_error(it->status(), "scan CF:txn");
    }
    return 0;
}

/*
 * The relids the catalog names now: for each catalog key, its
 * newest committed version, if that is not a tombstone. Versions
 * of restored prepared transactions are in flight and skipped.
 */
int live_relids(db_t* db, std::set<uint64_t>* live)
{
    rocksdb::ReadOptions ro;
    ro.total_order_seek = true;
    std::unique_ptr<rocksdb::Iterator> it(
        db->rdb->NewIterator(ro, db->rels.at(0)));
    std::string decided; // userkey whose newest committed version was seen
    bool have = false;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        version_t v;
        version_decode_key(it->key(), &v);
        if (have && v.userkey == rocksdb::Slice(decided)) {
            continue;
        }
        if (!version_decode_value(it->value(), &v)) {
            return set_error(SM_ERR_CORRUPT, "malformed catalog version");
        }
        uint64_t commit_ts;
        if (!(v.flags & version_stamped)) {
            xct_state_t st = xct_status(db, v.xid, &commit_ts);
            if (st == XCT_ERROR) {
                return -1;
            }
            if (st != XCT_COMMITTED) {
                continue;
            }
        }
        decided = v.userkey.ToString();
        have = true;
        if (!(v.flags & version_tombstone) && v.payload.size() == 8) {
            live->insert(get_be64(v.payload.data()));
        }
    }
    if (!it->status().ok()) {
        return set_status_error(it->status(), "scan catalog");
    }
    return 0;
}

/*
 * Drop every relation CF that neither the catalog nor a prepared
 * transaction's created list names (§9.1 step 6): a crash inside
 * an uncommitted sm_mkrel, or a committed sm_rmrel whose CF GC
 * had not yet dropped.
 */
int sweep_orphan_cfs(db_t* db)
{
    std::set<uint64_t> keep{0};
    if (live_relids(db, &keep) != 0) {
        return -1;
    }
    for (auto& kv : db->prepared) {
        keep.insert(kv.second->created.begin(), kv.second->created.end());
    }
    std::vector<uint64_t> orphans;
    for (auto& kv : db->rels) {
        if (!keep.count(kv.first)) {
            orphans.push_back(kv.first);
        }
    }
    for (uint64_t relid : orphans) {
        // Drop an orphaned relation column family.
        if (db_drop_rel_cf(db, relid) != 0) {
            return -1;
        }
    }
    return 0;
}

} // namespace

namespace wisr
{

/* Restore prepared transactions, then sweep orphaned CFs. */
int recover(db_t* db)
{
    if (restore_prepared(db) != 0) {
        return -1;
    }
    return sweep_orphan_cfs(db);
}

} // namespace wisr
