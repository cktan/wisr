/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * getseek.cc -- time one row read by key in RocksDB two ways
 * (../../sm/VCHAIN.md section 1): as sm1 reads it today, a seek of
 * a new iterator to the newest version of the key, and as VCHAIN's
 * CURR would, a Get of the plain key.
 *
 *   getseek load DIR       build the stores of N rows, compacted
 *   getseek VARIANT DIR    time M random reads of one variant
 *
 * load_bw and load_long build one store more each, which load
 * builds too: versioned keys under RocksDB's bytewise comparator,
 * read by any variant with _bw appended, and plain keys padded with
 * a version tail, scanned by scan_plain with LONG set. NOPREFIX and
 * NODECODE take the prefix extractor and the version decode out of
 * scan_versioned. load_classed builds a store as sm1 builds a
 * chained one: class-byte keys under sm1's comparator and prefix
 * extractor, values with a chained version's 29-byte header; read
 * by scan_classed as sm1::chain_scan reads, with a snapshot, auto
 * prefix mode and an upper bound, which NOSNAP, NOAUTO and NOUPPER
 * each take away. CLS_BYTEWISE builds and reads it under RocksDB's
 * comparator with no prefix extractor, CLS_NOHEADER with no version
 * header, CLS_HEADER=n with an n-byte one, CLS_RANDOM with its bytes
 * random, each in a store of its own.
 *
 * Variants: seek (today: sm1's comparator and prefix bloom, a new
 * iterator a read), seek_reuse (one iterator for every read),
 * seek_r4 (seek with block_restart_interval 4), get (bytewise,
 * whole-key bloom), get_hash (get with the data block hash index).
 * scan_versioned and scan_plain read every row in key order, as a
 * sequential scan does, and report the time a row.
 * With THRASH=BYTES in the environment, BYTES of memory are touched
 * between reads, as a query's parse and plan evict the CPU caches,
 * and only the reads are timed.
 * Build: see Makefile.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include "sm0_codec.h"
#include "version.h"

namespace {

constexpr uint64_t nrows = 1000000;     /* rows in each store */
constexpr uint64_t nreads = 2000000;    /* timed reads */
constexpr size_t value_len = 500;       /* about a wide_mixed tuple */
constexpr uint16_t heap_relid = 0x0001; /* the row rel's relid */

/* The row key of id: relid then be64(id), as sm2 encodes it. */
std::string row_key(uint64_t id) {
  std::string k;
  k.push_back(static_cast<char>(heap_relid >> 8));
  k.push_back(static_cast<char>(heap_relid & 0xff));
  sm0::put_be64(k, id);
  return k;
}

/* A value of value_len bytes, fixed by id, incompressible enough. */
std::string row_value(uint64_t id) {
  std::string v(value_len, '\0');
  std::mt19937_64 g(id);
  for (size_t i = 0; i + 8 <= v.size(); i += 8) {
    uint64_t x = g();
    memcpy(&v[i], &x, 8);
  }
  return v;
}

/* The block cache both stores share: room for every block. */
std::shared_ptr<rocksdb::Cache> cache() {
  static std::shared_ptr<rocksdb::Cache> c = rocksdb::NewLRUCache(2ull << 30);
  return c;
}

/* Versioned keys under RocksDB's bytewise comparator, not sm1's. */
bool bytewise = false;

/* Options of sm1's store CF (sm1 db.cc store_cf_options), or VCHAIN's. */
rocksdb::Options options(bool versioned, bool hash, int restart) {
  rocksdb::Options o;
  rocksdb::BlockBasedTableOptions t;
  o.create_if_missing = true;
  o.compression = rocksdb::kNoCompression;
  t.block_cache = cache();
  t.block_size = 16 << 10;
  t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  t.block_restart_interval = restart;
  if (versioned) {
    // Today: versions in the key, a bloom filter on the userkey.
    if (!bytewise)
      o.comparator = wisr::version_comparator();
    o.prefix_extractor = wisr::version_prefix_extractor();
    t.whole_key_filtering = false;
  } else if (hash) {
    // A point lookup finds its key in a block by hash.
    t.data_block_index_type =
        rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
  }
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
  return o;
}

/* Open dir with o, or exit. */
rocksdb::DB *open(const std::string &dir, const rocksdb::Options &o) {
  rocksdb::DB *db = nullptr;
  rocksdb::Status s = rocksdb::DB::Open(o, dir, &db);
  if (!s.ok()) {
    fprintf(stderr, "open %s: %s\n", dir.c_str(), s.ToString().c_str());
    exit(1);
  }
  return db;
}

/* Write N rows into a store at dir, then compact it to one level. */
void load(const std::string &dir, const rocksdb::Options &o, bool versioned) {
  std::unique_ptr<rocksdb::DB> db(open(dir, o));
  rocksdb::WriteOptions w;
  w.disableWAL = true;
  for (uint64_t id = 1; id <= nrows; id++) {
    // One committed version, as sm1 holds a row once compacted.
    std::string k = row_key(id);
    std::string v = row_value(id);
    rocksdb::Status s =
        versioned ? db->Put(w, wisr::version_key(k, 100, 1),
                            wisr::version_value(wisr::version_stamped, 50, v))
                  : db->Put(w, k, v);
    if (!s.ok()) {
      fprintf(stderr, "put: %s\n", s.ToString().c_str());
      exit(1);
    }
  }
  // Settle the store, as the bench's VACUUM does (sm_tbl_compact).
  rocksdb::CompactRangeOptions c;
  c.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  db->CompactRange(c, nullptr, nullptr);
}

/* THRASH's buffer, touched between reads; empty without THRASH. */
std::vector<char> thrash;

/* The nanoseconds the timed reads took, THRASH's touches left out. */
double read_ns = 0;

/* Touch every cache line of thrash, so the next read starts cold. */
void evict() {
  static volatile char sink;
  for (size_t i = 0; i < thrash.size(); i += 64)
    thrash[i]++;
  sink = thrash.empty() ? 0 : thrash[0];
}

/* Read one key as variant does; whether its row was found. */
bool read_one(rocksdb::DB *db, const std::string &variant, uint64_t id,
              rocksdb::Iterator *kept);

/* Read the keys in ids as variant does; the count of rows found. */
uint64_t run(rocksdb::DB *db, const std::string &variant,
             const std::vector<uint64_t> &ids) {
  uint64_t found = 0;
  if (!thrash.empty()) {
    rocksdb::ReadOptions ro;
    ro.prefix_same_as_start = true;
    std::unique_ptr<rocksdb::Iterator> kept;
    if (variant == "seek_reuse")
      kept.reset(db->NewIterator(ro));
    read_ns = 0;
    for (uint64_t id : ids) {
      evict();
      auto t0 = std::chrono::steady_clock::now();
      found += read_one(db, variant, id, kept.get());
      auto t1 = std::chrono::steady_clock::now();
      read_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
    return found;
  }
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  if (variant == "get" || variant == "get_hash") {
    rocksdb::ReadOptions gro;
    rocksdb::PinnableSlice v;
    for (uint64_t id : ids) {
      // One Get of the plain key, as CURR would be read.
      v.Reset();
      if (db->Get(gro, db->DefaultColumnFamily(), row_key(id), &v).ok())
        found += v.size() == value_len;
    }
    return found;
  }
  std::unique_ptr<rocksdb::Iterator> kept;
  if (variant == "seek_reuse")
    kept.reset(db->NewIterator(ro));
  for (uint64_t id : ids) {
    std::string k = row_key(id);
    // The newest version a reader at ts 1000 sees: sm1 access_read.
    std::string target = wisr::version_key(k, 1000, UINT32_MAX);
    std::unique_ptr<rocksdb::Iterator> fresh;
    rocksdb::Iterator *it = kept.get();
    if (!it) {
      fresh.reset(db->NewIterator(ro));
      it = fresh.get();
    }
    it->Seek(target);
    if (it->Valid() && wisr::version_userkey(it->key()) == rocksdb::Slice(k)) {
      wisr::version_t v;
      if (wisr::version_decode_value(it->value(), &v))
        found += v.payload.size() == value_len;
    }
  }
  return found;
}

/* Read one key as variant does; whether its row was found. */
bool read_one(rocksdb::DB *db, const std::string &variant, uint64_t id,
              rocksdb::Iterator *kept) {
  std::string k = row_key(id);
  if (variant == "get" || variant == "get_hash") {
    rocksdb::PinnableSlice v;
    return db->Get(rocksdb::ReadOptions(), db->DefaultColumnFamily(), k, &v)
               .ok() &&
           v.size() == value_len;
  }
  rocksdb::ReadOptions ro;
  ro.prefix_same_as_start = true;
  std::unique_ptr<rocksdb::Iterator> fresh;
  rocksdb::Iterator *it = kept;
  if (!it) {
    fresh.reset(db->NewIterator(ro));
    it = fresh.get();
  }
  it->Seek(wisr::version_key(k, 1000, UINT32_MAX));
  wisr::version_t v;
  return it->Valid() && wisr::version_userkey(it->key()) == rocksdb::Slice(k) &&
         wisr::version_decode_value(it->value(), &v) &&
         v.payload.size() == value_len;
}

} // namespace

/* Load both stores, or time one variant's reads. */
int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: getseek load|VARIANT DIR\n");
    return 2;
  }
  std::string what = argv[1], dir = argv[2];
  if (what == "load") {
    load(dir + "/versioned", options(true, false, 16), true);
    load(dir + "/versioned_r4", options(true, false, 4), true);
    load(dir + "/plain", options(false, false, 16), false);
    load(dir + "/plain_hash", options(false, true, 16), false);
    bytewise = true;
    load(dir + "/versioned_bw", options(true, false, 16), true);
    what = "load_long";
  }
  if (what == "load_long") {
    // Plain keys padded with a version tail: the tail's cost alone.
    std::unique_ptr<rocksdb::DB> db(
        open(dir + "/plain_long", options(false, false, 16)));
    rocksdb::WriteOptions w;
    w.disableWAL = true;
    for (uint64_t id = 1; id <= nrows; id++)
      db->Put(w, wisr::version_key(row_key(id), 100, 1), row_value(id));
    rocksdb::CompactRangeOptions c;
    c.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
    db->CompactRange(c, nullptr, nullptr);
    return 0;
  }
  // The classed store's options and directory, as CLS_ toggles say.
  bool cls_bw = getenv("CLS_BYTEWISE") != nullptr;
  size_t cls_header = getenv("CLS_NOHEADER") ? 0
                      : getenv("CLS_HEADER")
                          ? strtoul(getenv("CLS_HEADER"), nullptr, 10)
                          : 29;
  std::string cls_dir = dir + "/classed" + (cls_bw ? "_bw" : "") + "_h" +
                        std::to_string(cls_header) +
                        (getenv("CLS_RANDOM") ? "_r" : "");
  rocksdb::Options co = options(!cls_bw, false, 16);
  if (what == "load_classed") {
    // As sm1 builds a chained store: current keys, a version header.
    std::unique_ptr<rocksdb::DB> db(open(cls_dir, co));
    rocksdb::WriteOptions w;
    w.disableWAL = true;
    std::string header(cls_header, '\0');
    if (cls_header)
      header[0] = static_cast<char>(wisr::version_stamped);
    // CLS_RANDOM: the header's bytes random, as the payload's are.
    bool random_header = getenv("CLS_RANDOM") != nullptr;
    for (uint64_t id = 1; id <= nrows; id++) {
      if (random_header)
        header = row_value(id + nrows).substr(0, cls_header);
      db->Put(w, std::string(1, wisr::key_class_current) + row_key(id),
              header + row_value(id));
    }
    rocksdb::CompactRangeOptions c;
    c.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
    db->CompactRange(c, nullptr, nullptr);
    return 0;
  }
  if (what == "scan_classed") {
    co.create_if_missing = false;
    std::unique_ptr<rocksdb::DB> db(open(cls_dir, co));
    double best = 0;
    for (int pass = 0; pass < 4; pass++) {
      // As sm1::chain_scan reads: a snapshot, auto prefix mode, the
      // end of the current keys as the upper bound.
      rocksdb::ReadOptions ro;
      const rocksdb::Snapshot *snap =
          getenv("NOSNAP") ? nullptr : db->GetSnapshot();
      ro.snapshot = snap;
      ro.auto_prefix_mode = !getenv("NOAUTO");
      std::string upper(1, wisr::key_class_old);
      rocksdb::Slice upper_slice(upper);
      if (!getenv("NOUPPER"))
        ro.iterate_upper_bound = &upper_slice;
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
      uint64_t rows = 0;
      size_t bytes = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (it->Seek(std::string(1, wisr::key_class_current)); it->Valid();
           it->Next()) {
        bytes += it->value().size() - cls_header;
        rows++;
      }
      auto t1 = std::chrono::steady_clock::now();
      it.reset();
      if (snap)
        db->ReleaseSnapshot(snap);
      double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
      if (pass > 0 && (best == 0 || ns < best))
        best = ns;
      if (rows != nrows || bytes != nrows * value_len)
        return 1;
    }
    printf("%-14s %6.1f ns/row\n", what.c_str(), best / nrows);
    return 0;
  }
  if (what == "load_bw") {
    bytewise = true;
    load(dir + "/versioned_bw", options(true, false, 16), true);
    return 0;
  }
  // The _bw variants: versioned keys, the bytewise comparator.
  if (what.size() > 3 && 0 == what.compare(what.size() - 3, 3, "_bw")) {
    bytewise = true;
    what.resize(what.size() - 3);
  }
  rocksdb::Options o;
  std::string sub;
  if (what == "scan_versioned" || what == "scan_plain") {
    bool versioned = what == "scan_versioned";
    o = options(versioned, false, 16);
    o.create_if_missing = false;
    // Experiments: no prefix extractor; no version decode.
    if (getenv("NOPREFIX"))
      o.prefix_extractor.reset();
    bool decode = !getenv("NODECODE");
    std::unique_ptr<rocksdb::DB> db(
        open(dir + (versioned ? (bytewise ? "/versioned_bw" : "/versioned")
                              : (getenv("LONG") ? "/plain_long" : "/plain")),
             o));
    double best = 0;
    for (int pass = 0; pass < 4; pass++) {
      // As sm1's scan reads: a snapshot, every key in order.
      rocksdb::ReadOptions ro;
      ro.auto_prefix_mode = versioned && !getenv("NOAUTO");
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
      uint64_t rows = 0;
      size_t bytes = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        if (versioned && decode) {
          // Decode the version, as sm1's scan does for visibility.
          wisr::version_t v;
          wisr::version_decode_key(it->key(), &v);
          wisr::version_decode_value(it->value(), &v);
          bytes += v.payload.size();
        } else {
          // A version's value carries 9 bytes of header before the row.
          bytes += it->value().size() - (versioned ? 9 : 0);
        }
        rows++;
      }
      auto t1 = std::chrono::steady_clock::now();
      double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
      // The first pass warms the cache; the best of the rest counts.
      if (pass > 0 && (best == 0 || ns < best))
        best = ns;
      if (rows != nrows || bytes != nrows * value_len)
        return 1;
    }
    printf("%-14s %6.1f ns/row\n", what.c_str(), best / nrows);
    return 0;
  }
  if (what == "seek" || what == "seek_reuse") {
    o = options(true, false, 16);
    sub = bytewise ? "versioned_bw" : "versioned";
  } else if (what == "seek_r4") {
    o = options(true, false, 4);
    sub = "versioned_r4";
  } else if (what == "get") {
    o = options(false, false, 16);
    sub = "plain";
  } else if (what == "get_hash") {
    o = options(false, true, 16);
    sub = "plain_hash";
  } else {
    fprintf(stderr, "no variant %s\n", what.c_str());
    return 2;
  }
  o.create_if_missing = false;
  std::unique_ptr<rocksdb::DB> db(open(dir + "/" + sub, o));
  std::vector<uint64_t> ids(nreads);
  std::mt19937_64 g(42);
  for (uint64_t &id : ids)
    id = g() % nrows + 1;
  // Warm the block cache with every block, then time the reads.
  std::vector<uint64_t> all(nrows);
  for (uint64_t i = 0; i < nrows; i++)
    all[i] = i + 1;
  run(db.get(), what, all);
  if (const char *t = getenv("THRASH"))
    thrash.assign(strtoull(t, nullptr, 10), 0);
  auto t0 = std::chrono::steady_clock::now();
  uint64_t found = run(db.get(), what, ids);
  auto t1 = std::chrono::steady_clock::now();
  double ns = thrash.empty()
                  ? std::chrono::duration<double, std::nano>(t1 - t0).count()
                  : read_ns;
  printf("%-10s %8.1f ns/read  found %llu of %llu\n", what.c_str(), ns / nreads,
         (unsigned long long)found, (unsigned long long)nreads);
  return found == nreads ? 0 : 1;
}
