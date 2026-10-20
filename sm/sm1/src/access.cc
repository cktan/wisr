/*
 * access.cc -- sm1::insert, sm1::update, sm1::erase, sm1::read, and
 * the sm1::scan cursor (SM1_DESIGN.md §4.4, §5, §10).
 *
 * Under SERIALIZABLE, reads leave SIREAD entries and report the
 * concurrent versions they step past, and writes report the
 * readers they invalidate (ssi.cc, §7).
 */

#include "error.h"
#include "session.h"
#include "sm0_defer.h"
#include "version.h"

#include <algorithm>
#include <cassert>

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
int skip_version(sm1::session_t *s, const version_t &v, const view_t &view) {
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

/* The store storeid; its absence after a catalog hit is corruption. */
store_t store_of(db_t *db, uint64_t storeid) {
  // Look up the store's instance and CF.
  store_t st = db_store(db, storeid);
  // The catalog named it, so a missing CF is corruption.
  if (!st.cf) {
    set_error(sm1::SM_ERR_CORRUPT, "column family store.{} is missing",
              storeid);
  }
  return st;
}

/*
 * Hand back the payload of a live version the iterator is on:
 * left in the iterator the pin then holds, or copied into value.
 */
void keep_value(const version_t &v, std::unique_ptr<rocksdb::Iterator> &it,
                std::string *value, sm1::pin_t *pin, bool live) {
  if (!live) {
    return;
  }
  if (pin) {
    pin->row = v.payload;
    pin->it = std::move(it);
  } else if (value) {
    value->assign(v.payload.data(), v.payload.size());
  }
}

/*
 * The version a write checks against (§5 step 3): the newest
 * own version still visible to the transaction, else the newest
 * committed version, whatever its commit_ts. Sets *live to
 * whether it exists and is not a tombstone, and *commit_ts to
 * its commit_ts (0 for an own version or none), and *own_seq, if
 * given, to an own version's seq (0 if a committed one or none
 * decides). With inflight given, a version written by another
 * transaction still running stops the search: *inflight is set to
 * its xid, for the caller to wait for (WRITELOCK.md §3.2), and is
 * 0 otherwise. Without it such a version is skipped. A live
 * version's payload is copied into *value, or, with pin given,
 * left in the iterator the pin then holds. Returns 0 or -1.
 */
int latest_version(sm1::session_t *s, store_t st, rocksdb::Slice key,
                   bool *live, uint64_t *commit_ts, std::string *value,
                   uint32_t *own_seq = nullptr, uint64_t *inflight = nullptr,
                   sm1::pin_t *pin = nullptr) {
  // Assume no version until one is found.
  *live = false;
  *commit_ts = 0;
  if (own_seq) {
    *own_seq = 0;
  }
  if (inflight) {
    *inflight = 0;
  }
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
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
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
      return set_error(sm1::SM_ERR_CORRUPT, "malformed version value");
    }
    // Our own version: it decides, unless rolled back.
    if (s->xid != 0 && v.xid == s->xid) {
      // Still in effect: it is the latest.
      if (v.seq <= s->seq_hwm) {
        *live = !(v.flags & version_tombstone);
        if (own_seq) {
          *own_seq = v.seq;
        }
        // Hand back what it holds, for the old image.
        keep_value(v, it, value, pin, *live);
        return 0;
      }
      continue;
    }
    // Assume a stamped version committed.
    int committed = 1;
    uint64_t ts = v.commit_ts;
    // Resolve an unstamped version's writer.
    if (!(v.flags & version_stamped)) {
      xct_state_t st = xct_status(s->db, v.xid, &ts);
      // Fail on a status lookup error.
      if (st == XCT_ERROR) {
        return -1;
      }
      // Another transaction's write still in flight: its version
      // is its lock, and the caller waits for it.
      if (st == XCT_IN_FLIGHT && inflight) {
        *inflight = v.xid;
        return 0;
      }
      committed = st == XCT_COMMITTED ? 1 : 0;
    }
    // The newest committed version decides.
    if (committed) {
      *live = !(v.flags & version_tombstone);
      *commit_ts = ts;
      // Hand back what it holds, for the old image.
      keep_value(v, it, value, pin, *live);
      return 0;
    }
  }
  // Fail if the iterator failed.
  if (!it->status().ok()) {
    return set_status_error(it->status(), "read versions");
  }
  return 0;
}

/* Check a caller's key: non-NULL unless empty, within sm1::max_keylen. */
int check_key(const char *key, size_t klen) {
  // Only an empty key may be NULL.
  if (!key && klen > 0) {
    return set_error(sm1::SM_ERR_INVALID, "key is NULL");
  }
  // Reject a key over the limit.
  if (klen > sm1::max_keylen) {
    return set_error(sm1::SM_ERR_INVALID, "key of {} bytes exceeds {}", klen,
                     sm1::max_keylen);
  }
  return 0;
}

/*
 * Common entry checks for a data operation: a valid key, and a
 * storeid that names a live store at view.
 */
int enter(sm1::session_t *s, const view_t &view, uint64_t storeid,
          const char *key, size_t klen) {
  // Validate the key.
  if (check_key(key, klen) != 0) {
    return -1;
  }
  // Store 0, the catalog, always exists, and takes a caller's key
  // only under the level above's prefix.
  if (storeid == 0) {
    if (klen == 0 || key[0] != sm1::upper_prefix) {
      return set_error(sm1::SM_ERR_INVALID,
                       "store 0 takes only keys under the upper prefix");
    }
    return 0;
  }
  // Look the store up at the view.
  int r = catalog_lookup(s, view, storeid);
  // Report an unknown store.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  return r;
}

/* sm1::insert / sm1::update / sm1::erase, minus the out-of-memory guard. */
int write_row(sm1::session_t *s, write_op_t op, uint64_t storeid,
              const char *key, size_t klen, const char *data, size_t datalen,
              const char **old, size_t *oldlen) {
  // The out-parameters come as a pair; check before either is
  // written to.
  if ((old != nullptr) != (oldlen != nullptr)) {
    return set_error(sm1::SM_ERR_INVALID, "old and oldlen must both be given");
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
    return set_error(sm1::SM_ERR_INVALID, "data is NULL");
  }
  // Reject a value over the limit.
  if (datalen > sm1::max_datalen) {
    return set_error(sm1::SM_ERR_INVALID, "value of {} bytes exceeds {}",
                     datalen, sm1::max_datalen);
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

/*
 * A lock on a name in a namespace nothing writes: no store is
 * read and no version is checked, since none can exist. Only
 * the lock table is touched (sm1.h, locktype_t).
 */
int lock_name(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
              const char *key, size_t klen, sm1::lockmode_t mode) {
  rocksdb::Slice k(key ? key : "", klen);
  for (;;) {
    // No writer of this name can exist, so no latch is needed.
    if (lock_try(s, type, storeid, k, mode) == 0) {
      return 0;
    }
    // Wait for the holders that conflict.
    int waited = lock_wait(s, type, storeid, k, mode);
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
  }
}

/* sm1::lock, minus the out-of-memory guard. */
int lock_row(sm1::session_t *s, sm1::locktype_t type, uint64_t storeid,
             const char *key, size_t klen, sm1::lockmode_t mode,
             bool nowait = false) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject a mode that is not one of Gray's five.
  if (static_cast<int>(mode) < 0 || static_cast<int>(mode) > 4) {
    return set_error(sm1::SM_ERR_INVALID, "unknown lock mode {}", (int)mode);
  }
  // Reject a type that is neither of the two.
  if (type > sm1::LOCKTYPE_OID) {
    return set_error(sm1::SM_ERR_INVALID, "unknown lock type {}", (int)type);
  }
  // An oid is 8 bytes, so a name of any other length is a mistake.
  if (type == sm1::LOCKTYPE_OID && klen != 8) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a LOCKTYPE_OID key is 8 bytes, not {}", klen);
  }
  // A name outside any store takes the lock and nothing else.
  if (type != sm1::LOCKTYPE_ROW) {
    if (!nowait) {
      return lock_name(s, type, storeid, key, klen, mode);
    }
    // Taken, or converted, only if no other holder conflicts.
    return lock_try(s, type, storeid, rocksdb::Slice(key ? key : "", klen),
                    mode) == 0
               ? 0
               : 1;
  }
  // Validate the storeid and key at the current view.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  rocksdb::Slice k(key ? key : "", klen);
  // Find the store's CF.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return -1;
  }
  // A temporary store is the session's alone: there is no one to
  // lock a row against (TEMPTABLE.md §3.4).
  if (st.owner) {
    return 0;
  }
  for (;;) {
    uint64_t inflight;
    {
      // Under the key's latch, so no write of it comes between
      // the check and the lock (WRITELOCK.md §3.3).
      std::lock_guard<std::mutex> latch(lock_latch(s->db, storeid, k));
      bool live;
      uint64_t commit_ts;
      if (latest_version(s, st, k, &live, &commit_ts, nullptr, nullptr,
                         &inflight) != 0) {
        return -1;
      }
      // Free of uncommitted writes: take the lock if no holder
      // conflicts.
      if (inflight == 0 &&
          lock_try(s, sm1::LOCKTYPE_ROW, storeid, k, mode) == 0) {
        break;
      }
    }
    // Asked not to wait: the row is another's for now.
    if (nowait) {
      return 1;
    }
    // Wait, without the latch, for the uncommitted writer, or for
    // the holders that conflict.
    int waited = inflight != 0
                     ? lock_wait_xact(s, inflight)
                     : lock_wait(s, sm1::LOCKTYPE_ROW, storeid, k, mode);
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
  }
  // Read committed does not fail here: the caller reads the
  // row with sm1::read_latest instead.
  if (s->level == sm1::xctlevel_t::read_committed) {
    return 0;
  }
  // Now that the lock is held, run §5.2's check, so what the
  // caller reads next under this lock is current: a version
  // committed after this transaction began fails the call.
  return access_check_current(s, storeid, k);
}

/* sm1::read into ref, minus the out-of-memory guard. */
int read_row(sm1::session_t *s, uint64_t storeid, const char *key, size_t klen,
             sm1::pin_ref_t &ref, const char **data, size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_read: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // A read into ref replaces the row it held.
  sm1::unpin(ref);
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
  // Read the key into a pin, which keeps the row where it is.
  sm1::pin_t *p = pin_take(s);
  int r = access_read(s, storeid, view, rocksdb::Slice(key ? key : "", klen),
                      nullptr, p);
  // No row: the pin goes back unused.
  if (r != 0) {
    pin_put(p);
    return r;
  }
  // Found: hold it, and hand out its bytes.
  pin_hold(p, ref);
  *data = p->row.data();
  *datalen = p->row.size();
  return 0;
}

/* sm1::read_latest into ref, minus the out-of-memory guard. */
int read_latest_row(sm1::session_t *s, uint64_t storeid, const char *key,
                    size_t klen, sm1::pin_ref_t &ref, const char **data,
                    size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // A read into ref replaces the row it held.
  sm1::unpin(ref);
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Only read committed has a snapshot to look past.
  if (s->level != sm1::xctlevel_t::read_committed) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires READ COMMITTED");
  }
  // Validate the storeid and key.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  rocksdb::Slice k(key ? key : "", klen);
  // Find the store's CF.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return -1;
  }
  bool live;
  uint64_t commit_ts;
  uint32_t own_seq;
  // Read the newest version into a pin, ignoring the snapshot;
  // the pin goes back unused unless a row is handed out.
  sm1::pin_t *p = pin_take(s);
  sm0::scope_guard_t put([&] { pin_put(p); });
  if (latest_version(s, st, k, &live, &commit_ts, nullptr, &own_seq, nullptr,
                     p) != 0) {
    return -1;
  }
  // Something must keep the newest version from changing: a lock
  // on the key, every mode of which a writer waits for, or a
  // write of our own still in flight, which every other writer
  // waits for (WRITELOCK.md §3.2).
  sm1::lockmode_t mode;
  bool held = lock_held(s, storeid, k, &mode);
  if (!held && own_seq == 0 && !st.owner) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires a lock held on the key, or "
                     "a write of it by this transaction");
  }
  // An exclusive holder may now write.
  if (held && mode == sm1::SM_LOCKMODE_EX) {
    lock_set_latest(s, storeid, k);
  }
  // Absent, or deleted.
  if (!live) {
    return 1;
  }
  // Found: hold it, and hand out its bytes.
  put.dismiss();
  pin_hold(p, ref);
  *data = p->row.data();
  *datalen = p->row.size();
  return 0;
}

/* sm1::scan, minus the out-of-memory guard. */
sm1::scan_t *scan(sm1::session_t *s, uint64_t storeid, const char *skey,
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
  // A scan of store 0 stays inside the upper prefix: it starts
  // under it and ends under it, or at its successor, exclusive.
  if (storeid == 0) {
    const char up = sm1::upper_prefix, next = sm1::upper_prefix + 1;
    bool start_in = skey && sklen > 0 && skey[0] == up;
    bool end_in =
        ekey && eklen > 0 &&
        (ekey[0] == up || (eklen == 1 && ekey[0] == next && !ekey_inclusive));
    if (!start_in || !end_in) {
      set_error(sm1::SM_ERR_INVALID,
                "a scan of store 0 must stay under the upper prefix");
      return nullptr;
    }
  } else if (enter(s, view, storeid, "", 0) != 0) {
    // Validate the storeid.
    return nullptr;
  }
  // Open the cursor on the store's column family.
  return access_scan(s, storeid, view, skey, sklen, skey_inclusive, ekey, eklen,
                     ekey_inclusive);
}

} // namespace

namespace wisr {

/* Open a cursor over storeid at view, between the given bounds. */
sm1::scan_t *access_scan(sm1::session_t *s, uint64_t storeid,
                         const view_t &view, const char *skey, size_t sklen,
                         int skey_inclusive, const char *ekey, size_t eklen,
                         int ekey_inclusive) {
  // Find the column family.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return nullptr;
  }

  // Allocate the cursor.
  std::unique_ptr<sm1::scan_t> c(new sm1::scan_t);
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
  // the SIREAD entry (§10.2). A temporary store takes none.
  if (!st.owner) {
    ssi_range_open(c.get(), storeid, skey, sklen, skey_inclusive, ekey, eklen,
                   ekey_inclusive);
  }
  // Count the cursor as a reader for its whole life (§8.3),
  // then take the RocksDB snapshot: its physical view (§10.2).
  c->epoch = db_read_enter(s->db);
  c->rdb = st.rdb;
  c->snap = st.rdb->GetSnapshot();

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
  c->it.reset(st.rdb->NewIterator(ro, st.cf));
  // Position at skey, or at the start.
  if (skey) {
    // Build the start bound; sm1::scan_seek compares against it.
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

/* What a cursor a rollback closed says when it is used (§6.5). */
constexpr const char *rolled_back_msg =
    "the cursor was closed by a rollback to a savepoint set before it opened";

/* sm1::scan_seek, minus the out-of-memory guard. */
int scan_seek(sm1::scan_t *c, const char *key, size_t klen, int inclusive) {
  // Reject a NULL cursor.
  if (!c) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_scan_seek: scan is NULL");
  }
  // Validate the target key.
  if (check_key(key, klen) != 0) {
    return -1;
  }
  // A cursor a rollback closed reads no more.
  if (c->rolled_back) {
    return set_error(sm1::SM_ERR_INVALID, rolled_back_msg);
  }
  // Nor does one of a transaction a rollback doomed (§6.5).
  if (c->session->active && c->session->doomed == sm1::SM_ERR_DOOMED) {
    return xct_check_active(c->session);
  }
  // A failed cursor stays failed.
  if (c->state == -1) {
    return set_error(sm1::SM_ERR_INVALID,
                     "the cursor failed and cannot be reused");
  }
  rocksdb::Slice target(key ? key : "", klen);
  // Refuse a target before the last key examined: the cursor
  // moves forward only, and its SIREAD range is narrowed to
  // the last key it reached (§7.1, §10.2).
  if (c->examined && target.compare(rocksdb::Slice(c->last_examined)) < 0) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_scan_seek moves forward only; the cursor has "
                     "already read past that key");
  }
  // Build the seek bound, past the target if exclusive.
  std::string bound = version_bound(target, inclusive ? '\x00' : '\xff');
  // Refuse a target below the range the cursor registered.
  if (bound < c->lower) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_scan_seek target is below the scan's start key");
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

/* sm1::scannxt, minus the out-of-memory guard. */
int scan_next(sm1::scan_t *c, const char **key, size_t *klen, const char **data,
              size_t *datalen) {
  // Require every out-parameter.
  if (!c || !key || !klen || !data || !datalen) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_scannxt: NULL argument");
  }
  // Report no row until one is found.
  *key = *data = nullptr;
  *klen = *datalen = 0;
  // A cursor a rollback closed reads no more.
  if (c->rolled_back) {
    return set_error(sm1::SM_ERR_INVALID, rolled_back_msg);
  }
  // Nor does one of a transaction a rollback doomed (§6.5).
  if (c->session->active && c->session->doomed == sm1::SM_ERR_DOOMED) {
    return xct_check_active(c->session);
  }
  // A cursor at its end, or failed, stays so.
  if (c->state != 0) {
    return c->state;
  }
  // The view frozen at sm1::scan.
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
        return set_error(sm1::SM_ERR_CORRUPT, "malformed version value");
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
int access_read(sm1::session_t *s, uint64_t storeid, const view_t &view,
                rocksdb::Slice key, std::string *out, sm1::pin_t *pin) {
  // Find the store's CF.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return -1;
  }
  // Under SERIALIZABLE, take the SIREAD entry before the
  // iterator exists, so a write landing in between still finds
  // it (§10.1). A temporary store takes none.
  if (!st.owner) {
    ssi_on_read(s, storeid, key);
  }
  // Hold a read epoch from before the iterator exists until we are
  // done resolving what it read: the CF:txn sweep waits for it.
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  // Stay within this userkey's versions.
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  // Open an iterator on the store.
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
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
      return set_error(sm1::SM_ERR_CORRUPT, "malformed version value");
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
      // Keep the iterator on the value for the pin, or copy it out.
      if (pin) {
        pin->row = v.payload;
        pin->it = std::move(it);
      } else {
        out->assign(v.payload.data(), v.payload.size());
      }
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
int access_write(sm1::session_t *s, uint64_t storeid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data, std::string *old) {
  // Find the store's CF.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return -1;
  }
  // Refuse writes after fail-stop, or in a read-only transaction,
  // which may still write a temporary store.
  if (xct_check_writable(s->db) != 0 ||
      (!st.owner && xct_check_readonly(s) != 0)) {
    return -1;
  }
  // A load open into the store has keys this write cannot be
  // checked against: they are not in the store yet.
  if (load_open_on(s, storeid)) {
    return set_error(sm1::SM_ERR_INVALID, "a bulk load is open into the store");
  }
  // Check the key and put the version under its latch, so no
  // other write of it comes between (WRITELOCK.md §3.2).
  std::unique_lock<std::mutex> latch(lock_latch(s->db, storeid, key));
  bool live;
  uint64_t commit_ts;
  uint32_t own_seq;
  for (;;) {
    int waited = 0;
    // Another transaction's explicit lock that a write conflicts
    // with: wait for it, without the latch.
    // No one else reaches a temporary store, so it has none.
    if (!st.owner && lock_write_check(s, storeid, key) != 0) {
      latch.unlock();
      waited =
          lock_wait(s, sm1::LOCKTYPE_ROW, storeid, key, sm1::SM_LOCKMODE_EX);
    } else {
      uint64_t inflight;
      // Find the version this write checks against, keeping its
      // value when the caller asked for the old image.
      if (latest_version(s, st, key, &live, &commit_ts, old, &own_seq,
                         &inflight) != 0) {
        return -1;
      }
      // Done looking: this write goes on with the latch held.
      if (inflight == 0) {
        break;
      }
      // Another transaction's uncommitted version is its lock:
      // wait for that transaction, without the latch.
      latch.unlock();
      waited = lock_wait_xact(s, inflight);
    }
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
    // Look again from the start.
    latch.lock();
  }
  // The running statement changed this row already: it cannot
  // see that change, so it must not change the row again (§6.2).
  if (op != WRITE_INSERT && s->stmt_marked && own_seq > s->stmt_seq) {
    return set_error(sm1::SM_ERR_SELFMODIFIED,
                     "the row was already changed by this statement");
  }
  // Something committed after we began (§5.2).
  if (commit_ts > s->snapshot_ts) {
    // Read committed: an insert goes ahead against the newest
    // version; an update or delete must have read it first.
    if (s->level == sm1::xctlevel_t::read_committed) {
      if (op != WRITE_INSERT && !lock_has_latest(s, storeid, key)) {
        // Keep the key locked until the re-read and the repeated
        // write, so its newest version cannot change in between
        // (§5.4, WRITELOCK.md §3.5). No other holder conflicts:
        // the check above found none, under the latch.
        lock_try(s, sm1::LOCKTYPE_ROW, storeid, key, sm1::SM_LOCKMODE_EX);
        return set_error(sm1::SM_ERR_UPDATED,
                         "key was changed by a transaction that committed "
                         "after this one began; sm1_read_latest it first");
      }
    } else {
      // First-committer-wins, aborting the loser.
      return xct_fail_retryable(
          s, sm1::SM_ERR_CONFLICT,
          "key was changed by a transaction that committed after "
          "this one began");
    }
  }
  // An insert needs the key absent.
  if (op == WRITE_INSERT && live) {
    return set_error(sm1::SM_ERR_DUPKEY, "key already exists");
  }
  // An update or delete needs it present.
  if (op != WRITE_INSERT && !live) {
    return set_error(sm1::SM_ERR_NOTFOUND, "key does not exist");
  }
  // Refuse a transaction's 2^32nd write.
  if (s->seq == UINT32_MAX) {
    return set_error(sm1::SM_ERR_INVALID, "too many writes in one transaction");
  }
  // Allocate the xid at the first write (§4.1).
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  // This write's seq, one past the last.
  uint32_t seq = s->seq + 1;
  // A delete writes a tombstone version.
  uint8_t flags = op == WRITE_DELETE ? version_tombstone : 0;
  // Allocate the write timestamp now that no other writer of the
  // key is in flight, so it sorts after every committed version
  // of the key (§4.1, WRITELOCK.md §3.6).
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  // The previous writer took its commit_ts before it ended.
  assert(commit_ts < wts);
  // Put the version eagerly; it is not synced (§5 step 5).
  std::string vkey = version_key(key, wts, seq);
  std::string vval = version_value(
      flags, s->xid, op == WRITE_DELETE ? rocksdb::Slice() : data);
  rocksdb::Status status =
      st.rdb->Put(store_write_options(st), st.cf, vkey, vval);
  // Fail if the put failed.
  if (!status.ok()) {
    return set_status_error(status, "write version");
  }
  count_write(st, std::string_view(key.data(), key.size()),
              vkey.size() + vval.size());
  // Advance seq, and make the write visible to ourselves.
  s->seq = seq;
  s->seq_hwm = seq;
  // The seek bound of our own reads must cover this version.
  s->wts_hi = wts;
  // The write used the re-read; the next needs its own.
  lock_clear_latest(s, storeid, key);
  // Record it for savepoint rollback, if a savepoint could
  // still roll it back; with none set, or every one lost,
  // nothing is kept (§6.5).
  if (s->savepoints.size() > s->lost_marks) {
    s->undo.push_back({storeid, key.ToString(), wts, seq});
    s->undo_bytes += sizeof(sm1::session_t::undo_t) + key.size();
    // Past the cap: give up the savepoints, not the write.
    if (s->undo_bytes > s->db->undo_max_bytes) {
      xct_undo_overflow(s);
    }
  }
  // A temporary store has no other reader, and bars a prepare.
  if (st.owner) {
    s->temp_touched = true;
    return 0;
  }
  // After the put, look for readers the write invalidates; a
  // reader arriving later steps past the version instead (§7.2).
  ssi_on_write(s, storeid, key);
  return 0;
}

/*
 * §5.2's check on its own, for sm1::lock: fail if a version of
 * key committed after this transaction's begin snapshot. The
 * caller holds the lock, so no one can add one behind us.
 */
int access_check_current(sm1::session_t *s, uint64_t storeid,
                         rocksdb::Slice key) {
  // Find the store's CF.
  store_t st = store_of(s->db, storeid);
  if (!st.cf) {
    return -1;
  }
  bool live;
  uint64_t commit_ts;
  // Find the newest committed version of the key.
  if (latest_version(s, st, key, &live, &commit_ts, nullptr) != 0) {
    return -1;
  }
  // Committed after we began: what we would read is stale.
  if (commit_ts > s->snapshot_ts) {
    return xct_fail_retryable(
        s, sm1::SM_ERR_CONFLICT,
        "key was changed by a transaction that committed after "
        "this one began");
  }
  return 0;
}

} // namespace wisr

/* Move an open cursor to key, forward only. */
int sm1::scan_seek(sm1::scan_t *scan, std::string_view key, int inclusive) {
  return ::scan_seek(scan, key.data(), key.size(), inclusive);
}

/* Take a lock without writing anything; see sm1.h. */
int sm1::lock(sm1::session_t *session, sm1::locktype_t type, uint64_t storeid,
              std::string_view key, sm1::lockmode_t mode) {
  return lock_row(session, type, storeid, key.data(), key.size(), mode);
}

/* Take a lock if it can be had at once; see sm1.h. */
int sm1::trylock(sm1::session_t *session, sm1::locktype_t type,
                 uint64_t storeid, std::string_view key, sm1::lockmode_t mode) {
  return lock_row(session, type, storeid, key.data(), key.size(), mode, true);
}

/* Insert a row; the key must not exist. */
int sm1::insert(sm1::session_t *session, uint64_t storeid, std::string_view key,
                std::string_view data) {
  return write_row(session, WRITE_INSERT, storeid, key.data(), key.size(),
                   data.data(), data.size(), nullptr, nullptr);
}

/* Replace a row's value; the key must exist. */
int sm1::update(sm1::session_t *session, uint64_t storeid, std::string_view key,
                std::string_view data, std::string_view *old) {
  // Ask for the old row only if the caller wants it.
  const char *o = nullptr;
  size_t olen = 0;
  int rc = write_row(session, WRITE_UPDATE, storeid, key.data(), key.size(),
                     data.data(), data.size(), old ? &o : nullptr,
                     old ? &olen : nullptr);
  // Hand it back: the old row, or empty on failure.
  if (old) {
    *old = std::string_view(o, olen);
  }
  return rc;
}

/* Delete a row; the key must exist. */
int sm1::erase(sm1::session_t *session, uint64_t storeid, std::string_view key,
               std::string_view *old) {
  // Ask for the old row only if the caller wants it.
  const char *o = nullptr;
  size_t olen = 0;
  int rc = write_row(session, WRITE_DELETE, storeid, key.data(), key.size(),
                     nullptr, 0, old ? &o : nullptr, old ? &olen : nullptr);
  // Hand it back: the old row, or empty on failure.
  if (old) {
    *old = std::string_view(o, olen);
  }
  return rc;
}

/* Read one key into ref. */
int sm1::read(sm1::session_t *session, uint64_t storeid, std::string_view key,
              sm1::pin_ref_t &ref, std::string_view &data) {
  const char *d = nullptr;
  size_t dlen = 0;
  int rc = read_row(session, storeid, key.data(), key.size(), ref, &d, &dlen);
  // Hand back the value: empty for none (1) and on failure.
  data = std::string_view(d, dlen);
  return rc;
}

/* Read one key, lent until the session's next call. */
int sm1::read(sm1::session_t *session, uint64_t storeid, std::string_view key,
              std::string_view &data) {
  // A NULL session has no ref: fail as any call on it does.
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  // Into the session's own ref, which the next such read reuses.
  return sm1::read(session, storeid, key, session->readref, data);
}

/* Read the newest committed version of a key into ref. */
int sm1::read_latest(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, sm1::pin_ref_t &ref,
                     std::string_view &data) {
  const char *d = nullptr;
  size_t dlen = 0;
  int rc =
      read_latest_row(session, storeid, key.data(), key.size(), ref, &d, &dlen);
  // Hand back the value: empty for none (1) and on failure.
  data = std::string_view(d, dlen);
  return rc;
}

/* Read the newest committed version of a key, lent as sm1::read's. */
int sm1::read_latest(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, std::string_view &data) {
  // A NULL session has no ref: fail as any call on it does.
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  // Into the session's own ref, which sm1::read shares.
  return sm1::read_latest(session, storeid, key, session->readref, data);
}

/* Open a cursor over a key range. */
int sm1::scan(sm1::session_t *session, uint64_t storeid, const char *skey,
              size_t sklen, int skey_inclusive, const char *ekey, size_t eklen,
              int ekey_inclusive, sm1::scan_t *&out) {
  // Open the cursor, handing it back only on success.
  sm1::scan_t *c = ::scan(session, storeid, skey, sklen, skey_inclusive, ekey,
                          eklen, ekey_inclusive);
  if (!c) {
    return -1;
  }
  out = c;
  return 0;
}

/* Advance a cursor to its next row. */
int sm1::scannxt(sm1::scan_t *scan, std::string_view &key,
                 std::string_view &data) {
  const char *k = nullptr, *d = nullptr;
  size_t klen = 0, dlen = 0;
  int rc = scan_next(scan, &k, &klen, &d, &dlen);
  // Hand back both: empty at the end (1) and on failure.
  key = std::string_view(k, klen);
  data = std::string_view(d, dlen);
  return rc;
}

/* Release a cursor's hold: the iterator first, then its snapshot. */
void wisr::scan_release(sm1::scan_t *scan) {
  ssi_range_close(scan); // narrow the SIREAD range to what was read
  // Destroy the iterator before its snapshot and bound.
  scan->it.reset();
  // Release the RocksDB snapshot.
  scan->rdb->ReleaseSnapshot(scan->snap);
  // Stop holding back the CF:txn sweep.
  db_read_leave(scan->session->db, scan->epoch);
}

/* Close a cursor: release its hold, unless a rollback did, and free it. */
void sm1::scanend(sm1::scan_t *scan) {
  // Closing NULL is a no-op.
  if (!scan) {
    return;
  }
  if (!scan->rolled_back) {
    scan_release(scan);
  }
  // Unregister the cursor from its session.
  scan->session->scans.erase(scan);
  // Free the cursor.
  delete scan;
}
