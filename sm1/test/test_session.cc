/*
 * test_session.cc -- sm1_opendb, sm1_closedb, the open-database
 * registry, and the transaction state machine.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

TEST_CASE("sm1_opendb requires an existing database") {
  // Verify open never creates a database.
  // 1. Open a name that was never created; expect SM_ERR_NOTFOUND.
  // 2. Create it; open now succeeds.
  home_t home;
  CHECK(sm1_opendb(home.env(), "db", nullptr) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1_mkdb(home.env(), "db", nullptr) == 0);
  sm1_session_t *s = sm1_opendb(home.env(), "db", nullptr);
  CHECK(s);
  CHECK(sm1_closedb(s) == 0);
}

TEST_CASE("an open database cannot be removed, and detach honors force") {
  // Verify open sessions block rmdb and unforced detach.
  // 1. Open two sessions on one database.
  // 2. sm1_rmdb fails with SM_ERR_INUSE.
  // 3. Close one; rmdb still fails. Close the other; it succeeds.
  // 4. Open a session again; sm1_detach(env, 0) fails with
  //    SM_ERR_INUSE, and sm1_detach(env, 1) succeeds.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  sm1_env_t *env = sm1_attach(tmp.path());
  REQUIRE(env);
  REQUIRE(sm1_mkdb(env, "db", nullptr) == 0);
  sm1_session_t *a = sm1_opendb(env, "db", nullptr);
  sm1_session_t *b = sm1_opendb(env, "db", nullptr);
  REQUIRE(a);
  REQUIRE(b);

  CHECK(sm1_rmdb(env, "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1_closedb(a) == 0);
  CHECK(sm1_rmdb(env, "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1_closedb(b) == 0);
  CHECK(sm1_rmdb(env, "db") == 0);

  REQUIRE(sm1_mkdb(env, "db", nullptr) == 0);
  sm1_session_t *c = sm1_opendb(env, "db", nullptr);
  REQUIRE(c);
  REQUIRE(sm1_begin(c, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(sm1_detach(env, 0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1_detach(env, 1) == 0);
}

TEST_CASE("the transaction state machine rejects out-of-order calls") {
  // Verify begin/commit/abort enforce one active transaction.
  // 1. Commit, abort, and data calls with no transaction fail
  //    with SM_ERR_NOTRANS.
  // 2. An unknown isolation level fails with SM_ERR_INVALID.
  // 3. A second begin fails with SM_ERR_INTRANS.
  // 4. After commit, the session can begin again.
  home_t home;
  REQUIRE(sm1_mkdb(home.env(), "db", nullptr) == 0);
  sm1_session_t *s = sm1_opendb(home.env(), "db", nullptr);
  REQUIRE(s);

  CHECK(sm1_commit(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  CHECK(sm1_abort(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  uint64_t storeid = 0;
  CHECK(sm1_mkstore(s, &storeid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  const char *data;
  size_t len;
  CHECK(sm1_read(s, 1, "k", 1, &data, &len) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  CHECK(sm1_begin(s, (sm1_xctlevel_t)7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1_begin(s, SM1_XCT_SERIALIZABLE) == 0);
  CHECK(sm1_begin(s, SM1_XCT_READ_COMMITTED) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTRANS);
  CHECK(sm1_commit(s) == 0);
  CHECK(sm1_begin(s, SM1_XCT_READ_COMMITTED) == 0);
  CHECK(sm1_closedb(s) == 0);
}
