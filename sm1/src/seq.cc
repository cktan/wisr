/*
 * seq.cc -- named sequences: sm_mkseq, sm_rmseq, sm_nextseq
 * (DESIGN.md §11).
 *
 * A sequence's definition is a catalog row, keyed "S" | name in
 * CF:store.0 and holding { seqid, start, increment }, so creating
 * and dropping one is transactional like any other DDL. Its
 * seqid comes from the object-id lease, the same one storeids
 * come from, so a name reused after a drop names a new
 * sequence that starts over.
 *
 * The sequence's position is NOT in that row: sm_nextseq takes
 * no locks, never blocks, and is not undone by an abort, so its
 * state lives in CF:meta under a lease (§4.2), outside MVCC.
 */

#include "codec.h"
#include "error.h"
#include "session.h"

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
int seq_lookup(sm_session_t *s, const view_t &view, const char *name,
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
    return set_error(SM_ERR_CORRUPT, "bad catalog row for sequence {}", name);
  }
  // Decode the definition.
  def->seqid = get_be64(val.data());
  def->start = get_be64(val.data() + 8);
  def->increment = get_be64(val.data() + 16);
  return 0;
}

/* sm_mkseq, minus the out-of-memory guard. */
int mkseq(sm_session_t *s, const char *name, uint64_t start,
          uint64_t increment) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(name, SM_MAX_SEQNAME, "sequence") != 0) {
    return -1;
  }
  // A sequence that never advances is a mistake, not a feature.
  if (increment == 0) {
    return set_error(SM_ERR_INVALID, "sequence increment is 0");
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
    return set_error(SM_ERR_EXISTS, "sequence {} already exists", name);
  }
  // Refuse writes after fail-stop, then allocate the seqid.
  if (xct_check_writable(s->db) != 0 ||
      db_alloc_objid(s->db, &def.seqid) != 0) {
    return -1;
  }

  // Encode { seqid, start, increment } as the row's value.
  std::string val;
  put_be64(val, def.seqid);
  put_be64(val, start);
  put_be64(val, increment);
  // Insert the catalog row through the ordinary write path.
  if (access_write(s, 0, WRITE_INSERT, seq_key(name), val, nullptr) != 0) {
    // Report a concurrent same-name insert as SM_ERR_EXISTS.
    if (s->active && sm_errno() == SM_ERR_DUPKEY) {
      return set_error(SM_ERR_EXISTS, "sequence {} already exists", name);
    }
    return -1;
  }
  return 0;
}

/* sm_rmseq, minus the out-of-memory guard. */
int rmseq(sm_session_t *s, const char *name) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(name, SM_MAX_SEQNAME, "sequence") != 0) {
    return -1;
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
    return set_error(SM_ERR_NOTFOUND, "sequence {} does not exist", name);
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

/* sm_nextseq, minus the out-of-memory guard. */
int nextseq(sm_session_t *s, const char *name, uint64_t *out) {
  // Require somewhere to put the value and a valid name.
  if (!out) {
    return set_error(SM_ERR_INVALID, "sm_nextseq: out is NULL");
  }
  if (check_name(name, SM_MAX_SEQNAME, "sequence") != 0) {
    return -1;
  }
  // A session holding a prepared transaction does nothing else.
  if (!s->active && !s->prepared_gid.empty()) {
    return set_error(SM_ERR_PREPARED, "the transaction is prepared as {}",
                     s->prepared_gid.c_str());
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
    return set_error(SM_ERR_NOTFOUND, "sequence {} does not exist", name);
  }
  // Take the next value; a used-up lease is persisted first.
  return db_next_seq(s->db, def.seqid, def.start, def.increment, out);
}

} // namespace

/* Create a sequence. */
int sm_mkseq(sm_session_t *session, const char *name, uint64_t start,
             uint64_t increment) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    // Require a session, which xct_check_active cannot check.
    if (!session) {
      return set_error(SM_ERR_INVALID, "sm_mkseq: session is NULL");
    }
    return mkseq(session, name, start, increment);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_mkseq: out of memory");
  }
}

/* Drop a sequence. */
int sm_rmseq(sm_session_t *session, const char *name) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    // Require a session, which xct_check_active cannot check.
    if (!session) {
      return set_error(SM_ERR_INVALID, "sm_rmseq: session is NULL");
    }
    return rmseq(session, name);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_rmseq: out of memory");
  }
}

/* Hand out a sequence's next value. */
int sm_nextseq(sm_session_t *session, const char *name, uint64_t *out) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    // Require a session; sm_nextseq needs no transaction.
    if (!session) {
      return set_error(SM_ERR_INVALID, "sm_nextseq: session is NULL");
    }
    return nextseq(session, name, out);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_nextseq: out of memory");
  }
}
