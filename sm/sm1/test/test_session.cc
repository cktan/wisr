/*
 * test_session.cc -- sm1::opendb, sm1::closedb, the open-database
 * registry, and the transaction state machine.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"

#include <chrono>
#include <mutex>
#include <thread>

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

namespace {

/* The database dbname open in env now, or nullptr: closed, or never. */
wisr::db_t *open_db(sm1::env_t *env, const char *dbname) {
  std::lock_guard<std::mutex> guard(env->mu);
  auto it = env->dbs.find(dbname);
  return it == env->dbs.end() ? nullptr : it->second;
}

} // namespace

TEST_CASE("a database stays open for its idle time after its last session") {
  // Verify the reaper and closedb's idle time.
  // 1. With idle_close_ms 300, close the only session: the database
  //    is still open, and a session opened at once joins it as it is.
  // 2. Close that one and wait well past 300 ms: the reaper has
  //    closed it.
  // 3. With idle_close_ms 0, closing the last session closes it at
  //    once, as before there was an idle time.
  // 4. A database left idle can be removed: rmdb closes it first.
  // 5. One left idle that a session asks other options of is opened
  //    anew with them; asked for none, or the same, it is reused.
  // 6. Other sizes of a bulk load are other options too.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  sm1::env_t *env = attach(tmp.path());
  REQUIRE(env);
  sm1::options_t idle;
  idle.idle_close_ms = 300;
  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *a = opendb(env, "db", &idle);
  REQUIRE(a);
  wisr::db_t *db = open_db(env, "db");
  REQUIRE(sm1::closedb(a) == 0);
  CHECK(open_db(env, "db") == db);
  sm1::session_t *b = opendb(env, "db", &idle);
  REQUIRE(b);
  CHECK(open_db(env, "db") == db);

  REQUIRE(sm1::closedb(b) == 0);
  for (int i = 0; i < 50 && open_db(env, "db"); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(open_db(env, "db") == nullptr);

  sm1::options_t now;
  now.idle_close_ms = 0;
  sm1::session_t *c = opendb(env, "db", &now);
  REQUIRE(c);
  REQUIRE(sm1::closedb(c) == 0);
  CHECK(open_db(env, "db") == nullptr);

  sm1::session_t *d = opendb(env, "db", nullptr);
  REQUIRE(d);
  REQUIRE(sm1::closedb(d) == 0);
  CHECK(open_db(env, "db") != nullptr);
  CHECK(sm1::rmdb(env, "db") == 0);
  CHECK(open_db(env, "db") == nullptr);

  REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
  sm1::session_t *e = opendb(env, "db", &idle);
  REQUIRE(e);
  db = open_db(env, "db");
  REQUIRE(sm1::closedb(e) == 0);
  sm1::session_t *f = opendb(env, "db", nullptr);
  REQUIRE(f);
  CHECK(open_db(env, "db") == db);
  REQUIRE(sm1::closedb(f) == 0);
  sm1::options_t other = idle;
  other.savepoint_undo_bytes = 1 << 20;
  sm1::session_t *g = opendb(env, "db", &other);
  REQUIRE(g);
  CHECK(g->db->opts.savepoint_undo_bytes == 1 << 20);
  REQUIRE(sm1::closedb(g) == 0);

  // 6.
  sm1::options_t loads = other;
  loads.load_small_bytes = other.load_small_bytes + 1;
  loads.load_sort_bytes = other.load_sort_bytes + 1;
  loads.load_file_bytes = other.load_file_bytes + 1;
  sm1::session_t *h = opendb(env, "db", &loads);
  REQUIRE(h);
  CHECK(h->db->opts.load_small_bytes == loads.load_small_bytes);
  CHECK(h->db->opts.load_sort_bytes == loads.load_sort_bytes);
  CHECK(h->db->opts.load_file_bytes == loads.load_file_bytes);
  REQUIRE(sm1::closedb(h) == 0);
  CHECK(sm1::detach(env, 0) == 0);
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
