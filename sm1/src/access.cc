/*
 * access.cc -- sm_insert, sm_update, sm_delete, sm_read, and
 * the sm_scan cursor (SM1_DESIGN.md §4.4, §5, §10).
 *
 * Under SERIALIZABLE, reads leave SIREAD entries and report the
 * concurrent versions they step past, and writes report the
 * readers they invalidate (ssi.cc, §7).
 */

#include "defer.h"
#include "error.h"
#include "session.h"
#include "version.h"

#include <algorithm>
#include <cassert>
#include <new>

#include <rocksdb/options.h>

using namespace wisr;

namespace {

/* 1 if version v is visible at view, 0 if not, -1 on error (§4.4). */
int visible(db_t *db, const version_t &v, const view_t &view) {
  // Our own write: visible unless a rollback undid it.
  if (view.xid != 0 && v.xid == view.xid) {
    return v.seq <= view.seq_hwm; // own write, savepoint-aware
  }
  // Written after our snapshot, so committed after it too.
  if (v.wts > view.snapshot_ts) {
    return 0; // commit_ts > wts > snapshot
  }
  // Find when the writer committed.
  uint64_t commit_ts;
  // A stamped version carries its commit_ts.
  if (v.flags & version_stamped) {
    commit_ts = v.commit_ts;
  } else {
    // Otherwise resolve the writer through CF:txn.
    int r = xct_resolve(db, v.xid, &commit_ts);
    // Not committed, or an error: not visible.
    if (r <= 0) {
      return r;
    }
  }
  // Visible if it committed at or before our snapshot.
  return commit_ts <= view.snapshot_ts;
}

/*
 * For SSI (§10.1): 1 if v, invisible to view, was written by a
 * concurrent transaction -- one still in flight, or committed
 * after the snapshot -- so the reader owes an rw-edge; 0 if not
 * (its own, dead, or older); -1 on error.
 */
int concurrent_writer(db_t *db, const version_t &v, const view_t &view) {
  // Our own writes owe no edge.
  if (view.xid != 0 && v.xid == view.xid) {
    return 0;
  }
  // A stamped version committed at its stamp.
  if (v.flags & version_stamped) {
    return v.commit_ts > view.snapshot_ts;
  }
  uint64_t commit_ts;
  // Resolve the writer's fate.
  switch (xct_status(db, v.xid, &commit_ts)) {
  case XCT_IN_FLIGHT:
    return 1;
  case XCT_COMMITTED:
    return commit_ts > view.snapshot_ts;
  case XCT_DEAD:
    return 0;
  default:
    return -1;
  }
}

/* Step past an invisible version: under SSI, owe its writer an edge. */
int skip_version(sm_session_t *s, const version_t &v, const view_t &view) {
  // Only SERIALIZABLE readers track skipped versions.
  if (!s->ssi) {
    return 0;
  }
  // Was the writer concurrent with us?
  int c = concurrent_writer(s->db, v, view);
  // Fail on a status lookup error.
  if (c < 0) {
    return -1;
  }
  // Concurrent: record the edge reader -> writer.
  if (c) {
    ssi_on_skip(s, v.xid);
  }
  return 0;
}

/* The CF of storeid; its absence after a catalog hit is corruption. */
rocksdb::ColumnFamilyHandle *store_cf(db_t *db, uint64_t storeid) {
  // Look up the store's CF.
  rocksdb::ColumnFamilyHandle *cf = db_store_cf(db, storeid);
  // The catalog named it, so a missing CF is corruption.
  if (!cf) {
    set_error(SM_ERR_CORRUPT, "column family store.{} is missing", storeid);
  }
  return cf;
}

/*
 * The version a write checks against (§5 step 3): the newest
 * own version still visible to the transaction, else the newest
 * committed version, whatever its commit_ts. Sets *live to
 * whether it exists and is not a tombstone, and *commit_ts to
 * its commit_ts (0 for an own version or none). Returns 0 or -1.
 */
int latest_version(sm_session_t *s, rocksdb::ColumnFamilyHandle *cf,
                   rocksdb::Slice key, bool *live, uint64_t *commit_ts,
                   std::string *value) {
  // Assume no version until one is found.
  *live = false;
  *commit_ts = 0;
  // No row to hand back yet.
  if (value) {
    value->clear();
  }
  // Hold a read epoch from before the iterator exists until we are
  // done resolving what it read: the CF:txn sweep waits for it.
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  // Stay within this userkey's versions.
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  // Open an iterator on the store.
  std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
  // Walk every version of the key, newest first.
  for (it->Seek(version_bound(key, '\x00')); it->Valid(); it->Next()) {
    version_t v;
    // Decode the version's userkey, wts, and seq.
    version_decode_key(it->key(), &v);
    // Past the last version of this key.
    if (v.userkey != key) {
      break;
    }
    // Reject a malformed value.
    if (!version_decode_value(it->value(), &v)) {
      return set_error(SM_ERR_CORRUPT, "malformed version value");
    }
    // Our own version: it decides, unless rolled back.
    if (s->xid != 0 && v.xid == s->xid) {
      // Still in effect: it is the latest.
      if (v.seq <= s->seq_hwm) {
        *live = !(v.flags & version_tombstone);
        // Hand back what it holds, for the old image.
        if (value && *live) {
          value->assign(v.payload.data(), v.payload.size());
        }
        return 0;
      }
      continue;
    }
    // Assume a stamped version committed.
    int committed = 1;
    uint64_t ts = v.commit_ts;
    // Resolve an unstamped version's writer.
    if (!(v.flags & version_stamped)) {
      committed = xct_resolve(s->db, v.xid, &ts);
    }
    // Fail on a status lookup error.
    if (committed < 0) {
      return -1;
    }
    // The newest committed version decides.
    if (committed) {
      *live = !(v.flags & version_tombstone);
      *commit_ts = ts;
      // Hand back what it holds, for the old image.
      if (value && *live) {
        value->assign(v.payload.data(), v.payload.size());
      }
      return 0;
    }
  }
  // Fail if the iterator failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "read versions");
  }
  return 0;
}

/* Check a caller's key: non-NULL unless empty, within SM_MAX_KEYLEN. */
int check_key(const char *key, size_t klen) {
  // Only an empty key may be NULL.
  if (!key && klen > 0) {
    return set_error(SM_ERR_INVALID, "key is NULL");
  }
  // Reject a key over the limit.
  if (klen > SM_MAX_KEYLEN) {
    return set_error(SM_ERR_INVALID, "key of {} bytes exceeds {}", klen,
                     SM_MAX_KEYLEN);
  }
  return 0;
}

/*
 * Common entry checks for a data operation: a valid key, and a
 * storeid that names a live store at view.
 */
int enter(sm_session_t *s, const view_t &view, uint64_t storeid,
          const char *key, size_t klen) {
  // Reject storeid 0, which no store has.
  if (storeid == 0) {
    return set_error(SM_ERR_INVALID, "storeid 0 is not a store");
  }
  // Validate the key.
  if (check_key(key, klen) != 0) {
    return -1;
  }
  // Look the store up at the view.
  int r = catalog_lookup(s, view, storeid);
  // Report an unknown store.
  if (r == 1) {
    return set_error(SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  return r;
}

/* sm_insert / sm_update / sm_delete, minus the out-of-memory guard. */
int write_row(sm_session_t *s, write_op_t op, uint64_t storeid, const char *key,
              size_t klen, const char *data, size_t datalen, const char **old,
              size_t *oldlen) {
  // The out-parameters come as a pair; check before either is
  // written to.
  if ((old != nullptr) != (oldlen != nullptr)) {
    return set_error(SM_ERR_INVALID, "old and oldlen must both be given");
  }
  // Report no old row until the write finds one.
  if (old) {
    *old = nullptr;
    *oldlen = 0;
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Only an empty value may be NULL.
  if (!data && datalen > 0) {
    return set_error(SM_ERR_INVALID, "data is NULL");
  }
  // Reject a value over the limit.
  if (datalen > SM_MAX_DATALEN) {
    return set_error(SM_ERR_INVALID, "value of {} bytes exceeds {}", datalen,
                     SM_MAX_DATALEN);
  }
  // Validate the storeid and key at the current view.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  // Write the version, keeping the row it replaces when asked.
  int r = access_write(s, storeid, op, rocksdb::Slice(key ? key : "", klen),
                       rocksdb::Slice(data ? data : "", datalen),
                       old ? &s->readbuf : nullptr);
  // Hand out the session-owned bytes of the old row.
  if (r == 0 && old) {
    *old = s->readbuf.data();
    *oldlen = s->readbuf.size();
  }
  return r;
}

/* sm_lock, minus the out-of-memory guard. */
int lock_row(sm_session_t *s, uint64_t storeid, const char *key, size_t klen,
             sm_lockmode_t mode) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject a mode that is neither shared nor exclusive.
  if (mode != SM_LOCK_SHARED && mode != SM_LOCK_EXCLUSIVE) {
    return set_error(SM_ERR_INVALID, "unknown lock mode {}", (int)mode);
  }
  // Validate the storeid and key at the current view.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  // Take the lock, blocking while a conflicting holder has it.
  if (lock_acquire(s, storeid, rocksdb::Slice(key ? key : "", klen),
                   mode == SM_LOCK_EXCLUSIVE) != 0) {
    return xct_fail_retryable(s, SM_ERR_DEADLOCK,
                              "chosen as a deadlock victim");
  }
  // Read committed does not fail here: the caller reads the
  // row with sm_read_latest instead.
  if (s->level == SM_XCT_READ_COMMITTED) {
    return 0;
  }
  // Now that the lock is held, run §5.2's check, so what the
  // caller reads next under this lock is current: a version
  // committed after this transaction began fails the call.
  return access_check_current(s, storeid, rocksdb::Slice(key ? key : "", klen));
}

/* sm_read, minus the out-of-memory guard. */
int read_row(sm_session_t *s, uint64_t storeid, const char *key, size_t klen,
             const char **data, size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(SM_ERR_INVALID, "sm_read: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // The view this statement reads at.
  view_t view = xct_read_view(s);
  // Validate the storeid and key.
  if (enter(s, view, storeid, key, klen) != 0) {
    return -1;
  }
  // Read the key into the session's buffer.
  int r = access_read(s, storeid, view, rocksdb::Slice(key ? key : "", klen),
                      &s->readbuf);
  // Found: hand out the session-owned bytes.
  if (r == 0) {
    *data = s->readbuf.data();
    *datalen = s->readbuf.size();
  }
  return r;
}

/* sm_read_latest, minus the out-of-memory guard. */
int read_latest_row(sm_session_t *s, uint64_t storeid, const char *key,
                    size_t klen, const char **data, size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(SM_ERR_INVALID, "sm_read_latest: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Only read committed has a snapshot to look past.
  if (s->level != SM_XCT_READ_COMMITTED) {
    return set_error(SM_ERR_INVALID, "sm_read_latest requires READ COMMITTED");
  }
  // Validate the storeid and key.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  rocksdb::Slice k(key ? key : "", klen);
  // The lock is what keeps the newest version from changing.
  int mode = lock_mode_held(s, storeid, k);
  if (mode == 0) {
    return set_error(SM_ERR_INVALID,
                     "sm_read_latest requires a lock held on the key");
  }
  // Find the store's CF.
  rocksdb::ColumnFamilyHandle *cf = store_cf(s->db, storeid);
  if (!cf) {
    return -1;
  }
  bool live;
  uint64_t commit_ts;
  // Read the newest version, ignoring the snapshot.
  if (latest_version(s, cf, k, &live, &commit_ts, &s->readbuf) != 0) {
    return -1;
  }
  // An exclusive holder may now write.
  if (mode == 2) {
    lock_set_latest(s, storeid, k);
  }
  // Absent, or deleted.
  if (!live) {
    return 1;
  }
  // Hand out the session-owned bytes.
  *data = s->readbuf.data();
  *datalen = s->readbuf.size();
  return 0;
}

/* sm_scan, minus the out-of-memory guard. */
sm_scan_t *scan(sm_session_t *s, uint64_t storeid, const char *skey,
                size_t sklen, int skey_inclusive, const char *ekey,
                size_t eklen, int ekey_inclusive) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return nullptr;
  }
  // Validate whichever bounds are given.
  if ((skey && check_key(skey, sklen) != 0) ||
      (ekey && check_key(ekey, eklen) != 0)) {
    return nullptr;
  }
  // The view the cursor reads at, fixed from now on.
  view_t view = xct_read_view(s);
  // Validate the storeid.
  if (enter(s, view, storeid, "", 0) != 0) {
    return nullptr;
  }
  // Open the cursor on the store's column family.
  return access_scan(s, storeid, view, skey, sklen, skey_inclusive, ekey, eklen,
                     ekey_inclusive);
}

} // namespace

namespace wisr {

/* Open a cursor over storeid at view, between the given bounds. */
sm_scan_t *access_scan(sm_session_t *s, uint64_t storeid, const view_t &view,
                       const char *skey, size_t sklen, int skey_inclusive,
                       const char *ekey, size_t eklen, int ekey_inclusive) {
  // Find the column family.
  rocksdb::ColumnFamilyHandle *cf = store_cf(s->db, storeid);
  if (!cf) {
    return nullptr;
  }

  // Allocate the cursor.
  std::unique_ptr<sm_scan_t> c(new sm_scan_t);
  // Bind it to the session.
  c->session = s;
  // Number it, for savepoint rollback.
  c->id = ++s->next_scan_id;
  // Freeze the view: later writes stay invisible to it.
  c->snapshot_ts = view.snapshot_ts;
  c->xid = view.xid;
  c->seq_hwm = view.seq_hwm;
  // Under SERIALIZABLE, register the requested range before
  // the snapshot, so a write into it after the snapshot finds
  // the SIREAD entry (§10.2).
  ssi_range_open(c.get(), storeid, skey, sklen, skey_inclusive, ekey, eklen,
                 ekey_inclusive);
  // Count the cursor as a reader for its whole life (§8.3),
  // then take the RocksDB snapshot: its physical view (§10.2).
  c->epoch = db_read_enter(s->db);
  c->snap = s->db->rdb->GetSnapshot();

  // Read at the cursor's snapshot.
  rocksdb::ReadOptions ro;
  ro.snapshot = c->snap;
  ro.auto_prefix_mode = true; // a scan crosses userkeys (§10.2)
  // Bound the iterator at ekey, carrying a full tail.
  if (ekey) {
    // Build the bound; the cursor owns its bytes.
    c->upper = version_bound(rocksdb::Slice(ekey, eklen),
                             ekey_inclusive ? '\xff' : '\x00');
    // RocksDB borrows the bound through this slice.
    c->upper_slice = rocksdb::Slice(c->upper);
    // Install the bound.
    ro.iterate_upper_bound = &c->upper_slice;
  }
  // Open the iterator.
  c->it.reset(s->db->rdb->NewIterator(ro, cf));
  // Position at skey, or at the start.
  if (skey) {
    // Build the start bound; sm_scan_seek compares against it.
    c->lower = version_bound(rocksdb::Slice(skey, sklen),
                             skey_inclusive ? '\x00' : '\xff');
    // Seek to skey, past it if exclusive.
    c->it->Seek(c->lower);
  } else {
    // No lower bound: start at the first key.
    c->it->SeekToFirst();
  }
  // Register the cursor with the session.
  s->scans.insert(c.get());
  return c.release();
}

} // namespace wisr

namespace {

/* sm_scan_seek, minus the out-of-memory guard. */
int scan_seek(sm_scan_t *c, const char *key, size_t klen, int inclusive) {
  // Reject a NULL cursor.
  if (!c) {
    return set_error(SM_ERR_INVALID, "sm_scan_seek: scan is NULL");
  }
  // Validate the target key.
  if (check_key(key, klen) != 0) {
    return -1;
  }
  // A failed cursor stays failed.
  if (c->state == -1) {
    return set_error(SM_ERR_INVALID, "the cursor failed and cannot be reused");
  }
  rocksdb::Slice target(key ? key : "", klen);
  // Refuse a target before the last key examined: the cursor
  // moves forward only, and its SIREAD range is narrowed to
  // the last key it reached (§7.1, §10.2).
  if (c->examined && target.compare(rocksdb::Slice(c->last_examined)) < 0) {
    return set_error(SM_ERR_INVALID,
                     "sm_scan_seek moves forward only; the cursor has "
                     "already read past that key");
  }
  // Build the seek bound, past the target if exclusive.
  std::string bound = version_bound(target, inclusive ? '\x00' : '\xff');
  // Refuse a target below the range the cursor registered.
  if (bound < c->lower) {
    return set_error(SM_ERR_INVALID,
                     "sm_scan_seek target is below the scan's start key");
  }
  // A cursor at its end stays there: every forward target is
  // past the end too, and keeping state lets ssi_range_close
  // still count the whole range as read (§7.1).
  if (c->state == 1) {
    return 0;
  }
  // Reposition, keeping the iterator, its snapshot, and its
  // upper bound.
  c->it->Seek(bound);
  return 0;
}

/* sm_scannxt, minus the out-of-memory guard. */
int scan_next(sm_scan_t *c, const char **key, size_t *klen, const char **data,
              size_t *datalen) {
  // Require every out-parameter.
  if (!c || !key || !klen || !data || !datalen) {
    return set_error(SM_ERR_INVALID, "sm_scannxt: NULL argument");
  }
  // Report no row until one is found.
  *key = *data = nullptr;
  *klen = *datalen = 0;
  // A cursor at its end, or failed, stays so.
  if (c->state != 0) {
    return c->state;
  }
  // The view frozen at sm_scan.
  db_t *db = c->session->db;
  view_t view = {c->snapshot_ts, c->xid, c->seq_hwm};
  rocksdb::Iterator *it = c->it.get();

  // Advance one userkey's run of versions at a time.
  while (it->Valid()) {
    version_t v;
    // Decode the next run's userkey.
    version_decode_key(it->key(), &v);
    std::string uk = v.userkey.ToString();
    // Record how far the scan has read, for SIREAD narrowing.
    c->examined = true;
    c->last_examined = uk;
    // Nothing decided about this key yet.
    bool found = false;
    bool live = false;
    // Walk the run of uk's versions, newest first: the first
    // visible one decides, the rest are stepped past.
    for (; it->Valid(); it->Next()) {
      // Decode this version's userkey, wts, and seq.
      version_decode_key(it->key(), &v);
      // Past the last version of this key.
      if (v.userkey != rocksdb::Slice(uk)) {
        break;
      }
      // Once decided, only step past the older versions.
      if (found) {
        continue;
      }
      // Reject a malformed value.
      if (!version_decode_value(it->value(), &v)) {
        c->state = -1;
        return set_error(SM_ERR_CORRUPT, "malformed version value");
      }
      // Is this version visible at the cursor's view?
      int r = visible(db, v, view);
      // Fail the cursor on a status lookup error.
      if (r < 0) {
        c->state = -1;
        return -1;
      }
      // Visible: this version decides the key.
      if (r == 1) {
        // Stop judging this key's older versions.
        found = true;
        // A tombstone hides the key.
        live = !(v.flags & version_tombstone);
        // Copy the value out before the iterator moves.
        if (live) {
          c->data.assign(v.payload.data(), v.payload.size());
        }
      } else if (skip_version(c->session, v, view) != 0) {
        c->state = -1;
        return -1;
      }
    }
    // A live row: hand it out.
    if (live) {
      // Keep the key in the cursor's buffer.
      c->key = std::move(uk);
      *key = c->key.data();
      *klen = c->key.size();
      *data = c->data.data();
      *datalen = c->data.size();
      return 0;
    }
  }
  // Fail the cursor if the iterator failed.
  if (!it->status().ok()) {
    c->state = -1;
    return set_status_error(it->status(), "scan");
  }
  // Reached the end of the range.
  c->state = 1;
  return 1;
}

} // namespace

namespace wisr {

/* Seek to the newest version a reader at view could see (§10.1). */
int access_read(sm_session_t *s, uint64_t storeid, const view_t &view,
                rocksdb::Slice key, std::string *out) {
  // Find the store's CF.
  rocksdb::ColumnFamilyHandle *cf = store_cf(s->db, storeid);
  if (!cf) {
    return -1;
  }
  // Under SERIALIZABLE, take the SIREAD entry before the
  // iterator exists, so a write landing in between still finds
  // it (§10.1).
  ssi_on_read(s, storeid, key);
  // Hold a read epoch from before the iterator exists until we are
  // done resolving what it read: the CF:txn sweep waits for it.
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  // Stay within this userkey's versions.
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  // Open an iterator on the store.
  std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
  // Seek to the newest version the reader could see; ~seq of 0
  // lands before every own version of the key. Under SSI, start
  // before every version instead: the newer ones are exactly
  // the concurrent writers owed an edge.
  uint64_t hi = std::max(s->wts_hi, view.snapshot_ts);
  std::string target =
      s->ssi ? version_bound(key, '\x00') : version_key(key, hi, UINT32_MAX);
  // Walk the key's versions, newest first.
  for (it->Seek(target); it->Valid(); it->Next()) {
    version_t v;
    // Decode the version's userkey, wts, and seq.
    version_decode_key(it->key(), &v);
    // Past the last version of this key.
    if (v.userkey != key) {
      break;
    }
    // Reject a malformed value.
    if (!version_decode_value(it->value(), &v)) {
      return set_error(SM_ERR_CORRUPT, "malformed version value");
    }
    // Is it visible at the view?
    int r = visible(s->db, v, view);
    // Fail on a status lookup error.
    if (r < 0) {
      return -1;
    }
    // Visible: this version decides.
    if (r == 1) {
      // A tombstone: the key is absent.
      if (v.flags & version_tombstone) {
        return 1;
      }
      // Copy the value out.
      out->assign(v.payload.data(), v.payload.size());
      return 0;
    }
    // Invisible: under SSI, owe its writer an edge.
    if (skip_version(s, v, view) != 0) {
      return -1;
    }
  }
  // Fail if the iterator failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "read");
  }
  return 1;
}

/* Check existence, then put one new version (§5). */
int access_write(sm_session_t *s, uint64_t storeid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data, std::string *old) {
  // Find the store's CF.
  rocksdb::ColumnFamilyHandle *cf = store_cf(s->db, storeid);
  if (!cf) {
    return -1;
  }
  // Refuse writes after fail-stop.
  if (xct_check_writable(s->db) != 0) {
    return -1;
  }
  // Take the row lock; this blocks while another transaction
  // holds it, and fails if we are chosen as a deadlock victim.
  if (lock_acquire(s, storeid, key, true) != 0) {
    return xct_fail_retryable(s, SM_ERR_DEADLOCK,
                              "chosen as a deadlock victim");
  }
  bool live;
  uint64_t commit_ts;
  // Find the version this write checks against, keeping its
  // value when the caller asked for the old image.
  if (latest_version(s, cf, key, &live, &commit_ts, old) != 0) {
    return -1;
  }
  // Something committed after we began (§5.2).
  if (commit_ts > s->snapshot_ts) {
    // Read committed: an insert goes ahead against the newest
    // version; an update or delete must have read it first.
    if (s->level == SM_XCT_READ_COMMITTED) {
      if (op != WRITE_INSERT && !lock_has_latest(s, storeid, key)) {
        return set_error(SM_ERR_UPDATED,
                         "key was changed by a transaction that committed "
                         "after this one began; sm_read_latest it first");
      }
    } else {
      // First-committer-wins, aborting the loser.
      return xct_fail_retryable(
          s, SM_ERR_CONFLICT,
          "key was changed by a transaction that committed after "
          "this one began");
    }
  }
  // An insert needs the key absent.
  if (op == WRITE_INSERT && live) {
    return set_error(SM_ERR_DUPKEY, "key already exists");
  }
  // An update or delete needs it present.
  if (op != WRITE_INSERT && !live) {
    return set_error(SM_ERR_NOTFOUND, "key does not exist");
  }
  // Refuse a transaction's 2^32nd write.
  if (s->seq == UINT32_MAX) {
    return set_error(SM_ERR_INVALID, "too many writes in one transaction");
  }
  // Allocate the xid at the first write (§4.1).
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  // This write's seq, one past the last.
  uint32_t seq = s->seq + 1;
  // A delete writes a tombstone version.
  uint8_t flags = op == WRITE_DELETE ? version_tombstone : 0;
  // Allocate the write timestamp now that the lock is held, so it
  // sorts after every committed version of the key (§4.1).
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  // The previous writer took its commit_ts before releasing the lock.
  assert(commit_ts < wts);
  // Put the version eagerly; it is not synced (§5 step 5).
  rocksdb::Status st = s->db->rdb->Put(
      rocksdb::WriteOptions(), cf, version_key(key, wts, seq),
      version_value(flags, s->xid,
                    op == WRITE_DELETE ? rocksdb::Slice() : data));
  // Fail if the put failed.
  if (!st.ok()) {
    return set_status_error(st, "write version");
  }
  // Advance seq, and make the write visible to ourselves.
  s->seq = seq;
  s->seq_hwm = seq;
  // The seek bound of our own reads must cover this version.
  s->wts_hi = wts;
  // The write used the re-read; the next needs its own.
  lock_clear_latest(s, storeid, key);
  // Record it for savepoint rollback.
  s->undo.push_back({storeid, key.ToString(), wts, seq});
  // After the put, look for readers the write invalidates; a
  // reader arriving later steps past the version instead (§7.2).
  ssi_on_write(s, storeid, key);
  return 0;
}

/*
 * §5.2's check on its own, for sm_lock: fail if a version of
 * key committed after this transaction's begin snapshot. The
 * caller holds the lock, so no one can add one behind us.
 */
int access_check_current(sm_session_t *s, uint64_t storeid,
                         rocksdb::Slice key) {
  // Find the store's CF.
  rocksdb::ColumnFamilyHandle *cf = store_cf(s->db, storeid);
  if (!cf) {
    return -1;
  }
  bool live;
  uint64_t commit_ts;
  // Find the newest committed version of the key.
  if (latest_version(s, cf, key, &live, &commit_ts, nullptr) != 0) {
    return -1;
  }
  // Committed after we began: what we would read is stale.
  if (commit_ts > s->snapshot_ts) {
    return xct_fail_retryable(
        s, SM_ERR_CONFLICT,
        "key was changed by a transaction that committed after "
        "this one began");
  }
  return 0;
}

} // namespace wisr

/* Move an open cursor to key, forward only. */
int sm_scan_seek(sm_scan_t *scan, const char *key, size_t klen, int inclusive) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return scan_seek(scan, key, klen, inclusive);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_scan_seek: out of memory");
  }
}

/* Take a row lock without writing the row. */
int sm_lock(sm_session_t *session, uint64_t storeid, const char *key,
            size_t klen, sm_lockmode_t mode) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return lock_row(session, storeid, key, klen, mode);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_lock: out of memory");
  }
}

/* Insert a row; the key must not exist. */
int sm_insert(sm_session_t *session, uint64_t storeid, const char *key,
              size_t klen, const char *data, size_t datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_INSERT, storeid, key, klen, data, datalen,
                     nullptr, nullptr);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_insert: out of memory");
  }
}

/* Replace a row's value; the key must exist. */
int sm_update(sm_session_t *session, uint64_t storeid, const char *key,
              size_t klen, const char *data, size_t datalen, const char **old,
              size_t *oldlen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_UPDATE, storeid, key, klen, data, datalen,
                     old, oldlen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_update: out of memory");
  }
}

/* Delete a row; the key must exist. */
int sm_delete(sm_session_t *session, uint64_t storeid, const char *key,
              size_t klen, const char **old, size_t *oldlen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_DELETE, storeid, key, klen, nullptr, 0, old,
                     oldlen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_delete: out of memory");
  }
}

/* Read one key. */
int sm_read(sm_session_t *session, uint64_t storeid, const char *key,
            size_t klen, const char **data, size_t *datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return read_row(session, storeid, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_read: out of memory");
  }
}

/* Read the newest committed version of a key. */
int sm_read_latest(sm_session_t *session, uint64_t storeid, const char *key,
                   size_t klen, const char **data, size_t *datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return read_latest_row(session, storeid, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_read_latest: out of memory");
  }
}

/* Open a cursor over a key range. */
sm_scan_t *sm_scan(sm_session_t *session, uint64_t storeid, const char *skey,
                   size_t sklen, int skey_inclusive, const char *ekey,
                   size_t eklen, int ekey_inclusive) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return scan(session, storeid, skey, sklen, skey_inclusive, ekey, eklen,
                ekey_inclusive);
  } catch (const std::bad_alloc &) {
    set_error(SM_ERR_NOMEM, "sm_scan: out of memory");
    return nullptr;
  }
}

/* Advance a cursor to its next row. */
int sm_scannxt(sm_scan_t *scan, const char **key, size_t *klen,
               const char **data, size_t *datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return scan_next(scan, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    // The cursor is unusable after this.
    scan->state = -1;
    return set_error(SM_ERR_NOMEM, "sm_scannxt: out of memory");
  }
}

/* Close a cursor: the iterator first, then its snapshot. */
void sm_scanend(sm_scan_t *scan) {
  // Closing NULL is a no-op.
  if (!scan) {
    return;
  }
  // Keep the database pointer past the free.
  db_t *db = scan->session->db;
  ssi_range_close(scan); // narrow the SIREAD range to what was read
  // Unregister the cursor from its session.
  scan->session->scans.erase(scan);
  // Destroy the iterator before its snapshot and bound.
  scan->it.reset();
  // Release the RocksDB snapshot.
  db->rdb->ReleaseSnapshot(scan->snap);
  // Stop holding back the CF:txn sweep.
  db_read_leave(db, scan->epoch);
  // Free the cursor.
  delete scan;
}
