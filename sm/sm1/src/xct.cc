/*
 * xct.cc -- sm1::begin, sm1::commit, sm1::abort, and the commit
 * status table CF:txn (SM1_DESIGN.md §3.2, §3.4, §6, §8.2).
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
 * sm1::prepare moves the transaction into a database-owned object
 * registered under its gid, holding its locks, snapshot, and
 * SSI state; any session can then commit or abort it by gid,
 * and recovery rebuilds the same object from the PREPARED
 * record (§8.4, §9.1).
 */

#include "error.h"
#include "log.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <rocksdb/write_batch.h>

using namespace wisr;

namespace {

/* CF:txn states (§3.2). */
constexpr uint8_t txn_prepared = 1;
constexpr uint8_t txn_committed = 2;
constexpr uint8_t txn_aborted = 3;

/* Status record size: state, snapshot_ts, commit_ts. */
constexpr size_t txn_record_size = 1 + 8 + 8;

int finish_commit(sm1::session_t *s, uint64_t commit_ts);
void publish_commit(sm1::session_t *s, uint64_t xid,
                    uint64_t commit_ts) noexcept;
int resolve(db_t *db, const std::string &gid, bool commit_it);

/*
 * The on_commit functions of the transaction this thread has just
 * committed, kept by the commit for its entry point to run once it
 * has left sm1.
 */
thread_local std::vector<std::function<void()>> committed_fns;

/*
 * Run what the commit kept, newest first, as the caller's code:
 * outside sm1 and outside any transaction.
 */
void run_committed_fns() {
  // Take them first: one may commit a transaction of its own.
  std::vector<std::function<void()>> fns;
  fns.swap(committed_fns);
  for (auto fn = fns.rbegin(); fn != fns.rend(); ++fn) {
    (*fn)();
  }
}

/*
 * Resolve the session's own prepared transaction, from its
 * sm1::commit or sm1::abort. Another session may resolve it first,
 * even between the caller's check and this call; the session
 * then simply has no transaction, as if it had seen that.
 */
int resolve_own(sm1::session_t *s, bool commit_it) {
  // Take the gid; the session no longer refers to it.
  std::string gid = s->prepared_gid;
  s->prepared_gid.clear();
  // Resolve the transaction by gid.
  if (resolve(s->db, gid, commit_it) != 0) {
    // Resolved elsewhere first: the session simply has none.
    if (sm1::errnum() == sm1::SM_ERR_NOTFOUND) {
      return set_error(sm1::SM_ERR_NOTRANS, "no active transaction");
    }
    return -1;
  }
  return 0;
}

/*
 * Whether the session's prepared transaction is still in doubt.
 * Once some session has resolved it, the session forgets the gid
 * and has no transaction at all.
 */
bool pending_prepared(sm1::session_t *s) {
  // No prepared transaction on this session.
  if (s->prepared_gid.empty()) {
    return false;
  }
  // Hold prepared_mu while reading the registry.
  std::lock_guard<std::mutex> guard(s->db->prepared_mu);
  // Still registered: still in doubt.
  if (s->db->prepared.count(s->prepared_gid)) {
    return true;
  }
  // Resolved elsewhere: forget the gid.
  s->prepared_gid.clear();
  return false;
}

/* The CF:txn key of xid. */
std::string txn_key(uint64_t xid) {
  std::string k;
  // The xid, big-endian, so records sort by xid.
  sm0::put_be64(k, xid);
  return k;
}

/* Remove xid from the active table: its versions now resolve dead. */
void leave_active(db_t *db, uint64_t xid) {
  {
    // Hold txn_mu while editing the active table.
    std::lock_guard<std::mutex> guard(db->txn_mu);
    // Its versions now resolve through CF:txn, or as dead.
    db->active.erase(xid);
    db->xid_owner.erase(xid);
  }
  // Writers waiting for it look again (WRITELOCK.md §3.4).
  lock_wake(db, false);
}

/* The first of the two slots of xid's set in the commit cache. */
db_t::cache_slot_t *cache_set(db_t *db, uint64_t xid) {
  // xids are sparse -- one counter hands out every timestamp --
  // so mix the bits before taking the set.
  uint64_t h = xid * 0x9e3779b97f4a7c15ULL;
  return &db->cache[2 * ((h >> 32) % db->cache_sets)];
}

/*
 * Put xid's commit_ts in the commit cache, with txn_mu held. It
 * takes the slot in its set that holds the smaller xid, so a
 * lookup of an old transaction never pushes out the newer entry
 * of the two (COMMITCACHE.md §3.4). Eviction cannot change an
 * answer: the status record was written before any entry is made.
 */
void cache_put_locked(db_t *db, uint64_t xid, uint64_t commit_ts) noexcept {
  db_t::cache_slot_t *set = cache_set(db, xid);
  // Already cached, by a commit or by another reader: keep it.
  if (set[0].xid == xid || set[1].xid == xid) {
    return;
  }
  // The slot of the smaller xid, an empty one, holding 0, first.
  db_t::cache_slot_t *victim = set[0].xid <= set[1].xid ? &set[0] : &set[1];
  victim->xid = xid;
  victim->commit_ts = commit_ts;
}

/* The commit_ts cached for xid, with txn_mu held; false if none. */
bool cache_get_locked(db_t *db, uint64_t xid, uint64_t *commit_ts) {
  db_t::cache_slot_t *set = cache_set(db, xid);
  for (int i = 0; i < 2; i++) {
    if (set[i].xid == xid) {
      *commit_ts = set[i].commit_ts;
      return true;
    }
  }
  return false;
}

/*
 * Move xid from the active table into the commit cache. It never
 * throws: running out of memory aborts the process (env.cc's
 * new-handler).
 */
void enter_cache(db_t *db, uint64_t xid, uint64_t commit_ts) noexcept {
  {
    // Hold txn_mu, so the xid moves atomically.
    std::lock_guard<std::mutex> guard(db->txn_mu);
    // No longer in flight.
    db->active.erase(xid);
    db->xid_owner.erase(xid);
    // Cache its commit_ts by xid.
    cache_put_locked(db, xid, commit_ts);
  }
  // Writers waiting for it look again (WRITELOCK.md §3.4).
  lock_wake(db, false);
}

/*
 * Forget the transaction; the session has none afterwards. Its
 * snapshot stops holding back gc_watermark.
 */
void reset(sm1::session_t *s) {
  // Stop holding back the GC watermark.
  if (s->snapshot_registered) {
    // Unregister the snapshot.
    db_release_snapshot(s->db, s->snapshot_ts);
    s->snapshot_registered = false;
  }
  // No active transaction, and none of its state.
  s->active = false;
  s->xid = 0;
  s->seq = 0;
  s->seq_hwm = 0;
  s->wts_hi = 0;
  s->created.clear();
  s->dropped.clear();
  s->dropped_seqs.clear();
  s->temp_touched = false;
  s->loaded = false;
  s->loaded_stores.clear();
  s->temp_seqs_made.clear();
  s->temp_seqs_dropped.clear();
  s->undo.clear();
  s->undo_bytes = 0;
  // Its on_commit functions go with it, run or not.
  s->on_commit.clear();
  // The savepoints end with the transaction.
  s->savepoints.clear();
  s->lost_marks = 0;
  // So does the statement snapshot (§6.2).
  s->stmt_pinned = false;
  s->stmt_taken = false;
  s->stmt_snapshot_ts = 0;
  s->stmt_marked = false;
  s->stmt_seq = 0;
}

/*
 * Drop the column families this transaction created, newest
 * first (§6.6). A failure leaves an orphan CF with no catalog
 * row, which the startup sweep (§9.1 step 6) is meant to catch.
 */
void drop_created(sm1::session_t *s) {
  // Drop each created CF, newest first.
  for (auto it = s->created.rbegin(); it != s->created.rend(); ++it) {
    db_drop_store_cf(s->db, *it);
    // A temporary one is the session's no longer.
    s->temp_stores.erase(*it);
  }
}

/*
 * Drop the temporary stores the committing transaction dropped
 * (TEMPTABLE.md §3.2): its cursors are closed, and no other session
 * could read them. Its dropped temporary sequences go with them.
 */
void drop_temp_dropped(sm1::session_t *s) noexcept {
  for (uint64_t storeid : s->dropped) {
    if (s->temp_stores.erase(storeid)) {
      db_drop_store_cf(s->db, storeid);
    }
  }
  for (const std::string &name : s->temp_seqs_dropped) {
    s->temp_seqs.erase(name);
  }
}

/*
 * Take out the temporary sequences the transaction made from the
 * from'th on, newest first, as it ends without committing or rolls
 * back past them.
 */
void drop_temp_seqs_made(sm1::session_t *s, size_t from) {
  while (s->temp_seqs_made.size() > from) {
    s->temp_seqs.erase(s->temp_seqs_made.back());
    s->temp_seqs_made.pop_back();
  }
}

/*
 * End the transaction without committing (§6.6): close its
 * cursors, take it out of the active table so its versions
 * resolve dead, drop the stores it created, and release its
 * locks.
 */
void discard(sm1::session_t *s) {
  // A load still open ends here, and its files with it.
  load_discard(s);
  // Close the transaction's cursors.
  xct_end_scans(s);
  ssi_end(s); // an aborted node constrains nobody: freed
  // Take a writer out of the active table: its versions die.
  if (s->xid != 0) {
    leave_active(s->db, s->xid);
  }
  // Drop the stores it created, and its temporary sequences.
  drop_created(s);
  drop_temp_seqs_made(s, 0);
  // Release its locks.
  lock_release_all(s);
  // Forget the transaction.
  reset(s);
}

/* sm1::stmt_begin, minus the out-of-memory guard. */
int stmt_begin(sm1::session_t *s) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Turn statement snapshots on, and end any statement running.
  s->stmt_pinned = true;
  // The next read takes the new statement's snapshot (§6.2).
  s->stmt_taken = false;
  s->stmt_snapshot_ts = 0;
  // Its reads see the transaction's own writes made before now.
  s->stmt_marked = true;
  s->stmt_seq = s->seq;
  return 0;
}
/*
 * The mark at depth, or NULL with the error set. Depth counts
 * from 1 at the outermost savepoint (§6.5).
 */
sm1::session_t::mark_t *mark_at(sm1::session_t *s, int depth) {
  // Reject a depth outside the stack.
  if (depth < 1 || static_cast<size_t>(depth) > s->savepoints.size()) {
    set_error(sm1::SM_ERR_NOSAVEPOINT,
              "no savepoint at depth {}; the stack holds {}", depth,
              s->savepoints.size());
    return nullptr;
  }
  return &s->savepoints[depth - 1];
}

/* Free the session's undo list, not just empty it: it may be large. */
void undo_free(sm1::session_t *s) {
  std::vector<sm1::session_t::undo_t>().swap(s->undo);
  s->undo_bytes = 0;
}

/* sm1::savepoint, minus the out-of-memory guard. */
int savepoint(sm1::session_t *s) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // A load is whole: a mark inside it could not undo its keys
  // after the mark apart from those before (BULKLOAD.md §3.5).
  if (s->loader) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a savepoint cannot be set with a bulk load open");
  }
  // Refuse to nest deeper than the limit.
  if (s->savepoints.size() >= sm1::max_savepoints) {
    return set_error(sm1::SM_ERR_INVALID, "savepoint stack is full at {}",
                     sm1::max_savepoints);
  }
  // Mark the current positions; the mark goes on top.
  sm1::session_t::mark_t m;
  m.undo = s->undo.size();
  m.seq_hwm = s->seq_hwm;
  m.created = s->created.size();
  m.dropped = s->dropped.size();
  m.dropped_seqs = s->dropped_seqs.size();
  m.temp_seqs_made = s->temp_seqs_made.size();
  m.temp_seqs_dropped = s->temp_seqs_dropped.size();
  m.scan_id = s->next_scan_id;
  m.on_commit = s->on_commit.size();
  m.loaded_stores = s->loaded_stores.size();
  {
    // Hold lock_mu while reading the lock list.
    std::lock_guard<std::mutex> guard(s->db->lock_mu);
    // Mark the lock list.
    m.locks = s->locks.size();
  }
  s->savepoints.push_back(m);
  // The new mark's depth.
  return static_cast<int>(s->savepoints.size());
}

/* sm1::forget, minus the out-of-memory guard. */
int forget(sm1::session_t *s, int depth) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Require a mark at that depth.
  if (!mark_at(s, depth)) {
    return -1;
  }
  // Drop it and every mark inside it; the writes stay, and so
  // do the locks (§6.5).
  s->savepoints.resize(static_cast<size_t>(depth) - 1);
  // A lost mark that is forgotten is no longer counted.
  s->lost_marks = std::min(s->lost_marks, s->savepoints.size());
  // With no mark left that can be rolled back to, no rollback
  // can reach the writes listed.
  if (s->savepoints.size() == s->lost_marks) {
    undo_free(s);
  }
  return 0;
}

/*
 * The range [first, end) that holds every key of store: 0, 1 if it
 * holds none, or -1 with the error set.
 */
int store_range(const store_t &store, std::string &first, std::string &end) {
  std::unique_ptr<rocksdb::Iterator> it(
      store.rdb->NewIterator(rocksdb::ReadOptions(), store.cf));
  it->SeekToFirst();
  if (it->Valid()) {
    first = it->key().ToString();
    it->SeekToLast();
  }
  if (!it->Valid()) {
    return it->status().ok()
               ? 1
               : set_status_error(it->status(), "roll back to savepoint");
  }
  // Just past the last key: no key sorts between it and itself
  // with a zero byte added.
  end = it->key().ToString();
  end.push_back('\0');
  return 0;
}

/* sm1::rollback, minus the out-of-memory guard. */
int rollback(sm1::session_t *s, int depth) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // A load is whole, and its loader is its caller's to close
  // (BULKLOAD.md §3.5).
  if (s->loader) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a savepoint cannot be rolled back to with a bulk "
                     "load open");
  }
  // Require a mark at that depth.
  sm1::session_t::mark_t *mp = mark_at(s, depth);
  if (!mp) {
    return -1;
  }
  // Refuse a mark whose undo list was dropped at the cap: the
  // versions to delete are no longer known (§6.5).
  if (static_cast<size_t>(depth) <= s->lost_marks) {
    return set_error(sm1::SM_ERR_UNDOLIMIT,
                     "savepoint {} can no longer be rolled back to: the "
                     "transaction passed savepoint_undo_bytes",
                     depth);
  }
  // Copy it: the stack is truncated before it is used below.
  const sm1::session_t::mark_t m = *mp;
  // The shared database object.
  db_t *db = s->db;

  // Delete exactly the versions written since the mark, in one
  // batch for each instance that holds some (TEMPTABLE.md §3.1). If
  // one fails the marks and the undo list stay as they were, so the
  // rollback can be asked for again: deleting a version twice is
  // harmless.
  std::map<rocksdb::DB *, rocksdb::WriteBatch> batches;
  // Queue a delete for each version written after the mark.
  for (size_t i = s->undo.size(); i-- > m.undo;) {
    // The next undo entry, newest first.
    const sm1::session_t::undo_t &u = s->undo[i];
    // Find its store.
    store_t store = db_store(db, u.storeid);
    // Skip a CF already dropped.
    if (store.cf) {
      // Delete the exact version: key, its wts, its seq.
      std::string vkey = version_key(u.key, u.wts, u.seq);
      batches[store.rdb].Delete(store.cf, vkey);
      // The tombstone is in the memtable, written or not below: a
      // failed write fails the transaction, which aborts.
      count_write(store, u.key, vkey.size());
    }
  }
  // Empty each store loaded since the mark: a load lists no key,
  // and the store holds only what the transaction wrote since it
  // opened the load (BULKLOAD.md §3.5). One made since the mark is
  // dropped below instead.
  for (size_t i = m.loaded_stores; i < s->loaded_stores.size(); i++) {
    uint64_t storeid = s->loaded_stores[i];
    if (std::find(s->created.begin() + m.created, s->created.end(), storeid) !=
        s->created.end()) {
      continue;
    }
    store_t store = db_store(db, storeid);
    std::string first, end;
    int rc = store.cf ? store_range(store, first, end) : 1;
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      batches[store.rdb].DeleteRange(store.cf, first, end);
      rocksdb::Slice userkey = version_userkey(first);
      count_write(store, std::string_view(userkey.data(), userkey.size()),
                  first.size() + end.size());
    }
  }
  // Apply each instance's deletes in one write.
  for (auto &kv : batches) {
    // The temporary instance is never logged.
    rocksdb::WriteOptions wo;
    wo.disableWAL = kv.first == db->tmp;
    rocksdb::Status st = kv.first->Write(wo, &kv.second);
    // Fail with the marks and the undo list unchanged.
    if (!st.ok()) {
      return set_status_error(st, "roll back to savepoint");
    }
  }

  // Close the cursors opened after the mark: their snapshot
  // and frozen seq_hwm would still admit the undone versions.
  std::vector<sm1::scan_t *> later;
  // Walk the open cursors.
  for (sm1::scan_t *c : s->scans) {
    // Opened after the mark.
    if (c->id > m.scan_id) {
      later.push_back(c);
    }
  }
  // Close each one.
  for (sm1::scan_t *c : later) {
    sm1::scanend(c);
  }

  // Drop the stores created after the mark, newest first.
  for (size_t i = s->created.size(); i-- > m.created;) {
    // Drop the CF.
    db_drop_store_cf(db, s->created[i]);
    // A temporary one is the session's no longer.
    s->temp_stores.erase(s->created[i]);
  }
  // Forget those stores.
  s->created.resize(m.created);
  s->dropped.resize(m.dropped);           // the undo restored their rows
  s->dropped_seqs.resize(m.dropped_seqs); // and their sequences
  // Temporary sequences made after the mark go; dropped ones stay.
  drop_temp_seqs_made(s, m.temp_seqs_made);
  s->temp_seqs_dropped.resize(m.temp_seqs_dropped);
  // Drop the on_commit functions added after the mark.
  s->on_commit.resize(m.on_commit);
  // The loads since the mark are undone.
  s->loaded_stores.resize(m.loaded_stores);

  // Stop counting the entries undone.
  for (size_t i = m.undo; i < s->undo.size(); i++) {
    s->undo_bytes -= sizeof(sm1::session_t::undo_t) + s->undo[i].key.size();
  }
  // Rewind seq with seq_hwm, so the own versions still in
  // effect are exactly seqs 1..seq_hwm (§6.5).
  s->undo.resize(m.undo);
  // The last write still in effect: the one before the mark,
  // which the list does not hold if no savepoint was set then.
  s->seq_hwm = m.seq_hwm;
  // The next write reuses the undone seqs.
  s->seq = s->seq_hwm;
  // A statement's mark must stay below them: they are its own
  // writes again (§6.2).
  s->stmt_seq = std::min(s->stmt_seq, s->seq);

  // Give back the locks taken after the mark; nobody saw our
  // intent on those rows.
  lock_release_from(s, m.locks);
  // Writers waiting for this transaction look again: the
  // versions they waited on may be gone (WRITELOCK.md §3.8).
  lock_wake(db, true);
  // The mark survives, so it can be rolled back to again; the
  // marks set inside it are gone with the work they covered.
  s->savepoints.resize(static_cast<size_t>(depth));
  return 0;
}

/* sm1::begin, minus the out-of-memory guard. */
int begin(sm1::session_t *s, sm1::xctlevel_t level, bool read_only) {
  // Reject a NULL session.
  if (!s) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_begin: session is NULL");
  }
  // Refuse a second transaction.
  if (s->active) {
    return set_error(sm1::SM_ERR_INTRANS, "a transaction is already active");
  }
  // Refuse while this session's prepared transaction is in doubt.
  if (pending_prepared(s)) {
    return set_error(sm1::SM_ERR_INTRANS,
                     "the session's transaction is prepared as {}",
                     s->prepared_gid.c_str());
  }
  // Reject an unknown isolation level.
  if (level != sm1::xctlevel_t::read_committed &&
      level != sm1::xctlevel_t::repeatable_read &&
      level != sm1::xctlevel_t::serializable) {
    return set_error(sm1::SM_ERR_INVALID, "unknown isolation level {}",
                     (int)level);
  }
  // Clear any state of an earlier transaction.
  reset(s);
  // Record the level, and whether it may write.
  s->level = level;
  s->read_only = read_only;
  // Record begin order, for deadlock victim choice.
  s->begin_seq = ++s->db->begin_seq;
  // A new transaction starts undoomed.
  s->doomed = 0;
  // Take the snapshot: the current visible_ts (§4.1). Read
  // committed also counts in the watermark at this one, for
  // its whole life (§8.3).
  s->snapshot_ts = db_take_snapshot(s->db);
  // The snapshot now counts in the watermark.
  s->snapshot_registered = true;
  // Give a SERIALIZABLE transaction its SSI node.
  ssi_begin(s);
  // The transaction is active.
  s->active = true;
  return 0;
}

/* sm1::commit, minus the out-of-memory guard. */
int commit(sm1::session_t *s) {
  // A prepared transaction on this session: resolve it by gid.
  if (s && !s->active && pending_prepared(s)) {
    // The coordinator's commit decision, on the preparing session.
    return resolve_own(s, true);
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // A load's rows are not in the database until it is closed.
  if (s->loader) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a transaction with a bulk load open cannot commit");
  }
  // Close cursors first, releasing their snapshots and
  // narrowing their SIREAD ranges before validation (§8.2).
  xct_end_scans(s);
  // The shared database object.
  db_t *db = s->db;
  // No xid: the transaction never wrote.
  if (s->xid == 0) {
    // Read-only: nothing to publish. Under SSI it ended just
    // after the current visible_ts (§7.3).
    if (ssi_precommit(s, db->visible_ts.load() + 1) != 0) {
      // Not serializable: fail, and the engine aborts it.
      return xct_fail_retryable(s, sm1::SM_ERR_SERIALIZATION,
                                "not serializable");
    }
    // Keep the committed SSI node for its overlaps.
    ssi_end(s);
    // Release its locks: a transaction that never wrote can
    // still hold them, through sm1::lock (§5.1).
    lock_release_all(s);
    // Keep its on_commit functions for the entry point to run.
    committed_fns.swap(s->on_commit);
    // It may have dropped a temporary store, which writes nothing.
    drop_temp_dropped(s);
    // Forget the transaction.
    reset(s);
    return 0;
  }

  uint64_t commit_ts;
  // Refuse after fail-stop, else claim commit_ts from the sequencer.
  if (xct_check_writable(db) != 0 || db_claim_commit_ts(db, &commit_ts) != 0) {
    // No record was written, so the transaction is dead.
    discard(s);
    return -1;
  }
  // Validate under SSI before the commit point; on failure give
  // the commit_ts back to the sequencer unused (§7.3, §8.2).
  if (ssi_precommit(s, commit_ts) != 0) {
    // Hand the commit_ts back to the sequencer unused.
    db_commit_done(db, commit_ts);
    return xct_fail_retryable(s, sm1::SM_ERR_SERIALIZATION, "not serializable");
  }
  // Write the record, publish, and release.
  return finish_commit(s, commit_ts);
}

/*
 * Commit a transaction that holds a claimed commit_ts and has
 * passed validation: write the status record, then resolve,
 * publish, and release (§8.2 steps 3-6). Shared by sm1::commit and
 * the commit of a prepared transaction, whose record this
 * overwrites.
 */
int finish_commit(sm1::session_t *s, uint64_t commit_ts) {
  db_t *db = s->db;
  // Keep the xid past reset.
  uint64_t xid = s->xid;
  // Build the COMMITTED record -- state, snapshot, commit_ts --
  // and its key on the stack.
  char val[txn_record_size];
  val[0] = static_cast<char>(txn_committed);
  uint64_t be = sm0::to_be64(s->snapshot_ts);
  memcpy(val + 1, &be, sizeof(be));
  be = sm0::to_be64(commit_ts);
  memcpy(val + 1 + sizeof(be), &be, sizeof(be));
  char key[sizeof(uint64_t)];
  be = sm0::to_be64(xid);
  memcpy(key, &be, sizeof(be));
  // No error unless the hook or the write says so.
  rocksdb::Status st;
  // Let a test stall or fail the status write.
  if (db->test_before_commit) {
    st = db->test_before_commit(commit_ts);
  }
  // Write the record unless the hook failed it.
  if (st.ok()) {
    // Sync it, unless durability was traded away.
    rocksdb::WriteOptions wo;
    wo.sync = db->durability_sync != 0;
    // Write the status record: the commit point (§8.2).
    st = db->rdb->Put(wo, db->cf_txn, rocksdb::Slice(key, sizeof(key)),
                      rocksdb::Slice(val, sizeof(val)));
  }
  // The record's fate is unknown.
  if (!st.ok()) {
    // Fail stop: the record may have reached the WAL, so the
    // transaction stays in flight -- in the active table,
    // its commit_ts holding back visible_ts -- until a
    // reopen resolves it from the log (§8.2). Its locks go:
    // the database refuses writes from now on anyway.
    db->failed = true;
    log_msg(sm1::loglevel_t::error,
            "database {}: commit record write failed, database stopped "
            "until reopened: {}",
            db->name.c_str(), st.ToString().c_str());
    // Release the locks: writes are refused from now on.
    lock_release_all(s);
    // Forget the transaction.
    reset(s);
    return set_error(sm1::SM_ERR_IO,
                     "write commit record (outcome unknown): {}",
                     st.ToString().c_str());
  }
  // Keep its on_commit functions for the entry point to run.
  committed_fns.swap(s->on_commit);
  // Past the commit point: publish, and report success.
  publish_commit(s, xid, commit_ts);
  return 0;
}

/*
 * Everything after the commit point (§8.2 steps 4-6). The record
 * is written, so the transaction has committed whatever happens
 * here: nothing may throw, or sm1::commit would report a failure
 * for a committed transaction and a caller's sm1::abort would drop
 * the stores it created. Nothing does: running out of memory
 * aborts the process (env.cc's new-handler), and the next open's WAL
 * replay finds the commit.
 */
void publish_commit(sm1::session_t *s, uint64_t xid,
                    uint64_t commit_ts) noexcept {
  db_t *db = s->db;
  // Resolve before publishing: a reader that misses the xid in
  // the active table finds it in the cache or CF:txn (§3.4).
  enter_cache(db, xid, commit_ts);
  // Publish: advance visible_ts past this commit.
  db_commit_done(db, commit_ts);
  // Release the locks only once the commit is visible, and
  // queue dropped stores for removal once no snapshot can
  // see them (§8.3).
  lock_release_all(s);
  // Drop its dropped temporary stores now; the queue finds them gone.
  drop_temp_dropped(s);
  // Queue its dropped stores for GC.
  gc_queue_drops(db, commit_ts, s->dropped);
  // Drop each dropped sequence's lease; nothing reads it now
  // that its catalog row is gone.
  for (uint64_t seqid : s->dropped_seqs) {
    db_forget_seq(db, seqid);
  }
  ssi_end(s); // the committed node stays while others overlap it
  // Forget the transaction.
  reset(s);
}

/*
 * sm1::abort, minus the out-of-memory guard. It skips the doomed
 * check: a doomed transaction can still be aborted (§5.1). A
 * session whose transaction is prepared aborts it by gid.
 */
int abort_xct(sm1::session_t *s) {
  // Reject a NULL session.
  if (!s) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_abort: session is NULL");
  }
  // No active transaction of its own.
  if (!s->active) {
    // Its prepared transaction: abort it by gid.
    if (pending_prepared(s)) {
      return resolve_own(s, false);
    }
    return set_error(sm1::SM_ERR_NOTRANS, "no active transaction");
  }
  // Abort: discard every trace.
  discard(s);
  return 0;
}

/*
 * The PREPARED record (§3.2) of t's transaction under gid, prepared
 * at prepared_at: state, snapshot_ts, a zero commit_ts, then
 * prepared_at, gid, the write set as lock keys, and the storeids
 * created and dropped. One value, however large the write set;
 * RocksDB stores it as a blob.
 */
std::string encode_prepared(const sm1::session_t *t, const std::string &gid,
                            long long prepared_at) {
  // Build the PREPARED record.
  std::string v;
  // The state byte.
  v.push_back(static_cast<char>(txn_prepared));
  // The snapshot, for the GC watermark after a restart.
  sm0::put_be64(v, t->snapshot_ts);
  // No commit_ts yet.
  sm0::put_be64(v, 0);
  // When it was prepared.
  sm0::put_be64(v, static_cast<uint64_t>(prepared_at));
  // The gid, length-prefixed.
  sm0::put_be32(v, static_cast<uint32_t>(gid.size()));
  v.append(gid);
  // The lock set: every lock key, length-prefixed, each
  // followed by the mode byte it is held in (§8.4).
  sm0::put_be32(v, static_cast<uint32_t>(t->locks.size()));
  for (const sm1::session_t::lockref_t &l : t->locks) {
    sm0::put_be32(v, static_cast<uint32_t>(l.key.size()));
    v.append(l.key);
    v.push_back(static_cast<char>(l.mode));
  }
  // The storeids created, then those dropped, each counted.
  for (const std::vector<uint64_t> *stores : {&t->created, &t->dropped}) {
    sm0::put_be32(v, static_cast<uint32_t>(stores->size()));
    for (uint64_t storeid : *stores) {
      sm0::put_be64(v, storeid);
    }
  }
  return v;
}

/*
 * Move the session's transaction into a new database-owned
 * object: its state, snapshot registration, SSI node, and row
 * locks, whose table entries are re-pointed at the new owner.
 * The session is left with no transaction. All or nothing: every
 * allocation comes before anything moves, so if one throws the
 * session still holds its transaction, untouched.
 */
sm1::session_t *detach_prepared(sm1::session_t *s, const std::string &gid,
                                long long prepared_at) {
  // Allocate the database-owned transaction object, freed if a
  // copy below throws.
  std::unique_ptr<sm1::session_t> owner(new sm1::session_t);
  sm1::session_t *g = owner.get();
  // Copy the transaction's identity and state.
  g->db = s->db;
  g->active = true;
  g->prepared = true;
  g->level = s->level;
  g->snapshot_ts = s->snapshot_ts;
  g->xid = s->xid;
  g->seq = s->seq;
  g->seq_hwm = s->seq_hwm;
  g->created = s->created;
  g->dropped = s->dropped;
  g->begin_seq = s->begin_seq;
  // Name it by gid.
  g->prepared_gid = gid;
  // Stamp the wall-clock time of the prepare.
  g->prepared_at = prepared_at;
  // Nothing below allocates: take ownership, then move.
  owner.release();
  // Move the snapshot registration over.
  g->snapshot_registered = s->snapshot_registered;
  s->snapshot_registered = false;
  // Move the SSI node over.
  g->ssi = s->ssi;
  s->ssi = nullptr;
  // Its on_commit functions go with it, for whoever commits it.
  g->on_commit.swap(s->on_commit);
  // Hand every held lock, in its mode, to the new owner.
  lock_transfer(s->db, s, g);
  if (g->xid != 0) {
    // Hold txn_mu while re-pointing the xid at its new owner.
    std::lock_guard<std::mutex> guard(s->db->txn_mu);
    // A writer waiting for it now waits on the prepared object.
    s->db->xid_owner[g->xid] = g;
  }
  {
    // Hold lock_mu while moving the list the table points into.
    std::lock_guard<std::mutex> guard(s->db->lock_mu);
    // Move the lock list.
    g->locks.swap(s->locks);
    s->db->lock_cv.notify_all(); // waiters re-read their holders
  }
  // Leave the session with no transaction.
  reset(s);
  return g;
}

/*
 * Register g under gid, which prepare reserved: the entry exists,
 * so this finds it rather than inserting, and allocates nothing.
 * It runs after the PREPARED record is written (§8.4).
 */
void register_prepared(db_t *db, sm1::session_t *g,
                       const std::string &gid) noexcept {
  // Hold prepared_mu while registering.
  std::lock_guard<std::mutex> guard(db->prepared_mu);
  // Fill in the reservation.
  db->prepared.find(gid)->second = g;
}

/* sm1::prepare, minus the out-of-memory guard. */
int prepare(sm1::session_t *s, const char *gid) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Validate the gid.
  if (check_name(gid, sm1::max_gid, "gid") != 0) {
    return -1;
  }
  db_t *db = s->db;
  // Refuse after fail-stop.
  if (xct_check_writable(db) != 0) {
    return -1;
  }
  // A prepared transaction outlives the session; its temporary
  // stores would not (TEMPTABLE.md §3.5).
  if (s->temp_touched) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a transaction that made, dropped or wrote a "
                     "temporary store cannot be prepared");
  }
  // A PREPARED record names the keys written, and a load keeps
  // none (BULKLOAD.md §5.6).
  if (s->loader || s->loaded) {
    return set_error(sm1::SM_ERR_INVALID,
                     "a transaction that bulk loaded cannot be prepared");
  }
  // Own the gid before reserving it: releasing the reservation,
  // and everything after the record is written, must not
  // allocate.
  std::string key(gid);
  // Reserve the gid: unique among unresolved prepared ones.
  {
    // Hold prepared_mu while reserving.
    std::lock_guard<std::mutex> guard(db->prepared_mu);
    // Refuse a gid already in use.
    if (db->prepared.count(key)) {
      return set_error(sm1::SM_ERR_EXISTS, "gid {} is already in use", gid);
    }
    // Reserve it with a null entry.
    db->prepared[key] = nullptr;
  }
  // Release the reservation if the prepare fails before the
  // transaction is detached; dismissed once it is.
  sm0::scope_guard_t unreserve([&] {
    std::lock_guard<std::mutex> guard(db->prepared_mu);
    db->prepared.erase(key);
  });

  // Close cursors, then validate: the last moment the engine
  // may still say no (§8.4). A passed node counts as committed.
  xct_end_scans(s);
  // Validate; a passed node counts as committed.
  if (ssi_precommit(s, UINT64_MAX) != 0) {
    return xct_fail_retryable(s, sm1::SM_ERR_SERIALIZATION, "not serializable");
  }
  // A read-only transaction needs an xid to key its record.
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }

  // Build the record and its key while the transaction is still
  // the session's: a failure here leaves it there to abort.
  long long prepared_at = static_cast<long long>(time(nullptr));
  std::string val = encode_prepared(s, key, prepared_at);
  std::string txnkey = txn_key(s->xid);
  // Move the transaction out of the session, all or nothing.
  sm1::session_t *g = detach_prepared(s, key, prepared_at);
  // From here the entry is replaced, whether the write succeeds or
  // fails stop: keep the reservation.
  unreserve.dismiss();
  // No error unless the hook or the write says so.
  rocksdb::Status st;
  // Let a test fail the prepare write.
  if (db->test_before_prepare) {
    st = db->test_before_prepare();
  }
  // Write the record unless the hook failed it.
  if (st.ok()) {
    rocksdb::WriteOptions wo;
    wo.sync = true; // a prepare is always synced (§8.4)
    // Write the PREPARED record: the vote of yes.
    st = db->rdb->Put(wo, db->cf_txn, txnkey, val);
  }
  // The record's fate is unknown: mark it before registering, so
  // no resolver ever sees it resolvable.
  if (!st.ok()) {
    db->failed = true;
    g->limbo = true;
  }
  // Register the transaction under its gid.
  register_prepared(db, g, key);
  // The record's fate is unknown.
  if (!st.ok()) {
    // Fail stop: the record may be in the WAL, so the
    // transaction stays in flight, unlisted and unresolvable,
    // until a reopen decides (§8.4).
    log_msg(sm1::loglevel_t::error,
            "database {}: prepare record write for {} failed, database "
            "stopped until reopened: {}",
            db->name.c_str(), gid, st.ToString().c_str());
    return set_error(sm1::SM_ERR_IO,
                     "write prepare record (outcome unknown): {}",
                     st.ToString().c_str());
  }
  // The session remembers only the gid; a move cannot allocate.
  s->prepared_gid = std::move(key);
  return 0;
}

/* Commit a prepared transaction g (§8.4: §8.2 from step 2). */
int commit_prepared_txn(sm1::session_t *g) {
  uint64_t commit_ts;
  // Refuse after fail-stop, else claim commit_ts.
  if (xct_check_writable(g->db) != 0 ||
      db_claim_commit_ts(g->db, &commit_ts) != 0) {
    return -1;
  }
  // Validated at prepare: the node only learns its end.
  ssi_prepared_committed(g, commit_ts);
  // Write COMMITTED over PREPARED, publish, and release.
  return finish_commit(g, commit_ts);
}

/*
 * Abort a prepared transaction g: supersede its PREPARED record
 * durably with ABORTED, so a crash cannot restore it, then
 * discard it as an ordinary abort would.
 */
int abort_prepared_txn(sm1::session_t *g) {
  db_t *db = g->db;
  // Refuse after fail-stop.
  if (xct_check_writable(db) != 0) {
    return -1;
  }
  // Build the ABORTED record.
  std::string val;
  val.push_back(static_cast<char>(txn_aborted));
  sm0::put_be64(val, g->snapshot_ts);
  sm0::put_be64(val, 0);
  // Sync it, so the PREPARED record cannot come back.
  rocksdb::WriteOptions wo;
  wo.sync = true;
  // Write the ABORTED record: the coordinator's decision.
  rocksdb::Status st = db->rdb->Put(wo, db->cf_txn, txn_key(g->xid), val);
  // The record's fate is unknown: fail stop.
  if (!st.ok()) {
    db->failed = true;
    log_msg(sm1::loglevel_t::error,
            "database {}: abort record write for {} failed, database "
            "stopped until reopened: {}",
            db->name.c_str(), g->prepared_gid.c_str(), st.ToString().c_str());
    return set_error(sm1::SM_ERR_IO, "write abort record: {}",
                     st.ToString().c_str());
  }
  // Free its SSI node and restored entry.
  ssi_prepared_aborted(g);
  // Discard it as an ordinary abort would.
  discard(g);
  return 0;
}

/*
 * End a resolution of g under gid: put g back in the entry resolve
 * held for it if it is still unresolved, else drop the entry and
 * free g. The entry is still there, so this allocates nothing.
 */
void unhold_prepared(db_t *db, const std::string &gid,
                     sm1::session_t *g) noexcept {
  // Still in doubt after a failure: it stays registered.
  bool keep = g->active;
  {
    // Hold prepared_mu while editing the registry.
    std::lock_guard<std::mutex> guard(db->prepared_mu);
    // The held entry: only its resolver removes it.
    auto it = db->prepared.find(gid);
    if (keep) {
      // Put it back.
      it->second = g;
    } else {
      // Resolved: the gid is free again.
      db->prepared.erase(it);
    }
  }
  // Resolved: free its object.
  if (!keep) {
    delete g;
  }
}

/*
 * Resolve the prepared transaction gid (§8.5): hold its entry
 * null, so a concurrent resolver finds nothing and no prepare can
 * take the gid, commit or abort it, then put it back or drop it.
 * Holding the entry rather than erasing it is what lets a failed
 * resolution put the transaction back without allocating.
 */
int resolve(db_t *db, const std::string &gid, bool commit_it) {
  sm1::session_t *g = nullptr;
  {
    // Hold prepared_mu while taking it from the registry.
    std::lock_guard<std::mutex> guard(db->prepared_mu);
    // Find the gid.
    auto it = db->prepared.find(gid);
    // Unknown, still being prepared or resolved, or in limbo:
    // nothing to do.
    if (it == db->prepared.end() || !it->second || it->second->limbo) {
      return set_error(sm1::SM_ERR_NOTFOUND,
                       "no unresolved prepared transaction {}", gid.c_str());
    }
    // Take the transaction, holding its entry null.
    g = it->second;
    it->second = nullptr;
  }
  // Commit or abort it.
  int rc = commit_it ? commit_prepared_txn(g) : abort_prepared_txn(g);
  // Put it back if still unresolved; otherwise drop and free it.
  unhold_prepared(db, gid, g);
  return rc;
}

/* sm1::list_prepared, minus the out-of-memory guard. */
int list_prepared(sm1::session_t *s, std::vector<sm1::prepared_t> &out) {
  // Require a session.
  if (!s) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_list_prepared: session is NULL");
  }
  // Collect the listable transactions.
  std::vector<sm1::prepared_t> rows;
  {
    // Hold prepared_mu while reading the registry.
    std::lock_guard<std::mutex> guard(s->db->prepared_mu);
    for (auto &kv : s->db->prepared) {
      // Skip reservations and transactions in limbo.
      if (kv.second && !kv.second->limbo) {
        rows.push_back({kv.first, kv.second->prepared_at});
      }
    }
  }
  // Hand the list to the caller.
  out = std::move(rows);
  return 0;
}

} // namespace

namespace wisr {

/* READONLY if the transaction was declared read-only. */
int xct_check_readonly(sm1::session_t *s) {
  // Declared read-only at sm1::begin: refuse the write.
  if (s->read_only) {
    return set_error(sm1::SM_ERR_READONLY,
                     "the transaction was declared read-only");
  }
  return 0;
}

/* NOTRANS unless a transaction is active. */
int xct_check_active(sm1::session_t *s) {
  // Reject a NULL session.
  if (!s) {
    return set_error(sm1::SM_ERR_INVALID, "session is NULL");
  }
  // No active transaction of its own.
  if (!s->active) {
    // A prepared one in doubt: data calls are refused.
    if (pending_prepared(s)) {
      return set_error(sm1::SM_ERR_PREPARED,
                       "the transaction is prepared as {}",
                       s->prepared_gid.c_str());
    }
    return set_error(sm1::SM_ERR_NOTRANS, "no active transaction");
  }
  // A doomed transaction fails here, before doing anything.
  if (s->doomed) {
    return xct_fail_retryable(s, sm1::SM_ERR_DEADLOCK,
                              "chosen as a deadlock victim");
  }
  return 0;
}

/* Abort on the engine's side, then report the retryable error. */
int xct_fail_retryable(sm1::session_t *s, sm1::errnum_t e, const char *msg) {
  // Abort the transaction on the engine's side.
  discard(s);
  return set_error(e, "{}; the transaction was aborted", msg);
}

/* Read committed takes a statement snapshot; the others reuse the begin one. */
view_t xct_view(sm1::session_t *s) {
  view_t v;
  // RC takes a fresh snapshot per statement; RR and SER keep theirs.
  v.snapshot_ts = s->level == sm1::xctlevel_t::read_committed
                      ? s->db->visible_ts.load()
                      : s->snapshot_ts;
  // Once a statement has its snapshot, RC reads at that one.
  if (s->level == sm1::xctlevel_t::read_committed && s->stmt_taken) {
    v.snapshot_ts = s->stmt_snapshot_ts;
  }
  // Its own writes, up to the last still in effect, and within
  // a statement only those made before it began.
  v.xid = s->xid;
  v.seq_hwm = s->stmt_marked ? std::min(s->seq_hwm, s->stmt_seq) : s->seq_hwm;
  return v;
}

/*
 * The view a read runs at, taking the statement's snapshot if
 * the statement has not read yet (§6.2). Only reads take it:
 * a write consults its view to validate the storeid and key, and
 * pinning there would hold a statement's later reads at a
 * snapshot from before the write waited for another writer.
 */
view_t xct_read_view(sm1::session_t *s) {
  // A statement running without its snapshot: this read takes it.
  if (s->level == sm1::xctlevel_t::read_committed && s->stmt_pinned &&
      !s->stmt_taken) {
    s->stmt_snapshot_ts = s->db->visible_ts.load();
    s->stmt_taken = true;
  }
  return xct_view(s);
}

/* Fail with SM_ERR_IO once the database has failed stop. */
int xct_check_writable(db_t *db) {
  // Refuse writes once a status write has failed.
  if (db->failed) {
    return set_error(sm1::SM_ERR_IO,
                     "database {} failed a status write; reopen it",
                     db->name.c_str());
  }
  return 0;
}

/* Allocate the xid and enter the active table, before any version. */
int xct_alloc_xid(sm1::session_t *s) {
  uint64_t xid;
  // Allocate the xid from the timestamp counter.
  if (db_alloc_ts(s->db, &xid) != 0) {
    return -1;
  }
  {
    // Hold txn_mu while entering the active table.
    std::lock_guard<std::mutex> guard(s->db->txn_mu);
    // In flight from now on, before any version exists.
    s->db->active.insert(xid);
    // Run by this session, for a writer that waits for it.
    s->db->xid_owner[xid] = s;
  }
  // Record the xid.
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
xct_state_t xct_status(db_t *db, uint64_t xid, uint64_t *commit_ts) {
  {
    // Hold txn_mu while checking the active table and cache.
    std::lock_guard<std::mutex> guard(db->txn_mu);
    // Still in flight, or prepared.
    if (db->active.count(xid)) {
      return XCT_IN_FLIGHT;
    }
    // Committed recently: the cache has its commit_ts.
    if (cache_get_locked(db, xid, commit_ts)) {
      return XCT_COMMITTED;
    }
  }
  std::string val;
  // Look up the status record.
  rocksdb::Status s =
      db->rdb->Get(rocksdb::ReadOptions(), db->cf_txn, txn_key(xid), &val);
  // No record: it never committed.
  if (s.IsNotFound()) {
    return XCT_DEAD;
  }
  // Fail on a read error.
  if (!s.ok()) {
    set_status_error(s, "read CF:txn");
    return XCT_ERROR;
  }
  // Reject a record too short to hold its state.
  if (val.size() < txn_record_size) {
    set_error(sm1::SM_ERR_CORRUPT, "short status record for xid {}", xid);
    return XCT_ERROR;
  }
  // Decide by the record's state.
  switch (static_cast<uint8_t>(val[0])) {
  case txn_committed: {
    *commit_ts = sm0::get_be64(val.data() + 9);
    // Hold txn_mu while adding to the cache.
    std::lock_guard<std::mutex> guard(db->txn_mu);
    // Remember it: the transaction's other versions, and the
    // next reader's, then cost no read of CF:txn. A commit is
    // final, so the entry can never become wrong (§3.4).
    cache_put_locked(db, xid, *commit_ts);
    return XCT_COMMITTED;
  }
  case txn_aborted:
    return XCT_DEAD;
  default:
    return XCT_IN_FLIGHT;
  }
}

/* Scan CF:txn and delete, in one batch, the records the sweep covers. */
int64_t xct_drop_committed_records(db_t *db, uint64_t w0) {
  // Collect the deletes into one batch.
  rocksdb::WriteBatch batch;
  // Scan every status record.
  std::unique_ptr<rocksdb::Iterator> it(
      db->rdb->NewIterator(rocksdb::ReadOptions(), db->cf_txn));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    // Each record's value.
    rocksdb::Slice v = it->value();
    // Its state byte.
    uint8_t state = v.empty() ? 0 : static_cast<uint8_t>(v[0]);
    // ABORTED records always go; COMMITTED ones at or below W0.
    bool swept = state == txn_aborted ||
                 (state == txn_committed && v.size() >= txn_record_size &&
                  sm0::get_be64(v.data() + 9) <= w0);
    // Delete a swept record.
    if (swept) {
      batch.Delete(db->cf_txn, it->key());
    }
  }
  // Fail if the scan failed.
  if (!it->status().ok()) {
    set_status_error(it->status(), "scan CF:txn");
    return -1;
  }
  // Apply the deletes in one write.
  rocksdb::Status s = db->rdb->Write(rocksdb::WriteOptions(), &batch);
  // Fail if they did not apply.
  if (!s.ok()) {
    set_status_error(s, "delete swept status records");
    return -1;
  }
  return batch.Count();
}

/*
 * Decode a PREPARED record into a new database-owned transaction
 * object: false if val is not one, or is malformed.
 */
bool xct_decode_prepared(db_t *db, uint64_t xid, rocksdb::Slice val,
                         sm1::session_t **out) {
  // Reject anything but a PREPARED record long enough to decode.
  if (val.size() < txn_record_size + 8 + 4 ||
      static_cast<uint8_t>(val[0]) != txn_prepared) {
    return false;
  }
  // Allocate the database-owned transaction object.
  std::unique_ptr<sm1::session_t> g(new sm1::session_t);
  // An active, prepared transaction of this database.
  g->db = db;
  g->active = true;
  g->prepared = true;
  // Its xid is the record's key.
  g->xid = xid;
  // Its snapshot, for the GC watermark.
  g->snapshot_ts = sm0::get_be64(val.data() + 1);
  // Walk the record's variable part.
  const char *p = val.data() + txn_record_size;
  const char *end = val.data() + val.size();
  // When it was prepared.
  g->prepared_at = static_cast<long long>(sm0::get_be64(p));
  p += 8;
  // Read a 4-byte count, checking bounds.
  auto take32 = [&](uint32_t *x) {
    if (end - p < 4) {
      return false;
    }
    *x = sm0::get_be32(p);
    p += 4;
    return true;
  };
  uint32_t len, count;
  // Read the gid's length and check it fits.
  if (!take32(&len) || (size_t)(end - p) < len) {
    return false;
  }
  // Take the gid.
  g->prepared_gid.assign(p, len);
  p += len;
  // Read the write set's size.
  if (!take32(&count)) {
    return false;
  }
  // Read each lock key.
  for (uint32_t i = 0; i < count; i++) {
    // Check each key's length fits.
    if (!take32(&len) || (size_t)(end - p) < len) {
      return false;
    }
    // Take the lock key.
    sm1::session_t::lockref_t l;
    l.key.assign(p, len);
    p += len;
    // Take the mode byte that follows it.
    if (end - p < 1) {
      return false;
    }
    uint8_t m = static_cast<uint8_t>(*p++);
    if (m > 4) {
      return false;
    }
    l.mode = static_cast<sm1::lockmode_t>(m);
    g->locks.push_back(std::move(l));
  }
  // Read the storeids created, then dropped.
  for (std::vector<uint64_t> *stores : {&g->created, &g->dropped}) {
    // Check the count fits.
    if (!take32(&count) || (size_t)(end - p) < (size_t)count * 8) {
      return false;
    }
    // Take each storeid.
    for (uint32_t i = 0; i < count; i++) {
      stores->push_back(sm0::get_be64(p));
      p += 8;
    }
  }
  // Hand the object to the caller.
  *out = g.release();
  return true;
}

/* xct_status, folded to committed or not. */
int xct_resolve(db_t *db, uint64_t xid, uint64_t *commit_ts) {
  // Fold the three-way status into committed or not.
  switch (xct_status(db, xid, commit_ts)) {
  case XCT_COMMITTED:
    return 1;
  case XCT_ERROR:
    return -1;
  default:
    return 0;
  }
}

/* Whether the commit cache holds xid. */
bool xct_cache_has(db_t *db, uint64_t xid) {
  std::lock_guard<std::mutex> guard(db->txn_mu);
  uint64_t ts;
  return cache_get_locked(db, xid, &ts);
}

/* How many entries the commit cache holds. */
size_t xct_cache_used(db_t *db) {
  std::lock_guard<std::mutex> guard(db->txn_mu);
  size_t n = 0;
  for (const db_t::cache_slot_t &slot : db->cache) {
    n += slot.xid != 0;
  }
  return n;
}

/* Drop session's undo list, which passed the cap; its marks are lost. */
void xct_undo_overflow(sm1::session_t *s) {
  // Tell the operator: a rollback to these savepoints will fail.
  log_msg(sm1::loglevel_t::warn,
          "database {}: transaction {} passed savepoint_undo_bytes ({}) with "
          "{} savepoint(s) set; they can no longer be rolled back to",
          s->db->name.c_str(), s->xid, s->db->undo_max_bytes,
          s->savepoints.size() - s->lost_marks);
  // Give the memory back.
  undo_free(s);
  // Every mark now set is lost; one set later starts a new list.
  s->lost_marks = s->savepoints.size();
}

} // namespace wisr

/* Begin a statement inside another; see sm1.h. */
int sm1::stmt_begin(sm1::session_t *session, sm1::stmt_t &outer) {
  if (xct_check_active(session) != 0) {
    return -1;
  }
  // Keep the outer statement, for sm1::stmt_end to put back.
  outer.pinned = session->stmt_pinned;
  outer.taken = session->stmt_taken;
  outer.snapshot_ts = session->stmt_snapshot_ts;
  outer.marked = session->stmt_marked;
  outer.seq = session->stmt_seq;
  return ::stmt_begin(session);
}

/* End a statement begun inside another; see sm1.h. */
int sm1::stmt_end(sm1::session_t *session, const sm1::stmt_t &outer) {
  if (xct_check_active(session) != 0) {
    return -1;
  }
  // Resume the outer statement's snapshot and mark.
  session->stmt_pinned = outer.pinned;
  session->stmt_taken = outer.taken;
  session->stmt_snapshot_ts = outer.snapshot_ts;
  session->stmt_marked = outer.marked;
  // Never above the writes still in effect: a rollback since may
  // have rewound seq below the outer mark (§6.2).
  session->stmt_seq = std::min(outer.seq, session->seq);
  return 0;
}

namespace wisr {

/* Close every open cursor of the session. */
void xct_end_scans(sm1::session_t *s) {
  // Close cursors until none is left.
  while (!s->scans.empty()) {
    sm1::scanend(*s->scans.begin());
  }
}

} // namespace wisr

/* Begin a transaction at level, declared read-only or not. */
int sm1::begin(sm1::session_t *session, sm1::xctlevel_t level, bool read_only) {
  return ::begin(session, level, read_only);
}

/* Commit the active transaction, then run its on_commit functions. */
int sm1::commit(sm1::session_t *session) {
  int rc;
  { rc = ::commit(session); }
  run_committed_fns();
  return rc;
}

/* Have fn run once the active transaction has committed. */
int sm1::on_commit(sm1::session_t *session, std::function<void()> fn) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(session) != 0) {
    return -1;
  }
  // Reject an empty function.
  if (!fn) {
    return set_error(sm1::SM_ERR_INVALID, "on_commit: no function");
  }
  // Last added runs first.
  session->on_commit.push_back(std::move(fn));
  return 0;
}

/* Read the rest of this statement at one snapshot. */
int sm1::stmt_begin(sm1::session_t *session) { return ::stmt_begin(session); }

/* QA only: make the session's lock requests fail where they would wait. */
int sm1::set_qa_nowait(sm1::session_t *session, bool on) {
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "NULL session");
  }
  session->qa_nowait = on;
  return 0;
}

/* Set the session's one savepoint. */
int sm1::savepoint(sm1::session_t *session) { return ::savepoint(session); }

/* Undo every write made since the savepoint at depth. */
int sm1::rollback(sm1::session_t *session, int depth) {
  return ::rollback(session, depth);
}

/* Forget the savepoint at depth, and those inside it. */
int sm1::forget(sm1::session_t *session, int depth) {
  return ::forget(session, depth);
}

/* Prepare the active transaction under gid. */
int sm1::prepare(sm1::session_t *session, std::string_view gid_arg) {
  // Take the name as a C string, as the engine uses it.
  std::string gid_str;
  if (copy_name(gid_arg, "gid", gid_str) != 0) {
    return -1;
  }
  const char *gid = gid_str.c_str();
  return ::prepare(session, gid);
}

/* List the unresolved prepared transactions. */
int sm1::list_prepared(sm1::session_t *session,
                       std::vector<sm1::prepared_t> &out) {
  return ::list_prepared(session, out);
}

/* sm1::commit_prepared, minus the out-of-memory guard. */
static int commit_prepared_gid(sm1::session_t *session,
                               std::string_view gid_arg) {
  // Take the name as a C string, as the engine uses it.
  std::string gid_str;
  if (copy_name(gid_arg, "gid", gid_str) != 0) {
    return -1;
  }
  const char *gid = gid_str.c_str();
  // Require a session and a valid gid.
  if (!session || check_name(gid, sm1::max_gid, "gid") != 0) {
    return session ? -1 : set_error(sm1::SM_ERR_INVALID, "session is NULL");
  }
  // Commit it by gid.
  return resolve(session->db, gid, true);
}

/* Commit a prepared transaction by gid, then run its on_commit functions. */
int sm1::commit_prepared(sm1::session_t *session, std::string_view gid_arg) {
  int rc;
  { rc = commit_prepared_gid(session, gid_arg); }
  run_committed_fns();
  return rc;
}

/* Abort a prepared transaction by gid, from any session. */
int sm1::abort_prepared(sm1::session_t *session, std::string_view gid_arg) {
  // Take the name as a C string, as the engine uses it.
  std::string gid_str;
  if (copy_name(gid_arg, "gid", gid_str) != 0) {
    return -1;
  }
  const char *gid = gid_str.c_str();
  // Require a session and a valid gid.
  if (!session || check_name(gid, sm1::max_gid, "gid") != 0) {
    return session ? -1 : set_error(sm1::SM_ERR_INVALID, "session is NULL");
  }
  // Abort it by gid.
  return resolve(session->db, gid, false);
}

/* Abort the active transaction. */
int sm1::abort(sm1::session_t *session) { return abort_xct(session); }

/* Whether the session's prepared transaction is still unresolved. */
bool sm1::in_doubt(sm1::session_t *session) {
  return session != nullptr && !session->active && pending_prepared(session);
}
