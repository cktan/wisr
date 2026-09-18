/*
 * xct.cc -- sm_begin, sm_commit, sm_abort, and the commit
 * status table CF:txn (DESIGN.md §3.2, §3.4, §6, §8.2).
 *
 * Build order step 1: one transaction at a time. There is no
 * commit sequencer, active-transaction table, or commit cache
 * yet (step 2), no row locks (step 3), and no savepoint or
 * prepare. A transaction that has not committed has no status
 * record, and absence of a record resolves as dead (§3.4).
 */

#include "codec.h"
#include "error.h"
#include "session.h"

#include <new>

using namespace wisr;

namespace
{

/* CF:txn states (§3.2). */
constexpr uint8_t txn_committed = 2;

/* Status record size: state, snapshot_ts, commit_ts. */
constexpr size_t txn_record_size = 1 + 8 + 8;

/* The CF:txn key of xid. */
std::string txn_key(uint64_t xid)
{
    std::string k;
    put_be64(k, xid);
    return k;
}

/* Raise visible_ts to at least ts; it never moves backwards. */
void publish(db_t* db, uint64_t ts)
{
    uint64_t cur = db->visible_ts.load();
    while (cur < ts && !db->visible_ts.compare_exchange_weak(cur, ts)) {
    }
}

/* Forget the transaction; the session has none afterwards. */
void reset(sm_session_t* s)
{
    s->active = false;
    s->xid = 0;
    s->seq = 0;
    s->seq_hwm = 0;
    s->created.clear();
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
    // Take the snapshot: the current visible_ts (§4.1).
    s->snapshot_ts = s->db->visible_ts.load();
    s->active = true;
    return 0;
}

/* sm_commit, minus the out-of-memory guard. */
int commit(sm_session_t* s)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    // Close cursors first, releasing their snapshots (§8.2).
    xct_end_scans(s);
    if (s->xid == 0) {
        reset(s); // read-only: nothing to publish
        return 0;
    }

    db_t* db = s->db;
    uint64_t commit_ts;
    if (db_alloc_ts(db, &commit_ts) != 0) {
        // No record was written, so the transaction is dead.
        drop_created(s);
        reset(s);
        return -1;
    }
    std::string val;
    val.push_back(static_cast<char>(txn_committed));
    put_be64(val, s->snapshot_ts);
    put_be64(val, commit_ts);
    rocksdb::WriteOptions wo;
    wo.sync = db->durability_sync != 0;
    // Write the status record: the commit point (§8.2).
    rocksdb::Status st = db->rdb->Put(wo, db->cf_txn, txn_key(s->xid), val);
    reset(s);
    if (!st.ok()) {
        // Outcome unknown: the record may have reached the WAL.
        // The fail-stop state of §8.2 is build order step 2.
        return set_error(SM_ERR_IO, "write commit record (outcome unknown): %s",
                         st.ToString().c_str());
    }
    publish(db, commit_ts);
    return 0;
}

/* sm_abort, minus the out-of-memory guard. */
int abort_xct(sm_session_t* s)
{
    if (xct_check_active(s) != 0) {
        return -1;
    }
    xct_end_scans(s);
    // Discard the writes: with no status record they resolve
    // dead (§3.4). Only the created column families need undoing.
    drop_created(s);
    reset(s);
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
    return 0;
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

/* Look up xid's status record in CF:txn. */
int xct_resolve(db_t* db, uint64_t xid, uint64_t* commit_ts)
{
    std::string val;
    rocksdb::Status s =
        db->rdb->Get(rocksdb::ReadOptions(), db->cf_txn, txn_key(xid), &val);
    if (s.IsNotFound()) {
        return 0;
    }
    if (!s.ok()) {
        return set_status_error(s, "read CF:txn");
    }
    if (val.size() < txn_record_size) {
        return set_error(SM_ERR_CORRUPT, "short status record for xid %llu",
                         (unsigned long long)xid);
    }
    if (static_cast<uint8_t>(val[0]) != txn_committed) {
        return 0;
    }
    *commit_ts = get_be64(val.data() + 9);
    return 1;
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

/* Abort the active transaction. */
int sm_abort(sm_session_t* session)
{
    try {
        return abort_xct(session);
    } catch (const std::bad_alloc&) {
        return set_error(SM_ERR_NOMEM, "sm_abort: out of memory");
    }
}
