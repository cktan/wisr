/*
 * test_seq.cc -- sm_nextseq: a persistent, strictly increasing,
 * non-transactional per-database sequence starting at
 * SM_SEQ_FIRST.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A database "db", opened with opt, reopenable. */
class seq_fixture_t {
public:
  /* Create "db" and open it with opt. */
  explicit seq_fixture_t(const sm_options_t *opt = nullptr) : m_opt(opt) {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
  }

  /* The fixture's session. */
  sm_session_t *s() const { return m_s; }

  /* The home directory. */
  home_t &home() { return m_home; }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", m_opt);
    REQUIRE_MESSAGE(s, sm_errmsg());
    return s;
  }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm_closedb(m_s) == 0);
    m_home.reattach();
    m_s = open();
  }

private:
  home_t m_home;
  const sm_options_t *m_opt;
  sm_session_t *m_s = nullptr;
};

/* The next value, failing the test on an error. */
uint64_t next(sm_session_t *s) {
  uint64_t v = 0;
  REQUIRE_MESSAGE(sm_nextseq(s, &v) == 0, sm_errmsg());
  return v;
}

} // namespace

/*
 * Child: hand out values forever, writing each to stdout after
 * sm_nextseq returns it.
 */
int seq_child_main(const char *homedir) {
  sm_env_t *env = sm_attach(homedir);
  sm_session_t *s = env ? sm_opendb(env, "db", nullptr) : nullptr;
  if (!s) {
    return 2;
  }
  for (;;) {
    uint64_t v;
    if (sm_nextseq(s, &v) != 0 || write(1, &v, sizeof(v)) != sizeof(v)) {
      return 3;
    }
  }
}

TEST_CASE("the sequence starts at SM_SEQ_FIRST and strictly increases") {
  // Verify the basic contract.
  // 1. The first value in a new database is SM_SEQ_FIRST.
  // 2. Each later value is greater, with or without an active
  //    transaction.
  // 3. NULL arguments fail with SM_ERR_INVALID.
  seq_fixture_t f;
  sm_session_t *s = f.s();
  uint64_t v = next(s);
  CHECK(v == SM_SEQ_FIRST);
  for (int i = 0; i < 100; i++) {
    uint64_t w = next(s);
    CHECK(w > v);
    v = w;
  }
  REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(next(s) > v);
  REQUIRE(sm_commit(s) == 0);
  uint64_t out;
  CHECK(sm_nextseq(nullptr, &out) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_nextseq(s, nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
}

TEST_CASE("an abort does not give a sequence value back") {
  // Verify the sequence is not transactional.
  // 1. Take a value inside a transaction, then abort it.
  // 2. The next value is still greater.
  seq_fixture_t f;
  sm_session_t *s = f.s();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t v = next(s);
  REQUIRE(sm_abort(s) == 0);
  CHECK(next(s) > v);
}

TEST_CASE("each database has its own sequence") {
  // Verify the sequence is per database.
  // 1. Two databases both start at SM_SEQ_FIRST.
  // 2. Taking values from one does not advance the other.
  seq_fixture_t f;
  REQUIRE(sm_mkdb(f.home().env(), "db2", nullptr) == 0);
  sm_session_t *t = sm_opendb(f.home().env(), "db2", nullptr);
  REQUIRE(t);
  CHECK(next(f.s()) == SM_SEQ_FIRST);
  CHECK(next(f.s()) == SM_SEQ_FIRST + 1);
  CHECK(next(t) == SM_SEQ_FIRST);
  sm_closedb(t);
}

TEST_CASE("the sequence survives reopening, across many leases") {
  // Verify persistence, with a tiny lease so it renews often.
  // 1. With ts_lease = 3, take 20 values, then reopen, five
  //    times over.
  // 2. Every value is greater than every value before it.
  sm_options_t opt;
  sm_options_init(&opt);
  opt.ts_lease = 3;
  seq_fixture_t f(&opt);
  uint64_t last = 0;
  for (int round = 0; round < 5; round++) {
    CAPTURE(round);
    for (int i = 0; i < 20; i++) {
      uint64_t v = next(f.s());
      CHECK(v > last);
      last = v;
    }
    f.reopen();
  }
}

TEST_CASE("concurrent sessions never receive the same value") {
  // Verify uniqueness under concurrency.
  // 1. Four threads, each on its own session, take 2000 values.
  // 2. Each thread's values increase, and all 8000 are distinct.
  seq_fixture_t f;
  std::vector<sm_session_t *> sessions;
  for (int t = 0; t < 4; t++) {
    sessions.push_back(f.open());
  }
  std::mutex mu;
  std::set<uint64_t> all;
  std::vector<std::thread> threads;
  int out_of_order = 0;
  for (sm_session_t *s : sessions) {
    threads.emplace_back([&, s] {
      std::vector<uint64_t> mine;
      for (int i = 0; i < 2000; i++) {
        uint64_t v = 0;
        sm_nextseq(s, &v);
        mine.push_back(v);
      }
      std::lock_guard<std::mutex> guard(mu);
      for (size_t i = 1; i < mine.size(); i++) {
        out_of_order += mine[i] <= mine[i - 1];
      }
      all.insert(mine.begin(), mine.end());
    });
  }
  for (std::thread &t : threads) {
    t.join();
  }
  CHECK(out_of_order == 0);
  CHECK(all.size() == 8000);
  for (sm_session_t *s : sessions) {
    sm_closedb(s);
  }
}

TEST_CASE("a stopped database hands out no sequence values") {
  // Verify the failed state (§8.2) covers the sequence.
  // 1. Make a commit's status write fail.
  // 2. sm_nextseq then fails with SM_ERR_IO.
  seq_fixture_t f;
  sm_session_t *s = f.s();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  REQUIRE(sm_mkrel(s, 1) == 0);
  s->db->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(sm_commit(s) == -1);
  s->db->test_before_commit = nullptr;
  uint64_t v;
  CHECK(sm_nextseq(s, &v) == -1);
  CHECK(sm_errno() == SM_ERR_IO);
}

TEST_CASE(
    "a database made before the sequence existed starts it at SM_SEQ_FIRST") {
  // Verify older databases, whose CF:meta has no sequence lease.
  // 1. Delete the lease from CF:meta, as in a database made
  //    before sm_nextseq, and reopen.
  // 2. The first value is SM_SEQ_FIRST, and the next is greater.
  seq_fixture_t f;
  REQUIRE(f.s()
              ->db->rdb
              ->Delete(rocksdb::WriteOptions(), f.s()->db->cf_meta,
                       WISR_META_NEXT_SEQ)
              .ok());
  f.reopen();
  CHECK(next(f.s()) == SM_SEQ_FIRST);
  CHECK(next(f.s()) == SM_SEQ_FIRST + 1);
}

TEST_CASE("values handed out before a kill are never handed out again") {
  // Verify persistence against SIGKILL.
  // 1. A child process takes values as fast as it can and
  //    reports each; kill it after 10000.
  // 2. Reattach: the next value is greater than every value the
  //    child reported.
  seq_fixture_t f;
  REQUIRE(sm_closedb(f.s()) == 0);
  f.home().detach();
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: stdout becomes the pipe, then exec.
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    execl("/proc/self/exe", "test_wisr", "--seq-child", f.home().path(),
          (char *)nullptr);
    _exit(127);
  }
  close(fds[1]);
  uint64_t v = 0, highest = 0;
  int got = 0;
  while (got < 10000 && read(fds[0], &v, sizeof(v)) == sizeof(v)) {
    highest = v;
    got++;
  }
  REQUIRE(got == 10000);
  kill(pid, SIGKILL);
  int status;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  while (read(fds[0], &v, sizeof(v)) == sizeof(v)) {
    highest = v;
  }
  close(fds[0]);

  f.home().attach();
  sm_session_t *s = f.open();
  CHECK(next(s) > highest);
  sm_closedb(s);
}
