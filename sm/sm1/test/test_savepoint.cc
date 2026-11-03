/*
 * test_savepoint.cc -- sm1::savepoint and sm1::rollback (SM1_DESIGN.md
 * §6.5, §12.5 "Own writes").
 *
 * The lock rule these calls obey is a timeline between two
 * sessions, so it lives in mvcc/in/b25_savepoint_releases_lock.t
 * and b31_savepoint_and_explicit_locks.t. What stays here is
 * the single-session part: values restored, the stack and its
 * bounds, cursors closed, and DDL undone.
 */

#include "doctest.h"
#include "util.h"

#include "session.h"
#include "sm1.h"

#include <string>
#include <utility>
#include <vector>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "a" -> "0". */
class sp_fixture_t {
public:
  /* Create "db", the store, and the row. */
  sp_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    g_store = mkstore(s);
    put(s, g_store, "a", "0");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }

  /* A new session on "db"; opt applies if it is the first. */
  sm1::session_t *open(const sm1::options_t *opt = nullptr) const {
    sm1::session_t *s = opendb(m_home.env(), "db", opt);
    REQUIRE(s);
    return s;
  }

  /* Whether opening "db" with opt is refused. */
  bool open_fails(const sm1::options_t *opt) const {
    return ::opendb(m_home.env(), "db", opt) == nullptr;
  }

private:
  home_t m_home;
};

/* Set key in store 1 to val; the result of sm1::update. */
int set(sm1::session_t *s, const std::string &key, const std::string &val) {
  return sm1::update(s, g_store, key, val);
}

/* Delete key from store 1; the result of sm1::erase. */
int del(sm1::session_t *s, const std::string &key) {
  return sm1::erase(s, g_store, key);
}

/* Drain a cursor into its keys and close it. */
std::vector<std::string> keys_of(sm1::scan_t *c) {
  REQUIRE_MESSAGE(c, sm1::errmsg());
  std::vector<std::string> out;
  std::string_view k, d;
  while (sm1::scannxt(c, k, d) == 0) {
    out.emplace_back(k);
  }
  sm1::scanend(c);
  return out;
}

} // namespace

TEST_CASE("rollback restores the values as of the savepoint, at every level") {
  // Verify rollback undoes inserts, updates, and deletes after the mark.
  // 1. Before the mark: update "a" to "1", insert "b".
  // 2. Set the savepoint.
  // 3. After it: update "a" twice, delete "b", insert "c",
  //    re-insert "b".
  // 4. Roll back: "a" is "1", "b" is "1", "c" is absent.
  // 5. Roll back again to the same mark; still the same.
  // 6. Write after the rollback and commit; a new transaction
  //    sees the post-rollback writes and nothing undone.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(level);
    sp_fixture_t f;
    sm1::session_t *s = f.open();
    REQUIRE(sm1::begin(s, level) == 0);
    REQUIRE(set(s, "a", "1") == 0);
    put(s, g_store, "b", "1");
    REQUIRE(sm1::savepoint(s) == 1);

    REQUIRE(set(s, "a", "2") == 0);
    REQUIRE(set(s, "a", "3") == 0);
    REQUIRE(del(s, "b") == 0);
    put(s, g_store, "c", "2");
    put(s, g_store, "b", "2");
    CHECK(get(s, g_store, "a") == "3");

    for (int round = 0; round < 2; round++) {
      CAPTURE(round);
      REQUIRE(sm1::rollback(s, 1) == 0);
      CHECK(get(s, g_store, "a") == "1");
      CHECK(get(s, g_store, "b") == "1");
      CHECK(get(s, g_store, "c") == "<none>");
      CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
            std::vector<std::string>{"a", "b"});
    }

    REQUIRE(set(s, "a", "4") == 0);
    put(s, g_store, "d", "4");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(get(s, g_store, "a") == "4");
    CHECK(get(s, g_store, "b") == "1");
    CHECK(get(s, g_store, "c") == "<none>");
    CHECK(get(s, g_store, "d") == "4");
    REQUIRE(sm1::commit(s) == 0);
    sm1::closedb(s);
  }
}

TEST_CASE("rollback lets go of the session's read pin") {
  // Verify a rollback to a savepoint lets go of the row the session's own
  // read pin holds, as an abort or a commit does, so no iterator stays on
  // a store the rollback drops.
  // 1. Past a savepoint, make a store, write a key and read it: the read
  //    pin holds an iterator on that store.
  // 2. Roll back to the savepoint: the store goes, and the pin holds no
  //    iterator.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  uint64_t made = mkstore(s);
  put(s, made, "k", "v");
  CHECK(get(s, made, "k") == "v");
  REQUIRE(s->readpin.it != nullptr);

  // 2.
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(s->readpin.it == nullptr);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("rollback needs a savepoint, and a savepoint lives only in its "
          "transaction") {
  // Verify SM_ERR_NOSAVEPOINT and the savepoint's lifetime.
  // 1. Rollback without a savepoint fails with SM_ERR_NOSAVEPOINT.
  // 2. Savepoint, commit; in the next transaction rollback
  //    fails again: commit discarded the savepoint.
  // 3. A depth outside the stack fails the same way.
  // 4. Savepoint right after begin rolls back to the start.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  CHECK(sm1::savepoint(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);

  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  CHECK(sm1::rollback(s, 2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);
  CHECK(sm1::rollback(s, 0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);
  CHECK(sm1::forget(s, 2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(sm1::abort(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  put(s, g_store, "b", "1");
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  CHECK(get(s, g_store, "b") == "<none>");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("savepoints nest, and rolling back to one drops those inside it") {
  // Verify the stack: depths, partial undo, and reuse of a mark.
  // 1. Push three savepoints, writing a distinct value before
  //    each; the depths returned are 1, 2, 3.
  // 2. Roll back to depth 2: writes after it are gone, writes
  //    before it stay, and depth 3 is gone with them.
  // 3. Depth 2 survives, so a new write and a second rollback
  //    to it work; a fresh push lands at depth 3 again.
  // 4. Rolling back to depth 1 undoes the lot.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(set(s, "a", "2") == 0);
  REQUIRE(sm1::savepoint(s) == 2);
  REQUIRE(set(s, "a", "3") == 0);
  REQUIRE(sm1::savepoint(s) == 3);
  REQUIRE(set(s, "a", "4") == 0);
  put(s, g_store, "b", "4");

  REQUIRE(sm1::rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  CHECK(get(s, g_store, "b") == "<none>");
  CHECK(sm1::rollback(s, 3) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);

  REQUIRE(set(s, "a", "5") == 0);
  REQUIRE(sm1::rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  REQUIRE(sm1::savepoint(s) == 3);

  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("sm1_forget drops a mark and those inside it, keeping the writes") {
  // Verify RELEASE SAVEPOINT semantics and the depth reuse it causes.
  // 1. Push two savepoints with a write after each.
  // 2. Forget depth 2: both writes stay, and depth 2 can no
  //    longer be rolled back to.
  // 3. A new savepoint takes depth 2 again, so a stale depth
  //    now names a different mark -- the documented hazard.
  // 4. Rolling back to depth 1 still undoes everything after it.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::savepoint(s) == 2);
  REQUIRE(set(s, "a", "2") == 0);

  REQUIRE(sm1::forget(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");
  CHECK(sm1::rollback(s, 2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOSAVEPOINT);

  REQUIRE(sm1::savepoint(s) == 2);
  REQUIRE(set(s, "a", "3") == 0);
  REQUIRE(sm1::rollback(s, 2) == 0);
  CHECK(get(s, g_store, "a") == "2");

  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("the savepoint stack is bounded, and forgetting makes room") {
  // Verify sm1::max_savepoints.
  // 1. Push sm1::max_savepoints savepoints; each returns its depth.
  // 2. The next push fails with SM_ERR_INVALID.
  // 3. Forgetting the innermost mark makes room for one more.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int depth = 1; depth <= sm1::max_savepoints; depth++) {
    REQUIRE(sm1::savepoint(s) == depth);
  }
  CHECK(sm1::savepoint(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  REQUIRE(sm1::forget(s, sm1::max_savepoints) == 0);
  CHECK(sm1::savepoint(s) == sm1::max_savepoints);
  REQUIRE(sm1::abort(s) == 0);
  sm1::closedb(s);
}

TEST_CASE(
    "cursors before the mark survive rollback; cursors after it are closed") {
  // Verify §6.5's cursor rule and the seq rewind.
  // 1. Insert "b"; open cursor C1; set the savepoint.
  // 2. Insert "c"; open cursor C2.
  // 3. Roll back: C2 is closed, failing each read and seek with
  //    SM_ERR_SCANCLOSED until it is ended; C1 stays open.
  // 4. Insert "d" (it reuses the undone seq number).
  // 5. C1 returns exactly "a" and "b": no row written after
  //    the mark, and nothing written after C1 opened.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, g_store, "b", "1");
  sm1::scan_t *c1 = scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c1);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "c", "2");
  sm1::scan_t *c2 = scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c2);
  CHECK(s->scans.size() == 2);

  REQUIRE(sm1::rollback(s, 1) == 0);
  std::string_view k, d;
  CHECK(sm1::scannxt(c2, k, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SCANCLOSED);
  CHECK(sm1::scannxt(c2, k, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SCANCLOSED);
  CHECK(sm1::scan_seek(c2, "z", 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SCANCLOSED);
  sm1::scanend(c2);
  CHECK(s->scans.size() == 1);
  CHECK(s->scans.count(c1) == 1);
  put(s, g_store, "d", "3");
  CHECK(keys_of(c1) == std::vector<std::string>{"a", "b"});
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a", "b", "d"});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("a cursor a rollback closed is freed by the transaction's end") {
  // Verify a closed cursor its caller never ends is no leak.
  // 1. Set a savepoint, open a cursor, and roll back.
  // 2. The cursor is still the session's, until the commit.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  REQUIRE(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm1::rollback(s, 1) == 0);

  CHECK(s->scans.size() == 1);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(s->scans.empty());
  sm1::closedb(s);
}

TEST_CASE("rollback undoes sm1_mkstore and sm1_rmstore made after the mark") {
  // Verify DDL follows the savepoint (§6.5, §11).
  // 1. Create store P before the mark; set the savepoint.
  // 2. Create store Q and write to it; drop the fixture's.
  // 3. Roll back: P exists, Q does not, and the fixture's
  //    store is back with its rows.
  // 4. Commit; a new transaction sees the same. A later
  //    sm1::mkstore hands out a storeid of its own.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t p = mkstore(s);
  REQUIRE(sm1::savepoint(s) == 1);
  uint64_t q = mkstore(s);
  put(s, q, "k", "v");
  REQUIRE(sm1::rmstore(s, g_store) == 0);
  REQUIRE(sm1::rollback(s, 1) == 0);

  for (int pass = 0; pass < 2; pass++) {
    CAPTURE(pass);
    std::string_view d;
    CHECK(sm1::read(s, p, "k", d) == 1);
    CHECK(sm1::read(s, q, "k", d) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
    CHECK(get(s, g_store, "a") == "0");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  }
  CHECK(mkstore(s) > q); // a fresh storeid, never q again
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("a transaction with no savepoint keeps no undo list") {
  // Verify a write is listed for rollback only when one could need it.
  // 1. Write three rows with no savepoint set.
  // 2. The undo list is empty.
  // 3. Commit; the rows are there.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  CHECK(s->undo.empty());

  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, g_store, "a") == "1");
  CHECK(get(s, g_store, "c") == "1");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("the undo list lives only while a savepoint does") {
  // Verify the list starts with the first mark and goes with the last.
  // 1. Write before any savepoint: nothing is listed.
  // 2. Set two savepoints with a write after each: both are listed.
  // 3. Forget the inner one: its write is still listed, for the outer.
  // 4. Forget the outer one: the list is dropped.
  // 5. A later write lists nothing, and every write commits.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, g_store, "b", "1");
  CHECK(s->undo.empty());

  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "c", "1");
  REQUIRE(sm1::savepoint(s) == 2);
  put(s, g_store, "d", "1");
  CHECK(s->undo.size() == 2);

  REQUIRE(sm1::forget(s, 2) == 0);
  CHECK(s->undo.size() == 2);

  REQUIRE(sm1::forget(s, 1) == 0);
  CHECK(s->undo.empty());

  put(s, g_store, "e", "1");
  CHECK(s->undo.empty());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a", "b", "c", "d", "e"});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("rollback rewinds seq to the mark with nothing listed before it") {
  // Verify the seq rewind no longer reads the write before the mark.
  // 1. Write two rows with no savepoint, so neither is listed.
  // 2. Set a savepoint, write a third row, and roll back.
  // 3. seq is back at the second write, whose rows are still seen.
  // 4. Write again and roll back again, to the same mark.
  // 5. Write once more and commit: only the surviving rows are there.
  sp_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  const uint32_t before = s->seq_hwm;

  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "d", "1");
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(s->seq_hwm == before);
  CHECK(s->seq == before);
  CHECK(s->undo.empty());
  CHECK(get(s, g_store, "c") == "1");
  CHECK(get(s, g_store, "d") == "<none>");

  put(s, g_store, "d", "2");
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(s->seq_hwm == before);

  put(s, g_store, "e", "1");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a", "b", "c", "e"});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

namespace {

/* An undo entry's cost for a one-byte key, as sm1 counts it. */
const size_t g_undo_entry = sizeof(sm1::session_t::undo_t) + 1;

/* Options whose undo list holds n one-byte-key writes and no more. */
sm1::options_t undo_cap(size_t n) {
  sm1::options_t opt;
  opt.savepoint_undo_bytes = static_cast<long long>(n * g_undo_entry);
  return opt;
}

} // namespace

TEST_CASE("past savepoint_undo_bytes the undo list is dropped, and the "
          "savepoints with it") {
  // Verify the cap bounds a transaction's undo memory without
  // stopping its writes.
  // 1. Open with room for three writes; set a savepoint.
  // 2. Three writes are listed and counted.
  // 3. The fourth passes the cap: the list is freed and the mark
  //    is lost.
  // 4. Later writes list nothing.
  // 5. Rolling back to the mark fails with SM_ERR_UNDOLIMIT, and
  //    dooms the transaction: its commit fails with SM_ERR_DOOMED.
  // 6. It aborts, and none of its writes is there.
  sp_fixture_t f;
  sm1::options_t opt = undo_cap(3);
  sm1::session_t *s = f.open(&opt);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  put(s, g_store, "d", "1");
  CHECK(s->undo.size() == 3);
  CHECK(s->undo_bytes == 3 * g_undo_entry);
  CHECK(s->lost_marks == 0);

  put(s, g_store, "e", "1");
  CHECK(s->undo.empty());
  CHECK(s->undo_bytes == 0);
  CHECK(s->lost_marks == 1);

  put(s, g_store, "f", "1");
  CHECK(s->undo.empty());

  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UNDOLIMIT);
  CHECK(sm1::commit(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DOOMED);

  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a"});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("a savepoint set after the undo list was dropped works") {
  // Verify only the marks set before the loss are lost.
  // 1. Lose the mark at depth 1 by passing the cap.
  // 2. Set a savepoint at depth 2, write, and roll back to it:
  //    that write is gone and the earlier ones stay.
  // 3. Forget depth 1: no mark is lost any more, and a new
  //    savepoint at depth 1 rolls back as usual.
  // 4. Abort: nothing of the transaction is left.
  sp_fixture_t f;
  sm1::options_t opt = undo_cap(2);
  sm1::session_t *s = f.open(&opt);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  put(s, g_store, "d", "1");
  REQUIRE(s->lost_marks == 1);

  REQUIRE(sm1::savepoint(s) == 2);
  put(s, g_store, "e", "1");
  CHECK(s->undo.size() == 1);
  REQUIRE(sm1::rollback(s, 2) == 0);
  CHECK(get(s, g_store, "e") == "<none>");
  CHECK(get(s, g_store, "d") == "1");
  CHECK(s->undo_bytes == 0);

  REQUIRE(sm1::forget(s, 1) == 0);
  CHECK(s->lost_marks == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "g", "1");
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(get(s, g_store, "g") == "<none>");
  CHECK(get(s, g_store, "d") == "1");

  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a"});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("passing the cap loses every savepoint set, and a rollback gives "
          "its bytes back") {
  // Verify the count follows the list, and the loss covers the stack.
  // 1. With room for three writes, set a savepoint, write twice,
  //    and roll back: the count is back at zero.
  // 2. Three more writes fit, which they would not had the count
  //    kept the two undone.
  // 3. Set a second savepoint and write once more: both marks
  //    are lost. Rolling back to the inner fails with
  //    SM_ERR_UNDOLIMIT and dooms the transaction, so rolling
  //    back to the outer fails with SM_ERR_DOOMED.
  // 4. The next transaction starts with nothing lost.
  sp_fixture_t f;
  sm1::options_t opt = undo_cap(3);
  sm1::session_t *s = f.open(&opt);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(s->undo_bytes == 0);

  put(s, g_store, "b", "2");
  put(s, g_store, "c", "2");
  put(s, g_store, "d", "2");
  CHECK(s->undo.size() == 3);
  CHECK(s->lost_marks == 0);

  REQUIRE(sm1::savepoint(s) == 2);
  put(s, g_store, "e", "2");
  CHECK(s->lost_marks == 2);
  CHECK(sm1::rollback(s, 2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UNDOLIMIT);
  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DOOMED);
  REQUIRE(sm1::abort(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(s->lost_marks == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "b", "3");
  REQUIRE(sm1::rollback(s, 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("a rollback that fails dooms the transaction until it aborts") {
  // Verify a failed rollback leaves nothing to commit: every call on
  // the transaction but an abort fails, and the abort undoes it all.
  // 1. With room for one write, make a sequence, open a cursor,
  //    set a savepoint, and write twice: the mark is lost.
  // 2. Rolling back to it fails with SM_ERR_UNDOLIMIT.
  // 3. A read, a write, a new cursor, the open cursor, a sequence
  //    draw, a savepoint, a forget, a statement, a rollback and
  //    the commit each fail with SM_ERR_DOOMED, naming the cap.
  // 4. The abort succeeds, and the next transaction reads only
  //    what was there before, and draws from its sequence.
  sp_fixture_t f;
  sm1::options_t opt = undo_cap(1);
  sm1::session_t *s = f.open(&opt);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::mkseq(s, "q", 1, 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm1::scan_t *c = scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, g_store, "b", "1");
  put(s, g_store, "c", "1");
  REQUIRE(s->lost_marks == 1);

  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UNDOLIMIT);

  auto doomed = [] {
    return sm1::errnum() == sm1::SM_ERR_DOOMED &&
           std::string(sm1::errmsg()).find("savepoint_undo_bytes") !=
               std::string::npos;
  };
  std::string_view k, d;
  uint64_t v = 0;
  CHECK(sm1::read(s, g_store, "a", d) == -1);
  CHECK(doomed());
  CHECK(sm1::insert(s, g_store, "d", "1") == -1);
  CHECK(doomed());
  CHECK(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0) == nullptr);
  CHECK(doomed());
  CHECK(sm1::scannxt(c, k, d) == -1);
  CHECK(doomed());
  CHECK(sm1::nextseq(s, "q", v) == -1);
  CHECK(doomed());
  CHECK(sm1::savepoint(s) == -1);
  CHECK(doomed());
  CHECK(sm1::forget(s, 1) == -1);
  CHECK(doomed());
  CHECK(sm1::stmt_begin(s) == -1);
  CHECK(doomed());
  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(doomed());
  CHECK(sm1::commit(s) == -1);
  CHECK(doomed());

  sm1::scanend(c);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(keys_of(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0)) ==
        std::vector<std::string>{"a"});
  REQUIRE(sm1::nextseq(s, "q", v) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
}

TEST_CASE("savepoint_undo_bytes must be at least 1") {
  // Verify the option is checked when the database opens.
  // 1. Open with savepoint_undo_bytes = 0.
  // 2. The open fails with SM_ERR_INVALID.
  sp_fixture_t f;
  sm1::options_t opt;
  opt.savepoint_undo_bytes = 0;
  CHECK(f.open_fails(&opt));
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}
