/*
 * smbench.cc -- the floor of a scan and of a point read in sm, layer
 * by layer, on the same rows: sm1 alone, versioned and chained
 * stores (../../sm/VCHAIN.md), then sm as digres uses it, a table of
 * sm3 with a primary key; and under them RocksDB alone, as if sm
 * went to it directly: the bare rows under its own comparator, and
 * the rows as sm1 lays a chained one, class byte, sm1's comparator
 * and prefix extractor, and a chained version's 29-byte header.
 * What digres adds is what its own timings of the same rows add to
 * the sm3 table's.
 *
 *   smbench [rows] [pad bytes] [reads] [churn]
 *
 * churn, as "20,10,5", gives the shares of rows, in percent, that
 * have one, two and three older versions: after the load a session
 * holds a snapshot, as a long report would, and three passes update
 * the rows of each share, a row picked by a hash of its id; a
 * compaction then keeps what the snapshot needs. The scans and reads
 * are timed at a new snapshot, which sees each row's newest version,
 * and at the held one, which reaches back to its first. "0" is a
 * fresh table.
 *
 * Every layer is loaded with the same rows, t (key int8 primary key,
 * x int8, pad text): key 1 to rows, x = 3 key, and pad bytes more;
 * bulk loaded and compacted through GC, as the bench's VACUUM leaves
 * them, and read through a 2 GB block cache, as the bench's digres
 * has in shared_buffers. Below the table, a row's value is be64(x)
 * then the pad. A scan is timed per row; a point read is SELECT sum(x)
 * WHERE key = k for a random k, its x decoded and summed, timed per
 * read over reads of them: in one transaction, and in a transaction
 * each, as a digres statement is.
 */

#include "sm1.h"
#include "sm2.h"
#include "sm3.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

namespace {

/* What the reads read, kept so they are not optimized away. */
volatile uint64_t g_sink = 0;

/* Fail the run with sm1's message. */
void need(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "smbench: %s: %s\n", what, sm1::errmsg());
    exit(1);
  }
}

/* The churn bucket of row id, 0 to 99, the same in SQL (sqlbench.sh). */
unsigned bucket(uint64_t id) { return (id * 2654435761ULL) % 100; }

/* How many older versions row id gets under churn shares sh. */
int olds(uint64_t id, const unsigned sh[3]) {
  unsigned b = bucket(id);
  return b < sh[2] ? 3 : b < sh[1] + sh[2] ? 2 : b < sh[0] + sh[1] + sh[2] ? 1
                                                                          : 0;
}

/* The median of fn's results over runs calls. */
double median(int runs, const std::function<double()> &fn) {
  std::vector<double> v;
  for (int r = 0; r < runs; r++) {
    v.push_back(fn());
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

/* Nanoseconds since t0. */
double since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::nano>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/* A row's x, as every layer holds it. */
uint64_t x_of(uint64_t key) { return 3 * key; }

/* A row's value below the table: be64(x), then pad bytes. */
std::string row_value(uint64_t key, size_t pad, char fill) {
  std::string v;
  for (int i = 7; i >= 0; i--) {
    v.push_back(static_cast<char>(x_of(key) >> (8 * i)));
  }
  v.append(pad, fill);
  return v;
}

/* The x a row's value below the table holds, at offset off. */
uint64_t x_in(std::string_view v, size_t off) {
  uint64_t x = 0;
  for (size_t i = 0; i < 8; i++) {
    x = x << 8 | static_cast<uint8_t>(v[off + i]);
  }
  return x;
}

/* An sm1 row's key, as a table's row has one: relid, then be64 id. */
std::string row_key(uint64_t id) {
  std::string k = {'\x00', '\x01'};
  for (int i = 7; i >= 0; i--) {
    k.push_back(static_cast<char>(id >> (8 * i)));
  }
  return k;
}

/* An sm1 store, chained or not, of rows ids with pad-byte values. */
uint64_t sm1_fill(sm1::session_t *s, bool chained, uint64_t rows,
                  size_t pad) {
  uint64_t store = 0;
  need(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  need(sm1::mkstore(s, store, false, chained) == 0, "mkstore");
  need(sm1::commit(s) == 0, "commit");
  need(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  sm1::loader_t *l = nullptr;
  size_t stream = 0;
  need(sm1::load_open(s, store, l) == 0, "load_open");
  need(sm1::load_stream(l, true, stream, chained) == 0, "load_stream");
  for (uint64_t i = 1; i <= rows; i++) {
    need(sm1::load_put(l, stream, row_key(i), row_value(i, pad, 'v')) == 0,
         "load_put");
  }
  need(sm1::load_close(l) == 0, "load_close");
  need(sm1::commit(s) == 0, "commit");
  need(sm1::compact(s, store) == 0, "compact");
  return store;
}

/*
 * Churn an sm1 store: three passes, each a transaction, the n-th
 * updating every row with at least n older versions to come.
 */
void sm1_churn(sm1::session_t *s, uint64_t store, bool chained, uint64_t rows,
               size_t pad, const unsigned sh[3]) {
  for (int pass = 1; pass <= 3; pass++) {
    need(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
    for (uint64_t i = 1; i <= rows; i++) {
      if (olds(i, sh) >= pass) {
        std::string k = row_key(i);
        std::string value = row_value(i, pad, 'u');
        need((chained ? sm1::chain_update(s, store, k, value)
                      : sm1::update(s, store, k, value)) == 0,
             "update");
      }
    }
    need(sm1::commit(s) == 0, "commit");
  }
  need(sm1::compact(s, store) == 0, "compact");
}

/*
 * ns a row of a full scan of an sm1 store, in a transaction of its
 * own, or with open in the session's own, already begun.
 */
double sm1_scan(sm1::session_t *s, uint64_t store, bool chained,
                uint64_t rows, bool open = false) {
  if (!open) {
    need(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0, "begin");
  }
  auto t0 = std::chrono::steady_clock::now();
  sm1::scan_t *c = nullptr;
  need((chained ? sm1::chain_scan(s, store, nullptr, 0, 0, nullptr, 0, 0, c)
                : sm1::scan(s, store, nullptr, 0, 0, nullptr, 0, 0, c)) == 0,
       "scan");
  std::string_view k, v;
  uint64_t n = 0, sum = 0;
  while (sm1::scannxt(c, k, v) == 0) {
    n++;
    sum += v.size();
  }
  sm1::scanend(c);
  double ns = since(t0);
  if (!open) {
    need(sm1::commit(s) == 0, "commit");
  }
  need(n == rows, "scan row count");
  g_sink = g_sink + sum;
  return ns / static_cast<double>(n);
}

/*
 * ns a read of an sm1 store's rows at ids, in one transaction, or
 * with each a transaction begun at READ COMMITTED and committed.
 */
double sm1_reads(sm1::session_t *s, uint64_t store, bool chained,
                 const std::vector<uint64_t> &ids, bool each,
                 bool open = false) {
  std::vector<std::string> keys;
  for (uint64_t id : ids) {
    keys.push_back(row_key(id));
  }
  if (!each && !open) {
    need(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0, "begin");
  }
  auto t0 = std::chrono::steady_clock::now();
  uint64_t sum = 0;
  for (const std::string &k : keys) {
    if (each) {
      need(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
    }
    std::string_view v;
    need((chained ? sm1::chain_read(s, store, k, v)
                  : sm1::read(s, store, k, v)) == 0,
         "read");
    sum += x_in(v, 0);
    if (each) {
      need(sm1::commit(s) == 0, "commit");
    }
  }
  double ns = since(t0);
  if (!each && !open) {
    need(sm1::commit(s) == 0, "commit");
  }
  g_sink = g_sink + sum;
  return ns / static_cast<double>(keys.size());
}

/*
 * A RocksDB of its own at dir with sm1's store options: no
 * compression, 16 KB blocks, a ten-bit bloom filter, the 2 GB cache;
 * with classed, sm1's comparator and prefix extractor, the filter on
 * the prefix, as a store CF has (sm1 db.cc store_cf_options).
 */
rocksdb::DB *raw_open(const std::string &dir, bool classed) {
  static std::shared_ptr<rocksdb::Cache> cache =
      rocksdb::NewLRUCache(2ULL << 30);
  rocksdb::Options o;
  rocksdb::BlockBasedTableOptions t;
  o.create_if_missing = true;
  o.compression = rocksdb::kNoCompression;
  t.block_cache = cache;
  t.block_size = 16 << 10;
  t.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  if (classed) {
    o.comparator = wisr::version_comparator();
    o.prefix_extractor = wisr::version_prefix_extractor();
    t.whole_key_filtering = false;
  }
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
  rocksdb::DB *db = nullptr;
  rocksdb::Status st = rocksdb::DB::Open(o, dir, &db);
  if (!st.ok()) {
    fprintf(stderr, "smbench: open %s: %s\n", dir.c_str(), st.ToString().c_str());
    exit(1);
  }
  return db;
}

/* The key a raw store keeps row id under: classed, a current key. */
std::string raw_key(uint64_t id, bool classed) {
  return classed ? std::string(1, wisr::key_class_current) + row_key(id)
                 : row_key(id);
}

/*
 * Write rows rows of pad bytes into a raw store, compacted to one
 * level: classed, each after a stamped chained version's header.
 */
void raw_fill(rocksdb::DB *db, bool classed, uint64_t rows, size_t pad) {
  rocksdb::WriteOptions w;
  w.disableWAL = true;
  std::string header(classed ? 29 : 0, '\0');
  if (classed) {
    header[0] = static_cast<char>(wisr::version_stamped);
  }
  for (uint64_t i = 1; i <= rows; i++) {
    if (!db->Put(w, raw_key(i, classed), header + row_value(i, pad, 'v'))
             .ok()) {
      fprintf(stderr, "smbench: put failed\n");
      exit(1);
    }
  }
  rocksdb::CompactRangeOptions c;
  c.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  db->CompactRange(c, nullptr, nullptr);
}

/*
 * ns a row of a full scan of a raw store: classed, as sm1 reads a
 * chained store, with a snapshot, auto prefix mode and an upper bound.
 */
double raw_scan(rocksdb::DB *db, bool classed, uint64_t rows) {
  rocksdb::ReadOptions ro;
  const rocksdb::Snapshot *snap = classed ? db->GetSnapshot() : nullptr;
  std::string upper(1, wisr::key_class_old);
  rocksdb::Slice upper_slice(upper);
  if (classed) {
    ro.snapshot = snap;
    ro.auto_prefix_mode = true;
    ro.iterate_upper_bound = &upper_slice;
  }
  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  uint64_t n = 0, sum = 0;
  for (classed ? it->Seek(std::string(1, wisr::key_class_current))
               : it->SeekToFirst();
       it->Valid(); it->Next()) {
    n++;
    sum += it->value().size();
  }
  it.reset();
  double ns = since(t0);
  if (snap) {
    db->ReleaseSnapshot(snap);
  }
  need(n == rows, "raw scan row count");
  g_sink = g_sink + sum;
  return ns / static_cast<double>(n);
}

/* ns a Get of a raw store's rows at ids. */
double raw_reads(rocksdb::DB *db, bool classed,
                 const std::vector<uint64_t> &ids) {
  std::vector<std::string> keys;
  for (uint64_t id : ids) {
    keys.push_back(raw_key(id, classed));
  }
  rocksdb::ReadOptions ro;
  rocksdb::PinnableSlice v;
  auto t0 = std::chrono::steady_clock::now();
  uint64_t sum = 0;
  for (const std::string &k : keys) {
    v.Reset();
    need(db->Get(ro, db->DefaultColumnFamily(), k, &v).ok(), "raw get");
    sum += x_in(std::string_view(v.data(), v.size()), classed ? 29 : 0);
  }
  double ns = since(t0);
  g_sink = g_sink + sum;
  return ns / static_cast<double>(keys.size());
}

/* The table t (key int8 primary key, x int8, pad text), loaded. */
uint64_t sm3_fill(sm3::session_t *s, uint64_t rows, size_t pad) {
  uint64_t oid = 0, pkey = 0;
  std::vector<sm3::column_spec_t> cols = {
      {"key", sm2::type_int8.code, -1, true},
      {"x", sm2::type_int8.code, -1, false},
      {"pad", sm2::type_text.code, -1, false}};
  std::vector<sm3::index_key_spec_t> by_id = {{0, false, false, std::nullopt}};
  need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  need(sm3::create_table(s, sm3::nsp_public, "t", cols, oid) == 0,
       "create_table");
  need(sm3::create_index(s, oid, "t_pkey", by_id, {}, true, pkey, true,
                         true) == 0,
       "create_index");
  need(sm3::commit(s) == 0, "commit");
  need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  sm3::table_t *t = nullptr;
  need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IX, t) == 0, "tbl_open");
  sm3::loader_t *l = nullptr;
  need(sm3::tbl_load_open(t, l) == 0, "tbl_load_open");
  std::string text = sm2::make_varlena(std::string(pad, 'p'));
  bool isnull[3] = {false, false, false};
  for (uint64_t i = 1; i <= rows; i++) {
    sm2::datum_t d[3] = {sm2::datum_from_int64(static_cast<int64_t>(i)),
                         sm2::datum_from_int64(static_cast<int64_t>(x_of(i))),
                         sm2::datum_from_ptr(text.data())};
    uint64_t id = 0;
    need(sm3::tbl_load_add(l, d, isnull, id) == 0, "tbl_load_add");
  }
  need(sm3::tbl_load_close(l) == 0, "tbl_load_close");
  sm3::tbl_close(t);
  need(sm3::commit(s) == 0, "commit");
  need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0, "tbl_open");
  need(sm3::tbl_compact(t) == 0, "tbl_compact");
  sm3::tbl_close(t);
  need(sm3::commit(s) == 0, "commit");
  return oid;
}

/* Churn t as sm1_churn does an sm1 store, through sm3's updates. */
void sm3_churn(sm3::session_t *s, uint64_t oid, uint64_t rows, size_t pad,
               const unsigned sh[3]) {
  std::string text = sm2::make_varlena(std::string(pad, 'u'));
  bool isnull[3] = {false, false, false};
  for (int pass = 1; pass <= 3; pass++) {
    need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
    sm3::table_t *t = nullptr;
    need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IX, t) == 0, "tbl_open");
    for (uint64_t i = 1; i <= rows; i++) {
      if (olds(i, sh) >= pass) {
        sm2::datum_t d[3] = {
            sm2::datum_from_int64(static_cast<int64_t>(i)),
            sm2::datum_from_int64(static_cast<int64_t>(x_of(i))),
            sm2::datum_from_ptr(text.data())};
        sm3::row_key_t key{{d, 1}, {isnull, 1}};
        need(sm3::tbl_update(t, key, d, isnull) == 0, "tbl_update");
      }
    }
    sm3::tbl_close(t);
    need(sm3::commit(s) == 0, "commit");
  }
  need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
  sm3::table_t *t = nullptr;
  need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0, "tbl_open");
  need(sm3::tbl_compact(t) == 0, "tbl_compact");
  sm3::tbl_close(t);
  need(sm3::commit(s) == 0, "commit");
}

/*
 * ns a row of a full scan of t through sm3: with sum, each row's x
 * decoded and summed, as SELECT sum(x) does; else rows counted. With
 * open, in the session's transaction, already begun.
 */
double sm3_scan(sm3::session_t *s, uint64_t oid, uint64_t rows, bool sum,
                bool open = false) {
  if (!open) {
    need(sm3::begin(s, sm1::xctlevel_t::repeatable_read) == 0, "begin");
  }
  auto t0 = std::chrono::steady_clock::now();
  sm3::table_t *t = nullptr;
  need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0, "tbl_open");
  const sm2::tup_desc_t &desc = sm3::tbl_def(t).desc;
  sm3::scan_t *c = nullptr;
  need(sm3::tbl_scan_rows(t, c) == 0, "tbl_scan_rows");
  uint64_t id, n = 0, total = 0;
  std::string_view tuple;
  while (sm3::tbl_scannxt(c, id, tuple) == 0) {
    n++;
    if (sum) {
      sm2::datum_t d;
      bool null;
      need(sm2::tup_decode(desc, tuple, 1, d, null) == 0, "tup_decode");
      total += static_cast<uint64_t>(sm2::datum_to_int64(d));
    }
  }
  sm3::tbl_scanend(c);
  sm3::tbl_close(t);
  double ns = since(t0);
  if (!open) {
    need(sm3::commit(s) == 0, "commit");
  }
  need(n == rows, "sm3 scan row count");
  g_sink = g_sink + total;
  return ns / static_cast<double>(n);
}

/*
 * ns a read of t's rows at ids by primary key, each one's x decoded
 * and summed: in one transaction on an open table, or each a
 * statement's worth -- a transaction begun at READ COMMITTED, the
 * table opened, the row read, the table closed, the transaction
 * committed.
 */
double sm3_reads(sm3::session_t *s, uint64_t oid,
                 const std::vector<uint64_t> &ids, bool each,
                 bool open = false) {
  sm3::table_t *t = nullptr;
  if (!each) {
    if (!open) {
      need(sm3::begin(s, sm1::xctlevel_t::repeatable_read) == 0, "begin");
    }
    need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0, "tbl_open");
  }
  auto t0 = std::chrono::steady_clock::now();
  uint64_t total = 0;
  bool isnull[1] = {false};
  for (uint64_t id : ids) {
    if (each) {
      need(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0, "begin");
      need(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IS, t) == 0, "tbl_open");
    }
    sm2::datum_t d[1] = {sm2::datum_from_int64(static_cast<int64_t>(id))};
    sm3::row_key_t key{d, isnull};
    std::string_view tuple;
    need(sm3::tbl_read(t, key, tuple) == 0, "tbl_read");
    sm2::datum_t x;
    bool xnull;
    need(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 1, x, xnull) == 0,
         "tup_decode");
    total += static_cast<uint64_t>(sm2::datum_to_int64(x));
    if (each) {
      sm3::tbl_close(t);
      need(sm3::commit(s) == 0, "commit");
    }
  }
  double ns = since(t0);
  if (!each) {
    sm3::tbl_close(t);
    if (!open) {
      need(sm3::commit(s) == 0, "commit");
    }
  }
  g_sink = g_sink + total;
  return ns / static_cast<double>(ids.size());
}

} // namespace

/* Load every layer, then time its scans and reads. */
int main(int argc, char **argv) {
  uint64_t rows = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1000000;
  size_t pad = argc > 2 ? strtoull(argv[2], nullptr, 10) : 60;
  size_t nreads = argc > 3 ? strtoull(argv[3], nullptr, 10) : 200000;
  unsigned sh[3] = {0, 0, 0};
  if (argc > 4) {
    sscanf(argv[4], "%u,%u,%u", &sh[0], &sh[1], &sh[2]);
  }
  const bool churn = sh[0] + sh[1] + sh[2] > 0;
  const int runs = 5;
  std::string home =
      (std::filesystem::temp_directory_path() / "smbench").string();
  std::filesystem::remove_all(home);
  std::filesystem::create_directories(home);
  need(sm1::init(home) == 0, "init");
  sm1::env_t *env = nullptr;
  // A block cache as the bench's digres has, its shared_buffers.
  sm1::env_options_t eopt;
  eopt.block_cache_bytes = 2LL << 30;
  need(sm1::attach(home, env, &eopt) == 0, "attach");
  need(sm1::mkdb(env, "db", nullptr) == 0, "mkdb");
  sm1::session_t *s1 = nullptr;
  need(sm1::opendb(env, "db", s1) == 0, "opendb");
  need(sm3::cat_init(env, "db") == 0, "cat_init");
  sm3::catalog_t *cat = nullptr;
  need(sm3::cat_open(env, "db", cat) == 0, "cat_open");
  sm1::session_t *s3raw = nullptr;
  need(sm1::opendb(env, "db", s3raw) == 0, "opendb");
  sm3::session_t *s3 = nullptr;
  need(sm3::session_open(cat, s3raw, s3) == 0, "session_open");

  std::unique_ptr<rocksdb::DB> raw_plain(raw_open(home + "/raw_plain", false));
  std::unique_ptr<rocksdb::DB> raw_classed(
      raw_open(home + "/raw_classed", true));
  raw_fill(raw_plain.get(), false, rows, pad);
  raw_fill(raw_classed.get(), true, rows, pad);
  uint64_t versioned = sm1_fill(s1, false, rows, pad);
  uint64_t chained = sm1_fill(s1, true, rows, pad);
  uint64_t table = sm3_fill(s3, rows, pad);

  // The held snapshot, an sm1 session's for the stores and an sm3
  // session's for the table, taken before the churn.
  sm1::session_t *h1 = nullptr;
  need(sm1::opendb(env, "db", h1) == 0, "opendb");
  sm1::session_t *h3raw = nullptr;
  need(sm1::opendb(env, "db", h3raw) == 0, "opendb");
  sm3::session_t *h3 = nullptr;
  need(sm3::session_open(cat, h3raw, h3) == 0, "session_open");
  if (churn) {
    need(sm1::begin(h1, sm1::xctlevel_t::repeatable_read) == 0, "begin");
    need(sm3::begin(h3, sm1::xctlevel_t::repeatable_read) == 0, "begin");
    sm1_scan(h1, versioned, false, rows, true);
    sm3_scan(h3, table, rows, false, true);
    sm1_churn(s1, versioned, false, rows, pad, sh);
    sm1_churn(s1, chained, true, rows, pad, sh);
    sm3_churn(s3, table, rows, pad, sh);
  }

  // The keys each layer reads, random, the same for all.
  std::mt19937_64 g(42);
  std::vector<uint64_t> ids(nreads);
  for (uint64_t &id : ids) {
    id = 1 + g() % rows;
  }
  // A pass of each to warm the block cache.
  raw_scan(raw_plain.get(), false, rows);
  raw_scan(raw_classed.get(), true, rows);
  sm1_scan(s1, versioned, false, rows);
  sm1_scan(s1, chained, true, rows);
  sm3_scan(s3, table, rows, false);

  printf("%llu rows, pad %zu bytes, %zu reads of a random key, summing x; "
         "churn %u%% %u%% %u%% with 1, 2, 3 older versions; medians of %d\n",
         static_cast<unsigned long long>(rows), pad, nreads, sh[0], sh[1],
         sh[2], runs);
  printf("%-34s %10s %12s %12s\n", "layer", "scan ns/row", "read ns", "read+xact ns");
  auto line = [](const char *name, double scan, double r1, double r2) {
    printf("%-34s %10.1f %12.1f %12.1f\n", name, scan, r1, r2);
  };
  line("RocksDB, bare rows",
       median(runs, [&] { return raw_scan(raw_plain.get(), false, rows); }),
       median(runs, [&] { return raw_reads(raw_plain.get(), false, ids); }),
       0);
  line("RocksDB, laid out as sm1 chained",
       median(runs, [&] { return raw_scan(raw_classed.get(), true, rows); }),
       median(runs, [&] { return raw_reads(raw_classed.get(), true, ids); }),
       0);
  line("sm1, versioned store",
       median(runs, [&] { return sm1_scan(s1, versioned, false, rows); }),
       median(runs, [&] { return sm1_reads(s1, versioned, false, ids, false); }),
       median(runs, [&] { return sm1_reads(s1, versioned, false, ids, true); }));
  line("sm1, chained store",
       median(runs, [&] { return sm1_scan(s1, chained, true, rows); }),
       median(runs, [&] { return sm1_reads(s1, chained, true, ids, false); }),
       median(runs, [&] { return sm1_reads(s1, chained, true, ids, true); }));
  line("sm3 table, count",
       median(runs, [&] { return sm3_scan(s3, table, rows, false); }),
       median(runs, [&] { return sm3_reads(s3, table, ids, false); }),
       median(runs, [&] { return sm3_reads(s3, table, ids, true); }));
  line("sm3 table, sum(x)",
       median(runs, [&] { return sm3_scan(s3, table, rows, true); }), 0, 0);
  if (churn) {
    printf("at the held, older snapshot:\n");
    line("sm1, versioned store",
         median(runs, [&] { return sm1_scan(h1, versioned, false, rows, true); }),
         median(runs,
                [&] { return sm1_reads(h1, versioned, false, ids, false, true); }),
         0);
    line("sm1, chained store",
         median(runs, [&] { return sm1_scan(h1, chained, true, rows, true); }),
         median(runs,
                [&] { return sm1_reads(h1, chained, true, ids, false, true); }),
         0);
    line("sm3 table, count",
         median(runs, [&] { return sm3_scan(h3, table, rows, false, true); }),
         median(runs, [&] { return sm3_reads(h3, table, ids, false, true); }),
         0);
    need(sm1::commit(h1) == 0, "commit");
    need(sm3::commit(h3) == 0, "commit");
  }

  raw_plain.reset();
  raw_classed.reset();
  sm3::session_close(h3);
  sm1::closedb(h1);
  sm3::session_close(s3);
  sm3::cat_close(cat);
  sm1::closedb(s1);
  sm1::detach(env, 1);
  std::filesystem::remove_all(home);
  return 0;
}
