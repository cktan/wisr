/*
 * test_access.cc -- relations and rows: sm_mkrel, sm_rmrel,
 * sm_lsrel, sm_insert, sm_update, sm_delete, sm_read, and sm_scan.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

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
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_session = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_session);
  }

  /* The session. */
  sm_session_t *session() const { return m_session; }

  /* Another session on the same database. */
  sm_session_t *open_other() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm_closedb(m_session) == 0);
    m_home.reattach();
    m_session = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE(m_session);
  }

private:
  home_t m_home;
  sm_session_t *m_session = nullptr;
};

using rows_t = std::vector<std::pair<std::string, std::string>>;

/* Drain a cursor into (key, value) pairs and close it. */
rows_t drain(sm_scan_t *c) {
  REQUIRE_MESSAGE(c, sm_errmsg());
  rows_t rows;
  const char *k, *d;
  size_t kl, dl;
  int r;
  while ((r = sm_scannxt(c, &k, &kl, &d, &dl)) == 0) {
    rows.emplace_back(std::string(k, kl), std::string(d, dl));
  }
  CHECK(r == 1);
  CHECK(sm_scannxt(c, &k, &kl, &d, &dl) == 1);
  sm_scanend(c);
  return rows;
}

/* Every row of rel, in order. */
rows_t scan_all(sm_session_t *s, uint64_t rel) {
  return drain(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0));
}

/* Create a relation in its own committed transaction. */
uint64_t mkrel_committed(sm_session_t *s) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t rel = mkrel(s);
  REQUIRE(sm_commit(s) == 0);
  return rel;
}

} // namespace

TEST_CASE("sm_mkrel and sm_rmrel are transactional") {
  // Verify DDL follows commit and abort.
  // 1. Create a relation and abort; its relid names nothing
  //    afterwards.
  // 2. Create one and commit; it holds its rows.
  // 3. Drop it and abort; it still exists with its rows.
  // 4. Drop it and commit; it is gone, and dropping it again
  //    fails with SM_ERR_NOTFOUND.
  // 5. A relation created now gets a relid of its own, never
  //    one handed out before, and starts empty.
  db_fixture_t f;
  sm_session_t *s = f.session();

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t gone = mkrel(s);
  put(s, gone, "k", "v");
  REQUIRE(sm_abort(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm_read(s, gone, "k", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  uint64_t rel = mkrel(s);
  CHECK(rel != gone);
  put(s, rel, "k", "v");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmrel(s, rel) == 0);
  REQUIRE(sm_abort(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, rel, "k") == "v");
  REQUIRE(sm_rmrel(s, rel) == 0);
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_rmrel(s, rel) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  uint64_t fresh = mkrel(s);
  CHECK(fresh > rel);
  CHECK(get(s, fresh, "k") == "<none>");
  REQUIRE(sm_commit(s) == 0);
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
  for (sm_xctlevel_t level :
       {SM_XCT_READ_COMMITTED, SM_XCT_REPEATABLE_READ, SM_XCT_SERIALIZABLE}) {
    CAPTURE(level);
    db_fixture_t f;
    sm_session_t *s = f.session();
    uint64_t rel = mkrel_committed(s);
    REQUIRE(sm_begin(s, level) == 0);

    put(s, rel, "k", "v1");
    CHECK(get(s, rel, "k") == "v1");
    CHECK(sm_insert(s, rel, "k", 1, "x", 1) == -1);
    CHECK(sm_errno() == SM_ERR_DUPKEY);

    REQUIRE(sm_update(s, rel, "k", 1, "v2", 2, nullptr, nullptr) == 0);
    REQUIRE(sm_update(s, rel, "k", 1, "v3", 2, nullptr, nullptr) == 0);
    CHECK(get(s, rel, "k") == "v3");

    REQUIRE(sm_delete(s, rel, "k", 1, nullptr, nullptr) == 0);
    CHECK(get(s, rel, "k") == "<none>");
    CHECK(sm_update(s, rel, "k", 1, "x", 1, nullptr, nullptr) == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);
    CHECK(sm_delete(s, rel, "k", 1, nullptr, nullptr) == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);

    put(s, rel, "k", "v4");
    CHECK(get(s, rel, "k") == "v4");

    put(s, rel, "", "empty-key");
    put(s, rel, "e", "");
    CHECK(get(s, rel, "") == "empty-key");
    CHECK(get(s, rel, "e") == "");
    CHECK(get(s, rel, "missing") == "<none>");
    REQUIRE(sm_commit(s) == 0);
  }
}

TEST_CASE("commit persists writes across reattach; abort discards them") {
  // Verify durability of commits and the absence of aborted writes.
  // 1. Commit a key "a"; write "b" and update "a", then abort.
  // 2. A new transaction sees "a" with its committed value and
  //    no "b".
  // 3. Detach and reattach; the same holds.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "a", "committed");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "b", "aborted");
  REQUIRE(sm_update(s, rel, "a", 1, "aborted", 7, nullptr, nullptr) == 0);
  REQUIRE(sm_abort(s) == 0);

  for (int pass = 0; pass < 2; pass++) {
    CAPTURE(pass);
    if (pass == 1) {
      f.reopen();
      s = f.session();
    }
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(s, rel, "a") == "committed");
    CHECK(get(s, rel, "b") == "<none>");
    CHECK(scan_all(s, rel) == rows_t{{"a", "committed"}});
    REQUIRE(sm_commit(s) == 0);
  }
}

TEST_CASE("repeatable read keeps its snapshot; read committed does not") {
  // Verify the two levels' snapshots against another session's commit.
  // 1. Session A begins RR, session B begins RC; both read "k"
  //    as missing.
  // 2. Session C inserts "k" and commits.
  // 3. A still reads it as missing; B now reads the new value.
  db_fixture_t f;
  sm_session_t *a = f.session();
  uint64_t rel = mkrel_committed(a);
  sm_session_t *b = f.open_other();
  sm_session_t *c = f.open_other();

  REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(b, SM_XCT_READ_COMMITTED) == 0);
  CHECK(get(a, rel, "k") == "<none>");
  CHECK(get(b, rel, "k") == "<none>");

  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  put(c, rel, "k", "v");
  REQUIRE(sm_commit(c) == 0);

  CHECK(get(a, rel, "k") == "<none>");
  CHECK(scan_all(a, rel).empty());
  CHECK(get(b, rel, "k") == "v");
  CHECK(scan_all(b, rel) == rows_t{{"k", "v"}});
  REQUIRE(sm_commit(a) == 0);
  REQUIRE(sm_commit(b) == 0);
  sm_closedb(b);
  sm_closedb(c);
}

TEST_CASE("sm_stmt_begin gives read committed one snapshot per statement") {
  // Verify a statement spanning several calls sees one committed
  // state, the anomaly SECINDEX.md gap 3 describes.
  // 1. Two rows each hold 50; an RC session begins a statement
  //    and reads the first.
  // 2. Another session moves 30 from the first row to the
  //    second and commits, mid-statement.
  // 3. The second read still sees 50, so the statement totals
  //    100, not 130.
  // 4. The next sm_stmt_begin starts a new statement, which
  //    sees the transfer.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  sm_session_t *other = f.open_other();

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "a", "50");
  put(s, rel, "b", "50");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_stmt_begin(s) == 0);
  CHECK(get(s, rel, "a") == "50");

  REQUIRE(sm_begin(other, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_update(other, rel, "a", 1, "20", 2, nullptr, nullptr) == 0);
  REQUIRE(sm_update(other, rel, "b", 1, "80", 2, nullptr, nullptr) == 0);
  REQUIRE(sm_commit(other) == 0);

  CHECK(get(s, rel, "b") == "50");
  CHECK(scan_all(s, rel) == rows_t{{"a", "50"}, {"b", "50"}});

  REQUIRE(sm_stmt_begin(s) == 0);
  CHECK(get(s, rel, "a") == "20");
  CHECK(get(s, rel, "b") == "80");
  REQUIRE(sm_commit(s) == 0);
  sm_closedb(other);
}

TEST_CASE("the statement snapshot is taken by the first read, not the call") {
  // Verify a statement that waits before reading still sees what
  // committed while it waited (SECINDEX.md gap 1's build protocol).
  // 1. An RC session begins a statement and does not read.
  // 2. Another session commits a row.
  // 3. The first read of the statement sees it.
  // 4. A write does not take the snapshot: after a new
  //    statement writes, a commit by another session is still
  //    visible to that statement's first read.
  // 5. A commit clears the pin: a fresh RC transaction that
  //    never calls sm_stmt_begin still reads per call.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  sm_session_t *other = f.open_other();

  REQUIRE(sm_begin(s, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_stmt_begin(s) == 0);

  REQUIRE(sm_begin(other, SM_XCT_READ_COMMITTED) == 0);
  put(other, rel, "k", "v");
  REQUIRE(sm_commit(other) == 0);

  CHECK(get(s, rel, "k") == "v");

  REQUIRE(sm_stmt_begin(s) == 0);
  put(s, rel, "mine", "1");
  REQUIRE(sm_begin(other, SM_XCT_READ_COMMITTED) == 0);
  put(other, rel, "late", "v");
  REQUIRE(sm_commit(other) == 0);
  CHECK(get(s, rel, "late") == "v");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_READ_COMMITTED) == 0);
  CHECK(get(s, rel, "k") == "v");
  REQUIRE(sm_begin(other, SM_XCT_READ_COMMITTED) == 0);
  REQUIRE(sm_update(other, rel, "k", 1, "w", 1, nullptr, nullptr) == 0);
  REQUIRE(sm_commit(other) == 0);
  CHECK(get(s, rel, "k") == "w");
  REQUIRE(sm_commit(s) == 0);
  sm_closedb(other);
}

TEST_CASE("sm_stmt_begin needs a transaction and does nothing above RC") {
  // Verify the call's contract at the other levels and outside a
  // transaction.
  // 1. Without a transaction it fails with SM_ERR_NOTRANS.
  // 2. Under REPEATABLE READ it succeeds and changes nothing:
  //    the transaction still reads at its begin snapshot.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  sm_session_t *other = f.open_other();

  CHECK(sm_stmt_begin(s) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_stmt_begin(s) == 0);
  CHECK(get(s, rel, "k") == "<none>");

  REQUIRE(sm_begin(other, SM_XCT_READ_COMMITTED) == 0);
  put(other, rel, "k", "v");
  REQUIRE(sm_commit(other) == 0);

  REQUIRE(sm_stmt_begin(s) == 0);
  CHECK(get(s, rel, "k") == "<none>");
  REQUIRE(sm_commit(s) == 0);
  sm_closedb(other);
}

TEST_CASE("a scan returns userkeys in memcmp order within its bounds") {
  // Verify key order and bound handling (DESIGN.md §12.5 key order).
  // 1. Insert keys that prefix one another, each updated so it
  //    has several versions.
  // 2. A full scan returns each key once, newest value, in
  //    memcmp order.
  // 3. Inclusive and exclusive bounds include or exclude the
  //    bound keys; an inverted range is empty.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  std::vector<std::string> keys = {"", "a", std::string("a\0", 2), "a\xff",
                                   "ab"};
  for (int round = 0; round < 3; round++) {
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (const std::string &k : keys) {
      std::string v = k + "#" + std::to_string(round);
      if (round == 0) {
        put(s, rel, k, v);
      } else {
        REQUIRE(sm_update(s, rel, k.data(), k.size(), v.data(), v.size(),
                          nullptr, nullptr) == 0);
      }
    }
    REQUIRE(sm_commit(s) == 0);
  }

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  rows_t all;
  // memcmp order: "ab" (61 62) sorts before "a\xff" (61 ff).
  for (const std::string &k :
       {std::string(""), std::string("a"), std::string("a\0", 2),
        std::string("ab"), std::string("a\xff")}) {
    all.emplace_back(k, k + "#2");
    CHECK(get(s, rel, k) == k + "#2");
  }
  CHECK(scan_all(s, rel) == all);

  std::string a0("a\0", 2);
  CHECK(drain(sm_scan(s, rel, "a", 1, 1, "a\xff", 2, 1)) ==
        rows_t(all.begin() + 1, all.begin() + 5));
  CHECK(drain(sm_scan(s, rel, "a", 1, 0, "a\xff", 2, 0)) ==
        rows_t(all.begin() + 2, all.begin() + 4));
  CHECK(drain(sm_scan(s, rel, "", 0, 0, a0.data(), 2, 1)) ==
        rows_t(all.begin() + 1, all.begin() + 3));
  CHECK(drain(sm_scan(s, rel, "b", 1, 1, "a", 1, 1)).empty());
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a cursor does not see writes made after it opened") {
  // Verify Halloween protection (DESIGN.md §10.2).
  // 1. Insert "a" and "b", then open a cursor.
  // 2. Insert "c" and update "a" in the same transaction.
  // 3. The cursor returns exactly the rows as of its opening.
  // 4. A cursor opened afterwards sees the new writes.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "a", "1");
  put(s, rel, "b", "2");
  sm_scan_t *c = sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  put(s, rel, "c", "3");
  REQUIRE(sm_update(s, rel, "a", 1, "9", 1, nullptr, nullptr) == 0);
  CHECK(drain(c) == rows_t{{"a", "1"}, {"b", "2"}});
  CHECK(scan_all(s, rel) == rows_t{{"a", "9"}, {"b", "2"}, {"c", "3"}});
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("commit closes open cursors") {
  // Verify commit and abort release a session's cursors.
  // 1. Open two cursors and commit without closing them.
  // 2. Open one and abort. Nothing leaks or crashes (ASan/valgrind
  //    would report it); the session begins again normally.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0));
  REQUIRE(sm_abort(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_commit(s) == 0);
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
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  char key[16];
  for (int i = 0; i < 10000; i += 2) {
    snprintf(key, sizeof(key), "k%05d", i);
    put(s, rel, key, "v");
  }
  REQUIRE(sm_commit(s) == 0);
  f.reopen();
  s = f.session();

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  rows_t rows = drain(sm_scan(s, rel, "k00001", 6, 1, nullptr, 0, 0));
  CHECK(rows.size() == 4999);
  CHECK(rows.front().first == "k00002");
  CHECK(rows.back().first == "k09998");
  rows = drain(sm_scan(s, rel, "k00101", 6, 1, "k00301", 6, 0));
  CHECK(rows.size() == 100);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("sm_scan_seek repositions a cursor without reopening it") {
  // Verify the reposition keeps bounds, view and order (§10.2).
  // 1. Scan k1..k9 and read one row.
  // 2. Seek to k5 inclusive: the next rows start at k5.
  // 3. Seek to k7 exclusive: the next row is k8.
  // 4. Seek past the end key: the cursor reports end, and
  //    stays at end afterwards.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, rel, k, std::to_string(i));
  }
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  sm_scan_t *c = sm_scan(s, rel, "k1", 2, 1, "k9", 2, 1);
  REQUIRE(c);
  const char *k, *d;
  size_t kl, dl;
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k1");

  REQUIRE(sm_scan_seek(c, "k5", 2, 1) == 0);
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k5");
  CHECK(std::string(d, dl) == "5");

  REQUIRE(sm_scan_seek(c, "k7", 2, 0) == 0);
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k8");

  REQUIRE(sm_scan_seek(c, "k99", 3, 1) == 0);
  CHECK(sm_scannxt(c, &k, &kl, &d, &dl) == 1);
  REQUIRE(sm_scan_seek(c, "k99", 3, 1) == 0);
  CHECK(sm_scannxt(c, &k, &kl, &d, &dl) == 1);
  sm_scanend(c);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("sm_scan_seek refuses to move backwards or out of range") {
  // Verify the forward-only rule and the bounds check (§10.2).
  // 1. Open a cursor over k3..k7 and read k3.
  // 2. A seek to k2, below the start key, fails.
  // 3. A seek back to k3 after reading k5 fails.
  // 4. Re-reading the key just read is allowed, since the
  //    cursor has not passed it.
  // 5. A NULL cursor and an oversized key are rejected.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, rel, k, std::to_string(i));
  }
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  sm_scan_t *c = sm_scan(s, rel, "k3", 2, 1, "k7", 2, 1);
  REQUIRE(c);
  const char *k, *d;
  size_t kl, dl;
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k3");

  CHECK(sm_scan_seek(c, "k2", 2, 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);

  REQUIRE(sm_scan_seek(c, "k5", 2, 1) == 0);
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k5");
  CHECK(sm_scan_seek(c, "k3", 2, 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);

  REQUIRE(sm_scan_seek(c, "k5", 2, 1) == 0);
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "k5");

  CHECK(sm_scan_seek(nullptr, "k5", 2, 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  std::string huge(SM_MAX_KEYLEN + 1, 'z');
  CHECK(sm_scan_seek(c, huge.data(), huge.size(), 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  sm_scanend(c);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("keys skipped by sm_scan_seek still count as read under SSI") {
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
  sm_session_t *s = f.session();
  sm_session_t *t2 = f.open_other();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (int i = 1; i <= 9; i++) {
    std::string k = "k" + std::to_string(i);
    put(s, rel, k, std::to_string(i));
  }
  put(s, rel, "own", "0");
  REQUIRE(sm_commit(s) == 0);

  // Run the write skew, seeking over k5 or stopping short of
  // it; true if either transaction was a serialization victim.
  auto skew = [&](bool seek_past_k5) {
    REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
    sm_scan_t *c = sm_scan(s, rel, "k1", 2, 1, "k9", 2, 1);
    REQUIRE(c);
    const char *k, *d;
    size_t kl, dl;
    REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
    CHECK(std::string(k, kl) == "k1");
    if (seek_past_k5) {
      REQUIRE(sm_scan_seek(c, "k8", 2, 1) == 0);
      REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
      CHECK(std::string(k, kl) == "k8");
    }
    sm_scanend(c);

    REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
    CHECK(get(t2, rel, "own") != "<none>");
    REQUIRE(sm_update(s, rel, "own", 3, "t1", 2, nullptr, nullptr) == 0);
    REQUIRE(sm_update(t2, rel, "k5", 2, "t2", 2, nullptr, nullptr) == 0);

    int rc2 = sm_commit(t2);
    int err2 = sm_errno();
    int rc1 = sm_commit(s);
    int err1 = sm_errno();
    // Every failure here is a serialization failure.
    if (rc2 == -1) {
      CHECK(err2 == SM_ERR_SERIALIZATION);
    }
    if (rc1 == -1) {
      CHECK(err1 == SM_ERR_SERIALIZATION);
    }
    return rc1 == -1 || rc2 == -1;
  };

  // A cursor that stopped at k1 never covered k5.
  CHECK_FALSE(skew(false));
  // Seeking over k5 still counts it as read.
  CHECK(skew(true));
  sm_closedb(t2);
}

TEST_CASE("sm_update and sm_delete hand back the row they replaced") {
  // Verify the old-image return (SECINDEX.md gap 6).
  // 1. Update a row: the old value is the one committed before.
  // 2. Update it again: the old value is this transaction's own
  //    previous write, not the committed one.
  // 3. Delete it: the old value is the last one written.
  // 4. An empty old value is reported with length 0.
  // 5. A failed write returns no old row, and passing one
  //    out-parameter without the other is SM_ERR_INVALID.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "k", "v0");
  put(s, rel, "empty", "");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  const char *old = nullptr;
  size_t oldlen = 0;
  REQUIRE(sm_update(s, rel, "k", 1, "v1", 2, &old, &oldlen) == 0);
  CHECK(std::string(old, oldlen) == "v0");

  REQUIRE(sm_update(s, rel, "k", 1, "v2", 2, &old, &oldlen) == 0);
  CHECK(std::string(old, oldlen) == "v1");

  REQUIRE(sm_delete(s, rel, "k", 1, &old, &oldlen) == 0);
  CHECK(std::string(old, oldlen) == "v2");

  REQUIRE(sm_update(s, rel, "empty", 5, "now", 3, &old, &oldlen) == 0);
  CHECK(oldlen == 0);

  // The key is gone, so this fails and reports no old row.
  old = (const char *)1;
  oldlen = 99;
  CHECK(sm_update(s, rel, "k", 1, "v3", 2, &old, &oldlen) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  CHECK(old == nullptr);
  CHECK(oldlen == 0);

  CHECK(sm_update(s, rel, "empty", 5, "x", 1, &old, nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_delete(s, rel, "empty", 5, nullptr, &oldlen) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("the old row shares sm_read's buffer and its lifetime") {
  // Verify the documented lifetime of the old image.
  // 1. Update a row, keeping the old value's pointer.
  // 2. A read on the same session overwrites it, so the caller
  //    must copy what it wants to keep.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, rel, "k", "old");
  put(s, rel, "other", "other-value");
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  const char *old = nullptr;
  size_t oldlen = 0;
  REQUIRE(sm_update(s, rel, "k", 1, "new", 3, &old, &oldlen) == 0);
  std::string copied(old, oldlen);
  CHECK(copied == "old");

  const char *d;
  size_t dl;
  REQUIRE(sm_read(s, rel, "other", 5, &d, &dl) == 0);
  CHECK(std::string(d, dl) == "other-value");
  // The copy still holds what the update returned.
  CHECK(copied == "old");
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("oversized keys and values are rejected") {
  // Verify the documented limits.
  // 1. A key of SM_MAX_KEYLEN bytes is accepted; one byte more
  //    fails with SM_ERR_INVALID, for insert and for read.
  // 2. An unknown relation fails with SM_ERR_NOTFOUND.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t rel = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  std::string max(SM_MAX_KEYLEN, 'k');
  put(s, rel, max, "v");
  CHECK(get(s, rel, max) == "v");
  std::string over(SM_MAX_KEYLEN + 1, 'k');
  CHECK(sm_insert(s, rel, over.data(), over.size(), "v", 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  const char *d;
  size_t dl;
  CHECK(sm_read(s, rel, over.data(), over.size(), &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_insert(s, 999, "k", 1, "v", 1) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a dropped relation stays readable at an older snapshot") {
  // Verify a drop leaves older snapshots alone, and that its
  // relid never comes back.
  // 1. Create a relation with "k" -> "old" and commit.
  // 2. Reader R begins REPEATABLE READ and reads "old".
  // 3. S drops it and creates another in one transaction: the
  //    new relation has a new relid and starts empty. S
  //    inserts "k" -> "new" and commits.
  // 4. R still reads and scans the dropped relation's rows.
  // 5. A new transaction finds the old relid gone and reads
  //    "new" from the new relation.
  db_fixture_t f;
  sm_session_t *s = f.session();
  uint64_t old = mkrel_committed(s);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, old, "k", "old");
  REQUIRE(sm_commit(s) == 0);

  sm_session_t *r = f.open_other();
  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(r, old, "k") == "old");

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmrel(s, old) == 0);
  uint64_t rel = mkrel(s);
  CHECK(rel > old);
  CHECK(get(s, rel, "k") == "<none>");
  put(s, rel, "k", "new");
  REQUIRE(sm_commit(s) == 0);

  CHECK(get(r, old, "k") == "old");
  CHECK(scan_all(r, old) == rows_t{{"k", "old"}});
  REQUIRE(sm_commit(r) == 0);

  REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm_read(r, old, "k", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  CHECK(get(r, rel, "k") == "new");
  CHECK(scan_all(r, rel) == rows_t{{"k", "new"}});
  REQUIRE(sm_commit(r) == 0);
  sm_closedb(r);
}

TEST_CASE("sm_lsrel lists the relids a transaction sees, in order") {
  // Verify sm_lsrel reads the catalog at the transaction's view.
  // 1. In a new database the list is empty: *n is 0 and *out
  //    NULL.
  // 2. T creates three relations: T lists all three, in
  //    ascending relid order, while a concurrent transaction
  //    lists none. The lowest is the first one created.
  // 3. T commits; T drops the middle one, which leaves T's
  //    list without it, while a transaction begun before that
  //    commit still lists all three.
  // 4. With no transaction active, sm_lsrel fails with
  //    SM_ERR_NOTRANS.
  db_fixture_t f;
  sm_session_t *t = f.session();
  sm_session_t *o = f.open_other();
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t *list = reinterpret_cast<uint64_t *>(1);
  size_t n = 1;
  REQUIRE(sm_lsrel(t, &list, &n) == 0);
  CHECK(n == 0);
  CHECK(list == nullptr);

  uint64_t a = mkrel(t), b = mkrel(t), c = mkrel(t);
  CHECK(a < b);
  CHECK(b < c);
  CHECK(lsrel(t) == std::vector<uint64_t>{a, b, c});
  REQUIRE(sm_begin(o, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(lsrel(o).empty());
  REQUIRE(sm_commit(o) == 0);
  REQUIRE(sm_commit(t) == 0);

  REQUIRE(sm_begin(o, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_rmrel(t, b) == 0);
  CHECK(lsrel(t) == std::vector<uint64_t>{a, c});
  REQUIRE(sm_commit(t) == 0);
  CHECK(lsrel(o) == std::vector<uint64_t>{a, b, c});
  REQUIRE(sm_commit(o) == 0);
  sm_closedb(o);

  CHECK(sm_lsrel(t, &list, &n) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
}
