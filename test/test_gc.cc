/*
 * test_gc.cc -- garbage collection: the watermark, the
 * compaction filter, tombstone purge, and dropped relations
 * (DESIGN.md §8.3, §12.5 "GC").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"
#include "version.h"

#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

using namespace wisr;

namespace {

/* One raw version as stored in a relation CF. */
struct raw_t {
  std::string key;
  uint64_t xid;
  uint8_t flags;
};

/* A database "db" with relation 1, and GC helpers. */
class gc_fixture_t {
public:
  /* Create "db" and relation 1; the first session opens "db" with opt. */
  explicit gc_fixture_t(const sm_options_t *opt = nullptr) {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = sm_opendb(m_home.env(), "db", opt);
    REQUIRE(m_s);
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(m_s, 1) == 0);
    REQUIRE(sm_commit(m_s) == 0);
    m_cfid = cfid_of(1);
  }

  /* The fixture's own session. */
  sm_session_t *s() const { return m_s; }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* The shared database object. */
  db_t *db() const { return m_s->db; }

  /* The cfid of relation 1. */
  uint64_t cfid() const { return m_cfid; }

  /* The cfid relid resolves to now, through the catalog. */
  uint64_t cfid_of(uint64_t relid) {
    uint64_t cfid = 0;
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(catalog_lookup(m_s, xct_view(m_s), relid, &cfid) == 0);
    REQUIRE(sm_commit(m_s) == 0);
    return cfid;
  }

  /* Force a full compaction of cfid's CF, through the filter. */
  void compact(uint64_t cfid) {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(
        db()->rdb->CompactRange(opt, db_rel_cf(db(), cfid), nullptr, nullptr)
            .ok());
  }

  /* The number of records in CF:txn. */
  size_t txn_records() {
    std::unique_ptr<rocksdb::Iterator> it(
        db()->rdb->NewIterator(rocksdb::ReadOptions(), db()->cf_txn));
    size_t n = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      n++;
    }
    REQUIRE(it->status().ok());
    return n;
  }

  /* Close the session, reattach, and reopen "db" with opt. */
  void reopen(const sm_options_t *opt) {
    REQUIRE(sm_closedb(m_s) == 0);
    m_home.reattach();
    m_s = sm_opendb(m_home.env(), "db", opt);
    REQUIRE(m_s);
  }

  /* Every version stored in cfid's CF, in order. */
  std::vector<raw_t> raw(uint64_t cfid) {
    rocksdb::ReadOptions ro;
    ro.total_order_seek = true;
    std::unique_ptr<rocksdb::Iterator> it(
        db()->rdb->NewIterator(ro, db_rel_cf(db(), cfid)));
    std::vector<raw_t> out;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      version_t v;
      version_decode_key(it->key(), &v);
      REQUIRE(version_decode_value(it->value(), &v));
      out.push_back({v.userkey.ToString(), v.xid, v.flags});
    }
    REQUIRE(it->status().ok());
    return out;
  }

private:
  home_t m_home;
  sm_session_t *m_s = nullptr;
  uint64_t m_cfid = 0;
};

/* Set key in relation 1 to val in one committed transaction. */
void commit_set(sm_session_t *s, const std::string &key,
                const std::string &val) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_update(s, 1, key.data(), key.size(), val.data(), val.size()) == 0);
  REQUIRE(sm_commit(s) == 0);
}

/* Insert key -> val into relation 1 in one committed transaction. */
void commit_put(sm_session_t *s, const std::string &key,
                const std::string &val) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, 1, key, val);
  REQUIRE(sm_commit(s) == 0);
}

/* Delete key from relation 1 in one committed transaction. */
void commit_del(sm_session_t *s, const std::string &key) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_delete(s, 1, key.data(), key.size()) == 0);
  REQUIRE(sm_commit(s) == 0);
}

} // namespace

TEST_CASE("a long-running reader keeps the versions it can see") {
  // Verify the watermark protects a live snapshot, then lets go.
  // 1. Commit "k" = v0; reader R begins and reads v0.
  // 2. Commit five more values of "k"; compact.
  // 3. R still reads v0, and all six versions are stored; the
  //    one R sees (v0) is stamped, the newer five are not.
  // 4. R ends; compact again: only v5 remains, stamped.
  gc_fixture_t f;
  sm_session_t *w = f.open();
  sm_session_t *r = f.open();
  commit_put(w, "k", "v0");
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, 1, "k") == "v0");
  for (int i = 1; i <= 5; i++) {
    commit_set(w, "k", "v" + std::to_string(i));
  }

  f.compact(f.cfid());
  CHECK(get(r, 1, "k") == "v0");
  std::vector<raw_t> raw = f.raw(f.cfid());
  REQUIRE(raw.size() == 6);
  for (size_t i = 0; i < 5; i++) {
    CHECK((raw[i].flags & version_stamped) == 0);
  }
  CHECK((raw[5].flags & version_stamped) != 0);
  REQUIRE(sm_commit(r) == 0);

  f.compact(f.cfid());
  raw = f.raw(f.cfid());
  REQUIRE(raw.size() == 1);
  CHECK((raw[0].flags & version_stamped) != 0);
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, 1, "k") == "v5");
  REQUIRE(sm_commit(r) == 0);
  sm_closedb(w);
  sm_closedb(r);
}

TEST_CASE("a running transaction's versions survive compaction unstamped") {
  // Verify the in-flight rule comes before every other (§8.3).
  // 1. T inserts "k" and stays open; compact.
  // 2. T's version is stored and unstamped; another session
  //    does not see it; T still reads it.
  // 3. T aborts; compact again: the version is gone.
  gc_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  put(t, 1, "k", "v");
  f.compact(f.cfid());

  std::vector<raw_t> raw = f.raw(f.cfid());
  REQUIRE(raw.size() == 1);
  CHECK((raw[0].flags & version_stamped) == 0);
  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, 1, "k") == "<none>");
  REQUIRE(sm_commit(u) == 0);
  CHECK(get(t, 1, "k") == "v");

  REQUIRE(sm_abort(t) == 0);
  f.compact(f.cfid());
  CHECK(f.raw(f.cfid()).empty());
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("tombstones are purged per key, sparing neighbors and re-inserts") {
  // Verify the purge deletes only the purged key's old versions.
  // 1. Commit keys k00..k19, each updated once (two versions).
  // 2. Delete every even key and commit; compact, which stamps
  //    and queues the tombstones.
  // 3. Before GC runs, re-insert k04 and commit.
  // 4. Run GC and compact again.
  // 5. Every odd key and the new k04 remain; the other even
  //    keys are gone from reads and from storage entirely.
  gc_fixture_t f;
  sm_session_t *s = f.open();
  char key[8];
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i++) {
    snprintf(key, sizeof(key), "k%02d", i);
    put(s, 1, key, "a");
  }
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i++) {
    snprintf(key, sizeof(key), "k%02d", i);
    REQUIRE(sm_update(s, 1, key, 3, "b", 1) == 0);
  }
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i += 2) {
    snprintf(key, sizeof(key), "k%02d", i);
    REQUIRE(sm_delete(s, 1, key, 3) == 0);
  }
  REQUIRE(sm_commit(s) == 0);

  f.compact(f.cfid());
  commit_put(s, "k04", "c");
  gc_run_once(f.db());
  f.compact(f.cfid());

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i++) {
    CAPTURE(i);
    snprintf(key, sizeof(key), "k%02d", i);
    std::string want = i == 4 ? "c" : i % 2 ? "b" : "<none>";
    CHECK(get(s, 1, key) == want);
  }
  REQUIRE(sm_commit(s) == 0);

  std::vector<raw_t> raw = f.raw(f.cfid());
  CHECK(raw.size() == 11); // ten odd keys and the new k04
  for (const raw_t &v : raw) {
    CHECK((v.flags & version_tombstone) == 0);
  }
  sm_closedb(s);
}

TEST_CASE("an idle read-committed transaction holds back the tombstone purge") {
  // Verify RC counts in the watermark at its begin snapshot (§8.3, §12.5).
  // 1. T begins at read committed and stays idle.
  // 2. U deletes "k" and commits.
  // 3. Compact and run GC: the tombstone must survive.
  // 4. T's insert of "k" fails with SM_ERR_CONFLICT, as it
  //    would have without any GC.
  gc_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  commit_put(u, "k", "v");
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  commit_del(u, "k");

  f.compact(f.cfid());
  gc_run_once(f.db());
  f.compact(f.cfid());
  CHECK(sm_insert(t, 1, "k", 1, "t", 1) == -1);
  CHECK(sm_errno() == SM_ERR_CONFLICT);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("a dropped relation's column family goes once no snapshot sees it") {
  // Verify dropped-relation removal waits for the watermark.
  // 1. Reader R begins and reads relation 1.
  // 2. S drops relation 1 and commits; run GC: the CF stays, and R
  //    still reads relation 1.
  // 3. R ends; run GC: the CF is gone.
  // 4. A drop undone by rollback to a savepoint is never
  //    queued: relation 2 survives GC.
  gc_fixture_t f;
  sm_session_t *s = f.open();
  sm_session_t *r = f.open();
  commit_put(s, "k", "v");
  uint64_t cfid = f.cfid();
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, 1, "k") == "v");

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmrel(s, 1) == 0);
  REQUIRE(sm_commit(s) == 0);
  gc_run_once(f.db());
  CHECK(db_rel_cf(f.db(), cfid) != nullptr);
  CHECK(get(r, 1, "k") == "v");
  REQUIRE(sm_commit(r) == 0);
  gc_run_once(f.db());
  CHECK(db_rel_cf(f.db(), cfid) == nullptr);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(s, 2) == 0);
  REQUIRE(sm_commit(s) == 0);
  uint64_t ucfid = f.cfid_of(2);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_savepoint(s) == 0);
  REQUIRE(sm_rmrel(s, 2) == 0);
  REQUIRE(sm_rollback(s) == 0);
  REQUIRE(sm_commit(s) == 0);
  gc_run_once(f.db());
  CHECK(db_rel_cf(f.db(), ucfid) != nullptr);
  sm_closedb(s);
  sm_closedb(r);
}

namespace {

/* Options with a one-entry commit cache, so CF:txn answers lookups. */
sm_options_t tiny_cache() {
  sm_options_t opt;
  sm_options_init(&opt);
  opt.commit_cache_entries = 1;
  return opt;
}

/* The keys a full scan of relation 1 returns, through s. */
std::vector<std::string> scan_keys(sm_session_t *s) {
  sm_scan_t *c = sm_scan(s, 1, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE_MESSAGE(c, sm_errmsg());
  std::vector<std::string> out;
  const char *k, *d;
  size_t kl, dl;
  while (sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
    out.emplace_back(k, kl);
  }
  sm_scanend(c);
  return out;
}

} // namespace

TEST_CASE(
    "the CF:txn sweep deletes records only once every version is stamped") {
  // Verify committed rows stay committed after their records go (§8.3).
  // 1. Commit 30 transactions, one key each; CF:txn holds a
  //    record per writing commit.
  // 2. Sweep: the pass stamps every version, then the records
  //    at or below the watermark are deleted.
  // 3. Every key reads back, by point read and scan, now and
  //    after reopening (no commit cache to help).
  sm_options_t opt = tiny_cache();
  gc_fixture_t f(&opt);
  sm_session_t *s = f.s();
  for (int i = 0; i < 30; i++) {
    commit_put(s, "k" + std::to_string(100 + i), "v");
  }
  size_t before = f.txn_records();
  CHECK(before >= 30);

  CHECK(gc_sweep_txn(f.db(), true) == (int64_t)before);
  CHECK(f.txn_records() == 0);
  for (const raw_t &v : f.raw(f.cfid())) {
    CHECK((v.flags & version_stamped) != 0);
  }

  for (int pass = 0; pass < 2; pass++) {
    CAPTURE(pass);
    if (pass == 1) {
      f.reopen(&opt);
      s = f.s();
    }
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (int i = 0; i < 30; i++) {
      CHECK(get(s, 1, "k" + std::to_string(100 + i)) == "v");
    }
    CHECK(scan_keys(s).size() == 30);
    REQUIRE(sm_commit(s) == 0);
  }
}

TEST_CASE(
    "a cursor opened before the sweep's pass holds back record deletion") {
  // Verify the sweep waits out pre-pass readers (§8.3 step 3).
  // 1. Commit 10 keys; open cursor C, which pins the unstamped,
  //    pre-pass versions.
  // 2. Start the sweep: the pass runs, but deletion waits on C,
  //    so the call reports it pending and the records remain.
  // 3. C still returns all 10 rows.
  // 4. Close C; the next step of the sweep deletes the records.
  sm_options_t opt = tiny_cache();
  gc_fixture_t f(&opt);
  sm_session_t *s = f.s();
  sm_session_t *r = f.open();
  for (int i = 0; i < 10; i++) {
    commit_put(s, "k" + std::to_string(i), "v");
  }
  size_t before = f.txn_records();
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  sm_scan_t *c = sm_scan(r, 1, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);

  CHECK(gc_sweep_txn(f.db(), true) == 0);
  CHECK(f.txn_records() == before);
  CHECK(gc_sweep_txn(f.db(), false) == 0); // still waiting on C

  int n = 0;
  const char *k, *d;
  size_t kl, dl;
  while (sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
    n++;
  }
  CHECK(n == 10);
  sm_scanend(c);
  REQUIRE(sm_commit(r) == 0);

  // The worker may finish the sweep first; either way it is done.
  int64_t done = gc_sweep_txn(f.db(), false);
  CHECK((done == (int64_t)before || done == 0));
  CHECK(f.txn_records() == 0);
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(scan_keys(r).size() == 10);
  REQUIRE(sm_commit(r) == 0);
  sm_closedb(r);
}

TEST_CASE("the sweep keeps records a live snapshot may still need") {
  // Verify only records at or below the watermark are deleted.
  // 1. Commit "a"; reader R begins; commit "b" and "c".
  // 2. Sweep: the watermark is R's snapshot, so the records of
  //    "b" and "c" (committed after it) stay; older ones go.
  // 3. R sees only "a"; a new transaction sees all three.
  sm_options_t opt = tiny_cache();
  gc_fixture_t f(&opt);
  sm_session_t *s = f.s();
  sm_session_t *r = f.open();
  commit_put(s, "a", "v");
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  commit_put(s, "b", "v");
  commit_put(s, "c", "v");

  CHECK(gc_sweep_txn(f.db(), true) > 0);
  CHECK(f.txn_records() == 2);
  CHECK(scan_keys(r) == std::vector<std::string>{"a"});
  REQUIRE(sm_commit(r) == 0);
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(scan_keys(r) == std::vector<std::string>{"a", "b", "c"});
  REQUIRE(sm_commit(r) == 0);
  sm_closedb(r);
}

TEST_CASE("the GC run sweeps CF:txn once it grows past its threshold") {
  // Verify the size trigger (§8.3: by CF:txn size, not by timer).
  // 1. Lower the threshold to 5 records; commit 20 keys.
  // 2. Run GC: CF:txn is swept down to nothing.
  // 3. All 20 keys still read back.
  sm_options_t opt = tiny_cache();
  gc_fixture_t f(&opt);
  sm_session_t *s = f.s();
  f.db()->txn_sweep_records = 5;
  for (int i = 0; i < 20; i++) {
    commit_put(s, "k" + std::to_string(100 + i), "v");
  }
  CHECK(f.txn_records() >= 20);
  gc_run_once(f.db());
  CHECK(f.txn_records() == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(scan_keys(s).size() == 20);
  REQUIRE(sm_commit(s) == 0);
}
