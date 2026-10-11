/*
 * stats.cc -- what a store's LSM looks like now: sm1::approxsize
 * and sm1::probes (sm3/SM3_DESIGN.md §6.2, §6.3 at the stack's
 * root).
 *
 * Both read RocksDB's own estimates and properties of a store's
 * column family. Neither reads a version, so neither needs a
 * transaction, takes a lock, or sees a snapshot.
 */

#include "error.h"
#include "session.h"
#include "version.h"

#include <string>

#include <rocksdb/options.h>

using namespace wisr;

namespace {

/*
 * Check a session for a call that reads no transaction: it must
 * exist and must not hold a prepared transaction. Returns 0 or -1.
 */
int check_session(sm1::session_t *s, const char *call) {
  // Require a session.
  if (!s) {
    return set_error(sm1::SM_ERR_INVALID, "{}: session is NULL", call);
  }
  // A session holding a prepared transaction does nothing else.
  if (!s->active && !s->prepared_gid.empty()) {
    return set_error(sm1::SM_ERR_PREPARED, "the transaction is prepared as {}",
                     s->prepared_gid.c_str());
  }
  return 0;
}

/* The store storeid, its cf NULL with SM_ERR_NOTFOUND set. */
store_t find_store(sm1::session_t *s, uint64_t storeid) {
  // Look the store up by its CF, not its catalog row.
  store_t st = db_store(s->db, storeid);
  // Another session's temporary store is not there to this one.
  if (!st.cf || (st.owner && st.owner != s)) {
    set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
    return store_t{};
  }
  return st;
}

/* sm1::approxsize, minus the out-of-memory guard. */
int approxsize(sm1::session_t *s, uint64_t storeid, const char *lo,
               size_t lolen, const char *hi, size_t hilen, uint64_t *disk,
               uint64_t *mem) {
  // Require a usable session and a store.
  if (check_session(s, "sm1_approxsize") != 0) {
    return -1;
  }
  store_t store = find_store(s, storeid);
  if (!store.cf) {
    return -1;
  }
  // Bound by version keys: a tail of zeros sorts before every
  // version of its userkey, so [lo, hi) holds every version of
  // every userkey in it. No bound is the store's first or last
  // possible key.
  std::string top(sm1::max_keylen + 1, '\xff'); // above every userkey
  std::string start =
      version_bound(lo ? rocksdb::Slice(lo, lolen) : rocksdb::Slice(), '\x00');
  std::string limit = version_bound(
      hi ? rocksdb::Slice(hi, hilen) : rocksdb::Slice(top), '\x00');
  rocksdb::Range range(start, limit);

  // Sorted files only: the memtables are reported apart, since
  // they hold versions uncompressed.
  rocksdb::SizeApproximationOptions so;
  so.include_memtables = false;
  so.include_files = true;
  uint64_t files = 0;
  rocksdb::Status st =
      store.rdb->GetApproximateSizes(so, store.cf, &range, 1, &files);
  if (!st.ok()) {
    return set_status_error(st, "approximate store size");
  }
  uint64_t count = 0, bytes = 0;
  // The memtables' share of the range.
  store.rdb->GetApproximateMemTableStats(store.cf, range, &count, &bytes);
  *disk = files;
  *mem = bytes;
  return 0;
}

/* An integer property of store, or 0 if RocksDB has none. */
uint64_t int_property(const store_t &store, const std::string &name) {
  uint64_t v = 0;
  return store.rdb->GetIntProperty(store.cf, name, &v) ? v : 0;
}

/* sm1::probes, minus the out-of-memory guard. */
int probes(sm1::session_t *s, uint64_t storeid, uint64_t *out) {
  // Require a usable session and a store.
  if (check_session(s, "sm1_probes") != 0) {
    return -1;
  }
  store_t store = find_store(s, storeid);
  if (!store.cf) {
    return -1;
  }
  uint64_t n = 0;
  // The active memtable, when it holds anything.
  if (int_property(store, rocksdb::DB::Properties::kNumEntriesActiveMemTable) >
      0) {
    n += 1;
  }
  // Each immutable memtable not yet flushed.
  n += int_property(store, rocksdb::DB::Properties::kNumImmutableMemTable);
  // Level 0's files overlap, so each is a probe; a lower level
  // is one probe if it has any file.
  int levels = store.rdb->NumberLevels(store.cf);
  for (int l = 0; l < levels; l++) {
    std::string v;
    if (!store.rdb->GetProperty(
            store.cf,
            rocksdb::DB::Properties::kNumFilesAtLevelPrefix + std::to_string(l),
            &v)) {
      continue;
    }
    uint64_t files = std::stoull(v);
    n += l == 0 ? files : (files > 0 ? 1 : 0);
  }
  *out = n;
  return 0;
}

} // namespace

/* Estimate the bytes of a key range, in files and in memtables. */
int sm1::approxsize(sm1::session_t *session, uint64_t storeid, const char *lo,
                    size_t lolen, const char *hi, size_t hilen, uint64_t &disk,
                    uint64_t &mem) {
  return ::approxsize(session, storeid, lo, lolen, hi, hilen, &disk, &mem);
}

/* Count the sorted runs a point lookup of a store may visit. */
int sm1::probes(sm1::session_t *session, uint64_t storeid, uint64_t &out) {
  return ::probes(session, storeid, &out);
}
