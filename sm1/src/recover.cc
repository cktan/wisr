/*
 * recover.cc -- what opening a database does beyond RocksDB's
 * own WAL replay (SM1_DESIGN.md §9.1 steps 4-6): restore prepared
 * transactions in doubt, and drop orphaned store CFs.
 *
 * There is no redo or undo pass. A transaction running at the
 * crash has no status record and is absent from the fresh
 * active table, so it resolves as dead (§3.4); GC drops its
 * versions.
 */

#include "codec.h"
#include "error.h"
#include "log.h"
#include "session.h"
#include "version.h"

#include <memory>
#include <set>
#include <vector>

using namespace wisr;

namespace {

/*
 * Rebuild each PREPARED record as an in-doubt transaction: in
 * the active table, holding the row locks of its write set,
 * pinning the watermark at its snapshot, and marked restored
 * for SSI (§8.4, §9.1 step 5).
 */
int restore_prepared(db_t *db) {
  // Walk every status record in CF:txn.
  std::unique_ptr<rocksdb::Iterator> it(
      db->rdb->NewIterator(rocksdb::ReadOptions(), db->cf_txn));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    // Skip anything that is not an 8-byte xid key.
    if (it->key().size() != 8) {
      continue;
    }
    // Decode the xid.
    uint64_t xid = get_be64(it->key().data());
    sm_session_t *g = nullptr;
    // Rebuild a PREPARED record; skip every other state.
    if (!xct_decode_prepared(db, xid, it->value(), &g)) {
      continue; // not a PREPARED record
    }
    // Mark it restored, for §8.4's SSI rule.
    g->restored = true;
    // Give it a begin order, for deadlock victim choice.
    g->begin_seq = ++db->begin_seq;
    {
      // Put it back in the active table: its versions stay in flight.
      std::lock_guard<std::mutex> guard(db->txn_mu);
      db->active.insert(xid);
    }
    // Re-acquire the row locks of its write set, each in the
    // mode the record carries.
    lock_restore(db, g);
    // Pin the GC watermark at its snapshot.
    db_register_snapshot(db, g->snapshot_ts);
    g->snapshot_registered = true;
    // Record it as restored for SSI.
    ssi_restore(db, xid);
    // Register it under its gid for listing and resolution.
    db->prepared[g->prepared_gid] = g;
    // Tell the operator: it holds locks until a coordinator acts.
    log_msg(SM_LOG_INFO,
            "database {}: restored prepared transaction {} in doubt",
            db->name.c_str(), g->prepared_gid.c_str());
  }
  // Fail if the scan of CF:txn failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "scan CF:txn");
  }
  return 0;
}

/*
 * The storeids the catalog names now: for each catalog key, its
 * newest committed version, if that is not a tombstone. Versions
 * of restored prepared transactions are in flight and skipped.
 */
int live_storeids(db_t *db, std::set<uint64_t> *live) {
  // Scan the catalog in total order, across every userkey.
  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(
      db->rdb->NewIterator(ro, db->stores.at(0)));
  // Track the key whose newest committed version is decided.
  std::string decided; // userkey whose newest committed version was seen
  bool have = false;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    version_t v;
    // Decode the version's userkey and xid.
    version_decode_key(it->key(), &v);
    // Skip older versions of a key already decided.
    if (have && v.userkey == rocksdb::Slice(decided)) {
      continue;
    }
    // Reject a malformed catalog version.
    if (!version_decode_value(it->value(), &v)) {
      return set_error(SM_ERR_CORRUPT, "malformed catalog version");
    }
    uint64_t commit_ts;
    // Resolve an unstamped version's writer.
    if (!(v.flags & version_stamped)) {
      xct_state_t st = xct_status(db, v.xid, &commit_ts);
      // Fail on a status lookup error.
      if (st == XCT_ERROR) {
        return -1;
      }
      // Skip versions in flight or dead.
      if (st != XCT_COMMITTED) {
        continue;
      }
    }
    // This is the key's newest committed version.
    decided = v.userkey.ToString();
    have = true;
    // A live row's key names a storeid to keep: "R" | be64(storeid).
    if (!(v.flags & version_tombstone) && v.userkey.size() == 9) {
      live->insert(get_be64(v.userkey.data() + 1));
    }
  }
  // Fail if the catalog scan failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "scan catalog");
  }
  return 0;
}

/*
 * Drop every store CF that neither the catalog nor a prepared
 * transaction's created list names (§9.1 step 6): a crash inside
 * an uncommitted sm_mkstore, or a committed sm_rmstore whose CF GC
 * had not yet dropped.
 */
int sweep_orphan_cfs(db_t *db) {
  // Always keep the catalog's own CF.
  std::set<uint64_t> keep{0};
  // Keep every storeid a live catalog row names.
  if (live_storeids(db, &keep) != 0) {
    return -1;
  }
  // Keep every CF a prepared transaction created.
  for (auto &kv : db->prepared) {
    keep.insert(kv.second->created.begin(), kv.second->created.end());
  }
  std::vector<uint64_t> orphans;
  // Collect the store CFs nothing names.
  for (auto &kv : db->stores) {
    if (!keep.count(kv.first)) {
      orphans.push_back(kv.first);
    }
  }
  // Drop each orphan.
  for (uint64_t storeid : orphans) {
    // Drop an orphaned store column family.
    if (db_drop_store_cf(db, storeid) != 0) {
      return -1;
    }
    log_msg(SM_LOG_INFO, "database {}: dropped orphaned column family store.{}",
            db->name.c_str(), storeid);
  }
  return 0;
}

} // namespace

namespace wisr {

/* Restore prepared transactions, then sweep orphaned CFs. */
int recover(db_t *db) {
  // Restore prepared transactions first: the sweep needs them.
  if (restore_prepared(db) != 0) {
    return -1;
  }
  // Then drop orphaned store CFs.
  return sweep_orphan_cfs(db);
}

} // namespace wisr
