/*
 * seq.cc -- named sequences: sm1::mkseq, sm1::rmseq, sm1::nextseq,
 * sm1::lastseq (SM1_DESIGN.md §11).
 *
 * A sequence's definition is a catalog row, keyed "S" | name in
 * CF:store.0 and holding { seqid, start, increment }, so creating
 * and dropping one is transactional like any other DDL. Its
 * seqid comes from the object-id lease, the same one storeids
 * come from, so a name reused after a drop names a new
 * sequence that starts over.
 *
 * The sequence's position is NOT in that row: sm1::nextseq takes
 * no locks, never blocks, and is not undone by an abort, so its
 * state lives in CF:meta under a lease (§4.2), outside MVCC.
 */

#include "error.h"
#include "session.h"
#include "sm0_codec.h"

#include <algorithm>
#include <new>
#include <string>

using namespace wisr;

namespace {

/* One sequence's catalog row. */
struct seqdef_t {
  uint64_t seqid;
  uint64_t start;
  uint64_t increment;
};

/* The catalog key of a sequence: "S" | name. */
std::string seq_key(const char *name) { return std::string("S") + name; }

/*
 * Look name up at view: 0 and *def if it names a live sequence,
 * 1 if not, -1 on error.
 */
int seq_lookup(sm1::session_t *s, const view_t &view, const char *name,
               seqdef_t *def) {
  std::string val;
  // Read the catalog row at view.
  int r = access_read(s, 0, view, seq_key(name), &val);
  // Pass through "not found" and errors.
  if (r != 0) {
    return r;
  }
  // Reject a malformed catalog row.
  if (val.size() != 24) {
    return set_error(sm1::SM_ERR_CORRUPT, "bad catalog row for sequence {}",
                     name);
  }
  // Decode the definition.
  def->seqid = sm0::get_be64(val.data());
  def->start = sm0::get_be64(val.data() + 8);
  def->increment = sm0::get_be64(val.data() + 16);
  return 0;
}

/*
 * The session's temporary sequence name, or NULL if it has none, or
 * its transaction has dropped it.
 */
sm1::session_t::temp_seq_t *temp_seq(sm1::session_t *s, const char *name) {
  auto it = s->temp_seqs.find(name);
  if (it == s->temp_seqs.end() ||
      std::find(s->temp_seqs_dropped.begin(), s->temp_seqs_dropped.end(),
                name) != s->temp_seqs_dropped.end()) {
    return nullptr;
  }
  return &it->second;
}

/* Hand out temporary sequence q's next value, as db_next_seq does. */
int temp_seq_next(sm1::session_t::temp_seq_t *q, uint64_t *out) {
  // Refuse a sequence whose last value is already handed out.
  if (q->exhausted) {
    return set_error(sm1::SM_ERR_INVALID, "sequence is exhausted");
  }
  *out = q->next;
  // Stop here if another step would wrap.
  if (q->next > UINT64_MAX - q->increment) {
    q->exhausted = true;
  } else {
    q->next += q->increment;
  }
  return 0;
}

/* sm1::mkseq, minus the out-of-memory guard. */
int mkseq(sm1::session_t *s, const char *name, uint64_t start,
          uint64_t increment, bool temporary) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(name, sm1::max_seqname, "sequence") != 0) {
    return -1;
  }
  // A sequence that never advances is a mistake, not a feature.
  if (increment == 0) {
    return set_error(sm1::SM_ERR_INVALID, "sequence increment is 0");
  }
  // A temporary sequence is an entry in the session, and no more.
  if (temporary) {
    // One the transaction dropped still holds its name until commit.
    if (s->temp_seqs.count(name)) {
      return set_error(sm1::SM_ERR_EXISTS, "sequence {} already exists", name);
    }
    s->temp_seqs[name] = {start, increment, start, false};
    // Remember it, so abort or rollback takes it out again.
    s->temp_seqs_made.push_back(name);
    s->temp_touched = true;
    return 0;
  }
  seqdef_t def;
  // Resolve the name at the transaction's view.
  int r = seq_lookup(s, xct_view(s), name, &def);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a name that is already live.
  if (r == 0) {
    return set_error(sm1::SM_ERR_EXISTS, "sequence {} already exists", name);
  }
  // Refuse writes after fail-stop, then allocate the seqid.
  if (xct_check_writable(s->db) != 0 ||
      db_alloc_objid(s->db, &def.seqid) != 0) {
    return -1;
  }

  // Encode { seqid, start, increment } as the row's value.
  std::string val;
  sm0::put_be64(val, def.seqid);
  sm0::put_be64(val, start);
  sm0::put_be64(val, increment);
  // Insert the catalog row through the ordinary write path.
  if (access_write(s, 0, WRITE_INSERT, seq_key(name), val, nullptr) != 0) {
    // Report a concurrent same-name insert as SM_ERR_EXISTS.
    if (s->active && sm1::errnum() == sm1::SM_ERR_DUPKEY) {
      return set_error(sm1::SM_ERR_EXISTS, "sequence {} already exists", name);
    }
    return -1;
  }
  return 0;
}

/* sm1::rmseq, minus the out-of-memory guard. */
int rmseq(sm1::session_t *s, const char *name) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(name, sm1::max_seqname, "sequence") != 0) {
    return -1;
  }
  // A temporary sequence goes at commit; until then it is only
  // hidden from the transaction.
  if (temp_seq(s, name)) {
    s->temp_seqs_dropped.push_back(name);
    s->temp_touched = true;
    return 0;
  }
  seqdef_t def;
  // Resolve the name at the transaction's view.
  int r = seq_lookup(s, xct_view(s), name, &def);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a name that is not live.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "sequence {} does not exist", name);
  }
  // Delete the catalog row.
  if (access_write(s, 0, WRITE_DELETE, seq_key(name), rocksdb::Slice(),
                   nullptr) != 0) {
    return -1;
  }
  // Remember the seqid, so commit drops its lease.
  s->dropped_seqs.push_back(def.seqid);
  return 0;
}

/* sm1::nextseq, minus the out-of-memory guard. */
int nextseq(sm1::session_t *s, const char *name, uint64_t *out) {
  // Require somewhere to put the value and a valid name.
  if (!out) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_nextseq: out is NULL");
  }
  if (check_name(name, sm1::max_seqname, "sequence") != 0) {
    return -1;
  }
  // A session holding a prepared transaction does nothing else.
  if (!s->active && !s->prepared_gid.empty()) {
    return set_error(sm1::SM_ERR_PREPARED, "the transaction is prepared as {}",
                     s->prepared_gid.c_str());
  }
  // Nor does one a rollback doomed (§6.5).
  if (s->active && s->doomed == sm1::SM_ERR_DOOMED) {
    return xct_check_active(s);
  }
  // The session's temporary sequence, if the name is one's.
  if (sm1::session_t::temp_seq_t *q = temp_seq(s, name)) {
    return temp_seq_next(q, out);
  }
  // A stopped database persists nothing, leases included.
  if (xct_check_writable(s->db) != 0) {
    return -1;
  }
  // Read the definition at the transaction's view if there is
  // one, so a sequence this transaction created is usable at
  // once; otherwise at the newest committed state.
  view_t view =
      s->active ? xct_view(s) : view_t{s->db->visible_ts.load(), 0, 0};
  seqdef_t def;
  int r = seq_lookup(s, view, name, &def);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Report an unknown sequence.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "sequence {} does not exist", name);
  }
  // Take the next value; a used-up lease is persisted first.
  return db_next_seq(s->db, def.seqid, def.start, def.increment, out);
}

/* sm1::lastseq, minus the out-of-memory guard. */
int lastseq(sm1::session_t *s, const char *name, uint64_t *out) {
  // Require a valid name.
  if (check_name(name, sm1::max_seqname, "sequence") != 0) {
    return -1;
  }
  // A session holding a prepared transaction does nothing else.
  if (!s->active && !s->prepared_gid.empty()) {
    return set_error(sm1::SM_ERR_PREPARED, "the transaction is prepared as {}",
                     s->prepared_gid.c_str());
  }
  // Nor does one a rollback doomed (§6.5).
  if (s->active && s->doomed == sm1::SM_ERR_DOOMED) {
    return xct_check_active(s);
  }
  // The session's temporary sequence, if the name is one's.
  if (sm1::session_t::temp_seq_t *q = temp_seq(s, name)) {
    // Still at its start, and not because it is used up: none drawn.
    if (q->next == q->start && !q->exhausted) {
      return 1;
    }
    *out = q->exhausted ? q->next : q->next - q->increment;
    return 0;
  }
  // Read the definition as sm1::nextseq does.
  view_t view =
      s->active ? xct_view(s) : view_t{s->db->visible_ts.load(), 0, 0};
  seqdef_t def;
  int r = seq_lookup(s, view, name, &def);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Report an unknown sequence.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "sequence {} does not exist", name);
  }
  // Read the position, drawing nothing.
  return db_last_seq(s->db, def.seqid, def.start, def.increment, out);
}

} // namespace

/* Create a sequence. */
int sm1::mkseq(sm1::session_t *session, std::string_view name_arg,
               uint64_t start, uint64_t increment, bool temporary) {
  // Take the name as a C string, as the engine uses it.
  std::string name_str;
  if (copy_name(name_arg, "sequence", name_str) != 0) {
    return -1;
  }
  const char *name = name_str.c_str();
  // Require a session, which xct_check_active cannot check.
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_mkseq: session is NULL");
  }
  return ::mkseq(session, name, start, increment, temporary);
}

/* Drop a sequence. */
int sm1::rmseq(sm1::session_t *session, std::string_view name_arg) {
  // Take the name as a C string, as the engine uses it.
  std::string name_str;
  if (copy_name(name_arg, "sequence", name_str) != 0) {
    return -1;
  }
  const char *name = name_str.c_str();
  // Require a session, which xct_check_active cannot check.
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_rmseq: session is NULL");
  }
  return ::rmseq(session, name);
}

/* Hand out a sequence's next value. */
int sm1::nextseq(sm1::session_t *session, std::string_view name_arg,
                 uint64_t &out) {
  // Take the name as a C string, as the engine uses it.
  std::string name_str;
  if (copy_name(name_arg, "sequence", name_str) != 0) {
    return -1;
  }
  const char *name = name_str.c_str();
  // Require a session; sm1::nextseq needs no transaction.
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_nextseq: session is NULL");
  }
  return ::nextseq(session, name, &out);
}

/* Report a sequence's last value without drawing one. */
int sm1::lastseq(sm1::session_t *session, std::string_view name_arg,
                 uint64_t &out) {
  // Take the name as a C string, as the engine uses it.
  std::string name_str;
  if (copy_name(name_arg, "sequence", name_str) != 0) {
    return -1;
  }
  const char *name = name_str.c_str();
  // Require a session; sm1::lastseq needs no transaction.
  if (!session) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_lastseq: session is NULL");
  }
  return ::lastseq(session, name, &out);
}
