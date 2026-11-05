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
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
    REQUIRE(sm1::begin(m_s, sm1::xctlevel_t::repeatable_read) == 0);
    g_store = mkstore(m_s);
    put(m_s, g_store, "a", "1");
    REQUIRE(sm1::commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm1::session_t *s() const { return m_s; }

  /* The environment. */
  sm1::env_t *env() const { return m_home.env(); }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

private:
  home_t m_home;
  sm1::session_t *m_s = nullptr;
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
  // 1. sm1::attach with a NUL byte in homedir, or "", fails with
  //    SM_ERR_INVALID.
  // 2. A NULL env fails with SM_ERR_INVALID in sm1::mkdb, sm1::rmdb,
  //    sm1::lsdb, and sm1::opendb; so does a NULL out in sm1::lsdb.
  // 3. sm1::rmdb and sm1::opendb reject names holding a NUL byte,
  //    empty names, and over-long ones with SM_ERR_INVALID.
  args_fixture_t f;
  expect_null(attach(std::string_view("a\0b", 3)), sm1::SM_ERR_INVALID);
  expect_null(attach(""), sm1::SM_ERR_INVALID);

  std::vector<std::string> names;
  expect_fail(sm1::mkdb(nullptr, "x", nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1::rmdb(nullptr, "x"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::lsdb(nullptr, names), sm1::SM_ERR_INVALID);
  expect_null(opendb(nullptr, "db", nullptr), sm1::SM_ERR_INVALID);

  std::string over(sm1::max_dbname + 1, 'd');
  for (std::string_view name : {std::string_view("x\0y", 3),
                                std::string_view(""), std::string_view(over)}) {
    CAPTURE(std::string(name));
    expect_fail(sm1::rmdb(f.env(), name), sm1::SM_ERR_INVALID);
    expect_null(opendb(f.env(), name, nullptr), sm1::SM_ERR_INVALID);
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
    REQUIRE(sm1::mkdb(f.env(), name.c_str(), nullptr) == 0);
    sm1::options_t opt;
    if (bad == 0) {
      opt.deadlock_ms = 0;
    } else if (bad == 1) {
      opt.ts_lease = 0;
    } else {
      opt.rocksdb_options = "not_an_option=1";
    }
    expect_null(opendb(f.env(), name.c_str(), &opt), sm1::SM_ERR_INVALID);
    sm1::session_t *s = opendb(f.env(), name.c_str(), nullptr);
    CHECK(s);
    sm1::closedb(s);
  }
}

TEST_CASE("every session call rejects a NULL session") {
  // Verify no call dereferences a NULL session.
  // 1. Each call taking a session, given NULL, fails with
  //    SM_ERR_INVALID (or returns NULL with it).
  // 2. The release calls accept NULL as a no-op.
  std::string_view d;
  std::vector<sm1::prepared_t> list;
  expect_fail(sm1::begin(nullptr, sm1::xctlevel_t::repeatable_read),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1::commit(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1::abort(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1::savepoint(nullptr), sm1::SM_ERR_INVALID);
  expect_fail(sm1::rollback(nullptr, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1::forget(nullptr, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1::lock_row(nullptr, 1, "k", sm1::SM_LOCKMODE_SH),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1::lock_oid(nullptr, 1, sm1::SM_LOCKMODE_SH),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1::prepare(nullptr, "g"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::list_prepared(nullptr, list), sm1::SM_ERR_INVALID);
  expect_fail(sm1::commit_prepared(nullptr, "g"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::abort_prepared(nullptr, "g"), sm1::SM_ERR_INVALID);
  uint64_t storeid;
  expect_fail(sm1::mkstore(nullptr, storeid), sm1::SM_ERR_INVALID);
  expect_fail(sm1::rmstore(nullptr, g_store), sm1::SM_ERR_INVALID);
  expect_fail(sm1::insert(nullptr, g_store, "k", "v"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::update(nullptr, g_store, "k", "v"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::erase(nullptr, g_store, "k"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::read(nullptr, g_store, "k", d), sm1::SM_ERR_INVALID);
  expect_null(scan(nullptr, g_store, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_INVALID);
  std::vector<uint64_t> stores;
  expect_fail(sm1::lsstore(nullptr, stores), sm1::SM_ERR_INVALID);
  uint64_t v;
  expect_fail(sm1::mkseq(nullptr, "s", 1, 1), sm1::SM_ERR_INVALID);
  expect_fail(sm1::rmseq(nullptr, "s"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::nextseq(nullptr, "s", v), sm1::SM_ERR_INVALID);

  CHECK(sm1::closedb(nullptr) == 0);
  sm1::scanend(nullptr);
}

TEST_CASE("storeid 0 is rejected by every call taking a storeid") {
  // Verify no call treats storeid 0 -- the catalog's slot -- as a
  // store, and that sm1::mkstore needs somewhere to put its
  // storeid.
  // 1. sm1::rmstore, sm1::insert, sm1::update, sm1::erase, sm1::read, and
  //    sm1::scan with storeid 0 fail with SM_ERR_INVALID, as does
  //    sm1::mkstore with a NULL out-parameter.
  // 2. The transaction is undisturbed: the store still reads.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string_view d;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  expect_fail(sm1::rmstore(s, 0), sm1::SM_ERR_INVALID);
  expect_fail(sm1::insert(s, 0, "k", "v"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::update(s, 0, "k", "v"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::erase(s, 0, "k"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::read(s, 0, "k", d), sm1::SM_ERR_INVALID);
  expect_null(scan(s, 0, nullptr, 0, 0, nullptr, 0, 0), sm1::SM_ERR_INVALID);

  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("sm1_mkstore hands out a fresh storeid every time") {
  // Verify storeids rise and are never reused (../DBMGR.md §7).
  // 1. Three stores in one transaction get three ascending
  //    storeids, all above the fixture's.
  // 2. Dropping one and creating another gives a storeid above
  //    every one handed out so far.
  // 3. Each new store is usable at once and commits.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t a = mkstore(s), b = mkstore(s), c = mkstore(s);
  CHECK(g_store < a);
  CHECK(a < b);
  CHECK(b < c);

  REQUIRE(sm1::rmstore(s, b) == 0);
  CHECK(mkstore(s) > c);

  put(s, a, "k", "va");
  put(s, c, "k", "vc");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, a, "k") == "va");
  CHECK(get(s, c, "k") == "vc");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("unknown stores fail with SM_ERR_NOTFOUND") {
  // Verify every data call on a store that does not exist.
  // 1. sm1::rmstore, sm1::insert, sm1::update, sm1::erase, sm1::read, and
  //    sm1::scan on store 999 fail with SM_ERR_NOTFOUND.
  // 2. A store dropped earlier in the same transaction is
  //    unknown to that transaction's later calls.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string_view d;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  expect_fail(sm1::rmstore(s, 999), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::insert(s, 999, "k", "v"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::update(s, 999, "k", "v"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::erase(s, 999, "k"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::read(s, 999, "k", d), sm1::SM_ERR_NOTFOUND);
  expect_null(scan(s, 999, nullptr, 0, 0, nullptr, 0, 0), sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm1::rmstore(s, g_store) == 0);
  expect_fail(sm1::read(s, g_store, "a", d), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::insert(s, g_store, "k", "v"), sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(s) == 0);
}

TEST_CASE("keys and values: an empty string_view is the empty key or value") {
  // Verify empty inputs: a default string_view, whose data() is
  // null, is the empty key or the empty value, and works.
  // 1. An empty key inserts and reads back.
  // 2. An empty value inserts and reads back as "".
  args_fixture_t f;
  sm1::session_t *s = f.s();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::insert(s, g_store, std::string_view(), "empty-key") == 0);
  CHECK(get(s, g_store, "") == "empty-key");
  REQUIRE(sm1::insert(s, g_store, "e", std::string_view()) == 0);
  CHECK(get(s, g_store, "e") == "");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a NULL cursor is refused") {
  // Verify the one out-parameter that can still be NULL.
  // 1. sm1::scannxt with a NULL cursor fails with SM_ERR_INVALID,
  //    and a real cursor is usable afterwards.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string_view k, d;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);

  sm1::scan_t *c = scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE(c);
  expect_fail(sm1::scannxt(nullptr, k, d), sm1::SM_ERR_INVALID);
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == "a");
  sm1::scanend(c);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("resolving by gid validates the gid") {
  // Verify sm1::commit_prepared and sm1::abort_prepared reject gids
  // holding a NUL byte, empty gids, and over-long ones with
  // SM_ERR_INVALID, and report a
  // well-formed but unknown gid as SM_ERR_NOTFOUND.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string over(sm1::max_gid + 1, 'g');
  for (std::string_view gid : {std::string_view("x\0y", 3),
                               std::string_view(""), std::string_view(over)}) {
    CAPTURE(std::string(gid));
    expect_fail(sm1::commit_prepared(s, gid), sm1::SM_ERR_INVALID);
    expect_fail(sm1::abort_prepared(s, gid), sm1::SM_ERR_INVALID);
  }
  expect_fail(sm1::commit_prepared(s, "unknown"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::abort_prepared(s, "unknown"), sm1::SM_ERR_NOTFOUND);
}

TEST_CASE("calls needing a transaction fail with SM_ERR_NOTRANS without one") {
  // Verify every call that requires an active transaction.
  // 1. With none active, each fails with SM_ERR_NOTRANS.
  // 2. The committed row is unchanged afterwards.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string_view d;
  expect_fail(sm1::commit(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::abort(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::savepoint(s), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::rollback(s, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::forget(s, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::lock_row(s, 1, "k", sm1::SM_LOCKMODE_SH),
              sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::prepare(s, "g"), sm1::SM_ERR_NOTRANS);
  uint64_t storeid;
  expect_fail(sm1::mkstore(s, storeid), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::mkseq(s, "s", 1, 1), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::rmseq(s, "s"), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::rmstore(s, g_store), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::insert(s, g_store, "k", "v"), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::update(s, g_store, "a", "v"), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::erase(s, g_store, "a"), sm1::SM_ERR_NOTRANS);
  expect_fail(sm1::read(s, g_store, "a", d), sm1::SM_ERR_NOTRANS);
  expect_null(scan(s, g_store, nullptr, 0, 0, nullptr, 0, 0),
              sm1::SM_ERR_NOTRANS);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, g_store, "a") == "1");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a rejected call leaves the transaction active and unchanged") {
  // Verify sm1.h's promise: a non-retryable error changes nothing.
  // 1. Write "b", then provoke SM_ERR_INVALID, SM_ERR_NOTFOUND,
  //    and SM_ERR_DUPKEY.
  // 2. The transaction is still active: "b" reads back, a further
  //    write succeeds, and the commit makes both durable.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string over(sm1::max_keylen + 1, 'k');
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, g_store, "b", "2");
  expect_fail(sm1::insert(s, g_store, over, "v"), sm1::SM_ERR_INVALID);
  expect_fail(sm1::update(s, g_store, "missing", "v"), sm1::SM_ERR_NOTFOUND);
  expect_fail(sm1::insert(s, g_store, "a", "v"), sm1::SM_ERR_DUPKEY);
  CHECK(get(s, g_store, "b") == "2");
  put(s, g_store, "c", "3");
  REQUIRE(sm1::commit(s) == 0);

  sm1::session_t *v = f.open();
  REQUIRE(sm1::begin(v, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(v, g_store, "a") == "1");
  CHECK(get(v, g_store, "b") == "2");
  CHECK(get(v, g_store, "c") == "3");
  REQUIRE(sm1::commit(v) == 0);
  sm1::closedb(v);
}

TEST_CASE("every sequence call validates its name") {
  // Verify sequence names: one holding a NUL byte, "", and
  // sm1::max_seqname + 1 bytes fail with SM_ERR_INVALID;
  // sm1::max_seqname bytes work.
  // 1. Each bad name is rejected by sm1::mkseq, sm1::rmseq, and
  //    sm1::nextseq.
  // 2. A sequence named with exactly sm1::max_seqname bytes can
  //    be created, drawn from, and dropped.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  std::string over(sm1::max_seqname + 1, 's');
  uint64_t v = 0;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (std::string_view name : {std::string_view("x\0y", 3),
                                std::string_view(""), std::string_view(over)}) {
    CAPTURE(std::string(name));
    expect_fail(sm1::mkseq(s, name, 1, 1), sm1::SM_ERR_INVALID);
    expect_fail(sm1::rmseq(s, name), sm1::SM_ERR_INVALID);
    expect_fail(sm1::nextseq(s, name, v), sm1::SM_ERR_INVALID);
  }

  std::string max(sm1::max_seqname, 's');
  REQUIRE(sm1::mkseq(s, max.c_str(), 7, 2) == 0);
  REQUIRE(sm1::nextseq(s, max.c_str(), v) == 0);
  CHECK(v == 7);
  CHECK(sm1::rmseq(s, max.c_str()) == 0);
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("sm1::lock_oid validates its mode and its transaction") {
  // Verify lock_oid's arguments: a mode, a transaction, any oid.
  // 1. Outside a transaction it is SM_ERR_NOTRANS.
  // 2. A mode past Gray's five is SM_ERR_INVALID, waiting or not.
  // 3. Oids 0 and the largest are taken, with no store behind them.
  args_fixture_t f;
  sm1::session_t *s = f.s();
  expect_fail(sm1::lock_oid(s, 1, sm1::SM_LOCKMODE_SH), sm1::SM_ERR_NOTRANS);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);

  expect_fail(sm1::lock_oid(s, 1, static_cast<sm1::lockmode_t>(5)),
              sm1::SM_ERR_INVALID);
  expect_fail(sm1::trylock_oid(s, 1, static_cast<sm1::lockmode_t>(5)),
              sm1::SM_ERR_INVALID);

  CHECK(sm1::lock_oid(s, 0, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm1::lock_oid(s, UINT64_MAX, sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::commit(s) == 0);
}
