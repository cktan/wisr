/*
 * test_2pc.cc -- two-phase commit and recovery: sm1::prepare,
 * resolution by gid, in-doubt transactions across a restart, the
 * restored-transaction SSI rule, and the orphan CF sweep
 * (SM1_DESIGN.md §8.4, §8.5, §9, §12.5 "2PC").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm0_defer.h"
#include "sm1.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "a" -> "0", "b" -> "0". */
class tpc_fixture_t {
public:
  /* Create "db", the store, and the rows. */
  tpc_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    g_store = mkstore(s);
    put(s, g_store, "a", "0");
    put(s, g_store, "b", "0");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

  /* Detach, closing every session, and attach again. */
  void restart() {
    m_home.detach();
    m_home.attach();
  }

  /* The home directory, to detach while a child process runs. */
  home_t &home() { return m_home; }

private:
  home_t m_home;
};

/* Set key in store 1 to val; the result of sm1::update. */
int set(sm1::session_t *s, const std::string &key, const std::string &val) {
  return sm1::update(s, g_store, key, val);
}

/* The unresolved prepared transactions, as (gid, prepared_at). */
std::vector<std::pair<std::string, long long>> listed(sm1::session_t *s) {
  std::vector<sm1::prepared_t> list;
  REQUIRE(sm1::list_prepared(s, list) == 0);
  std::vector<std::pair<std::string, long long>> out;
  for (const sm1::prepared_t &p : list) {
    out.emplace_back(p.gid, p.prepared_at);
  }
  return out;
}

/* The value of key in store 1 at a fresh RR snapshot. */
std::string read_now(sm1::session_t *s, const std::string &key) {
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string v = get(s, g_store, key);
  REQUIRE(sm1::commit(s) == 0);
  return v;
}

/* Start `test_sm1 --<name>-child homedir`, its stdout piped to *fd. */
pid_t spawn_child(const char *name, const char *homedir, int *fd) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: stdout becomes the pipe, then exec.
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    std::string flag = std::string("--") + name + "-child";
    execl("/proc/self/exe", "test_sm1", flag.c_str(), homedir, (char *)nullptr);
    _exit(127);
  }
  close(fds[1]);
  *fd = fds[0];
  return pid;
}

/* Wait for the child's one-byte report, then SIGKILL it. */
void kill_after_report(pid_t pid, int fd) {
  char c = 0;
  REQUIRE(read(fd, &c, 1) == 1);
  CHECK(c == 'r');
  kill(pid, SIGKILL);
  int status;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status));
  close(fd);
}

/*
 * The lowest storeid s's transaction lists -- the fixture's
 * store -- or 0 if the database has none. A child process
 * finds the store this way, having none of the parent's
 * state.
 */
uint64_t first_store(sm1::session_t *s) {
  std::vector<uint64_t> list;
  if (sm1::lsstore(s, list) != 0 || list.empty()) {
    return 0;
  }
  return list[0];
}

} // namespace

/*
 * Child: insert "c" and prepare it as "g-crash", report, then
 * wait to be killed with the transaction in doubt.
 */
int prepare_child_main(const char *homedir) {
  sm1::env_t *env = attach(homedir);
  sm1::session_t *s = env ? opendb(env, "db", nullptr) : nullptr;
  if (!s || sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
    return 2;
  }
  // Find the store, then write into it and prepare.
  uint64_t store = first_store(s);
  if (store == 0 || sm1::insert(s, store, "c", "v") != 0 ||
      sm1::prepare(s, "g-crash") != 0) {
    return 2;
  }
  if (write(1, "r", 1) != 1) {
    return 3;
  }
  for (;;) {
    pause();
  }
}

/*
 * Child: drop the newest store and commit, then create one
 * without committing; report, then wait to be killed. Both
 * leave a store CF that only the orphan sweep can remove.
 */
int orphan_child_main(const char *homedir) {
  sm1::env_t *env = attach(homedir);
  sm1::session_t *s = env ? opendb(env, "db", nullptr) : nullptr;
  if (!s || sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
    return 2;
  }
  // The newest store is the last one sm1::lsstore lists.
  std::vector<uint64_t> list;
  if (sm1::lsstore(s, list) != 0 || list.size() < 2) {
    return 2;
  }
  uint64_t newest = list.back();
  uint64_t fresh = 0;
  if (sm1::rmstore(s, newest) != 0 || sm1::commit(s) != 0 ||
      sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
      sm1::mkstore(s, fresh) != 0) {
    return 2;
  }
  if (write(1, "r", 1) != 1) {
    return 3;
  }
  for (;;) {
    pause();
  }
}

TEST_CASE("prepare, then commit or abort on the preparing session") {
  // Verify the basic 2PC lifecycle (§8.4).
  // 1. Update "a" and prepare as "g1": the change is not yet
  //    visible, and "g1" is listed.
  // 2. sm1::commit resolves it: the change is visible, nothing
  //    is listed, and the session can begin again.
  // 3. Update "b", prepare as "g2", then sm1::abort: the change
  //    is gone and nothing is listed.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  sm1::session_t *r = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g1") == 0);
  CHECK(read_now(r, "a") == "0");
  auto rows = listed(r);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g1");
  REQUIRE(sm1::commit(s) == 0);
  CHECK(read_now(r, "a") == "1");
  CHECK(listed(r).empty());

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm1::prepare(s, "g2") == 0);
  REQUIRE(sm1::abort(s) == 0);
  CHECK(read_now(r, "b") == "0");
  CHECK(listed(r).empty());
  sm1::closedb(s);
  sm1::closedb(r);
}

TEST_CASE("sm1::in_doubt holds from prepare until the gid is resolved") {
  // Verify that in_doubt names exactly a session whose prepared
  // transaction no session has resolved yet.
  // 1. NULL, a session with no transaction, and one with an
  //    active transaction are not in doubt.
  // 2. After prepare the preparing session is in doubt, and the
  //    other session is not.
  // 3. Once the other session commits the gid, the preparing
  //    session is no longer in doubt and can begin again.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  sm1::session_t *r = f.open();
  CHECK_FALSE(sm1::in_doubt(nullptr));
  CHECK_FALSE(sm1::in_doubt(s));
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  CHECK_FALSE(sm1::in_doubt(s));

  REQUIRE(sm1::prepare(s, "g1") == 0);
  CHECK(sm1::in_doubt(s));
  CHECK_FALSE(sm1::in_doubt(r));

  REQUIRE(sm1::commit_prepared(r, "g1") == 0);
  CHECK_FALSE(sm1::in_doubt(s));
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::abort(s) == 0);
  sm1::closedb(s);
  sm1::closedb(r);
}

TEST_CASE("a prepared transaction accepts only commit and abort") {
  // Verify the PREPARED state's rules (§8.4, API.md).
  // 1. Preparing without a transaction fails with NOTRANS; an
  //    empty or over-long gid fails with INVALID.
  // 2. After prepare, data calls, savepoint, and rollback fail
  //    with SM_ERR_PREPARED, and sm1::begin with SM_ERR_INTRANS.
  // 3. A second prepare under the same gid fails with EXISTS.
  // 4. Resolving an unknown gid fails with NOTFOUND.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  sm1::session_t *u = f.open();
  CHECK(sm1::prepare(s, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::prepare(s, "") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string long_gid(sm1::max_gid + 1, 'g');
  CHECK(sm1::prepare(s, long_gid.c_str()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);

  std::string_view d;
  CHECK(sm1::read(s, g_store, "a", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(sm1::savepoint(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(sm1::rollback(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(sm1::forget(s, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(sm1::lock_row(s, g_store, "a", sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  uint64_t fresh = 0;
  CHECK(sm1::mkstore(s, fresh) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_PREPARED);
  CHECK(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTRANS);

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(u, "b", "1") == 0);
  CHECK(sm1::prepare(u, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
  REQUIRE(sm1::abort(u) == 0);
  CHECK(sm1::commit_prepared(u, "nope") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm1::closedb(s);
  sm1::closedb(u);
}

TEST_CASE("a prepare that fails validation releases its gid") {
  // Verify the gid reservation is undone on a failed prepare.
  // 1. T1 and T2 (SERIALIZABLE) begin. T2 reads "b", updates
  //    "a", and commits.
  // 2. T1 reads "a", stepping past T2's version, and updates
  //    "b", which T2 read: T1 is doomed (§7.3).
  // 3. T1's prepare under "g" fails with SM_ERR_SERIALIZATION,
  //    and "g" is not listed.
  // 4. A new transaction prepares and commits under "g".
  tpc_fixture_t f;
  sm1::session_t *t1 = f.open();
  sm1::session_t *t2 = f.open();
  REQUIRE(sm1::begin(t1, sm1::xctlevel_t::serializable) == 0);
  REQUIRE(sm1::begin(t2, sm1::xctlevel_t::serializable) == 0);
  CHECK(get(t2, g_store, "b") == "0");
  REQUIRE(set(t2, "a", "1") == 0);
  REQUIRE(sm1::commit(t2) == 0);
  CHECK(get(t1, g_store, "a") == "0");
  REQUIRE(set(t1, "b", "1") == 0);
  CHECK(sm1::prepare(t1, "g") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SERIALIZATION);
  CHECK(listed(t2).empty());

  REQUIRE(sm1::begin(t1, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(t1, "b", "2") == 0);
  REQUIRE(sm1::prepare(t1, "g") == 0);
  REQUIRE(sm1::commit(t1) == 0);
  CHECK(read_now(t2, "b") == "2");
  sm1::closedb(t1);
  sm1::closedb(t2);
}

TEST_CASE("another session can resolve a prepared transaction by gid") {
  // Verify resolution from any session (§8.5) and what the
  // preparing session sees afterwards.
  // 1. S prepares "g"; U commits it by gid.
  // 2. S has no transaction: sm1::commit fails with NOTRANS, and
  //    sm1::begin succeeds.
  // 3. U's own active transaction is undisturbed by a resolve.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  sm1::session_t *u = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(u, g_store, "a") == "0");
  REQUIRE(sm1::commit_prepared(u, "g") == 0);
  CHECK(get(u, g_store, "a") == "0"); // its snapshot predates the commit
  REQUIRE(sm1::commit(u) == 0);

  CHECK(sm1::commit(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTRANS);
  CHECK(read_now(s, "a") == "1");
  sm1::closedb(s);
  sm1::closedb(u);
}

TEST_CASE("a prepared transaction keeps its locks after its session closes") {
  // Verify an in-doubt transaction blocks writers (§8.4, §9.3).
  // 1. S updates "a", prepares "g", and closes: "g" stays in
  //    doubt and is listed with a sane prepared_at.
  // 2. U's update of "a" blocks.
  // 3. V commits "g" by gid; U's update then fails with
  //    SM_ERR_CONFLICT.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  long long before = (long long)time(nullptr);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);
  REQUIRE(sm1::closedb(s) == 0);

  sm1::session_t *u = f.open();
  sm1::session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g");
  CHECK(rows[0].second >= before);
  CHECK(rows[0].second <= (long long)time(nullptr));

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm1::errnum();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1::commit_prepared(v, "g") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_CONFLICT);
  CHECK(read_now(v, "a") == "1");
  sm1::closedb(u);
  sm1::closedb(v);
}

TEST_CASE("a prepared transaction keeps its shared locks across a restart") {
  // Verify sm1::lock_row's shared locks reach the PREPARED record and
  // come back with it (§8.4, §9.1).
  // 1. S locks "b" shared, writes "a", and prepares "g".
  // 2. Restart the environment, so "g" is rebuilt from its record.
  // 3. A writer of "b" blocks on the restored shared lock, and
  //    a second shared lock on "b" is granted at once.
  // 4. Aborting "g" by gid frees "b"; the blocked write completes.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(s, g_store, "b", sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);
  f.restart();

  sm1::session_t *u = f.open();
  sm1::session_t *v = f.open();
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::lock_row(u, g_store, "b", sm1::SM_LOCKMODE_SH) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tu([&] {
    rc = set(u, "b", "u");
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1::abort_prepared(v, "g") == 0);
  tu.join();
  CHECK(rc == 0);
  REQUIRE(sm1::commit(u) == 0);
  sm1::closedb(u);
  sm1::closedb(v);
}

TEST_CASE("row locks whose keys hash alike are one lock after a restart") {
  // Verify a prepared transaction's record keeps the one lock two row
  // keys whose ids are one make, by its id, with its mode and kind
  // (§8.4, §9.1).
  // 1. Make every row key hash alike; S locks "a" shared, of kind 7,
  //    and "b" exclusive, one lock, writes nothing, and prepares "g":
  //    S keeps no lock, and lock_status lists g's one lock, its word
  //    the hash every key has, 0, of kind 7, exclusive.
  // 2. Restart the environment, so "g" is rebuilt from its record.
  // 3. lock_status lists g's one lock again, its kind kept, and the
  //    table holds it as one hold of g's.
  // 4. Aborting "g" by gid leaves the lock table empty.
  tpc_fixture_t f;
  wisr::lock_test_rows_collide = true;
  defer { wisr::lock_test_rows_collide = false; };
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(s, g_store, "a", sm1::SM_LOCKMODE_SH, false, 7) == 0);
  REQUIRE(sm1::lock_row(s, g_store, "b", sm1::SM_LOCKMODE_EX) == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);
  CHECK(s->locks.empty());

  // The locks lock_status lists, as "word kind mode", sorted.
  auto listed = [&] {
    std::vector<sm1::lock_status_t> out;
    REQUIRE(sm1::lock_status(f.home().env(), out) == 0);
    std::vector<std::string> got;
    for (const auto &r : out) {
      got.push_back(std::to_string(r.word) + " " + std::to_string(r.kind) +
                    " " + std::to_string(r.mode));
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  const std::vector<std::string> one = {
      "0 7 " + std::to_string(sm1::SM_LOCKMODE_EX),
  };
  CHECK(listed() == one);
  sm1::closedb(s);
  f.restart();

  sm1::session_t *v = f.open();
  CHECK(listed() == one);
  {
    std::lock_guard<std::mutex> guard(v->db->lock_mu);
    REQUIRE(v->db->lock_table.size() == 1);
    std::vector<sm1::lockmode_t> modes;
    v->db->lock_table.each(
        [&](const wisr::lockid_t &, const wisr::locktab_t::holder_t &h) {
          modes.push_back(h.mode);
        });
    REQUIRE(modes.size() == 1);
    CHECK(modes[0] == sm1::SM_LOCKMODE_EX);
  }
  REQUIRE(sm1::abort_prepared(v, "g") == 0);
  {
    std::lock_guard<std::mutex> guard(v->db->lock_mu);
    CHECK(v->db->lock_table.empty());
  }
  sm1::closedb(v);
}

TEST_CASE("a prepared transaction's table locks keep DDL out, across a "
          "restart") {
  // Verify the fast path's locks reach the PREPARED record, and a
  // strong one's count comes back with it (§5.1, §8.4, §9.1).
  // 1. S locks oid 1 IS, in a slot, and oid 2 X, and prepares "g".
  // 2. V's trylock of oid 1 X fails, the IS gone to the table with
  //    the prepare, and of oid 2 IS fails, X counted.
  // 3. Restart; both fail again, "g" rebuilt from its record.
  // 4. Aborting "g" frees both: V's two trylocks succeed, and no
  //    count is left.
  tpc_fixture_t f;
  uint64_t oid1 = 1, oid2 = 2;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_oid(s, oid1, sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::lock_oid(s, oid2, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(s->fp_used == 1);
  REQUIRE(sm1::prepare(s, "g") == 0);
  CHECK(s->fp_used == 0);

  // V's two tries, each refused while "g" holds its locks.
  auto refused = [&](sm1::session_t *v) {
    REQUIRE(sm1::begin(v, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(sm1::trylock_oid(v, oid1, sm1::SM_LOCKMODE_EX) == 1);
    CHECK(sm1::trylock_oid(v, oid2, sm1::SM_LOCKMODE_IS) == 1);
    REQUIRE(sm1::commit(v) == 0);
  };
  sm1::session_t *v = f.open();
  refused(v);
  sm1::closedb(v);
  sm1::closedb(s);
  f.restart();

  v = f.open();
  refused(v);
  REQUIRE(sm1::abort_prepared(v, "g") == 0);
  REQUIRE(sm1::begin(v, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::trylock_oid(v, oid1, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm1::trylock_oid(v, oid2, sm1::SM_LOCKMODE_IS) == 0);
  REQUIRE(sm1::commit(v) == 0);
  for (const auto &c : v->db->fp_strong) {
    CHECK(0 == c.load());
  }
  sm1::closedb(v);
}

TEST_CASE("a prepared transaction keeps its intention locks across a "
          "restart") {
  // Verify a mode beyond shared and exclusive reaches the PREPARED
  // record's mode byte and comes back with it (§8.4, §9.1).
  // 1. S locks "b" IX, writes "a", and prepares "g".
  // 2. Restart, so "g" is rebuilt from its record.
  // 3. An IX on "b" is granted at once beside the restored one; an
  //    S on "b" waits.
  // 4. Aborting "g" by gid frees "b"; the S is granted.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::lock_row(s, g_store, "b", sm1::SM_LOCKMODE_IX) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "g") == 0);
  f.restart();

  sm1::session_t *u = f.open(), *v = f.open(), *w = f.open();
  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::lock_row(u, g_store, "b", sm1::SM_LOCKMODE_IX) == 0);
  REQUIRE(sm1::commit(u) == 0);
  REQUIRE(sm1::begin(w, sm1::xctlevel_t::read_committed) == 0);
  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tw([&] {
    rc = sm1::lock_row(w, g_store, "b", sm1::SM_LOCKMODE_SH);
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1::abort_prepared(v, "g") == 0);
  tw.join();
  CHECK(rc == 0);
  REQUIRE(sm1::commit(w) == 0);
  sm1::closedb(u);
  sm1::closedb(v);
  sm1::closedb(w);
}

TEST_CASE(
    "an in-doubt transaction survives a restart, then commits or aborts") {
  // Verify restore (§9.1 step 5) and both resolutions after it.
  // 1. Prepare "gc" (a = 1) and "ga" (b = 1); a forced detach
  //    leaves both in doubt.
  // 2. After reattaching both are listed with their original
  //    gid and prepared_at, and "gc" still holds its lock on
  //    "a": an update of "a" blocks.
  // 3. Commit "gc" and abort "ga": a = 1, b = 0, and after one
  //    more restart nothing is listed.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  sm1::session_t *s2 = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1::prepare(s, "gc") == 0);
  REQUIRE(sm1::begin(s2, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s2, "b", "1") == 0);
  REQUIRE(sm1::prepare(s2, "ga") == 0);
  auto before = listed(s);
  f.restart();

  sm1::session_t *u = f.open();
  sm1::session_t *v = f.open();
  CHECK(listed(v) == before);
  CHECK(read_now(v, "a") == "0");

  REQUIRE(sm1::begin(u, sm1::xctlevel_t::repeatable_read) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm1::errnum();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1::commit_prepared(v, "gc") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_CONFLICT);
  REQUIRE(sm1::abort_prepared(v, "ga") == 0);
  CHECK(read_now(v, "a") == "1");
  CHECK(read_now(v, "b") == "0");

  f.restart();
  sm1::session_t *w = f.open();
  CHECK(listed(w).empty());
  CHECK(read_now(w, "a") == "1");
  CHECK(read_now(w, "b") == "0");
  sm1::closedb(w);
}

TEST_CASE("read-only prepares survive a restart and resolve") {
  // Verify read-only prepares get distinct records (§8.4).
  // 1. Two read-only transactions prepare as "r1" and "r2".
  // 2. After a restart both are listed and both commit.
  tpc_fixture_t f;
  sm1::session_t *s1 = f.open();
  sm1::session_t *s2 = f.open();
  for (auto p : {std::make_pair(s1, "r1"), std::make_pair(s2, "r2")}) {
    REQUIRE(sm1::begin(p.first, sm1::xctlevel_t::serializable) == 0);
    CHECK(get(p.first, g_store, "a") == "0");
    REQUIRE(sm1::prepare(p.first, p.second) == 0);
  }
  f.restart();
  sm1::session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].first == "r1");
  CHECK(rows[1].first == "r2");
  CHECK(sm1::commit_prepared(v, "r1") == 0);
  CHECK(sm1::abort_prepared(v, "r2") == 0);
  CHECK(listed(v).empty());
  sm1::closedb(v);
}

TEST_CASE("a prepared drop commits after a restart and its CF goes") {
  // Verify T.dropped survives in the record (§3.2, §12.5).
  // 1. Create a second store; prepare its drop as "gd";
  //    restart.
  // 2. Commit "gd" by gid; the store is gone.
  // 3. With no snapshot left to see it, GC drops its CF.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t storeid = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmstore(s, storeid) == 0);
  REQUIRE(sm1::prepare(s, "gd") == 0);
  f.restart();

  sm1::session_t *v = f.open();
  CHECK(wisr::db_store(v->db, storeid).cf != nullptr);
  REQUIRE(sm1::commit_prepared(v, "gd") == 0);
  REQUIRE(sm1::begin(v, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view d;
  CHECK(sm1::read(v, storeid, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(v) == 0);
  wisr::gc_run_once(v->db);
  CHECK(wisr::db_store(v->db, storeid).cf == nullptr);
  sm1::closedb(v);
}

TEST_CASE("write skew with a transaction restored in doubt is still caught") {
  // Verify §8.4's restored rule: its SIREAD entries are lost.
  // 1. T_prep (SERIALIZABLE) reads "a", updates "b", prepares.
  // 2. Restart: T_prep is restored without its SSI state.
  // 3. T_new (SERIALIZABLE) reads "b", stepping past T_prep's
  //    version, and updates "a", which T_prep read.
  // 4. T_new fails at commit; T_prep then commits.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::serializable) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm1::prepare(s, "gp") == 0);
  f.restart();

  sm1::session_t *t = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::serializable) == 0);
  CHECK(get(t, g_store, "b") == "0");
  REQUIRE(set(t, "a", "1") == 0);
  CHECK(sm1::commit(t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SERIALIZATION);
  REQUIRE(sm1::commit_prepared(t, "gp") == 0);
  CHECK(read_now(t, "b") == "1");
  CHECK(read_now(t, "a") == "0");
  sm1::closedb(t);
}

TEST_CASE("a transaction prepared when the process is killed is restored") {
  // Verify the PREPARED record is durable against SIGKILL (§12.5).
  // 1. A child process inserts "c", prepares "g-crash", reports,
  //    and is killed.
  // 2. Reattach: "g-crash" is listed and "c" is invisible.
  // 3. Commit it by gid: "c" is visible.
  tpc_fixture_t f;
  f.home().detach();
  int fd;
  pid_t pid = spawn_child("prepare", f.home().path(), &fd);
  kill_after_report(pid, fd);
  f.home().attach();

  sm1::session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g-crash");
  CHECK(read_now(v, "c") == "<none>");
  REQUIRE(sm1::commit_prepared(v, "g-crash") == 0);
  CHECK(read_now(v, "c") == "v");
  sm1::closedb(v);
}

TEST_CASE("startup drops store CFs that nothing names") {
  // Verify the orphan CF sweep (§9.1 step 6).
  // 1. Create a second store and commit. A child drops it
  //    and commits, then creates one without committing, and
  //    is killed.
  // 2. Reattach: the dropped store is gone, and the only
  //    store CFs left are the catalog's and the fixture's.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t storeid = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);
  f.home().detach();

  int fd;
  pid_t pid = spawn_child("orphan", f.home().path(), &fd);
  kill_after_report(pid, fd);
  f.home().attach();

  sm1::session_t *v = f.open();
  std::set<uint64_t> stores;
  for (auto &kv : v->db->stores) {
    stores.insert(kv.first);
  }
  CHECK(stores == std::set<uint64_t>{0, g_store});
  REQUIRE(sm1::begin(v, sm1::xctlevel_t::repeatable_read) == 0);
  std::string_view d;
  CHECK(sm1::read(v, storeid, "k", d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(v) == 0);
  sm1::closedb(v);
}

TEST_CASE("a failed prepare write fails the database stop") {
  // Verify §8.4's fail-stop on the PREPARED record.
  // 1. The prepare write of "gx" fails: SM_ERR_IO, and the
  //    session has no transaction left.
  // 2. "gx" is not listed and cannot be resolved; writes fail.
  // 3. After a restart the record never existed: nothing is
  //    listed and the update of "a" is gone.
  tpc_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  s->db->test_before_prepare = [] {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(sm1::prepare(s, "gx") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
  s->db->test_before_prepare = nullptr;
  CHECK(listed(s).empty());
  CHECK(sm1::commit_prepared(s, "gx") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
  REQUIRE(sm1::commit(s) == 0);

  f.restart();
  sm1::session_t *v = f.open();
  CHECK(listed(v).empty());
  CHECK(read_now(v, "a") == "0");
  sm1::closedb(v);
}
