/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_temp.cc -- temporary stores: sm1::mkstore with its
 * temporary flag (TEMPTABLE.md §3).
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"

#include <filesystem>
#include <string>

#include <rocksdb/db.h>

using namespace wisr;

namespace {

/* Create a temporary store, failing the test on an error. */
uint64_t mktemp_store(sm1::session_t *s) {
  uint64_t storeid = 0;
  REQUIRE_MESSAGE(sm1::mkstore(s, storeid, true) == 0, sm1::errmsg());
  return storeid;
}

/* Begin a transaction at level, failing the test on an error. */
void begin_xct(sm1::session_t *s,
               sm1::xctlevel_t level = sm1::xctlevel_t::read_committed,
               int read_only = 0) {
  REQUIRE_MESSAGE(sm1::begin(s, level, read_only) == 0, sm1::errmsg());
}

/* Commit, failing the test on an error. */
void commit_xct(sm1::session_t *s) {
  REQUIRE_MESSAGE(sm1::commit(s) == 0, sm1::errmsg());
}

/* A home directory with database "db" and one session on it. */
struct temp_fixture_t {
  home_t home;
  sm1::session_t *s = nullptr;

  /* Make "db" and open the session. */
  temp_fixture_t() {
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

TEST_CASE("a temporary store lives in the temporary instance, off store 0") {
  // Verify where a temporary store is kept and that no catalog row names it.
  // 1. Make a temporary store and a row in it, and commit.
  // 2. Confirm its column family is in the environment's temporary
  //    instance, not the database's.
  // 3. Confirm sm1::lsstore does not list it.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "k", "v");
  commit_xct(f.s);

  store_t st = db_store(f.s->db, t);
  REQUIRE(st.cf);
  CHECK(st.rdb == f.home.env()->tmp);
  CHECK(st.rdb != f.s->db->rdb);

  begin_xct(f.s);
  CHECK(lsstore(f.s).empty());
  CHECK(get(f.s, t, "k") == "v");
  commit_xct(f.s);
}

TEST_CASE("a temporary store's rows commit, abort and roll back") {
  // Verify a temporary store keeps the transaction's guarantees.
  // 1. Insert "a" and commit: it is there.
  // 2. Insert "b" and abort: it is not.
  // 3. Insert "c", set a savepoint, update "a" and insert "d", and roll
  //    back to the savepoint: "c" stays, "a" is as before, "d" is gone.
  // 4. Commit, and read the same.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "a", "1");
  commit_xct(f.s);

  begin_xct(f.s);
  put(f.s, t, "b", "1");
  REQUIRE(sm1::abort(f.s) == 0);
  begin_xct(f.s);
  CHECK(get(f.s, t, "a") == "1");
  CHECK(get(f.s, t, "b") == "<none>");

  put(f.s, t, "c", "1");
  int depth = sm1::savepoint(f.s);
  REQUIRE(depth > 0);
  REQUIRE(sm1::update(f.s, t, "a", "2") == 0);
  put(f.s, t, "d", "1");
  CHECK(get(f.s, t, "a") == "2");
  REQUIRE_MESSAGE(sm1::rollback(f.s, depth) == 0, sm1::errmsg());
  CHECK(get(f.s, t, "a") == "1");
  CHECK(get(f.s, t, "c") == "1");
  CHECK(get(f.s, t, "d") == "<none>");
  commit_xct(f.s);

  begin_xct(f.s);
  CHECK(get(f.s, t, "a") == "1");
  CHECK(get(f.s, t, "c") == "1");
  CHECK(get(f.s, t, "d") == "<none>");
  commit_xct(f.s);
}

TEST_CASE("a temporary store is private to its session") {
  // Verify no other session reaches a temporary store.
  // 1. Session S makes a temporary store with a row, and commits.
  // 2. Session R reads, writes, scans, locks and drops it: each fails
  //    with SM_ERR_NOTFOUND.
  // 3. S still reads its row.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "k", "v");
  commit_xct(f.s);

  sm1::session_t *r = f.open();
  begin_xct(r);
  std::string_view data;
  CHECK(sm1::read(r, t, "k", data) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::insert(r, t, "x", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(scan(r, t, nullptr, 0, 0, nullptr, 0, 0) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::lock_row(r, t, "k", sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::rmstore(r, t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  commit_xct(r);
  REQUIRE(sm1::closedb(r) == 0);

  begin_xct(f.s);
  CHECK(get(f.s, t, "k") == "v");
  commit_xct(f.s);
}

TEST_CASE("making and dropping a temporary store follow the transaction") {
  // Verify a temporary store's making and dropping can be undone.
  // 1. Make one and abort: it is gone, and its column family with it.
  // 2. Make one and commit; drop it and abort: it is still there.
  // 3. Drop it under a savepoint and roll back: still there.
  // 4. Drop it and commit: it is gone, and its column family with it.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t gone = mktemp_store(f.s);
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK(db_store(f.s->db, gone).cf == nullptr);

  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "k", "v");
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE_MESSAGE(sm1::rmstore(f.s, t) == 0, sm1::errmsg());
  std::string_view data;
  CHECK(sm1::read(f.s, t, "k", data) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(f.s) == 0);

  begin_xct(f.s);
  CHECK(get(f.s, t, "k") == "v");
  int depth = sm1::savepoint(f.s);
  REQUIRE(sm1::rmstore(f.s, t) == 0);
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  CHECK(get(f.s, t, "k") == "v");
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE(sm1::rmstore(f.s, t) == 0);
  commit_xct(f.s);
  CHECK(db_store(f.s->db, t).cf == nullptr);
  begin_xct(f.s);
  CHECK(sm1::read(f.s, t, "k", data) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  commit_xct(f.s);
}

TEST_CASE("closing a session drops its temporary stores") {
  // Verify a temporary store does not outlive its session.
  // 1. Session R, kept open, holds the database open.
  // 2. Session S makes a temporary store, commits, and closes.
  // 3. Confirm the store's column family is gone.
  temp_fixture_t f;
  sm1::session_t *r = f.open();
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "k", "v");
  commit_xct(f.s);
  REQUIRE(db_store(r->db, t).cf != nullptr);
  REQUIRE(sm1::closedb(f.s) == 0);
  CHECK(db_store(r->db, t).cf == nullptr);
  REQUIRE(sm1::closedb(r) == 0);
}

TEST_CASE("a temporary store takes no lock and no SIREAD entry") {
  // Verify the concurrency control a temporary store skips.
  // 1. In a SERIALIZABLE transaction, make a temporary store; insert,
  //    read, scan and lock a row of it.
  // 2. Confirm the lock table holds nothing, and the SSI tracker no
  //    point and no range.
  // 3. At READ COMMITTED, lock a row and read its latest version: it
  //    needs no lock held.
  temp_fixture_t f;
  begin_xct(f.s, sm1::xctlevel_t::serializable);
  uint64_t t = mktemp_store(f.s);
  put(f.s, t, "k", "v");
  CHECK(get(f.s, t, "k") == "v");
  sm1::scan_t *c = scan(f.s, t, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  std::string_view k, d;
  CHECK(sm1::scannxt(c, k, d) == 0);
  sm1::scanend(c);
  REQUIRE(sm1::lock_row(f.s, t, "k", sm1::SM_LOCKMODE_EX) == 0);
  db_t *db = f.s->db;
  {
    wisr::lock_all_t all(db);
    CHECK(db->lock_table.empty());
  }
  {
    std::lock_guard<std::mutex> guard(db->ssi_mu);
    CHECK(db->ssi_points.empty());
    CHECK(db->ssi_ranges.empty());
  }
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE(sm1::lock_row(f.s, t, "k", sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm1::read_latest(f.s, t, "k", d) == 0);
  CHECK(d == "v");
  commit_xct(f.s);
}

TEST_CASE("a temporary store is not logged") {
  // Verify a write to a temporary store reaches no write-ahead log.
  // 1. Make a temporary store and insert 100 rows, and commit.
  // 2. Flush the temporary instance's log to its files.
  // 3. Confirm every log file of the instance is empty.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  for (int i = 0; i < 100; i++) {
    put(f.s, t, "k" + std::to_string(i), std::string(100, 'v'));
  }
  commit_xct(f.s);
  REQUIRE(f.home.env()->tmp->FlushWAL(true).ok());
  uintmax_t logged = 0;
  for (const auto &e : std::filesystem::directory_iterator(
           std::string(f.home.path()) + "/base/pgsql_tmp/sm")) {
    if (e.path().extension() == ".log") {
      logged += e.file_size();
    }
  }
  CHECK(logged == 0);
}

TEST_CASE("a read-only transaction may write a temporary store") {
  // Verify READ ONLY does not cover temporary stores, at each level.
  // 1. Begin read-only; make a temporary store and insert a row; commit.
  // 2. Begin read-only again; update the row; commit.
  // 3. Confirm the update is there, and a store of the database's is
  //    still refused.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    temp_fixture_t f;
    begin_xct(f.s, level, 1);
    uint64_t t = mktemp_store(f.s);
    put(f.s, t, "k", "1");
    commit_xct(f.s);

    begin_xct(f.s, level, 1);
    REQUIRE_MESSAGE(sm1::update(f.s, t, "k", "2") == 0, sm1::errmsg());
    uint64_t storeid;
    CHECK(sm1::mkstore(f.s, storeid) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_READONLY);
    commit_xct(f.s);

    begin_xct(f.s, level);
    CHECK(get(f.s, t, "k") == "2");
    commit_xct(f.s);
  }
}

TEST_CASE("a transaction that touched a temporary store cannot prepare") {
  // Verify sm1::prepare refuses what would outlive the session's store.
  // 1. Make a temporary store and commit.
  // 2. Write a row of it and prepare: SM_ERR_INVALID, and the
  //    transaction is still active.
  // 3. Abort; a transaction that leaves the store alone prepares.
  temp_fixture_t f;
  begin_xct(f.s);
  uint64_t t = mktemp_store(f.s);
  uint64_t p = mkstore(f.s);
  commit_xct(f.s);

  begin_xct(f.s);
  put(f.s, t, "k", "v");
  CHECK(sm1::prepare(f.s, "g1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(f.s) == 0);

  begin_xct(f.s);
  put(f.s, p, "k", "v");
  REQUIRE_MESSAGE(sm1::prepare(f.s, "g2") == 0, sm1::errmsg());
  REQUIRE(sm1::commit(f.s) == 0);
}

TEST_CASE("two databases' temporary stores share an id and nothing else") {
  // Verify a storeid is a database's own in the shared instance.
  // 1. In databases "db" and "other", make a temporary store each.
  // 2. Confirm the two got the same storeid.
  // 3. Insert a different value under one key in each, and read each
  //    back from its own.
  temp_fixture_t f;
  REQUIRE(sm1::mkdb(f.home.env(), "other", nullptr) == 0);
  sm1::session_t *o = opendb(f.home.env(), "other", nullptr);
  REQUIRE(o);
  begin_xct(f.s);
  begin_xct(o);
  uint64_t t1 = mktemp_store(f.s);
  uint64_t t2 = mktemp_store(o);
  CHECK(t1 == t2);
  put(f.s, t1, "k", "db");
  put(o, t2, "k", "other");
  commit_xct(f.s);
  commit_xct(o);
  begin_xct(f.s);
  begin_xct(o);
  CHECK(get(f.s, t1, "k") == "db");
  CHECK(get(o, t2, "k") == "other");
  commit_xct(f.s);
  commit_xct(o);
  REQUIRE(sm1::closedb(o) == 0);
}

TEST_CASE("the CF:txn sweep keeps a temporary store's rows committed") {
  // Verify the sweep stamps temporary versions before it deletes the
  // records that say they committed.
  // 1. With a commit cache of one entry, commit 30 rows of a temporary
  //    store, a transaction each.
  // 2. Run the sweep: the records are deleted.
  // 3. Confirm every row still reads as committed.
  sm1::options_t opt;
  opt.commit_cache_entries = 1;
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", &opt) == 0);
  sm1::session_t *s = opendb(home.env(), "db", &opt);
  REQUIRE(s);
  begin_xct(s);
  uint64_t t = mktemp_store(s);
  commit_xct(s);
  for (int i = 0; i < 30; i++) {
    begin_xct(s);
    put(s, t, "k" + std::to_string(i), "v");
    commit_xct(s);
  }
  CHECK(gc_sweep_txn(s->db, true) >= 30);
  begin_xct(s, sm1::xctlevel_t::repeatable_read);
  for (int i = 0; i < 30; i++) {
    CHECK(get(s, t, "k" + std::to_string(i)) == "v");
  }
  commit_xct(s);
}

TEST_CASE("a temporary sequence counts in the session and writes nothing") {
  // Verify a temporary sequence hands out values without the database.
  // 1. In a read-only transaction, make a temporary sequence from 5 by 2.
  // 2. Confirm it hands out 5, 7 and 9, that sm1::lastseq reports 9, and
  //    that the database keeps no lease and no catalog row for it.
  // 3. Confirm another session does not know the name.
  temp_fixture_t f;
  begin_xct(f.s, sm1::xctlevel_t::read_committed, 1);
  REQUIRE_MESSAGE(sm1::mkseq(f.s, "q", 5, 2, true) == 0, sm1::errmsg());
  uint64_t v = 0;
  CHECK(sm1::lastseq(f.s, "q", v) == 1);
  for (uint64_t want : {5, 7, 9}) {
    REQUIRE_MESSAGE(sm1::nextseq(f.s, "q", v) == 0, sm1::errmsg());
    CHECK(v == want);
  }
  CHECK(sm1::lastseq(f.s, "q", v) == 0);
  CHECK(v == 9);
  CHECK(f.s->db->seqs.empty());
  CHECK(f.s->xid == 0);
  commit_xct(f.s);

  sm1::session_t *r = f.open();
  CHECK(sm1::nextseq(r, "q", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::closedb(r) == 0);
  REQUIRE(sm1::nextseq(f.s, "q", v) == 0);
  CHECK(v == 11);
}

TEST_CASE("making and dropping a temporary sequence follow the transaction") {
  // Verify a temporary sequence's making and dropping can be undone.
  // 1. Make one and abort: the name is unknown.
  // 2. Make one under a savepoint and roll back: unknown again.
  // 3. Make one and commit; drop it and abort: it goes on counting.
  // 4. Drop it under a savepoint and roll back: it goes on counting.
  // 5. Drop it: unknown to the transaction at once, and after commit;
  //    making it again starts from its start.
  temp_fixture_t f;
  uint64_t v = 0;
  begin_xct(f.s);
  REQUIRE(sm1::mkseq(f.s, "q", 1, 1, true) == 0);
  CHECK(sm1::mkseq(f.s, "q", 1, 1, true) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
  REQUIRE(sm1::abort(f.s) == 0);
  CHECK(sm1::nextseq(f.s, "q", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  begin_xct(f.s);
  int depth = sm1::savepoint(f.s);
  REQUIRE(sm1::mkseq(f.s, "q", 1, 1, true) == 0);
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  CHECK(sm1::nextseq(f.s, "q", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::mkseq(f.s, "q", 1, 1, true) == 0);
  REQUIRE(sm1::nextseq(f.s, "q", v) == 0);
  commit_xct(f.s);

  begin_xct(f.s);
  REQUIRE_MESSAGE(sm1::rmseq(f.s, "q") == 0, sm1::errmsg());
  REQUIRE(sm1::abort(f.s) == 0);
  REQUIRE(sm1::nextseq(f.s, "q", v) == 0);
  CHECK(v == 2);

  begin_xct(f.s);
  depth = sm1::savepoint(f.s);
  REQUIRE(sm1::rmseq(f.s, "q") == 0);
  REQUIRE(sm1::rollback(f.s, depth) == 0);
  REQUIRE(sm1::nextseq(f.s, "q", v) == 0);
  CHECK(v == 3);
  REQUIRE(sm1::rmseq(f.s, "q") == 0);
  CHECK(sm1::nextseq(f.s, "q", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  commit_xct(f.s);
  CHECK(sm1::nextseq(f.s, "q", v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  begin_xct(f.s);
  REQUIRE(sm1::mkseq(f.s, "q", 1, 1, true) == 0);
  REQUIRE(sm1::nextseq(f.s, "q", v) == 0);
  CHECK(v == 1);
  CHECK(sm1::prepare(f.s, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  commit_xct(f.s);
}
