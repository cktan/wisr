/*
 * session.h -- a session, its transaction, and its cursors,
 * plus the internal calls between xct.cc, catalog.cc, and
 * access.cc.
 */
#ifndef WISR_SESSION_H
#define WISR_SESSION_H

#include "bufpool.h"
#include "db.h"
#include "sm1.h"

#include <stdint.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rocksdb/db.h>

/*
 * A raw pin: keeps one row's bytes in memory (../ROW.md §2). Its
 * caller owns it; empty, both members are.
 */
struct sm1::rawpin_t {
  std::unique_ptr<rocksdb::Iterator> it; /* found the row; null when empty */
  /* A chained store's row, found by Get: the block it holds. */
  rocksdb::PinnableSlice got;
  rocksdb::Slice row; /* the row's bytes, in a block it or got holds */
  /*
   * For an iterator a point read opened (access_read), what
   * decides whether the next read into the pin may seek it again
   * instead of opening another (SM1_DESIGN.md §10.1). No session
   * for any other.
   */
  struct reuse_t {
    const sm1::session_t *session = nullptr; /* whose read; compared only */
    uint64_t storeid = 0;
    uint64_t write_gen = 0;  /* the session's write_gen when opened */
    uint64_t visible_ts = 0; /* every commit at or below it is in it */
    uint64_t epoch = 0;      /* the read epoch it was opened in */
  } reuse;
};

struct sm1::session_t {
  sm1::env_t *env = nullptr;
  wisr::db_t *db = nullptr;

  /* The transaction; meaningful only while active. */
  bool active = false;
  /* What sm1::on_commit added, in the order added; run from the back. */
  std::vector<std::function<void()>> on_commit;
  sm1::xctlevel_t level = sm1::xctlevel_t::read_committed;
  bool read_only = false; /* declared at sm1::begin (§7.4) */
  uint64_t snapshot_ts = 0;
  uint64_t xid = 0;     /* 0 until the first write (§4.1) */
  uint32_t seq = 0;     /* last write's seq; the next is seq + 1 */
  uint32_t seq_hwm = 0; /* own versions above this are invisible */
  uint64_t wts_hi = 0;  /* largest wts written; bounds own seeks */
  /*
   * Raised by every write the session makes, and by each bulk
   * load as it closes, in any transaction, and never lowered as
   * seq is by a rollback: an iterator opened before it last rose
   * may lack the session's own versions.
   */
  uint64_t write_gen = 0;
  std::vector<uint64_t> created;      /* storeids made by sm1::mkstore */
  std::vector<uint64_t> dropped;      /* storeids removed by sm1::rmstore */
  std::vector<uint64_t> dropped_seqs; /* seqids removed by sm1::rmseq */
  /*
   * The temporary stores the session holds, from sm1::mkstore to the
   * commit of an sm1::rmstore or the session's close, and whether
   * the transaction made, dropped or wrote one (TEMPTABLE.md §3).
   */
  std::unordered_set<uint64_t> temp_stores;
  bool temp_touched = false;
  /*
   * The copies of pages it reads, by sm1::keep_page_copy (page.cc):
   * each page's store and key, the frame it was copied from, and the
   * copy, at the frame's version then, shared with the other sessions
   * that read it (bufpool_t::share). At most page_copies_max.
   */
  struct page_copy_t {
    uint64_t storeid = 0;
    std::string key;
    sm1::page_t *frame = nullptr;
    std::shared_ptr<const wisr::bufpool_copy_t> copy;
  };
  std::vector<page_copy_t> page_copies;
  /* The bulk load open in the transaction, if any (load.cc). */
  sm1::loader_t *loader = nullptr;
  /* The transaction bulk loaded: it keeps no write set to prepare. */
  bool loaded = false;
  /*
   * The stores it loaded while a savepoint was set, in load order
   * (BULKLOAD.md §3.5): a rollback to a mark set before a load
   * empties its store, as the load lists no key in the undo list.
   */
  std::vector<uint64_t> loaded_stores;
  /*
   * The session's temporary sequences, by name (sm1.h, mkseq), with
   * those the transaction made and those it dropped: an abort takes
   * the first out again, a commit the second.
   */
  struct temp_seq_t {
    uint64_t start = 0;
    uint64_t increment = 1;
    uint64_t next = 0;      /* next value to hand out */
    bool exhausted = false; /* the last value is handed out */
  };
  std::unordered_map<std::string, temp_seq_t> temp_seqs;
  std::vector<std::string> temp_seqs_made;
  std::vector<std::string> temp_seqs_dropped;
  bool snapshot_registered = false; /* counted in gc_watermark */
  uint64_t begin_seq = 0;           /* begin order; larger is younger */

  /*
   * The statement snapshot under read committed (§6.2). Off
   * until sm1::stmt_begin, which turns it on for the rest of the
   * transaction; the snapshot itself is taken by the first
   * read of each statement, not by the boundary call.
   */
  bool stmt_pinned = false; /* sm1::stmt_begin was called */
  bool qa_nowait = false;   /* QA only: fail, do not wait (§5.1) */
  /* The host's number for the session, 0 for none (sm1::set_tag). */
  std::atomic<uint64_t> tag{0};
  /* How long a wait lasts before a deadlock check, 0 for the db's. */
  int deadlock_ms = 0;
  bool stmt_taken = false; /* stmt_snapshot_ts is the one to use */
  uint64_t stmt_snapshot_ts = 0;
  /*
   * The statement mark (§6.2): once a statement has begun, its
   * reads see the transaction's own writes up to stmt_seq, those
   * made before it began, as PostgreSQL's command id does.
   */
  bool stmt_marked = false;
  uint32_t stmt_seq = 0;

  /*
   * The undo list (§6.5): every version written while a
   * savepoint is set, in write order, as (storeid, userkey,
   * wts, seq), so a rollback can delete exactly the versions
   * written since the savepoint. Empty while no savepoint is
   * set: no rollback could reach a write made then.
   */
  struct undo_t {
    uint64_t storeid;
    std::string key;
    uint64_t wts;
    uint32_t seq;
    bool chained = false; /* a chained key's write (../VCHAIN.md §6) */
  };
  std::vector<undo_t> undo;
  size_t undo_bytes = 0; /* what undo holds, keys included */

  /*
   * The savepoint stack (§6.5): one mark per savepoint, in
   * push order, so savepoints[d - 1] is the mark at depth d.
   */
  struct mark_t {
    size_t undo = 0;              /* undo.size() at the mark */
    uint32_t seq_hwm = 0;         /* seq_hwm at the mark */
    wisr::lock_mark_t locks;      /* the lock list at the mark */
    size_t created = 0;           /* created.size() at the mark */
    size_t dropped = 0;           /* dropped.size() at the mark */
    size_t dropped_seqs = 0;      /* dropped_seqs.size() at the mark */
    size_t temp_seqs_made = 0;    /* temp_seqs_made.size() at the mark */
    size_t temp_seqs_dropped = 0; /* temp_seqs_dropped.size() at the mark */
    uint64_t scan_id = 0;         /* cursors with a larger id came later */
    size_t on_commit = 0;         /* on_commit.size() at the mark */
    size_t loaded_stores = 0;     /* loaded_stores.size() at the mark */
  };
  std::vector<mark_t> savepoints;
  /*
   * The marks at depth 1..lost_marks can no longer be rolled
   * back to: the undo list passed savepoint_undo_bytes while
   * they were set, and was dropped (§6.5).
   */
  size_t lost_marks = 0;

  /*
   * Why the transaction must fail at its next call (§5.1):
   * SM_ERR_DEADLOCK once chosen as a deadlock victim,
   * SM_ERR_DOOMED once a rollback failed (§6.5), else 0.
   */
  std::atomic<int> doomed{0};
  /* The failed rollback's message, while doomed is SM_ERR_DOOMED. */
  std::string doom_msg;

  /*
   * Locks, row and OID (§5.1); changed only by the session's own
   * thread, an entry's fields under its partition's mutex, or by
   * fp_mu for an entry in a slot. lockref_t is db.h's.
   */
  using lockref_t = wisr::lockref_t;
  wisr::lock_lists_t locks; /* held, by partition */
  size_t latest_marks = 0;  /* locks with latest set; 0 skips scans */

  /*
   * The fast path (§5.1): an OID lock taken IS or IX, while no
   * strong lock is held or asked on a key that hashes with it,
   * is in a slot here and not in the lock table. Guarded by
   * fp_mu, which only the session and a strong locker's sweep
   * take; a slot's ref is its entry in locks.
   */
  static constexpr int fp_slots = 32;
  struct fp_slot_t {
    wisr::lockid_t id{};
    sm1::lockmode_t mode = sm1::SM_LOCKMODE_IS;
    wisr::lockref_t *ref = nullptr;
    bool used = false;
  };
  std::mutex fp_mu;
  fp_slot_t fp[fp_slots];
  int fp_used = 0; /* slots in use */
  /*
   * The holders blocking us when our wait last looked, empty when
   * we are not waiting. Written under the mutex our wait holds, its
   * lock's partition's or xact_mu, and read by another thread only
   * under every lock mutex (the scenario driver). It goes stale as
   * they release, so the cycle search reads the table instead
   * (lock_blockers).
   */
  std::vector<sm1::session_t *> waiting_for;
  /*
   * The lock we wait for in lock_wait, for lock_status and the
   * cycle search: its id, kind and mode. waiting_lock is false when
   * we wait for none, or behind a writer. Under the mutex of the
   * lock's partition.
   */
  bool waiting_lock = false;
  wisr::lockid_t waiting_id{};
  uint16_t waiting_kind = 0;
  sm1::lockmode_t waiting_mode = sm1::SM_LOCKMODE_SH;
  /*
   * The transaction we wait for in lock_wait_xact, or 0, and the
   * database's rollback_epoch as the wait began. Under xact_mu.
   */
  uint64_t waiting_xid = 0;
  uint64_t waiting_epoch = 0;

  wisr::ssi_node_t *ssi = nullptr; /* SERIALIZABLE only (§7) */

  /*
   * Two-phase commit (§8.4). sm1::prepare moves the transaction
   * out of the session into a database-owned sm1::session_t
   * with `prepared` set, registered under its gid; the
   * preparing session keeps only prepared_gid, and has no
   * transaction of its own until that gid is resolved.
   */
  std::string prepared_gid;
  bool prepared = false;     /* this object is a prepared transaction */
  long long prepared_at = 0; /* wall-clock seconds of sm1::prepare */
  bool restored = false;     /* rebuilt from its record at open (§9.1) */
  bool limbo = false;        /* its prepare write failed (§8.4) */

  /*
   * The session's store cache (§11): whether each storeid it
   * looked up was live, at the snapshot it was read at, by the
   * transaction (its begin_seq) that read it.
   */
  struct store_seen_t {
    uint64_t snapshot_ts = 0;
    uint64_t begin_seq = 0;
    bool live = false;
  };
  std::unordered_map<uint64_t, store_seen_t> store_cache;

  /*
   * The handles of the stores the session looked up, each with the
   * database's stores_version it was read at (STORECACHE.md):
   * refreshed in place, and erased only at a transaction's end, so
   * a reference to one stays good through a call.
   */
  struct store_handle_t {
    wisr::store_t store;
    uint64_t version = 0;
  };
  std::unordered_map<uint64_t, store_handle_t> store_handles;

  std::unordered_set<sm1::scan_t *> scans; /* open cursors */
  uint64_t next_scan_id = 0;               /* hands out sm1::scan_t::id */

  /*
   * The raw pin sm1::read and sm1::read_latest share when the
   * caller gives none, and a write's old row with it: each such
   * call lets go of the last one's row, so their row lasts until
   * the next of them, longer than the "next call on the session"
   * they promise.
   */
  sm1::rawpin_t readpin;
};

struct sm1::scan_t {
  sm1::session_t *session = nullptr;
  uint64_t id = 0;            /* opening order within the session */
  uint64_t epoch = 0;         /* read epoch held for the cursor's life */
  rocksdb::DB *rdb = nullptr; /* its store's instance */
  const rocksdb::Snapshot *snap = nullptr; /* the physical view, of rdb */
  std::unique_ptr<rocksdb::Iterator> it;
  std::string upper; /* owns iterate_upper_bound's bytes */
  rocksdb::Slice upper_slice;
  /*
   * The seek bound skey was positioned with, empty when the
   * scan has no lower bound. sm1::scan_seek refuses a target
   * below it: the iterator carries no lower bound, so a seek
   * under it would leave the range the cursor registered
   * (§10.2).
   */
  std::string lower;
  uint64_t snapshot_ts = 0; /* the logical view, fixed at open */
  uint64_t xid = 0;
  uint32_t seq_hwm = 0;
  int state = 0; /* 0 open, 1 at end, -1 failed */
  /*
   * Closed by a rollback to a savepoint set before it opened
   * (§6.5): what it held is released, and it waits only for
   * sm1::scanend to free it.
   */
  bool rolled_back = false;

  /* The scan's SIREAD range and how far it read (§7.1). */
  bool has_ssi_range = false;
  uint64_t ssi_storeid = 0;
  std::list<wisr::ssi_range_t>::iterator ssi_range;
  bool examined = false;     /* some userkey's run was inspected */
  std::string last_examined; /* the last one, the key scannxt hands out */
  /*
   * The iterator is still on the version sm1::scannxt handed out,
   * whose value it lends; the key's older versions are stepped
   * past at the next call.
   */
  bool on_row = false;
  /*
   * A cursor over a chained store's keys (sm1::chain_scan): it walks
   * the current keys, and reads a version it does not see there from
   * the old ones, at snap, into got, which then lends the row.
   */
  bool chained = false;
  rocksdb::ColumnFamilyHandle *cf = nullptr; /* a chained cursor's store */
  rocksdb::PinnableSlice got;
};

namespace wisr {

struct version_t;

/* What a read sees: the MVCC snapshot and the reader's own writes. */
struct view_t {
  uint64_t snapshot_ts;
  uint64_t xid;
  uint32_t seq_hwm;
};

/*
 * Write operations, for access_write(). WRITE_INSERT_NEW is an insert
 * of a key no version of was ever written (sm1::insert_new);
 * WRITE_MOVE a delete of a row that moved (sm1::erase_moved).
 */
enum write_op_t {
  WRITE_INSERT,
  WRITE_INSERT_NEW,
  WRITE_UPDATE,
  WRITE_DELETE,
  WRITE_MOVE
};

/* Fail with NOTRANS unless session has an active transaction. */
int xct_check_active(sm1::session_t *session);

/* Fail with READONLY if session's transaction was declared read-only. */
int xct_check_readonly(sm1::session_t *session);

/* Whether the commit cache holds xid (COMMITCACHE.md); for tests. */
bool xct_cache_has(wisr::db_t *db, uint64_t xid);

/* How many entries the commit cache holds; for tests. */
size_t xct_cache_used(wisr::db_t *db);

/* Drop session's undo list, which passed the cap; its marks are lost. */
void xct_undo_overflow(sm1::session_t *session);

/* The view a statement starting now reads at (§6.2, §10.1). */
view_t xct_view(sm1::session_t *session);

/*
 * The same, for sm1::read and sm1::scan: under a statement
 * boundary it also TAKES the statement's snapshot, if this is
 * the statement's first read (§6.2).
 */
view_t xct_read_view(sm1::session_t *session);

/*
 * Abort the active transaction on the engine's side, as for a
 * retryable error, then record e with msg. Returns -1. The
 * caller must not call sm1::abort afterwards (sm1.h).
 */
int xct_fail_retryable(sm1::session_t *session, sm1::errnum_t e,
                       const char *msg);

/* The write latch (storeid, key) hashes to (WRITELOCK.md §3.1). */
std::mutex &lock_latch(db_t *db, uint64_t storeid, rocksdb::Slice key);

/*
 * The sessions s waits for now, into out, read from the table under
 * every lock mutex, which the caller holds (lock_all_t): the
 * holders whose modes conflict with the lock s waits for, or the
 * transaction it waits for, while it runs. Empty if s waits for
 * nothing, or no longer has to.
 */
void lock_blockers(db_t *db, sm1::session_t *s,
                   std::vector<sm1::session_t *> *out);

/*
 * Every lock mutex of a database held, for as long as it lives:
 * each partition's in order, then xact_mu. What reads the whole
 * table or every waiter takes it: the deadlock search,
 * sm1::lock_status, sm1::waits_for, a prepare's and a restore's
 * moves of a transaction's locks.
 */
class lock_all_t {
public:
  explicit lock_all_t(db_t *db);
  ~lock_all_t();
  lock_all_t(const lock_all_t &) = delete;
  lock_all_t &operator=(const lock_all_t &) = delete;

private:
  db_t *m_db;
};

/*
 * Take the lock on oid in mode, by the fast path when it can
 * (§5.1): 0 once held; with nowait, 1 if another holder
 * conflicts, taking nothing; else, a wait that failed, -1 with
 * the error set (lock_wait_error).
 */
int lock_oid(sm1::session_t *session, uint64_t oid, sm1::lockmode_t mode,
             bool nowait);

/* Add the session to its database's, for a strong locker's sweep. */
void lock_register(sm1::session_t *session);

/* Take the session out of its database's, its locks all released. */
void lock_unregister(sm1::session_t *session);

/*
 * Take the lock id in mode if no other holder conflicts, recording
 * it with kind (sm1::lock_row); a holder already converts to the
 * weakest mode covering both, keeping its kind. Returns 0, or 2 if
 * another holder conflicts, taking nothing. Never waits.
 */
int lock_try(sm1::session_t *session, const lockid_t &id, sm1::lockmode_t mode,
             uint16_t kind = 0);

/*
 * For a write (WRITELOCK.md §3.2): 2 if another transaction holds
 * a lock on (storeid, key) that a write conflicts with, else 0,
 * converting the session's own lock on the key, if it holds one,
 * to exclusive. A write without a lock of its own takes none.
 */
int lock_write_check(sm1::session_t *session, uint64_t storeid,
                     rocksdb::Slice key);

/*
 * Block until no other transaction holds the lock id in a mode
 * that conflicts with mode, taking nothing, and checking for
 * deadlock (§5.1); kind is the lock's, for lock_status of the
 * wait, 0 for a write's, which reports its holder's. Returns 0; -1
 * without an error set if the
 * transaction was doomed while waiting, the caller then failing
 * it with s->doomed; 1 at once for a qa_nowait session, the
 * caller reporting SM_ERR_BLOCKED; or 2 if an interrupt to the
 * calling thread ended it (sm_sys.h), the caller reporting SM_ERR_INTERRUPTED
 * (lock_wait_error).
 */
int lock_wait(sm1::session_t *session, const lockid_t &id, sm1::lockmode_t mode,
              uint16_t kind = 0);

/*
 * Block until the transaction xid ends, or rolls back to a
 * savepoint, which may delete the version waited on (WRITELOCK.md
 * §3.4). Returns as lock_wait does.
 */
int lock_wait_xact(sm1::session_t *session, uint64_t xid);

/*
 * Wake every waiter, after a transaction ended, or after it rolled
 * back to a savepoint when rollback is true.
 */
void lock_wake(db_t *db, bool rollback);

/* §5.2's first-committer-wins check alone, for sm1::lock_row. */
int access_check_current(sm1::session_t *session, uint64_t storeid,
                         rocksdb::Slice key);

/*
 * Whether the session holds (storeid, key), and if so in which
 * mode. Reads only the session's own lock list.
 */
bool lock_held(sm1::session_t *session, uint64_t storeid, rocksdb::Slice key,
               sm1::lockmode_t *mode);

/* Mark the exclusive lock on (storeid, key): a write may go ahead. */
void lock_set_latest(sm1::session_t *session, uint64_t storeid,
                     rocksdb::Slice key);

/* Whether (storeid, key)'s lock carries the mark. */
bool lock_has_latest(sm1::session_t *session, uint64_t storeid,
                     rocksdb::Slice key);

/* Clear the mark on (storeid, key)'s lock, after a write used it. */
void lock_clear_latest(sm1::session_t *session, uint64_t storeid,
                       rocksdb::Slice key);

/*
 * Test hook: while set, every row lock's key hashes to 0, so a
 * store's row locks share one lock table entry (lockid_t).
 */
extern std::atomic<bool> lock_test_rows_collide;

/*
 * The 64-bit hash of a row lock's key, its lockid_t's lo: fixed,
 * not std::hash, so it is the same in every build (LOCKID.md §2.4).
 */
uint64_t lock_row_hash(rocksdb::Slice key);

/* The lock id of a row: (storeid, key) by its row hash. */
lockid_t lock_row_id(uint64_t storeid, rocksdb::Slice key);

/* The type of the lock id, its top bit. */
sm1::locktype_t lock_type(const lockid_t &id);

/* The storeid of the lock id: 0 for an OID lock. */
uint64_t lock_storeid(const lockid_t &id);

/* The size of a lock in a prepared transaction's record. */
constexpr size_t lock_record_size = 20;

/*
 * Append lock l to a prepared transaction's record (§8.4): its type
 * byte, be64 of each of its id's words, its mode byte, then be16 of
 * its kind, lock_record_size bytes in all.
 */
void lock_put_record(std::string *out, const sm1::session_t::lockref_t &l);

/*
 * The lock list entry the lock_record_size bytes at p stand for, into
 * l: false if they are no lock.
 */
bool lock_from_record(const char *p, sm1::session_t::lockref_t *l);

/*
 * Move every lock s holds, and its lock list, to g, the prepared
 * object (§8.4).
 */
void lock_transfer(db_t *db, sm1::session_t *s, sm1::session_t *g);

/* Put a restored transaction's locks back in the table (§9.1). */
void lock_restore(db_t *db, sm1::session_t *g);

/*
 * Fail the call for a wait that returned waited, not 0: with the
 * transaction's doom, retryable, or with SM_ERR_INTERRUPTED or
 * SM_ERR_BLOCKED, which leave the transaction active.
 */
int lock_wait_error(sm1::session_t *session, int waited);

/*
 * Refuse a row lock on a row whose newest version is the
 * transaction's own, own_seq, written after the running statement
 * began, as a write of it is refused (SM1_DESIGN.md §6.2): the
 * statement does not see that version, so may not lock it as the
 * row it read. PostgreSQL's TM_SelfModified, which a FOR UPDATE
 * cursor passes over. 0 if not, else -1 with SM_ERR_SELFMODIFIED.
 */
int lock_self_modified(sm1::session_t *session, uint32_t own_seq);

/* Release every lock the session's transaction holds. */
void lock_release_all(sm1::session_t *session);

/*
 * Release the locks the session took after m, the ones a rollback
 * to a savepoint gives back (§6.5).
 */
void lock_release_from(sm1::session_t *session, const lock_mark_t &m);

/*
 * Delete the COMMITTED status records with commit_ts <= w0, for
 * the CF:txn sweep (§8.3). Returns the number deleted, or -1.
 */
int64_t xct_drop_committed_records(wisr::db_t *db, uint64_t w0);

/* Give a SERIALIZABLE transaction its SSI node (§7). */
void ssi_begin(sm1::session_t *session);

/* Map the transaction's new xid to its SSI node. */
void ssi_on_xid(sm1::session_t *session);

/* Leave a SIREAD entry on (storeid, key) (§7.1). */
void ssi_on_read(sm1::session_t *session, uint64_t storeid, rocksdb::Slice key);

/* A read stepped past a version of concurrent writer xid (§7.2). */
void ssi_on_skip(sm1::session_t *session, uint64_t writer_xid);

/* A write of (storeid, key): edges from its concurrent readers (§7.2). */
void ssi_on_write(sm1::session_t *session, uint64_t storeid,
                  rocksdb::Slice key);

/* Register a scan's requested range as a SIREAD entry (§10.2). */
void ssi_range_open(sm1::scan_t *scan, uint64_t storeid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive);

/* Narrow a closing scan's range to what it traversed (§7.1). */
void ssi_range_close(sm1::scan_t *scan);

/*
 * The SSI commit check (§7.3) for a transaction ending at
 * end_ts. Returns 0, or -1 if it must fail with
 * SM_ERR_SERIALIZATION.
 */
int ssi_precommit(sm1::session_t *session, uint64_t end_ts);

/* Detach the session from its SSI node at transaction end. */
void ssi_end(sm1::session_t *session);

/* A prepared transaction committed at end_ts (§8.4). */
void ssi_prepared_committed(sm1::session_t *session, uint64_t end_ts);

/* A prepared transaction aborted: free its SSI state. */
void ssi_prepared_aborted(sm1::session_t *session);

/* Record xid as a transaction restored in doubt (§8.4, §9.1). */
void ssi_restore(wisr::db_t *db, uint64_t xid);

/* Free SSI nodes no active transaction overlaps (§7.1). */
void ssi_cleanup(wisr::db_t *db);

/*
 * Decode a PREPARED status record for xid into a new database-
 * owned transaction object; false if it is not one.
 */
bool xct_decode_prepared(wisr::db_t *db, uint64_t xid, rocksdb::Slice val,
                         sm1::session_t **out);

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
int xct_alloc_xid(sm1::session_t *session);

/*
 * End the bulk load the session has open, if any, deleting its
 * files: its transaction is ending without it (load.cc).
 */
void load_discard(sm1::session_t *session);

/* Whether the session has a bulk load open into storeid. */
bool load_open_on(const sm1::session_t *session, uint64_t storeid);

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
void xct_end_scans(sm1::session_t *session);

/*
 * Release what a cursor holds -- its SIREAD range, iterator,
 * snapshot and read epoch -- but not the cursor itself, which
 * stays the session's until sm1::scanend frees it.
 */
void scan_release(sm1::scan_t *scan);

/*
 * Look storeid up at view: 0 if it names a live store, 1 if
 * not, -1 on error.
 */
int catalog_lookup(sm1::session_t *session, const view_t &view,
                   uint64_t storeid);

/*
 * The store storeid's handle, as db_store() gives it, from the
 * session's own (STORECACHE.md): one with no CF if there is no
 * such store. Good until the session's transaction ends.
 */
const store_t &session_store(sm1::session_t *s, uint64_t storeid);

/*
 * Point read of key in storeid at view: 0 if a live version is
 * visible, 1 if none, -1 on error. On 0 the payload is copied
 * into *out, or, with pin given, left in the iterator the pin
 * then holds, its bytes in pin->row.
 */
int access_read(sm1::session_t *session, uint64_t storeid, const view_t &view,
                rocksdb::Slice key, std::string *out,
                sm1::rawpin_t *pin = nullptr);

/*
 * Open a cursor over storeid at view, between the bounds as
 * sm1::scan() takes them. Returns NULL on error.
 */
sm1::scan_t *access_scan(sm1::session_t *session, uint64_t storeid,
                         const view_t &view, const char *skey, size_t sklen,
                         int skey_inclusive, const char *ekey, size_t eklen,
                         int ekey_inclusive);

/*
 * Write one version of key into storeid, checking that the key
 * exists (update, delete) or does not (insert). With pin, an
 * empty one, the row the write replaces, which the existence
 * check has already found, is left in it on success. Returns 0, or -1 with
 * SM_ERR_DUPKEY / SM_ERR_NOTFOUND / another error.
 */
int access_write(sm1::session_t *session, uint64_t storeid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data, sm1::rawpin_t *pin);

/* 1 if v is visible at view, 0 if not, -1 on error (§4.4). */
int access_visible(db_t *db, const version_t &v, const view_t &view);

/* Step past v, invisible at view: under SSI, owe its writer an edge. */
int access_skip(sm1::session_t *session, const version_t &v,
                const view_t &view);

/*
 * A data operation's entry checks: a valid key, and a storeid that
 * names a live store at view. Returns 0 or -1.
 */
int access_enter(sm1::session_t *session, const view_t &view, uint64_t storeid,
                 const char *key, size_t klen);

/* Record a write for savepoint rollback, if a savepoint can reach it. */
void access_undo_add(sm1::session_t *session, sm1::session_t::undo_t undo);

/*
 * sm1::scannxt of a chained cursor: 0 with the next key it sees, 1
 * at the end, or -1 (chain.cc).
 */
int chain_scan_next(sm1::scan_t *scan, std::string_view &key,
                    std::string_view &data);

/* sm1::scan_seek of a chained cursor, its checks made (chain.cc). */
void chain_scan_seek(sm1::scan_t *scan, rocksdb::Slice target, int inclusive);

/*
 * sm1::read_newest of a chained store's key: 0 with its row in pin, 1
 * gone, 2 written by another still running, writer set, or -1.
 */
int chain_read_newest(sm1::session_t *s, const store_t &st, rocksdb::Slice key,
                      sm1::rawpin_t *pin, std::string_view &data,
                      uint64_t &writer);

/*
 * sm1::read_settled of a chained store's key: 0 with its row in pin,
 * 1 gone, 2 not settled, or -1.
 */
int chain_read_settled(sm1::session_t *s, const store_t &st, rocksdb::Slice key,
                       sm1::rawpin_t *pin, std::string_view &data);

/*
 * Put key's chain back as a rollback leaves it, the caller holding
 * the key's latch: its newest version becomes the newest not written
 * by xid above seq seq_above, and the old versions passed are
 * unlinked; with none left, the key goes. Nothing is done if its
 * newest version is not one to undo. Returns 0 or -1 (chain.cc).
 */
int chain_restore(db_t *db, const store_t &st, rocksdb::Slice key, uint64_t xid,
                  uint32_t seq_above);

} // namespace wisr

#endif /* WISR_SESSION_H */
