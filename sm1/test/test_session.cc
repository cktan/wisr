/*
 * test_session.cc -- sm1::opendb, sm1::closedb, the open-database
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
  CHECK(opendb(home.env(), "db", nullptr) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db", nullptr);
  CHECK(s);
  CHECK(sm1::closedb(s) == 0);
}

TEST_CASE("an open database cannot be removed, and detach honors force") {
  // Verify open sessions block rmdb and unforced detach.
  // 1. Open two sessions on one database.
  // 2. sm1::rmdb fails with SM_ERR_INUSE.
  // 3. Close one; rmdb still fails. Close the other; it succeeds.
  // 4. Open a session again; sm1::detach(env, 0) fails with
  //    SM_ERR_INUSE, and sm1::detach(env, 1) succeeds.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  sm1::env_t *env = attach(tmp.path());
  REQUIRE(env);
  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *a = opendb(env, "db", nullptr);
  sm1::session_t *b = opendb(env, "db", nullptr);
  REQUIRE(a);
  REQUIRE(b);

  CHECK(sm1::rmdb(env, "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1::closedb(a) == 0);
  CHECK(sm1::rmdb(env, "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1::closedb(b) == 0);
  CHECK(sm1::rmdb(env, "db") == 0);

  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *c = opendb(env, "db", nullptr);
  REQUIRE(c);
  REQUIRE(sm1::begin(c, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::detach(env, 0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm1::detach(env, 1) == 0);
}

TEST_CASE("the transaction state machine rejects out-of-order calls") {
  // Verify begin/commit/abort enforce one active transaction.
  // 1. Commit, abort, and data calls with no transaction fail
  //    with SM_ERR_NOTRANS.
  // 2. An unknown isolation level fails with SM_ERR_INVALID.
  // 3. A second begin fails with SM_ERR_INTRANS.
  // 4. After commit, the session can begin again.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db", nullptr);
  REQUIRE(s);

  CHECK(sm1::commit(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  CHECK(sm1::abort(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  uint64_t storeid = 0;
  CHECK(sm1::mkstore(s, storeid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  std::string_view data;
  CHECK(sm1::read(s, 1, "k", data) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);

  CHECK(sm1::begin(s, (sm1::xctlevel_t)7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::serializable) == 0);
  CHECK(sm1::begin(s, sm1::xctlevel_t::read_committed) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTRANS);
  CHECK(sm1::commit(s) == 0);
  CHECK(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm1::closedb(s) == 0);
}
