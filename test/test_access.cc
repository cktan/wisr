/*
 * test_access.cc -- relations and rows: sm_mkrel, sm_rmrel,
 * sm_insert, sm_update, sm_delete, sm_read, and sm_scan.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

#include <stdio.h>

#include <string>
#include <utility>
#include <vector>

namespace
{

/* A database "db" with one open session, for one test. */
class db_fixture_t
{
  public:
    /* Create "db" and open a session on it. */
    db_fixture_t()
    {
        REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
        m_session = sm_open(m_home.env(), "db", nullptr);
        REQUIRE(m_session);
    }

    /* The session. */
    sm_session_t* session() const
    {
        return m_session;
    }

    /* Another session on the same database. */
    sm_session_t* open_other() const
    {
        sm_session_t* s = sm_open(m_home.env(), "db", nullptr);
        REQUIRE(s);
        return s;
    }

    /* Close the session, reattach, and reopen "db". */
    void reopen()
    {
        REQUIRE(sm_close(m_session) == 0);
        m_home.reattach();
        m_session = sm_open(m_home.env(), "db", nullptr);
        REQUIRE(m_session);
    }

  private:
    home_t m_home;
    sm_session_t* m_session = nullptr;
};

using rows_t = std::vector<std::pair<std::string, std::string>>;

/* Drain a cursor into (key, value) pairs and close it. */
rows_t drain(sm_scan_t* c)
{
    REQUIRE_MESSAGE(c, sm_errmsg());
    rows_t rows;
    const char *k, *d;
    size_t kl, dl;
    int r;
    while ((r = sm_scan_next(c, &k, &kl, &d, &dl)) == 0) {
        rows.emplace_back(std::string(k, kl), std::string(d, dl));
    }
    CHECK(r == 1);
    CHECK(sm_scan_next(c, &k, &kl, &d, &dl) == 1);
    sm_scan_end(c);
    return rows;
}

/* Every row of rel, in order. */
rows_t scan_all(sm_session_t* s, const char* rel)
{
    return drain(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0));
}

/* Create relation rel in its own committed transaction. */
void mkrel(sm_session_t* s, const char* rel)
{
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, rel) == 0);
    REQUIRE(sm_commit(s) == 0);
}

} // namespace

TEST_CASE("sm_mkrel and sm_rmrel are transactional")
{
    // Verify DDL follows commit and abort.
    // 1. Create "t" and abort; "t" does not exist afterwards.
    // 2. Create "t" and commit; creating it again fails with
    //    SM_ERR_EXISTS.
    // 3. Drop "t" and abort; it still exists with its rows.
    // 4. Drop "t" and commit; it is gone, and dropping it again
    //    fails with SM_ERR_NOTFOUND.
    // 5. Create "t" again under the same name; it is empty.
    db_fixture_t f;
    sm_session_t* s = f.session();

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "t") == 0);
    put(s, "t", "k", "v");
    REQUIRE(sm_abort(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    const char* d;
    size_t dl;
    CHECK(sm_read(s, "t", "k", 1, &d, &dl) == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);
    REQUIRE(sm_mkrel(s, "t") == 0);
    put(s, "t", "k", "v");
    REQUIRE(sm_commit(s) == 0);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(sm_mkrel(s, "t") == -1);
    CHECK(sm_errno() == SM_ERR_EXISTS);
    REQUIRE(sm_rmrel(s, "t") == 0);
    REQUIRE(sm_abort(s) == 0);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(s, "t", "k") == "v");
    REQUIRE(sm_rmrel(s, "t") == 0);
    REQUIRE(sm_commit(s) == 0);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(sm_rmrel(s, "t") == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);
    REQUIRE(sm_mkrel(s, "t") == 0);
    CHECK(get(s, "t", "k") == "<none>");
    REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a transaction reads its own inserts, updates, and deletes")
{
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
        sm_session_t* s = f.session();
        mkrel(s, "t");
        REQUIRE(sm_begin(s, level) == 0);

        put(s, "t", "k", "v1");
        CHECK(get(s, "t", "k") == "v1");
        CHECK(sm_insert(s, "t", "k", 1, "x", 1) == -1);
        CHECK(sm_errno() == SM_ERR_DUPKEY);

        REQUIRE(sm_update(s, "t", "k", 1, "v2", 2) == 0);
        REQUIRE(sm_update(s, "t", "k", 1, "v3", 2) == 0);
        CHECK(get(s, "t", "k") == "v3");

        REQUIRE(sm_delete(s, "t", "k", 1) == 0);
        CHECK(get(s, "t", "k") == "<none>");
        CHECK(sm_update(s, "t", "k", 1, "x", 1) == -1);
        CHECK(sm_errno() == SM_ERR_NOTFOUND);
        CHECK(sm_delete(s, "t", "k", 1) == -1);
        CHECK(sm_errno() == SM_ERR_NOTFOUND);

        put(s, "t", "k", "v4");
        CHECK(get(s, "t", "k") == "v4");

        put(s, "t", "", "empty-key");
        put(s, "t", "e", "");
        CHECK(get(s, "t", "") == "empty-key");
        CHECK(get(s, "t", "e") == "");
        CHECK(get(s, "t", "missing") == "<none>");
        REQUIRE(sm_commit(s) == 0);
    }
}

TEST_CASE("commit persists writes across reattach; abort discards them")
{
    // Verify durability of commits and the absence of aborted writes.
    // 1. Commit a key "a"; write "b" and update "a", then abort.
    // 2. A new transaction sees "a" with its committed value and
    //    no "b".
    // 3. Detach and reattach; the same holds.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    put(s, "t", "a", "committed");
    REQUIRE(sm_commit(s) == 0);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    put(s, "t", "b", "aborted");
    REQUIRE(sm_update(s, "t", "a", 1, "aborted", 7) == 0);
    REQUIRE(sm_abort(s) == 0);

    for (int pass = 0; pass < 2; pass++) {
        CAPTURE(pass);
        if (pass == 1) {
            f.reopen();
            s = f.session();
        }
        REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
        CHECK(get(s, "t", "a") == "committed");
        CHECK(get(s, "t", "b") == "<none>");
        CHECK(scan_all(s, "t") == rows_t{{"a", "committed"}});
        REQUIRE(sm_commit(s) == 0);
    }
}

TEST_CASE("repeatable read keeps its snapshot; read committed does not")
{
    // Verify the two levels' snapshots against another session's commit.
    // 1. Session A begins RR, session B begins RC; both read "k"
    //    as missing.
    // 2. Session C inserts "k" and commits.
    // 3. A still reads it as missing; B now reads the new value.
    db_fixture_t f;
    sm_session_t* a = f.session();
    mkrel(a, "t");
    sm_session_t* b = f.open_other();
    sm_session_t* c = f.open_other();

    REQUIRE(sm_begin(a, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_begin(b, SM_XCT_READ_COMMITTED) == 0);
    CHECK(get(a, "t", "k") == "<none>");
    CHECK(get(b, "t", "k") == "<none>");

    REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
    put(c, "t", "k", "v");
    REQUIRE(sm_commit(c) == 0);

    CHECK(get(a, "t", "k") == "<none>");
    CHECK(scan_all(a, "t").empty());
    CHECK(get(b, "t", "k") == "v");
    CHECK(scan_all(b, "t") == rows_t{{"k", "v"}});
    REQUIRE(sm_commit(a) == 0);
    REQUIRE(sm_commit(b) == 0);
    sm_close(b);
    sm_close(c);
}

TEST_CASE("a scan returns userkeys in memcmp order within its bounds")
{
    // Verify key order and bound handling (DESIGN.md §12.5 key order).
    // 1. Insert keys that prefix one another, each updated so it
    //    has several versions.
    // 2. A full scan returns each key once, newest value, in
    //    memcmp order.
    // 3. Inclusive and exclusive bounds include or exclude the
    //    bound keys; an inverted range is empty.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    std::vector<std::string> keys = {"", "a", std::string("a\0", 2), "a\xff",
                                     "ab"};
    for (int round = 0; round < 3; round++) {
        REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
        for (const std::string& k : keys) {
            std::string v = k + "#" + std::to_string(round);
            if (round == 0) {
                put(s, "t", k, v);
            } else {
                REQUIRE(sm_update(s, "t", k.data(), k.size(), v.data(),
                                  v.size()) == 0);
            }
        }
        REQUIRE(sm_commit(s) == 0);
    }

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    rows_t all;
    // memcmp order: "ab" (61 62) sorts before "a\xff" (61 ff).
    for (const std::string& k :
         {std::string(""), std::string("a"), std::string("a\0", 2),
          std::string("ab"), std::string("a\xff")}) {
        all.emplace_back(k, k + "#2");
        CHECK(get(s, "t", k) == k + "#2");
    }
    CHECK(scan_all(s, "t") == all);

    std::string a0("a\0", 2);
    CHECK(drain(sm_scan(s, "t", "a", 1, 1, "a\xff", 2, 1)) ==
          rows_t(all.begin() + 1, all.begin() + 5));
    CHECK(drain(sm_scan(s, "t", "a", 1, 0, "a\xff", 2, 0)) ==
          rows_t(all.begin() + 2, all.begin() + 4));
    CHECK(drain(sm_scan(s, "t", "", 0, 0, a0.data(), 2, 1)) ==
          rows_t(all.begin() + 1, all.begin() + 3));
    CHECK(drain(sm_scan(s, "t", "b", 1, 1, "a", 1, 1)).empty());
    REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a cursor does not see writes made after it opened")
{
    // Verify Halloween protection (DESIGN.md §10.2).
    // 1. Insert "a" and "b", then open a cursor.
    // 2. Insert "c" and update "a" in the same transaction.
    // 3. The cursor returns exactly the rows as of its opening.
    // 4. A cursor opened afterwards sees the new writes.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    put(s, "t", "a", "1");
    put(s, "t", "b", "2");
    sm_scan_t* c = sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0);
    REQUIRE(c);
    put(s, "t", "c", "3");
    REQUIRE(sm_update(s, "t", "a", 1, "9", 1) == 0);
    CHECK(drain(c) == rows_t{{"a", "1"}, {"b", "2"}});
    CHECK(scan_all(s, "t") == rows_t{{"a", "9"}, {"b", "2"}, {"c", "3"}});
    REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("commit closes open cursors")
{
    // Verify commit and abort release a session's cursors.
    // 1. Open two cursors and commit without closing them.
    // 2. Open one and abort. Nothing leaks or crashes (ASan/valgrind
    //    would report it); the session begins again normally.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0));
    REQUIRE(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0));
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0));
    REQUIRE(sm_abort(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a scan from an absent key crosses many userkeys in SSTs")
{
    // Regression for inheriting RocksDB's prefix seek mode (§10.2, §12.5).
    // 1. Insert 5000 keys "k00000".."k04999" (every other number
    //    is skipped, so start keys can be absent) and commit.
    // 2. Reattach, which flushes the memtable into SSTs.
    // 3. Scan from an absent key "k00001" to the end: every key
    //    above it is returned, not zero or one.
    // 4. Scan a closed range spanning many keys: all are returned.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    char key[16];
    for (int i = 0; i < 10000; i += 2) {
        snprintf(key, sizeof(key), "k%05d", i);
        put(s, "t", key, "v");
    }
    REQUIRE(sm_commit(s) == 0);
    f.reopen();
    s = f.session();

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    rows_t rows = drain(sm_scan(s, "t", "k00001", 6, 1, nullptr, 0, 0));
    CHECK(rows.size() == 4999);
    CHECK(rows.front().first == "k00002");
    CHECK(rows.back().first == "k09998");
    rows = drain(sm_scan(s, "t", "k00101", 6, 1, "k00301", 6, 0));
    CHECK(rows.size() == 100);
    REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("oversized keys and values are rejected")
{
    // Verify the documented limits.
    // 1. A key of SM_MAX_KEYLEN bytes is accepted; one byte more
    //    fails with SM_ERR_INVALID, for insert and for read.
    // 2. An unknown relation fails with SM_ERR_NOTFOUND.
    db_fixture_t f;
    sm_session_t* s = f.session();
    mkrel(s, "t");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    std::string max(SM_MAX_KEYLEN, 'k');
    put(s, "t", max, "v");
    CHECK(get(s, "t", max) == "v");
    std::string over(SM_MAX_KEYLEN + 1, 'k');
    CHECK(sm_insert(s, "t", over.data(), over.size(), "v", 1) == -1);
    CHECK(sm_errno() == SM_ERR_INVALID);
    const char* d;
    size_t dl;
    CHECK(sm_read(s, "t", over.data(), over.size(), &d, &dl) == -1);
    CHECK(sm_errno() == SM_ERR_INVALID);
    CHECK(sm_insert(s, "nope", "k", 1, "v", 1) == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);
    REQUIRE(sm_commit(s) == 0);
}
