/*
 * test_stats.cc -- what a store's LSM looks like now: the sizes
 * sm1::approxsize estimates and the runs sm1::probes counts.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm0_codec.h"
#include "sm1.h"
#include "version.h"

#include <string>
#include <vector>

namespace {

/* A database "db" with one committed store. */
class stats_fixture_t {
public:
  /* Create "db", open it, and make the store. */
  stats_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(m_s, sm1::errmsg());
    REQUIRE(sm1::begin(m_s, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(sm1::mkstore(m_s, m_store) == 0);
    REQUIRE(sm1::commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm1::session_t *s() const { return m_s; }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* Insert n rows of width bytes under prefix, in one transaction. */
  void load(char prefix, int n, size_t width) {
    REQUIRE(sm1::begin(m_s, sm1::xctlevel_t::repeatable_read) == 0);
    for (int i = 0; i < n; i++) {
      std::string key(1, prefix);
      sm0::put_be32(key, i);
      // Mostly repeated bytes, which would compress.
      std::string val(width, 'v');
      memcpy(val.data(), key.data(), key.size());
      REQUIRE(sm1::insert(m_s, m_store, key, val) == 0);
    }
    REQUIRE(sm1::commit(m_s) == 0);
  }

  /* Flush the store's memtables to a level-0 file. */
  void flush() {
    REQUIRE(m_s->db->rdb->Flush(rocksdb::FlushOptions(), cf()).ok());
  }

  /*
   * Seal the store's memtable with background work paused, so it
   * stays unflushed until flush_sealed().
   */
  void seal() {
    REQUIRE(m_s->db->rdb->PauseBackgroundWork().ok());
    rocksdb::FlushOptions opt;
    opt.wait = false;
    opt.allow_write_stall = true;
    REQUIRE(m_s->db->rdb->Flush(opt, cf()).ok());
  }

  /* Resume background work, and flush what seal() left sealed. */
  void flush_sealed() {
    REQUIRE(m_s->db->rdb->ContinueBackgroundWork().ok());
    flush();
  }

  /* Compact the whole store into its bottom level. */
  void compact() {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(m_s->db->rdb->CompactRange(opt, cf(), nullptr, nullptr).ok());
  }

  /* The bytes of the store's sorted files, as RocksDB totals them. */
  uint64_t sst_bytes() {
    uint64_t v = 0;
    REQUIRE(m_s->db->rdb->GetIntProperty(
        cf(), rocksdb::DB::Properties::kTotalSstFilesSize, &v));
    return v;
  }

  /*
   * The compression of each sorted file of the column family cf,
   * the store's when cf is null, as RocksDB recorded it.
   */
  std::vector<std::string>
  compressions(rocksdb::ColumnFamilyHandle *cf = nullptr) {
    rocksdb::TablePropertiesCollection props;
    REQUIRE(m_s->db->rdb->GetPropertiesOfAllTables(cf ? cf : this->cf(), &props)
                .ok());
    std::vector<std::string> out;
    for (const auto &[file, p] : props) {
      out.push_back(p->compression_name);
    }
    return out;
  }

  /*
   * The mean size of a data block in each sorted file of the
   * store, as RocksDB recorded the file's data and block count.
   */
  std::vector<uint64_t> mean_block_sizes() {
    rocksdb::TablePropertiesCollection props;
    REQUIRE(m_s->db->rdb->GetPropertiesOfAllTables(cf(), &props).ok());
    std::vector<uint64_t> out;
    for (const auto &[file, p] : props) {
      REQUIRE(p->num_data_blocks > 0);
      out.push_back(p->data_size / p->num_data_blocks);
    }
    return out;
  }

  /* The version of the store's LSM tree, RocksDB's super version. */
  uint64_t tree_version() {
    uint64_t v = 0;
    REQUIRE(m_s->db->rdb->GetIntProperty(
        cf(), rocksdb::DB::Properties::kCurrentSuperVersionNumber, &v));
    return v;
  }

  /*
   * Keep bytes as the whole store's file bytes at the tree's version
   * now, under the range sm1::approxsize asks for with no bounds.
   */
  void plant_files(uint64_t bytes) {
    std::string top(sm1::max_keylen + 1, '\xff');
    wisr::db_store(m_s->db, m_store)
        .files->put(wisr::version_bound(rocksdb::Slice(), '\x00'),
                    wisr::version_bound(rocksdb::Slice(top), '\x00'),
                    tree_version(), bytes);
  }

  /* The store's sm1::probes. */
  uint64_t probes() {
    uint64_t n = 0;
    REQUIRE_MESSAGE(sm1::probes(m_s, m_store, n) == 0, sm1::errmsg());
    return n;
  }

private:
  /* The store's column family. */
  rocksdb::ColumnFamilyHandle *cf() const {
    return wisr::db_store(m_s->db, m_store).cf;
  }

  home_t m_home;
  sm1::session_t *m_s = nullptr;
  uint64_t m_store = 0;
};

/* A 2-byte prefix, big-endian, as sm2 writes a relid. */
std::string two_bytes(uint16_t prefix) {
  return std::string{static_cast<char>(prefix >> 8),
                     static_cast<char>(prefix & 0xff)};
}

/*
 * Insert n rows of width bytes under the 2-byte prefix, keyed from
 * first on, in one transaction; the bytes they put in the memtable,
 * each a version's key and value as sm1 writes them.
 */
uint64_t load_prefix(sm1::session_t *s, uint64_t store, uint16_t prefix, int n,
                     size_t width, int first = 0) {
  uint64_t bytes = 0;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = first; i < first + n; i++) {
    std::string key = two_bytes(prefix);
    sm0::put_be32(key, i);
    std::string val(width, 'v');
    REQUIRE(sm1::insert(s, store, key, val) == 0);
    bytes += wisr::version_key(key, 0, 0).size() +
             wisr::version_value(0, 0, val).size();
  }
  REQUIRE(sm1::commit(s) == 0);
  return bytes;
}

/* sm1::approxsize's memtable bytes of [lo, hi) as 2-byte prefixes, -1 open. */
uint64_t mem_of(sm1::session_t *s, uint64_t store, int lo, int hi,
                uint64_t &disk) {
  std::string l = lo >= 0 ? two_bytes(static_cast<uint16_t>(lo)) : "";
  std::string h = hi >= 0 ? two_bytes(static_cast<uint16_t>(hi)) : "";
  uint64_t mem = 0;
  REQUIRE_MESSAGE(sm1::approxsize(s, store, lo >= 0 ? l.data() : nullptr,
                                  l.size(), hi >= 0 ? h.data() : nullptr,
                                  h.size(), disk, mem) == 0,
                  sm1::errmsg());
  return mem;
}

/* sm1::approxsize of [lo, hi) as one-byte prefixes; 0 is open. */
void size_of(sm1::session_t *s, uint64_t store, char lo, char hi,
             uint64_t &disk, uint64_t &mem) {
  REQUIRE_MESSAGE(sm1::approxsize(s, store, lo ? &lo : nullptr, lo ? 1 : 0,
                                  hi ? &hi : nullptr, hi ? 1 : 0, disk,
                                  mem) == 0,
                  sm1::errmsg());
}

} // namespace

TEST_CASE("sm1_approxsize reports memtable and file bytes apart") {
  // Verify the two parts follow the rows from memtable to file.
  // 1. An empty store is 0 and 0, with no transaction.
  // 2. Rows not yet flushed are in mem only. Its amount is checked
  //    by prefix in the test below.
  // 3. After a flush and a compaction they are in disk only,
  //    and disk is the store's file bytes.
  // 4. An unknown store fails with SM_ERR_NOTFOUND.
  stats_fixture_t f;
  uint64_t disk = 1, mem = 1;
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk == 0);
  CHECK(mem == 0);

  // 2. Load, unflushed.
  f.load('a', 20000, 200);
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk == 0);
  CHECK(mem > 0);

  // 3. Flush and compact.
  f.flush();
  f.compact();
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(mem == 0);
  CHECK(disk > 0);
  CHECK(disk == f.sst_bytes());

  // 4. A storeid with no store.
  CHECK(sm1::approxsize(f.s(), f.store() + 1000, nullptr, 0, nullptr, 0, disk,
                        mem) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
}

TEST_CASE("sm1_approxsize keeps a store's file bytes until its tree changes") {
  // Verify the file bytes are estimated once a version of the tree.
  // 1. An empty store is 0 on disk; a bulk load, past the small
  //    load's bytes, then ingests a file, and disk is its bytes, not
  //    the 0 kept.
  // 2. Another estimate kept for the whole store at the tree's
  //    version now is what approxsize answers: RocksDB is not asked.
  // 3. Rows loaded and flushed move the version: disk is RocksDB's
  //    estimate again, greater than before.
  // 4. A compaction moves it too: disk is the store's file bytes.
  stats_fixture_t f;
  uint64_t disk = 1, mem = 1, before = 0;
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk == 0);
  f.s()->db->load_small_bytes = 1;
  REQUIRE(sm1::begin(f.s(), sm1::xctlevel_t::repeatable_read) == 0);
  sm1::loader_t *l = nullptr;
  REQUIRE_MESSAGE(sm1::load_open(f.s(), f.store(), l) == 0, sm1::errmsg());
  size_t stream = 0;
  REQUIRE(sm1::load_stream(l, true, stream) == 0);
  for (int i = 0; i < 1000; i++) {
    std::string key(1, 'a');
    sm0::put_be32(key, i);
    REQUIRE(sm1::load_put(l, stream, key, std::string(200, 'v')) == 0);
  }
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(f.s()) == 0);
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk > 0);
  before = disk;

  // 2. Plant an estimate at the version now.
  f.plant_files(7);
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk == 7);

  // 3. Load and flush.
  f.load('b', 20000, 200);
  f.flush();
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk > before);

  // 4. Plant again, then compact.
  f.plant_files(7);
  f.compact();
  size_of(f.s(), f.store(), 0, 0, disk, mem);
  CHECK(disk == f.sst_bytes());
}

TEST_CASE("sm1_approxsize charges a small range its own bytes") {
  // Verify a range beside a much larger one is not charged it.
  // 1. Load many rows under one prefix and few under another,
  //    then flush and compact.
  // 2. The two ranges sum to the whole store, within 5%.
  // 3. The small range is charged a small fraction of the large.
  stats_fixture_t f;
  f.load('a', 50000, 200);
  f.load('b', 100, 200);
  f.flush();
  f.compact();

  // 2. Each range, and the whole.
  uint64_t da, db, dall, mem;
  size_of(f.s(), f.store(), 'a', 'b', da, mem);
  size_of(f.s(), f.store(), 'b', 'c', db, mem);
  size_of(f.s(), f.store(), 0, 0, dall, mem);
  CHECK(da + db >= dall * 95 / 100);
  CHECK(da + db <= dall * 105 / 100);

  // 3. The hundred rows against the fifty thousand.
  CHECK(db < da / 50);
}

TEST_CASE("sm1_approxsize measures a store not yet committed") {
  // Verify a store made by a running transaction can be sized.
  // 1. Make a store and write a row in a transaction left open.
  // 2. Size it before the transaction ends: its rows are there.
  stats_fixture_t f;
  REQUIRE(sm1::begin(f.s(), sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = 0;
  REQUIRE(sm1::mkstore(f.s(), store) == 0);
  REQUIRE(sm1::insert(f.s(), store, "k", std::string(1000, 'x')) == 0);

  // 2. Size it while the transaction is still open.
  uint64_t disk = 0, mem = 0;
  REQUIRE(sm1::approxsize(f.s(), store, nullptr, 0, nullptr, 0, disk, mem) ==
          0);
  CHECK(mem > 0);
  REQUIRE(sm1::abort(f.s()) == 0);
}

TEST_CASE("sm1_probes counts the sorted runs a lookup may visit") {
  // Verify probes follow memtables, level-0 files and levels.
  // 1. An empty store has none.
  // 2. Rows in the memtable make one.
  // 3. A flush turns it into one level-0 file: still one.
  // 4. A second flush adds a second level-0 file: two.
  // 5. A compaction leaves one level: one.
  stats_fixture_t f;
  CHECK(f.probes() == 0);
  f.load('a', 100, 100);
  CHECK(f.probes() == 1);
  f.flush();
  CHECK(f.probes() == 1);
  f.load('b', 100, 100);
  f.flush();
  CHECK(f.probes() == 2);
  f.compact();
  CHECK(f.probes() == 1);
}

TEST_CASE("sm1_compact merges a store into one sorted run") {
  // Verify sm1::compact leaves a store one run, its rows all there.
  // 1. Two loads, each flushed, and a third in the memtable: three
  //    probes.
  // 2. After sm1::compact, one probe, and no bytes in memtables.
  // 3. A row of each load reads as written.
  // 4. A store not there is SM_ERR_NOTFOUND.
  stats_fixture_t f;
  f.load('a', 1000, 100);
  f.flush();
  f.load('b', 1000, 100);
  f.flush();
  f.load('c', 1000, 100);
  CHECK(f.probes() == 3);

  // 2. Compact.
  REQUIRE_MESSAGE(sm1::compact(f.s(), f.store()) == 0, sm1::errmsg());
  CHECK(f.probes() == 1);
  uint64_t disk = 0, mem = 1;
  REQUIRE(sm1::approxsize(f.s(), f.store(), nullptr, 0, nullptr, 0, disk,
                          mem) == 0);
  CHECK(mem == 0);
  CHECK(disk > 0);

  // 3. Read a row of each load.
  REQUIRE(sm1::begin(f.s(), sm1::xctlevel_t::repeatable_read) == 0);
  for (char prefix : {'a', 'b', 'c'}) {
    std::string key(1, prefix);
    sm0::put_be32(key, 500);
    std::string_view d;
    REQUIRE(sm1::read(f.s(), f.store(), key, d) == 0);
    CHECK(d.size() == 100);
    CHECK(d.substr(0, key.size()) == key);
  }
  REQUIRE(sm1::commit(f.s()) == 0);

  // 4. No such store.
  CHECK(sm1::compact(f.s(), f.store() + 1000) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
}

TEST_CASE("sm1_approxsize counts a 2-byte prefix's memtable bytes exactly") {
  // Verify the memtable bytes of a range of whole 2-byte prefixes, a
  // heap's or an index's in sm2, are what was written there, not a
  // sample of the memtable that may read 0 for a range among others.
  // 1. 2,000 rows of 200 bytes under prefix 1 and 1,000 of 20 under
  //    prefix 2, unflushed: each prefix's mem is exactly its rows'
  //    versions, a prefix with none is 0, the whole store is both,
  //    and disk is 0.
  // 2. After a flush every mem is 0 and the store's disk is not.
  // 3. 500 more rows under prefix 1: its mem is exactly theirs.
  stats_fixture_t f;
  uint64_t a = load_prefix(f.s(), f.store(), 1, 2000, 200);
  uint64_t b = load_prefix(f.s(), f.store(), 2, 1000, 20);
  uint64_t disk = 1;
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == a);
  CHECK(disk == 0);
  CHECK(mem_of(f.s(), f.store(), 2, 3, disk) == b);
  CHECK(mem_of(f.s(), f.store(), 3, -1, disk) == 0);
  CHECK(mem_of(f.s(), f.store(), -1, -1, disk) == a + b);

  f.flush();
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == 0);
  CHECK(mem_of(f.s(), f.store(), 2, 3, disk) == 0);
  CHECK(mem_of(f.s(), f.store(), -1, -1, disk) == 0);
  CHECK(disk > 0);

  uint64_t c = load_prefix(f.s(), f.store(), 1, 500, 200, 2000);
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == c);
  CHECK(mem_of(f.s(), f.store(), 2, 3, disk) == 0);
}

TEST_CASE("sm1_approxsize counts a sealed memtable until it is flushed") {
  // Verify the count of sealed memtables not yet flushed, kept by
  // the tree's version, is read again once a seal moves it.
  // 1. 1,000 rows under prefix 1, in the active memtable: its mem is
  //    theirs, and none sealed is kept at the tree's version.
  // 2. The memtable sealed and, background work paused, not flushed:
  //    its mem is still theirs, not 0 by the count kept before.
  // 3. Once flushed, its mem is 0.
  stats_fixture_t f;
  uint64_t a = load_prefix(f.s(), f.store(), 1, 1000, 200);
  uint64_t disk = 0;
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == a);

  // 2. Seal, unflushed.
  f.seal();
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == a);

  // 3. Flush.
  f.flush_sealed();
  CHECK(mem_of(f.s(), f.store(), 1, 2, disk) == 0);
}

TEST_CASE("no sorted file is compressed") {
  // Verify sm1 stores every column family's blocks uncompressed.
  // 1. Load rows of repeated bytes, which would compress, and
  //    flush the store, then compact it into its bottom level.
  // 2. Every file of the store says NoCompression.
  // 3. So does every file of CF:txn and CF:meta, flushed.
  stats_fixture_t f;
  f.load('a', 2000, 200);
  f.flush();
  REQUIRE(!f.compressions().empty());
  for (const std::string &c : f.compressions()) {
    CHECK(c == "NoCompression");
  }
  f.compact();
  for (const std::string &c : f.compressions()) {
    CHECK(c == "NoCompression");
  }
  wisr::db_t *db = f.s()->db;
  for (rocksdb::ColumnFamilyHandle *cf : {db->cf_txn, db->cf_meta}) {
    REQUIRE(db->rdb->Flush(rocksdb::FlushOptions(), cf).ok());
    for (const std::string &c : f.compressions(cf)) {
      CHECK(c == "NoCompression");
    }
  }
}

TEST_CASE("a store's blocks are store_block_size") {
  // Verify a store CF writes blocks of store_block_size, not
  // RocksDB's 4 KB.
  // 1. Load about 450 KB of rows and flush the store.
  // 2. Every file's mean block is over three quarters of
  //    store_block_size, which 4 KB blocks are not.
  // 3. So is every file's after compacting into the bottom level.
  stats_fixture_t f;
  f.load('a', 2000, 200);
  f.flush();
  REQUIRE(!f.mean_block_sizes().empty());
  for (uint64_t n : f.mean_block_sizes()) {
    CHECK(n > wisr::store_block_size * 3 / 4);
  }
  f.compact();
  for (uint64_t n : f.mean_block_sizes()) {
    CHECK(n > wisr::store_block_size * 3 / 4);
  }
}
