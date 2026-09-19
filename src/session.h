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
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>

struct sm_session_t {
  sm_env_t *env = nullptr;
  wisr::db_t *db = nullptr;

  /* The transaction; meaningful only while active. */
  bool active = false;
  sm_xctlevel_t level = SM_XCT_READ_COMMITTED;
  uint64_t snapshot_ts = 0;
  uint64_t xid = 0;                 /* 0 until the first write (§4.1) */
  uint32_t seq = 0;                 /* last write's seq; the next is seq + 1 */
  uint32_t seq_hwm = 0;             /* own versions above this are invisible */
  std::vector<uint64_t> created;    /* cfids made by sm_mkrel */
  std::vector<uint64_t> dropped;    /* cfids removed by sm_rmrel */
  bool snapshot_registered = false; /* counted in gc_watermark */
  uint64_t begin_seq = 0;           /* begin order; larger is younger */

  /*
   * The undo list (§6.5): every version written, in write
   * order, as (cfid, userkey, seq), so a rollback can delete
   * exactly the versions written since the savepoint.
   */
  struct undo_t {
    uint64_t cfid;
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
  sm_session_t *waiting_for = nullptr; /* the holder blocking us */

  wisr::ssi_node_t *ssi = nullptr; /* SERIALIZABLE only (§7) */

  /*
   * Two-phase commit (§8.4). sm_prepare moves the transaction
   * out of the session into a database-owned sm_session_t
   * with `prepared` set, registered under its gid; the
   * preparing session keeps only prepared_gid, and has no
   * transaction of its own until that gid is resolved.
   */
  std::string prepared_gid;
  bool prepared = false;     /* this object is a prepared transaction */
  long long prepared_at = 0; /* wall-clock seconds of sm_prepare */
  bool restored = false;     /* rebuilt from its record at open (§9.1) */
  bool limbo = false;        /* its prepare write failed (§8.4) */

  std::set<sm_scan_t *> scans; /* open cursors */
  uint64_t next_scan_id = 0;   /* hands out sm_scan_t::id */
  std::string readbuf;         /* what sm_read hands out */
};

struct sm_scan_t {
  sm_session_t *session = nullptr;
  uint64_t id = 0;    /* opening order within the session */
  uint64_t epoch = 0; /* read epoch held for the cursor's life */
  const rocksdb::Snapshot *snap = nullptr; /* the physical view */
  std::unique_ptr<rocksdb::Iterator> it;
  std::string upper; /* owns iterate_upper_bound's bytes */
  rocksdb::Slice upper_slice;
  uint64_t snapshot_ts = 0; /* the logical view, fixed at open */
  uint64_t xid = 0;
  uint32_t seq_hwm = 0;
  int state = 0; /* 0 open, 1 at end, -1 failed */

  /* The scan's SIREAD range and how far it read (§7.1). */
  bool has_ssi_range = false;
  uint64_t ssi_cfid = 0;
  std::list<wisr::ssi_range_t>::iterator ssi_range;
  bool examined = false;     /* some userkey's run was inspected */
  std::string last_examined; /* the last one */
  std::string key, data;     /* what sm_scannxt hands out */
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
int xct_check_active(sm_session_t *session);

/* The view a statement starting now reads at (§6.2, §10.1). */
view_t xct_view(sm_session_t *session);

/*
 * Abort the active transaction on the engine's side, as for a
 * retryable error, then record e with msg. Returns -1. The
 * caller must not call sm_abort afterwards (sm.h).
 */
int xct_fail_retryable(sm_session_t *session, sm_errno_t e, const char *msg);

/*
 * Take the exclusive row lock on (cfid, key) for the session's
 * transaction, blocking while another holds it (§5.1). Returns
 * 0, or -1 without an error set if the transaction was doomed
 * while waiting; the caller then fails it with s->doomed.
 */
int lock_acquire(sm_session_t *session, uint64_t cfid, rocksdb::Slice key);

/* Release every row lock the session's transaction holds. */
void lock_release_all(sm_session_t *session);

/*
 * Release the locks the session took after its first n, the
 * ones a rollback to a savepoint gives back (§6.5).
 */
void lock_release_from(sm_session_t *session, size_t n);

/*
 * Delete the COMMITTED status records with commit_ts <= w0, for
 * the CF:txn sweep (§8.3). Returns the number deleted, or -1.
 */
int64_t xct_drop_committed_records(wisr::db_t *db, uint64_t w0);

/* Give a SERIALIZABLE transaction its SSI node (§7). */
void ssi_begin(sm_session_t *session);

/* Map the transaction's new xid to its SSI node. */
void ssi_on_xid(sm_session_t *session);

/* Leave a SIREAD entry on (cfid, key) (§7.1). */
void ssi_on_read(sm_session_t *session, uint64_t cfid, rocksdb::Slice key);

/* A read stepped past a version of concurrent writer xid (§7.2). */
void ssi_on_skip(sm_session_t *session, uint64_t writer_xid);

/* A write of (cfid, key): edges from its concurrent readers (§7.2). */
void ssi_on_write(sm_session_t *session, uint64_t cfid, rocksdb::Slice key);

/* Register a scan's requested range as a SIREAD entry (§10.2). */
void ssi_range_open(sm_scan_t *scan, uint64_t cfid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive);

/* Narrow a closing scan's range to what it traversed (§7.1). */
void ssi_range_close(sm_scan_t *scan);

/*
 * The SSI commit check (§7.3) for a transaction ending at
 * end_ts. Returns 0, or -1 if it must fail with
 * SM_ERR_SERIALIZATION.
 */
int ssi_precommit(sm_session_t *session, uint64_t end_ts);

/* Detach the session from its SSI node at transaction end. */
void ssi_end(sm_session_t *session);

/* A prepared transaction committed at end_ts (§8.4). */
void ssi_prepared_committed(sm_session_t *session, uint64_t end_ts);

/* A prepared transaction aborted: free its SSI state. */
void ssi_prepared_aborted(sm_session_t *session);

/* Record xid as a transaction restored in doubt (§8.4, §9.1). */
void ssi_restore(wisr::db_t *db, uint64_t xid);

/* Free SSI nodes no active transaction overlaps (§7.1). */
void ssi_cleanup(wisr::db_t *db);

/*
 * Decode a PREPARED status record for xid into a new database-
 * owned transaction object; false if it is not one.
 */
bool xct_decode_prepared(wisr::db_t *db, uint64_t xid, rocksdb::Slice val,
                         sm_session_t **out);

/*
 * Restore every prepared transaction from CF:txn, then drop the
 * relation CFs nothing needs (§9.1 steps 4-6). Returns 0 or -1.
 */
int recover(wisr::db_t *db);

/* Fail with SM_ERR_IO if db has failed stop (§8.2). */
int xct_check_writable(wisr::db_t *db);

/*
 * Give the session's transaction its xid (§4.1) and enter it
 * into the active table. Returns 0 or -1.
 */
int xct_alloc_xid(sm_session_t *session);

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
void xct_end_scans(sm_session_t *session);

/*
 * Resolve relid at view: 0 and *cfid if it names a live
 * relation, 1 if not, -1 on error.
 */
int catalog_lookup(sm_session_t *session, const view_t &view, uint64_t relid,
                   uint64_t *cfid);

/*
 * Point read of key in cfid at view: 0 and *out = payload if
 * a live version is visible, 1 if none, -1 on error.
 */
int access_read(sm_session_t *session, uint64_t cfid, const view_t &view,
                rocksdb::Slice key, std::string *out);

/*
 * Open a cursor over cfid at view, between the bounds as
 * sm_scan() takes them. Returns NULL on error.
 */
sm_scan_t *access_scan(sm_session_t *session, uint64_t cfid, const view_t &view,
                       const char *skey, size_t sklen, int skey_inclusive,
                       const char *ekey, size_t eklen, int ekey_inclusive);

/*
 * Write one version of key into cfid, checking that the key
 * exists (update, delete) or does not (insert). Returns 0, or
 * -1 with SM_ERR_DUPKEY / SM_ERR_NOTFOUND / another error.
 */
int access_write(sm_session_t *session, uint64_t cfid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data);

} // namespace wisr

#endif /* WISR_SESSION_H */
