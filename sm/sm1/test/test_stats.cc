/*
 * test_stats.cc -- what a store's LSM looks like now: the sizes
 * sm1::approxsize estimates and the runs sm1::probes counts.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"
#include "sm1_codec.h"

#include <string>

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
      sm1::put_be32(key, i);
      // Mostly repeated bytes, so the files compress.
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

  /* The store's sm1::probes. */
  uint64_t probes() {
    uint64_t n = 0;
    REQUIRE_MESSAGE(sm1::probes(m_s, m_store, n) == 0, sm1::errmsg());
    return n;
  }

private:
  /* The store's column family. */
  rocksdb::ColumnFamilyHandle *cf() const {
    return wisr::db_store_cf(m_s->db, m_store);
  }

  home_t m_home;
  sm1::session_t *m_s = nullptr;
  uint64_t m_store = 0;
};

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
  // 2. Rows not yet flushed are in mem only. Its amount is not
  //    checked: RocksDB samples its memtables, and over a whole
  //    store mem was measured from 0.4 to 1.05 times the bytes.
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
