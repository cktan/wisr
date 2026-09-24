/*
 * test_args.cc -- negative tests: every public call rejects bad
 * arguments with its documented error, and a rejected call
 * changes nothing.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <string>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store and one row, "a" -> "1". */
class args_fixture_t {
public:
  /* Create "db", the store, and the row. */
  args_fixture_t() {
    REQUIRE(sm1_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
    REQUIRE(sm1_begin(m_s, SM1_XCT_REPEATABLE_READ) == 0);
    g_store = mkstore(m_s);
    put(m_s, g_store, "a", "1");
    REQUIRE(sm1_commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm1_session_t *s() const { return m_s; }

  /* The environment. */
  sm1_env_t *env() const { return m_home.env(); }

  /* A new session on "db". */
  sm1_session_t *open() const {
    sm1_session_t *s = sm1_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

private:
  home_t m_home;
  sm1_session_t *m_s = nullptr;
};

/* Expect rc to be -1 with error e. */
void expect_fail(int rc, sm1::errnum_t e) {
  CHECK(rc == -1);
  CHECK(sm1::errnum() == e);
}

/* Expect a NULL pointer with error e. */
void expect_null(const void *p, sm1::errnum_t e) {
  CHECK(p == nullptr);
  CHECK(sm1::errnum() == e);
}

} // namespace

TEST_CASE("environment and database calls reject NULL and bad names") {
  // Verify argument checks outside any session.
  // 1. sm1_attach with NULL or "" fails with SM_ERR_INVALID.
  // 2. A NULL env fails with SM_ERR_INVALID in sm1_mkdb, sm1_rmdb,
  //    sm1_lsdb, and sm1_opendb; so does a NULL out in sm1_lsdb.
  // 3. sm1_rmdb and sm1_opendb reject NULL, empty, and over-long
  //    database names with SM_ERR_INVALID.
  args_fixture_t f;
  expect_null(sm1_attach(nullptr), sm1::SM_ERR_INVALID);
  expect_null(sm1_attach(""), sm1::SM_ERR_INVALID);

  sm1_strlist_t *sl = nullptr;
  expect_fail(sm1_mkdb(nullptr, "x", nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_rmdb(nullptr, "x"), sm1::SM_ERR_INVALID);
  expect_fail(sm1_lsdb(nullptr, &sl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_lsdb(f.env(), nullptr), sm1::SM_ERR_INVALID);
  expect_null(sm1_opendb(nullptr, "db", nullptr), sm1::SM_ERR_INVALID);

  std::string over(SM1_MAX_DBNAME + 1, 'd');
  for (const char *name : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(name ? name : "(null)");
    expect_fail(sm1_rmdb(f.env(), name), sm1::SM_ERR_INVALID);
    expect_null(sm1_opendb(f.env(), name, nullptr), sm1::SM_ERR_INVALID);
  }
}

TEST_CASE("sm1_opendb rejects invalid options when it first opens a database") {
  // Verify the option checks of the opening session.
  // 1. For each of deadlock_ms = 0, ts_lease = 0, and a
  //    malformed rocksdb_options, opening a fresh database fails
  //    with SM_ERR_INVALID.
  // 2. The database opens normally with the defaults afterwards.
  args_fixture_t f;
  for (int bad = 0; bad < 3; bad++) {
    CAPTURE(bad);
    std::string name = "o" + std::to_string(bad);
    REQUIRE(sm1_mkdb(f.env(), name.c_str(), nullptr) == 0);
    sm1_options_t opt;
    sm1_options_init(&opt);
    if (bad == 0) {
      opt.deadlock_ms = 0;
    } else if (bad == 1) {
      opt.ts_lease = 0;
    } else {
      opt.rocksdb_options = "not_an_option=1";
    }
    expect_null(sm1_opendb(f.env(), name.c_str(), &opt), sm1::SM_ERR_INVALID);
    sm1_session_t *s = sm1_opendb(f.env(), name.c_str(), nullptr);
    CHECK(s);
    sm1_closedb(s);
  }
}

TEST_CASE("every session call rejects a NULL session") {
  // Verify no call dereferences a NULL session.
  // 1. Each call taking a session, given NULL, fails with
  //    SM_ERR_INVALID (or returns NULL with it).
  // 2. The release calls accept NULL as a no-op.
  const char *d;
  size_t dl;
  sm1_prepared_t *list;
  size_t n;
  expect_fail(sm1_begin(nullptr, SM1_XCT_REPEATABLE_READ), sm1::SM_ERR_INVALID);
  expect_fail(sm1_commit(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_abort(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_savepoint(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_rollback(nullptr, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_forget(nullptr, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_lock(nullptr, 1, "k", 1, SM1_LOCK_SHARED),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_prepare(nullptr, "g"), sm1::SM_ERR_INVALID);
  expect_fail(sm1_list_prepared(nullptr, &list, &n), sm1::SM_ERR_INVALID);
  expect_fail(sm1_commit_prepared(nullptr, "g"), sm1::SM_ERR_INVALID);
  expect_fail(sm1_abort_prepared(nullptr, "g"), sm1::SM_ERR_INVALID);
  uint64_t storeid;
  expect_fail(sm1_mkstore(nullptr, &storeid), sm1::SM_ERR_INVALID);
  expect_fail(sm1_rmstore(nullptr, g_store), sm1::SM_ERR_INVALID);
  expect_fail(sm1_insert(nullptr, g_store, "k", 1, "v", 1),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_update(nullptr, g_store, "k", 1, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_delete(nullptr, g_store, "k", 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_read(nullptr, g_store, "k", 1, &d, &dl), sm1::SM_ERR_INVALID);
  expect_null(sm1_scan(nullptr, g_store, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_INVALID);
  uint64_t *stores;
  expect_fail(sm1_lsstore(nullptr, &stores, &n), sm1::SM_ERR_INVALID);
  uint64_t v;
  expect_fail(sm1_mkseq(nullptr, "s", 1, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_rmseq(nullptr, "s"), sm1::SM_ERR_INVALID);
  expect_fail(sm1_nextseq(nullptr, "s", &v), sm1::SM_ERR_INVALID);

  CHECK(sm1_closedb(nullptr) == 0);
  sm1_scanend(nullptr);
  sm1_free_prepared(nullptr, 0);
}

TEST_CASE("storeid 0 is rejected by every call taking a storeid") {
  // Verify no call treats storeid 0 -- the catalog's slot -- as a
  // store, and that sm1_mkstore needs somewhere to put its
  // storeid.
  // 1. sm1_rmstore, sm1_insert, sm1_update, sm1_delete, sm1_read, and
  //    sm1_scan with storeid 0 fail with SM_ERR_INVALID, as does
  //    sm1_mkstore with a NULL out-parameter.
  // 2. The transaction is undisturbed: the store still reads.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  const char *d;
  size_t dl;
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm1_mkstore(s, nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_rmstore(s, 0), sm1::SM_ERR_INVALID);
  expect_fail(sm1_insert(s, 0, "k", 1, "v", 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_update(s, 0, "k", 1, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_delete(s, 0, "k", 1, nullptr, nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1_read(s, 0, "k", 1, &d, &dl), sm1::SM_ERR_INVALID);
  expect_null(sm1_scan(s, 0, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_INVALID);

  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1_commit(s) == 0);
}

TEST_CASE("sm1_mkstore hands out a fresh storeid every time") {
  // Verify storeids rise and are never reused (SM1_DESIGN.md §11).
  // 1. Three stores in one transaction get three ascending
  //    storeids, all above the fixture's.
  // 2. Dropping one and creating another gives a storeid above
  //    every one handed out so far.
  // 3. Each new store is usable at once and commits.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  uint64_t a = mkstore(s), b = mkstore(s), c = mkstore(s);
  CHECK(g_store < a);
  CHECK(a < b);
  CHECK(b < c);

  REQUIRE(sm1_rmstore(s, b) == 0);
  CHECK(mkstore(s) > c);

  put(s, a, "k", "va");
  put(s, c, "k", "vc");
  REQUIRE(sm1_commit(s) == 0);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, a, "k") == "va");
  CHECK(get(s, c, "k") == "vc");
  REQUIRE(sm1_commit(s) == 0);
}

TEST_CASE("unknown stores fail with SM_ERR_NOTFOUND") {
  // Verify every data call on a store that does not exist.
  // 1. sm1_rmstore, sm1_insert, sm1_update, sm1_delete, sm1_read, and
  //    sm1_scan on store 999 fail with SM_ERR_NOTFOUND.
  // 2. A store dropped earlier in the same transaction is
  //    unknown to that transaction's later calls.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  const char *d;
  size_t dl;
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm1_rmstore(s, 999), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_insert(s, 999, "k", 1, "v", 1), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_update(s, 999, "k", 1, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_delete(s, 999, "k", 1, nullptr, nullptr),
              sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_read(s, 999, "k", 1, &d, &dl), sm1::SM_ERR_NOTFOUND);
  expect_null(sm1_scan(s, 999, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm1_rmstore(s, g_store) == 0);
  expect_fail(sm1_read(s, g_store, "a", 1, &d, &dl), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_insert(s, g_store, "k", 1, "v", 1), sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1_abort(s) == 0);
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
  sm1_session_t *s = f.s();
  const char *d;
  size_t dl;
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm1_insert(s, g_store, nullptr, 1, "v", 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_update(s, g_store, nullptr, 1, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_delete(s, g_store, nullptr, 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_read(s, g_store, nullptr, 1, &d, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_insert(s, g_store, "k", 1, nullptr, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1_update(s, g_store, "a", 1, nullptr, 1, nullptr, nullptr),
              sm1::SM_ERR_INVALID);
  CHECK(get(s, g_store, "a") == "1");

  REQUIRE(sm1_insert(s, g_store, nullptr, 0, "empty-key", 9) == 0);
  CHECK(get(s, g_store, "") == "empty-key");
  REQUIRE(sm1_insert(s, g_store, "e", 1, nullptr, 0) == 0);
  CHECK(get(s, g_store, "e") == "");
  REQUIRE(sm1_commit(s) == 0);
}

TEST_CASE("out-parameters must not be NULL") {
  // Verify calls that hand results back through pointers.
  // 1. sm1_read with a NULL data or datalen fails with
  //    SM_ERR_INVALID.
  // 2. sm1_scannxt with a NULL cursor or any NULL out-parameter
  //    fails with SM_ERR_INVALID and leaves the cursor usable.
  // 3. sm1_list_prepared with a NULL out or n fails with
  //    SM_ERR_INVALID.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  const char *k, *d;
  size_t kl, dl;
  sm1_prepared_t *list;
  size_t n;
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  expect_fail(sm1_read(s, g_store, "a", 1, nullptr, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_read(s, g_store, "a", 1, &d, nullptr), sm1::SM_ERR_INVALID);

  sm1_scan_t *c = sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  expect_fail(sm1_scannxt(nullptr, &k, &kl, &d, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_scannxt(c, nullptr, &kl, &d, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_scannxt(c, &k, nullptr, &d, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_scannxt(c, &k, &kl, nullptr, &dl), sm1::SM_ERR_INVALID);
  expect_fail(sm1_scannxt(c, &k, &kl, &d, nullptr), sm1::SM_ERR_INVALID);
  REQUIRE(sm1_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "a");
  sm1_scanend(c);
  REQUIRE(sm1_commit(s) == 0);

  expect_fail(sm1_list_prepared(s, nullptr, &n), sm1::SM_ERR_INVALID);
  expect_fail(sm1_list_prepared(s, &list, nullptr), sm1::SM_ERR_INVALID);
}

TEST_CASE("resolving by gid validates the gid") {
  // Verify sm1_commit_prepared and sm1_abort_prepared reject NULL,
  // empty, and over-long gids with SM_ERR_INVALID, and report a
  // well-formed but unknown gid as SM_ERR_NOTFOUND.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  std::string over(SM1_MAX_GID + 1, 'g');
  for (const char *gid : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(gid ? gid : "(null)");
    expect_fail(sm1_commit_prepared(s, gid), sm1::SM_ERR_INVALID);
    expect_fail(sm1_abort_prepared(s, gid), sm1::SM_ERR_INVALID);
  }
  expect_fail(sm1_commit_prepared(s, "unknown"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_abort_prepared(s, "unknown"), sm1::SM_ERR_NOTFOUND);
}

TEST_CASE("calls needing a transaction fail with SM_ERR_NOTRANS without one") {
  // Verify every call that requires an active transaction.
  // 1. With none active, each fails with SM_ERR_NOTRANS.
  // 2. The committed row is unchanged afterwards.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  const char *d;
  size_t dl;
  expect_fail(sm1_commit(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_abort(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_savepoint(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_rollback(s, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_forget(s, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_lock(s, 1, "k", 1, SM1_LOCK_SHARED), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_prepare(s, "g"), sm1::SM_ERR_NOTRANS);
  uint64_t storeid;
  expect_fail(sm1_mkstore(s, &storeid), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_mkseq(s, "s", 1, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_rmseq(s, "s"), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_rmstore(s, g_store), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_insert(s, g_store, "k", 1, "v", 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_update(s, g_store, "a", 1, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_delete(s, g_store, "a", 1, nullptr, nullptr),
              sm1::SM_ERR_NOTRANS);
  expect_fail(sm1_read(s, g_store, "a", 1, &d, &dl), sm1::SM_ERR_NOTRANS);
  expect_null(sm1_scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1_commit(s) == 0);
}

TEST_CASE("a rejected call leaves the transaction active and unchanged") {
  // Verify sm1.h's promise: a non-retryable error changes nothing.
  // 1. Write "b", then provoke SM_ERR_INVALID, SM_ERR_NOTFOUND,
  //    and SM_ERR_DUPKEY.
  // 2. The transaction is still active: "b" reads back, a further
  //    write succeeds, and the commit makes both durable.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  std::string over(SM1_MAX_KEYLEN + 1, 'k');
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  put(s, g_store, "b", "2");
  expect_fail(sm1_insert(s, g_store, over.data(), over.size(), "v", 1),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1_update(s, g_store, "missing", 7, "v", 1, nullptr, nullptr),
              sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1_insert(s, g_store, "a", 1, "v", 1), sm1::SM_ERR_DUPKEY);
  CHECK(get(s, g_store, "b") == "2");
  put(s, g_store, "c", "3");
  REQUIRE(sm1_commit(s) == 0);

  sm1_session_t *v = f.open();
  REQUIRE(sm1_begin(v, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(get(v, g_store, "a") == "1");
  CHECK(get(v, g_store, "b") == "2");
  CHECK(get(v, g_store, "c") == "3");
  REQUIRE(sm1_commit(v) == 0);
  sm1_closedb(v);
}

TEST_CASE("every sequence call validates its name") {
  // Verify sequence names: NULL, "", and SM1_MAX_SEQNAME + 1
  // bytes fail with SM_ERR_INVALID; SM1_MAX_SEQNAME bytes work.
  // 1. Each bad name is rejected by sm1_mkseq, sm1_rmseq, and
  //    sm1_nextseq.
  // 2. A sequence named with exactly SM1_MAX_SEQNAME bytes can
  //    be created, drawn from, and dropped.
  args_fixture_t f;
  sm1_session_t *s = f.s();
  std::string over(SM1_MAX_SEQNAME + 1, 's');
  uint64_t v = 0;
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  for (const char *name : {(const char *)nullptr, "", over.c_str()}) {
    CAPTURE(name ? name : "(null)");
    expect_fail(sm1_mkseq(s, name, 1, 1), sm1::SM_ERR_INVALID);
    expect_fail(sm1_rmseq(s, name), sm1::SM_ERR_INVALID);
    expect_fail(sm1_nextseq(s, name, &v), sm1::SM_ERR_INVALID);
  }

  std::string max(SM1_MAX_SEQNAME, 's');
  REQUIRE(sm1_mkseq(s, max.c_str(), 7, 2) == 0);
  REQUIRE(sm1_nextseq(s, max.c_str(), &v) == 0);
  CHECK(v == 7);
  CHECK(sm1_rmseq(s, max.c_str()) == 0);
  REQUIRE(sm1_commit(s) == 0);
}
