/*
 * test_2pc.cc -- two-phase commit and recovery: sm_prepare,
 * resolution by gid, in-doubt transactions across a restart, the
 * restored-transaction SSI rule, and the orphan CF sweep
 * (DESIGN.md §8.4, §8.5, §9, §12.5 "2PC").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"

#include <signal.h>
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

/* A database "db" with relation "t" holding "a" -> "0", "b" -> "0". */
class tpc_fixture_t {
public:
  /* Create "db", "t", and the rows. */
  tpc_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    sm_session_t *s = open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "t") == 0);
    put(s, "t", "a", "0");
    put(s, "t", "b", "0");
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_closedb(s) == 0);
  }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm_errmsg());
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

/* Set key in "t" to val; the result of sm_update. */
int set(sm_session_t *s, const std::string &key, const std::string &val) {
  return sm_update(s, "t", key.data(), key.size(), val.data(), val.size());
}

/* The unresolved prepared transactions, as (gid, prepared_at). */
std::vector<std::pair<std::string, long long>> listed(sm_session_t *s) {
  sm_prepared_t *list = nullptr;
  size_t n = 0;
  REQUIRE(sm_list_prepared(s, &list, &n) == 0);
  std::vector<std::pair<std::string, long long>> out;
  for (size_t i = 0; i < n; i++) {
    out.emplace_back(list[i].gid, list[i].prepared_at);
  }
  CHECK((n == 0) == (list == nullptr));
  sm_free_prepared(list, n);
  return out;
}

/* The value of key in "t" at a fresh RR snapshot. */
std::string read_now(sm_session_t *s, const std::string &key) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  std::string v = get(s, "t", key);
  REQUIRE(sm_commit(s) == 0);
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

} // namespace

/*
 * Child: insert "c" and prepare it as "g-crash", report, then
 * wait to be killed with the transaction in doubt.
 */
int prepare_child_main(const char *homedir) {
  sm_env_t *env = sm_attach(homedir);
  sm_session_t *s = env ? sm_opendb(env, "db", nullptr) : nullptr;
  if (!s || sm_begin(s, SM_XCT_REPEATABLE_READ) != 0 ||
      sm_insert(s, "t", "c", 1, "v", 1) != 0 || sm_prepare(s, "g-crash") != 0) {
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
 * Child: drop relation "u" and commit, then create "o" without
 * committing; report, then wait to be killed. Both leave a
 * relation CF that only the orphan sweep can remove.
 */
int orphan_child_main(const char *homedir) {
  sm_env_t *env = sm_attach(homedir);
  sm_session_t *s = env ? sm_opendb(env, "db", nullptr) : nullptr;
  if (!s || sm_begin(s, SM_XCT_REPEATABLE_READ) != 0 || sm_rmrel(s, "u") != 0 ||
      sm_commit(s) != 0 || sm_begin(s, SM_XCT_REPEATABLE_READ) != 0 ||
      sm_mkrel(s, "o") != 0) {
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
  // 2. sm_commit resolves it: the change is visible, nothing
  //    is listed, and the session can begin again.
  // 3. Update "b", prepare as "g2", then sm_abort: the change
  //    is gone and nothing is listed.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  sm_session_t *r = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm_prepare(s, "g1") == 0);
  CHECK(read_now(r, "a") == "0");
  auto rows = listed(r);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g1");
  REQUIRE(sm_commit(s) == 0);
  CHECK(read_now(r, "a") == "1");
  CHECK(listed(r).empty());

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm_prepare(s, "g2") == 0);
  REQUIRE(sm_abort(s) == 0);
  CHECK(read_now(r, "b") == "0");
  CHECK(listed(r).empty());
  sm_closedb(s);
  sm_closedb(r);
}

TEST_CASE("a prepared transaction accepts only commit and abort") {
  // Verify the PREPARED state's rules (§8.4, API.md).
  // 1. Preparing without a transaction fails with NOTRANS; an
  //    empty or over-long gid fails with INVALID.
  // 2. After prepare, data calls, savepoint, and rollback fail
  //    with SM_ERR_PREPARED, and sm_begin with SM_ERR_INTRANS.
  // 3. A second prepare under the same gid fails with EXISTS.
  // 4. Resolving an unknown gid fails with NOTFOUND.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  sm_session_t *u = f.open();
  CHECK(sm_prepare(s, "g") == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_prepare(s, "") == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  std::string long_gid(SM_MAX_GID + 1, 'g');
  CHECK(sm_prepare(s, long_gid.c_str()) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm_prepare(s, "g") == 0);

  const char *d;
  size_t dl;
  CHECK(sm_read(s, "t", "a", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_PREPARED);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm_errno() == SM_ERR_PREPARED);
  CHECK(sm_savepoint(s) == -1);
  CHECK(sm_errno() == SM_ERR_PREPARED);
  CHECK(sm_rollback(s) == -1);
  CHECK(sm_errno() == SM_ERR_PREPARED);
  CHECK(sm_mkrel(s, "x") == -1);
  CHECK(sm_errno() == SM_ERR_PREPARED);
  CHECK(sm_begin(s, SM_XCT_REPEATABLE_READ) == -1);
  CHECK(sm_errno() == SM_ERR_INTRANS);

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(u, "b", "1") == 0);
  CHECK(sm_prepare(u, "g") == -1);
  CHECK(sm_errno() == SM_ERR_EXISTS);
  REQUIRE(sm_abort(u) == 0);
  CHECK(sm_commit_prepared(u, "nope") == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  REQUIRE(sm_commit(s) == 0);
  sm_closedb(s);
  sm_closedb(u);
}

TEST_CASE("another session can resolve a prepared transaction by gid") {
  // Verify resolution from any session (§8.5) and what the
  // preparing session sees afterwards.
  // 1. S prepares "g"; U commits it by gid.
  // 2. S has no transaction: sm_commit fails with NOTRANS, and
  //    sm_begin succeeds.
  // 3. U's own active transaction is undisturbed by a resolve.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm_prepare(s, "g") == 0);

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(u, "t", "a") == "0");
  REQUIRE(sm_commit_prepared(u, "g") == 0);
  CHECK(get(u, "t", "a") == "0"); // its snapshot predates the commit
  REQUIRE(sm_commit(u) == 0);

  CHECK(sm_commit(s) == -1);
  CHECK(sm_errno() == SM_ERR_NOTRANS);
  CHECK(read_now(s, "a") == "1");
  sm_closedb(s);
  sm_closedb(u);
}

TEST_CASE("a prepared transaction keeps its locks after its session closes") {
  // Verify an in-doubt transaction blocks writers (§8.4, §9.3).
  // 1. S updates "a", prepares "g", and closes: "g" stays in
  //    doubt and is listed with a sane prepared_at.
  // 2. U's update of "a" blocks.
  // 3. V commits "g" by gid; U's update then fails with
  //    SM_ERR_CONFLICT.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  long long before = (long long)time(nullptr);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm_prepare(s, "g") == 0);
  REQUIRE(sm_closedb(s) == 0);

  sm_session_t *u = f.open();
  sm_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g");
  CHECK(rows[0].second >= before);
  CHECK(rows[0].second <= (long long)time(nullptr));

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm_errno();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm_commit_prepared(v, "g") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == SM_ERR_CONFLICT);
  CHECK(read_now(v, "a") == "1");
  sm_closedb(u);
  sm_closedb(v);
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
  sm_session_t *s = f.open();
  sm_session_t *s2 = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  REQUIRE(sm_prepare(s, "gc") == 0);
  REQUIRE(sm_begin(s2, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s2, "b", "1") == 0);
  REQUIRE(sm_prepare(s2, "ga") == 0);
  auto before = listed(s);
  f.restart();

  sm_session_t *u = f.open();
  sm_session_t *v = f.open();
  CHECK(listed(v) == before);
  CHECK(read_now(v, "a") == "0");

  REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tu([&] {
    rc = set(u, "a", "u");
    err = sm_errno();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  REQUIRE(sm_commit_prepared(v, "gc") == 0);
  tu.join();
  CHECK(rc == -1);
  CHECK(err == SM_ERR_CONFLICT);
  REQUIRE(sm_abort_prepared(v, "ga") == 0);
  CHECK(read_now(v, "a") == "1");
  CHECK(read_now(v, "b") == "0");

  f.restart();
  sm_session_t *w = f.open();
  CHECK(listed(w).empty());
  CHECK(read_now(w, "a") == "1");
  CHECK(read_now(w, "b") == "0");
  sm_closedb(w);
}

TEST_CASE("read-only prepares survive a restart and resolve") {
  // Verify read-only prepares get distinct records (§8.4).
  // 1. Two read-only transactions prepare as "r1" and "r2".
  // 2. After a restart both are listed and both commit.
  tpc_fixture_t f;
  sm_session_t *s1 = f.open();
  sm_session_t *s2 = f.open();
  for (auto p : {std::make_pair(s1, "r1"), std::make_pair(s2, "r2")}) {
    REQUIRE(sm_begin(p.first, SM_XCT_SERIALIZABLE) == 0);
    CHECK(get(p.first, "t", "a") == "0");
    REQUIRE(sm_prepare(p.first, p.second) == 0);
  }
  f.restart();
  sm_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].first == "r1");
  CHECK(rows[1].first == "r2");
  CHECK(sm_commit_prepared(v, "r1") == 0);
  CHECK(sm_abort_prepared(v, "r2") == 0);
  CHECK(listed(v).empty());
  sm_closedb(v);
}

TEST_CASE("a prepared drop commits after a restart and its CF goes") {
  // Verify T.dropped survives in the record (§3.2, §12.5).
  // 1. Create "u"; prepare its drop as "gd"; restart.
  // 2. Commit "gd" by gid; "u" is gone by name.
  // 3. With no snapshot left to see it, GC drops its CF.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(s, "u") == 0);
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t relid = 0;
  REQUIRE(wisr::catalog_lookup(s, wisr::xct_view(s), "u", &relid) == 0);
  REQUIRE(sm_rmrel(s, "u") == 0);
  REQUIRE(sm_prepare(s, "gd") == 0);
  f.restart();

  sm_session_t *v = f.open();
  CHECK(wisr::db_rel_cf(v->db, relid) != nullptr);
  REQUIRE(sm_commit_prepared(v, "gd") == 0);
  REQUIRE(sm_begin(v, SM_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm_read(v, "u", "k", 1, &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  REQUIRE(sm_commit(v) == 0);
  wisr::gc_run_once(v->db);
  CHECK(wisr::db_rel_cf(v->db, relid) == nullptr);
  sm_closedb(v);
}

TEST_CASE("write skew with a transaction restored in doubt is still caught") {
  // Verify §8.4's restored rule: its SIREAD entries are lost.
  // 1. T_prep (SERIALIZABLE) reads "a", updates "b", prepares.
  // 2. Restart: T_prep is restored without its SSI state.
  // 3. T_new (SERIALIZABLE) reads "b", stepping past T_prep's
  //    version, and updates "a", which T_prep read.
  // 4. T_new fails at commit; T_prep then commits.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(s, "t", "a") == "0");
  REQUIRE(set(s, "b", "1") == 0);
  REQUIRE(sm_prepare(s, "gp") == 0);
  f.restart();

  sm_session_t *t = f.open();
  REQUIRE(sm_begin(t, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t, "t", "b") == "0");
  REQUIRE(set(t, "a", "1") == 0);
  CHECK(sm_commit(t) == -1);
  CHECK(sm_errno() == SM_ERR_SERIALIZATION);
  REQUIRE(sm_commit_prepared(t, "gp") == 0);
  CHECK(read_now(t, "b") == "1");
  CHECK(read_now(t, "a") == "0");
  sm_closedb(t);
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

  sm_session_t *v = f.open();
  auto rows = listed(v);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "g-crash");
  CHECK(read_now(v, "c") == "<none>");
  REQUIRE(sm_commit_prepared(v, "g-crash") == 0);
  CHECK(read_now(v, "c") == "v");
  sm_closedb(v);
}

TEST_CASE("startup drops relation CFs that nothing names") {
  // Verify the orphan CF sweep (§9.1 step 6).
  // 1. Create "u" and commit. A child drops "u" and commits,
  //    then creates "o" without committing, and is killed.
  // 2. Reattach: neither "u" nor "o" exists by name, and the
  //    only relation CFs left are the catalog's and "t"'s.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(s, "u") == 0);
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t trelid = 0;
  REQUIRE(wisr::catalog_lookup(s, wisr::xct_view(s), "t", &trelid) == 0);
  REQUIRE(sm_commit(s) == 0);
  f.home().detach();

  int fd;
  pid_t pid = spawn_child("orphan", f.home().path(), &fd);
  kill_after_report(pid, fd);
  f.home().attach();

  sm_session_t *v = f.open();
  std::set<uint64_t> rels;
  for (auto &kv : v->db->rels) {
    rels.insert(kv.first);
  }
  CHECK(rels == std::set<uint64_t>{0, trelid});
  REQUIRE(sm_begin(v, SM_XCT_REPEATABLE_READ) == 0);
  const char *d;
  size_t dl;
  CHECK(sm_read(v, "u", "k", 1, &d, &dl) == -1);
  CHECK(sm_read(v, "o", "k", 1, &d, &dl) == -1);
  REQUIRE(sm_commit(v) == 0);
  sm_closedb(v);
}

TEST_CASE("a failed prepare write fails the database stop") {
  // Verify §8.4's fail-stop on the PREPARED record.
  // 1. The prepare write of "gx" fails: SM_ERR_IO, and the
  //    session has no transaction left.
  // 2. "gx" is not listed and cannot be resolved; writes fail.
  // 3. After a restart the record never existed: nothing is
  //    listed and the update of "a" is gone.
  tpc_fixture_t f;
  sm_session_t *s = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(set(s, "a", "1") == 0);
  s->db->test_before_prepare = [] {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(sm_prepare(s, "gx") == -1);
  CHECK(sm_errno() == SM_ERR_IO);
  s->db->test_before_prepare = nullptr;
  CHECK(listed(s).empty());
  CHECK(sm_commit_prepared(s, "gx") == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(set(s, "b", "1") == -1);
  CHECK(sm_errno() == SM_ERR_IO);
  REQUIRE(sm_commit(s) == 0);

  f.restart();
  sm_session_t *v = f.open();
  CHECK(listed(v).empty());
  CHECK(read_now(v, "a") == "0");
  sm_closedb(v);
}
