/*
 * test_load.cc -- a bulk load into an empty store (BULKLOAD.md §5):
 * sm1::load_open, load_stream, load_put and load_close.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <rocksdb/db.h>

using namespace wisr;

namespace {

/* Options under which every load is written as files, not a batch. */
sm1::options_t files_options() {
  sm1::options_t opt;
  opt.load_small_bytes = 1;
  return opt;
}

/* A home directory with database "db", made with opt, and one session. */
struct load_fixture_t {
  home_t home;
  sm1::options_t opt;
  sm1::session_t *s = nullptr;

  /* Make "db" with opt and open the session. */
  explicit load_fixture_t(const sm1::options_t &o = sm1::options_t()) : opt(o) {
    REQUIRE(sm1::mkdb(home.env(), "db", &opt) == 0);
    s = open();
  }

  /* Another session on "db". */
  sm1::session_t *open() {
    sm1::session_t *out = opendb(home.env(), "db", &opt);
    REQUIRE_MESSAGE(out, sm1::errmsg());
    return out;
  }
};

/* Begin a READ COMMITTED transaction, failing the test on an error. */
void begin_xct(sm1::session_t *s, int read_only = 0) {
  REQUIRE_MESSAGE(sm1::begin(s, sm1::xctlevel_t::read_committed, read_only) ==
                      0,
                  sm1::errmsg());
}

/* Commit, failing the test on an error. */
void commit_xct(sm1::session_t *s) {
  REQUIRE_MESSAGE(sm1::commit(s) == 0, sm1::errmsg());
}

/* Open a load into storeid, failing the test on an error. */
sm1::loader_t *open_load(sm1::session_t *s, uint64_t storeid) {
  sm1::loader_t *l = nullptr;
  REQUIRE_MESSAGE(sm1::load_open(s, storeid, l) == 0, sm1::errmsg());
  return l;
}

/* Declare a stream of l, failing the test on an error. */
size_t stream(sm1::loader_t *l, bool ascending) {
  size_t n = 0;
  REQUIRE(sm1::load_stream(l, ascending, n) == 0);
  return n;
}

/* The key of row i under prefix: prefix then six digits. */
std::string key_of(char prefix, int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%c%06d", prefix, i);
  return buf;
}

/* Every key of storeid as s's transaction sees it, in order. */
std::vector<std::string> keys(sm1::session_t *s, uint64_t storeid) {
  std::vector<std::string> out;
  sm1::scan_t *c = scan(s, storeid, nullptr, 0, 1, nullptr, 0, 1);
  REQUIRE_MESSAGE(c, sm1::errmsg());
  std::string_view k, v;
  int rc;
  while ((rc = sm1::scannxt(c, k, v)) == 0) {
    out.emplace_back(k);
  }
  sm1::scanend(c);
  REQUIRE(rc == 1);
  return out;
}

/* How many sorted files RocksDB holds for storeid. */
size_t sst_files(sm1::session_t *s, uint64_t storeid) {
  store_t st = db_store(s->db, storeid);
  REQUIRE(st.cf);
  rocksdb::ColumnFamilyMetaData meta;
  st.rdb->GetColumnFamilyMetaData(st.cf, &meta);
  return meta.file_count;
}

/* The directory every load writes its files under: the temporary one's. */
std::string load_dir(sm1::session_t *s) {
  return s->db->tmp->GetName() + "/load";
}

/* Whether path is there and holds at least one entry. */
bool has_entries(const std::string &path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) &&
         !std::filesystem::is_empty(path, ec);
}

/*
 * Load n rows under 'a', in order, into an ascending stream, and
 * the same n under 'b', shuffled, into one that is sorted; the
 * loader is left open.
 */
sm1::loader_t *load_two(sm1::session_t *s, uint64_t storeid, int n) {
  sm1::loader_t *l = open_load(s, storeid);
  size_t a = stream(l, true), b = stream(l, false);
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), std::mt19937(7));
  for (int i = 0; i < n; i++) {
    REQUIRE_MESSAGE(
        sm1::load_put(l, a, key_of('a', i), "va" + std::to_string(i)) == 0,
        sm1::errmsg());
    REQUIRE_MESSAGE(sm1::load_put(l, b, key_of('b', order[i]),
                                  "vb" + std::to_string(order[i])) == 0,
                    sm1::errmsg());
  }
  return l;
}

/* The keys load_two loads, in the store's order. */
std::vector<std::string> two_keys(int n) {
  std::vector<std::string> out;
  for (char p : {'a', 'b'}) {
    for (int i = 0; i < n; i++) {
      out.push_back(key_of(p, i));
    }
  }
  return out;
}

} // namespace

TEST_CASE("a load and an emptiness check take a store the transaction sees") {
  // Verify sm1::load_open and sm1::store_empty find their store as every
  // write and read does: not the catalog's, store 0, and not one another
  // transaction made and has not committed.
  // 1. Both refuse store 0 with SM_ERR_INVALID.
  // 2. Another session makes a store and does not commit; both refuse it
  //    with SM_ERR_NOTFOUND.
  // 3. Another session makes a temporary store; both refuse it with
  //    SM_ERR_NOTFOUND.
  load_fixture_t f;
  sm1::session_t *other = f.open();
  sm1::loader_t *l = nullptr;
  bool empty = false;
  begin_xct(f.s);
  CHECK(sm1::load_open(f.s, 0, l) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::store_empty(f.s, 0, empty) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  // 2.
  begin_xct(other);
  uint64_t made = mkstore(other);
  CHECK(sm1::load_open(f.s, made, l) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::store_empty(f.s, made, empty) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  // 3.
  uint64_t temp = 0;
  REQUIRE(sm1::mkstore(other, temp, true) == 0);
  CHECK(sm1::load_open(f.s, temp, l) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::store_empty(f.s, temp, empty) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(other) == 0);
  REQUIRE(sm1::commit(f.s) == 0);
  sm1::closedb(other);
}

TEST_CASE("a load's keys are the transaction's own once it closes") {
  // Verify a load in each of its two forms: one batch, and files.
  // 1. Make a store, open a load, and put 500 keys to an ascending
  //    stream and 500, shuffled, to one that is sorted.
  // 2. Before the close the transaction reads none of them.
  // 3. After it, it reads every one, in order, with its value.
  // 4. The small load left no sorted file; the other only files,
  //    and its directory is gone.
  // 5. Another session sees no such store until the commit, and
  //    every key after it.
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    sm1::session_t *other = f.open();
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    sm1::loader_t *l = load_two(f.s, store, 500);
    CHECK(keys(f.s, store).empty());
    REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
    CHECK(keys(f.s, store) == two_keys(500));
    CHECK(get(f.s, store, key_of('a', 7)) == "va7");
    CHECK(get(f.s, store, key_of('b', 499)) == "vb499");
    CHECK((sst_files(f.s, store) > 0) == files);
    CHECK_FALSE(has_entries(load_dir(f.s)));

    begin_xct(other);
    std::string_view v;
    CHECK(sm1::read(other, store, key_of('a', 7), v) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
    commit_xct(other);
    commit_xct(f.s);
    begin_xct(other);
    CHECK(keys(other, store) == two_keys(500));
    CHECK(get(other, store, key_of('b', 0)) == "vb0");
    commit_xct(other);
  }
}

TEST_CASE("a loaded row is written like any other afterwards") {
  // Verify an ordinary write meets a loaded version as it would
  // one it had put itself.
  // 1. Load rows as files and close.
  // 2. In the same transaction update one, erase one, and insert
  //    a new one; inserting a loaded key again is SM_ERR_DUPKEY.
  // 3. Commit: another session reads the three changes.
  load_fixture_t f(files_options());
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  REQUIRE(sm1::load_close(load_two(f.s, store, 50)) == 0);
  REQUIRE_MESSAGE(sm1::update(f.s, store, key_of('a', 1), "changed") == 0,
                  sm1::errmsg());
  REQUIRE(sm1::erase(f.s, store, key_of('a', 2)) == 0);
  put(f.s, store, "c-new", "new");
  CHECK(sm1::insert(f.s, store, key_of('b', 3), "again") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  commit_xct(f.s);

  sm1::session_t *other = f.open();
  begin_xct(other);
  CHECK(get(other, store, key_of('a', 1)) == "changed");
  CHECK(get(other, store, key_of('a', 2)) == "<none>");
  CHECK(get(other, store, "c-new") == "new");
  CHECK(keys(other, store).size() == 100);
  commit_xct(other);
}

TEST_CASE("a load sorts in runs when its memory is short, and merges them") {
  // Verify the external sort, with a budget of a few rows.
  // 1. A database whose loads sort in 2000 bytes and close a file
  //    at 4096.
  // 2. Load 3000 keys in order and 3000 shuffled.
  // 3. While it is open, runs are on disk; after the close every
  //    key reads back in order, in more than one sorted file, and
  //    the load's directory is gone.
  sm1::options_t opt = files_options();
  opt.load_sort_bytes = 2000;
  opt.load_file_bytes = 4096;
  load_fixture_t f(opt);
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  sm1::loader_t *l = load_two(f.s, store, 3000);
  bool run = false;
  for (const auto &e :
       std::filesystem::recursive_directory_iterator(load_dir(f.s))) {
    run = run || e.path().filename().string().rfind("run-", 0) == 0;
  }
  CHECK(run);
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  CHECK(keys(f.s, store) == two_keys(3000));
  CHECK(get(f.s, store, key_of('b', 2999)) == "vb2999");
  CHECK(sst_files(f.s, store) > 2);
  CHECK_FALSE(has_entries(load_dir(f.s)));
  commit_xct(f.s);
}

TEST_CASE("two equal keys fail a load, wherever they meet") {
  // Verify the one check a load makes: no two of its keys are equal.
  // 1. In an ascending stream the second fails its put.
  // 2. In a sorted stream, as a batch and as files, the close fails.
  // 3. In a sorted stream that has spilled, with the two in
  //    different runs, the close fails.
  // 4. Each is SM_ERR_DUPKEY, and the abort that follows leaves no
  //    store and no file.
  {
    load_fixture_t f;
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    sm1::loader_t *l = open_load(f.s, store);
    size_t a = stream(l, true);
    REQUIRE(sm1::load_put(l, a, "k1", "v") == 0);
    CHECK(sm1::load_put(l, a, "k1", "w") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    REQUIRE(sm1::abort(f.s) == 0);
  }
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    sm1::loader_t *l = open_load(f.s, store);
    size_t b = stream(l, false);
    for (const char *k : {"k3", "k1", "k2", "k1"}) {
      REQUIRE(sm1::load_put(l, b, k, "v") == 0);
    }
    CHECK(sm1::load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    REQUIRE(sm1::abort(f.s) == 0);
    CHECK_FALSE(has_entries(load_dir(f.s)));
    begin_xct(f.s);
    CHECK(lsstore(f.s).empty());
    commit_xct(f.s);
  }
  {
    sm1::options_t opt = files_options();
    opt.load_sort_bytes = 500;
    load_fixture_t f(opt);
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    sm1::loader_t *l = open_load(f.s, store);
    size_t b = stream(l, false);
    for (int i = 0; i < 200; i++) {
      REQUIRE(sm1::load_put(l, b, key_of('b', i), "v") == 0);
    }
    REQUIRE(sm1::load_put(l, b, key_of('b', 3), "again") == 0);
    CHECK(sm1::load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    REQUIRE(sm1::abort(f.s) == 0);
    CHECK_FALSE(has_entries(load_dir(f.s)));
  }
}

TEST_CASE("a stream's keys are in order if it says so, and its range its own") {
  // Verify what a caller promises of its streams is checked.
  // 1. A key below the last in an ascending stream fails its put
  //    with SM_ERR_INVALID.
  // 2. Two streams whose key ranges overlap fail the close with
  //    SM_ERR_INVALID, as a batch and as files.
  // 3. A put to a stream never declared is SM_ERR_INVALID.
  {
    load_fixture_t f;
    begin_xct(f.s);
    sm1::loader_t *l = open_load(f.s, mkstore(f.s));
    size_t a = stream(l, true);
    REQUIRE(sm1::load_put(l, a, "k2", "v") == 0);
    CHECK(sm1::load_put(l, a, "k1", "v") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(sm1::load_put(l, a + 1, "k9", "v") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    REQUIRE(sm1::abort(f.s) == 0);
  }
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    begin_xct(f.s);
    sm1::loader_t *l = open_load(f.s, mkstore(f.s));
    size_t a = stream(l, true), b = stream(l, false);
    REQUIRE(sm1::load_put(l, a, "k1", "v") == 0);
    REQUIRE(sm1::load_put(l, a, "k5", "v") == 0);
    REQUIRE(sm1::load_put(l, b, "k3", "v") == 0);
    CHECK(sm1::load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    REQUIRE(sm1::abort(f.s) == 0);
  }
}

TEST_CASE("a load is into an empty store, whoever made it") {
  // Verify what load_open takes and refuses.
  // 1. A store committed earlier and still empty is taken, as
  //    store_empty says: loaded and committed, another session
  //    reads its keys.
  // 2. A store that holds a key, committed or this transaction's
  //    own, is SM_ERR_INVALID, and store_empty says it is not.
  // 3. A store whose one row was deleted still holds a version,
  //    and is refused.
  // 4. A second load while one is open: SM_ERR_INVALID.
  // 5. With a load open, an ordinary write to its store is
  //    SM_ERR_INVALID, and one to another store goes through.
  // 6. A read-only transaction: SM_ERR_READONLY for a store, and
  //    allowed for a temporary one, which then reads back.
  load_fixture_t f(files_options());
  begin_xct(f.s);
  uint64_t old = mkstore(f.s), full = mkstore(f.s), gone = mkstore(f.s);
  put(f.s, full, "k", "v");
  put(f.s, gone, "k", "v");
  commit_xct(f.s);
  begin_xct(f.s);
  REQUIRE(sm1::erase(f.s, gone, "k") == 0);
  commit_xct(f.s);

  begin_xct(f.s);
  bool empty = false;
  REQUIRE(sm1::store_empty(f.s, old, empty) == 0);
  CHECK(empty);
  REQUIRE_MESSAGE(sm1::load_close(load_two(f.s, old, 100)) == 0, sm1::errmsg());
  commit_xct(f.s);
  sm1::session_t *other = f.open();
  begin_xct(other);
  CHECK(keys(other, old) == two_keys(100));
  commit_xct(other);

  begin_xct(f.s);
  sm1::loader_t *l = nullptr;
  for (uint64_t store : {full, gone, old}) {
    CAPTURE(store);
    REQUIRE(sm1::store_empty(f.s, store, empty) == 0);
    CHECK_FALSE(empty);
    CHECK(sm1::load_open(f.s, store, l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  uint64_t written = mkstore(f.s);
  put(f.s, written, "k", "v");
  CHECK(sm1::load_open(f.s, written, l) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  uint64_t fresh = mkstore(f.s), second = mkstore(f.s);
  l = open_load(f.s, fresh);
  sm1::loader_t *l2 = nullptr;
  CHECK(sm1::load_open(f.s, second, l2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::insert(f.s, fresh, "k", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  put(f.s, second, "k", "v");
  REQUIRE(sm1::abort(f.s) == 0);

  begin_xct(f.s, 1);
  uint64_t temp = 0;
  REQUIRE(sm1::mkstore(f.s, temp, true) == 0);
  l = open_load(f.s, temp);
  size_t a = stream(l, true);
  REQUIRE(sm1::load_put(l, a, "k1", "v1") == 0);
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  CHECK(get(f.s, temp, "k1") == "v1");
  commit_xct(f.s);
}

TEST_CASE("a load open into a store refuses its chained writes and its drop") {
  // Verify sm1.h's rule, that while a load is open the transaction's
  // other writes to its store are refused, holds for a chained store's
  // writes and for sm1::rmstore, as for a plain store's writes.
  // 1. A load opens into a chained store; an insert, an update and an
  //    erase there are SM_ERR_INVALID.
  // 2. sm1::rmstore of the chained store is SM_ERR_INVALID.
  // 3. A load opens into a plain store; its sm1::rmstore is
  //    SM_ERR_INVALID.
  load_fixture_t f;
  begin_xct(f.s);
  uint64_t chained = 0;
  REQUIRE(sm1::mkstore(f.s, chained, false, true) == 0);
  open_load(f.s, chained);
  CHECK(sm1::chain_insert(f.s, chained, "k", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::chain_update(f.s, chained, "k", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::chain_erase(f.s, chained, "k") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  // 2.
  CHECK(sm1::rmstore(f.s, chained) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(f.s) == 0);

  // 3.
  begin_xct(f.s);
  uint64_t plain = mkstore(f.s);
  open_load(f.s, plain);
  CHECK(sm1::rmstore(f.s, plain) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(f.s) == 0);
}

TEST_CASE("a transaction cannot commit or prepare with a load open") {
  // Verify how a load ties its transaction's hands.
  // 1. With a load open, commit and prepare are SM_ERR_INVALID, and
  //    the transaction goes on.
  // 2. After the close, prepare succeeds and the session's commit
  //    resolves it (../XCTMGR.md §10.1).
  // 3. A savepoint set before a load opened is rolled back to, and
  //    the load is gone, after the commit too.
  load_fixture_t f;
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  sm1::loader_t *l = load_two(f.s, store, 10);
  CHECK(sm1::commit(f.s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::prepare(f.s, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm1::prepare(f.s, "g") == 0, sm1::errmsg());
  commit_xct(f.s);

  begin_xct(f.s);
  uint64_t other = mkstore(f.s);
  int sp = sm1::savepoint(f.s);
  REQUIRE(sp >= 1);
  REQUIRE(sm1::load_close(load_two(f.s, other, 10)) == 0);
  REQUIRE_MESSAGE(sm1::rollback(f.s, sp) == 0, sm1::errmsg());
  CHECK(keys(f.s, other).empty());
  commit_xct(f.s);
  begin_xct(f.s);
  CHECK(keys(f.s, other).empty());
  commit_xct(f.s);
}

TEST_CASE("no savepoint is set or rolled back to while a load is open") {
  // Verify a load is whole within its transaction: nothing in it
  // can be undone apart from the rest.
  // 1. Set a savepoint, then open a load and put keys into it.
  // 2. With the load open, a savepoint and a rollback are each
  //    SM_ERR_INVALID.
  // 3. The load closes with every key, and the transaction commits.
  load_fixture_t f;
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  int sp = sm1::savepoint(f.s);
  REQUIRE(sp >= 1);
  sm1::loader_t *l = load_two(f.s, store, 10);
  CHECK(sm1::savepoint(f.s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::rollback(f.s, sp) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  CHECK(keys(f.s, store) == two_keys(10));
  commit_xct(f.s);
}

TEST_CASE("a rollback to a savepoint set before a load empties its store") {
  // Verify a rollback undoes a load it reaches back past, in either
  // of the load's two forms, and only that.
  // 1. In a store committed empty, set a savepoint, load and close,
  //    and write one key more.
  // 2. Roll back to the savepoint: no key reads, loaded or written.
  // 3. Load the store again, set a savepoint after the close, write
  //    a key, and roll back to it: the load stays, the key goes.
  // 4. Commit and attach the home again: the second load's keys
  //    read back, and nothing of the first.
  // 5. Set a savepoint, make a store and load it, and roll back:
  //    the store is gone, and the first is still listed.
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    commit_xct(f.s);

    begin_xct(f.s);
    int sp = sm1::savepoint(f.s);
    REQUIRE(sp >= 1);
    REQUIRE_MESSAGE(sm1::load_close(load_two(f.s, store, 300)) == 0,
                    sm1::errmsg());
    CHECK((sst_files(f.s, store) > 0) == files);
    put(f.s, store, "c", "vc");
    REQUIRE_MESSAGE(sm1::rollback(f.s, sp) == 0, sm1::errmsg());
    CHECK(keys(f.s, store).empty());
    CHECK(get(f.s, store, key_of('a', 1)) == "<none>");

    REQUIRE_MESSAGE(sm1::load_close(load_two(f.s, store, 50)) == 0,
                    sm1::errmsg());
    int inner = sm1::savepoint(f.s);
    REQUIRE(inner >= 1);
    put(f.s, store, "c", "vc");
    REQUIRE_MESSAGE(sm1::rollback(f.s, inner) == 0, sm1::errmsg());
    CHECK(keys(f.s, store) == two_keys(50));
    commit_xct(f.s);

    f.home.detach();
    f.home.attach();
    f.s = f.open();
    begin_xct(f.s);
    CHECK(keys(f.s, store) == two_keys(50));
    CHECK(get(f.s, store, key_of('a', 299)) == "<none>");
    commit_xct(f.s);

    begin_xct(f.s);
    sp = sm1::savepoint(f.s);
    REQUIRE(sp >= 1);
    uint64_t made = mkstore(f.s);
    REQUIRE_MESSAGE(sm1::load_close(load_two(f.s, made, 10)) == 0,
                    sm1::errmsg());
    REQUIRE_MESSAGE(sm1::rollback(f.s, sp) == 0, sm1::errmsg());
    CHECK(db_store(f.s->db, made).cf == nullptr);
    CHECK(lsstore(f.s) == std::vector<uint64_t>{store});
    commit_xct(f.s);
  }
}

TEST_CASE("a load whose close fails is ended, and its savepoint rolled "
          "back to") {
  // Verify a failed close leaves no loader open and the store empty,
  // so the transaction rolls back to its savepoint and goes on.
  // 1. Set a savepoint, load two equal keys, and close: the close
  //    fails with SM_ERR_DUPKEY, in either of the load's two forms.
  // 2. The store holds no key, its files are gone, and a second
  //    load could open.
  // 3. Roll back to the savepoint, write a key, and commit.
  // 4. From a new transaction only that key reads.
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    commit_xct(f.s);

    begin_xct(f.s);
    int sp = sm1::savepoint(f.s);
    REQUIRE(sp >= 1);
    sm1::loader_t *l = load_two(f.s, store, 300);
    REQUIRE(sm1::load_put(l, 1, key_of('b', 7), "again") == 0);
    CHECK(sm1::load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    CHECK(keys(f.s, store).empty());
    CHECK_FALSE(has_entries(load_dir(f.s)));
    CHECK_FALSE(load_open_on(f.s, store));

    REQUIRE_MESSAGE(sm1::rollback(f.s, sp) == 0, sm1::errmsg());
    put(f.s, store, "c", "vc");
    commit_xct(f.s);

    begin_xct(f.s);
    CHECK(keys(f.s, store) == std::vector<std::string>{"c"});
    commit_xct(f.s);
  }
}

TEST_CASE("an abort leaves nothing of a load, before its close or after") {
  // Verify a load ends with its transaction.
  // 1. Load as files and abort with the load open: its files were
  //    on disk, and are gone; the store is gone.
  // 2. Load, close, and abort: the store is gone.
  // 3. A later transaction lists no store.
  // 4. A store committed empty, loaded and closed, then aborted,
  //    is still there, and no one reads a key of the load in it.
  load_fixture_t f(files_options());
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  load_two(f.s, store, 200);
  CHECK(has_entries(load_dir(f.s)));
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK_FALSE(has_entries(load_dir(f.s)));

  begin_xct(f.s);
  store = mkstore(f.s);
  REQUIRE(sm1::load_close(load_two(f.s, store, 200)) == 0);
  REQUIRE(sm1::abort(f.s) == 0);
  begin_xct(f.s);
  CHECK(lsstore(f.s).empty());
  CHECK(db_store(f.s->db, store).cf == nullptr);
  uint64_t kept = mkstore(f.s);
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE(sm1::load_close(load_two(f.s, kept, 200)) == 0);
  CHECK(keys(f.s, kept).size() == 400);
  REQUIRE(sm1::abort(f.s) == 0);
  begin_xct(f.s);
  CHECK(lsstore(f.s) == std::vector<uint64_t>{kept});
  CHECK(keys(f.s, kept).empty());
  CHECK(get(f.s, kept, key_of('a', 1)) == "<none>");
  commit_xct(f.s);
}

TEST_CASE("a committed load is there after a restart, and a crash's files "
          "are not") {
  // Verify what a load leaves on disk.
  // 1. Load as files, commit, and attach the home again: every key
  //    reads back, though none went through the log.
  // 2. Leave a file under the load directory, as a crash in the
  //    middle of a load would, and attach again: it is gone, with
  //    the rest of the temporary instance.
  // 3. While a load is open the database's own directory holds
  //    nothing of it.
  load_fixture_t f(files_options());
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  REQUIRE(sm1::load_close(load_two(f.s, store, 300)) == 0);
  commit_xct(f.s);
  std::string dir = load_dir(f.s);
  begin_xct(f.s);
  load_two(f.s, mkstore(f.s), 300);
  CHECK(has_entries(dir));
  CHECK_FALSE(std::filesystem::exists(f.s->db->rdb->GetName() + "/load"));
  REQUIRE(sm1::abort(f.s) == 0);

  f.home.detach();
  std::filesystem::create_directories(dir + "/9-9");
  std::ofstream(dir + "/9-9/file-0.sst") << "left by a crash";
  REQUIRE(has_entries(dir));
  f.home.attach();
  f.s = f.open();
  CHECK_FALSE(has_entries(dir));
  begin_xct(f.s);
  CHECK(keys(f.s, store) == two_keys(300));
  CHECK(get(f.s, store, key_of('a', 299)) == "va299");
  commit_xct(f.s);
}

namespace {

/* The key of page i, as a tree's: a relid, then i, big-endian. */
std::string page_of(uint16_t relid, uint64_t i) {
  std::string k;
  k.push_back(static_cast<char>(relid >> 8));
  k.push_back(static_cast<char>(relid & 0xFF));
  for (int b = 7; b >= 0; b--) {
    k.push_back(static_cast<char>(i >> (8 * b)));
  }
  return k;
}

/* The bytes of page key, or "<none>" if no page is there. */
std::string page_bytes(sm1::session_t *s, uint64_t storeid,
                       const std::string &key) {
  sm1::page_t *page = nullptr;
  std::string_view bytes;
  int r = sm1::pin_page(s, storeid, key, sm1::page_mode_t::sh, 0, page, bytes);
  REQUIRE_MESSAGE(r >= 0, sm1::errmsg());
  if (r == 1) {
    return "<none>";
  }
  std::string out(bytes);
  sm1::unpin_page(s, page);
  return out;
}

/* Declare a page stream of l, failing the test on an error. */
size_t page_stream(sm1::loader_t *l) {
  size_t n = 0;
  REQUIRE(sm1::load_page_stream(l, n) == 0);
  return n;
}

/* Declare a drained stream of l, failing the test on an error. */
size_t drain_stream(sm1::loader_t *l) {
  size_t n = 0;
  REQUIRE(sm1::load_drain_stream(l, n) == 0);
  return n;
}

} // namespace

TEST_CASE("a load puts a page stream's pages as they are, beside its rows") {
  // Verify a page stream: pages, not versions, in a batch and in files.
  // 1. For a small load (a batch) and a large one (files), load 300
  //    rows and 300 pages of relid 7.
  // 2. After the close, each page reads back by pin_page with its
  //    bytes, and a scan of the store gives the rows alone.
  // 3. A page key out of order fails its put.
  for (bool files : {false, true}) {
    CAPTURE(files);
    load_fixture_t f(files ? files_options() : sm1::options_t());
    begin_xct(f.s);
    uint64_t store = mkstore(f.s);
    sm1::loader_t *l = open_load(f.s, store);
    size_t rows = stream(l, true), pages = page_stream(l);
    for (int i = 1; i <= 300; i++) {
      REQUIRE(sm1::load_put(l, rows, key_of('a', i), "row") == 0);
      REQUIRE_MESSAGE(sm1::load_put(l, pages, page_of(7, i),
                                    "page" + std::to_string(i)) == 0,
                      sm1::errmsg());
    }
    CHECK(sm1::load_put(l, pages, page_of(7, 5), "x") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
    CHECK(page_bytes(f.s, store, page_of(7, 1)) == "page1");
    CHECK(page_bytes(f.s, store, page_of(7, 300)) == "page300");
    CHECK(page_bytes(f.s, store, page_of(7, 301)) == "<none>");
    CHECK(keys(f.s, store).size() == 300);
    commit_xct(f.s);
  }
}

TEST_CASE("two page streams of one range fail a load's close") {
  // Verify page streams are checked apart from each other.
  // 1. Load pages 1 to 10 of relid 7 in one page stream and 5 to 15
  //    in another: the close fails with SM_ERR_INVALID.
  // 2. Pages of relids 7 and 8 in two streams close.
  load_fixture_t f(files_options());
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  sm1::loader_t *l = open_load(f.s, store);
  size_t a = page_stream(l), b = page_stream(l);
  for (int i = 1; i <= 10; i++) {
    REQUIRE(sm1::load_put(l, a, page_of(7, i), "p") == 0);
    REQUIRE(sm1::load_put(l, b, page_of(7, i + 4), "p") == 0);
  }
  CHECK(sm1::load_close(l) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  l = open_load(f.s, store);
  a = page_stream(l);
  b = page_stream(l);
  for (int i = 1; i <= 10; i++) {
    REQUIRE(sm1::load_put(l, a, page_of(7, i), "p") == 0);
    REQUIRE(sm1::load_put(l, b, page_of(8, i), "p") == 0);
  }
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  CHECK(page_bytes(f.s, store, page_of(8, 10)) == "p");
  commit_xct(f.s);
}

TEST_CASE("a drained stream gives its keys back in order, and none to the "
          "store") {
  // Verify a drained stream sorts as any other, but for its caller.
  // 1. With a budget of a few keys, put 2000 shuffled keys into a
  //    drained stream, and 2000 rows into an ascending one.
  // 2. Drain it: every key in order, with its value, then 1.
  // 3. A put after the drain fails; so does draining a stream that
  //    is not drained.
  // 4. After the close the store holds the rows alone, and the
  //    load's directory is gone.
  sm1::options_t opt = files_options();
  opt.load_sort_bytes = 2000;
  load_fixture_t f(opt);
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  sm1::loader_t *l = open_load(f.s, store);
  size_t rows = stream(l, true), d = drain_stream(l);
  std::vector<int> order(2000);
  for (int i = 0; i < 2000; i++) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), std::mt19937(3));
  for (int i = 0; i < 2000; i++) {
    REQUIRE(sm1::load_put(l, rows, key_of('a', i), "row") == 0);
    REQUIRE(sm1::load_put(l, d, key_of('z', order[i]),
                          std::to_string(order[i])) == 0);
  }
  std::string_view k, v;
  for (int i = 0; i < 2000; i++) {
    REQUIRE_MESSAGE(sm1::load_drain(l, d, k, v) == 0, sm1::errmsg());
    CHECK(k == key_of('z', i));
    CHECK(v == std::to_string(i));
  }
  CHECK(sm1::load_drain(l, d, k, v) == 1);
  CHECK(sm1::load_drain(l, d, k, v) == 1);
  CHECK(sm1::load_put(l, d, key_of('z', 5000), "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::load_drain(l, rows, k, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE_MESSAGE(sm1::load_close(l) == 0, sm1::errmsg());
  CHECK(keys(f.s, store).size() == 2000);
  CHECK(get(f.s, store, key_of('z', 1)) == "<none>");
  CHECK_FALSE(has_entries(load_dir(f.s)));
  commit_xct(f.s);
}

TEST_CASE("two equal keys of a drained stream fail its drain") {
  // Verify a drained stream's keys are unique, as every stream's.
  // 1. Put 500 keys and one of them again into a drained stream,
  //    with a budget that spills it to runs.
  // 2. Draining it fails with SM_ERR_DUPKEY before its end.
  sm1::options_t opt = files_options();
  opt.load_sort_bytes = 2000;
  load_fixture_t f(opt);
  begin_xct(f.s);
  uint64_t store = mkstore(f.s);
  sm1::loader_t *l = open_load(f.s, store);
  size_t d = drain_stream(l);
  for (int i = 0; i < 500; i++) {
    REQUIRE(sm1::load_put(l, d, key_of('z', i), "v") == 0);
  }
  REQUIRE(sm1::load_put(l, d, key_of('z', 17), "v") == 0);
  std::string_view k, v;
  int rc;
  int n = 0;
  while ((rc = sm1::load_drain(l, d, k, v)) == 0) {
    n++;
  }
  CHECK(rc == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(n < 500);
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK_FALSE(has_entries(load_dir(f.s)));
}
