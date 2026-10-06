/*
 * catalog.cc -- stores: lookup, sm1::mkstore, sm1::rmstore,
 * sm1::lsstore (SM1_DESIGN.md §11).
 *
 * The catalog is storeid 0, an ordinary MVCC store keyed
 * "R" | be64(storeid) with an empty value, so DDL is
 * transactional by going through the ordinary write path. The
 * row records that the store exists; the storeid itself names
 * the column family, CF:store.<storeid>. The design's created_ts
 * field is left out: nothing reads it.
 *
 * sm1::mkstore allocates the storeid from the lease in CF:meta
 * (§4.2), so a storeid is never reused and never names two
 * stores.
 *
 * No storeid cache yet; every lookup reads CF:store.0. A
 * committed sm1::rmstore leaves its column family in place until
 * the watermark passes the drop and GC removes it (§8.3).
 */

#include "error.h"
#include "oom.h"
#include "session.h"
#include "sm1_codec.h"
#include "sm1_defer.h"

#include <new>
#include <vector>

using namespace wisr;

namespace {

/* The catalog key of storeid: "R" | be64(storeid). */
std::string catalog_key(uint64_t storeid) {
  std::string key("R");
  sm1::put_be64(key, storeid);
  return key;
}

/* sm1::mkstore, minus the out-of-memory guard. */
int mkstore(sm1::session_t *s, uint64_t *out) {
  // Require the out-parameter.
  if (!out) {
    return set_error(sm1::SM_ERR_INVALID, "sm1_mkstore: storeid is NULL");
  }
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  uint64_t storeid;
  // Refuse writes after fail-stop or in a read-only transaction,
  // then allocate the storeid: before the CF is created.
  if (xct_check_writable(s->db) != 0 || xct_check_readonly(s) != 0 ||
      db_alloc_objid(s->db, &storeid) != 0) {
    return -1;
  }

  // Create the column family eagerly, before the catalog row,
  // so this transaction can write into it at once (§11).
  if (db_create_store_cf(s->db, storeid) != 0) {
    return -1;
  }
  // Remember the CF, so abort or rollback drops it.
  s->created.push_back(storeid);

  // Insert the catalog row through the ordinary write path. The
  // storeid is fresh, so no other transaction can be writing it.
  if (access_write(s, 0, WRITE_INSERT, catalog_key(storeid), rocksdb::Slice(),
                   nullptr) != 0) {
    // A retryable error already aborted the transaction.
    if (!s->active) {
      return -1; // retryable: the abort already dropped the CF
    }
    // Undo the eager CF creation.
    s->created.pop_back();
    db_drop_store_cf(s->db, storeid);
    return -1;
  }
  *out = storeid;
  return 0;
}

/* sm1::rmstore, minus the out-of-memory guard. */
int rmstore(sm1::session_t *s, uint64_t storeid) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Reject storeid 0, the catalog's own slot.
  if (storeid == 0) {
    return set_error(sm1::SM_ERR_INVALID, "storeid 0 is not a store");
  }
  // Resolve the storeid at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), storeid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a storeid that is not live.
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  // Delete the catalog row; the column family stays until no
  // snapshot can still read the store (§8.3).
  if (access_write(s, 0, WRITE_DELETE, catalog_key(storeid), rocksdb::Slice(),
                   nullptr) != 0) {
    return -1;
  }
  // Remember the storeid, so GC drops its CF after commit.
  s->dropped.push_back(storeid);
  return 0;
}

/* sm1::lsstore, minus the out-of-memory guard. */
int lsstore(sm1::session_t *s, std::vector<uint64_t> &out) {
  // Require an active, non-prepared transaction.
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Scan every catalog row, "R" up to but excluding "S"; under
  // SERIALIZABLE this records the whole catalog as read.
  sm1::scan_t *c = access_scan(s, 0, xct_view(s), "R", 1, 1, "S", 1, 0);
  if (!c) {
    return -1;
  }
  // Close the cursor however the scan ends.
  defer { sm1::scanend(c); };

  std::vector<uint64_t> ids;
  std::string_view key, data;
  int r;
  // Collect the storeid of each live catalog row, in order.
  while ((r = sm1::scannxt(c, key, data)) == 0) {
    // Reject a malformed catalog key.
    if (key.size() != 9) {
      return set_error(sm1::SM_ERR_CORRUPT, "bad catalog key of {} bytes",
                       key.size());
    }
    ids.push_back(sm1::get_be64(key.data() + 1));
  }
  // Fail on a scan error.
  if (r < 0) {
    return -1;
  }
  // Hand the storeids to the caller.
  out = std::move(ids);
  return 0;
}

} // namespace

namespace wisr {

/* A point read of the catalog row at view. */
int catalog_lookup(sm1::session_t *s, const view_t &view, uint64_t storeid) {
  std::string val;
  // Read the catalog row at view: 0 if the store is live.
  return access_read(s, 0, view, catalog_key(storeid), &val);
}

} // namespace wisr

/* Create a store and hand out its storeid. */
int sm1::mkstore(sm1::session_t *session, uint64_t &storeid) {
  // Inside sm1: running out of memory is fatal.
  in_sm1_t in_sm1;
  return ::mkstore(session, &storeid);
}

/* Drop a store and all of its rows. */
int sm1::rmstore(sm1::session_t *session, uint64_t storeid) {
  // Inside sm1: running out of memory is fatal.
  in_sm1_t in_sm1;
  return ::rmstore(session, storeid);
}

/* List the stores visible to the transaction. */
int sm1::lsstore(sm1::session_t *session, std::vector<uint64_t> &out) {
  // Inside sm1: running out of memory is fatal.
  in_sm1_t in_sm1;
  return ::lsstore(session, out);
}
