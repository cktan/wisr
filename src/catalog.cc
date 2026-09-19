/*
 * catalog.cc -- relation ids: lookup, sm_mkrel, sm_rmrel,
 * sm_lsrel (DESIGN.md §11).
 *
 * The catalog is cfid 0, an ordinary MVCC relation keyed
 * "R" | be64(relid) with the 8-byte cfid as its value, so DDL
 * is transactional by going through the ordinary write path.
 * Each sm_mkrel takes a fresh cfid, so a relid dropped and
 * created again names a new column family at once, while
 * older snapshots still read the old one. The design's
 * created_ts field is left out: nothing reads it.
 *
 * No relid cache yet; every lookup reads CF:rel.0. A
 * committed sm_rmrel leaves its column family in place until
 * the watermark passes the drop and GC removes it (§8.3).
 */

#include "codec.h"
#include "defer.h"
#include "error.h"
#include "session.h"

#include <stdlib.h>
#include <string.h>

#include <new>
#include <vector>

using namespace wisr;

namespace {

/* The catalog key of relid: "R" | be64(relid). */
std::string catalog_key(uint64_t relid) {
  std::string key("R");
  put_be64(key, relid);
  return key;
}

/*
 * Check that relid may name a relation: not 0, and either a
 * caller's constant below SM_SEQ_FIRST or a value sm_nextseq
 * has handed out.
 */
int check_relid(sm_session_t *s, uint64_t relid) {
  // Reject relid 0, which no relation has.
  if (relid == 0) {
    return set_error(SM_ERR_INVALID, "relid 0 is not a relation");
  }
  // Reject a sequence value not yet handed out.
  if (relid >= SM_SEQ_FIRST && !db_seq_issued(s->db, relid)) {
    return set_error(SM_ERR_INVALID,
                     "relid {} was not handed out by sm_nextseq", relid);
  }
  return 0;
}

/* sm_mkrel, minus the out-of-memory guard. */
int mkrel(sm_session_t *s, uint64_t relid) {
  // Require an active, non-prepared transaction and a valid relid.
  if (xct_check_active(s) != 0 || check_relid(s, relid) != 0) {
    return -1;
  }
  uint64_t cfid;
  // Resolve the relid at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), relid, &cfid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a relid that is already live.
  if (r == 0) {
    return set_error(SM_ERR_EXISTS, "relation {} already exists", relid);
  }
  // Refuse writes after fail-stop, then allocate a fresh cfid.
  if (xct_check_writable(s->db) != 0 || db_alloc_cfid(s->db, &cfid) != 0) {
    return -1;
  }

  // Create the column family eagerly, before the catalog row,
  // so this transaction can write into it at once (§11).
  if (db_create_rel_cf(s->db, cfid) != 0) {
    return -1;
  }
  // Remember the CF, so abort or rollback drops it.
  s->created.push_back(cfid);

  // Encode the cfid as the catalog row's value.
  std::string val;
  put_be64(val, cfid);
  // Insert the catalog row through the ordinary write path.
  if (access_write(s, 0, WRITE_INSERT, catalog_key(relid), val) != 0) {
    // A retryable error already aborted the transaction.
    if (!s->active) {
      return -1; // retryable: the abort already dropped the CF
    }
    // Report a concurrent same-relid insert as SM_ERR_EXISTS.
    bool dup = sm_errno() == SM_ERR_DUPKEY;
    // Undo the eager CF creation.
    s->created.pop_back();
    db_drop_rel_cf(s->db, cfid);
    // Map a duplicate catalog key to SM_ERR_EXISTS.
    if (dup) {
      return set_error(SM_ERR_EXISTS, "relation {} already exists", relid);
    }
    return -1;
  }
  return 0;
}

/* sm_rmrel, minus the out-of-memory guard. */
int rmrel(sm_session_t *s, uint64_t relid) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject relid 0, which no relation has.
  if (relid == 0) {
    return set_error(SM_ERR_INVALID, "relid 0 is not a relation");
  }
  uint64_t cfid;
  // Resolve the relid at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), relid, &cfid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a relid that is not live.
  if (r == 1) {
    return set_error(SM_ERR_NOTFOUND, "relation {} does not exist", relid);
  }
  // Delete the catalog row; the column family stays until no
  // snapshot can still read the relation (§8.3).
  if (access_write(s, 0, WRITE_DELETE, catalog_key(relid), rocksdb::Slice()) !=
      0) {
    return -1;
  }
  // Remember the cfid, so GC drops its CF after commit.
  s->dropped.push_back(cfid);
  return 0;
}

/* sm_lsrel, minus the out-of-memory guard. */
int lsrel(sm_session_t *s, uint64_t **out, size_t *n) {
  // Require the out-parameters.
  if (!out || !n) {
    return set_error(SM_ERR_INVALID, "sm_lsrel: out or n is NULL");
  }
  // Report an empty list until one is built.
  *out = nullptr;
  *n = 0;
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Scan every catalog row, "R" up to but excluding "S"; under
  // SERIALIZABLE this records the whole catalog as read.
  sm_scan_t *c = access_scan(s, 0, xct_view(s), "R", 1, 1, "S", 1, 0);
  if (!c) {
    return -1;
  }
  // Close the cursor however the scan ends.
  defer { sm_scanend(c); };

  std::vector<uint64_t> ids;
  const char *key, *data;
  size_t klen, datalen;
  int r;
  // Collect the relid of each live catalog row, in order.
  while ((r = sm_scannxt(c, &key, &klen, &data, &datalen)) == 0) {
    // Reject a malformed catalog key.
    if (klen != 9) {
      return set_error(SM_ERR_CORRUPT, "bad catalog key of {} bytes", klen);
    }
    ids.push_back(get_be64(key + 1));
  }
  // Fail on a scan error.
  if (r < 0) {
    return -1;
  }
  // An empty list stays NULL.
  if (ids.empty()) {
    return 0;
  }
  // Copy the ids into memory the caller frees.
  uint64_t *list =
      static_cast<uint64_t *>(malloc(ids.size() * sizeof(uint64_t)));
  if (!list) {
    return set_error(SM_ERR_NOMEM, "sm_lsrel: out of memory");
  }
  memcpy(list, ids.data(), ids.size() * sizeof(uint64_t));
  *out = list;
  *n = ids.size();
  return 0;
}

} // namespace

namespace wisr {

/* A point read of the catalog row at view. */
int catalog_lookup(sm_session_t *s, const view_t &view, uint64_t relid,
                   uint64_t *cfid) {
  std::string val;
  // Read the catalog row at view.
  int r = access_read(s, 0, view, catalog_key(relid), &val);
  // Pass through "not found" and errors.
  if (r != 0) {
    return r;
  }
  // Reject a malformed catalog row.
  if (val.size() != 8) {
    return set_error(SM_ERR_CORRUPT, "bad catalog row for relation {}", relid);
  }
  // Decode the cfid.
  *cfid = get_be64(val.data());
  return 0;
}

} // namespace wisr

/* Create a relation. */
int sm_mkrel(sm_session_t *session, uint64_t relid) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return mkrel(session, relid);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_mkrel: out of memory");
  }
}

/* Drop a relation and all of its rows. */
int sm_rmrel(sm_session_t *session, uint64_t relid) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return rmrel(session, relid);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_rmrel: out of memory");
  }
}

/* List the relations visible to the transaction. */
int sm_lsrel(sm_session_t *session, uint64_t **out, size_t *n) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return lsrel(session, out, n);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_lsrel: out of memory");
  }
}

/* Release a list from sm_lsrel(). */
void sm_free_rels(uint64_t *list) { free(list); }
