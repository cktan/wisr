/*
 * session.h -- a session, its transaction, and its cursors,
 * plus the internal calls between xct.cc, catalog.cc, and
 * access.cc.
 */
#ifndef WISR_SESSION_H
#define WISR_SESSION_H

#include "db.h"
#include "sm1.h"

#include <stdint.h>

#include <atomic>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>

struct sm1_session_t {
  sm1_env_t *env = nullptr;
  wisr::db_t *db = nullptr;

  /* The transaction; meaningful only while active. */
  bool active = false;
  sm1_xctlevel_t level = SM1_XCT_READ_COMMITTED;
  uint64_t snapshot_ts = 0;
  uint64_t xid = 0;              /* 0 until the first write (§4.1) */
  uint32_t seq = 0;              /* last write's seq; the next is seq + 1 */
  uint32_t seq_hwm = 0;          /* own versions above this are invisible */
  uint64_t wts_hi = 0;           /* largest wts written; bounds own seeks */
  std::vector<uint64_t> created; /* storeids made by sm1_mkstore */
  std::vector<uint64_t> dropped; /* storeids removed by sm1_rmstore */
  std::vector<uint64_t> dropped_seqs; /* seqids removed by sm1_rmseq */
  bool snapshot_registered = false;   /* counted in gc_watermark */
  uint64_t begin_seq = 0;             /* begin order; larger is younger */

  /*
   * The statement snapshot under read committed (§6.2). Off
   * until sm1_stmt_begin, which turns it on for the rest of the
   * transaction; the snapshot itself is taken by the first
   * read of each statement, not by the boundary call.
   */
  bool stmt_pinned = false; /* sm1_stmt_begin was called */
  bool stmt_taken = false;  /* stmt_snapshot_ts is the one to use */
  uint64_t stmt_snapshot_ts = 0;

  /*
   * The undo list (§6.5): every version written, in write
   * order, as (storeid, userkey, wts, seq), so a rollback can delete
   * exactly the versions written since the savepoint.
   */
  struct undo_t {
    uint64_t storeid;
    std::string key;
    uint64_t wts;
    uint32_t seq;
  };
  std::vector<undo_t> undo;

  /*
   * The savepoint stack (§6.5): one mark per savepoint, in
   * push order, so savepoints[d - 1] is the mark at depth d.
   */
  struct mark_t {
    size_t undo = 0;         /* undo.size() at the mark */
    size_t locks = 0;        /* locks.size() at the mark */
    size_t created = 0;      /* created.size() at the mark */
    size_t dropped = 0;      /* dropped.size() at the mark */
    size_t dropped_seqs = 0; /* dropped_seqs.size() at the mark */
    uint64_t scan_id = 0;    /* cursors with a larger id came later */
  };
  std::vector<mark_t> savepoints;

  /*
   * Why the transaction must fail at its next call (§5.1):
   * SM_ERR_DEADLOCK once chosen as a deadlock victim, else 0.
   */
  std::atomic<int> doomed{0};

  /* Row locks (§5.1); guarded by the database's lock_mu. */
  struct lockref_t {
    std::string key;        /* the lock table key */
    bool exclusive = false; /* the mode held now, after upgrades */
    bool latest = false;    /* read_latest done: the write may go ahead */
  };
  std::vector<lockref_t> locks; /* held, in acquisition order */
  size_t latest_marks = 0;      /* locks with latest set; 0 skips scans */
  /*
   * The holders blocking us, empty when we are not waiting. A
   * shared lock has many, so the waits-for graph branches and
   * §5.1's cycle search walks it rather than a chain.
   */
  std::vector<sm1_session_t *> waiting_for;

  wisr::ssi_node_t *ssi = nullptr; /* SERIALIZABLE only (§7) */

  /*
   * Two-phase commit (§8.4). sm1_prepare moves the transaction
   * out of the session into a database-owned sm1_session_t
   * with `prepared` set, registered under its gid; the
   * preparing session keeps only prepared_gid, and has no
   * transaction of its own until that gid is resolved.
   */
  std::string prepared_gid;
  bool prepared = false;     /* this object is a prepared transaction */
  long long prepared_at = 0; /* wall-clock seconds of sm1_prepare */
  bool restored = false;     /* rebuilt from its record at open (§9.1) */
  bool limbo = false;        /* its prepare write failed (§8.4) */

  std::set<sm1_scan_t *> scans; /* open cursors */
  uint64_t next_scan_id = 0;    /* hands out sm1_scan_t::id */
  std::string readbuf;          /* what sm1_read hands out */
};

struct sm1_scan_t {
  sm1_session_t *session = nullptr;
  uint64_t id = 0;    /* opening order within the session */
  uint64_t epoch = 0; /* read epoch held for the cursor's life */
  const rocksdb::Snapshot *snap = nullptr; /* the physical view */
  std::unique_ptr<rocksdb::Iterator> it;
  std::string upper; /* owns iterate_upper_bound's bytes */
  rocksdb::Slice upper_slice;
  /*
   * The seek bound skey was positioned with, empty when the
   * scan has no lower bound. sm1_scan_seek refuses a target
   * below it: the iterator carries no lower bound, so a seek
   * under it would leave the range the cursor registered
   * (§10.2).
   */
  std::string lower;
  uint64_t snapshot_ts = 0; /* the logical view, fixed at open */
  uint64_t xid = 0;
  uint32_t seq_hwm = 0;
  int state = 0; /* 0 open, 1 at end, -1 failed */

  /* The scan's SIREAD range and how far it read (§7.1). */
  bool has_ssi_range = false;
  uint64_t ssi_storeid = 0;
  std::list<wisr::ssi_range_t>::iterator ssi_range;
  bool examined = false;     /* some userkey's run was inspected */
  std::string last_examined; /* the last one */
  std::string key, data;     /* what sm1_scannxt hands out */
};

namespace wisr {

/* What a read sees: the MVCC snapshot and the reader's own writes. */
struct view_t {
  uint64_t snapshot_ts;
  uint64_t xid;
  uint32_t seq_hwm;
};

/* Write operations, for access_write(). */
enum write_op_t { WRITE_INSERT, WRITE_UPDATE, WRITE_DELETE };

/* Fail with NOTRANS unless session has an active transaction. */
int xct_check_active(sm1_session_t *session);

/* The view a statement starting now reads at (§6.2, §10.1). */
view_t xct_view(sm1_session_t *session);

/*
 * The same, for sm1_read and sm1_scan: under a statement
 * boundary it also TAKES the statement's snapshot, if this is
 * the statement's first read (§6.2).
 */
view_t xct_read_view(sm1_session_t *session);

/*
 * Abort the active transaction on the engine's side, as for a
 * retryable error, then record e with msg. Returns -1. The
 * caller must not call sm1_abort afterwards (sm1.h).
 */
int xct_fail_retryable(sm1_session_t *session, sm1_errno_t e, const char *msg);

/*
 * Take the row lock on (storeid, key) for the session's
 * transaction in the given mode, blocking while another holds
 * it in a conflicting one (§5.1). A transaction already
 * holding it shared and asking exclusive upgrades. Returns 0,
 * or -1 without an error set if the transaction was doomed
 * while waiting; the caller then fails it with s->doomed.
 */
int lock_acquire(sm1_session_t *session, uint64_t storeid, rocksdb::Slice key,
                 bool exclusive);

/* §5.2's first-committer-wins check alone, for sm1_lock. */
int access_check_current(sm1_session_t *session, uint64_t storeid,
                         rocksdb::Slice key);

/*
 * The mode the session holds (storeid, key) in: 0 none, 1 shared,
 * 2 exclusive. Reads only the session's own lock list.
 */
int lock_mode_held(sm1_session_t *session, uint64_t storeid,
                   rocksdb::Slice key);

/* Mark the exclusive lock on (storeid, key): a write may go ahead. */
void lock_set_latest(sm1_session_t *session, uint64_t storeid,
                     rocksdb::Slice key);

/* Whether (storeid, key)'s lock carries the mark. */
bool lock_has_latest(sm1_session_t *session, uint64_t storeid,
                     rocksdb::Slice key);

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1_session_t *session, uint64_t storeid,
                       rocksdb::Slice key);

/* Move every lock s holds to g, the prepared object (§8.4). */
void lock_transfer(db_t *db, sm1_session_t *s, sm1_session_t *g);

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm1_session_t *g);

/* Release every row lock the session's transaction holds. */
void lock_release_all(sm1_session_t *session);

/*
 * Release the locks the session took after its first n, the
 * ones a rollback to a savepoint gives back (§6.5).
 */
void lock_release_from(sm1_session_t *session, size_t n);

/*
 * Delete the COMMITTED status records with commit_ts <= w0, for
 * the CF:txn sweep (§8.3). Returns the number deleted, or -1.
 */
int64_t xct_drop_committed_records(wisr::db_t *db, uint64_t w0);

/* Give a SERIALIZABLE transaction its SSI node (§7). */
void ssi_begin(sm1_session_t *session);

/* Map the transaction's new xid to its SSI node. */
void ssi_on_xid(sm1_session_t *session);

/* Leave a SIREAD entry on (storeid, key) (§7.1). */
void ssi_on_read(sm1_session_t *session, uint64_t storeid, rocksdb::Slice key);

/* A read stepped past a version of concurrent writer xid (§7.2). */
void ssi_on_skip(sm1_session_t *session, uint64_t writer_xid);

/* A write of (storeid, key): edges from its concurrent readers (§7.2). */
void ssi_on_write(sm1_session_t *session, uint64_t storeid, rocksdb::Slice key);

/* Register a scan's requested range as a SIREAD entry (§10.2). */
void ssi_range_open(sm1_scan_t *scan, uint64_t storeid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive);

/* Narrow a closing scan's range to what it traversed (§7.1). */
void ssi_range_close(sm1_scan_t *scan);

/*
 * The SSI commit check (§7.3) for a transaction ending at
 * end_ts. Returns 0, or -1 if it must fail with
 * SM_ERR_SERIALIZATION.
 */
int ssi_precommit(sm1_session_t *session, uint64_t end_ts);

/* Detach the session from its SSI node at transaction end. */
void ssi_end(sm1_session_t *session);

/* A prepared transaction committed at end_ts (§8.4). */
void ssi_prepared_committed(sm1_session_t *session, uint64_t end_ts);

/* A prepared transaction aborted: free its SSI state. */
void ssi_prepared_aborted(sm1_session_t *session);

/* Record xid as a transaction restored in doubt (§8.4, §9.1). */
void ssi_restore(wisr::db_t *db, uint64_t xid);

/* Free SSI nodes no active transaction overlaps (§7.1). */
void ssi_cleanup(wisr::db_t *db);

/*
 * Decode a PREPARED status record for xid into a new database-
 * owned transaction object; false if it is not one.
 */
bool xct_decode_prepared(wisr::db_t *db, uint64_t xid, rocksdb::Slice val,
                         sm1_session_t **out);

/*
 * Restore every prepared transaction from CF:txn, then drop the
 * store CFs nothing needs (§9.1 steps 4-6). Returns 0 or -1.
 */
int recover(wisr::db_t *db);

/* Fail with SM_ERR_IO if db has failed stop (§8.2). */
int xct_check_writable(wisr::db_t *db);

/*
 * Give the session's transaction its xid (§4.1) and enter it
 * into the active table. Returns 0 or -1.
 */
int xct_alloc_xid(sm1_session_t *session);

/* The fate of a version's writer (§3.4). */
enum xct_state_t { XCT_DEAD, XCT_IN_FLIGHT, XCT_COMMITTED, XCT_ERROR };

/*
 * Resolve the writer of xid (§3.4), setting *commit_ts if it
 * committed. XCT_ERROR sets the error. Callers check the stamp
 * and their own xid first.
 */
xct_state_t xct_status(wisr::db_t *db, uint64_t xid, uint64_t *commit_ts);

/*
 * Resolve the writer of xid (§3.4): 1 and *commit_ts if it
 * committed, 0 if it is in flight or dead, -1 on error.
 */
int xct_resolve(wisr::db_t *db, uint64_t xid, uint64_t *commit_ts);

/* Close every cursor open on session. */
void xct_end_scans(sm1_session_t *session);

/*
 * Look storeid up at view: 0 if it names a live store, 1 if
 * not, -1 on error.
 */
int catalog_lookup(sm1_session_t *session, const view_t &view,
                   uint64_t storeid);

/*
 * Point read of key in storeid at view: 0 and *out = payload if
 * a live version is visible, 1 if none, -1 on error.
 */
int access_read(sm1_session_t *session, uint64_t storeid, const view_t &view,
                rocksdb::Slice key, std::string *out);

/*
 * Open a cursor over storeid at view, between the bounds as
 * sm1_scan() takes them. Returns NULL on error.
 */
sm1_scan_t *access_scan(sm1_session_t *session, uint64_t storeid,
                        const view_t &view, const char *skey, size_t sklen,
                        int skey_inclusive, const char *ekey, size_t eklen,
                        int ekey_inclusive);

/*
 * Write one version of key into storeid, checking that the key
 * exists (update, delete) or does not (insert). With old
 * non-NULL, it is filled with the row the write replaces, which
 * the existence check has already decoded. Returns 0, or -1
 * with SM_ERR_DUPKEY / SM_ERR_NOTFOUND / another error.
 */
int access_write(sm1_session_t *session, uint64_t storeid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data, std::string *old);

} // namespace wisr

#endif /* WISR_SESSION_H */
