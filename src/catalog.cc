/*
 * catalog.cc -- relations: lookup, sm_mkrel, sm_rmrel,
 * sm_lsrel (DESIGN.md §11).
 *
 * The catalog is relid 0, an ordinary MVCC relation keyed
 * "R" | be64(relid) with an empty value, so DDL is
 * transactional by going through the ordinary write path. The
 * row records that the relation exists; the relid itself names
 * the column family, CF:rel.<relid>. The design's created_ts
 * field is left out: nothing reads it.
 *
 * sm_mkrel allocates the relid from the lease in CF:meta
 * (§4.2), so a relid is never reused and never names two
 * relations.
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

/* sm_mkrel, minus the out-of-memory guard. */
int mkrel(sm_session_t *s, uint64_t *out) {
  // Require the out-parameter.
  if (!out) {
    return set_error(SM_ERR_INVALID, "sm_mkrel: relid is NULL");
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  uint64_t relid;
  // Refuse writes after fail-stop, then allocate the relid.
  if (xct_check_writable(s->db) != 0 || db_alloc_objid(s->db, &relid) != 0) {
    return -1;
  }

  // Create the column family eagerly, before the catalog row,
  // so this transaction can write into it at once (§11).
  if (db_create_rel_cf(s->db, relid) != 0) {
    return -1;
  }
  // Remember the CF, so abort or rollback drops it.
  s->created.push_back(relid);

  // Insert the catalog row through the ordinary write path. The
  // relid is fresh, so no other transaction can be writing it.
  if (access_write(s, 0, WRITE_INSERT, catalog_key(relid), rocksdb::Slice()) !=
      0) {
    // A retryable error already aborted the transaction.
    if (!s->active) {
      return -1; // retryable: the abort already dropped the CF
    }
    // Undo the eager CF creation.
    s->created.pop_back();
    db_drop_rel_cf(s->db, relid);
    return -1;
  }
  *out = relid;
  return 0;
}

/* sm_rmrel, minus the out-of-memory guard. */
int rmrel(sm_session_t *s, uint64_t relid) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject relid 0, the catalog's own slot.
  if (relid == 0) {
    return set_error(SM_ERR_INVALID, "relid 0 is not a relation");
  }
  // Resolve the relid at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), relid);
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
  // Remember the relid, so GC drops its CF after commit.
  s->dropped.push_back(relid);
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
int catalog_lookup(sm_session_t *s, const view_t &view, uint64_t relid) {
  std::string val;
  // Read the catalog row at view: 0 if the relation is live.
  return access_read(s, 0, view, catalog_key(relid), &val);
}

} // namespace wisr

/* Create a relation and hand out its relid. */
int sm_mkrel(sm_session_t *session, uint64_t *relid) {
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
