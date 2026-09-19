/*
 * test_session.cc -- sm_opendb, sm_closedb, the open-database
 * registry, and the transaction state machine.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

TEST_CASE("sm_opendb requires an existing database") {
  // Verify open never creates a database.
  // 1. Open a name that was never created; expect SM_ERR_NOTFOUND.
  // 2. Create it; open now succeeds.
  home_t home;
  CHECK(sm_opendb(home.env(), "db", nullptr) == nullptr);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  REQUIRE(sm_mkdb(home.env(), "db", nullptr) == 0);
  sm_session_t *s = sm_opendb(home.env(), "db", nullptr);
  CHECK(s);
  CHECK(sm_closedb(s) == 0);
}

TEST_CASE("an open database cannot be removed, and detach honors force") {
  // Verify open sessions block rmdb and unforced detach.
  // 1. Open two sessions on one database.
  // 2. sm_rmdb fails with SM_ERR_INUSE.
  // 3. Close one; rmdb still fails. Close the other; it succeeds.
  // 4. Open a session again; sm_detach(env, 0) fails with
  //    SM_ERR_INUSE, and sm_detach(env, 1) succeeds.
  tmpdir_t tmp;
  REQUIRE(sm_init(tmp.path()) == 0);
  sm_env_t *env = sm_attach(tmp.path());
  REQUIRE(env);
  REQUIRE(sm_mkdb(env, "db", nullptr) == 0);
  sm_session_t *a = sm_opendb(env, "db", nullptr);
  sm_session_t *b = sm_opendb(env, "db", nullptr);
  REQUIRE(a);
  REQUIRE(b);

  CHECK(sm_rmdb(env, "db") == -1);
  CHECK(sm_errno() == SM_ERR_INUSE);
  CHECK(sm_closedb(a) == 0);
  CHECK(sm_rmdb(env, "db") == -1);
  CHECK(sm_errno() == SM_ERR_INUSE);
  CHECK(sm_closedb(b) == 0);
  CHECK(sm_rmdb(env, "db") == 0);

  REQUIRE(sm_mkdb(env, "db", nullptr) == 0);
  sm_session_t *c = sm_opendb(env, "db", nullptr);
  REQUIRE(c);
  REQUIRE(sm_begin(c, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_detach(env, 0) == -1);
  CHECK(sm_errno() == SM_ERR_INUSE);
  CHECK(sm_detach(env, 1) == 0);
}

TEST_CASE("the transaction state machine rejects out-of-order calls") {
  // Verify begin/commit/abort enforce one active transaction.
  // 1. Commit, abort, and data calls with no transaction fail
  //    with SM_ERR_NOTRANS.
  // 2. An unknown isolation level fails with SM_ERR_INVALID.
  // 3. A second begin fails with SM_ERR_INTRANS.
  // 4. After commit, the session can begin again.
  home_t home;
  REQUIRE(sm_mkdb(home.env(), "db", nullptr) == 0);
  sm_session_t *s = sm_opendb(home.env(), "db", nullptr);
  REQUIRE(s);

  CHECK(sm_commit(s) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  CHECK(sm_abort(s) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  CHECK(sm_mkrel(s, 1) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  const char *data;
  size_t len;
  CHECK(sm_read(s, 1, "k", 1, &data, &len) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);

  CHECK(sm_begin(s, (sm_xctlevel_t)7) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(sm_begin(s, SM_XCT_READ_COMMITTED) == -1);
  CHECK(sm_errno() == SM_ERR_INTRANS);
  CHECK(sm_commit(s) == 0);
  CHECK(sm_begin(s, SM_XCT_READ_COMMITTED) == 0);
  CHECK(sm_closedb(s) == 0);
}
