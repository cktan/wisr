/*
 * stats.cc -- what a store's LSM looks like now: sm1::approxsize
 * and sm1::probes (sm3/SM3_DESIGN.md §6.2, §6.3 at the stack's
 * root); and sm1::compact, which merges it into one level.
 *
 * Each works on RocksDB's column family of a store, and none
 * reads a version, so none needs a transaction, takes a lock, or
 * sees a snapshot.
 */

#include "error.h"
#include "session.h"
#include "version.h"

#include <charconv>
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
  std::string start =
      version_bound(lo ? rocksdb::Slice(lo, lolen) : rocksdb::Slice(), '\x00');
  std::string limit;
  if (hi) {
    limit = version_bound(rocksdb::Slice(hi, hilen), '\x00');
  } else {
    std::string top(sm1::max_keylen + 1, '\xff'); // above every userkey
    limit = version_bound(rocksdb::Slice(top), '\x00');
  }
  rocksdb::Range range(start, limit);

  // Sorted files only: the memtables are reported apart, since
  // they hold versions uncompressed. The files change only with the
  // tree's version, read first, so an estimate kept under it is one
  // RocksDB made of those files or newer ones.
  uint64_t version = 0;
  bool versioned =
      store.files &&
      store.rdb->GetIntProperty(
          store.cf, rocksdb::DB::Properties::kCurrentSuperVersionNumber,
          &version);
  uint64_t files = 0;
  if (!versioned || !store.files->find(start, limit, version, files)) {
    rocksdb::SizeApproximationOptions so;
    so.include_memtables = false;
    so.include_files = true;
    rocksdb::Status st =
        store.rdb->GetApproximateSizes(so, store.cf, &range, 1, &files);
    if (!st.ok()) {
      return set_status_error(st, "approximate store size");
    }
    if (versioned) {
      store.files->put(start, limit, version, files);
    }
  }
  *disk = files;
  // The memtables' share of a range of whole 2-byte prefixes, a heap's
  // or an index's: counted as written (memcount.h). Of any other
  // range, RocksDB's sample of its memtables.
  if ((!lo || lolen == 2) && (!hi || hilen == 2) && store.mem) {
    uint32_t from = lo ? key_prefix(std::string_view(lo, lolen)) : 0;
    uint32_t to = hi ? key_prefix(std::string_view(hi, hilen)) : 0x10000;
    // The sealed memtables RocksDB has not flushed yet: a seal and a
    // flush each raise the tree's version, so a count kept under it
    // is one RocksDB gave at that version or later.
    uint64_t unflushed = 0;
    if (!versioned || !store.files->find_unflushed(version, unflushed)) {
      store.rdb->GetIntProperty(
          store.cf, rocksdb::DB::Properties::kNumImmutableMemTable, &unflushed);
      if (versioned) {
        store.files->put_unflushed(version, unflushed);
      }
    }
    *mem = store.mem->bytes(from, to, unflushed);
    return 0;
  }
  uint64_t count = 0, bytes = 0;
  store.rdb->GetApproximateMemTableStats(store.cf, range, &count, &bytes);
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
    uint64_t files = 0;
    std::from_chars(v.data(), v.data() + v.size(), files);
    n += l == 0 ? files : (files > 0 ? 1 : 0);
  }
  *out = n;
  return 0;
}

/* sm1::compact, minus the out-of-memory guard. */
int compact(sm1::session_t *s, uint64_t storeid) {
  // Require a usable session and a store.
  if (check_session(s, "sm1_compact") != 0) {
    return -1;
  }
  store_t store = find_store(s, storeid);
  if (!store.cf) {
    return -1;
  }
  rocksdb::CompactRangeOptions opt;
  // Into the bottom level, without compacting again the files this
  // compaction writes there.
  opt.bottommost_level_compaction =
      rocksdb::BottommostLevelCompaction::kForceOptimized;
  // Other stores' compactions go on meanwhile.
  opt.exclusive_manual_compaction = false;
  // The whole store, its memtables flushed first.
  rocksdb::Status st = store.rdb->CompactRange(opt, store.cf, nullptr, nullptr);
  if (!st.ok()) {
    return set_status_error(st, "compact store");
  }
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

/* Merge a store's memtables and levels into its bottom level. */
int sm1::compact(sm1::session_t *session, uint64_t storeid) {
  return ::compact(session, storeid);
}
