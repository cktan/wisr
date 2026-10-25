/*
 * chain.cc -- a chained store's keys (../VCHAIN.md): the codec, the
 * chain_ calls, the rollback to a savepoint, and GC's repair.
 *
 * A key's newest version is under its current key; a write moves it
 * to an old key under a fresh id, and the new version points to it.
 * A read Gets the current key and follows prev into the old ones
 * until it finds a version it sees. Both live in the store's one
 * column family, told apart by their class byte (version.h).
 */

#include "chain.h"

#include "error.h"
#include "log.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm0_defer.h"

#include <cassert>
#include <memory>
#include <vector>

#include <rocksdb/write_batch.h>

using namespace wisr;

namespace wisr {

/* The same version as the visibility test takes it. */
version_t chain_version_t::as_version() const {
  version_t v;
  v.wts = wts;
  v.seq = seq;
  v.flags = flags;
  v.xid = xid;
  v.commit_ts = commit_ts;
  v.payload = payload;
  return v;
}

/* Encode the header, then displaced for an old one, then the payload. */
std::string chain_value(const chain_version_t &v, bool old) {
  std::string out;
  out.reserve(1 + 8 + 8 + 4 + 8 + (old ? 8 : 0) + v.payload.size());
  out.push_back(static_cast<char>(v.flags));
  // A stamped version carries its commit_ts, an unstamped one its xid.
  sm0::put_be64(out, (v.flags & version_stamped) ? v.commit_ts : v.xid);
  sm0::put_be64(out, v.wts);
  sm0::put_be32(out, v.seq);
  sm0::put_be64(out, v.prev);
  if (old) {
    sm0::put_be64(out, v.displaced);
  }
  out.append(v.payload.data(), v.payload.size());
  return out;
}

/* Decode chain_value's encoding; false if it is cut short. */
bool chain_decode(rocksdb::Slice value, chain_version_t *v, bool old) {
  const size_t header = 1 + 8 + 8 + 4 + 8 + (old ? 8 : 0);
  if (value.size() < header) {
    return false;
  }
  const char *p = value.data();
  v->flags = static_cast<uint8_t>(p[0]);
  uint64_t who = sm0::get_be64(p + 1);
  v->xid = (v->flags & version_stamped) ? 0 : who;
  v->commit_ts = (v->flags & version_stamped) ? who : 0;
  v->wts = sm0::get_be64(p + 9);
  v->seq = sm0::get_be32(p + 17);
  v->prev = sm0::get_be64(p + 21);
  v->displaced = old ? sm0::get_be64(p + 29) : 0;
  v->payload = rocksdb::Slice(p + header, value.size() - header);
  return true;
}

/* The class byte, then the userkey. */
std::string curr_key(rocksdb::Slice userkey) {
  std::string k;
  k.reserve(1 + userkey.size());
  k.push_back(key_class_current);
  k.append(userkey.data(), userkey.size());
  return k;
}

/* The class byte, then be64(id), so old ids sort as numbers. */
std::string old_key(uint64_t id) {
  std::string k(1, key_class_old);
  sm0::put_be64(k, id);
  return k;
}

} // namespace wisr

namespace {

/*
 * Put key and value into batch, adding their bytes to bytes: what
 * the batch puts into the memtable, counted once it is written
 * (memcount.h).
 */
void batch_put(rocksdb::WriteBatch &batch, const store_t &st,
               const std::string &key, const std::string &value,
               uint64_t &bytes) {
  batch.Put(st.cf, key, value);
  bytes += key.size() + value.size();
}

/*
 * Get the old version id of the store CF cf, in rdb, into *v, its
 * bytes kept in *buf, at snap when given: 0, 1 if there is none (GC
 * dropped it: no older version), or -1.
 */
int get_old_in(rocksdb::DB *rdb, rocksdb::ColumnFamilyHandle *cf, uint64_t id,
               rocksdb::PinnableSlice *buf, chain_version_t *v,
               const rocksdb::Snapshot *snap = nullptr) {
  buf->Reset();
  rocksdb::ReadOptions ro;
  ro.snapshot = snap;
  rocksdb::Status s = rdb->Get(ro, cf, old_key(id), buf);
  if (s.IsNotFound()) {
    return 1;
  }
  if (!s.ok()) {
    return set_status_error(s, "read an old version");
  }
  if (!chain_decode(*buf, v, true)) {
    return set_error(sm1::SM_ERR_CORRUPT, "malformed old version");
  }
  return 0;
}

/* get_old_in on the chained store st, now. */
int get_old(const store_t &st, uint64_t id, rocksdb::PinnableSlice *buf,
            chain_version_t *v) {
  return get_old_in(st.rdb, st.cf, id, buf, v);
}

/*
 * Get key's newest version into *v, its bytes kept in *buf: 0, 1 if
 * the key has none, or -1.
 */
int get_curr(const store_t &st, rocksdb::Slice key, rocksdb::PinnableSlice *buf,
             chain_version_t *v) {
  buf->Reset();
  rocksdb::Status s =
      st.rdb->Get(rocksdb::ReadOptions(), st.cf, curr_key(key), buf);
  if (s.IsNotFound()) {
    return 1;
  }
  if (!s.ok()) {
    return set_status_error(s, "read a version");
  }
  if (!chain_decode(*buf, v, false)) {
    return set_error(sm1::SM_ERR_CORRUPT, "malformed version");
  }
  return 0;
}

/* The chained store storeid, or a store_t with no CF and the error set. */
store_t chained_store(sm1::session_t *s, uint64_t storeid) {
  store_t st = db_store(s->db, storeid);
  if (!st.cf) {
    set_error(sm1::SM_ERR_CORRUPT, "store {} has no column family", storeid);
    return store_t{};
  }
  if (!st.chained) {
    set_error(sm1::SM_ERR_INVALID, "store {} is not chained", storeid);
    return store_t{};
  }
  return st;
}

/*
 * What a chained write checks against (§5 step 3): the key's newest
 * version not written by a dead transaction, in CURR or in OLD.
 */
struct checked_t {
  bool found = false;     /* a version decides: own, committed, or in flight */
  bool live = false;      /* it exists and is no tombstone */
  bool moved = false;     /* it is a move's tombstone */
  uint64_t commit_ts = 0; /* its commit_ts; 0 for an own version or none */
  uint32_t own_seq = 0;   /* an own version's seq, else 0 */
  uint64_t inflight = 0;  /* another's uncommitted writer, to wait for */
  bool head_dead = false; /* CURR holds a dead transaction's version */
  uint64_t at = 0;        /* the OLD id of the version found, 0 if in CURR */
  std::vector<uint64_t> passed; /* dead OLD entries passed on the way */
  chain_version_t head;         /* CURR's version, when there is one */
  rocksdb::PinnableSlice head_buf;
  chain_version_t version; /* the version found, when in OLD */
  rocksdb::PinnableSlice version_buf;
  bool have_head = false;
};

/*
 * Walk key's chain from CURR to the version a write checks against,
 * into *c (checked_t); with skip_inflight, past another's version in
 * flight to the newest committed one, as sm1::read_latest reads.
 * Returns 0 or -1.
 */
int chain_latest(sm1::session_t *s, const store_t &st, rocksdb::Slice key,
                 checked_t *c, bool skip_inflight = false) {
  // Hold a read epoch until the statuses are resolved: the CF:txn
  // sweep waits for it.
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  int r = get_curr(st, key, &c->head_buf, &c->head);
  if (r != 0) {
    return r < 0 ? -1 : 0;
  }
  c->have_head = true;
  const chain_version_t *v = &c->head;
  for (;;) {
    // Our own version decides: a rollback puts back what it undid.
    if (s->xid != 0 && !(v->flags & version_stamped) && v->xid == s->xid) {
      assert(v->seq <= s->seq_hwm);
      c->found = true;
      c->live = !(v->flags & version_tombstone);
      c->moved = v->flags & version_moved;
      c->own_seq = v->seq;
      return 0;
    }
    uint64_t ts = v->commit_ts;
    xct_state_t state = XCT_COMMITTED;
    if (!(v->flags & version_stamped)) {
      state = xct_status(s->db, v->xid, &ts);
    }
    if (XCT_ERROR == state) {
      return -1;
    }
    // Another's uncommitted version is its lock: the caller waits.
    if (XCT_IN_FLIGHT == state && !skip_inflight) {
      c->found = true;
      c->inflight = v->xid;
      return 0;
    }
    if (XCT_COMMITTED == state) {
      c->found = true;
      c->live = !(v->flags & version_tombstone);
      c->moved = v->flags & version_moved;
      c->commit_ts = ts;
      return 0;
    }
    // A dead transaction's version, or one in flight read past: pass
    // it, to the one before.
    if (XCT_DEAD == state && v == &c->head) {
      c->head_dead = true;
    } else if (XCT_DEAD == state) {
      c->passed.push_back(c->at);
    }
    if (0 == v->prev) {
      return 0;
    }
    c->at = v->prev;
    r = get_old(st, c->at, &c->version_buf, &c->version);
    if (r < 0) {
      return -1;
    }
    if (r == 1) {
      // Unlinked and dropped: nothing older is anyone's.
      c->at = 0;
      return 0;
    }
    v = &c->version;
  }
}

/*
 * Hand the row c found, an update's or an erase's old image, to pin:
 * its buffer moves there, and the row's slice is made anew from its
 * place in it, as a buffer that copied its value moves the bytes.
 */
void pin_checked(checked_t &c, sm1::rawpin_t *pin) {
  const bool in_old = c.at != 0;
  rocksdb::PinnableSlice &buf = in_old ? c.version_buf : c.head_buf;
  const rocksdb::Slice row = in_old ? c.version.payload : c.head.payload;
  size_t offset = row.data() - buf.data();
  sm1::unpin(pin);
  pin->got = std::move(buf);
  pin->row = rocksdb::Slice(pin->got.data() + offset, row.size());
}

/*
 * Check existence, then put one new chained version (../VCHAIN.md §5).
 * With pin, the row an update or erase replaces is pinned into it once
 * the write is made; on failure pin keeps what it held.
 */
int chain_write(sm1::session_t *s, uint64_t storeid, write_op_t op,
                rocksdb::Slice key, rocksdb::Slice data, sm1::rawpin_t *pin) {
  store_t st = chained_store(s, storeid);
  if (!st.cf) {
    return -1;
  }
  // Refuse writes after fail-stop, or in a read-only transaction,
  // which may still write a temporary store.
  if (xct_check_writable(s->db) != 0 ||
      (!st.owner && xct_check_readonly(s) != 0)) {
    return -1;
  }
  // Check the key and put the version under its latch, so no other
  // write of it comes between (WRITELOCK.md §3.2).
  std::unique_lock<std::mutex> latch(lock_latch(s->db, storeid, key));
  const bool inserting = op == WRITE_INSERT || op == WRITE_INSERT_NEW;
  const bool erasing = op == WRITE_DELETE || op == WRITE_MOVE;
  std::unique_ptr<checked_t> cp;
  for (;;) {
    int waited = 0;
    // Another transaction's explicit lock that a write conflicts
    // with: wait for it, without the latch. A temporary store is
    // the session's alone.
    if (!st.owner && lock_write_check(s, storeid, key) != 0) {
      latch.unlock();
      waited =
          lock_wait(s, sm1::LOCKTYPE_ROW, storeid, key, sm1::SM_LOCKMODE_EX);
    } else if (op == WRITE_INSERT_NEW) {
      // A key no version of was ever written has none to check: no
      // read. A debug build reads, to catch a caller wrong about it.
      cp = std::make_unique<checked_t>();
#ifndef NDEBUG
      if (0 != chain_latest(s, st, key, cp.get())) {
        return -1;
      }
      assert(!cp->have_head);
#endif
      break;
    } else {
      // Look from CURR, afresh each time round.
      cp = std::make_unique<checked_t>();
      if (0 != chain_latest(s, st, key, cp.get())) {
        return -1;
      }
      // Done looking: this write goes on with the latch held.
      if (cp->inflight == 0) {
        break;
      }
      // Another transaction's uncommitted version is its lock: wait
      // for that transaction, without the latch.
      latch.unlock();
      waited = lock_wait_xact(s, cp->inflight);
    }
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
    // Look again from the start.
    latch.lock();
  }
  checked_t &c = *cp;
  // The running statement changed this row already (§6.2).
  if (!inserting && s->stmt_marked && c.own_seq > s->stmt_seq) {
    return set_error(sm1::SM_ERR_SELFMODIFIED,
                     "the row was already changed by this statement");
  }
  // Something committed after we began (§5.2).
  if (c.commit_ts > s->snapshot_ts) {
    if (s->level == sm1::xctlevel_t::read_committed) {
      if (!inserting && !lock_has_latest(s, storeid, key)) {
        // Keep the key locked until the re-read and the repeated
        // write (§5.4, WRITELOCK.md §3.5).
        lock_try(s, sm1::LOCKTYPE_ROW, storeid, key, sm1::SM_LOCKMODE_EX);
        return set_error(sm1::SM_ERR_UPDATED,
                         "key was changed by a transaction that committed "
                         "after this one began; sm1_read_latest it first");
      }
    } else {
      // First-committer-wins, aborting the loser.
      return xct_fail_retryable(
          s, sm1::SM_ERR_CONFLICT,
          "key was changed by a transaction that committed after "
          "this one began");
    }
  }
  if (inserting && c.live) {
    return set_error(sm1::SM_ERR_DUPKEY, "key already exists");
  }
  if (!inserting && !c.live) {
    return c.moved
               ? set_error(sm1::SM_ERR_MOVED, "the row moved to another key")
               : set_error(sm1::SM_ERR_NOTFOUND, "key does not exist");
  }
  // Refuse a transaction's 2^32nd write.
  if (s->seq == UINT32_MAX) {
    return set_error(sm1::SM_ERR_INVALID, "too many writes in one transaction");
  }
  // Allocate the xid at the first write (§4.1).
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  uint32_t seq = s->seq + 1;
  // No iterator opened before now serves the session's reads.
  s->write_gen++;
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  chain_version_t nv;
  nv.flags = !erasing           ? 0
             : op == WRITE_MOVE ? version_tombstone | version_moved
                                : version_tombstone;
  nv.xid = s->xid;
  nv.wts = wts;
  nv.seq = seq;
  nv.payload = erasing ? rocksdb::Slice() : data;
  rocksdb::WriteBatch batch;
  uint64_t bytes = 0;
  if (c.have_head && !c.head_dead) {
    // CURR's version moves to OLD under a fresh id, displaced by us.
    uint64_t id = st.next_old->fetch_add(1);
    chain_version_t moved = c.head;
    moved.displaced = s->xid;
    batch_put(batch, st, old_key(id), chain_value(moved, true), bytes);
    nv.prev = id;
  } else if (c.have_head) {
    // A dead version in CURR is overwritten; the version it hid,
    // if any, is displaced by us now, and the dead ones it passed
    // are no chain's. The ts they leave at keeps them for readers
    // already on their way (../VCHAIN.md §5).
    uint64_t now = 0;
    if (!c.passed.empty() && db_alloc_ts(s->db, &now) != 0) {
      return -1;
    }
    for (uint64_t id : c.passed) {
      chain_version_t gone;
      rocksdb::PinnableSlice buf;
      int r = get_old(st, id, &buf, &gone);
      if (r < 0) {
        return -1;
      }
      if (r == 0) {
        gone.flags |= chain_unlinked;
        gone.displaced = now;
        batch_put(batch, st, old_key(id), chain_value(gone, true), bytes);
      }
    }
    if (c.found && c.at != 0) {
      chain_version_t hid = c.version;
      hid.flags &= ~chain_displaced_stamped;
      hid.displaced = s->xid;
      batch_put(batch, st, old_key(c.at), chain_value(hid, true), bytes);
      nv.prev = c.at;
    } else {
      nv.prev = 0;
    }
  }
  batch_put(batch, st, curr_key(key), chain_value(nv, false), bytes);
  // Put the versions eagerly; they are not synced (§5 step 5).
  rocksdb::Status status = st.rdb->Write(store_write_options(st), &batch);
  if (!status.ok()) {
    return set_status_error(status, "write version");
  }
  // Every entry, OLD's too, counts under the row's key: OLD keys
  // carry no relid, and a table's size takes in its old rows.
  count_write(st, std::string_view(key.data(), key.size()), bytes);
  // Advance seq, and make the write visible to ourselves.
  s->seq = seq;
  s->seq_hwm = seq;
  s->wts_hi = wts;
  // The write used the re-read; the next needs its own.
  lock_clear_latest(s, storeid, key);
  // So a rollback to a savepoint can put the chain back
  // (../VCHAIN.md §6).
  access_undo_add(s, {storeid, key.ToString(), wts, seq, true});
  // The row replaced, for the caller's old image.
  if (pin && !inserting) {
    pin_checked(c, pin);
  }
  // A temporary store has no other reader, and bars a prepare.
  if (st.owner) {
    s->temp_touched = true;
    return 0;
  }
  // After the put, look for readers the write invalidates (§7.2).
  ssi_on_write(s, storeid, key);
  return 0;
}

/*
 * The checks of a chained write's arguments, then the write; with
 * pin, the row replaced pinned into it and viewed by *old.
 */
int write_chained(sm1::session_t *s, write_op_t op, uint64_t storeid,
                  std::string_view key, std::string_view data,
                  sm1::rawpin_t *pin = nullptr,
                  std::string_view *old = nullptr) {
  if (old) {
    *old = std::string_view();
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  if (data.size() > sm1::max_datalen) {
    return set_error(sm1::SM_ERR_INVALID, "value of {} bytes exceeds {}",
                     data.size(), sm1::max_datalen);
  }
  // Validate the storeid and key at the current view.
  if (access_enter(s, xct_view(s), storeid, key.data(), key.size()) != 0) {
    return -1;
  }
  int r = chain_write(s, storeid, op, rocksdb::Slice(key.data(), key.size()),
                      rocksdb::Slice(data.data(), data.size()), pin);
  if (0 == r && pin && old) {
    *old = std::string_view(pin->row.data(), pin->row.size());
  }
  return r;
}

/* sm1::chain_read_latest, minus the out-of-memory guard. */
int read_latest_chained(sm1::session_t *s, uint64_t storeid,
                        std::string_view key, sm1::rawpin_t *pin,
                        std::string_view &data) {
  data = std::string_view();
  sm1::unpin(pin);
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Only read committed has a snapshot to look past.
  if (s->level != sm1::xctlevel_t::read_committed) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires READ COMMITTED");
  }
  if (access_enter(s, xct_view(s), storeid, key.data(), key.size()) != 0) {
    return -1;
  }
  store_t st = chained_store(s, storeid);
  if (!st.cf) {
    return -1;
  }
  rocksdb::Slice k(key.data(), key.size());
  checked_t c;
  if (0 != chain_latest(s, st, k, &c, true)) {
    return -1;
  }
  // Something must keep the newest version from changing: a lock on
  // the key, or a write of our own still in flight (WRITELOCK.md §3.2).
  sm1::lockmode_t mode;
  bool held = lock_held(s, storeid, k, &mode);
  if (!held && c.own_seq == 0 && !st.owner) {
    return set_error(sm1::SM_ERR_INVALID,
                     "sm1_read_latest requires a lock held on the key, or "
                     "a write of it by this transaction");
  }
  // An exclusive holder may now write.
  if (held && mode == sm1::SM_LOCKMODE_EX) {
    lock_set_latest(s, storeid, k);
  }
  if (c.moved) {
    return set_error(sm1::SM_ERR_MOVED, "the row moved to another key");
  }
  if (!c.live) {
    return 1;
  }
  pin_checked(c, pin);
  data = std::string_view(pin->row.data(), pin->row.size());
  return 0;
}

/* sm1::chain_lock and chain_trylock, minus the out-of-memory guard. */
int lock_chained(sm1::session_t *s, uint64_t storeid, std::string_view key,
                 sm1::lockmode_t mode, bool nowait) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  if (static_cast<int>(mode) < 0 || static_cast<int>(mode) > 4) {
    return set_error(sm1::SM_ERR_INVALID, "unknown lock mode {}", (int)mode);
  }
  if (access_enter(s, xct_view(s), storeid, key.data(), key.size()) != 0) {
    return -1;
  }
  store_t st = chained_store(s, storeid);
  if (!st.cf) {
    return -1;
  }
  // A temporary store is the session's alone: there is no one to
  // lock a row against (TEMPTABLE.md §3.4).
  if (st.owner) {
    return 0;
  }
  rocksdb::Slice k(key.data(), key.size());
  uint64_t commit_ts = 0;
  for (;;) {
    uint64_t inflight;
    {
      // Under the key's latch, so no write of it comes between the
      // check and the lock (WRITELOCK.md §3.3).
      std::lock_guard<std::mutex> latch(lock_latch(s->db, storeid, k));
      checked_t c;
      if (chain_latest(s, st, k, &c) != 0) {
        return -1;
      }
      inflight = c.inflight;
      commit_ts = c.commit_ts;
      // Free of uncommitted writes: take the lock if no holder
      // conflicts.
      if (inflight == 0 &&
          lock_try(s, sm1::LOCKTYPE_ROW, storeid, k, mode) == 0) {
        break;
      }
    }
    if (nowait) {
      return 1;
    }
    // Wait, without the latch, for the uncommitted writer, or for the
    // holders that conflict.
    int waited = inflight != 0
                     ? lock_wait_xact(s, inflight)
                     : lock_wait(s, sm1::LOCKTYPE_ROW, storeid, k, mode);
    if (waited != 0) {
      return lock_wait_error(s, waited);
    }
  }
  // Read committed does not fail here: the caller reads the row with
  // sm1::chain_read_latest instead.
  if (s->level == sm1::xctlevel_t::read_committed) {
    return 0;
  }
  // The lock held, a version committed after the transaction began
  // fails it (§5.2).
  if (commit_ts > s->snapshot_ts) {
    return xct_fail_retryable(s, sm1::SM_ERR_CONFLICT,
                              "key was changed by a transaction that "
                              "committed after this one began");
  }
  return 0;
}

/*
 * The version of key a reader at view sees, into pin: 0, 1 if it
 * sees none, or -1 (../VCHAIN.md §4).
 */
int chain_get(sm1::session_t *s, const store_t &st, uint64_t storeid,
              const view_t &view, rocksdb::Slice key, sm1::rawpin_t *pin) {
  // Under SERIALIZABLE, the SIREAD entry comes before the read. A
  // temporary store takes none.
  if (!st.owner) {
    ssi_on_read(s, storeid, key);
  }
  // Hold a read epoch until the statuses are resolved.
  uint64_t epoch = db_read_enter(s->db);
  defer { db_read_leave(s->db, epoch); };
  chain_version_t v;
  int r = get_curr(st, key, &pin->got, &v);
  for (;;) {
    if (r != 0) {
      return r;
    }
    int seen = access_visible(s->db, v.as_version(), view);
    if (seen < 0) {
      return -1;
    }
    if (seen == 1) {
      if (v.flags & version_tombstone) {
        return 1;
      }
      pin->row = v.payload;
      return 0;
    }
    // Passed: under SSI, its writer is owed an edge.
    if (access_skip(s, v.as_version(), view) != 0) {
      return -1;
    }
    if (0 == v.prev) {
      return 1;
    }
    // The version before it, in OLD; gone means none is anyone's.
    r = get_old(st, v.prev, &pin->got, &v);
  }
}

/* sm1::chain_read into pin, minus the argument checks' details. */
int read_chained(sm1::session_t *s, uint64_t storeid, std::string_view key,
                 sm1::rawpin_t *pin, std::string_view &data) {
  data = std::string_view();
  if (xct_check_active(s) != 0) {
    return -1;
  }
  if (!pin) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_chain_read: pin is NULL");
  }
  // The pin lets go of what it held, as any read into it does.
  sm1::unpin(pin);
  view_t view = xct_read_view(s);
  if (access_enter(s, view, storeid, key.data(), key.size()) != 0) {
    return -1;
  }
  store_t st = chained_store(s, storeid);
  if (!st.cf) {
    return -1;
  }
  int r = chain_get(s, st, storeid, view,
                    rocksdb::Slice(key.data(), key.size()), pin);
  if (r != 0) {
    sm1::unpin(pin);
    return r;
  }
  data = std::string_view(pin->row.data(), pin->row.size());
  return 0;
}

/* sm1::chain_scan, minus the out-of-memory guard. */
sm1::scan_t *scan_chained(sm1::session_t *s, uint64_t storeid,
                          std::string_view skey, bool has_skey,
                          int skey_inclusive, std::string_view ekey,
                          bool has_ekey, int ekey_inclusive) {
  if (xct_check_active(s) != 0) {
    return nullptr;
  }
  // The view the cursor reads at, fixed from now on.
  view_t view = xct_read_view(s);
  if (access_enter(s, view, storeid, "", 0) != 0 ||
      (has_skey &&
       access_enter(s, view, storeid, skey.data(), skey.size()) != 0) ||
      (has_ekey &&
       access_enter(s, view, storeid, ekey.data(), ekey.size()) != 0)) {
    return nullptr;
  }
  store_t st = chained_store(s, storeid);
  if (!st.cf) {
    return nullptr;
  }
  std::unique_ptr<sm1::scan_t> c(new sm1::scan_t);
  c->session = s;
  c->id = ++s->next_scan_id;
  // Freeze the view: later writes stay invisible to it.
  c->snapshot_ts = view.snapshot_ts;
  c->xid = view.xid;
  c->seq_hwm = view.seq_hwm;
  c->chained = true;
  c->cf = st.cf;
  // Under SERIALIZABLE, register the range before the snapshot, so a
  // write into it after the snapshot finds the SIREAD entry (§10.2).
  // A temporary store takes none.
  if (!st.owner) {
    ssi_range_open(c.get(), storeid, has_skey ? skey.data() : nullptr,
                   skey.size(), skey_inclusive,
                   has_ekey ? ekey.data() : nullptr, ekey.size(),
                   ekey_inclusive);
  }
  // Count the cursor as a reader for its whole life (§8.3), then take
  // the RocksDB snapshot, at which CURR and OLD are both read.
  c->epoch = db_read_enter(s->db);
  c->rdb = st.rdb;
  c->snap = st.rdb->GetSnapshot();
  rocksdb::ReadOptions ro;
  ro.snapshot = c->snap;
  // A scan crosses keys: in prefix mode, a current key being its own
  // prefix, it would stop at the first (../VCHAIN.md §2).
  ro.auto_prefix_mode = true;
  // The current keys are plain: past ekey, inclusive, is ekey and a
  // 0 byte; with no ekey, the end of the current keys.
  if (has_ekey) {
    c->upper = curr_key(rocksdb::Slice(ekey.data(), ekey.size()));
    if (ekey_inclusive) {
      c->upper.push_back('\0');
    }
  } else {
    c->upper.assign(1, key_class_old);
  }
  c->upper_slice = rocksdb::Slice(c->upper);
  ro.iterate_upper_bound = &c->upper_slice;
  c->it.reset(st.rdb->NewIterator(ro, st.cf));
  if (has_skey) {
    c->lower.assign(skey.data(), skey.size());
  }
  // From skey, or from the first current key.
  chain_scan_seek(c.get(), rocksdb::Slice(c->lower),
                  !has_skey || skey_inclusive);
  s->scans.insert(c.get());
  return c.release();
}

} // namespace

namespace wisr {

/* Put key's chain back as a rollback leaves it; see session.h. */
int chain_restore(db_t *db, const store_t &st, rocksdb::Slice key, uint64_t xid,
                  uint32_t seq_above) {
  // Whether v is one of the versions to undo.
  auto undone = [&](const chain_version_t &v) {
    return !(v.flags & version_stamped) && v.xid == xid && v.seq > seq_above;
  };
  chain_version_t v;
  rocksdb::PinnableSlice buf;
  int r = get_curr(st, key, &buf, &v);
  if (r != 0) {
    return r < 0 ? -1 : 0;
  }
  // Repaired already, or overwritten: nothing to put back.
  if (!undone(v)) {
    return 0;
  }
  // Walk past the versions to undo; their OLD entries are passed.
  std::vector<uint64_t> passed;
  bool found = false;
  chain_version_t keep;
  rocksdb::PinnableSlice keep_buf;
  for (uint64_t id = v.prev; id != 0;) {
    r = get_old(st, id, &keep_buf, &keep);
    if (r < 0) {
      return -1;
    }
    if (r == 1) {
      break;
    }
    passed.push_back(id);
    if (!undone(keep)) {
      found = true;
      break;
    }
    id = keep.prev;
  }
  rocksdb::WriteBatch batch;
  uint64_t bytes = 0;
  if (found) {
    // The version kept goes back to CURR, its own prev unchanged, as
    // CURR holds it: none of OLD's flags.
    chain_version_t back = keep;
    back.flags &= ~(chain_unlinked | chain_displaced_stamped);
    back.displaced = 0;
    batch_put(batch, st, curr_key(key), chain_value(back, false), bytes);
  } else {
    // Nothing older: the key had no version before.
    std::string k = curr_key(key);
    batch.Delete(st.cf, k);
    bytes += k.size();
  }
  // The entries passed are no chain's now; the ts they leave at keeps
  // them for readers already on their way (../VCHAIN.md §3).
  if (!passed.empty()) {
    uint64_t now;
    if (db_alloc_ts(db, &now) != 0) {
      return -1;
    }
    for (uint64_t id : passed) {
      chain_version_t gone;
      rocksdb::PinnableSlice gone_buf;
      r = get_old(st, id, &gone_buf, &gone);
      if (r < 0) {
        return -1;
      }
      if (r == 0) {
        gone.flags |= chain_unlinked;
        gone.displaced = now;
        batch_put(batch, st, old_key(id), chain_value(gone, true), bytes);
      }
    }
  }
  rocksdb::Status status = st.rdb->Write(store_write_options(st), &batch);
  if (!status.ok()) {
    return set_status_error(status, "put a chained version back");
  }
  // Counted under the row's key, as a write's are.
  count_write(st, std::string_view(key.data(), key.size()), bytes);
  return 0;
}

/* Queue a chained key whose newest version is dead; see db.h. */
void chain_report(db_t *db, uint64_t storeid, rocksdb::Slice key,
                  uint64_t xid) {
  std::lock_guard<std::mutex> guard(db->gc_mu);
  db->repairs.push_back({storeid, key.ToString(), xid});
}

/* Put back each queued chained key; see db.h. */
void chain_repair(db_t *db, const std::vector<db_t::repair_t> &repairs) {
  for (const db_t::repair_t &r : repairs) {
    store_t st = db_store(db, r.storeid);
    // A store dropped since has nothing to repair.
    if (!st.chained) {
      continue;
    }
    // Under the key's latch, so no write of it comes between.
    std::lock_guard<std::mutex> latch(lock_latch(db, r.storeid, r.key));
    if (chain_restore(db, st, r.key, r.xid, 0) != 0) {
      log_msg(sm1::loglevel_t::warn,
              "database {}: a chained key could not be put back: {}",
              db->name.c_str(), sm1::errmsg());
    }
  }
}

/* The id after the store's last old version; see db.h. */
uint64_t chain_next_old(rocksdb::DB *rdb, rocksdb::ColumnFamilyHandle *cf) {
  // The old keys are the store's last: its last key is the highest
  // id, if it has any.
  std::unique_ptr<rocksdb::Iterator> it(
      rdb->NewIterator(rocksdb::ReadOptions(), cf));
  it->SeekToLast();
  if (it->Valid() && it->key().size() == 9 && it->key()[0] == key_class_old) {
    return sm0::get_be64(it->key().data() + 1) + 1;
  }
  return 1;
}

/* Position a chained cursor at target, past it if not inclusive. */
void chain_scan_seek(sm1::scan_t *c, rocksdb::Slice target, int inclusive) {
  c->on_row = false;
  std::string k = curr_key(target);
  c->it->Seek(k);
  if (!inclusive && c->it->Valid() && c->it->key() == rocksdb::Slice(k)) {
    c->it->Next();
  }
}

/* The next key a chained cursor sees, its newest version or an older one. */
int chain_scan_next(sm1::scan_t *c, const char **key, size_t *klen,
                    const char **data, size_t *datalen) {
  db_t *db = c->session->db;
  view_t view = {c->snapshot_ts, c->xid, c->seq_hwm};
  rocksdb::Iterator *it = c->it.get();
  // The key handed out last is done with: step off it.
  if (c->on_row) {
    c->on_row = false;
    it->Next();
  }
  for (; it->Valid(); it->Next()) {
    // How far the scan has read, for SIREAD narrowing; also the key
    // handed out.
    c->examined = true;
    c->last_examined.assign(it->key().data() + 1, it->key().size() - 1);
    chain_version_t v;
    if (!chain_decode(it->value(), &v, false)) {
      c->state = -1;
      return set_error(sm1::SM_ERR_CORRUPT, "malformed version");
    }
    // Walk the key's chain to the version the cursor sees.
    for (;;) {
      int r = access_visible(db, v.as_version(), view);
      if (r < 0) {
        c->state = -1;
        return -1;
      }
      if (1 == r) {
        // A tombstone hides the key; a row is handed out, lent by the
        // iterator or by got until the next call.
        if (v.flags & version_tombstone) {
          break;
        }
        c->on_row = true;
        *key = c->last_examined.data();
        *klen = c->last_examined.size();
        *data = v.payload.data();
        *datalen = v.payload.size();
        return 0;
      }
      if (0 != access_skip(c->session, v.as_version(), view)) {
        c->state = -1;
        return -1;
      }
      if (0 == v.prev) {
        break;
      }
      // The version before it, as the snapshot has it.
      r = get_old_in(c->rdb, c->cf, v.prev, &c->got, &v, c->snap);
      if (r < 0) {
        c->state = -1;
        return -1;
      }
      if (1 == r) {
        break;
      }
    }
  }
  if (!it->status().ok()) {
    c->state = -1;
    return set_status_error(it->status(), "scan");
  }
  c->state = 1;
  return 1;
}

} // namespace wisr

/* Open a cursor over a chained store's keys; see sm1.h. */
int sm1::chain_scan(sm1::session_t *session, uint64_t storeid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive, sm1::scan_t *&out) {
  sm1::scan_t *c = scan_chained(
      session, storeid, std::string_view(skey ? skey : "", sklen), skey,
      skey_inclusive, std::string_view(ekey ? ekey : "", eklen), ekey,
      ekey_inclusive);
  if (!c) {
    return -1;
  }
  out = c;
  return 0;
}

/* Whether a store is chained; see sm1.h. */
int sm1::store_chained(sm1::session_t *session, uint64_t storeid, bool &out) {
  out = false;
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_store_chained: session is NULL");
  }
  // A store's layout is fixed when it is made: no snapshot is needed.
  store_t st = db_store(session->db, storeid);
  if (!st.cf || (st.owner && st.owner != session)) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  out = st.chained;
  return 0;
}

/* Insert a key into a chained store; see sm1.h. */
int sm1::chain_insert(sm1::session_t *session, uint64_t storeid,
                      std::string_view key, std::string_view data) {
  return write_chained(session, WRITE_INSERT, storeid, key, data);
}

/* Insert a chained key never written before; see sm1.h. */
int sm1::chain_insert_new(sm1::session_t *session, uint64_t storeid,
                          std::string_view key, std::string_view data) {
  return write_chained(session, WRITE_INSERT_NEW, storeid, key, data);
}

/* Replace a chained key's value; see sm1.h. */
int sm1::chain_update(sm1::session_t *session, uint64_t storeid,
                      std::string_view key, std::string_view data) {
  return write_chained(session, WRITE_UPDATE, storeid, key, data);
}

/* Replace a chained key's value, pinning the row replaced; see sm1.h. */
int sm1::chain_update(sm1::session_t *session, uint64_t storeid,
                      std::string_view key, std::string_view data,
                      sm1::rawpin_t *pin, std::string_view &old) {
  return write_chained(session, WRITE_UPDATE, storeid, key, data, pin, &old);
}

/* Delete a chained key; see sm1.h. */
int sm1::chain_erase(sm1::session_t *session, uint64_t storeid,
                     std::string_view key) {
  return write_chained(session, WRITE_DELETE, storeid, key, {});
}

/* Delete a chained key, pinning the row deleted; see sm1.h. */
int sm1::chain_erase(sm1::session_t *session, uint64_t storeid,
                     std::string_view key, sm1::rawpin_t *pin,
                     std::string_view &old) {
  return write_chained(session, WRITE_DELETE, storeid, key, {}, pin, &old);
}

/* Delete a chained key whose row moved to another; see sm1.h. */
int sm1::chain_erase_moved(sm1::session_t *session, uint64_t storeid,
                           std::string_view key, sm1::rawpin_t *pin,
                           std::string_view &old) {
  return write_chained(session, WRITE_MOVE, storeid, key, {}, pin, &old);
}

/* Read a chained key's newest committed version; see sm1.h. */
int sm1::chain_read_latest(sm1::session_t *session, uint64_t storeid,
                           std::string_view key, sm1::rawpin_t *pin,
                           std::string_view &data) {
  return read_latest_chained(session, storeid, key, pin, data);
}

/* The same, into the session's own pin; see sm1.h. */
int sm1::chain_read_latest(sm1::session_t *session, uint64_t storeid,
                           std::string_view key, std::string_view &data) {
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  return read_latest_chained(session, storeid, key, &session->readpin, data);
}

/* Estimate the bytes of a chained store's keys in [lo, hi); see sm1.h. */
int sm1::chain_approxsize(sm1::session_t *session, uint64_t storeid,
                          const char *lo, size_t lolen, const char *hi,
                          size_t hilen, uint64_t &disk, uint64_t &mem,
                          bool old) {
  disk = mem = 0;
  if (!session) {
    return xct_check_active(session);
  }
  store_t st = db_store(session->db, storeid);
  if (!st.cf || (st.owner && st.owner != session)) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  if (!st.chained) {
    return set_error(sm1::SM_ERR_INVALID, "store {} is not chained", storeid);
  }
  // The current keys in [lo, hi), with no bound the class's ends; then
  // with old, every old key.
  std::string start = lo ? curr_key(rocksdb::Slice(lo, lolen))
                         : std::string(1, key_class_current);
  std::string limit =
      hi ? curr_key(rocksdb::Slice(hi, hilen)) : std::string(1, key_class_old);
  std::string old_start(1, key_class_old);
  std::string old_limit(1, static_cast<char>(key_class_old + 1));
  rocksdb::Range ranges[2] = {{start, limit}, {old_start, old_limit}};
  int n = old ? 2 : 1;
  rocksdb::SizeApproximationOptions so;
  so.include_memtables = false;
  so.include_files = true;
  uint64_t sizes[2] = {0, 0};
  rocksdb::Status s = st.rdb->GetApproximateSizes(so, st.cf, ranges, n, sizes);
  if (!s.ok()) {
    return set_status_error(s, "approximate store size");
  }
  for (int i = 0; i < n; i++) {
    uint64_t count = 0, bytes = 0;
    disk += sizes[i];
    st.rdb->GetApproximateMemTableStats(st.cf, ranges[i], &count, &bytes);
    mem += bytes;
  }
  return 0;
}

/* Lock a chained key without writing it; see sm1.h. */
int sm1::chain_lock(sm1::session_t *session, uint64_t storeid,
                    std::string_view key, sm1::lockmode_t mode) {
  return lock_chained(session, storeid, key, mode, false);
}

/* Lock a chained key if it can be had at once; see sm1.h. */
int sm1::chain_trylock(sm1::session_t *session, uint64_t storeid,
                       std::string_view key, sm1::lockmode_t mode) {
  return lock_chained(session, storeid, key, mode, true);
}

/* Read a chained key into pin; see sm1.h. */
int sm1::chain_read(sm1::session_t *session, uint64_t storeid,
                    std::string_view key, sm1::rawpin_t *pin,
                    std::string_view &data) {
  return read_chained(session, storeid, key, pin, data);
}

/* Read a chained key into the session's own pin; see sm1.h. */
int sm1::chain_read(sm1::session_t *session, uint64_t storeid,
                    std::string_view key, std::string_view &data) {
  if (!session) {
    data = std::string_view();
    return xct_check_active(session);
  }
  return read_chained(session, storeid, key, &session->readpin, data);
}
