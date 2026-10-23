/*
 * test_access.cc -- stores and rows: sm1::mkstore, sm1::rmstore,
 * sm1::lsstore, sm1::insert, sm1::update, sm1::erase, sm1::read, and sm1::scan.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <stdio.h>

#include <string>
#include <utility>
#include <vector>

namespace {

/* A database "db" with one open session, for one test. */
class db_fixture_t {
public:
  /* Create "db" and open a session on it. */
  db_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_session = opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_session);
  }

  /* The session. */
  sm1::session_t *session() const { return m_session; }

  /* Another session on the same database. */
  sm1::session_t *open_other() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm1::closedb(m_session) == 0);
    m_home.reattach();
    m_session = opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_session);
  }

private:
  home_t m_home;
  sm1::session_t *m_session = nullptr;
};

using rows_t = std::vector<std::pair<std::string, std::string>>;

/* Drain a cursor into (key, value) pairs and close it. */
rows_t drain(sm1::scan_t *c) {
  REQUIRE_MESSAGE(c, sm1::errmsg());
  rows_t rows;
  std::string_view k, d;
  int r;
  while ((r = sm1::scannxt(c, k, d)) == 0) {
    rows.emplace_back(std::string(k), std::string(d));
  }
  CHECK(r == 1);
  CHECK(sm1::scannxt(c, k, d) == 1);
  sm1::scanend(c);
  return rows;
}

/* Every row of store, in order. */
rows_t scan_all(sm1::session_t *s, uint64_t store) {
  return drain(scan(s, store, nullptr, 0, 0, nullptr, 0, 0));
}

/* Create a store in its own committed transaction. */
uint64_t mkstore_committed(sm1::session_t *s) {
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  return store;
}

} // namespace

TEST_CASE("sm1_mkstore and sm1_rmstore are transactional") {
  // Verify DDL follows commit and abort.
  // 1. Create a store and abort; its storeid names nothing
  //    afterwards.
  // 2. Create one and commit; it holds its rows.
  // 3. Drop it and abort; it still exists with its rows.
  // 4. Drop it and commit; it is gone, and dropping it again
  //    fails with SM_ERR_NOTFOUND.
  // 5. A store created now gets a storeid of its own, never
  //    one handed out before, and starts empty.
  db_fixture_t f;
  sm1::session_t *s = f.session();

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t gone = mkstore(s);
  put(s, gone, "k", "v");
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view d;
  CHECK(sm1::read(s, gone, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  uint64_t store = mkstore(s);
  CHECK(store != gone);
  put(s, store, "k", "v");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmstore(s, store) == 0);
  REQUIRE(sm1::abort(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "k") == "v");
  REQUIRE(sm1::rmstore(s, store) == 0);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::rmstore(s, store) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  uint64_t fresh = mkstore(s);
  CHECK(fresh > store);
  CHECK(get(s, fresh, "k") == "<none>");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a transaction reads its own inserts, updates, and deletes") {
  // Verify own-write visibility and the existence rules, at every level.
  // 1. Insert a key, read it back; inserting it again fails
  //    with SM_ERR_DUPKEY.
  // 2. Update it twice; the read returns the latest value.
  // 3. Delete it; the read returns 1, and update and delete
  //    fail with SM_ERR_NOTFOUND.
  // 4. Re-insert the deleted key; the read returns the new value.
  // 5. A zero-length key and a zero-length value are legal, and
  //    an empty value is distinct from a missing key.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(level);
    db_fixture_t f;
    sm1::session_t *s = f.session();
    uint64_t store = mkstore_committed(s);
    REQUIRE(sm1::begin(s, level) == 0);

    put(s, store, "k", "v1");
    CHECK(get(s, store, "k") == "v1");
    CHECK(sm1::insert(s, store, "k", "x") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);

    REQUIRE(sm1::update(s, store, "k", "v2") == 0);
    REQUIRE(sm1::update(s, store, "k", "v3") == 0);
    CHECK(get(s, store, "k") == "v3");

    REQUIRE(sm1::erase(s, store, "k") == 0);
    CHECK(get(s, store, "k") == "<none>");
    CHECK(sm1::update(s, store, "k", "x") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
    CHECK(sm1::erase(s, store, "k") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

    put(s, store, "k", "v4");
    CHECK(get(s, store, "k") == "v4");

    put(s, store, "", "empty-key");
    put(s, store, "e", "");
    CHECK(get(s, store, "") == "empty-key");
    CHECK(get(s, store, "e") == "");
    CHECK(get(s, store, "missing") == "<none>");
    REQUIRE(sm1::commit(s) == 0);
  }
}

TEST_CASE("an insert of a key never written is a write like any other") {
  // Verify sm1::insert_new, which reads nothing, writes as sm1::insert.
  // 1. A key inserted with insert_new is read back, and inserting
  //    it again with sm1::insert fails with SM_ERR_DUPKEY.
  // 2. A key inserted with insert_new after a savepoint is gone
  //    once the transaction rolls back to it.
  // 3. Another session does not see the key until the commit,
  //    then does.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(level);
    db_fixture_t f;
    sm1::session_t *s = f.session();
    uint64_t store = mkstore_committed(s);
    sm1::session_t *b = f.open_other();
    REQUIRE(sm1::begin(s, level) == 0);

    REQUIRE(sm1::insert_new(s, store, "k1", "v1") == 0);
    CHECK(get(s, store, "k1") == "v1");
    CHECK(sm1::insert(s, store, "k1", "x") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);

    REQUIRE(sm1::savepoint(s) == 1);
    REQUIRE(sm1::insert_new(s, store, "k2", "v2") == 0);
    CHECK(get(s, store, "k2") == "v2");
    REQUIRE(sm1::rollback(s, 1) == 0);
    CHECK(get(s, store, "k2") == "<none>");

    REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
    CHECK(get(b, store, "k1") == "<none>");
    REQUIRE(sm1::commit(s) == 0);
    CHECK(get(b, store, "k1") == "v1");
    REQUIRE(sm1::commit(b) == 0);
    sm1::closedb(b);
  }
}

TEST_CASE("a move's tombstone is a delete to a reader, a move to a writer") {
  // Verify sm1::erase_moved: its version reads as a tombstone, and a
  // write that meets it fails with SM_ERR_MOVED, not NOTFOUND.
  // 1. k1 and k2 are committed; k1 is erased as moved, k2 erased.
  // 2. A read of either finds none.
  // 3. An update or erase of k1 fails with SM_ERR_MOVED, of k2 with
  //    SM_ERR_NOTFOUND.
  // 4. An insert of k1 goes in, as over any tombstone.
  // 5. Committed, another transaction reads k1's new value.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::insert(s, store, "k1", "v1") == 0);
  REQUIRE(sm1::insert(s, store, "k2", "v2") == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::rawpin_t *pin = sm1::rawpin_new();
  std::string_view old;
  REQUIRE(sm1::erase_moved(s, store, "k1", pin, old) == 0);
  CHECK(old == "v1");
  sm1::rawpin_free(pin);
  REQUIRE(sm1::erase(s, store, "k2") == 0);
  CHECK(get(s, store, "k1") == "<none>");
  CHECK(get(s, store, "k2") == "<none>");
  CHECK(sm1::update(s, store, "k1", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_MOVED);
  CHECK(sm1::erase(s, store, "k1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_MOVED);
  CHECK(sm1::update(s, store, "k2", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::insert(s, store, "k1", "back") == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(get(s, store, "k1") == "back");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("commit persists writes across reattach; abort discards them") {
  // Verify durability of commits and the absence of aborted writes.
  // 1. Commit a key "a"; write "b" and update "a", then abort.
  // 2. A new transaction sees "a" with its committed value and
  //    no "b".
  // 3. Detach and reattach; the same holds.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "a", "committed");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "b", "aborted");
  REQUIRE(sm1::update(s, store, "a", "aborted") == 0);
  REQUIRE(sm1::abort(s) == 0);

  for (int pass = 0; pass < 2; pass++) {
    CAPTURE(pass);
    if (pass == 1) {
      f.reopen();
      s = f.session();
    }
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(get(s, store, "a") == "committed");
    CHECK(get(s, store, "b") == "<none>");
    CHECK(scan_all(s, store) == rows_t{{"a", "committed"}});
    REQUIRE(sm1::commit(s) == 0);
  }
}

TEST_CASE("repeatable read keeps its snapshot; read committed does not") {
  // Verify the two levels' snapshots against another session's commit.
  // 1. Session A begins RR, session B begins RC; both read "k"
  //    as missing.
  // 2. Session C inserts "k" and commits.
  // 3. A still reads it as missing; B now reads the new value.
  db_fixture_t f;
  sm1::session_t *a = f.session();
  uint64_t store = mkstore_committed(a);
  sm1::session_t *b = f.open_other();
  sm1::session_t *c = f.open_other();

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  CHECK(get(a, store, "k") == "<none>");
  CHECK(get(b, store, "k") == "<none>");

  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  put(c, store, "k", "v");
  REQUIRE(sm1::commit(c) == 0);

  CHECK(get(a, store, "k") == "<none>");
  CHECK(scan_all(a, store).empty());
  CHECK(get(b, store, "k") == "v");
  CHECK(scan_all(b, store) == rows_t{{"k", "v"}});
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::commit(b) == 0);
  sm1::closedb(b);
  sm1::closedb(c);
}

TEST_CASE("sm1_stmt_begin gives read committed one snapshot per statement") {
  // Verify a statement spanning several calls sees one committed
  // state, the anomaly SM1_DESIGN.md §6.2 describes.
  // 1. Two rows each hold 50; an RC session begins a statement
  //    and reads the first.
  // 2. Another session moves 30 from the first row to the
  //    second and commits, mid-statement.
  // 3. The second read still sees 50, so the statement totals
  //    100, not 130.
  // 4. The next sm1::stmt_begin starts a new statement, which
  //    sees the transfer.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  sm1::session_t *other = f.open_other();

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "a", "50");
  put(s, store, "b", "50");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "a") == "50");

  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::update(other, store, "a", "20") == 0);
  REQUIRE(sm1::update(other, store, "b", "80") == 0);
  REQUIRE(sm1::commit(other) == 0);

  CHECK(get(s, store, "b") == "50");
  CHECK(scan_all(s, store) == rows_t{{"a", "50"}, {"b", "50"}});

  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "a") == "20");
  CHECK(get(s, store, "b") == "80");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(other);
}

TEST_CASE("the statement snapshot is taken by the first read, not the call") {
  // Verify a statement that waits before reading still sees what
  // committed while it waited (sm3/SM3_DESIGN.md §3's build protocol).
  // 1. An RC session begins a statement and does not read.
  // 2. Another session commits a row.
  // 3. The first read of the statement sees it.
  // 4. A write does not take the snapshot: after a new
  //    statement writes, a commit by another session is still
  //    visible to that statement's first read.
  // 5. A commit clears the pin: a fresh RC transaction that
  //    never calls sm1::stmt_begin still reads per call.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  sm1::session_t *other = f.open_other();

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);

  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  put(other, store, "k", "v");
  REQUIRE(sm1::commit(other) == 0);

  CHECK(get(s, store, "k") == "v");

  REQUIRE(sm1::stmt_begin(s) == 0);
  put(s, store, "mine", "1");
  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  put(other, store, "late", "v");
  REQUIRE(sm1::commit(other) == 0);
  CHECK(get(s, store, "late") == "v");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(get(s, store, "k") == "v");
  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::update(other, store, "k", "w") == 0);
  REQUIRE(sm1::commit(other) == 0);
  CHECK(get(s, store, "k") == "w");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(other);
}

TEST_CASE("sm1_stmt_begin needs a transaction and does nothing above RC") {
  // Verify the call's contract at the other levels and outside a
  // transaction.
  // 1. Without a transaction it fails with SM_ERR_NOTRANS.
  // 2. Under REPEATABLE READ it succeeds and changes nothing:
  //    the transaction still reads at its begin snapshot.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  sm1::session_t *other = f.open_other();

  CHECK(sm1::stmt_begin(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "k") == "<none>");

  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  put(other, store, "k", "v");
  REQUIRE(sm1::commit(other) == 0);

  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "k") == "<none>");
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(other);
}

TEST_CASE("a scan returns userkeys in memcmp order within its bounds") {
  // Verify key order and bound handling (SM1_DESIGN.md §12.5 key order).
  // 1. Insert keys that prefix one another, each updated so it
  //    has several versions.
  // 2. A full scan returns each key once, newest value, in
  //    memcmp order.
  // 3. Inclusive and exclusive bounds include or exclude the
  //    bound keys; an inverted range is empty.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  std::vector<std::string> keys = {"", "a", std::string("a\0", 2), "a\xff",
                                   "ab"};
  for (int round = 0; round < 3; round++) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    for (const std::string &k : keys) {
      std::string v = k + "#" + std::to_string(round);
      if (round == 0) {
        put(s, store, k, v);
      } else {
        REQUIRE(sm1::update(s, store, k, v) == 0);
      }
    }
    REQUIRE(sm1::commit(s) == 0);
  }

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  rows_t all;
  // memcmp order: "ab" (61 62) sorts before "a\xff" (61 ff).
  for (const std::string &k :
       {std::string(""), std::string("a"), std::string("a\0", 2),
        std::string("ab"), std::string("a\xff")}) {
    all.emplace_back(k, k + "#2");
    CHECK(get(s, store, k) == k + "#2");
  }
  CHECK(scan_all(s, store) == all);

  std::string a0("a\0", 2);
  CHECK(drain(scan(s, store, "a", 1, 1, "a\xff", 2, 1)) ==
        rows_t(all.begin() + 1, all.begin() + 5));
  CHECK(drain(scan(s, store, "a", 1, 0, "a\xff", 2, 0)) ==
        rows_t(all.begin() + 2, all.begin() + 4));
  CHECK(drain(scan(s, store, "", 0, 0, a0.data(), 2, 1)) ==
        rows_t(all.begin() + 1, all.begin() + 3));
  CHECK(drain(scan(s, store, "b", 1, 1, "a", 1, 1)).empty());
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a cursor does not see writes made after it opened") {
  // Verify Halloween protection (SM1_DESIGN.md §10.2).
  // 1. Insert "a" and "b", then open a cursor.
  // 2. Insert "c" and update "a" in the same transaction.
  // 3. The cursor returns exactly the rows as of its opening.
  // 4. A cursor opened afterwards sees the new writes.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "a", "1");
  put(s, store, "b", "2");
  sm1::scan_t *c = scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  put(s, store, "c", "3");
  REQUIRE(sm1::update(s, store, "a", "9") == 0);
  CHECK(drain(c) == rows_t{{"a", "1"}, {"b", "2"}});
  CHECK(scan_all(s, store) == rows_t{{"a", "9"}, {"b", "2"}, {"c", "3"}});
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("commit closes open cursors") {
  // Verify commit and abort release a session's cursors.
  // 1. Open two cursors and commit without closing them.
  // 2. Open one and abort. Nothing leaks or crashes (ASan/valgrind
  //    would report it); the session begins again normally.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(scan(s, store, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(scan(s, store, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(scan(s, store, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a cursor lends its row until its next call, and passes over the "
          "key's older versions") {
  // Verify the row scannxt hands out where the iterator has it.
  // 1. Commit "a", "b" and "c"; update "b" twice and delete "c", each
  //    in a transaction of its own; then, in the reading one, update
  //    "a" without committing.
  // 2. A cursor gives "a" at its own write. While it is on "a", the
  //    session reads a key, inserts "d" and drains a second cursor:
  //    "a"'s row and key still read as they did.
  // 3. The cursor then gives "b" at its newest version, passes over
  //    the deleted "c" and the "d" written after it opened, and ends.
  // 4. A third cursor gives "a"; a seek to "a" gives it again, and a
  //    seek past "b" gives "d", which it opened after.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  put(s, store, "a", "1");
  put(s, store, "b", "1");
  put(s, store, "c", "1");
  REQUIRE(0 == sm1::commit(s));
  for (const char *v : {"2", "3"}) {
    REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
    REQUIRE(0 == sm1::update(s, store, "b", v));
    REQUIRE(0 == sm1::commit(s));
  }
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  REQUIRE(0 == sm1::erase(s, store, "c"));
  REQUIRE(0 == sm1::commit(s));
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::repeatable_read));
  REQUIRE(0 == sm1::update(s, store, "a", "own"));

  sm1::scan_t *c = scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  std::string_view k, d;
  REQUIRE(0 == sm1::scannxt(c, k, d));
  CHECK("a" == k);
  CHECK("own" == d);
  CHECK("3" == get(s, store, "b"));
  put(s, store, "d", "4");
  CHECK(rows_t{{"a", "own"}, {"b", "3"}, {"d", "4"}} == scan_all(s, store));
  CHECK("a" == k);
  CHECK("own" == d);

  REQUIRE(0 == sm1::scannxt(c, k, d));
  CHECK("b" == k);
  CHECK("3" == d);
  CHECK(1 == sm1::scannxt(c, k, d));
  sm1::scanend(c);

  c = scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  REQUIRE(0 == sm1::scannxt(c, k, d));
  CHECK("a" == k);
  REQUIRE(0 == sm1::scan_seek(c, "a", 1));
  REQUIRE(0 == sm1::scannxt(c, k, d));
  CHECK("a" == k);
  CHECK("own" == d);
  REQUIRE(0 == sm1::scan_seek(c, "b", 0));
  CHECK(0 == sm1::scannxt(c, k, d));
  CHECK("d" == k);
  sm1::scanend(c);
  REQUIRE(0 == sm1::commit(s));
}

TEST_CASE("a scan from an absent key crosses many userkeys in SSTs") {
  // Regression for inheriting RocksDB's prefix seek mode (§10.2, §12.5).
  // 1. Insert 5000 keys "k00000".."k04999" (every other number
  //    is skipped, so start keys can be absent) and commit.
  // 2. Reattach, which flushes the memtable into SSTs.
  // 3. Scan from an absent key "k00001" to the end: every key
  //    above it is returned, not zero or one.
  // 4. Scan a closed range spanning many keys: all are returned.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  char key[16];
  for (int i = 0; i < 10000; i += 2) {
    snprintf(key, sizeof(key), "k%05d", i);
    put(s, store, key, "v");
  }
  REQUIRE(sm1::commit(s) == 0);
  f.reopen();
  s = f.session();

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  rows_t rows = drain(scan(s, store, "k00001", 6, 1, nullptr, 0, 0));
  CHECK(rows.size() == 4999);
  CHECK(rows.front().first == "k00002");
  CHECK(rows.back().first == "k09998");
  rows = drain(scan(s, store, "k00101", 6, 1, "k00301", 6, 0));
  CHECK(rows.size() == 100);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("sm1_scan_seek repositions a cursor without reopening it") {
  // Verify the reposition keeps bounds, view and order (§10.2).
  // 1. Scan k1..k9 and read one row.
  // 2. Seek to k5 inclusive: the next rows start at k5.
  // 3. Seek to k7 exclusive: the next row is k8.
  // 4. Seek past the end key: the cursor reports end, and
  //    stays at end afterwards.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, store, k, std::to_string(i));
  }
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm1::scan_t *c = scan(s, store, "k1", 2, 1, "k9", 2, 1);
  REQUIRE(c);
  std::string_view k, d;
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k1");

  REQUIRE(sm1::scan_seek(c, "k5", 1) == 0);
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k5");
  CHECK(std::string(d) == "5");

  REQUIRE(sm1::scan_seek(c, "k7", 0) == 0);
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k8");

  REQUIRE(sm1::scan_seek(c, "k99", 1) == 0);
  CHECK(sm1::scannxt(c, k, d) == 1);
  REQUIRE(sm1::scan_seek(c, "k99", 1) == 0);
  CHECK(sm1::scannxt(c, k, d) == 1);
  sm1::scanend(c);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("sm1_scan_seek refuses to move backwards or out of range") {
  // Verify the forward-only rule and the bounds check (§10.2).
  // 1. Open a cursor over k3..k7 and read k3.
  // 2. A seek to k2, below the start key, fails.
  // 3. A seek back to k3 after reading k5 fails.
  // 4. Re-reading the key just read is allowed, since the
  //    cursor has not passed it.
  // 5. A NULL cursor and an oversized key are rejected.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, store, k, std::to_string(i));
  }
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm1::scan_t *c = scan(s, store, "k3", 2, 1, "k7", 2, 1);
  REQUIRE(c);
  std::string_view k, d;
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k3");

  CHECK(sm1::scan_seek(c, "k2", 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  REQUIRE(sm1::scan_seek(c, "k5", 1) == 0);
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k5");
  CHECK(sm1::scan_seek(c, "k3", 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  REQUIRE(sm1::scan_seek(c, "k5", 1) == 0);
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "k5");

  CHECK(sm1::scan_seek(nullptr, "k5", 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string huge(sm1::max_keylen + 1, 'z');
  CHECK(sm1::scan_seek(c, huge, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm1::scanend(c);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("keys skipped by sm1_scan_seek still count as read under SSI") {
  // Verify the predicate covers the gap a seek jumps over
  // (§7.1): an over-approximation, never a missed conflict.
  // 1. T1, SERIALIZABLE, opens a cursor over k1..k9 and reads
  //    k1. T2 reads "own", which only T1 writes, and writes
  //    k5; T1 writes "own". That is a write skew, but it
  //    closes into a cycle only if k5 is inside T1's predicate.
  // 2. Control: T1 stops after k1, so its predicate ends at k1,
  //    k5 is outside it, and both transactions commit.
  // 3. With a seek from k1 to k8, k5 falls in the skipped gap
  //    and is covered all the same, so the cycle forms and a
  //    commit fails with SM_ERR_SERIALIZATION.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  sm1::session_t *t2 = f.open_other();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, store, k, std::to_string(i));
  }
  put(s, store, "own", "0");
  REQUIRE(sm1::commit(s) == 0);

  // Run the write skew, seeking over k5 or stopping short of
  // it; true if either transaction was a serialization victim.
  auto skew = [&](bool seek_past_k5) {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::serializable) == 0);
    sm1::scan_t *c = scan(s, store, "k1", 2, 1, "k9", 2, 1);
    REQUIRE(c);
    std::string_view k, d;
    REQUIRE(sm1::scannxt(c, k, d) == 0);
    CHECK(std::string(k) == "k1");
    if (seek_past_k5) {
      REQUIRE(sm1::scan_seek(c, "k8", 1) == 0);
      REQUIRE(sm1::scannxt(c, k, d) == 0);
      CHECK(std::string(k) == "k8");
    }
    sm1::scanend(c);

    REQUIRE(sm1::begin(t2, sm1::xctlevel_t::serializable) == 0);
    CHECK(get(t2, store, "own") != "<none>");
    REQUIRE(sm1::update(s, store, "own", "t1") == 0);
    REQUIRE(sm1::update(t2, store, "k5", "t2") == 0);

    int rc2 = sm1::commit(t2);
    int err2 = sm1::errnum();
    int rc1 = sm1::commit(s);
    int err1 = sm1::errnum();
    // Every failure here is a serialization failure.
    if (rc2 == -1) {
      CHECK(err2 == sm1::SM_ERR_SERIALIZATION);
    }
    if (rc1 == -1) {
      CHECK(err1 == sm1::SM_ERR_SERIALIZATION);
    }
    return rc1 == -1 || rc2 == -1;
  };

  // A cursor that stopped at k1 never covered k5.
  CHECK_FALSE(skew(false));
  // Seeking over k5 still counts it as read.
  CHECK(skew(true));
  sm1::closedb(t2);
}

TEST_CASE("sm1_update and sm1_delete hand back the row they replaced") {
  // Verify the old-image return (SM1_DESIGN.md §5.3).
  // 1. Update a row: the old value is the one committed before.
  // 2. Update it again: the old value is this transaction's own
  //    previous write, not the committed one.
  // 3. Delete it: the old value is the last one written.
  // 4. An empty old value is reported with length 0.
  // 5. A failed write returns no old row, and passing one
  //    out-parameter without the other is SM_ERR_INVALID.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "k", "v0");
  put(s, store, "empty", "");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view old;
  REQUIRE(sm1::update(s, store, "k", "v1", &old) == 0);
  CHECK(std::string(old) == "v0");

  REQUIRE(sm1::update(s, store, "k", "v2", &old) == 0);
  CHECK(std::string(old) == "v1");

  REQUIRE(sm1::erase(s, store, "k", &old) == 0);
  CHECK(std::string(old) == "v2");

  REQUIRE(sm1::update(s, store, "empty", "now", &old) == 0);
  CHECK(old.size() == 0);

  // The key is gone, so this fails and reports no old row.
  old = "sentinel";
  CHECK(sm1::update(s, store, "k", "v3", &old) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(old.data() == nullptr);
  CHECK(old.size() == 0);

  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("the old row shares sm1_read's buffer and its lifetime") {
  // Verify the documented lifetime of the old image.
  // 1. Update a row, keeping the old value's pointer.
  // 2. A read on the same session overwrites it, so the caller
  //    must copy what it wants to keep.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "k", "old");
  put(s, store, "other", "other-value");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view old;
  REQUIRE(sm1::update(s, store, "k", "new", &old) == 0);
  std::string copied(old);
  CHECK(copied == "old");

  std::string_view d;
  REQUIRE(sm1::read(s, store, "other", d) == 0);
  CHECK(std::string(d) == "other-value");
  // The copy still holds what the update returned.
  CHECK(copied == "old");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("oversized keys and values are rejected") {
  // Verify the documented limits.
  // 1. A key of sm1::max_keylen bytes is accepted; one byte more
  //    fails with SM_ERR_INVALID, for insert and for read.
  // 2. An unknown store fails with SM_ERR_NOTFOUND.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string max(sm1::max_keylen, 'k');
  put(s, store, max, "v");
  CHECK(get(s, store, max) == "v");
  std::string over(sm1::max_keylen + 1, 'k');
  CHECK(sm1::insert(s, store, over, "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string_view d;
  CHECK(sm1::read(s, store, over, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::insert(s, 999, "k", "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a dropped store stays readable at an older snapshot") {
  // Verify a drop leaves older snapshots alone, and that its
  // storeid never comes back.
  // 1. Create a store with "k" -> "old" and commit.
  // 2. Reader R begins REPEATABLE READ and reads "old".
  // 3. S drops it and creates another in one transaction: the
  //    new store has a new storeid and starts empty. S
  //    inserts "k" -> "new" and commits.
  // 4. R still reads and scans the dropped store's rows.
  // 5. A new transaction finds the old storeid gone and reads
  //    "new" from the new store.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t old = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, old, "k", "old");
  REQUIRE(sm1::commit(s) == 0);

  sm1::session_t *r = f.open_other();
  REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(r, old, "k") == "old");

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmstore(s, old) == 0);
  uint64_t store = mkstore(s);
  CHECK(store > old);
  CHECK(get(s, store, "k") == "<none>");
  put(s, store, "k", "new");
  REQUIRE(sm1::commit(s) == 0);

  CHECK(get(r, old, "k") == "old");
  CHECK(scan_all(r, old) == rows_t{{"k", "old"}});
  REQUIRE(sm1::commit(r) == 0);

  REQUIRE(sm1::begin(r, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view d;
  CHECK(sm1::read(r, old, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(get(r, store, "k") == "new");
  CHECK(scan_all(r, store) == rows_t{{"k", "new"}});
  REQUIRE(sm1::commit(r) == 0);
  sm1::closedb(r);
}

TEST_CASE("a transaction's own drop hides the store it looked up") {
  // Verify the session's store cache never answers for a
  // transaction's own drop (§11).
  // 1. Create a store with "k" -> "v" and commit.
  // 2. A transaction reads "k", which caches the store as live.
  // 3. It drops the store: reading "k" now fails NOTFOUND.
  // 4. It aborts, and the next transaction reads "v" again.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "k", "v");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "k") == "v");
  REQUIRE(sm1::rmstore(s, store) == 0);
  std::string_view d;
  CHECK(sm1::read(s, store, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "k") == "v");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("read committed's next statement misses a store dropped since") {
  // Verify a store cache entry from one statement does not
  // outlive a drop committed before the next (§11).
  // 1. Create a store with "k" -> "v" and commit.
  // 2. A read committed statement reads "k", which caches the
  //    store as live at its snapshot.
  // 3. Another session drops the store and commits.
  // 4. The statement still reads "v"; the next one fails
  //    NOTFOUND.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, "k", "v");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "k") == "v");

  sm1::session_t *other = f.open_other();
  REQUIRE(sm1::begin(other, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmstore(other, store) == 0);
  REQUIRE(sm1::commit(other) == 0);

  CHECK(get(s, store, "k") == "v");
  REQUIRE(sm1::stmt_begin(s) == 0);
  std::string_view d;
  CHECK(sm1::read(s, store, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(other);
}

TEST_CASE("object ids start at 1000, below which none is handed out") {
  // Verify the reserved range of object ids (SM1_DESIGN.md §4.2).
  // 1. The first store a new database makes is 1000.
  // 2. A sequence made next takes an object id too, so the store
  //    after it skips one: 1002.
  // 3. After a reopen, the next store is above both, never below
  //    1000.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t first = mkstore_committed(s);
  CHECK(first == 1000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::mkseq(s, "q", 1, 1) == 0);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(mkstore_committed(s) == 1002);
  f.reopen();
  CHECK(mkstore_committed(f.session()) > 1002);
}

TEST_CASE("store 0 takes the level above's keys, under its prefix only") {
  // Verify callers' access to store 0, the catalog (SM1_DESIGN.md
  // §11), under sm1::upper_prefix.
  // 1. In a transaction, "U" keys in store 0 insert, read, update,
  //    lock, and scan over ["U", "V"), in key order.
  // 2. An abort undoes the writes.
  // 3. A key outside the prefix -- sm1's own "R" and "S", or empty
  //    -- is SM_ERR_INVALID for a read, a write and a lock; so is a
  //    scan that starts or ends outside it.
  // 4. "U" rows add no store to lsstore, and after a reopen they
  //    are still there and lsstore is unchanged.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::insert(s, 0, "Ub", "2") == 0);
  REQUIRE(sm1::insert(s, 0, "Ua", "1") == 0);
  REQUIRE(sm1::update(s, 0, "Ub", "two") == 0);
  std::string_view v;
  REQUIRE(sm1::read(s, 0, "Ua", v) == 0);
  CHECK(v == "1");
  CHECK(sm1::lock(s, sm1::LOCKTYPE_ROW, 0, "Ua", sm1::SM_LOCKMODE_SH) == 0);
  sm1::scan_t *c = nullptr;
  REQUIRE(sm1::scan(s, 0, "U", 1, 1, "V", 1, 0, c) == 0);
  CHECK(drain(c) == rows_t{{"Ua", "1"}, {"Ub", "two"}});
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::insert(s, 0, "Uc", "3") == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::read(s, 0, "Uc", v) == 1);

  for (std::string_view key :
       {std::string_view("R"), std::string_view("Sq"), std::string_view()}) {
    CAPTURE(key);
    CHECK(sm1::read(s, 0, key, v) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(sm1::insert(s, 0, key, "x") == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(sm1::lock(s, sm1::LOCKTYPE_ROW, 0, key, sm1::SM_LOCKMODE_SH) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  CHECK(sm1::scan(s, 0, "R", 1, 1, "V", 1, 0, c) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::scan(s, 0, "U", 1, 1, "V", 1, 1, c) == -1);
  CHECK(sm1::scan(s, 0, "U", 1, 1, nullptr, 0, 0, c) == -1);
  CHECK(lsstore(s) == std::vector<uint64_t>{store});
  REQUIRE(sm1::commit(s) == 0);

  f.reopen();
  s = f.session();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::read(s, 0, "Ub", v) == 0);
  CHECK(v == "two");
  CHECK(lsstore(s) == std::vector<uint64_t>{store});
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("sm1_lsstore lists the storeids a transaction sees, in order") {
  // Verify sm1::lsstore reads the catalog at the transaction's view.
  // 1. In a new database the list is empty, and it replaces what
  //    out held.
  // 2. T creates three stores: T lists all three, in
  //    ascending storeid order, while a concurrent transaction
  //    lists none. The lowest is the first one created.
  // 3. T commits; T drops the middle one, which leaves T's
  //    list without it, while a transaction begun before that
  //    commit still lists all three.
  // 4. With no transaction active, sm1::lsstore fails with
  //    SM_ERR_NOTRANS.
  db_fixture_t f;
  sm1::session_t *t = f.session();
  sm1::session_t *o = f.open_other();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<uint64_t> list{42};
  REQUIRE(sm1::lsstore(t, list) == 0);
  CHECK(list.empty());

  uint64_t a = mkstore(t), b = mkstore(t), c = mkstore(t);
  CHECK(a < b);
  CHECK(b < c);
  CHECK(lsstore(t) == std::vector<uint64_t>{a, b, c});
  REQUIRE(sm1::begin(o, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(lsstore(o).empty());
  REQUIRE(sm1::commit(o) == 0);
  REQUIRE(sm1::commit(t) == 0);

  REQUIRE(sm1::begin(o, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmstore(t, b) == 0);
  CHECK(lsstore(t) == std::vector<uint64_t>{a, c});
  REQUIRE(sm1::commit(t) == 0);
  CHECK(lsstore(o) == std::vector<uint64_t>{a, b, c});
  REQUIRE(sm1::commit(o) == 0);
  sm1::closedb(o);

  CHECK(sm1::lsstore(t, list) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
}

TEST_CASE("a statement does not see its own writes; the next one does") {
  // Verify the statement mark, as PostgreSQL's command id, at each level.
  // 1. Commit row a = 1.
  // 2. Begin a statement: update a to 2 and insert b.
  // 3. A point read, and a cursor opened after the writes, see
  //    a = 1 and no b.
  // 4. The next statement sees both writes.
  // 5. Commit: the writes are there for a new transaction.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    db_fixture_t f;
    sm1::session_t *s = f.session();
    uint64_t store = mkstore_committed(s);
    REQUIRE(sm1::begin(s, level) == 0);
    put(s, store, "a", "1");
    REQUIRE(sm1::commit(s) == 0);

    REQUIRE(sm1::begin(s, level) == 0);
    REQUIRE(sm1::stmt_begin(s) == 0);
    REQUIRE(sm1::update(s, store, "a", "2") == 0);
    put(s, store, "b", "1");
    CHECK(get(s, store, "a") == "1");
    CHECK(get(s, store, "b") == "<none>");
    CHECK(scan_all(s, store) == rows_t{{"a", "1"}});

    REQUIRE(sm1::stmt_begin(s) == 0);
    CHECK(get(s, store, "a") == "2");
    CHECK(scan_all(s, store) == rows_t{{"a", "2"}, {"b", "1"}});
    REQUIRE(sm1::commit(s) == 0);

    REQUIRE(sm1::begin(s, level) == 0);
    CHECK(scan_all(s, store) == rows_t{{"a", "2"}, {"b", "1"}});
    REQUIRE(sm1::commit(s) == 0);
  }
}

TEST_CASE("a write to a row the statement already changed fails, writing "
          "nothing") {
  // Verify SM_ERR_SELFMODIFIED, PostgreSQL's "already modified by me".
  // 1. Commit row a = 1.
  // 2. In one statement update a to 2; updating or deleting it
  //    again fails with SM_ERR_SELFMODIFIED.
  // 3. The transaction is still active, and the next statement
  //    sees a = 2 and may update it.
  // 4. Commit: a holds the last update.
  // 5. With no statement begun, a transaction writes a row twice
  //    as before.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  put(s, store, "a", "1");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);
  REQUIRE(sm1::update(s, store, "a", "2") == 0);
  CHECK(sm1::update(s, store, "a", "3") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
  CHECK(sm1::erase(s, store, "a") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);

  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "a") == "2");
  REQUIRE(sm1::update(s, store, "a", "4") == 0);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(get(s, store, "a") == "4");
  REQUIRE(sm1::update(s, store, "a", "5") == 0);
  REQUIRE(sm1::update(s, store, "a", "6") == 0);
  CHECK(get(s, store, "a") == "6");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a statement inside another sees the outer's writes, and the outer "
          "resumes its own view") {
  // Verify sm1::stmt_end puts back the outer statement, as a
  // trigger's commands run inside the statement that fired it.
  // 1. Commit a = 1. An RC statement updates a to 2.
  // 2. Another session commits c, mid-statement.
  // 3. An inner statement begins: it sees a = 2 and c, and
  //    inserts b.
  // 4. The inner one ends: the outer again sees a = 1, no b, and
  //    no c, its own snapshot and mark.
  // 5. The next statement sees all three.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  sm1::session_t *other = f.open_other();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  put(s, store, "a", "1");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "a") == "1");
  REQUIRE(sm1::update(s, store, "a", "2") == 0);

  REQUIRE(sm1::begin(other, sm1::xctlevel_t::read_committed) == 0);
  put(other, store, "c", "1");
  REQUIRE(sm1::commit(other) == 0);

  sm1::stmt_t outer;
  REQUIRE(sm1::stmt_begin(s, outer) == 0);
  CHECK(get(s, store, "a") == "2");
  CHECK(get(s, store, "c") == "1");
  put(s, store, "b", "1");
  CHECK(get(s, store, "b") == "<none>");

  REQUIRE(sm1::stmt_end(s, outer) == 0);
  CHECK(scan_all(s, store) == rows_t{{"a", "1"}});

  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(scan_all(s, store) == rows_t{{"a", "2"}, {"b", "1"}, {"c", "1"}});
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(other);
}

TEST_CASE("a rollback within a statement keeps its mark below the writes "
          "that follow") {
  // Verify the mark follows seq down when a rollback rewinds it.
  // 1. Set a savepoint, write a, and begin a statement.
  // 2. Roll back to the savepoint: a is gone, and seq is rewound
  //    below the statement's mark.
  // 3. Write b in the same statement: the statement does not see
  //    it, though b reuses a's seq.
  // 4. The next statement sees b.
  db_fixture_t f;
  sm1::session_t *s = f.session();
  uint64_t store = mkstore_committed(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  int depth = sm1::savepoint(s);
  REQUIRE(depth == 1);
  put(s, store, "a", "1");
  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(get(s, store, "a") == "1");

  REQUIRE(sm1::rollback(s, depth) == 0);
  CHECK(get(s, store, "a") == "<none>");
  put(s, store, "b", "1");
  CHECK(get(s, store, "b") == "<none>");

  REQUIRE(sm1::stmt_begin(s) == 0);
  CHECK(scan_all(s, store) == rows_t{{"b", "1"}});
  REQUIRE(sm1::commit(s) == 0);
}
