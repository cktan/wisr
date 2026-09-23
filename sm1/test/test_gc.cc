/*
 * test_gc.cc -- garbage collection: the watermark, the
 * compaction filter, tombstone purge, and dropped stores
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

/* One raw version as stored in a store CF. */
struct raw_t {
  std::string key;
  uint64_t xid;
  uint8_t flags;
  uint64_t wts;
};

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store, and GC helpers. */
class gc_fixture_t {
public:
  /* Create "db" and the store; the first session opens "db" with opt. */
  explicit gc_fixture_t(const sm_options_t *opt = nullptr) {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = sm_opendb(m_home.env(), "db", opt);
    REQUIRE(m_s);
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    m_storeid = g_store = mkstore(m_s);
    REQUIRE(sm_commit(m_s) == 0);
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

  /* The store's storeid. */
  uint64_t storeid() const { return m_storeid; }

  /* Force a full compaction of storeid's CF, through the filter. */
  void compact(uint64_t storeid) {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(
        db()->rdb
            ->CompactRange(opt, db_store_cf(db(), storeid), nullptr, nullptr)
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

  /* Every version stored in storeid's CF, in order. */
  std::vector<raw_t> raw(uint64_t storeid) {
    rocksdb::ReadOptions ro;
    ro.total_order_seek = true;
    std::unique_ptr<rocksdb::Iterator> it(
        db()->rdb->NewIterator(ro, db_store_cf(db(), storeid)));
    std::vector<raw_t> out;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      version_t v;
      version_decode_key(it->key(), &v);
      REQUIRE(version_decode_value(it->value(), &v));
      out.push_back({v.userkey.ToString(), v.xid, v.flags, v.wts});
    }
    REQUIRE(it->status().ok());
    return out;
  }

private:
  home_t m_home;
  sm_session_t *m_s = nullptr;
  uint64_t m_storeid = 0;
};

/* Set key in store 1 to val in one committed transaction. */
void commit_set(sm_session_t *s, const std::string &key,
                const std::string &val) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_update(s, g_store, key.data(), key.size(), val.data(), val.size(),
                    nullptr, nullptr) == 0);
  REQUIRE(sm_commit(s) == 0);
}

/* Insert key -> val into store 1 in one committed transaction. */
void commit_put(sm_session_t *s, const std::string &key,
                const std::string &val) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, g_store, key, val);
  REQUIRE(sm_commit(s) == 0);
}

/* Delete key from store 1 in one committed transaction. */
void commit_del(sm_session_t *s, const std::string &key) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_delete(s, g_store, key.data(), key.size(), nullptr, nullptr) == 0);
  REQUIRE(sm_commit(s) == 0);
}

} // namespace

TEST_CASE("a key's versions are stored in commit order, by write timestamp") {
  // Verify each version's wts is taken after its writer's xid, and that
  // versions of one key sort newest commit first, even when a writer's xid
  // predates the commits of the writers before it.
  // 1. Commit "k" = v0 and "j" = x.
  // 2. Session L begins and writes "j", so its xid is early.
  // 3. Commit "k" = v1 and "k" = v2, then commit L.
  // 4. Confirm "k" has three versions whose wts strictly descend.
  // 5. Confirm every unstamped version's wts is above its xid.
  gc_fixture_t f;
  sm_session_t *w = f.open();
  sm_session_t *l = f.open();
  commit_put(w, "k", "v0");
  commit_put(w, "j", "x");
  REQUIRE(sm_begin(l, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_update(l, g_store, "j", 1, "y", 1, nullptr, nullptr) == 0);
  commit_set(w, "k", "v1");
  commit_set(w, "k", "v2");
  REQUIRE(sm_commit(l) == 0);
  REQUIRE(sm_begin(w, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(w, g_store, "k") == "v2");
  REQUIRE(sm_commit(w) == 0);

  std::vector<raw_t> raw = f.raw(g_store);
  std::vector<uint64_t> k_wts;
  for (const raw_t &v : raw) {
    if (v.key == "k") {
      k_wts.push_back(v.wts);
    }
    if ((v.flags & version_stamped) == 0) {
      CHECK(v.wts > v.xid);
    }
  }
  REQUIRE(k_wts.size() == 3);
  CHECK(k_wts[0] > k_wts[1]);
  CHECK(k_wts[1] > k_wts[2]);
  sm_closedb(w);
}

TEST_CASE("a read committed write past a newer commit survives compaction") {
  // Verify the newer commit is not dropped as superseded (§6.2, f07).
  // 1. Commit "k" = v0 and "j" = x.
  // 2. L (read committed) begins and writes "j", so its xid is early.
  // 3. Commit "k" = v1 from another session.
  // 4. L's write of "k" fails with SM_ERR_UPDATED; it reads the
  //    latest, writes v2 and commits.
  // 5. Compact; "k" reads v2 and only v2 is left.
  gc_fixture_t f;
  sm_session_t *w = f.open();
  sm_session_t *l = f.open();
  commit_put(w, "k", "v0");
  commit_put(w, "j", "x");
  REQUIRE(sm_begin(l, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_update(l, g_store, "j", 1, "y", 1, nullptr, nullptr) == 0);
  commit_set(w, "k", "v1");
  CHECK(sm_update(l, g_store, "k", 1, "v2", 2, nullptr, nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_UPDATED);
  const char *data = nullptr;
  size_t len = 0;
  REQUIRE(sm_read_latest(l, g_store, "k", 1, &data, &len) == 0);
  CHECK(std::string(data, len) == "v1");
  REQUIRE(sm_update(l, g_store, "k", 1, "v2", 2, nullptr, nullptr) == 0);
  REQUIRE(sm_commit(l) == 0);

  f.compact(g_store);
  REQUIRE(sm_begin(w, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(w, g_store, "k") == "v2");
  REQUIRE(sm_commit(w) == 0);
  size_t k_versions = 0;
  for (const raw_t &v : f.raw(g_store)) {
    if (v.key == "k") {
      k_versions++;
    }
  }
  CHECK(k_versions == 1);
  sm_closedb(w);
}

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
  CHECK(get(r, g_store, "k") == "v0");
  for (int i = 1; i <= 5; i++) {
    commit_set(w, "k", "v" + std::to_string(i));
  }

  f.compact(g_store);
  CHECK(get(r, g_store, "k") == "v0");
  std::vector<raw_t> raw = f.raw(g_store);
  REQUIRE(raw.size() == 6);
  for (size_t i = 0; i < 5; i++) {
    CHECK((raw[i].flags & version_stamped) == 0);
  }
  CHECK((raw[5].flags & version_stamped) != 0);
  REQUIRE(sm_commit(r) == 0);

  f.compact(g_store);
  raw = f.raw(g_store);
  REQUIRE(raw.size() == 1);
  CHECK((raw[0].flags & version_stamped) != 0);
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, g_store, "k") == "v5");
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
  put(t, g_store, "k", "v");
  f.compact(g_store);

  std::vector<raw_t> raw = f.raw(g_store);
  REQUIRE(raw.size() == 1);
  CHECK((raw[0].flags & version_stamped) == 0);
  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, g_store, "k") == "<none>");
  REQUIRE(sm_commit(u) == 0);
  CHECK(get(t, g_store, "k") == "v");

  REQUIRE(sm_abort(t) == 0);
  f.compact(g_store);
  CHECK(f.raw(g_store).empty());
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
    put(s, g_store, key, "a");
  }
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i++) {
    snprintf(key, sizeof(key), "k%02d", i);
    REQUIRE(sm_update(s, g_store, key, 3, "b", 1, nullptr, nullptr) == 0);
  }
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i += 2) {
    snprintf(key, sizeof(key), "k%02d", i);
    REQUIRE(sm_delete(s, g_store, key, 3, nullptr, nullptr) == 0);
  }
  REQUIRE(sm_commit(s) == 0);

  f.compact(g_store);
  commit_put(s, "k04", "c");
  gc_run_once(f.db());
  f.compact(g_store);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 0; i < 20; i++) {
    CAPTURE(i);
    snprintf(key, sizeof(key), "k%02d", i);
    std::string want = i == 4 ? "c" : i % 2 ? "b" : "<none>";
    CHECK(get(s, g_store, key) == want);
  }
  REQUIRE(sm_commit(s) == 0);

  std::vector<raw_t> raw = f.raw(g_store);
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
  // 4. The tombstone is still stored, and T's insert of "k",
  //    judged against the newest committed version, goes ahead.
  gc_fixture_t f;
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  commit_put(u, "k", "v");
  REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
  commit_del(u, "k");

  f.compact(g_store);
  gc_run_once(f.db());
  f.compact(g_store);
  bool tombstone = false;
  for (const raw_t &v : f.raw(g_store)) {
    tombstone |= v.key == "k" && (v.flags & version_tombstone) != 0;
  }
  CHECK(tombstone);
  CHECK(sm_insert(t, g_store, "k", 1, "t", 1) == 0);
  REQUIRE(sm_commit(t) == 0);
  sm_closedb(t);
  sm_closedb(u);
}

TEST_CASE("a dropped store's column family goes once no snapshot sees it") {
  // Verify dropped-store removal waits for the watermark.
  // 1. Reader R begins and reads store 1.
  // 2. S drops store 1 and commits; run GC: the CF stays, and R
  //    still reads store 1.
  // 3. R ends; run GC: the CF is gone.
  // 4. A drop undone by rollback to a savepoint is never
  //    queued: the second store survives GC.
  gc_fixture_t f;
  sm_session_t *s = f.open();
  sm_session_t *r = f.open();
  commit_put(s, "k", "v");
  uint64_t storeid = g_store;
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, g_store, "k") == "v");

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmstore(s, g_store) == 0);
  REQUIRE(sm_commit(s) == 0);
  gc_run_once(f.db());
  CHECK(db_store_cf(f.db(), storeid) != nullptr);
  CHECK(get(r, g_store, "k") == "v");
  REQUIRE(sm_commit(r) == 0);
  gc_run_once(f.db());
  CHECK(db_store_cf(f.db(), storeid) == nullptr);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t ustoreid = mkstore(s);
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_savepoint(s) == 1);
  REQUIRE(sm_rmstore(s, ustoreid) == 0);
  REQUIRE(sm_rollback(s, 1) == 0);
  REQUIRE(sm_commit(s) == 0);
  gc_run_once(f.db());
  CHECK(db_store_cf(f.db(), ustoreid) != nullptr);
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

/* The keys a full scan of store 1 returns, through s. */
std::vector<std::string> scan_keys(sm_session_t *s) {
  sm_scan_t *c = sm_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
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
  for (const raw_t &v : f.raw(g_store)) {
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
      CHECK(get(s, g_store, "k" + std::to_string(100 + i)) == "v");
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
  sm_scan_t *c = sm_scan(r, g_store, nullptr, 0, 0, nullptr, 0, 0);
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
