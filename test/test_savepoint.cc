/*
 * test_savepoint.cc -- sm_savepoint and sm_rollback (DESIGN.md
 * §6.5, §12.5 "Own writes").
 */

#include "doctest.h"
#include "util.h"

#include "session.h"
#include "sm.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

/* A database "db" with relation "t" holding "a" -> "0". */
class sp_fixture_t
{
  public:
    /* Create "db", "t", and the row. */
    sp_fixture_t()
    {
        REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
        sm_session_t* s = open();
        REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
        REQUIRE(sm_mkrel(s, "t") == 0);
        put(s, "t", "a", "0");
        REQUIRE(sm_commit(s) == 0);
        REQUIRE(sm_close(s) == 0);
    }

    /* A new session on "db". */
    sm_session_t* open() const
    {
        sm_session_t* s = sm_open(m_home.env(), "db", nullptr);
        REQUIRE(s);
        return s;
    }

  private:
    home_t m_home;
};

/* Set key in "t" to val; the result of sm_update. */
int set(sm_session_t* s, const std::string& key, const std::string& val)
{
    return sm_update(s, "t", key.data(), key.size(), val.data(), val.size());
}

/* Delete key from "t"; the result of sm_delete. */
int del(sm_session_t* s, const std::string& key)
{
    return sm_delete(s, "t", key.data(), key.size());
}

/* Drain a cursor into its keys and close it. */
std::vector<std::string> keys_of(sm_scan_t* c)
{
    REQUIRE_MESSAGE(c, sm_errmsg());
    std::vector<std::string> out;
    const char *k, *d;
    size_t kl, dl;
    while (sm_scan_next(c, &k, &kl, &d, &dl) == 0) {
        out.emplace_back(k, kl);
    }
    sm_scan_end(c);
    return out;
}

} // namespace

TEST_CASE("rollback restores the values as of the savepoint, at every level")
{
    // Verify rollback undoes inserts, updates, and deletes after the mark.
    // 1. Before the mark: update "a" to "1", insert "b".
    // 2. Set the savepoint.
    // 3. After it: update "a" twice, delete "b", insert "c",
    //    re-insert "b".
    // 4. Roll back: "a" is "1", "b" is "1", "c" is absent.
    // 5. Roll back again to the same mark; still the same.
    // 6. Write after the rollback and commit; a new transaction
    //    sees the post-rollback writes and nothing undone.
    for (sm_xctlevel_t level :
         {SM_XCT_READ_COMMITTED, SM_XCT_REPEATABLE_READ, SM_XCT_SERIALIZABLE}) {
        CAPTURE(level);
        sp_fixture_t f;
        sm_session_t* s = f.open();
        REQUIRE(sm_begin(s, level) == 0);
        REQUIRE(set(s, "a", "1") == 0);
        put(s, "t", "b", "1");
        REQUIRE(sm_savepoint(s) == 0);

        REQUIRE(set(s, "a", "2") == 0);
        REQUIRE(set(s, "a", "3") == 0);
        REQUIRE(del(s, "b") == 0);
        put(s, "t", "c", "2");
        put(s, "t", "b", "2");
        CHECK(get(s, "t", "a") == "3");

        for (int round = 0; round < 2; round++) {
            CAPTURE(round);
            REQUIRE(sm_rollback(s) == 0);
            CHECK(get(s, "t", "a") == "1");
            CHECK(get(s, "t", "b") == "1");
            CHECK(get(s, "t", "c") == "<none>");
            CHECK(keys_of(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0)) ==
                  std::vector<std::string>{"a", "b"});
        }

        REQUIRE(set(s, "a", "4") == 0);
        put(s, "t", "d", "4");
        REQUIRE(sm_commit(s) == 0);
        REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
        CHECK(get(s, "t", "a") == "4");
        CHECK(get(s, "t", "b") == "1");
        CHECK(get(s, "t", "c") == "<none>");
        CHECK(get(s, "t", "d") == "4");
        REQUIRE(sm_commit(s) == 0);
        sm_close(s);
    }
}

TEST_CASE(
    "rollback needs a savepoint, and a savepoint lives only in its transaction")
{
    // Verify SM_ERR_NOSAVEPOINT and the savepoint's lifetime.
    // 1. Rollback without a savepoint fails with SM_ERR_NOSAVEPOINT.
    // 2. Savepoint, commit; in the next transaction rollback
    //    fails again: commit discarded the savepoint.
    // 3. A second savepoint replaces the first: rollback returns
    //    to the later mark.
    // 4. Savepoint right after begin rolls back to the start.
    sp_fixture_t f;
    sm_session_t* s = f.open();
    CHECK(sm_savepoint(s) == -1);
    CHECK(sm_errno() == SM_ERR_NOTRANS);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(sm_rollback(s) == -1);
    CHECK(sm_errno() == SM_ERR_NOSAVEPOINT);
    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(sm_rollback(s) == -1);
    CHECK(sm_errno() == SM_ERR_NOSAVEPOINT);

    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(set(s, "a", "1") == 0);
    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(set(s, "a", "2") == 0);
    REQUIRE(sm_rollback(s) == 0);
    CHECK(get(s, "t", "a") == "1");
    REQUIRE(sm_abort(s) == 0);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(set(s, "a", "1") == 0);
    put(s, "t", "b", "1");
    REQUIRE(sm_rollback(s) == 0);
    CHECK(get(s, "t", "a") == "0");
    CHECK(get(s, "t", "b") == "<none>");
    REQUIRE(sm_commit(s) == 0);
    sm_close(s);
}

TEST_CASE(
    "cursors before the mark survive rollback; cursors after it are closed")
{
    // Verify §6.5's cursor rule and the seq rewind.
    // 1. Insert "b"; open cursor C1; set the savepoint.
    // 2. Insert "c"; open cursor C2.
    // 3. Roll back: C2 is closed, C1 stays open.
    // 4. Insert "d" (it reuses the undone seq number).
    // 5. C1 returns exactly "a" and "b": no row written after
    //    the mark, and nothing written after C1 opened.
    sp_fixture_t f;
    sm_session_t* s = f.open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    put(s, "t", "b", "1");
    sm_scan_t* c1 = sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0);
    REQUIRE(c1);
    REQUIRE(sm_savepoint(s) == 0);
    put(s, "t", "c", "2");
    sm_scan_t* c2 = sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0);
    REQUIRE(c2);
    CHECK(s->scans.size() == 2);

    REQUIRE(sm_rollback(s) == 0);
    CHECK(s->scans.size() == 1);
    CHECK(s->scans.count(c1) == 1);
    put(s, "t", "d", "3");
    CHECK(keys_of(c1) == std::vector<std::string>{"a", "b"});
    CHECK(keys_of(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0)) ==
          std::vector<std::string>{"a", "b", "d"});
    REQUIRE(sm_commit(s) == 0);
    sm_close(s);
}

TEST_CASE("rollback releases locks taken after the mark and keeps earlier ones")
{
    // Verify §6.5's lock rule.
    // 1. T locks "a" before the mark; sets the savepoint; then
    //    rewrites "a" and locks "b" by inserting it.
    // 2. T rolls back.
    // 3. U can insert "b" at once: T's lock on it was released.
    // 4. U's update of "a" blocks: T still holds that lock.
    // 5. T commits; U's update then fails with SM_ERR_CONFLICT.
    sp_fixture_t f;
    sm_session_t* t = f.open();
    sm_session_t* u = f.open();
    REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(set(t, "a", "1") == 0);
    REQUIRE(sm_savepoint(t) == 0);
    REQUIRE(set(t, "a", "2") == 0);
    put(t, "t", "b", "t");
    REQUIRE(sm_rollback(t) == 0);

    put(u, "t", "b", "u");
    std::atomic<bool> returned{false};
    int rc = 0, err = 0;
    std::thread tu([&] {
        rc = set(u, "a", "u");
        err = sm_errno();
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_FALSE(returned);
    REQUIRE(sm_commit(t) == 0);
    tu.join();
    CHECK(rc == -1);
    CHECK(err == SM_ERR_CONFLICT);
    sm_close(t);
    sm_close(u);
}

TEST_CASE("rollback undoes sm_mkrel and sm_rmrel made after the mark")
{
    // Verify DDL follows the savepoint (§6.5, §11).
    // 1. Create "p" before the mark; set the savepoint.
    // 2. Create "q" and write to it; drop "t".
    // 3. Roll back: "p" exists, "q" does not, "t" is back with
    //    its rows.
    // 4. Commit; a new transaction sees the same.
    sp_fixture_t f;
    sm_session_t* s = f.open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "p") == 0);
    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(sm_mkrel(s, "q") == 0);
    put(s, "q", "k", "v");
    REQUIRE(sm_rmrel(s, "t") == 0);
    REQUIRE(sm_rollback(s) == 0);

    for (int pass = 0; pass < 2; pass++) {
        CAPTURE(pass);
        const char* d;
        size_t dl;
        CHECK(sm_read(s, "p", "k", 1, &d, &dl) == 1);
        CHECK(sm_read(s, "q", "k", 1, &d, &dl) == -1);
        CHECK(sm_errno() == SM_ERR_NOTFOUND);
        CHECK(get(s, "t", "a") == "0");
        REQUIRE(sm_commit(s) == 0);
        REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    }
    REQUIRE(sm_mkrel(s, "q") == 0); // the name is free again
    REQUIRE(sm_commit(s) == 0);
    sm_close(s);
}
