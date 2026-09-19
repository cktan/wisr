/*
 * catalog.cc -- relation names: lookup, sm_mkrel, sm_rmrel
 * (DESIGN.md §11).
 *
 * The catalog is relation 0, an ordinary MVCC relation keyed
 * "R" | relname with the 8-byte relid as its value, so DDL is
 * transactional by going through the ordinary write path. The
 * design's created_ts field is left out: nothing reads it.
 *
 * No relname cache yet; every lookup reads CF:rel.0. A
 * committed sm_rmrel leaves its column family in place until
 * the watermark passes the drop and GC removes it (§8.3).
 */

#include "codec.h"
#include "error.h"
#include "session.h"

#include <new>

using namespace wisr;

namespace {

/* The catalog key of relname: "R" | relname. */
std::string catalog_key(const char *relname) {
  return std::string("R") + relname;
}

/* sm_mkrel, minus the out-of-memory guard. */
int mkrel(sm_session_t *s, const char *relname) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(relname, SM_MAX_RELNAME, "relation") != 0) {
    return -1;
  }
  uint64_t relid;
  // Resolve the name at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), relname, &relid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a name that is already live.
  if (r == 0) {
    return set_error(SM_ERR_EXISTS, "relation {} already exists", relname);
  }
  // Refuse writes after fail-stop, then allocate a fresh relid.
  if (xct_check_writable(s->db) != 0 || db_alloc_relid(s->db, &relid) != 0) {
    return -1;
  }

  // Create the column family eagerly, before the catalog row,
  // so this transaction can write into it at once (§11).
  if (db_create_rel_cf(s->db, relid) != 0) {
    return -1;
  }
  // Remember the CF, so abort or rollback drops it.
  s->created.push_back(relid);

  // Encode the relid as the catalog row's value.
  std::string val;
  put_be64(val, relid);
  // Insert the catalog row through the ordinary write path.
  if (access_write(s, 0, WRITE_INSERT, catalog_key(relname), val) != 0) {
    // A retryable error already aborted the transaction.
    if (!s->active) {
      return -1; // retryable: the abort already dropped the CF
    }
    // Report a concurrent same-name insert as SM_ERR_EXISTS.
    bool dup = sm_errno() == SM_ERR_DUPKEY;
    // Undo the eager CF creation.
    s->created.pop_back();
    db_drop_rel_cf(s->db, relid);
    // Map a duplicate catalog key to SM_ERR_EXISTS.
    if (dup) {
      return set_error(SM_ERR_EXISTS, "relation {} already exists", relname);
    }
    return -1;
  }
  return 0;
}

/* sm_rmrel, minus the out-of-memory guard. */
int rmrel(sm_session_t *s, const char *relname) {
  // Require an active, non-prepared transaction and a valid name.
  if (xct_check_active(s) != 0 ||
      check_name(relname, SM_MAX_RELNAME, "relation") != 0) {
    return -1;
  }
  uint64_t relid;
  // Resolve the name at the transaction's view.
  int r = catalog_lookup(s, xct_view(s), relname, &relid);
  // Fail on a lookup error.
  if (r < 0) {
    return -1;
  }
  // Refuse a name that is not live.
  if (r == 1) {
    return set_error(SM_ERR_NOTFOUND, "relation {} does not exist", relname);
  }
  // Delete the catalog row; the column family stays until no
  // snapshot can still read the relation (§8.3).
  if (access_write(s, 0, WRITE_DELETE, catalog_key(relname),
                   rocksdb::Slice()) != 0) {
    return -1;
  }
  // Remember the relid, so GC drops its CF after commit.
  s->dropped.push_back(relid);
  return 0;
}

} // namespace

namespace wisr {

/* A point read of the catalog row at view. */
int catalog_lookup(sm_session_t *s, const view_t &view, const char *relname,
                   uint64_t *relid) {
  std::string val;
  // Read the catalog row at view.
  int r = access_read(s, 0, view, catalog_key(relname), &val);
  // Pass through "not found" and errors.
  if (r != 0) {
    return r;
  }
  // Reject a malformed catalog row.
  if (val.size() != 8) {
    return set_error(SM_ERR_CORRUPT, "bad catalog row for {}", relname);
  }
  // Decode the relid.
  *relid = get_be64(val.data());
  return 0;
}

} // namespace wisr

/* Create a relation. */
int sm_mkrel(sm_session_t *session, const char *relname) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return mkrel(session, relname);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_mkrel: out of memory");
  }
}

/* Drop a relation and all of its rows. */
int sm_rmrel(sm_session_t *session, const char *relname) {
  // Report an allocation failure as SM_ERR_NOMEM.
  try {
    return rmrel(session, relname);
  } catch (const std::bad_alloc &) {
    return set_error(SM_ERR_NOMEM, "sm_rmrel: out of memory");
  }
}
