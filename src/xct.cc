/*
 * xct.cc -- sm_begin, sm_commit, sm_abort, and the commit
 * status table CF:txn (DESIGN.md §3.2, §3.4, §6, §8.2).
 *
 * A transaction enters the active table when it allocates its
 * xid and leaves it at commit or abort. Commit claims a
 * commit_ts from the sequencer, writes the status record, moves
 * the xid from the active table into the commit cache, and only
 * then publishes (§8.2). Abort writes nothing: absence of a
 * record resolves as dead (§3.4).
 *
 * A retryable error (CONFLICT, DEADLOCK) aborts the transaction
 * on the engine's side before it is reported (§12.1).
 *
 * Not yet: prepare (build order step 7).
 */

#include "codec.h"
#include "error.h"
#include "session.h"
#include "version.h"

#include <memory>
#include <new>
#include <vector>

#include <rocksdb/write_batch.h>

using namespace wisr;

namespace
{

/* CF:txn states (§3.2). */
constexpr uint8_t txn_committed = 2;
constexpr uint8_t txn_aborted = 3;

/* Status record size: state, snapshot_ts, commit_ts. */
constexpr size_t txn_record_size = 1 + 8 + 8;

/* The CF:txn key of xid. */
std::string txn_key(uint64_t xid)
{
    std::string k;
    put_be64(k, xid);
    return k;
}

/* Remove xid from the active table: its versions now resolve dead. */
void leave_active(db_t* db, uint64_t xid)
{
    std::lock_guard<std::mutex> guard(db->txn_mu);
    db->active.erase(xid);
}

/*
 * Move xid from the active table into the commit cache, evicting
 * the oldest commit_ts if the cache is full. Eviction cannot
 * change an answer: the status record was written first (§3.4).
 */
void enter_cache(db_t* db, uint64_t xid, uint64_t commit_ts)
{
    std::lock_guard<std::mutex> guard(db->txn_mu);
    db->active.erase(xid);
    db->cache[xid] = commit_ts;
    db->cache_by_ts[commit_ts] = xid;
    while (db->cache.size() > db->cache_entries) {
        auto oldest = db->cache_by_ts.begin();
        db->cache.erase(oldest->second);
        db->cache_by_ts.erase(oldest);
    }
}

/*
 * Forget the transaction; the session has none afterwards. Its
 * snapshot stops holding back gc_watermark.
 */
void reset(sm_session_t* s)
{
    if (s->snapshot_registered) {
        db_release_snapshot(s->db, s->snapshot_ts);
        s->snapshot_registered = false;
    }
    s->active = false;
    s->xid = 0;
    s->seq = 0;
    s->seq_hwm = 0;
    s->created.clear();
    s->dropped.clear();
    s->undo.clear();
    s->has_savepoint = false;
}

/*
 * Drop the column families this transaction created, newest
 * first (§6.6). A failure leaves an orphan CF with no catalog
 * row, which the startup sweep (§9.1 step 6) is meant to catch.
 */
void drop_created(sm_session_t* s)
{
    for (auto it = s->created.rbegin(); it != s->created.rend(); ++it) {
        db_drop_rel_cf(s->db, *it);
    }
}

/*
 * End the transaction without committing (§6.6): close its
 * cursors, take it out of the active table so its versions
 * resolve dead, drop the relations it created, and release its
 * locks.
 */
void discard(sm_session_t* s)
{
    xct_end_scans(s);
    ssi_end(s); // an aborted node constrains nobody: freed
    if (s->xid != 0) {
        leave_active(s->db, s->xid);
    }
    drop_created(s);
    lock_release_all(s);
    reset(s);
}

/* sm_savepoint, minus the out-of-memory guard. */
int savepoint(sm_session_t* s)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    // Mark the current positions; this replaces any earlier mark.
    s->has_savepoint = true;
    s->sp_undo = s->undo.size();
    s->sp_created = s->created.size();
    s->sp_dropped = s->dropped.size();
    s->sp_scan_id = s->next_scan_id;
    {
        std::lock_guard<std::mutex> guard(s->db->lock_mu);
        s->sp_locks = s->locks.size();
    }
    return 0;
}

/* sm_rollback, minus the out-of-memory guard. */
int rollback(sm_session_t* s)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    if (!s->has_savepoint) {
        return set_error(SM_ERR_NOSAVEPOINT, "no savepoint to roll back to");
    }
    db_t* db = s->db;

    // Delete exactly the versions written since the mark, in one
    // batch: if it fails, nothing changed and the transaction
    // stays as it was.
    rocksdb::WriteBatch batch;
    for (size_t i = s->undo.size(); i-- > s->sp_undo;) {
        const sm_session_t::undo_t& u = s->undo[i];
        rocksdb::ColumnFamilyHandle* cf = db_rel_cf(db, u.relid);
        if (cf) {
            batch.Delete(cf, version_key(u.key, s->xid, u.seq));
        }
    }
    if (batch.Count() > 0) {
        rocksdb::Status st = db->rdb->Write(rocksdb::WriteOptions(), &batch);
        if (!st.ok()) {
            return set_status_error(st, "roll back to savepoint");
        }
    }

    // Close the cursors opened after the mark: their snapshot
    // and frozen seq_hwm would still admit the undone versions.
    std::vector<sm_scan_t*> later;
    for (sm_scan_t* c : s->scans) {
        if (c->id > s->sp_scan_id) {
            later.push_back(c);
        }
    }
    for (sm_scan_t* c : later) {
        sm_scan_end(c);
    }

    // Drop the relations created after the mark, newest first.
    for (size_t i = s->created.size(); i-- > s->sp_created;) {
        db_drop_rel_cf(db, s->created[i]);
    }
    s->created.resize(s->sp_created);
    s->dropped.resize(s->sp_dropped); // the undo restored their rows

    // Rewind seq with seq_hwm, so the own versions still in
    // effect are exactly seqs 1..seq_hwm (§6.5).
    s->undo.resize(s->sp_undo);
    s->seq_hwm = s->undo.empty() ? 0 : s->undo.back().seq;
    s->seq = s->seq_hwm;

    // Give back the locks taken after the mark; nobody saw our
    // intent on those rows.
    lock_release_from(s, s->sp_locks);
    return 0;
}

/* sm_begin, minus the out-of-memory guard. */
int begin(sm_session_t* s, sm_xctlevel_t level)
{
    if (!s) {
        return set_error(SM_ERR_INVALID, "sm_begin: session is NULL");
    }
    if (s->active) {
        return set_error(SM_ERR_INTRANS, "a transaction is already active");
    }
    if (level != SM_XCT_READ_COMMITTED && level != SM_XCT_REPEATABLE_READ &&
        level != SM_XCT_SERIALIZABLE) {
        return set_error(SM_ERR_INVALID, "unknown isolation level %d",
                         (int)level);
    }
    reset(s);
    s->level = level;
    s->begin_seq = ++s->db->begin_seq;
    s->doomed = 0;
    // Take the snapshot: the current visible_ts (§4.1). Read
    // committed also counts in the watermark at this one, for
    // its whole life (§8.3).
    s->snapshot_ts = db_take_snapshot(s->db);
    s->snapshot_registered = true;
    ssi_begin(s);
    s->active = true;
    return 0;
}

/* sm_commit, minus the out-of-memory guard. */
int commit(sm_session_t* s)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    // Close cursors first, releasing their snapshots and
    // narrowing their SIREAD ranges before validation (§8.2).
    xct_end_scans(s);
    db_t* db = s->db;
    if (s->xid == 0) {
        // Read-only: nothing to publish. Under SSI it ended just
        // after the current visible_ts (§7.3).
        if (ssi_precommit(s, db->visible_ts.load() + 1) != 0) {
            return xct_fail_retryable(s, SM_ERR_SERIALIZATION,
                                      "not serializable");
        }
        ssi_end(s);
        reset(s);
        return 0;
    }

    uint64_t xid = s->xid;
    uint64_t commit_ts;
    if (xct_check_writable(db) != 0 ||
        db_claim_commit_ts(db, &commit_ts) != 0) {
        // No record was written, so the transaction is dead.
        discard(s);
        return -1;
    }
    // Validate under SSI before the commit point; on failure give
    // the commit_ts back to the sequencer unused (§7.3, §8.2).
    if (ssi_precommit(s, commit_ts) != 0) {
        db_commit_done(db, commit_ts);
        return xct_fail_retryable(s, SM_ERR_SERIALIZATION, "not serializable");
    }
    std::string val;
    val.push_back(static_cast<char>(txn_committed));
    put_be64(val, s->snapshot_ts);
    put_be64(val, commit_ts);
    rocksdb::Status st;
    if (db->test_before_commit) {
        st = db->test_before_commit(commit_ts);
    }
    if (st.ok()) {
        rocksdb::WriteOptions wo;
        wo.sync = db->durability_sync != 0;
        // Write the status record: the commit point (§8.2).
        st = db->rdb->Put(wo, db->cf_txn, txn_key(xid), val);
    }
    if (!st.ok()) {
        // Fail stop: the record may have reached the WAL, so the
        // transaction stays in flight -- in the active table,
        // its commit_ts holding back visible_ts -- until a
        // reopen resolves it from the log (§8.2). Its locks go:
        // the database refuses writes from now on anyway.
        db->failed = true;
        lock_release_all(s);
        reset(s);
        return set_error(SM_ERR_IO, "write commit record (outcome unknown): %s",
                         st.ToString().c_str());
    }
    // Resolve before publishing: a reader that misses the xid in
    // the active table finds it in the cache or CF:txn (§3.4).
    enter_cache(db, xid, commit_ts);
    db_commit_done(db, commit_ts);
    // Release the row locks only once the commit is visible, and
    // queue dropped relations for removal once no snapshot can
    // see them (§8.3).
    lock_release_all(s);
    gc_queue_drops(db, commit_ts, s->dropped);
    ssi_end(s); // the committed node stays while others overlap it
    reset(s);
    return 0;
}

/*
 * sm_abort, minus the out-of-memory guard. It skips the doomed
 * check: a doomed transaction can still be aborted (§5.1).
 */
int abort_xct(sm_session_t* s)
{
    if (!s) {
        return set_error(SM_ERR_INVALID, "sm_abort: session is NULL");
    }
    if (!s->active) {
        return set_error(SM_ERR_NOTRANS, "no active transaction");
    }
    discard(s);
    return 0;
}

} // namespace

namespace wisr
{

/* NOTRANS unless a transaction is active. */
int xct_check_active(sm_session_t* s)
{
    if (!s) {
        return set_error(SM_ERR_INVALID, "session is NULL");
    }
    if (!s->active) {
        return set_error(SM_ERR_NOTRANS, "no active transaction");
    }
    // A doomed transaction fails here, before doing anything.
    if (s->doomed) {
        return xct_fail_retryable(s, SM_ERR_DEADLOCK,
                                  "chosen as a deadlock victim");
    }
    return 0;
}

/* Abort on the engine's side, then report the retryable error. */
int xct_fail_retryable(sm_session_t* s, sm_errno_t e, const char* msg)
{
    discard(s);
    return set_error(e, "%s; the transaction was aborted", msg);
}

/* Read committed takes a statement snapshot; the others reuse the begin one. */
view_t xct_view(sm_session_t* s)
{
    view_t v;
    v.snapshot_ts = s->level == SM_XCT_READ_COMMITTED ? s->db->visible_ts.load()
                                                      : s->snapshot_ts;
    v.xid = s->xid;
    v.seq_hwm = s->seq_hwm;
    return v;
}

/* Fail with SM_ERR_IO once the database has failed stop. */
int xct_check_writable(db_t* db)
{
    if (db->failed) {
        return set_error(SM_ERR_IO,
                         "database %s failed a status write; reopen it",
                         db->name.c_str());
    }
    return 0;
}

/* Allocate the xid and enter the active table, before any version. */
int xct_alloc_xid(sm_session_t* s)
{
    uint64_t xid;
    if (db_alloc_ts(s->db, &xid) != 0) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> guard(s->db->txn_mu);
        s->db->active.insert(xid);
    }
    s->xid = xid;
    // Map the xid to the SSI node before any version carries it,
    // so a reader that steps past one finds the writer (§7.2).
    ssi_on_xid(s);
    return 0;
}

/*
 * Active table, then commit cache, then CF:txn (§3.4 steps 1-4).
 * A record in any state but COMMITTED or ABORTED -- PREPARED --
 * is in flight, so a compaction filter running before recovery
 * has rebuilt the active table still leaves it alone.
 */
xct_state_t xct_status(db_t* db, uint64_t xid, uint64_t* commit_ts)
{
    {
        std::lock_guard<std::mutex> guard(db->txn_mu);
        if (db->active.count(xid)) {
            return XCT_IN_FLIGHT;
        }
        auto it = db->cache.find(xid);
        if (it != db->cache.end()) {
            *commit_ts = it->second;
            return XCT_COMMITTED;
        }
    }
    std::string val;
    rocksdb::Status s =
        db->rdb->Get(rocksdb::ReadOptions(), db->cf_txn, txn_key(xid), &val);
    if (s.IsNotFound()) {
        return XCT_DEAD;
    }
    if (!s.ok()) {
        set_status_error(s, "read CF:txn");
        return XCT_ERROR;
    }
    if (val.size() < txn_record_size) {
        set_error(SM_ERR_CORRUPT, "short status record for xid %llu",
                  (unsigned long long)xid);
        return XCT_ERROR;
    }
    switch (static_cast<uint8_t>(val[0])) {
    case txn_committed:
        *commit_ts = get_be64(val.data() + 9);
        return XCT_COMMITTED;
    case txn_aborted:
        return XCT_DEAD;
    default:
        return XCT_IN_FLIGHT;
    }
}

/* Scan CF:txn and delete, in one batch, the records the sweep covers. */
int64_t xct_drop_committed_records(db_t* db, uint64_t w0)
{
    rocksdb::WriteBatch batch;
    std::unique_ptr<rocksdb::Iterator> it(
        db->rdb->NewIterator(rocksdb::ReadOptions(), db->cf_txn));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        rocksdb::Slice v = it->value();
        if (v.size() >= txn_record_size &&
            static_cast<uint8_t>(v[0]) == txn_committed &&
            get_be64(v.data() + 9) <= w0) {
            batch.Delete(db->cf_txn, it->key());
        }
    }
    if (!it->status().ok()) {
        set_status_error(it->status(), "scan CF:txn");
        return -1;
    }
    rocksdb::Status s = db->rdb->Write(rocksdb::WriteOptions(), &batch);
    if (!s.ok()) {
        set_status_error(s, "delete swept status records");
        return -1;
    }
    return batch.Count();
}

/* xct_status, folded to committed or not. */
int xct_resolve(db_t* db, uint64_t xid, uint64_t* commit_ts)
{
    switch (xct_status(db, xid, commit_ts)) {
    case XCT_COMMITTED:
        return 1;
    case XCT_ERROR:
        return -1;
    default:
        return 0;
    }
}

/* Close every open cursor of the session. */
void xct_end_scans(sm_session_t* s)
{
    while (!s->scans.empty()) {
        sm_scan_end(*s->scans.begin());
    }
}

} // namespace wisr

/* Begin a transaction at level. */
int sm_begin(sm_session_t* session, sm_xctlevel_t level)
{
    try {
        return begin(session, level);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_begin: out of memory");
    }
}

/* Commit the active transaction. */
int sm_commit(sm_session_t* session)
{
    try {
        return commit(session);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_commit: out of memory");
    }
}

/* Set the session's one savepoint. */
int sm_savepoint(sm_session_t* session)
{
    try {
        return savepoint(session);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_savepoint: out of memory");
    }
}

/* Undo every write made since the savepoint. */
int sm_rollback(sm_session_t* session)
{
    try {
        return rollback(session);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_rollback: out of memory");
    }
}

/* Abort the active transaction. */
int sm_abort(sm_session_t* session)
{
    try {
        return abort_xct(session);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_abort: out of memory");
    }
}
