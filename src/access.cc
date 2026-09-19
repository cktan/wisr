/*
 * access.cc -- sm_insert, sm_update, sm_delete, sm_read, and
 * the sm_scan cursor (DESIGN.md §4.4, §5, §10).
 *
 * Under SERIALIZABLE, reads leave SIREAD entries and report the
 * concurrent versions they step past, and writes report the
 * readers they invalidate (ssi.cc, §7).
 */

#include "error.h"
#include "session.h"
#include "version.h"

#include <algorithm>
#include <new>

#include <rocksdb/options.h>

using namespace wisr;

namespace {

/*
 * Holds a read epoch for one call: from before its iterator is
 * opened until it is done resolving what it read, so the CF:txn
 * sweep waits for it (§8.3).
 */
class read_epoch_guard_t {
public:
  /* Enter the current read epoch. */
  explicit read_epoch_guard_t(db_t *db)
      : m_db(db), m_epoch(db_read_enter(db)) {}

  /* Leave it. */
  ~read_epoch_guard_t() { db_read_leave(m_db, m_epoch); }

private:
  db_t *m_db;
  uint64_t m_epoch;
};

/* 1 if version v is visible at view, 0 if not, -1 on error (§4.4). */
int visible(db_t *db, const version_t &v, const view_t &view) {
  // Our own write: visible unless a rollback undid it.
  if (view.xid != 0 && v.xid == view.xid) {
    return v.seq <= view.seq_hwm; // own write, savepoint-aware
  }
  // Written after our snapshot, so committed after it too.
  if (v.xid > view.snapshot_ts) {
    return 0; // commit_ts > xid > snapshot
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

/* The CF of relid; its absence after a catalog hit is corruption. */
rocksdb::ColumnFamilyHandle *rel_cf(db_t *db, uint64_t relid) {
  // Look up the relation's CF.
  rocksdb::ColumnFamilyHandle *cf = db_rel_cf(db, relid);
  // The catalog named it, so a missing CF is corruption.
  if (!cf) {
    set_error(SM_ERR_CORRUPT, "relation {} has no column family", relid);
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
                   rocksdb::Slice key, bool *live, uint64_t *commit_ts) {
  // Assume no version until one is found.
  *live = false;
  *commit_ts = 0;
  // Keep the CF:txn sweep waiting while we resolve versions.
  read_epoch_guard_t epoch(s->db);
  // Stay within this userkey's versions.
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  // Open an iterator on the relation.
  std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
  // Walk every version of the key, newest first.
  for (it->Seek(version_bound(key, '\x00')); it->Valid(); it->Next()) {
    version_t v;
    // Decode the version's userkey, xid, and seq.
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
 * Common entry checks for a data operation: a valid relation
 * name and key. Resolves relname at view into *relid.
 */
int enter(sm_session_t *s, const view_t &view, const char *relname,
          const char *key, size_t klen, uint64_t *relid) {
  // Validate the relation name and the key.
  if (check_name(relname, SM_MAX_RELNAME, "relation") != 0 ||
      check_key(key, klen) != 0) {
    return -1;
  }
  // Resolve the relation at the view.
  int r = catalog_lookup(s, view, relname, relid);
  // Report an unknown relation.
  if (r == 1) {
    return set_error(SM_ERR_NOTFOUND, "relation {} does not exist", relname);
  }
  return r;
}

/* sm_insert / sm_update / sm_delete, minus the out-of-memory guard. */
int write_row(sm_session_t *s, write_op_t op, const char *relname,
              const char *key, size_t klen, const char *data, size_t datalen) {
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
  uint64_t relid;
  // Validate and resolve at the current view.
  if (enter(s, xct_view(s), relname, key, klen, &relid) != 0) {
    return -1;
  }
  // Write the version.
  return access_write(s, relid, op, rocksdb::Slice(key ? key : "", klen),
                      rocksdb::Slice(data ? data : "", datalen));
}

/* sm_read, minus the out-of-memory guard. */
int read_row(sm_session_t *s, const char *relname, const char *key, size_t klen,
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
  view_t view = xct_view(s);
  uint64_t relid;
  // Validate and resolve the relation.
  if (enter(s, view, relname, key, klen, &relid) != 0) {
    return -1;
  }
  // Read the key into the session's buffer.
  int r = access_read(s, relid, view, rocksdb::Slice(key ? key : "", klen),
                      &s->readbuf);
  // Found: hand out the session-owned bytes.
  if (r == 0) {
    *data = s->readbuf.data();
    *datalen = s->readbuf.size();
  }
  return r;
}

/* sm_scan, minus the out-of-memory guard. */
sm_scan_t *scan(sm_session_t *s, const char *relname, const char *skey,
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
  view_t view = xct_view(s);
  uint64_t relid;
  // Validate and resolve the relation.
  if (enter(s, view, relname, "", 0, &relid) != 0) {
    return nullptr;
  }
  // Find the relation's CF.
  rocksdb::ColumnFamilyHandle *cf = rel_cf(s->db, relid);
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
  ssi_range_open(c.get(), relid, skey, sklen, skey_inclusive, ekey, eklen,
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
    // Seek to skey, past it if exclusive.
    c->it->Seek(version_bound(rocksdb::Slice(skey, sklen),
                              skey_inclusive ? '\x00' : '\xff'));
  } else {
    // No lower bound: start at the first key.
    c->it->SeekToFirst();
  }
  // Register the cursor with the session.
  s->scans.insert(c.get());
  return c.release();
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
      // Decode this version's userkey, xid, and seq.
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
int access_read(sm_session_t *s, uint64_t relid, const view_t &view,
                rocksdb::Slice key, std::string *out) {
  // Find the relation's CF.
  rocksdb::ColumnFamilyHandle *cf = rel_cf(s->db, relid);
  if (!cf) {
    return -1;
  }
  // Under SERIALIZABLE, take the SIREAD entry before the
  // iterator exists, so a write landing in between still finds
  // it (§10.1).
  ssi_on_read(s, relid, key);
  // Keep the CF:txn sweep waiting while we resolve versions.
  read_epoch_guard_t epoch(s->db);
  // Stay within this userkey's versions.
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  // Open an iterator on the relation.
  std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
  // Seek to the newest version the reader could see; ~seq of 0
  // lands before every own version of the key. Under SSI, start
  // before every version instead: the newer ones are exactly
  // the concurrent writers owed an edge.
  uint64_t hi = std::max(view.xid, view.snapshot_ts);
  std::string target =
      s->ssi ? version_bound(key, '\x00') : version_key(key, hi, UINT32_MAX);
  // Walk the key's versions, newest first.
  for (it->Seek(target); it->Valid(); it->Next()) {
    version_t v;
    // Decode the version's userkey, xid, and seq.
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
int access_write(sm_session_t *s, uint64_t relid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data) {
  // Find the relation's CF.
  rocksdb::ColumnFamilyHandle *cf = rel_cf(s->db, relid);
  if (!cf) {
    return -1;
  }
  // Refuse writes after fail-stop.
  if (xct_check_writable(s->db) != 0) {
    return -1;
  }
  // Take the row lock; this blocks while another transaction
  // holds it, and fails if we are chosen as a deadlock victim.
  if (lock_acquire(s, relid, key) != 0) {
    return xct_fail_retryable(s, SM_ERR_DEADLOCK,
                              "chosen as a deadlock victim");
  }
  bool live;
  uint64_t commit_ts;
  // Find the version this write checks against.
  if (latest_version(s, cf, key, &live, &commit_ts) != 0) {
    return -1;
  }
  // First-committer-wins (§5.2), at every level: the check is
  // against the begin snapshot, even under read committed.
  if (commit_ts > s->snapshot_ts) {
    return xct_fail_retryable(
        s, SM_ERR_CONFLICT,
        "key was changed by a transaction that committed after "
        "this one began");
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
  // Put the version eagerly; it is not synced (§5 step 5).
  rocksdb::Status st = s->db->rdb->Put(
      rocksdb::WriteOptions(), cf, version_key(key, s->xid, seq),
      version_value(flags, op == WRITE_DELETE ? rocksdb::Slice() : data));
  // Fail if the put failed.
  if (!st.ok()) {
    return set_status_error(st, "write version");
  }
  // Advance seq, and make the write visible to ourselves.
  s->seq = seq;
  s->seq_hwm = seq;
  // Record it for savepoint rollback.
  s->undo.push_back({relid, key.ToString(), seq});
  // After the put, look for readers the write invalidates; a
  // reader arriving later steps past the version instead (§7.2).
  ssi_on_write(s, relid, key);
  return 0;
}

} // namespace wisr

/* Insert a row; the key must not exist. */
int sm_insert(sm_session_t *session, const char *relname, const char *key,
              size_t klen, const char *data, size_t datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_INSERT, relname, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_insert: out of memory");
  }
}

/* Replace a row's value; the key must exist. */
int sm_update(sm_session_t *session, const char *relname, const char *key,
              size_t klen, const char *data, size_t datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_UPDATE, relname, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_update: out of memory");
  }
}

/* Delete a row; the key must exist. */
int sm_delete(sm_session_t *session, const char *relname, const char *key,
              size_t klen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return write_row(session, WRITE_DELETE, relname, key, klen, nullptr, 0);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_delete: out of memory");
  }
}

/* Read one key. */
int sm_read(sm_session_t *session, const char *relname, const char *key,
            size_t klen, const char **data, size_t *datalen) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return read_row(session, relname, key, klen, data, datalen);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_read: out of memory");
  }
}

/* Open a cursor over a key range. */
sm_scan_t *sm_scan(sm_session_t *session, const char *relname, const char *skey,
                   size_t sklen, int skey_inclusive, const char *ekey,
                   size_t eklen, int ekey_inclusive) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return scan(session, relname, skey, sklen, skey_inclusive, ekey, eklen,
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
