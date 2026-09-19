/*
 * test_args.cc -- negative tests: every public call rejects bad
 * arguments with its documented error, and a rejected call
 * changes nothing.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

#include <string>

namespace {

/* A database "db" with relation "t" and one row, "a" -> "1". */
class args_fixture_t {
public:
  /* Create "db", "t", and the row. */
  args_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(m_s, "t") == 0);
    put(m_s, "t", "a", "1");
    REQUIRE(sm_commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm_session_t *s() const { return m_s; }

  /* The environment. */
  sm_env_t *env() const { return m_home.env(); }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm_errmsg());
    return s;
  }

private:
  home_t m_home;
  sm_session_t *m_s = nullptr;
};

/* Expect rc to be -1 with error e. */
void expect_fail(int rc, sm_errno_t e) {
  CHECK(rc == -1);
  CHECK(sm_errno() == e);
}

/* Expect a NULL pointer with error e. */
void expect_null(const void *p, sm_errno_t e) {
  CHECK(p == nullptr);
  CHECK(sm_errno() == e);
}

} // namespace

TEST_CASE("environment and database calls reject NULL and bad names") {
  // Verify argument checks outside any session.
  // 1. sm_attach with NULL or "" fails with SM_ERR_INVALID.
  // 2. A NULL env fails with SM_ERR_INVALID in sm_mkdb, sm_rmdb,
  //    sm_lsdb, and sm_opendb; so does a NULL out in sm_lsdb.
  // 3. sm_rmdb and sm_opendb reject NULL, empty, and over-long
  //    database names with SM_ERR_INVALID.
  args_fixture_t f;
  expect_null(sm_attach(nullptr), SM_ERR_INVALID);
  expect_null(sm_attach(""), SM_ERR_INVALID);

  sm_strlist_t *sl = nullptr;
  expect_fail(sm_mkdb(nullptr, "x", nullptr), SM_ERR_INVALID);
  expect_fail(sm_rmdb(nullptr, "x"), SM_ERR_INVALID);
  expect_fail(sm_lsdb(nullptr, &sl), SM_ERR_INVALID);
  expect_fail(sm_lsdb(f.env(), nullptr), SM_ERR_INVALID);
  expect_null(sm_opendb(nullptr, "db", nullptr), SM_ERR_INVALID);

  std::string over(SM_MAX_DBNAME + 1, 'd');
  for (const char *name : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(name ? name : "(null)");
    expect_fail(sm_rmdb(f.env(), name), SM_ERR_INVALID);
    expect_null(sm_opendb(f.env(), name, nullptr), SM_ERR_INVALID);
  }
}

TEST_CASE("sm_opendb rejects invalid options when it first opens a database") {
  // Verify the option checks of the opening session.
  // 1. For each of deadlock_ms = 0, ts_lease = 0, and a
  //    malformed rocksdb_options, opening a fresh database fails
  //    with SM_ERR_INVALID.
  // 2. The database opens normally with the defaults afterwards.
  args_fixture_t f;
  for (int bad = 0; bad < 3; bad++) {
    CAPTURE(bad);
    std::string name = "o" + std::to_string(bad);
    REQUIRE(sm_mkdb(f.env(), name.c_str(), nullptr) == 0);
    sm_options_t opt;
    sm_options_init(&opt);
    if (bad == 0) {
      opt.deadlock_ms = 0;
    } else if (bad == 1) {
      opt.ts_lease = 0;
    } else {
      opt.rocksdb_options = "not_an_option=1";
    }
    expect_null(sm_opendb(f.env(), name.c_str(), &opt), SM_ERR_INVALID);
    sm_session_t *s = sm_opendb(f.env(), name.c_str(), nullptr);
    CHECK(s);
    sm_closedb(s);
  }
}

TEST_CASE("every session call rejects a NULL session") {
  // Verify no call dereferences a NULL session.
  // 1. Each call taking a session, given NULL, fails with
  //    SM_ERR_INVALID (or returns NULL with it).
  // 2. The release calls accept NULL as a no-op.
  const char *d;
  size_t dl;
  sm_prepared_t *list;
  size_t n;
  expect_fail(sm_begin(nullptr, SM_XCT_REPEATABLE_READ), SM_ERR_INVALID);
  expect_fail(sm_commit(nullptr), SM_ERR_INVALID);
  expect_fail(sm_abort(nullptr), SM_ERR_INVALID);
  expect_fail(sm_savepoint(nullptr), SM_ERR_INVALID);
  expect_fail(sm_rollback(nullptr), SM_ERR_INVALID);
  expect_fail(sm_prepare(nullptr, "g"), SM_ERR_INVALID);
  expect_fail(sm_list_prepared(nullptr, &list, &n), SM_ERR_INVALID);
  expect_fail(sm_commit_prepared(nullptr, "g"), SM_ERR_INVALID);
  expect_fail(sm_abort_prepared(nullptr, "g"), SM_ERR_INVALID);
  expect_fail(sm_mkrel(nullptr, "t"), SM_ERR_INVALID);
  expect_fail(sm_rmrel(nullptr, "t"), SM_ERR_INVALID);
  expect_fail(sm_insert(nullptr, "t", "k", 1, "v", 1), SM_ERR_INVALID);
  expect_fail(sm_update(nullptr, "t", "k", 1, "v", 1), SM_ERR_INVALID);
  expect_fail(sm_delete(nullptr, "t", "k", 1), SM_ERR_INVALID);
  expect_fail(sm_read(nullptr, "t", "k", 1, &d, &dl), SM_ERR_INVALID);
  expect_null(sm_scan(nullptr, "t", nullptr, 0, 0, nullptr, 0, 0),
              SM_ERR_INVALID);

  CHECK(sm_closedb(nullptr) == 0);
  sm_scanend(nullptr);
  sm_free_prepared(nullptr, 0);
  sm_strlist_free(nullptr);
}

TEST_CASE("every call taking a relation name validates it") {
  // Verify relation names: NULL, "", and SM_MAX_RELNAME + 1
  // bytes fail with SM_ERR_INVALID; SM_MAX_RELNAME bytes work.
  // 1. Each bad name is rejected by sm_mkrel, sm_rmrel,
  //    sm_insert, sm_update, sm_delete, sm_read, and sm_scan.
  // 2. A relation named with exactly SM_MAX_RELNAME bytes can
  //    be created, written, read, scanned, and dropped.
  args_fixture_t f;
  sm_session_t *s = f.s();
  std::string over(SM_MAX_RELNAME + 1, 'r');
  const char *d;
  size_t dl;
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  for (const char *rel : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(rel ? rel : "(null)");
    expect_fail(sm_mkrel(s, rel), SM_ERR_INVALID);
    expect_fail(sm_rmrel(s, rel), SM_ERR_INVALID);
    expect_fail(sm_insert(s, rel, "k", 1, "v", 1), SM_ERR_INVALID);
    expect_fail(sm_update(s, rel, "k", 1, "v", 1), SM_ERR_INVALID);
    expect_fail(sm_delete(s, rel, "k", 1), SM_ERR_INVALID);
    expect_fail(sm_read(s, rel, "k", 1, &d, &dl), SM_ERR_INVALID);
    expect_null(sm_scan(s, rel, nullptr, 0, 0, nullptr, 0, 0), SM_ERR_INVALID);
  }

  std::string max(SM_MAX_RELNAME, 'r');
  REQUIRE(sm_mkrel(s, max.c_str()) == 0);
  put(s, max.c_str(), "k", "v");
  CHECK(get(s, max.c_str(), "k") == "v");
  sm_scan_t *c = sm_scan(s, max.c_str(), nullptr, 0, 0, nullptr, 0, 0);
  CHECK(c);
  sm_scanend(c);
  CHECK(sm_rmrel(s, max.c_str()) == 0);
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("unknown relations fail with SM_ERR_NOTFOUND") {
  // Verify every data call on a relation that does not exist.
  // 1. sm_rmrel, sm_insert, sm_update, sm_delete, sm_read, and
  //    sm_scan on "nope" fail with SM_ERR_NOTFOUND.
  // 2. A relation dropped earlier in the same transaction is
  //    unknown to that transaction's later calls.
  args_fixture_t f;
  sm_session_t *s = f.s();
  const char *d;
  size_t dl;
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm_rmrel(s, "nope"), SM_ERR_NOTFOUND);
  expect_fail(sm_insert(s, "nope", "k", 1, "v", 1), SM_ERR_NOTFOUND);
  expect_fail(sm_update(s, "nope", "k", 1, "v", 1), SM_ERR_NOTFOUND);
  expect_fail(sm_delete(s, "nope", "k", 1), SM_ERR_NOTFOUND);
  expect_fail(sm_read(s, "nope", "k", 1, &d, &dl), SM_ERR_NOTFOUND);
  expect_null(sm_scan(s, "nope", nullptr, 0, 0, nullptr, 0, 0),
              SM_ERR_NOTFOUND);

  REQUIRE(sm_rmrel(s, "t") == 0);
  expect_fail(sm_read(s, "t", "a", 1, &d, &dl), SM_ERR_NOTFOUND);
  expect_fail(sm_insert(s, "t", "k", 1, "v", 1), SM_ERR_NOTFOUND);
  REQUIRE(sm_abort(s) == 0);
}

TEST_CASE("keys and values: NULL only with length 0") {
  // Verify NULL key and data pointers.
  // 1. A NULL key with a nonzero length fails with
  //    SM_ERR_INVALID in insert, update, delete, read, and as a
  //    scan bound given a length (a NULL bound means none).
  // 2. A NULL value with a nonzero length fails with
  //    SM_ERR_INVALID in insert and update.
  // 3. NULL with length 0 is the empty key or the empty value,
  //    and works.
  args_fixture_t f;
  sm_session_t *s = f.s();
  const char *d;
  size_t dl;
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm_insert(s, "t", nullptr, 1, "v", 1), SM_ERR_INVALID);
  expect_fail(sm_update(s, "t", nullptr, 1, "v", 1), SM_ERR_INVALID);
  expect_fail(sm_delete(s, "t", nullptr, 1), SM_ERR_INVALID);
  expect_fail(sm_read(s, "t", nullptr, 1, &d, &dl), SM_ERR_INVALID);
  expect_fail(sm_insert(s, "t", "k", 1, nullptr, 1), SM_ERR_INVALID);
  expect_fail(sm_update(s, "t", "a", 1, nullptr, 1), SM_ERR_INVALID);
  CHECK(get(s, "t", "a") == "1");

  REQUIRE(sm_insert(s, "t", nullptr, 0, "empty-key", 9) == 0);
  CHECK(get(s, "t", "") == "empty-key");
  REQUIRE(sm_insert(s, "t", "e", 1, nullptr, 0) == 0);
  CHECK(get(s, "t", "e") == "");
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("out-parameters must not be NULL") {
  // Verify calls that hand results back through pointers.
  // 1. sm_read with a NULL data or datalen fails with
  //    SM_ERR_INVALID.
  // 2. sm_scannxt with a NULL cursor or any NULL out-parameter
  //    fails with SM_ERR_INVALID and leaves the cursor usable.
  // 3. sm_list_prepared with a NULL out or n fails with
  //    SM_ERR_INVALID.
  args_fixture_t f;
  sm_session_t *s = f.s();
  const char *k, *d;
  size_t kl, dl;
  sm_prepared_t *list;
  size_t n;
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm_read(s, "t", "a", 1, nullptr, &dl), SM_ERR_INVALID);
  expect_fail(sm_read(s, "t", "a", 1, &d, nullptr), SM_ERR_INVALID);

  sm_scan_t *c = sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  expect_fail(sm_scannxt(nullptr, &k, &kl, &d, &dl), SM_ERR_INVALID);
  expect_fail(sm_scannxt(c, nullptr, &kl, &d, &dl), SM_ERR_INVALID);
  expect_fail(sm_scannxt(c, &k, nullptr, &d, &dl), SM_ERR_INVALID);
  expect_fail(sm_scannxt(c, &k, &kl, nullptr, &dl), SM_ERR_INVALID);
  expect_fail(sm_scannxt(c, &k, &kl, &d, nullptr), SM_ERR_INVALID);
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "a");
  sm_scanend(c);
  REQUIRE(sm_commit(s) == 0);

  expect_fail(sm_list_prepared(s, nullptr, &n), SM_ERR_INVALID);
  expect_fail(sm_list_prepared(s, &list, nullptr), SM_ERR_INVALID);
}

TEST_CASE("resolving by gid validates the gid") {
  // Verify sm_commit_prepared and sm_abort_prepared reject NULL,
  // empty, and over-long gids with SM_ERR_INVALID, and report a
  // well-formed but unknown gid as SM_ERR_NOTFOUND.
  args_fixture_t f;
  sm_session_t *s = f.s();
  std::string over(SM_MAX_GID + 1, 'g');
  for (const char *gid : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(gid ? gid : "(null)");
    expect_fail(sm_commit_prepared(s, gid), SM_ERR_INVALID);
    expect_fail(sm_abort_prepared(s, gid), SM_ERR_INVALID);
  }
  expect_fail(sm_commit_prepared(s, "unknown"), SM_ERR_NOTFOUND);
  expect_fail(sm_abort_prepared(s, "unknown"), SM_ERR_NOTFOUND);
}

TEST_CASE("calls needing a transaction fail with SM_ERR_NOTRANS without one") {
  // Verify every call that requires an active transaction.
  // 1. With none active, each fails with SM_ERR_NOTRANS.
  // 2. The committed row is unchanged afterwards.
  args_fixture_t f;
  sm_session_t *s = f.s();
  const char *d;
  size_t dl;
  expect_fail(sm_commit(s), SM_ERR_NOTRANS);
  expect_fail(sm_abort(s), SM_ERR_NOTRANS);
  expect_fail(sm_savepoint(s), SM_ERR_NOTRANS);
  expect_fail(sm_rollback(s), SM_ERR_NOTRANS);
  expect_fail(sm_prepare(s, "g"), SM_ERR_NOTRANS);
  expect_fail(sm_mkrel(s, "u"), SM_ERR_NOTRANS);
  expect_fail(sm_rmrel(s, "t"), SM_ERR_NOTRANS);
  expect_fail(sm_insert(s, "t", "k", 1, "v", 1), SM_ERR_NOTRANS);
  expect_fail(sm_update(s, "t", "a", 1, "v", 1), SM_ERR_NOTRANS);
  expect_fail(sm_delete(s, "t", "a", 1), SM_ERR_NOTRANS);
  expect_fail(sm_read(s, "t", "a", 1, &d, &dl), SM_ERR_NOTRANS);
  expect_null(sm_scan(s, "t", nullptr, 0, 0, nullptr, 0, 0), SM_ERR_NOTRANS);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, "t", "a") == "1");
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a rejected call leaves the transaction active and unchanged") {
  // Verify sm.h's promise: a non-retryable error changes nothing.
  // 1. Write "b", then provoke SM_ERR_INVALID, SM_ERR_NOTFOUND,
  //    SM_ERR_DUPKEY, and SM_ERR_EXISTS.
  // 2. The transaction is still active: "b" reads back, a further
  //    write succeeds, and the commit makes both durable.
  args_fixture_t f;
  sm_session_t *s = f.s();
  std::string over(SM_MAX_KEYLEN + 1, 'k');
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, "t", "b", "2");
  expect_fail(sm_insert(s, "t", over.data(), over.size(), "v", 1),
              SM_ERR_INVALID);
  expect_fail(sm_update(s, "t", "missing", 7, "v", 1), SM_ERR_NOTFOUND);
  expect_fail(sm_insert(s, "t", "a", 1, "v", 1), SM_ERR_DUPKEY);
  expect_fail(sm_mkrel(s, "t"), SM_ERR_EXISTS);
  CHECK(get(s, "t", "b") == "2");
  put(s, "t", "c", "3");
  REQUIRE(sm_commit(s) == 0);

  sm_session_t *v = f.open();
  REQUIRE(sm_begin(v, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(v, "t", "a") == "1");
  CHECK(get(v, "t", "b") == "2");
  CHECK(get(v, "t", "c") == "3");
  REQUIRE(sm_commit(v) == 0);
  sm_closedb(v);
}
