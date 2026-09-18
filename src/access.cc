/*
 * access.cc -- sm_insert, sm_update, sm_delete, sm_read, and
 * the sm_scan cursor (DESIGN.md §4.4, §5, §10).
 *
 * Not yet: SIREAD tracking (build order step 6).
 */

#include "error.h"
#include "session.h"
#include "version.h"

#include <algorithm>
#include <new>

#include <rocksdb/options.h>

using namespace wisr;

namespace
{

/*
 * Holds a read epoch for one call: from before its iterator is
 * opened until it is done resolving what it read, so the CF:txn
 * sweep waits for it (§8.3).
 */
class read_epoch_guard_t
{
  public:
    /* Enter the current read epoch. */
    explicit read_epoch_guard_t(db_t* db) : m_db(db), m_epoch(db_read_enter(db))
    {
    }

    /* Leave it. */
    ~read_epoch_guard_t()
    {
        db_read_leave(m_db, m_epoch);
    }

  private:
    db_t* m_db;
    uint64_t m_epoch;
};

/* 1 if version v is visible at view, 0 if not, -1 on error (§4.4). */
int visible(db_t* db, const version_t& v, const view_t& view)
{
    if (view.xid != 0 && v.xid == view.xid) {
        return v.seq <= view.seq_hwm; // own write, savepoint-aware
    }
    if (v.xid > view.snapshot_ts) {
        return 0; // commit_ts > xid > snapshot
    }
    uint64_t commit_ts;
    if (v.flags & version_stamped) {
        commit_ts = v.commit_ts;
    } else {
        int r = xct_resolve(db, v.xid, &commit_ts);
        if (r <= 0) {
            return r;
        }
    }
    return commit_ts <= view.snapshot_ts;
}

/* The CF of relid; its absence after a catalog hit is corruption. */
rocksdb::ColumnFamilyHandle* rel_cf(db_t* db, uint64_t relid)
{
    rocksdb::ColumnFamilyHandle* cf = db_rel_cf(db, relid);
    if (!cf) {
        set_error(SM_ERR_CORRUPT, "relation %llu has no column family",
                  (unsigned long long)relid);
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
int latest_version(sm_session_t* s, rocksdb::ColumnFamilyHandle* cf,
                   rocksdb::Slice key, bool* live, uint64_t* commit_ts)
{
    *live = false;
    *commit_ts = 0;
    read_epoch_guard_t epoch(s->db);
    rocksdb::ReadOptions ro;
    ro.prefix_same_as_start = true;
    std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
    for (it->Seek(version_bound(key, '\x00')); it->Valid(); it->Next()) {
        version_t v;
        version_decode_key(it->key(), &v);
        if (v.userkey != key) {
            break;
        }
        if (!version_decode_value(it->value(), &v)) {
            return set_error(SM_ERR_CORRUPT, "malformed version value");
        }
        if (s->xid != 0 && v.xid == s->xid) {
            if (v.seq <= s->seq_hwm) {
                *live = !(v.flags & version_tombstone);
                return 0;
            }
            continue;
        }
        int committed = 1;
        uint64_t ts = v.commit_ts;
        if (!(v.flags & version_stamped)) {
            committed = xct_resolve(s->db, v.xid, &ts);
        }
        if (committed < 0) {
            return -1;
        }
        if (committed) {
            *live = !(v.flags & version_tombstone);
            *commit_ts = ts;
            return 0;
        }
    }
    if (!it->status().ok()) {
        return set_status_error(it->status(), "read versions");
    }
    return 0;
}

/* Check a caller's key: non-NULL unless empty, within SM_MAX_KEYLEN. */
int check_key(const char* key, size_t klen)
{
    if (!key && klen > 0) {
        return set_error(SM_ERR_INVALID, "key is NULL");
    }
    if (klen > SM_MAX_KEYLEN) {
        return set_error(SM_ERR_INVALID, "key of %zu bytes exceeds %d", klen,
                         SM_MAX_KEYLEN);
    }
    return 0;
}

/*
 * Common entry checks for a data operation: a valid relation
 * name and key. Resolves relname at view into *relid.
 */
int enter(sm_session_t* s, const view_t& view, const char* relname,
          const char* key, size_t klen, uint64_t* relid)
{
    if (check_name(relname, SM_MAX_RELNAME, "relation") != 0 ||
        check_key(key, klen) != 0) {
        return -1;
    }
    int r = catalog_lookup(s, view, relname, relid);
    if (r == 1) {
        return set_error(SM_ERR_NOTFOUND, "relation %s does not exist",
                         relname);
    }
    return r;
}

/* sm_insert / sm_update / sm_delete, minus the out-of-memory guard. */
int write_row(sm_session_t* s, write_op_t op, const char* relname,
              const char* key, size_t klen, const char* data, size_t datalen)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    if (!data && datalen > 0) {
        return set_error(SM_ERR_INVALID, "data is NULL");
    }
    if (datalen > SM_MAX_DATALEN) {
        return set_error(SM_ERR_INVALID, "value of %zu bytes exceeds %d",
                         datalen, SM_MAX_DATALEN);
    }
    uint64_t relid;
    if (enter(s, xct_view(s), relname, key, klen, &relid) != 0) {
        return -1;
    }
    return access_write(s, relid, op, rocksdb::Slice(key ? key : "", klen),
                        rocksdb::Slice(data ? data : "", datalen));
}

/* sm_read, minus the out-of-memory guard. */
int read_row(sm_session_t* s, const char* relname, const char* key, size_t klen,
             const char** data, size_t* datalen)
{
    if (!data || !datalen) {
        return set_error(SM_ERR_INVALID, "sm_read: data or datalen is NULL");
    }
    *data = nullptr;
    *datalen = 0;
    if (xct_check_active(s) != 0) {
        return -1;
    }
    view_t view = xct_view(s);
    uint64_t relid;
    if (enter(s, view, relname, key, klen, &relid) != 0) {
        return -1;
    }
    int r = access_read(s, relid, view, rocksdb::Slice(key ? key : "", klen),
                        &s->readbuf);
    if (r == 0) {
        *data = s->readbuf.data();
        *datalen = s->readbuf.size();
    }
    return r;
}

/* sm_scan, minus the out-of-memory guard. */
sm_scan_t* scan(sm_session_t* s, const char* relname, const char* skey,
                size_t sklen, int skey_inclusive, const char* ekey,
                size_t eklen, int ekey_inclusive)
{
    if (xct_check_active(s) != 0) {
        return nullptr;
    }
    if ((skey && check_key(skey, sklen) != 0) ||
        (ekey && check_key(ekey, eklen) != 0)) {
        return nullptr;
    }
    view_t view = xct_view(s);
    uint64_t relid;
    if (enter(s, view, relname, "", 0, &relid) != 0) {
        return nullptr;
    }
    rocksdb::ColumnFamilyHandle* cf = rel_cf(s->db, relid);
    if (!cf) {
        return nullptr;
    }

    std::unique_ptr<sm_scan_t> c(new sm_scan_t);
    c->session = s;
    c->id = ++s->next_scan_id;
    c->snapshot_ts = view.snapshot_ts;
    c->xid = view.xid;
    c->seq_hwm = view.seq_hwm;
    // Count the cursor as a reader for its whole life (§8.3),
    // then take the RocksDB snapshot: its physical view (§10.2).
    c->epoch = db_read_enter(s->db);
    c->snap = s->db->rdb->GetSnapshot();

    rocksdb::ReadOptions ro;
    ro.snapshot = c->snap;
    ro.auto_prefix_mode = true; // a scan crosses userkeys (§10.2)
    if (ekey) {
        c->upper = version_bound(rocksdb::Slice(ekey, eklen),
                                 ekey_inclusive ? '\xff' : '\x00');
        c->upper_slice = rocksdb::Slice(c->upper);
        ro.iterate_upper_bound = &c->upper_slice;
    }
    c->it.reset(s->db->rdb->NewIterator(ro, cf));
    if (skey) {
        c->it->Seek(version_bound(rocksdb::Slice(skey, sklen),
                                  skey_inclusive ? '\x00' : '\xff'));
    } else {
        c->it->SeekToFirst();
    }
    s->scans.insert(c.get());
    return c.release();
}

/* sm_scan_next, minus the out-of-memory guard. */
int scan_next(sm_scan_t* c, const char** key, size_t* klen, const char** data,
              size_t* datalen)
{
    if (!c || !key || !klen || !data || !datalen) {
        return set_error(SM_ERR_INVALID, "sm_scan_next: NULL argument");
    }
    *key = *data = nullptr;
    *klen = *datalen = 0;
    if (c->state != 0) {
        return c->state;
    }
    db_t* db = c->session->db;
    view_t view = {c->snapshot_ts, c->xid, c->seq_hwm};
    rocksdb::Iterator* it = c->it.get();

    while (it->Valid()) {
        version_t v;
        version_decode_key(it->key(), &v);
        std::string uk = v.userkey.ToString();
        bool found = false;
        bool live = false;
        // Walk the run of uk's versions, newest first: the first
        // visible one decides, the rest are stepped past.
        for (; it->Valid(); it->Next()) {
            version_decode_key(it->key(), &v);
            if (v.userkey != rocksdb::Slice(uk)) {
                break;
            }
            if (found) {
                continue;
            }
            if (!version_decode_value(it->value(), &v)) {
                c->state = -1;
                return set_error(SM_ERR_CORRUPT, "malformed version value");
            }
            int r = visible(db, v, view);
            if (r < 0) {
                c->state = -1;
                return -1;
            }
            if (r == 1) {
                found = true;
                live = !(v.flags & version_tombstone);
                if (live) {
                    c->data.assign(v.payload.data(), v.payload.size());
                }
            }
        }
        if (live) {
            c->key = std::move(uk);
            *key = c->key.data();
            *klen = c->key.size();
            *data = c->data.data();
            *datalen = c->data.size();
            return 0;
        }
    }
    if (!it->status().ok()) {
        c->state = -1;
        return set_status_error(it->status(), "scan");
    }
    c->state = 1;
    return 1;
}

} // namespace

namespace wisr
{

/* Seek to the newest version a reader at view could see (§10.1). */
int access_read(sm_session_t* s, uint64_t relid, const view_t& view,
                rocksdb::Slice key, std::string* out)
{
    rocksdb::ColumnFamilyHandle* cf = rel_cf(s->db, relid);
    if (!cf) {
        return -1;
    }
    read_epoch_guard_t epoch(s->db);
    rocksdb::ReadOptions ro;
    ro.prefix_same_as_start = true;
    std::unique_ptr<rocksdb::Iterator> it(s->db->rdb->NewIterator(ro, cf));
    // ~seq of 0 lands before every own version of the key.
    uint64_t hi = std::max(view.xid, view.snapshot_ts);
    for (it->Seek(version_key(key, hi, UINT32_MAX)); it->Valid(); it->Next()) {
        version_t v;
        version_decode_key(it->key(), &v);
        if (v.userkey != key) {
            break;
        }
        if (!version_decode_value(it->value(), &v)) {
            return set_error(SM_ERR_CORRUPT, "malformed version value");
        }
        int r = visible(s->db, v, view);
        if (r < 0) {
            return -1;
        }
        if (r == 1) {
            if (v.flags & version_tombstone) {
                return 1;
            }
            out->assign(v.payload.data(), v.payload.size());
            return 0;
        }
    }
    if (!it->status().ok()) {
        return set_status_error(it->status(), "read");
    }
    return 1;
}

/* Check existence, then put one new version (§5). */
int access_write(sm_session_t* s, uint64_t relid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data)
{
    rocksdb::ColumnFamilyHandle* cf = rel_cf(s->db, relid);
    if (!cf) {
        return -1;
    }
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
    if (op == WRITE_INSERT && live) {
        return set_error(SM_ERR_DUPKEY, "key already exists");
    }
    if (op != WRITE_INSERT && !live) {
        return set_error(SM_ERR_NOTFOUND, "key does not exist");
    }
    if (s->seq == UINT32_MAX) {
        return set_error(SM_ERR_INVALID, "too many writes in one transaction");
    }
    // Allocate the xid at the first write (§4.1).
    if (s->xid == 0 && xct_alloc_xid(s) != 0) {
        return -1;
    }
    uint32_t seq = s->seq + 1;
    uint8_t flags = op == WRITE_DELETE ? version_tombstone : 0;
    // Put the version eagerly; it is not synced (§5 step 5).
    rocksdb::Status st = s->db->rdb->Put(
        rocksdb::WriteOptions(), cf, version_key(key, s->xid, seq),
        version_value(flags, op == WRITE_DELETE ? rocksdb::Slice() : data));
    if (!st.ok()) {
        return set_status_error(st, "write version");
    }
    s->seq = seq;
    s->seq_hwm = seq;
    s->undo.push_back({relid, key.ToString(), seq});
    return 0;
}

} // namespace wisr

/* Insert a row; the key must not exist. */
int sm_insert(sm_session_t* session, const char* relname, const char* key,
              size_t klen, const char* data, size_t datalen)
{
    try {
        return write_row(session, WRITE_INSERT, relname, key, klen, data,
                         datalen);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_insert: out of memory");
    }
}

/* Replace a row's value; the key must exist. */
int sm_update(sm_session_t* session, const char* relname, const char* key,
              size_t klen, const char* data, size_t datalen)
{
    try {
        return write_row(session, WRITE_UPDATE, relname, key, klen, data,
                         datalen);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_update: out of memory");
    }
}

/* Delete a row; the key must exist. */
int sm_delete(sm_session_t* session, const char* relname, const char* key,
              size_t klen)
{
    try {
        return write_row(session, WRITE_DELETE, relname, key, klen, nullptr, 0);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_delete: out of memory");
    }
}

/* Read one key. */
int sm_read(sm_session_t* session, const char* relname, const char* key,
            size_t klen, const char** data, size_t* datalen)
{
    try {
        return read_row(session, relname, key, klen, data, datalen);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_read: out of memory");
    }
}

/* Open a cursor over a key range. */
sm_scan_t* sm_scan(sm_session_t* session, const char* relname, const char* skey,
                   size_t sklen, int skey_inclusive, const char* ekey,
                   size_t eklen, int ekey_inclusive)
{
    try {
        return scan(session, relname, skey, sklen, skey_inclusive, ekey, eklen,
                    ekey_inclusive);
    } catch (const std::bad_alloc&) {
        set_error(SM_ERR_NOMEM, "sm_scan: out of memory");
        return nullptr;
    }
}

/* Advance a cursor to its next row. */
int sm_scan_next(sm_scan_t* scan, const char** key, size_t* klen,
                 const char** data, size_t* datalen)
{
    try {
        return scan_next(scan, key, klen, data, datalen);
    } catch (const std::bad_alloc&) {
        scan->state = -1;
        return set_error(SM_ERR_NOMEM, "sm_scan_next: out of memory");
    }
}

/* Close a cursor: the iterator first, then its snapshot. */
void sm_scan_end(sm_scan_t* scan)
{
    if (!scan) {
        return;
    }
    db_t* db = scan->session->db;
    scan->session->scans.erase(scan);
    scan->it.reset();
    db->rdb->ReleaseSnapshot(scan->snap);
    db_read_leave(db, scan->epoch);
    delete scan;
}
