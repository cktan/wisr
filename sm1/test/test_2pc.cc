/*
 * test_2pc.cc -- two-phase commit and recovery: sm1_prepare,
 * resolution by gid, in-doubt transactions across a restart, the
 * restored-transaction SSI rule, and the orphan CF sweep
 * (SM1_DESIGN.md §8.4, §8.5, §9, §12.5 "2PC").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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
    REQUIRE(sm1_mkdb(m_home.env(), "db", nullptr) == 0);
    sm1_session_t *s = open();
    REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
    g_store = mkstore(s);
    put(s, g_store, "a", "0");
    put(s, g_store, "b", "0");
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_closedb(s) == 0);
  }

  /* A new session on "db". */
  sm1_session_t *open() const {
    sm1_session_t *s = sm1_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1_errmsg());
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

/* Set key in store 1 to val; the result of sm1_update. */
int set(sm1_session_t *s, const std::string &key, const std::string &val) {
  return sm1_update(s, g_store, key.data(), key.size(), val.data(), val.size(),
                    nullptr, nullptr);
}

/* The unresolved prepared transactions, as (gid, prepared_at). */
std::vector<std::pair<std::string, long long>> listed(sm1_session_t *s) {
  sm1_prepared_t *list = nullptr;
  size_t n = 0;
  REQUIRE(sm1_list_prepared(s, &list, &n) == 0);
  std::vector<std::pair<std::string, long long>> out;
  for (size_t i = 0; i < n; i++) {
    out.emplace_back(list[i].gid, list[i].prepared_at);
  }
  CHECK((n == 0) == (list == nullptr));
  sm1_free_prepared(list, n);
  return out;
}

/* The value of key in store 1 at a fresh RR snapshot. */
std::string read_now(sm1_session_t *s, const std::string &key) {
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  std::string v = get(s, g_store, key);
  REQUIRE(sm1_commit(s) == 0);
  return v;
}

/* Start `test_wisr --<name>-child homedir`, its stdout piped to *fd. */
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
    execl("/proc/self/exe", "test_wisr", flag.c_str(), homedir,
          (char *)nullptr);
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
uint64_t first_store(sm1_session_t *s) {
  uint64_t *list = nullptr;
  size_t n = 0;
  if (sm1_lsstore(s, &list, &n) != 0 || n == 0) {
    return 0;
  }
  uint64_t store = list[0];
  free(list);
  return store;
}

} // namespace

/*
 * Child: insert "c" and prepare it as "g-crash", report, then
 * wait to be killed with the transaction in doubt.
 */
int prepare_child_main(const char *homedir) {
  sm1_env_t *env = sm1_attach(homedir);
  sm1_session_t *s = env ? sm1_opendb(env, "db", nullptr) : nullptr;
  if (!s || sm1_begin(s, SM1_XCT_REPEATABLE_READ) != 0) {
    return 2;
  }
  // Find the store, then write into it and prepare.
  uint64_t store = first_store(s);
  if (store == 0 || sm1_insert(s, store, "c", 1, "v", 1) != 0 ||
      sm1_prepare(s, "g-crash") != 0) {
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
  sm1_env_t *env = sm1_attach(homedir);
  sm1_session_t *s = env ? sm1_opendb(env, "db", nullptr) : nullptr;
  if (!s || sm1_begin(s, SM1_XCT_REPEATABLE_READ) != 0) {
    return 2;
  }
  // The newest store is the last one sm1_lsstore lists.
  uint64_t *list = nullptr;
  size_t n = 0;
  if (sm1_lsstore(s, &list, &n) != 0 || n < 2) {
    return 2;
  }
  uint64_t newest = list[n - 1];
  free(list);
  uint64_t fresh = 0;
  if (sm1_rmstore(s, newest) != 0 || sm1_commit(s) != 0 ||
      sm1_begin(s, SM1_XCT_REPEATABLE_READ) != 0 ||
      sm1_mkstore(s, &fresh) != 0) {
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
  // 2. sm1_commit resolves it: the change is visible, nothing
  //    is listed, and the session can begin again.
  // 3. Update "b", prepare as "g2", then sm1_abort: the change
  //    is gone and nothing is listed.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *r = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "g1") == 0);
  CHECK(read_now(r, "a") == "0");
  auto rows = listed(r);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g1");
  REQUIRE(sm1_commit(s) == 0);
  CHECK(read_now(r, "a") == "1");
  CHECK(listed(r).empty());

  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm1_prepare(s, "g2") == 0);
  REQUIRE(sm1_abort(s) == 0);
  CHECK(read_now(r, "b") == "0");
  CHECK(listed(r).empty());
  sm1_closedb(s);
  sm1_closedb(r);
}

TEST_CASE("a prepared transaction accepts only commit and abort") {
  // Verify the PREPARED state's rules (§8.4, API.md).
  // 1. Preparing without a transaction fails with NOTRANS; an
  //    empty or over-long gid fails with INVALID.
  // 2. After prepare, data calls, savepoint, and rollback fail
  //    with SM1_ERR_PREPARED, and sm1_begin with SM1_ERR_INTRANS.
  // 3. A second prepare under the same gid fails with EXISTS.
  // 4. Resolving an unknown gid fails with NOTFOUND.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  CHECK(sm1_prepare(s, "g") == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTRANS);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(sm1_prepare(s, "") == -1);
  CHECK(sm1_errno() == SM1_ERR_INVALID);
  std::string long_gid(SM1_MAX_GID + 1, 'g');
  CHECK(sm1_prepare(s, long_gid.c_str()) == -1);
  CHECK(sm1_errno() == SM1_ERR_INVALID);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "g") == 0);

  const char *d;
  size_t dl;
  CHECK(sm1_read(s, g_store, "a", 1, &d, &dl) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(sm1_savepoint(s) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(sm1_rollback(s, 1) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(sm1_forget(s, 1) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(sm1_lock(s, g_store, "a", 1, SM1_LOCK_SHARED) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  uint64_t fresh = 0;
  CHECK(sm1_mkstore(s, &fresh) == -1);
  CHECK(sm1_errno() == SM1_ERR_PREPARED);
  CHECK(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == -1);
  CHECK(sm1_errno() == SM1_ERR_INTRANS);

  REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(u, "b", "1") == 0);
  CHECK(sm1_prepare(u, "g") == -1);
  CHECK(sm1_errno() == SM1_ERR_EXISTS);
  REQUIRE(sm1_abort(u) == 0);
  CHECK(sm1_commit_prepared(u, "nope") == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTFOUND);
  REQUIRE(sm1_commit(s) == 0);
  sm1_closedb(s);
  sm1_closedb(u);
}

TEST_CASE("a prepare that fails validation releases its gid") {
  // Verify the gid reservation is undone on a failed prepare.
  // 1. T1 and T2 (SERIALIZABLE) begin. T2 reads "b", updates
  //    "a", and commits.
  // 2. T1 reads "a", stepping past T2's version, and updates
  //    "b", which T2 read: T1 is doomed (§7.3).
  // 3. T1's prepare under "g" fails with SM1_ERR_SERIALIZATION,
  //    and "g" is not listed.
  // 4. A new transaction prepares and commits under "g".
  tpc_fixture_t f;
  sm1_session_t *t1 = f.open();
  sm1_session_t *t2 = f.open();
  REQUIRE(sm1_begin(t1, SM1_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm1_begin(t2, SM1_XCT_SERIALIZABLE) == 0);
  CHECK(get(t2, g_store, "b") == "0");
  REQUIRE(set(t2, "a", "1") == 0);
  REQUIRE(sm1_commit(t2) == 0);
  CHECK(get(t1, g_store, "a") == "0");
  REQUIRE(set(t1, "b", "1") == 0);
  CHECK(sm1_prepare(t1, "g") == -1);
  CHECK(sm1_errno() == SM1_ERR_SERIALIZATION);
  CHECK(listed(t2).empty());

  REQUIRE(sm1_begin(t1, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(t1, "b", "2") == 0);
  REQUIRE(sm1_prepare(t1, "g") == 0);
  REQUIRE(sm1_commit(t1) == 0);
  CHECK(read_now(t2, "b") == "2");
  sm1_closedb(t1);
  sm1_closedb(t2);
}

TEST_CASE("nothing allocates once the PREPARED record is written") {
  // Verify a prepare whose record is written stands, even when
  // memory runs out right after the write (§8.4).
  // 1. S updates "a" and prepares it under a gid too long for a
  //    short string, so copying it would allocate.
  // 2. From just after the PREPARED record is written until
  //    sm1_prepare returns, every allocation on S's thread fails:
  //    sm1_prepare still returns 0.
  // 3. The transaction is listed under its gid, and U commits it
  //    by gid.
  // 4. A fresh snapshot reads S's update.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  const std::string gid = "g-" + std::string(60, 'x');
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);

  s->db->test_after_prepare = [] { t_alloc_budget = 0; };
  int rc = sm1_prepare(s, gid.c_str());
  t_alloc_budget = -1;
  s->db->test_after_prepare = nullptr;
  CHECK_MESSAGE(rc == 0, sm1_errmsg());

  auto l = listed(u);
  REQUIRE(l.size() == 1);
  CHECK(l[0].first == gid);
  REQUIRE_MESSAGE(sm1_commit_prepared(u, gid.c_str()) == 0, sm1_errmsg());
  CHECK(read_now(u, "a") == "1");
  sm1_closedb(s);
  sm1_closedb(u);
}

TEST_CASE("a prepare that runs out of memory before its record is undone") {
  // Verify that every allocation failure before the PREPARED
  // write leaves the transaction on its session, with its gid and
  // its locks free (§8.4).
  // 1. For n = 0, 1, 2, ...: S updates "a", and every allocation
  //    from the n-th on fails, from the start of sm1_prepare up to
  //    the PREPARED write.
  // 2. sm1_prepare fails with NOMEM, and S can still abort its
  //    transaction, which it would not have were it lost.
  // 3. Nothing is listed as prepared, and U updates "a" at once:
  //    no lock was stranded. The next round reuses the gid, so a
  //    stranded reservation would fail it.
  // 4. Once n passes every allocation, the prepare succeeds; U
  //    commits it by gid and reads S's update.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  const std::string gid = "g-" + std::string(60, 'x');
  s->db->test_before_prepare = [] {
    t_alloc_budget = -1;
    return rocksdb::Status::OK();
  };

  long n = 0;
  for (;; n++) {
    REQUIRE(n < 10000);
    REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
    REQUIRE(set(s, "a", "1") == 0);
    t_alloc_budget = n;
    int rc = sm1_prepare(s, gid.c_str());
    t_alloc_budget = -1;
    if (rc == 0) {
      break;
    }
    INFO("allocation ", n, " failed");
    CHECK(sm1_errno() == SM1_ERR_NOMEM);
    REQUIRE_MESSAGE(sm1_abort(s) == 0, sm1_errmsg());
    CHECK(listed(u).empty());
    REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
    CHECK(set(u, "a", "2") == 0);
    REQUIRE(sm1_abort(u) == 0);
  }
  s->db->test_before_prepare = nullptr;
  CHECK(n > 0);

  REQUIRE_MESSAGE(sm1_commit_prepared(u, gid.c_str()) == 0, sm1_errmsg());
  CHECK(read_now(u, "a") == "1");
  sm1_closedb(s);
  sm1_closedb(u);
}

TEST_CASE("a resolve that fails and runs out of memory keeps the transaction") {
  // Verify a prepared transaction whose resolution fails stays
  // registered and resolvable by gid, whichever allocation on the
  // way fails too (§8.5).
  // 1. S updates "a" and prepares it.
  // 2. Mark the database failed, so any resolution fails.
  // 3. For n = 0, 1, 2, ...: every allocation from the n-th on
  //    fails while U commits it by gid; the commit fails, and the
  //    transaction is still listed under its gid.
  // 4. Stop once the failure is the database's (IO), not NOMEM:
  //    every allocation on the way has then been failed.
  // 5. Clear the failure; U commits it by gid and reads S's update.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  const std::string gid = "g-" + std::string(60, 'x');
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, gid.c_str()) == 0);

  s->db->failed = true;
  long n = 0;
  for (;; n++) {
    REQUIRE(n < 10000);
    t_alloc_budget = n;
    int rc = sm1_commit_prepared(u, gid.c_str());
    t_alloc_budget = -1;
    INFO("allocation ", n, " failed");
    REQUIRE(rc == -1);
    sm1_errno_t e = sm1_errno();
    auto l = listed(u);
    REQUIRE(l.size() == 1);
    CHECK(l[0].first == gid);
    if (e != SM1_ERR_NOMEM) {
      CHECK(e == SM1_ERR_IO);
      break;
    }
  }
  s->db->failed = false;
  CHECK(n > 0);

  REQUIRE_MESSAGE(sm1_commit_prepared(u, gid.c_str()) == 0, sm1_errmsg());
  CHECK(read_now(u, "a") == "1");
  sm1_closedb(s);
  sm1_closedb(u);
}

TEST_CASE("a commit by gid that runs out of memory before its record frees its "
          "commit_ts") {
  // Verify every allocation failure between claiming a commit_ts
  // and writing the COMMITTED record of a prepared transaction
  // hands the commit_ts back and leaves it resolvable (§8.5).
  // 1. S reads "b" and updates "a" at SERIALIZABLE, and prepares.
  // 2. For n = 0, 1, 2, ...: every allocation from the n-th on
  //    fails while U commits it by gid, up to the status write.
  // 3. The commit fails with NOMEM; the transaction is still
  //    listed; V commits a key, and a fresh snapshot sees it, so
  //    no stranded commit_ts holds visible_ts back.
  // 4. Once n passes every allocation, the commit succeeds, and a
  //    fresh snapshot reads S's update.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  sm1_session_t *v = f.open();
  const std::string gid = "g-" + std::string(60, 'x');
  REQUIRE(sm1_begin(s, SM1_XCT_SERIALIZABLE) == 0);
  CHECK(get(s, g_store, "b") == "0");
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, gid.c_str()) == 0);
  s->db->test_before_commit = [](uint64_t) {
    t_alloc_budget = -1;
    return rocksdb::Status::OK();
  };

  long n = 0;
  for (;; n++) {
    REQUIRE(n < 10000);
    t_alloc_budget = n;
    int rc = sm1_commit_prepared(u, gid.c_str());
    t_alloc_budget = -1;
    if (rc == 0) {
      break;
    }
    INFO("allocation ", n, " failed");
    CHECK(sm1_errno() == SM1_ERR_NOMEM);
    auto l = listed(u);
    REQUIRE(l.size() == 1);
    CHECK(l[0].first == gid);
    std::string key = "p" + std::to_string(n);
    REQUIRE(sm1_begin(v, SM1_XCT_REPEATABLE_READ) == 0);
    put(v, g_store, key, "v");
    REQUIRE(sm1_commit(v) == 0);
    REQUIRE(read_now(v, key) == "v");
  }
  s->db->test_before_commit = nullptr;
  CHECK(n > 0);

  CHECK(read_now(u, "a") == "1");
  sm1_closedb(s);
  sm1_closedb(u);
  sm1_closedb(v);
}

TEST_CASE("another session can resolve a prepared transaction by gid") {
  // Verify resolution from any session (§8.5) and what the
  // preparing session sees afterwards.
  // 1. S prepares "g"; U commits it by gid.
  // 2. S has no transaction: sm1_commit fails with NOTRANS, and
  //    sm1_begin succeeds.
  // 3. U's own active transaction is undisturbed by a resolve.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  sm1_session_t *u = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "g") == 0);

  REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, g_store, "a") == "0");
  REQUIRE(sm1_commit_prepared(u, "g") == 0);
  CHECK(get(u, g_store, "a") == "0"); // its snapshot predates the commit
  REQUIRE(sm1_commit(u) == 0);

  CHECK(sm1_commit(s) == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTRANS);
  CHECK(read_now(s, "a") == "1");
  sm1_closedb(s);
  sm1_closedb(u);
}

TEST_CASE("a prepared transaction keeps its locks after its session closes") {
  // Verify an in-doubt transaction blocks writers (§8.4, §9.3).
  // 1. S updates "a", prepares "g", and closes: "g" stays in
  //    doubt and is listed with a sane prepared_at.
  // 2. U's update of "a" blocks.
  // 3. V commits "g" by gid; U's update then fails with
  //    SM1_ERR_CONFLICT.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  long long before = (long long)time(nullptr);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "g") == 0);
  REQUIRE(sm1_closedb(s) == 0);

  sm1_session_t *u = f.open();
  sm1_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g");
  CHECK(rows[0].second >= before);
  CHECK(rows[0].second <= (long long)time(nullptr));

  REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm1_errno();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1_commit_prepared(v, "g") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == SM1_ERR_CONFLICT);
  CHECK(read_now(v, "a") == "1");
  sm1_closedb(u);
  sm1_closedb(v);
}

TEST_CASE("a prepared transaction keeps its shared locks across a restart") {
  // Verify sm1_lock's shared locks reach the PREPARED record and
  // come back with it (§8.4, §9.1).
  // 1. S locks "b" shared, writes "a", and prepares "g".
  // 2. Restart the environment, so "g" is rebuilt from its record.
  // 3. A writer of "b" blocks on the restored shared lock, and
  //    a second shared lock on "b" is granted at once.
  // 4. Aborting "g" by gid frees "b"; the blocked write completes.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm1_lock(s, g_store, "b", 1, SM1_LOCK_SHARED) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "g") == 0);
  f.restart();

  sm1_session_t *u = f.open();
  sm1_session_t *v = f.open();
  REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(sm1_lock(u, g_store, "b", 1, SM1_LOCK_SHARED) == 0);

  std::atomic<bool> returned{false};
  int rc = -2;
  std::thread tu([&] {
    rc = set(u, "b", "u");
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1_abort_prepared(v, "g") == 0);
  tu.join();
  CHECK(rc == 0);
  REQUIRE(sm1_commit(u) == 0);
  sm1_closedb(u);
  sm1_closedb(v);
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
  sm1_session_t *s = f.open();
  sm1_session_t *s2 = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm1_prepare(s, "gc") == 0);
  REQUIRE(sm1_begin(s2, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s2, "b", "1") == 0);
  REQUIRE(sm1_prepare(s2, "ga") == 0);
  auto before = listed(s);
  f.restart();

  sm1_session_t *u = f.open();
  sm1_session_t *v = f.open();
  CHECK(listed(v) == before);
  CHECK(read_now(v, "a") == "0");

  REQUIRE(sm1_begin(u, SM1_XCT_REPEATABLE_READ) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm1_errno();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm1_commit_prepared(v, "gc") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == SM1_ERR_CONFLICT);
  REQUIRE(sm1_abort_prepared(v, "ga") == 0);
  CHECK(read_now(v, "a") == "1");
  CHECK(read_now(v, "b") == "0");

  f.restart();
  sm1_session_t *w = f.open();
  CHECK(listed(w).empty());
  CHECK(read_now(w, "a") == "1");
  CHECK(read_now(w, "b") == "0");
  sm1_closedb(w);
}

TEST_CASE("read-only prepares survive a restart and resolve") {
  // Verify read-only prepares get distinct records (§8.4).
  // 1. Two read-only transactions prepare as "r1" and "r2".
  // 2. After a restart both are listed and both commit.
  tpc_fixture_t f;
  sm1_session_t *s1 = f.open();
  sm1_session_t *s2 = f.open();
  for (auto p : {std::make_pair(s1, "r1"), std::make_pair(s2, "r2")}) {
    REQUIRE(sm1_begin(p.first, SM1_XCT_SERIALIZABLE) == 0);
    CHECK(get(p.first, g_store, "a") == "0");
    REQUIRE(sm1_prepare(p.first, p.second) == 0);
  }
  f.restart();
  sm1_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].first == "r1");
  CHECK(rows[1].first == "r2");
  CHECK(sm1_commit_prepared(v, "r1") == 0);
  CHECK(sm1_abort_prepared(v, "r2") == 0);
  CHECK(listed(v).empty());
  sm1_closedb(v);
}

TEST_CASE("a prepared drop commits after a restart and its CF goes") {
  // Verify T.dropped survives in the record (§3.2, §12.5).
  // 1. Create a second store; prepare its drop as "gd";
  //    restart.
  // 2. Commit "gd" by gid; the store is gone.
  // 3. With no snapshot left to see it, GC drops its CF.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  uint64_t storeid = mkstore(s);
  REQUIRE(sm1_commit(s) == 0);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm1_rmstore(s, storeid) == 0);
  REQUIRE(sm1_prepare(s, "gd") == 0);
  f.restart();

  sm1_session_t *v = f.open();
  CHECK(wisr::db_store_cf(v->db, storeid) != nullptr);
  REQUIRE(sm1_commit_prepared(v, "gd") == 0);
  REQUIRE(sm1_begin(v, SM1_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm1_read(v, storeid, "k", 1, &d, &dl) == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTFOUND);
  REQUIRE(sm1_commit(v) == 0);
  wisr::gc_run_once(v->db);
  CHECK(wisr::db_store_cf(v->db, storeid) == nullptr);
  sm1_closedb(v);
}

TEST_CASE("write skew with a transaction restored in doubt is still caught") {
  // Verify §8.4's restored rule: its SIREAD entries are lost.
  // 1. T_prep (SERIALIZABLE) reads "a", updates "b", prepares.
  // 2. Restart: T_prep is restored without its SSI state.
  // 3. T_new (SERIALIZABLE) reads "b", stepping past T_prep's
  //    version, and updates "a", which T_prep read.
  // 4. T_new fails at commit; T_prep then commits.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_SERIALIZABLE) == 0);
  CHECK(get(s, g_store, "a") == "0");
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm1_prepare(s, "gp") == 0);
  f.restart();

  sm1_session_t *t = f.open();
  REQUIRE(sm1_begin(t, SM1_XCT_SERIALIZABLE) == 0);
  CHECK(get(t, g_store, "b") == "0");
  REQUIRE(set(t, "a", "1") == 0);
  CHECK(sm1_commit(t) == -1);
  CHECK(sm1_errno() == SM1_ERR_SERIALIZATION);
  REQUIRE(sm1_commit_prepared(t, "gp") == 0);
  CHECK(read_now(t, "b") == "1");
  CHECK(read_now(t, "a") == "0");
  sm1_closedb(t);
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

  sm1_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g-crash");
  CHECK(read_now(v, "c") == "<none>");
  REQUIRE(sm1_commit_prepared(v, "g-crash") == 0);
  CHECK(read_now(v, "c") == "v");
  sm1_closedb(v);
}

TEST_CASE("startup drops store CFs that nothing names") {
  // Verify the orphan CF sweep (§9.1 step 6).
  // 1. Create a second store and commit. A child drops it
  //    and commits, then creates one without committing, and
  //    is killed.
  // 2. Reattach: the dropped store is gone, and the only
  //    store CFs left are the catalog's and the fixture's.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  uint64_t storeid = mkstore(s);
  REQUIRE(sm1_commit(s) == 0);
  f.home().detach();

  int fd;
  pid_t pid = spawn_child("orphan", f.home().path(), &fd);
  kill_after_report(pid, fd);
  f.home().attach();

  sm1_session_t *v = f.open();
  std::set<uint64_t> stores;
  for (auto &kv : v->db->stores) {
    stores.insert(kv.first);
  }
  CHECK(stores == std::set<uint64_t>{0, g_store});
  REQUIRE(sm1_begin(v, SM1_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm1_read(v, storeid, "k", 1, &d, &dl) == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTFOUND);
  REQUIRE(sm1_commit(v) == 0);
  sm1_closedb(v);
}

TEST_CASE("a failed prepare write fails the database stop") {
  // Verify §8.4's fail-stop on the PREPARED record.
  // 1. The prepare write of "gx" fails: SM1_ERR_IO, and the
  //    session has no transaction left.
  // 2. "gx" is not listed and cannot be resolved; writes fail.
  // 3. After a restart the record never existed: nothing is
  //    listed and the update of "a" is gone.
  tpc_fixture_t f;
  sm1_session_t *s = f.open();
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  s->db->test_before_prepare = [] {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(sm1_prepare(s, "gx") == -1);
  CHECK(sm1_errno() == SM1_ERR_IO);
  s->db->test_before_prepare = nullptr;
  CHECK(listed(s).empty());
  CHECK(sm1_commit_prepared(s, "gx") == -1);
  CHECK(sm1_errno() == SM1_ERR_NOTFOUND);
  REQUIRE(sm1_begin(s, SM1_XCT_REPEATABLE_READ) == 0);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm1_errno() == SM1_ERR_IO);
  REQUIRE(sm1_commit(s) == 0);

  f.restart();
  sm1_session_t *v = f.open();
  CHECK(listed(v).empty());
  CHECK(read_now(v, "a") == "0");
  sm1_closedb(v);
}
