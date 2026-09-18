/*
 * session.h -- a session, its transaction, and its cursors,
 * plus the internal calls between xct.cc, catalog.cc, and
 * access.cc.
 */
#ifndef WISR_SESSION_H
#define WISR_SESSION_H

#include "db.h"
#include "sm.h"

#include <stdint.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>

struct sm_session_t {
    sm_env_t* env = nullptr;
    wisr::db_t* db = nullptr;

    /* The transaction; meaningful only while active. */
    bool active = false;
    sm_xctlevel_t level = SM_XCT_READ_COMMITTED;
    uint64_t snapshot_ts = 0;
    uint64_t xid = 0;              /* 0 until the first write (§4.1) */
    uint32_t seq = 0;              /* last write's seq; the next is seq + 1 */
    uint32_t seq_hwm = 0;          /* own versions above this are invisible */
    std::vector<uint64_t> created; /* relids made by sm_mkrel */
    std::vector<uint64_t> dropped; /* relids removed by sm_rmrel */
    bool snapshot_registered = false; /* counted in gc_watermark */
    uint64_t begin_seq = 0;           /* begin order; larger is younger */

    /*
     * The undo list (§6.5): every version written, in write
     * order, as (relid, userkey, seq), so a rollback can delete
     * exactly the versions written since the savepoint.
     */
    struct undo_t {
        uint64_t relid;
        std::string key;
        uint32_t seq;
    };
    std::vector<undo_t> undo;

    /* The one savepoint: positions to roll back to (§6.5). */
    bool has_savepoint = false;
    size_t sp_undo = 0;      /* undo.size() at the mark */
    size_t sp_locks = 0;     /* locks.size() at the mark */
    size_t sp_created = 0;   /* created.size() at the mark */
    size_t sp_dropped = 0;   /* dropped.size() at the mark */
    uint64_t sp_scan_id = 0; /* cursors with a larger id came later */

    /*
     * Why the transaction must fail at its next call (§5.1):
     * SM_ERR_DEADLOCK once chosen as a deadlock victim, else 0.
     */
    std::atomic<int> doomed{0};

    /* Row locks (§5.1); guarded by the database's lock_mu. */
    std::vector<std::string> locks;      /* held, in acquisition order */
    sm_session_t* waiting_for = nullptr; /* the holder blocking us */

    std::set<sm_scan_t*> scans; /* open cursors */
    uint64_t next_scan_id = 0;  /* hands out sm_scan_t::id */
    std::string readbuf;        /* what sm_read hands out */
};

struct sm_scan_t {
    sm_session_t* session = nullptr;
    uint64_t id = 0; /* opening order within the session */
    const rocksdb::Snapshot* snap = nullptr; /* the physical view */
    std::unique_ptr<rocksdb::Iterator> it;
    std::string upper; /* owns iterate_upper_bound's bytes */
    rocksdb::Slice upper_slice;
    uint64_t snapshot_ts = 0; /* the logical view, fixed at open */
    uint64_t xid = 0;
    uint32_t seq_hwm = 0;
    int state = 0;         /* 0 open, 1 at end, -1 failed */
    std::string key, data; /* what sm_scan_next hands out */
};

namespace wisr
{

/* What a read sees: the MVCC snapshot and the reader's own writes. */
struct view_t {
    uint64_t snapshot_ts;
    uint64_t xid;
    uint32_t seq_hwm;
};

/* Write operations, for access_write(). */
enum write_op_t { WRITE_INSERT, WRITE_UPDATE, WRITE_DELETE };

/* Fail with NOTRANS unless session has an active transaction. */
int xct_check_active(sm_session_t* session);

/* The view a statement starting now reads at (§6.2, §10.1). */
view_t xct_view(sm_session_t* session);

/*
 * Abort the active transaction on the engine's side, as for a
 * retryable error, then record e with msg. Returns -1. The
 * caller must not call sm_abort afterwards (sm.h).
 */
int xct_fail_retryable(sm_session_t* session, sm_errno_t e, const char* msg);

/*
 * Take the exclusive row lock on (relid, key) for the session's
 * transaction, blocking while another holds it (§5.1). Returns
 * 0, or -1 without an error set if the transaction was doomed
 * while waiting; the caller then fails it with s->doomed.
 */
int lock_acquire(sm_session_t* session, uint64_t relid, rocksdb::Slice key);

/* Release every row lock the session's transaction holds. */
void lock_release_all(sm_session_t* session);

/*
 * Release the locks the session took after its first n, the
 * ones a rollback to a savepoint gives back (§6.5).
 */
void lock_release_from(sm_session_t* session, size_t n);

/* Fail with SM_ERR_IO if db has failed stop (§8.2). */
int xct_check_writable(wisr::db_t* db);

/*
 * Give the session's transaction its xid (§4.1) and enter it
 * into the active table. Returns 0 or -1.
 */
int xct_alloc_xid(sm_session_t* session);

/* The fate of a version's writer (§3.4). */
enum xct_state_t { XCT_DEAD, XCT_IN_FLIGHT, XCT_COMMITTED, XCT_ERROR };

/*
 * Resolve the writer of xid (§3.4), setting *commit_ts if it
 * committed. XCT_ERROR sets the error. Callers check the stamp
 * and their own xid first.
 */
xct_state_t xct_status(wisr::db_t* db, uint64_t xid, uint64_t* commit_ts);

/*
 * Resolve the writer of xid (§3.4): 1 and *commit_ts if it
 * committed, 0 if it is in flight or dead, -1 on error.
 */
int xct_resolve(wisr::db_t* db, uint64_t xid, uint64_t* commit_ts);

/* Close every cursor open on session. */
void xct_end_scans(sm_session_t* session);

/*
 * Resolve relname at view: 0 and *relid if it names a live
 * relation, 1 if not, -1 on error.
 */
int catalog_lookup(sm_session_t* session, const view_t& view,
                   const char* relname, uint64_t* relid);

/*
 * Point read of key in relid at view: 0 and *out = payload if
 * a live version is visible, 1 if none, -1 on error.
 */
int access_read(sm_session_t* session, uint64_t relid, const view_t& view,
                rocksdb::Slice key, std::string* out);

/*
 * Write one version of key into relid, checking that the key
 * exists (update, delete) or does not (insert). Returns 0, or
 * -1 with SM_ERR_DUPKEY / SM_ERR_NOTFOUND / another error.
 */
int access_write(sm_session_t* session, uint64_t relid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data);

} // namespace wisr

#endif /* WISR_SESSION_H */
