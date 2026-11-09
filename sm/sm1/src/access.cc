/*
 * access.cc -- sm1::insert, sm1::update, sm1::erase, sm1::read, and
 * the sm1::scan cursor (../XCTMGR.md §3.3, ../ROWSTORE.md §4, §5).
 *
 * Under SERIALIZABLE, reads leave SIREAD entries and report the
 * concurrent versions they step past, and writes report the
 * readers they invalidate (ssi.cc, ../XCTMGR.md §7).
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

/*
 * 1 if version v is visible at view, 0 if not, -1 on error (../XCTMGR.md §3.3).
 * When v is not visible because its writer is dead, *dead is set, if given.
 */
int visible(db_t *db, const version_t &v, const view_t &view,
            bool *dead = nullptr) {
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
    switch (xct_status(db, v.xid, &commit_ts)) {
    case XCT_COMMITTED:
      break;
    case XCT_DEAD:
      // Not visible; the caller may queue the dead version for repair.
      if (dead) {
        *dead = true;
      }
      return 0;
    case XCT_ERROR:
      return -1;
    default:
      // In flight: not visible.
      return 0;
    }
  }
  // Visible if it committed at or before our snapshot.
  return commit_ts <= view.snapshot_ts;
}

/*
 * For SSI (../ROWSTORE.md §5.2): 1 if v, invisible to view, was written by a
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

/*
 * The store storeid, the session's handle of it (../DBMGR.md);
 * its absence after a catalog hit is corruption.
 */
const store_t &store_of(sm1::session_t *s, uint64_t storeid) {
  // Look up the store's instance and CF.
  const store_t &st = session_store(s, storeid);
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
                std::string *value, sm1::rawpin_t *pin, bool live) {
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
 * Whether the iterator pin holds may serve a read of storeid at
 * view, entered in read epoch epoch (../ROWSTORE.md §5.2): a point read of this
 * session opened it on the store, in the epoch still current,
 * after every commit the view sees and after the session's last
 * write. Not under SERIALIZABLE, whose SIREAD must come before the
 * iterator.
 */
bool reusable(const sm1::session_t *s, const sm1::rawpin_t &pin,
              uint64_t storeid, const view_t &view, uint64_t epoch) {
  const sm1::rawpin_t::reuse_t &r = pin.reuse;
  return pin.it && r.session == s && r.storeid == storeid &&
         r.write_gen == s->write_gen && view.snapshot_ts <= r.visible_ts &&
         r.epoch == epoch && !s->ssi && pin.it->status().ok();
}

/*
 * The version a write checks against (../ROWSTORE.md §4 step 3): the newest
 * own version still visible to the transaction, else the newest
 * committed version, whatever its commit_ts. Sets *live to
 * whether it exists and is not a tombstone, and *commit_ts to
 * its commit_ts (0 for an own version or none), and *own_seq, if
 * given, to an own version's seq (0 if a committed one or none
 * decides). With inflight given, a version written by another
 * transaction still running stops the search: *inflight is set to
 * its xid, for the caller to wait for (../LOCKMGR.md §1.7), and is
 * 0 otherwise. Without it such a version is skipped. A live
 * version's payload is copied into *value, or, with pin given,
 * left in the iterator the pin then holds. Returns 0 or -1.
 */
int latest_version(sm1::session_t *s, const store_t &st, rocksdb::Slice key,
                   bool *live, uint64_t *commit_ts, std::string *value,
                   uint32_t *own_seq = nullptr, uint64_t *inflight = nullptr,
                   sm1::rawpin_t *pin = nullptr, bool *moved = nullptr) {
  // Assume no version until one is found.
  *live = false;
  *commit_ts = 0;
  if (moved) {
    *moved = false;
  }
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
        if (moved) {
          *moved = v.flags & version_moved;
        }
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
      if (moved) {
        *moved = v.flags & version_moved;
      }
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

/*
 * sm1::insert / sm1::update / sm1::erase, minus the out-of-memory
 * guard. With pin, the row an update or erase replaces is pinned
 * into it, and old and oldlen view it.
 */
int write_row(sm1::session_t *s, write_op_t op, uint64_t storeid,
              const char *key, size_t klen, const char *data, size_t datalen,
              sm1::rawpin_t *pin = nullptr, const char **old = nullptr,
              size_t *oldlen = nullptr) {
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
  // Write the version, keeping the row it replaces in a pin of
  // its own when asked, as data may view the row pin holds.
  sm1::rawpin_t got;
  int r = access_write(s, storeid, op, rocksdb::Slice(key ? key : "", klen),
                       rocksdb::Slice(data ? data : "", datalen),
                       pin ? &got : nullptr);
  // Failed, or not asked: the pin keeps what it held.
  if (!pin || 0 != r) {
    return r;
  }
  // The row pin held goes only now, its iterator with it.
  *pin = std::move(got);
  *old = pin->row.data();
  *oldlen = pin->row.size();
  return 0;
}

/*
 * -1 unless s may take a lock in mode: an active transaction, and one of
 * Gray's five, or with row one of a row's modes too, KS and NKU
 * (../LOCKMGR.md §1.13).
 */
int lock_check(sm1::session_t *s, sm1::lockmode_t mode, bool row = false) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  int last = row ? sm1::SM_LOCKMODE_NKU : sm1::SM_LOCKMODE_SIX;
  if (static_cast<int>(mode) < 0 || static_cast<int>(mode) > last) {
    return set_error(sm1::SM_ERR_INVALID, "unknown lock mode {}", (int)mode);
  }
  return 0;
}

/* sm1::lock_oid and sm1::trylock_oid. */
int lock_object(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode,
                bool nowait) {
  if (lock_check(s, mode) != 0) {
    return -1;
  }
  // An oid outside any store takes the lock and nothing else: no
  // store is read and no version is checked, since none can exist.
  return wisr::lock_oid(s, oid, mode, nowait);
}

/* sm1::lock_row and sm1::trylock_row, minus the out-of-memory guard. */
int lock_versioned(sm1::session_t *s, uint64_t storeid, const char *key,
                   size_t klen, sm1::lockmode_t mode, bool nowait, bool as_read,
                   uint16_t kind) {
  if (lock_check(s, mode, true) != 0) {
    return -1;
  }
  // Validate the storeid and key at the current view.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  rocksdb::Slice k(key ? key : "", klen);
  // Find the store's CF.
  const store_t &st = store_of(s, storeid);
  if (!st.cf) {
    return -1;
  }
  // A temporary store is the session's alone: there is no one to
  // lock a row against (TEMPTABLE.md §3.4); only the transaction's
  // own later change of a row read refuses the lock.
  if (st.owner) {
    if (!as_read) {
      return 0;
    }
    bool live;
    uint64_t commit_ts;
    uint32_t own_seq;
    if (latest_version(s, st, k, &live, &commit_ts, nullptr, &own_seq) != 0) {
      return -1;
    }
    return lock_self_modified(s, own_seq);
  }
  // The commit_ts of the newest version, as read when the lock
  // was taken.
  uint64_t commit_ts = 0;
  for (;;) {
    uint64_t inflight;
    {
      // Under the key's latch, so no write of it comes between
      // the check and the lock (../LOCKMGR.md §1.5).
      std::lock_guard<std::mutex> latch(lock_latch(s->db, storeid, k));
      bool live;
      uint32_t own_seq;
      if (latest_version(s, st, k, &live, &commit_ts, nullptr, &own_seq,
                         &inflight) != 0) {
        return -1;
      }
      // A row read, changed by a later write of this transaction:
      // not locked.
      if (as_read && lock_self_modified(s, own_seq) != 0) {
        return -1;
      }
      // Free of uncommitted writes: take the lock if no holder
      // conflicts.
      if (inflight == 0 &&
          lock_try(s, lock_row_id(storeid, k), mode, kind) == 0) {
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
                     : lock_wait(s, lock_row_id(storeid, k), mode, kind);
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
  }
  // Read committed runs no check: the lock is held and the call
  // returns 0, never SM_ERR_CONFLICT, however newer the row's
  // last commit than the snapshot. The caller then reads the row
  // with sm1::read_latest, which needs this lock (../LOCKMGR.md §1.6).
  if (s->level == sm1::xctlevel_t::read_committed) {
    return 0;
  }
  // Run ../LOCKMGR.md §1.6's check, so what the caller reads next
  // under this lock is current: a version committed after this
  // transaction began fails the call. The read under the latch
  // serves it: the lock was taken with no write in flight, and
  // since then a writer waits for the lock, so no version came
  // between.
  if (commit_ts > s->snapshot_ts) {
    return xct_fail_retryable(
        s, sm1::SM_ERR_CONFLICT,
        "key was changed by a transaction that committed after "
        "this one began");
  }
  return 0;
}

/* sm1::read into pin, minus the out-of-memory guard. */
int read_row(sm1::session_t *s, uint64_t storeid, const char *key, size_t klen,
             sm1::rawpin_t *pin, const char **data, size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_read: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // A read into pin replaces the row it held. The pin keeps its
  // iterator for access_read to seek again (../ROWSTORE.md §5.2), and is
  // emptied unless a row is found.
  pin->row = rocksdb::Slice();
  bool found = false;
  defer {
    if (!found) {
      sm1::unpin(pin);
    }
  };
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
  // Read the key into the pin, which keeps the row where it is;
  // with no row it stays empty.
  int r = access_read(s, storeid, view, rocksdb::Slice(key ? key : "", klen),
                      nullptr, pin);
  if (r != 0) {
    return r;
  }
  // Found: hand out its bytes.
  found = true;
  *data = pin->row.data();
  *datalen = pin->row.size();
  return 0;
}

/* sm1::read_latest into pin, minus the out-of-memory guard. */
int read_latest_row(sm1::session_t *s, uint64_t storeid, const char *key,
                    size_t klen, sm1::rawpin_t *pin, const char **data,
                    size_t *datalen) {
  // Require the out-parameters.
  if (!data || !datalen) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest: data or datalen is NULL");
  }
  // Report no row until one is found.
  *data = nullptr;
  *datalen = 0;
  // A read into pin replaces the row it held.
  sm1::unpin(pin);
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Validate the storeid and key.
  if (enter(s, xct_view(s), storeid, key, klen) != 0) {
    return -1;
  }
  rocksdb::Slice k(key ? key : "", klen);
  // Find the store's CF.
  const store_t &st = store_of(s, storeid);
  if (!st.cf) {
    return -1;
  }
  bool live, moved;
  uint64_t commit_ts;
  uint32_t own_seq;
  // Read the newest version into the pin, ignoring the snapshot;
  // it is emptied again unless a row is handed out.
  sm0::scope_guard_t put([&] { sm1::unpin(pin); });
  if (0 != latest_version(s, st, k, &live, &commit_ts, nullptr, &own_seq,
                          nullptr, pin, &moved)) {
    return -1;
  }
  // Only read committed has a snapshot to look past. Above it the
  // newest version is read only if it is the transaction's own,
  // which no snapshot hides from it and no other writer changes:
  // an insert's arbiter finding a key its own statement took.
  if (s->level != sm1::xctlevel_t::read_committed && own_seq == 0) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires READ COMMITTED, or a "
                     "version of this transaction's own");
  }
  // Something must keep the newest version from changing: a lock
  // on the key, every mode of which a writer waits for, or a
  // write of our own still in flight, which every other writer
  // waits for (../LOCKMGR.md §1.7).
  sm1::lockmode_t mode;
  bool held = lock_held(s, storeid, k, &mode);
  if (!held && own_seq == 0 && !st.owner) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires a lock held on the key, or "
                     "a write of it by this transaction");
  }
  // A holder no other writer passes, X or NKU, may now write: an
  // NKU holder's write that changes the key converts to X first.
  if (held && (mode == sm1::SM_LOCKMODE_EX || mode == sm1::SM_LOCKMODE_NKU)) {
    lock_set_latest(s, storeid, k);
  }
  // Moved to another key: the caller's row lives on there.
  if (moved) {
    return set_error(sm1::SM_ERR_MOVED, "the row moved to another key");
  }
  // Absent, or deleted.
  if (!live) {
    return 1;
  }
  // Found: keep it, and hand out its bytes.
  put.dismiss();
  *data = pin->row.data();
  *datalen = pin->row.size();
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
  const store_t &st = store_of(s, storeid);
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
  // the SIREAD entry (../ROWSTORE.md §5.6). A temporary store takes none.
  if (!st.owner) {
    ssi_range_open(c.get(), storeid, skey, sklen, skey_inclusive, ekey, eklen,
                   ekey_inclusive);
  }
  // Count the cursor as a reader for its whole life (../GC.md §1),
  // then take the RocksDB snapshot: its physical view (../ROWSTORE.md §5.6).
  c->epoch = db_read_enter(s->db);
  c->rdb = st.rdb;
  c->snap = st.rdb->GetSnapshot();

  // Read at the cursor's snapshot.
  rocksdb::ReadOptions ro;
  ro.snapshot = c->snap;
  ro.auto_prefix_mode = true; // a scan crosses userkeys (../ROWSTORE.md §5.6)
  // Bound the iterator at ekey, carrying a full tail; with none, at
  // the end of the versioned keys (../ROWSTORE.md §3.2).
  if (ekey) {
    // Build the bound; the cursor owns its bytes.
    c->upper = version_bound(rocksdb::Slice(ekey, eklen),
                             ekey_inclusive ? '\xff' : '\x00');
  } else {
    c->upper.assign(1, key_class_current);
  }
  // RocksDB borrows the bound through this slice.
  c->upper_slice = rocksdb::Slice(c->upper);
  // Install the bound.
  ro.iterate_upper_bound = &c->upper_slice;
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

/* What a cursor a rollback closed says when it is used (../XCTMGR.md §6). */
constexpr const char *rolled_back_msg =
    "the cursor was closed by a rollback to a savepoint set before it opened, "
    "or by the end of its transaction; its old view cannot be recreated";

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
    return set_error(sm1::SM_ERR_SCANCLOSED, rolled_back_msg);
  }
  // Nor does one of a transaction a rollback doomed (../XCTMGR.md §6).
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
  // the last key it reached (../XCTMGR.md §7.2, ../ROWSTORE.md §5.6).
  if (c->examined && target.compare(rocksdb::Slice(c->last_examined)) < 0) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_scan_seek moves forward only; the cursor has "
                     "already read past that key");
  }
  // A chained cursor's lower bound is its start key, as given.
  if (c->chained) {
    if (target.compare(rocksdb::Slice(c->lower)) < 0) {
      return set_error(sm1::SM_ERR_INVALID,
                       "sm1_scan_seek target is below the scan's start key");
    }
    if (c->state != 1) {
      chain_scan_seek(c, target, inclusive);
    }
    return 0;
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
  // still count the whole range as read (../XCTMGR.md §7.2).
  if (c->state == 1) {
    return 0;
  }
  // Reposition, keeping the iterator, its snapshot, and its
  // upper bound; the row handed out last is left behind.
  c->on_row = false;
  c->it->Seek(bound);
  return 0;
}

/*
 * sm1::scannxt of an unchained cursor, its checks made. Out of line,
 * so a chained cursor's step pays nothing for its frame.
 */
[[gnu::noinline]] int scan_next(sm1::scan_t *c, std::string_view &key,
                                std::string_view &data) {
  // The view frozen at sm1::scan.
  db_t *db = c->session->db;
  view_t view = {c->snapshot_ts, c->xid, c->seq_hwm};
  rocksdb::Iterator *it = c->it.get();

  // The row handed out last decided its key: step off it, and its
  // older versions are only stepped over, as they would have been
  // before it was handed out.
  bool decided = c->on_row;
  if (c->on_row) {
    c->on_row = false;
    it->Next();
  }

  // Walk the versions, newest first within a userkey's run: the
  // first visible one decides the key, the rest are stepped past.
  // Each version's key is decoded once.
  for (; it->Valid(); it->Next()) {
    version_t v;
    // Decode this version's userkey, wts, and seq.
    version_decode_key(it->key(), &v);
    // The first version of the next run: record how far the scan
    // has read, for SIREAD narrowing; the key's one copy, which is
    // also what is handed out. Nothing is decided about it yet.
    if (!c->examined || v.userkey != rocksdb::Slice(c->last_examined)) {
      c->examined = true;
      c->last_examined.assign(v.userkey.data(), v.userkey.size());
      decided = false;
    }
    // Once decided, only step past the older versions.
    if (decided) {
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
    if (1 == r) {
      // Stop judging this key's older versions.
      decided = true;
      // A live row, not a tombstone, which hides the key: hand it
      // out where the iterator has it, which stays on it until the
      // next call (on_row).
      if (!(v.flags & version_tombstone)) {
        c->on_row = true;
        key = c->last_examined;
        data = std::string_view(v.payload.data(), v.payload.size());
        return 0;
      }
    } else if (0 != skip_version(c->session, v, view)) {
      c->state = -1;
      return -1;
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

/* Seek to the newest version a reader at view could see (../ROWSTORE.md §5.2).
 */
int access_read(sm1::session_t *s, uint64_t storeid, const view_t &view,
                rocksdb::Slice key, std::string *out, sm1::rawpin_t *pin) {
  // Find the store's CF.
  const store_t &st = store_of(s, storeid);
  if (!st.cf) {
    return -1;
  }
  // Under SERIALIZABLE, take the SIREAD entry before the
  // iterator exists, so a write landing in between still finds
  // it (../ROWSTORE.md §5.2). A temporary store takes none.
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
  // Seek again the iterator pin holds when it can serve this
  // read, else open one on the store (../ROWSTORE.md §5.2).
  std::unique_ptr<rocksdb::Iterator> it;
  sm1::rawpin_t::reuse_t reuse;
  if (pin && reusable(s, *pin, storeid, view, epoch)) {
    it = std::move(pin->it);
    reuse = pin->reuse;
  } else {
    sm1::unpin(pin);
    // Every commit published by now is in an iterator opened next.
    reuse = {s, storeid, s->write_gen, s->db->visible_ts.load(), epoch};
    it.reset(st.rdb->NewIterator(ro, st.cf));
  }
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
        pin->reuse = reuse;
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

/* Check existence, then put one new version (../ROWSTORE.md §4). */
int access_write(sm1::session_t *s, uint64_t storeid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data, sm1::rawpin_t *pin) {
  // Find the store's CF.
  const store_t &st = store_of(s, storeid);
  if (!st.cf) {
    return -1;
  }
  // Refuse writes in a read-only transaction, which may still
  // write a temporary store.
  if (!st.owner && xct_check_readonly(s) != 0) {
    return -1;
  }
  // A load open into the store has keys this write cannot be
  // checked against: they are not in the store yet.
  if (load_open_on(s, storeid)) {
    return set_error(sm1::SM_ERR_INVALID, "a bulk load is open into the store");
  }
  // Check the key and put the version under its latch, so no
  // other write of it comes between (../LOCKMGR.md §1.7).
  std::unique_lock<std::mutex> latch(lock_latch(s->db, storeid, key));
  bool live, moved = false;
  uint64_t commit_ts;
  uint32_t own_seq;
  const bool inserting = op == WRITE_INSERT || op == WRITE_INSERT_NEW;
  const bool erasing = op == WRITE_DELETE || op == WRITE_MOVE;
  for (;;) {
    int waited = 0;
    // Another transaction's explicit lock that a write conflicts
    // with: wait for it, without the latch.
    // No one else reaches a temporary store, so it has none.
    if (!st.owner && lock_write_check(s, storeid, key) != 0) {
      latch.unlock();
      waited = lock_wait(s, lock_row_id(storeid, key), sm1::SM_LOCKMODE_EX);
    } else if (op == WRITE_INSERT_NEW) {
      // A key no version of was ever written has none to check
      // against, committed or in flight: no read. A debug build
      // reads, to catch a caller wrong about it.
#ifndef NDEBUG
      uint64_t inflight;
      if (0 != latest_version(s, st, key, &live, &commit_ts, nullptr, &own_seq,
                              &inflight, nullptr)) {
        return -1;
      }
      assert(!live && commit_ts == 0 && inflight == 0);
#endif
      live = false;
      commit_ts = 0;
      own_seq = 0;
      break;
    } else {
      uint64_t inflight;
      // Find the version this write checks against, keeping it in
      // pin when the caller asked for the old image.
      if (0 != latest_version(s, st, key, &live, &commit_ts, nullptr, &own_seq,
                              &inflight, pin, &moved)) {
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
  // see that change, so it must not change the row again (../XCTMGR.md §5).
  if (!inserting && s->stmt_marked && own_seq > s->stmt_seq) {
    return set_error(sm1::SM_ERR_SELFMODIFIED,
                     "the row was already changed by this statement");
  }
  // Something committed after we began (../ROWSTORE.md §4.4).
  if (commit_ts > s->snapshot_ts) {
    // Read committed: an insert goes ahead against the newest
    // version; an update or delete must have read it first.
    if (s->level == sm1::xctlevel_t::read_committed) {
      if (!inserting && !lock_has_latest(s, storeid, key)) {
        // Keep the key locked until the re-read and the repeated
        // write, so its newest version cannot change in between
        // (../ROWSTORE.md §4.6, ../LOCKMGR.md §1.9). No other holder conflicts:
        // the check above found none, under the latch.
        lock_try(s, lock_row_id(storeid, key), sm1::SM_LOCKMODE_EX);
        return set_error(sm1::SM_ERR_UPDATED,
                         "key was changed by a transaction that committed "
                         "after this one began; sm1_read_latest it first");
      }
    } else {
      // First-committer-wins, aborting the loser. The latch goes
      // first: the abort drops stores, and the latch is held only
      // to check a key and put a version (../LOCKMGR.md §3.5).
      latch.unlock();
      return xct_fail_retryable(
          s, sm1::SM_ERR_CONFLICT,
          "key was changed by a transaction that committed after "
          "this one began");
    }
  }
  // An insert needs the key absent.
  if (inserting && live) {
    return set_error(sm1::SM_ERR_DUPKEY, "key already exists");
  }
  // An update or delete needs it present: a row that moved lives
  // on under another key (../ROWSTORE.md §2.5).
  if (!inserting && !live) {
    return moved ? set_error(sm1::SM_ERR_MOVED, "the row moved to another key")
                 : set_error(sm1::SM_ERR_NOTFOUND, "key does not exist");
  }
  // Refuse a transaction's 2^32nd write.
  if (s->seq == UINT32_MAX) {
    return set_error(sm1::SM_ERR_INVALID, "too many writes in one transaction");
  }
  // Allocate the xid at the first write (../ROWSTORE.md §2.8).
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  // This write's seq, one past the last.
  uint32_t seq = s->seq + 1;
  // No iterator opened before now serves the session's reads.
  s->write_gen++;
  // A delete writes a tombstone version, a move's marked so.
  uint8_t flags = !erasing           ? 0
                  : op == WRITE_MOVE ? version_tombstone | version_moved
                                     : version_tombstone;
  // Allocate the write timestamp now that no other writer of the
  // key is in flight, so it sorts after every committed version
  // of the key (../XCTMGR.md §2.1, ../LOCKMGR.md §1.7).
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  // The previous writer took its commit_ts before it ended.
  assert(commit_ts < wts);
  // Put the version eagerly; it is not synced (../LOCKMGR.md §5 step 5).
  std::string vkey = version_key(key, wts, seq);
  std::string vval =
      version_value(flags, s->xid, erasing ? rocksdb::Slice() : data);
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
  // Record it for savepoint rollback.
  access_undo_add(s, {storeid, key.ToString(), wts, seq});
  // A temporary store has no other reader, and bars a prepare.
  if (st.owner) {
    s->temp_touched = true;
    return 0;
  }
  // After the put, look for readers the write invalidates; a
  // reader arriving later steps past the version instead (../XCTMGR.md §7.3).
  ssi_on_write(s, storeid, key);
  return 0;
}

/*
 * Record a write for savepoint rollback, if a savepoint could still
 * roll it back; with none set, or every one lost, nothing is kept
 * (../XCTMGR.md §6).
 */
void access_undo_add(sm1::session_t *s, sm1::session_t::undo_t undo) {
  if (s->savepoints.size() <= s->lost_marks) {
    return;
  }
  s->undo_bytes += sizeof(sm1::session_t::undo_t) + undo.key.size();
  s->undo.push_back(std::move(undo));
  // Past the cap: give up the savepoints, not the write.
  if (s->undo_bytes > s->db->undo_max_bytes) {
    xct_undo_overflow(s);
  }
}

/* The visibility test of ../XCTMGR.md §3.3, for chain.cc. */
int access_visible(db_t *db, const version_t &v, const view_t &view,
                   bool *dead) {
  return visible(db, v, view, dead);
}

/* Step past an invisible version, as a read does, for chain.cc. */
int access_skip(sm1::session_t *s, const version_t &v, const view_t &view) {
  return skip_version(s, v, view);
}

/* The entry checks of a data operation, for chain.cc. */
int access_enter(sm1::session_t *s, const view_t &view, uint64_t storeid,
                 const char *key, size_t klen) {
  return enter(s, view, storeid, key, klen);
}

} // namespace wisr

/* Move an open cursor to key, forward only. */
int sm1::scan_seek(sm1::scan_t *scan, std::string_view key, int inclusive) {
  return ::scan_seek(scan, key.data(), key.size(), inclusive);
}

/* Lock a row without writing it; see sm1.h. */
int sm1::lock_row(sm1::session_t *session, uint64_t storeid,
                  std::string_view key, sm1::lockmode_t mode, bool as_read,
                  uint16_t kind) {
  return lock_versioned(session, storeid, key.data(), key.size(), mode, false,
                        as_read, kind);
}

/* Lock a row if it can be had at once; see sm1.h. */
int sm1::trylock_row(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, sm1::lockmode_t mode, bool as_read,
                     uint16_t kind) {
  return lock_versioned(session, storeid, key.data(), key.size(), mode, true,
                        as_read, kind);
}

/* Lock an object id; see sm1.h. */
int sm1::lock_oid(sm1::session_t *session, uint64_t oid, sm1::lockmode_t mode) {
  return lock_object(session, oid, mode, false);
}

/* Lock an object id if it can be had at once; see sm1.h. */
int sm1::trylock_oid(sm1::session_t *session, uint64_t oid,
                     sm1::lockmode_t mode) {
  return lock_object(session, oid, mode, true);
}

/* Insert a row; the key must not exist. */
int sm1::insert(sm1::session_t *session, uint64_t storeid, std::string_view key,
                std::string_view data) {
  return write_row(session, WRITE_INSERT, storeid, key.data(), key.size(),
                   data.data(), data.size());
}

/* Insert a row under a key never written before; see sm1.h. */
int sm1::insert_new(sm1::session_t *session, uint64_t storeid,
                    std::string_view key, std::string_view data) {
  return write_row(session, WRITE_INSERT_NEW, storeid, key.data(), key.size(),
                   data.data(), data.size());
}

/* Replace a row's value; the key must exist. */
int sm1::update(sm1::session_t *session, uint64_t storeid, std::string_view key,
                std::string_view data, std::string_view *old) {
  if (!old) {
    return write_row(session, WRITE_UPDATE, storeid, key.data(), key.size(),
                     data.data(), data.size());
  }
  // A null session fails in write_row; it has no pin of its own.
  if (!session) {
    *old = {};
    return xct_check_active(session);
  }
  // Into the session's own pin, which sm1::read shares.
  return sm1::update(session, storeid, key, data, &session->readpin, *old);
}

/* Replace a row's value, pinning the row replaced into pin. */
int sm1::update(sm1::session_t *session, uint64_t storeid, std::string_view key,
                std::string_view data, sm1::rawpin_t *pin,
                std::string_view &old) {
  const char *o = nullptr;
  size_t olen = 0;
  int rc = write_row(session, WRITE_UPDATE, storeid, key.data(), key.size(),
                     data.data(), data.size(), pin, &o, &olen);
  // Hand it back: the old row, or empty on failure.
  old = std::string_view(o, olen);
  return rc;
}

/* Delete a row; the key must exist. */
int sm1::erase(sm1::session_t *session, uint64_t storeid, std::string_view key,
               std::string_view *old) {
  if (!old) {
    return write_row(session, WRITE_DELETE, storeid, key.data(), key.size(),
                     nullptr, 0);
  }
  // A null session fails in write_row; it has no pin of its own.
  if (!session) {
    *old = {};
    return xct_check_active(session);
  }
  // Into the session's own pin, which sm1::read shares.
  return sm1::erase(session, storeid, key, &session->readpin, *old);
}

/* Delete a row, pinning the row deleted into pin. */
int sm1::erase(sm1::session_t *session, uint64_t storeid, std::string_view key,
               sm1::rawpin_t *pin, std::string_view &old) {
  const char *o = nullptr;
  size_t olen = 0;
  int rc = write_row(session, WRITE_DELETE, storeid, key.data(), key.size(),
                     nullptr, 0, pin, &o, &olen);
  // Hand it back: the old row, or empty on failure.
  old = std::string_view(o, olen);
  return rc;
}

/* Delete a row that moved, pinning it into pin; see sm1.h. */
int sm1::erase_moved(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, sm1::rawpin_t *pin,
                     std::string_view &old) {
  const char *o = nullptr;
  size_t olen = 0;
  int rc = write_row(session, WRITE_MOVE, storeid, key.data(), key.size(),
                     nullptr, 0, pin, &o, &olen);
  // Hand it back: the old row, or empty on failure.
  old = std::string_view(o, olen);
  return rc;
}

/* Read one key into pin. */
int sm1::read(sm1::session_t *session, uint64_t storeid, std::string_view key,
              sm1::rawpin_t *pin, std::string_view &data) {
  const char *d = nullptr;
  size_t dlen = 0;
  int rc = read_row(session, storeid, key.data(), key.size(), pin, &d, &dlen);
  // Hand back the value: empty for none (1) and on failure.
  data = std::string_view(d, dlen);
  return rc;
}

/* Read one key, lent until the session's next call. */
int sm1::read(sm1::session_t *session, uint64_t storeid, std::string_view key,
              std::string_view &data) {
  // A NULL session has no pin: fail as any call on it does.
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  // Into the session's own pin, which the next such read reuses.
  return sm1::read(session, storeid, key, &session->readpin, data);
}

/* A versioned key's newest version, if settled; see sm1::read_settled. */
static int versioned_settled(sm1::session_t *s, const store_t &st,
                             std::string_view key, sm1::rawpin_t *pin,
                             std::string_view &data) {
  bool live;
  uint64_t commit_ts, inflight;
  uint32_t own_seq;
  sm0::scope_guard_t put([&] { sm1::unpin(pin); });
  if (0 != latest_version(s, st, rocksdb::Slice(key.data(), key.size()), &live,
                          &commit_ts, nullptr, &own_seq, &inflight, pin)) {
    return -1;
  }
  // Another's write in flight, this transaction's own, or a commit
  // some snapshot may not see yet: not settled. A key with no
  // version at all has commit_ts 0, and is settled gone.
  if (0 != inflight || 0 != own_seq || commit_ts > db_watermark(s->db)) {
    return 2;
  }
  if (!live) {
    return 1;
  }
  put.dismiss();
  data = std::string_view(pin->row.data(), pin->row.size());
  return 0;
}

/* The store of a settled read of key, nullptr with the error set. */
static const store_t *enter_settled(sm1::session_t *s, uint64_t storeid,
                                    std::string_view key) {
  if (xct_check_btree_reaper(s) != 0 ||
      enter(s, xct_view(s), storeid, key.data(), key.size()) != 0) {
    return nullptr;
  }
  const store_t &st = store_of(s, storeid);
  return st.cf ? &st : nullptr;
}

/* Read a key's newest version, if settled; see sm1.h. */
int sm1::read_settled(sm1::session_t *s, uint64_t storeid, std::string_view key,
                      sm1::rawpin_t *pin, std::string_view &data) {
  data = std::string_view();
  sm1::unpin(pin);
  const store_t *st = enter_settled(s, storeid, key);
  if (nullptr == st) {
    return -1;
  }
  if (st->chained) {
    return chain_read_settled(s, *st, rocksdb::Slice(key.data(), key.size()),
                              pin, data);
  }
  return versioned_settled(s, *st, key, pin, data);
}

/* Read a versioned key's newest version, if settled; see sm1.h. */
int sm1::read_settled_versioned(sm1::session_t *s, uint64_t storeid,
                                std::string_view key, sm1::rawpin_t *pin,
                                std::string_view &data) {
  data = std::string_view();
  sm1::unpin(pin);
  const store_t *st = enter_settled(s, storeid, key);
  return nullptr == st ? -1 : versioned_settled(s, *st, key, pin, data);
}

/* Let a session make settled reads and page calls alone; see sm1.h. */
void sm1::set_no_transaction(sm1::session_t *s) { s->no_transaction = true; }

/* The GC watermark; see sm1.h. */
uint64_t sm1::gc_watermark(sm1::session_t *s) { return db_watermark(s->db); }

/* Read a key's newest version, past the snapshot; see sm1.h. */
int sm1::read_newest(sm1::session_t *s, uint64_t storeid, std::string_view key,
                     sm1::rawpin_t *pin, std::string_view &data,
                     uint64_t &writer) {
  data = std::string_view();
  writer = 0;
  sm1::unpin(pin);
  if (xct_check_active(s) != 0 ||
      enter(s, xct_view(s), storeid, key.data(), key.size()) != 0) {
    return -1;
  }
  const store_t &st = store_of(s, storeid);
  if (!st.cf) {
    return -1;
  }
  rocksdb::Slice k(key.data(), key.size());
  if (st.chained) {
    return chain_read_newest(s, st, k, pin, data, writer);
  }
  bool live;
  uint64_t commit_ts, inflight;
  uint32_t own_seq;
  sm0::scope_guard_t put([&] { sm1::unpin(pin); });
  if (0 != latest_version(s, st, k, &live, &commit_ts, nullptr, &own_seq,
                          &inflight, pin)) {
    return -1;
  }
  // Another's write in flight: its writer is to be waited for.
  if (0 != inflight) {
    writer = inflight;
    return 2;
  }
  if (!live) {
    return 1;
  }
  put.dismiss();
  data = std::string_view(pin->row.data(), pin->row.size());
  return 0;
}

/* Wait for a writer read_newest named; see sm1.h. */
int sm1::wait_writer(sm1::session_t *s, uint64_t writer) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  int waited = lock_wait_xact(s, writer);
  return waited != 0 ? lock_wait_error(s, waited) : 0;
}

/* Read the newest committed version of a key into pin. */
int sm1::read_latest(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, sm1::rawpin_t *pin,
                     std::string_view &data) {
  const char *d = nullptr;
  size_t dlen = 0;
  int rc =
      read_latest_row(session, storeid, key.data(), key.size(), pin, &d, &dlen);
  // Hand back the value: empty for none (1) and on failure.
  data = std::string_view(d, dlen);
  return rc;
}

/* Read the newest committed version of a key, lent as sm1::read's. */
int sm1::read_latest(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, std::string_view &data) {
  // A NULL session has no pin: fail as any call on it does.
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  // Into the session's own pin, which sm1::read shares.
  return sm1::read_latest(session, storeid, key, &session->readpin, data);
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
int sm1::scannxt(sm1::scan_t *c, std::string_view &key,
                 std::string_view &data) {
  // Require a cursor.
  if (!c) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_scannxt: NULL argument");
  }
  // Report no row until one is found: both are empty at the end
  // (1) and on failure.
  key = data = {};
  // A cursor a rollback closed reads no more.
  if (c->rolled_back) {
    return set_error(sm1::SM_ERR_SCANCLOSED, rolled_back_msg);
  }
  // Nor does one of a transaction a rollback doomed (../XCTMGR.md §6).
  if (c->session->active && c->session->doomed == sm1::SM_ERR_DOOMED) {
    return xct_check_active(c->session);
  }
  // A cursor at its end, or failed, stays so.
  if (c->state != 0) {
    return c->state;
  }
  // A chained store's cursor walks its current keys and their chains.
  if (c->chained) {
    return chain_scan_next(c, key, data);
  }
  return scan_next(c, key, data);
}

/* Release a cursor's hold: the iterator first, then its snapshot. */
void wisr::scan_release(sm1::scan_t *scan) {
  ssi_range_close(scan); // narrow the SIREAD range to what was read
  // Destroy the iterator, and a chained cursor's row, before the
  // snapshot and bound.
  scan->it.reset();
  scan->got.Reset();
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

/* Whether a rollback to a savepoint closed the cursor. */
bool sm1::scan_rolled_back(const sm1::scan_t *scan) {
  return scan->rolled_back;
}
