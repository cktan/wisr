/*
 * test_chain.cc -- chained stores (../VCHAIN.md): their column
 * family, and the chain_ calls on them.
 */

#include "doctest.h"
#include "util.h"

#include "chain.h"
#include "db.h"
#include "session.h"
#include "sm1.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rocksdb/db.h>

using namespace wisr;

namespace {

/* Begin a transaction at level, failing the test on an error. */
void begin_xct(sm1::session_t *s,
               sm1::xctlevel_t level = sm1::xctlevel_t::read_committed) {
  REQUIRE_MESSAGE(sm1::begin(s, level) == 0, sm1::errmsg());
}

/* Commit, failing the test on an error. */
void commit_xct(sm1::session_t *s) {
  REQUIRE_MESSAGE(sm1::commit(s) == 0, sm1::errmsg());
}

/* Make a chained store in its own transaction, failing on an error. */
uint64_t mkchain(sm1::session_t *s) {
  uint64_t storeid = 0;
  begin_xct(s);
  REQUIRE_MESSAGE(sm1::mkstore(s, storeid, false, true) == 0, sm1::errmsg());
  commit_xct(s);
  return storeid;
}

/* The column families the session's database holds, by name. */
std::vector<std::string> cf_names(sm1::session_t *s) {
  std::vector<std::string> names;
  REQUIRE(rocksdb::DB::ListColumnFamilies(rocksdb::DBOptions(),
                                          s->db->rdb->GetName(), &names)
              .ok());
  return names;
}

/* Whether names holds name. */
bool has(const std::vector<std::string> &names, const std::string &name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

/* A home directory with database "db" and one session on it. */
struct chain_fixture_t {
  home_t home;
  sm1::session_t *s = nullptr;

  /* Make "db" and open the session. */
  chain_fixture_t() {
    REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
    s = open();
  }

  /* Another session on "db". */
  sm1::session_t *open() {
    sm1::session_t *out = opendb(home.env(), "db", nullptr);
    REQUIRE_MESSAGE(out, sm1::errmsg());
    return out;
  }
};

} // namespace

TEST_CASE("a chained store is one column family, and stays chained across "
          "a reopen") {
  // Verify that a chained store is one CF, named chain.N, whose flag
  // survives a reopen.
  // 1. Make a chained store and a plain one, and commit.
  // 2. Confirm the chained one's CF is chain.N, flagged, and the plain
  //    one's store.N; neither has another CF.
  // 3. Write a chained key; close every session and reopen: the store
  //    is chained still, its key reads, and its next old id is past
  //    the last one written.
  // 4. Drop the chained store, commit, and run GC: its CF goes.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  uint64_t p = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkstore(f.s, p) == 0);
  commit_xct(f.s);

  store_t st = db_store(f.s->db, c);
  REQUIRE(st.cf);
  CHECK(st.chained);
  CHECK(st.cf->GetName() == store_cf_name(c, true));
  store_t sp = db_store(f.s->db, p);
  REQUIRE(sp.cf);
  CHECK(!sp.chained);
  CHECK(sp.cf->GetName() == store_cf_name(p));
  std::vector<std::string> names = cf_names(f.s);
  CHECK(!has(names, store_cf_name(c)));
  CHECK(!has(names, store_cf_name(p, true)));

  begin_xct(f.s);
  REQUIRE(sm1::chain_insert(f.s, c, "k", "v1") == 0);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  commit_xct(f.s);
  uint64_t next = st.next_old->load();
  REQUIRE(sm1::closedb(f.s) == 0);
  // Detach closes the database, so the reattach opens it anew.
  f.home.reattach();
  f.s = f.open();
  st = db_store(f.s->db, c);
  CHECK(st.chained);
  CHECK(!db_store(f.s->db, p).chained);
  CHECK(st.next_old->load() == next);
  bool chained = false;
  REQUIRE(sm1::store_chained(f.s, c, chained) == 0);
  CHECK(chained);
  begin_xct(f.s);
  std::string_view v;
  REQUIRE(sm1::chain_read(f.s, c, "k", v) == 0);
  CHECK(v == "v2");
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE(sm1::rmstore(f.s, c) == 0);
  commit_xct(f.s);
  gc_run_once(f.s->db);
  CHECK(!db_store(f.s->db, c).cf);
  names = cf_names(f.s);
  CHECK(!has(names, store_cf_name(c, true)));
  CHECK(has(names, store_cf_name(p)));
}

TEST_CASE("an aborted chained store leaves no column family behind") {
  // Verify that abort drops the CF a chained mkstore made.
  // 1. Make a chained store and abort.
  // 2. Confirm its CF is not left in the database.
  chain_fixture_t f;
  uint64_t c = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkstore(f.s, c, false, true) == 0);
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK(!db_store(f.s->db, c).cf);
  std::vector<std::string> names = cf_names(f.s);
  CHECK(!has(names, store_cf_name(c, true)));
}

TEST_CASE("a temporary store may be chained") {
  // Verify a temporary chained store: the session's own, no lock, and
  // writable in a read-only transaction.
  // 1. Make a temporary chained store; it says it is chained.
  // 2. Insert and update a key, and read it back; lock it, which
  //    holds nothing.
  // 3. In a read-only transaction, update it again and read it.
  // 4. Another session does not see the store.
  chain_fixture_t f;
  uint64_t c = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkstore(f.s, c, true, true) == 0);
  bool chained = false;
  REQUIRE(sm1::store_chained(f.s, c, chained) == 0);
  CHECK(chained);
  REQUIRE(sm1::chain_insert(f.s, c, "k", "v1") == 0);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  std::string_view v;
  REQUIRE(sm1::chain_read(f.s, c, "k", v) == 0);
  CHECK(v == "v2");
  CHECK(sm1::chain_lock_row(f.s, c, "k", sm1::SM_LOCKMODE_EX) == 0);
  commit_xct(f.s);

  REQUIRE(sm1::begin(f.s, sm1::xctlevel_t::read_committed, true) == 0);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v3") == 0);
  REQUIRE(sm1::chain_read(f.s, c, "k", v) == 0);
  CHECK(v == "v3");
  commit_xct(f.s);

  sm1::session_t *t = f.open();
  CHECK(sm1::store_chained(t, c, chained) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
}

namespace {

/* The chained value of key a read sees now, or "-" for none. */
std::string chain_get(sm1::session_t *s, uint64_t storeid,
                      std::string_view key) {
  std::string_view v;
  int r = sm1::chain_read(s, storeid, key, v);
  REQUIRE_MESSAGE(r >= 0, sm1::errmsg());
  return r == 1 ? "-" : std::string(v);
}

/* Each old version of storeid, decoded, in id order. */
std::vector<chain_version_t> old_entries(sm1::session_t *s, uint64_t storeid,
                                         std::vector<std::string> *payloads) {
  store_t st = db_store(s->db, storeid);
  std::vector<chain_version_t> out;
  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
  for (it->Seek(std::string(1, key_class_old)); it->Valid(); it->Next()) {
    chain_version_t v;
    REQUIRE(chain_decode(it->value(), &v, true));
    payloads->push_back(v.payload.ToString());
    out.push_back(v);
  }
  return out;
}

/* Write key in its own transaction with op, failing on an error. */
void chain_put(sm1::session_t *s, uint64_t storeid, std::string_view key,
               std::string_view value, bool insert) {
  begin_xct(s);
  int r = insert ? sm1::chain_insert(s, storeid, key, value)
                 : sm1::chain_update(s, storeid, key, value);
  REQUIRE_MESSAGE(r == 0, sm1::errmsg());
  commit_xct(s);
}

} // namespace

TEST_CASE("a chained key is written and read as a versioned one is") {
  // Verify the chained calls' basic contract on one key.
  // 1. Insert a key and read it back in the same transaction; a
  //    second insert fails with SM_ERR_DUPKEY.
  // 2. Update it, read the new value, commit; another session sees it.
  // 3. The store's versioned key of the same bytes is another key.
  // 4. Erase it: a read finds none, and an update fails NOTFOUND.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  begin_xct(f.s);
  REQUIRE(sm1::chain_insert(f.s, c, "k", "v1") == 0);
  CHECK(chain_get(f.s, c, "k") == "v1");
  CHECK(sm1::chain_insert(f.s, c, "k", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  CHECK(chain_get(f.s, c, "k") == "v2");
  commit_xct(f.s);

  sm1::session_t *t = f.open();
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v2");
  std::string_view v;
  CHECK(sm1::read(t, c, "k", v) == 1);
  commit_xct(t);

  begin_xct(f.s);
  REQUIRE(sm1::chain_erase(f.s, c, "k") == 0);
  CHECK(chain_get(f.s, c, "k") == "-");
  CHECK(sm1::chain_update(f.s, c, "k", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  commit_xct(f.s);
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "-");
  commit_xct(t);
}

TEST_CASE("a snapshot older than a chained key's newest version follows the "
          "chain") {
  // Verify that a reader that does not see CURR's version finds its own
  // in OLD, however many writes ago.
  // 1. Insert k = v1 and commit.
  // 2. Begin a repeatable read transaction and read v1.
  // 3. Update k to v2, then to v3, committing each.
  // 4. Confirm the old snapshot still reads v1, two versions back, and a
  //    new one v3; OLD holds v1 and v2.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  chain_put(f.s, c, "k", "v2", false);
  chain_put(f.s, c, "k", "v3", false);
  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(t);
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v3");
  commit_xct(t);
  std::vector<std::string> payloads;
  old_entries(f.s, c, &payloads);
  CHECK(payloads == std::vector<std::string>{"v1", "v2"});
}

TEST_CASE("a chained write meets the conflicts a versioned one does") {
  // Verify first-committer-wins and read committed's SM_ERR_UPDATED.
  // 1. Under repeatable read, update a key another transaction changed
  //    and committed since the snapshot: SM_ERR_CONFLICT.
  // 2. Under read committed, the same: SM_ERR_UPDATED.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  sm1::session_t *t = f.open();

  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  chain_put(f.s, c, "k", "v2", false);
  CHECK(sm1::chain_update(t, c, "k", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);

  begin_xct(t);
  chain_put(f.s, c, "k", "v3", false);
  CHECK(sm1::chain_update(t, c, "k", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::abort(t) == 0);
}

TEST_CASE("an aborted chained write is passed by readers and writers") {
  // Verify that a dead version left in CURR costs a hop, never a wrong
  // answer, and that the next writer overwrites it.
  // 1. Insert k = v1 and commit.
  // 2. Update k to v2 and to v3 in one transaction, and abort it.
  // 3. Confirm a reader sees v1, behind both dead versions.
  // 4. Begin a repeatable read reader, then update k to v4 and commit.
  // 5. Confirm the reader still sees v1 and a new one v4, and that the
  //    dead v2 in OLD is unlinked while v1 is displaced by the writer.
  // An abort repairs nothing, and leaves the dead versions in place.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v3") == 0);
  REQUIRE(sm1::abort(f.s) == 0);

  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  chain_put(f.s, c, "k", "v4", false);
  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(t);
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v4");
  commit_xct(t);

  std::vector<std::string> payloads;
  std::vector<chain_version_t> old = old_entries(f.s, c, &payloads);
  REQUIRE(payloads == std::vector<std::string>{"v1", "v2"});
  CHECK(!(old[0].flags & chain_unlinked));
  CHECK((old[1].flags & chain_unlinked));
}

TEST_CASE("a chained writer waits for another's uncommitted version") {
  // Verify that an uncommitted chained version is its writer's lock.
  // 1. Erase a key, uncommitted.
  // 2. In another thread, insert the key: it waits.
  // 3. Commit the erase: the insert goes in, and reads back.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  begin_xct(f.s);
  REQUIRE(sm1::chain_erase(f.s, c, "k") == 0);
  sm1::session_t *t = f.open();
  std::atomic<int> done{-2};
  std::thread th([&] {
    REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
    done = sm1::chain_insert(t, c, "k", "v2");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(done == -2);
  commit_xct(f.s);
  th.join();
  CHECK(done == 0);
  CHECK(chain_get(t, c, "k") == "v2");
  commit_xct(t);
}

TEST_CASE("the chained calls refuse a store that is not chained") {
  // Verify the chained calls' refusal of a versioned store.
  // 1. A chained write on a versioned store fails SM_ERR_INVALID.
  // 2. So does a chained read, and a chained cursor.
  chain_fixture_t f;
  uint64_t p = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkstore(f.s, p) == 0);
  commit_xct(f.s);
  begin_xct(f.s);
  CHECK(sm1::chain_insert(f.s, p, "k", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string_view v;
  CHECK(sm1::chain_read(f.s, p, "k", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm1::scan_t *c = nullptr;
  CHECK(sm1::chain_scan(f.s, p, nullptr, 0, 0, nullptr, 0, 0, c) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(f.s) == 0);
}

namespace {

/* Every key and value a chained cursor over [lo, hi] hands out. */
std::vector<std::string> chain_rows(sm1::session_t *s, uint64_t storeid,
                                    const char *lo, int lo_incl, const char *hi,
                                    int hi_incl) {
  sm1::scan_t *c = nullptr;
  REQUIRE_MESSAGE(sm1::chain_scan(s, storeid, lo, lo ? strlen(lo) : 0, lo_incl,
                                  hi, hi ? strlen(hi) : 0, hi_incl, c) == 0,
                  sm1::errmsg());
  std::vector<std::string> out;
  std::string_view k, v;
  int r;
  while ((r = sm1::scannxt(c, k, v)) == 0) {
    out.push_back(std::string(k) + "=" + std::string(v));
  }
  CHECK(r == 1);
  sm1::scanend(c);
  return out;
}

} // namespace

TEST_CASE("a chained cursor hands out each key as its view sees it") {
  // Verify the chained cursor's bounds, and its walk into OLD.
  // 1. Insert a, b, c, d and commit.
  // 2. Open a repeatable read reader's view, then update b, erase c,
  //    insert e, and commit.
  // 3. Confirm the old view scans a..d as they were, with each bound
  //    inclusive and exclusive, and a new view the new rows.
  chain_fixture_t f;
  uint64_t st = mkchain(f.s);
  begin_xct(f.s);
  for (const char *k : {"a", "b", "c", "d"}) {
    REQUIRE(sm1::chain_insert(f.s, st, k, std::string(k) + "1") == 0);
  }
  commit_xct(f.s);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, st, "a") == "a1");
  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, st, "b", "b2") == 0);
  REQUIRE(sm1::chain_erase(f.s, st, "c") == 0);
  REQUIRE(sm1::chain_insert(f.s, st, "e", "e2") == 0);
  commit_xct(f.s);

  using rows_t = std::vector<std::string>;
  CHECK(chain_rows(t, st, nullptr, 0, nullptr, 0) ==
        rows_t{"a=a1", "b=b1", "c=c1", "d=d1"});
  CHECK(chain_rows(t, st, "b", 1, "c", 1) == rows_t{"b=b1", "c=c1"});
  CHECK(chain_rows(t, st, "b", 0, "d", 0) == rows_t{"c=c1"});
  commit_xct(t);
  begin_xct(t);
  CHECK(chain_rows(t, st, nullptr, 0, nullptr, 0) ==
        rows_t{"a=a1", "b=b2", "d=d1", "e=e2"});
  commit_xct(t);
}

TEST_CASE("a chained cursor does not see writes made after it opened") {
  // Verify the cursor's view is fixed at open, its own later writes too.
  // 1. Insert a and b and commit; open a cursor over the store.
  // 2. In the same transaction, update b and insert c.
  // 3. Confirm the cursor hands out a and b as they were, and seeks
  //    forward only.
  chain_fixture_t f;
  uint64_t st = mkchain(f.s);
  begin_xct(f.s);
  REQUIRE(sm1::chain_insert(f.s, st, "a", "a1") == 0);
  REQUIRE(sm1::chain_insert(f.s, st, "b", "b1") == 0);
  commit_xct(f.s);
  begin_xct(f.s);
  sm1::scan_t *c = nullptr;
  REQUIRE(sm1::chain_scan(f.s, st, nullptr, 0, 0, nullptr, 0, 0, c) == 0);
  REQUIRE(sm1::chain_update(f.s, st, "b", "b2") == 0);
  REQUIRE(sm1::chain_insert(f.s, st, "c", "c2") == 0);
  std::string_view k, v;
  REQUIRE(sm1::scannxt(c, k, v) == 0);
  CHECK(k == "a");
  REQUIRE(sm1::scan_seek(c, "b", 1) == 0);
  REQUIRE(sm1::scannxt(c, k, v) == 0);
  CHECK(k == "b");
  CHECK(v == "b1");
  CHECK(sm1::scan_seek(c, "a", 1) == -1);
  CHECK(sm1::scannxt(c, k, v) == 1);
  sm1::scanend(c);
  commit_xct(f.s);
}

namespace {

/* CURR's version of key, decoded, its payload copied into *payload. */
chain_version_t curr_of(sm1::session_t *s, uint64_t storeid,
                        std::string_view key, std::string *payload) {
  store_t st = db_store(s->db, storeid);
  rocksdb::PinnableSlice buf;
  chain_version_t v;
  rocksdb::Status got =
      st.rdb->Get(rocksdb::ReadOptions(), st.cf,
                  curr_key(rocksdb::Slice(key.data(), key.size())), &buf);
  if (got.IsNotFound()) {
    *payload = "-";
    return v;
  }
  REQUIRE(got.ok());
  REQUIRE(chain_decode(buf, &v, false));
  *payload = v.payload.ToString();
  return v;
}

} // namespace

TEST_CASE("a rollback to a savepoint puts chained versions back") {
  // Verify that chained writes under a savepoint are undone in CURR.
  // 1. Insert k = v1 and commit.
  // 2. Update k to v2, set a savepoint, update k to v3 and insert n.
  // 3. Roll back to the savepoint: k reads v2 and n none, and CURR
  //    holds v2 itself, with n's key gone.
  // 4. Update k to v4 and commit; a reader sees v4, and an older
  //    snapshot v1.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");

  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  int depth = sm1::savepoint(f.s);
  REQUIRE(depth == 1);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v3") == 0);
  REQUIRE(sm1::chain_insert(f.s, c, "n", "n1") == 0);
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  CHECK(chain_get(f.s, c, "k") == "v2");
  CHECK(chain_get(f.s, c, "n") == "-");
  std::string payload;
  curr_of(f.s, c, "k", &payload);
  CHECK(payload == "v2");
  curr_of(f.s, c, "n", &payload);
  CHECK(payload == "-");
  REQUIRE(sm1::chain_update(f.s, c, "k", "v4") == 0);
  commit_xct(f.s);

  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(t);
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v4");
  commit_xct(t);
}

namespace {

/* Compact a chained store's CF to the bottom, through GC. */
void compact_chain(sm1::session_t *s, uint64_t storeid) {
  store_t st = db_store(s->db, storeid);
  rocksdb::CompactRangeOptions opt;
  opt.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  REQUIRE(st.rdb->CompactRange(opt, st.cf, nullptr, nullptr).ok());
}

} // namespace

TEST_CASE("compaction reports a dead chained version, and GC puts it back") {
  // Verify the repair (../VCHAIN.md §7): an abort leaves its versions,
  // readers pass them, and the compaction filter queues the key for
  // the GC worker, which puts the version before them back.
  // 1. Insert k = v1 and n = n1 and commit; update k to v2 and v3,
  //    insert m, delete n, and abort.
  // 2. Readers see k = v1, n = n1 and no m; the dead versions are
  //    still each key's newest, and a read queues nothing.
  // 3. Compact, and run GC once: k's newest version is v1 again, n's
  //    n1, and m's key is gone; the old versions passed are unlinked.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  chain_put(f.s, c, "n", "n1", true);
  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v3") == 0);
  REQUIRE(sm1::chain_insert(f.s, c, "m", "m1") == 0);
  REQUIRE(sm1::chain_erase(f.s, c, "n") == 0);
  REQUIRE(sm1::abort(f.s) == 0);

  begin_xct(f.s);
  CHECK(chain_get(f.s, c, "k") == "v1");
  CHECK(chain_get(f.s, c, "n") == "n1");
  CHECK(chain_get(f.s, c, "m") == "-");
  CHECK(chain_rows(f.s, c, nullptr, 0, nullptr, 0) ==
        std::vector<std::string>{"k=v1", "n=n1"});
  commit_xct(f.s);
  std::string payload;
  curr_of(f.s, c, "k", &payload);
  CHECK(payload == "v3");
  curr_of(f.s, c, "m", &payload);
  CHECK(payload == "m1");
  {
    std::lock_guard<std::mutex> guard(f.s->db->gc_mu);
    CHECK(f.s->db->repairs.empty());
  }

  compact_chain(f.s, c);
  gc_run_once(f.s->db);
  chain_version_t k = curr_of(f.s, c, "k", &payload);
  CHECK(payload == "v1");
  CHECK(k.prev == 0);
  curr_of(f.s, c, "n", &payload);
  CHECK(payload == "n1");
  curr_of(f.s, c, "m", &payload);
  CHECK(payload == "-");
  std::vector<std::string> payloads;
  for (const chain_version_t &v : old_entries(f.s, c, &payloads)) {
    CHECK((v.flags & chain_unlinked));
  }
  CHECK(payloads.size() == 3);
  begin_xct(f.s);
  CHECK(chain_get(f.s, c, "k") == "v1");
  commit_xct(f.s);
}

TEST_CASE("GC harvests what no snapshot of a chained store needs") {
  // Verify the chained compaction filters: OLD keeps what a snapshot
  // needs and drops the rest, and CURR loses its deletes and is
  // stamped.
  // 1. Insert k = v1, then update it to v2 and v3, committing each;
  //    insert d and erase it.
  // 2. Hold a repeatable read snapshot from before v2, and compact:
  //    the reader still reads v1, through OLD.
  // 3. End the reader and compact again: OLD is empty, d's key is gone
  //    from CURR, and k's version there is stamped and reads v3.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  chain_put(f.s, c, "k", "v2", false);
  chain_put(f.s, c, "k", "v3", false);
  chain_put(f.s, c, "d", "d1", true);
  begin_xct(f.s);
  REQUIRE(sm1::chain_erase(f.s, c, "d") == 0);
  commit_xct(f.s);

  compact_chain(f.s, c);
  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(t);

  compact_chain(f.s, c);
  std::vector<std::string> payloads;
  old_entries(f.s, c, &payloads);
  CHECK(payloads.empty());
  std::string payload;
  curr_of(f.s, c, "d", &payload);
  CHECK(payload == "-");
  chain_version_t k = curr_of(f.s, c, "k", &payload);
  CHECK(payload == "v3");
  CHECK((k.flags & version_stamped));
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v3");
  CHECK(chain_get(t, c, "d") == "-");
  commit_xct(t);
}

TEST_CASE("a chained cursor does not see an old version stamped after its "
          "snapshot") {
  // Verify that a stamped version written before a cursor's snapshot,
  // but committed after it, is not seen: its stamp, not its write
  // timestamp, decides (§4.4).
  // 1. Insert k = v1 and commit.
  // 2. Update k to v2 in a transaction w, and leave it open; insert j
  //    and commit, so visible_ts passes v2's write (§4.3).
  // 3. Begin a repeatable read reader t and read k: its snapshot is past
  //    v2's write, and before its commit.
  // 4. Commit w, then update k to v3, so v2 goes to OLD; compact, which
  //    stamps v2 with w's commit, since t's snapshot keeps it.
  // 5. Confirm t's cursor reads k = v1, and a new view's k = v3.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  sm1::session_t *w = f.open();
  begin_xct(w);
  REQUIRE(sm1::chain_update(w, c, "k", "v2") == 0);
  chain_put(f.s, c, "j", "j1", true);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(w);
  chain_put(f.s, c, "k", "v3", false);
  compact_chain(f.s, c);
  std::vector<std::string> payloads;
  std::vector<chain_version_t> old = old_entries(f.s, c, &payloads);
  REQUIRE(payloads == std::vector<std::string>{"v1", "v2"});
  REQUIRE((old[1].flags & version_stamped));

  using rows_t = std::vector<std::string>;
  CHECK(chain_rows(t, c, nullptr, 0, nullptr, 0) == rows_t{"j=j1", "k=v1"});
  commit_xct(t);
  begin_xct(t);
  CHECK(chain_rows(t, c, nullptr, 0, nullptr, 0) == rows_t{"j=j1", "k=v3"});
  commit_xct(t);
}

TEST_CASE("GC drops unlinked OLD entries, and the CF:txn sweep keeps "
          "chained reads right") {
  // Verify that a repair's unlinked entries go, and that once the
  // sweep deletes status records, chained versions read by their
  // stamps.
  // 1. Insert k = v1 and commit; update it to v2 and abort; compact
  //    and run GC: the repair puts v1 back and unlinks its old entry.
  // 2. Hold a snapshot from before v3; update k to v3 and commit.
  // 3. Compact: v1's displaced entry stays for the snapshot, stamped
  //    twice; the unlinked one stays too, as the snapshot, taken from
  //    visible_ts, is older than the ts it was unlinked at.
  // 4. Run the CF:txn sweep to its end: the snapshot still reads v1.
  // 5. End the snapshot and compact: OLD is empty, and k reads v3.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v1", true);
  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, c, "k", "v2") == 0);
  REQUIRE(sm1::abort(f.s) == 0);
  compact_chain(f.s, c);
  gc_run_once(f.s->db);
  sm1::session_t *t = f.open();
  begin_xct(t, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(t, c, "k") == "v1");
  chain_put(f.s, c, "k", "v3", false);

  compact_chain(f.s, c);
  std::vector<std::string> payloads;
  std::vector<chain_version_t> old = old_entries(f.s, c, &payloads);
  REQUIRE(payloads == std::vector<std::string>{"v1", "v1"});
  CHECK((old[0].flags & chain_unlinked));
  CHECK((old[1].flags & version_stamped));
  CHECK((old[1].flags & chain_displaced_stamped));

  CHECK(gc_sweep_txn(f.s->db, true) >= 0);
  CHECK(gc_sweep_txn(f.s->db, false) >= 0);
  CHECK(chain_get(t, c, "k") == "v1");
  commit_xct(t);

  compact_chain(f.s, c);
  payloads.clear();
  old_entries(f.s, c, &payloads);
  CHECK(payloads.empty());
  begin_xct(t);
  CHECK(chain_get(t, c, "k") == "v3");
  commit_xct(t);
}

TEST_CASE("the chained calls a row layer needs: insert_new, old images, "
          "moves, read_latest and locks") {
  // Verify the rest of the chained surface sm2's rows use.
  // 1. insert_new a key; update and erase it with a pin, each handing
  //    back the row replaced.
  // 2. erase_moved a key: an update of it fails SM_ERR_MOVED, and an
  //    insert goes in over it.
  // 3. At read committed, after SM_ERR_UPDATED, lock the key and
  //    read_latest it: the newest value, and the update then goes in.
  // 4. Under repeatable read, a lock on a key changed since the snapshot
  //    fails SM_ERR_CONFLICT; a trylock on a key another holds returns 1.
  // 5. The store is not empty, compacts, and its size is estimated.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  bool empty = false;
  begin_xct(f.s);
  REQUIRE(sm1::store_empty(f.s, c, empty) == 0);
  CHECK(empty);
  REQUIRE(sm1::chain_insert_new(f.s, c, "n", "n1") == 0);
  sm1::rawpin_t *pin = sm1::rawpin_new();
  std::string_view old;
  REQUIRE(sm1::chain_update(f.s, c, "n", "n2", pin, old) == 0);
  CHECK(old == "n1");
  REQUIRE(sm1::chain_erase(f.s, c, "n", pin, old) == 0);
  CHECK(old == "n2");
  REQUIRE(sm1::chain_insert(f.s, c, "m", "m1") == 0);
  REQUIRE(sm1::chain_erase_moved(f.s, c, "m", pin, old) == 0);
  CHECK(old == "m1");
  CHECK(sm1::chain_update(f.s, c, "m", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_MOVED);
  REQUIRE(sm1::chain_insert(f.s, c, "m", "m2") == 0);
  REQUIRE(sm1::chain_insert(f.s, c, "k", "v1") == 0);
  commit_xct(f.s);
  sm1::rawpin_free(pin);

  sm1::session_t *t = f.open();
  begin_xct(t);
  chain_put(f.s, c, "k", "v2", false);
  CHECK(sm1::chain_update(t, c, "k", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm1::chain_lock_row(t, c, "k", sm1::SM_LOCKMODE_EX) == 0);
  std::string_view v;
  REQUIRE(sm1::chain_read_latest(t, c, "k", v) == 0);
  CHECK(v == "v2");
  REQUIRE(sm1::chain_update(t, c, "k", "v3") == 0);

  begin_xct(f.s, sm1::xctlevel_t::repeatable_read);
  CHECK(sm1::chain_trylock_row(f.s, c, "k", sm1::SM_LOCKMODE_SH) == 1);
  commit_xct(t);
  CHECK(sm1::chain_lock_row(f.s, c, "k", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);

  begin_xct(f.s);
  REQUIRE(sm1::store_empty(f.s, c, empty) == 0);
  CHECK(!empty);
  CHECK(chain_get(f.s, c, "k") == "v3");
  commit_xct(f.s);
  REQUIRE(sm1::compact(f.s, c) == 0);
  uint64_t disk = 0, mem = 0;
  REQUIRE(sm1::chain_approxsize(f.s, c, nullptr, 0, nullptr, 0, disk, mem) ==
          0);
  CHECK(disk + mem > 0);
}

TEST_CASE("a bulk load puts chained keys in CURR, as a batch and as files") {
  // Verify a load stream declared chained, small and large, and a
  // rollback past one.
  // 1. Load three chained keys and one versioned key into an empty
  //    chained store, a batch: each reads back through its own calls.
  // 2. Load a thousand chained keys into another, past
  //    load_small_bytes, so they go in as files.
  // 3. Load into a third under a savepoint and roll back to it: the
  //    store is empty again.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::options_t opt;
  opt.load_small_bytes = 4096;
  sm1::session_t *s = opendb(home.env(), "db", &opt);
  REQUIRE_MESSAGE(s, sm1::errmsg());
  uint64_t a = mkchain(s), b = mkchain(s), c = mkchain(s);

  begin_xct(s);
  sm1::loader_t *l = nullptr;
  size_t chained = 0, versioned = 0;
  REQUIRE(sm1::load_open(s, a, l) == 0);
  REQUIRE(sm1::load_stream(l, false, chained, true) == 0);
  REQUIRE(sm1::load_stream(l, false, versioned) == 0);
  for (const char *k : {"k3", "k1", "k2"}) {
    REQUIRE(sm1::load_put(l, chained, k, std::string(k) + "v") == 0);
  }
  REQUIRE(sm1::load_put(l, versioned, "t1", "tv") == 0);
  REQUIRE(sm1::load_close(l) == 0);
  CHECK(chain_get(s, a, "k2") == "k2v");
  std::string_view v;
  REQUIRE(sm1::read(s, a, "t1", v) == 0);
  CHECK(v == "tv");
  commit_xct(s);
  begin_xct(s);
  CHECK(chain_rows(s, a, nullptr, 0, nullptr, 0) ==
        std::vector<std::string>{"k1=k1v", "k2=k2v", "k3=k3v"});
  commit_xct(s);

  begin_xct(s);
  REQUIRE(sm1::load_open(s, b, l) == 0);
  REQUIRE(sm1::load_stream(l, true, chained, true) == 0);
  for (int i = 0; i < 1000; i++) {
    char k[16];
    snprintf(k, sizeof(k), "k%06d", i);
    REQUIRE(sm1::load_put(l, chained, k, std::string(100, 'x')) == 0);
  }
  REQUIRE(sm1::load_close(l) == 0);
  commit_xct(s);
  begin_xct(s);
  CHECK(chain_rows(s, b, nullptr, 0, nullptr, 0).size() == 1000);
  CHECK(chain_get(s, b, "k000500") == std::string(100, 'x'));
  commit_xct(s);

  begin_xct(s);
  int depth = sm1::savepoint(s);
  REQUIRE(sm1::load_open(s, c, l) == 0);
  REQUIRE(sm1::load_stream(l, false, chained, true) == 0);
  REQUIRE(sm1::load_put(l, chained, "k", "v") == 0);
  REQUIRE(sm1::load_close(l) == 0);
  CHECK(chain_get(s, c, "k") == "v");
  REQUIRE(sm1::rollback(s, depth) == 0);
  bool empty = false;
  REQUIRE(sm1::store_empty(s, c, empty) == 0);
  CHECK(empty);
  commit_xct(s);
}

TEST_CASE("a session's handle of a store is read again once the store map "
          "changes") {
  // Verify that a session's store handles (STORECACHE.md) follow
  // the database's stores: a dropped store's handle has no CF, a new
  // store's is read, and handles of an older map go at the
  // transaction's end.
  // 1. Session A reads chained store c: it keeps c's handle, at the
  //    store map's version now.
  // 2. Session B drops c, and GC drops its CF: the version is raised,
  //    and A's handle of c has none.
  // 3. B makes store d: A reads d's key, and c is not there for it;
  //    at the end of A's transaction its handle of c is gone, d's
  //    kept.
  // 4. A makes store e and aborts, and makes e2 in a savepoint it
  //    rolls back: neither handle has a CF, and both go at the end.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  chain_put(f.s, c, "k", "v", true);
  begin_xct(f.s);
  CHECK(chain_get(f.s, c, "k") == "v");
  commit_xct(f.s);
  uint64_t v0 = f.s->db->stores_version.load();
  REQUIRE(f.s->store_handles.count(c));
  CHECK(f.s->store_handles.at(c).version == v0);

  sm1::session_t *t = f.open();
  begin_xct(t);
  REQUIRE(sm1::rmstore(t, c) == 0);
  commit_xct(t);
  gc_run_once(f.s->db);
  CHECK(f.s->db->stores_version.load() > v0);
  CHECK(!session_store(f.s, c).cf);

  uint64_t d = mkchain(t);
  chain_put(t, d, "k", "w", true);
  begin_xct(f.s);
  CHECK(chain_get(f.s, d, "k") == "w");
  std::string_view v;
  CHECK(sm1::chain_read(f.s, c, "k", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  commit_xct(f.s);
  CHECK(!f.s->store_handles.count(c));
  CHECK(f.s->store_handles.count(d));

  uint64_t e = 0, e2 = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkstore(f.s, e, false, true) == 0);
  REQUIRE(sm1::chain_insert(f.s, e, "k", "x") == 0);
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK(!session_store(f.s, e).cf);
  begin_xct(f.s);
  int depth = sm1::savepoint(f.s);
  REQUIRE(depth == 1);
  REQUIRE(sm1::mkstore(f.s, e2, false, true) == 0);
  REQUIRE(sm1::chain_insert(f.s, e2, "k", "x") == 0);
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  CHECK(!session_store(f.s, e2).cf);
  commit_xct(f.s);
  CHECK(!f.s->store_handles.count(e));
  CHECK(!f.s->store_handles.count(e2));
  REQUIRE(sm1::closedb(t) == 0);
}

namespace {

/* Each key's row as single reads give them: the value, or "-". */
std::vector<std::string> single_reads(sm1::session_t *s, uint64_t storeid,
                                      const std::vector<std::string> &keys) {
  std::vector<std::string> out;
  for (const std::string &k : keys) {
    out.push_back(chain_get(s, storeid, k));
  }
  return out;
}

/*
 * Each key's row as one batch read gives them, through iter if given:
 * the value, or "-".
 */
std::vector<std::string> batch_reads(sm1::session_t *s, uint64_t storeid,
                                     const std::vector<std::string> &keys,
                                     sm1::batch_iter_t *iter = nullptr) {
  size_t n = keys.size();
  std::vector<std::string_view> kv(keys.begin(), keys.end());
  std::vector<std::unique_ptr<sm1::rawpin_t>> own;
  std::vector<sm1::rawpin_t *> pins;
  for (size_t i = 0; i < n; i++) {
    own.push_back(std::make_unique<sm1::rawpin_t>());
    pins.push_back(own.back().get());
  }
  std::vector<std::string_view> data(n);
  std::vector<int> rc(n, -2);
  REQUIRE_MESSAGE(sm1::chain_read_batch(s, storeid, kv.data(), n, pins.data(),
                                        data.data(), rc.data(), iter) == 0,
                  sm1::errmsg());
  std::vector<std::string> out;
  for (size_t i = 0; i < n; i++) {
    REQUIRE((rc[i] == 0 || rc[i] == 1));
    out.push_back(rc[i] == 1 ? "-" : std::string(data[i]));
  }
  for (sm1::rawpin_t *p : pins) {
    sm1::unpin(p);
  }
  return out;
}

} // namespace

TEST_CASE("a batch read gives each key's row as a single read does") {
  // Verify sm1::chain_read_batch against sm1::chain_read
  // (../BATCHREAD.md).
  // 1. Write k0..k9; update k1 twice and k2 once, so older versions
  //    are in OLD; erase k5. Session C takes a snapshot before the
  //    updates, R a snapshot after.
  // 2. Session W updates k3 and inserts k7b, uncommitted.
  // 3. Session O, this one, updates k4 and erases k6, uncommitted.
  // 4. Over keys out of order, repeated, and missing, and over them
  //    in ascending order, which one iterator reads, each session's
  //    batch gives what its single reads give, row by row.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  for (int i = 0; i < 10; i++) {
    chain_put(f.s, c, "k" + std::to_string(i), "v" + std::to_string(i), true);
  }
  sm1::session_t *old = f.open();
  begin_xct(old, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(old, c, "k1") == "v1");
  chain_put(f.s, c, "k1", "v1b", false);
  chain_put(f.s, c, "k1", "v1c", false);
  chain_put(f.s, c, "k2", "v2b", false);
  begin_xct(f.s);
  REQUIRE(sm1::chain_erase(f.s, c, "k5") == 0);
  commit_xct(f.s);
  sm1::session_t *now = f.open();
  begin_xct(now, sm1::xctlevel_t::repeatable_read);
  CHECK(chain_get(now, c, "k0") == "v0");

  sm1::session_t *w = f.open();
  begin_xct(w);
  REQUIRE(sm1::chain_update(w, c, "k3", "v3w") == 0);
  REQUIRE(sm1::chain_insert(w, c, "k7b", "w") == 0);
  begin_xct(f.s);
  REQUIRE(sm1::chain_update(f.s, c, "k4", "v4o") == 0);
  REQUIRE(sm1::chain_erase(f.s, c, "k6") == 0);

  std::vector<std::string> keys = {"k9", "k1", "k2",  "k3", "k1", "k5",
                                   "k4", "k6", "k7b", "zz", "k0", "k2"};
  std::vector<std::string> sorted = {"a",  "k0", "k1",  "k2", "k3", "k4", "k5",
                                     "k6", "k7", "k7b", "k8", "k9", "zz"};
  for (sm1::session_t *s : {old, now, f.s}) {
    std::vector<std::string> single = single_reads(s, c, keys);
    CHECK(batch_reads(s, c, keys) == single);
    CHECK(batch_reads(s, c, sorted) == single_reads(s, c, sorted));
  }
  CHECK(batch_reads(old, c, {"k1", "k2", "k5"}) ==
        std::vector<std::string>{"v1", "v2", "v5"});
  CHECK(batch_reads(now, c, {"k1", "k2", "k5", "k3"}) ==
        std::vector<std::string>{"v1c", "v2b", "-", "v3"});
  CHECK(batch_reads(f.s, c, {"k4", "k6", "k7b"}) ==
        std::vector<std::string>{"v4o", "-", "-"});
  CHECK(batch_reads(f.s, c, {}).empty());
  REQUIRE(sm1::abort(w) == 0);
  REQUIRE(sm1::abort(f.s) == 0);
  commit_xct(old);
  commit_xct(now);
  sm1::closedb(old);
  sm1::closedb(now);
  sm1::closedb(w);
}

TEST_CASE("a batch read's kept iterator reads the store as it is now") {
  // Verify that an iterator kept from one batch read to the next
  // sees what a new one would: other sessions' commits, the
  // session's own writes, and a store compacted in between.
  // 1. Write k0..k9. Read k1..k4 in a batch through a kept iterator.
  // 2. Session W updates k2 and commits. A new statement reads k2
  //    as W wrote it, through the same iterator.
  // 3. This session updates k3 and erases k4: the next batch reads
  //    its own writes.
  // 4. Compact the store, so its files change; W updates k1. The
  //    next batch reads W's k1, and a single read agrees.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  for (int i = 0; i < 10; i++) {
    chain_put(f.s, c, "k" + std::to_string(i), "v" + std::to_string(i), true);
  }
  std::vector<std::string> keys = {"k1", "k2", "k3", "k4"};
  sm1::batch_iter_t *iter = sm1::batch_iter_new();
  begin_xct(f.s);
  CHECK(batch_reads(f.s, c, keys, iter) ==
        std::vector<std::string>{"v1", "v2", "v3", "v4"});
  CHECK(sm1::batch_iter_held(iter));

  sm1::session_t *w = f.open();
  chain_put(w, c, "k2", "w2", false);
  REQUIRE(sm1::stmt_begin(f.s) == 0);
  CHECK(batch_reads(f.s, c, keys, iter) ==
        std::vector<std::string>{"v1", "w2", "v3", "v4"});

  REQUIRE(sm1::chain_update(f.s, c, "k3", "o3") == 0);
  REQUIRE(sm1::chain_erase(f.s, c, "k4") == 0);
  REQUIRE(sm1::stmt_begin(f.s) == 0);
  CHECK(batch_reads(f.s, c, keys, iter) ==
        std::vector<std::string>{"v1", "w2", "o3", "-"});

  REQUIRE(sm1::compact(f.s, c) == 0);
  chain_put(w, c, "k1", "w1", false);
  REQUIRE(sm1::stmt_begin(f.s) == 0);
  CHECK(batch_reads(f.s, c, keys, iter) ==
        std::vector<std::string>{"w1", "w2", "o3", "-"});
  CHECK(batch_reads(f.s, c, keys, iter) == single_reads(f.s, c, keys));
  commit_xct(f.s);
  sm1::batch_iter_free(iter);
  sm1::closedb(w);
}

TEST_CASE("a batch read's kept iterator goes with the transaction") {
  // Verify that sm1 lets go of a kept iterator where it ends its
  // cursors, so one its caller never frees holds no RocksDB files.
  // 1. A batch read keeps an iterator; the commit lets it go.
  // 2. Kept again, a rollback to a savepoint lets it go; the next
  //    batch keeps one again, and the abort lets that go.
  // 3. Kept again, the session's close lets it go, and it is freed
  //    after the session is gone.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  for (int i = 0; i < 4; i++) {
    chain_put(f.s, c, "k" + std::to_string(i), "v" + std::to_string(i), true);
  }
  std::vector<std::string> keys = {"k0", "k1", "k2", "k3"};
  sm1::batch_iter_t *iter = sm1::batch_iter_new();
  CHECK(!sm1::batch_iter_held(iter));
  begin_xct(f.s);
  batch_reads(f.s, c, keys, iter);
  CHECK(sm1::batch_iter_held(iter));
  commit_xct(f.s);
  CHECK(!sm1::batch_iter_held(iter));

  begin_xct(f.s);
  int depth = sm1::savepoint(f.s);
  batch_reads(f.s, c, keys, iter);
  CHECK(sm1::batch_iter_held(iter));
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  CHECK(!sm1::batch_iter_held(iter));
  batch_reads(f.s, c, keys, iter);
  CHECK(sm1::batch_iter_held(iter));
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK(!sm1::batch_iter_held(iter));

  begin_xct(f.s);
  batch_reads(f.s, c, keys, iter);
  CHECK(sm1::batch_iter_held(iter));
  sm1::closedb(f.s);
  f.s = nullptr;
  CHECK(!sm1::batch_iter_held(iter));
  sm1::batch_iter_free(iter);
}

TEST_CASE("a batch read's kept iterator gives way to MultiGet for scattered "
          "keys") {
  // Verify that a batch whose keys are too far apart to step between
  // marks its kept iterator, and that the iterator's later batches of
  // the store are read by MultiGet alone; close keys keep it.
  // 1. Write r0000..r0999.
  // 2. Batches of close keys through one iterator: it stays held.
  // 3. A batch of close keys, then a gap: the steps reached keys past
  //    the first, so it stays held.
  // 4. Through another iterator, a batch of scattered keys: it is let
  //    go. Its next batches, scattered and close, make none.
  // 5. An iterator made scattered makes none, for close keys too.
  //    Every batch gives what single reads give.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  char buf[16];
  begin_xct(f.s);
  for (int i = 0; i < 1000; i++) {
    snprintf(buf, sizeof buf, "r%04d", i);
    REQUIRE(sm1::chain_insert(f.s, c, buf, buf) == 0);
  }
  commit_xct(f.s);
  begin_xct(f.s);
  std::vector<std::string> close1 = {"r0100", "r0101", "r0102"};
  std::vector<std::string> close2 = {"r0200", "r0201", "r0203"};
  std::vector<std::string> gap = {"r0300", "r0301", "r0302", "r0900"};
  std::vector<std::string> far1 = {"r0000", "r0500", "r0900"};
  std::vector<std::string> far2 = {"r0010", "r0510", "r0910"};
  sm1::batch_iter_t *iter = sm1::batch_iter_new();
  CHECK(batch_reads(f.s, c, close1, iter) == close1);
  CHECK(sm1::batch_iter_held(iter));
  CHECK(batch_reads(f.s, c, close2, iter) == close2);
  CHECK(sm1::batch_iter_held(iter));
  CHECK(batch_reads(f.s, c, gap, iter) == gap);
  CHECK(sm1::batch_iter_held(iter));

  sm1::batch_iter_t *other = sm1::batch_iter_new();
  CHECK(batch_reads(f.s, c, far1, other) == far1);
  CHECK(!sm1::batch_iter_held(other));
  CHECK(batch_reads(f.s, c, far2, other) == far2);
  CHECK(!sm1::batch_iter_held(other));
  CHECK(batch_reads(f.s, c, close1, other) == close1);
  CHECK(!sm1::batch_iter_held(other));

  sm1::batch_iter_t *told = sm1::batch_iter_new(true);
  CHECK(batch_reads(f.s, c, close1, told) == close1);
  CHECK(!sm1::batch_iter_held(told));
  CHECK(batch_reads(f.s, c, close2, told) == close2);
  CHECK(!sm1::batch_iter_held(told));
  commit_xct(f.s);
  sm1::batch_iter_free(iter);
  sm1::batch_iter_free(other);
  sm1::batch_iter_free(told);
}

TEST_CASE("a batch of ascending keys is read by steps and seeks alike") {
  // Verify chain_read_batch's iterator over ascending keys, stepping
  // over short gaps and seeking past long ones.
  // 1. Write r0000..r0999 but every third; erase every tenth written.
  // 2. Over ascending runs with gaps of 1, 3, 9 and 40 keys between
  //    them, a batch gives what single reads give.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  begin_xct(f.s);
  char buf[16];
  for (int i = 0; i < 1000; i++) {
    if (i % 3 != 0) {
      snprintf(buf, sizeof buf, "r%04d", i);
      REQUIRE(sm1::chain_insert(f.s, c, buf, buf) == 0);
    }
  }
  commit_xct(f.s);
  begin_xct(f.s);
  for (int i = 0; i < 1000; i += 10) {
    if (i % 3 != 0) {
      snprintf(buf, sizeof buf, "r%04d", i);
      REQUIRE(sm1::chain_erase(f.s, c, buf) == 0);
    }
  }
  commit_xct(f.s);
  begin_xct(f.s);
  for (int gap : {1, 3, 9, 40}) {
    CAPTURE(gap);
    std::vector<std::string> keys;
    for (int i = 5; i < 1000; i += gap) {
      snprintf(buf, sizeof buf, "r%04d", i);
      keys.push_back(buf);
    }
    CHECK(batch_reads(f.s, c, keys) == single_reads(f.s, c, keys));
  }
  commit_xct(f.s);
}

TEST_CASE("rows of an ascending batch stay in their pins past the call") {
  // Verify rows a batch read leaves pinned in its iterator's blocks
  // stay valid until each pin lets go, whatever comes after.
  // 1. Write k000..k199 and compact them into a file, so a read pins
  //    blocks of it.
  // 2. Read every other key, ascending, into pins.
  // 3. Write each key again, flush and compact: the pinned rows still
  //    read as they were.
  // 4. Let go of half, read the rest again, let go of all.
  chain_fixture_t f;
  uint64_t c = mkchain(f.s);
  char buf[16];
  begin_xct(f.s);
  for (int i = 0; i < 200; i++) {
    snprintf(buf, sizeof buf, "k%03d", i);
    REQUIRE(sm1::chain_insert(f.s, c, buf, std::string(buf) + "-old") == 0);
  }
  commit_xct(f.s);
  begin_xct(f.s);
  REQUIRE(sm1::compact(f.s, c) == 0);
  std::vector<std::string> keys;
  for (int i = 0; i < 200; i += 2) {
    snprintf(buf, sizeof buf, "k%03d", i);
    keys.push_back(buf);
  }
  size_t n = keys.size();
  std::vector<std::string_view> kv(keys.begin(), keys.end());
  std::vector<std::unique_ptr<sm1::rawpin_t>> own;
  std::vector<sm1::rawpin_t *> pins;
  for (size_t i = 0; i < n; i++) {
    own.push_back(std::make_unique<sm1::rawpin_t>());
    pins.push_back(own.back().get());
  }
  std::vector<std::string_view> data(n);
  std::vector<int> rc(n, -2);
  REQUIRE(sm1::chain_read_batch(f.s, c, kv.data(), n, pins.data(), data.data(),
                                rc.data()) == 0);
  commit_xct(f.s);
  begin_xct(f.s);
  for (int i = 0; i < 200; i++) {
    snprintf(buf, sizeof buf, "k%03d", i);
    REQUIRE(sm1::chain_update(f.s, c, buf, std::string(buf) + "-new") == 0);
  }
  commit_xct(f.s);
  begin_xct(f.s);
  REQUIRE(sm1::compact(f.s, c) == 0);
  for (size_t i = 0; i < n; i++) {
    CHECK(0 == rc[i]);
    CHECK(keys[i] + "-old" == data[i]);
  }
  for (size_t i = 0; i < n; i += 2) {
    sm1::unpin(pins[i]);
  }
  for (size_t i = 1; i < n; i += 2) {
    CHECK(keys[i] + "-old" == data[i]);
  }
  for (sm1::rawpin_t *p : pins) {
    sm1::unpin(p);
  }
  commit_xct(f.s);
}
