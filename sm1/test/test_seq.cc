/*
 * test_seq.cc -- named sequences: sm1::mkseq and sm1::rmseq are
 * transactional, while the values sm1::nextseq hands out are
 * persistent, strictly increasing, and not.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
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
  explicit seq_fixture_t(const sm1::options_t *opt = nullptr) : m_opt(opt) {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
  }

  /* The fixture's session. */
  sm1::session_t *s() const { return m_s; }

  /* The home directory. */
  home_t &home() { return m_home; }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", m_opt);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm1::closedb(m_s) == 0);
    m_home.reattach();
    m_s = open();
  }

private:
  home_t m_home;
  const sm1::options_t *m_opt;
  sm1::session_t *m_s = nullptr;
};

/* The number of sequence leases in the database's CF:meta. */
int meta_seq_keys(sm1::session_t *s) {
  std::unique_ptr<rocksdb::Iterator> it(
      s->db->rdb->NewIterator(rocksdb::ReadOptions(), s->db->cf_meta));
  int n = 0;
  for (it->Seek(WISR_META_SEQ_PREFIX); it->Valid(); it->Next()) {
    if (!it->key().starts_with(WISR_META_SEQ_PREFIX)) {
      break;
    }
    n++;
  }
  return n;
}

/* The sequence every test uses unless it makes its own. */
const char *const SEQ = "s";

/* Create a sequence in its own committed transaction. */
void commit_mkseq(sm1::session_t *s, const char *name, uint64_t start,
                  uint64_t increment) {
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE_MESSAGE(sm1::mkseq(s, name, start, increment) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);
}

/* The next value of name, failing the test on an error. */
uint64_t next(sm1::session_t *s, const char *name = SEQ) {
  uint64_t v = 0;
  REQUIRE_MESSAGE(sm1::nextseq(s, name, &v) == 0, sm1::errmsg());
  return v;
}

} // namespace

/*
 * Child: hand out values forever, writing each to stdout after
 * sm1::nextseq returns it.
 */
int seq_child_main(const char *homedir) {
  sm1::env_t *env = attach(homedir);
  sm1::session_t *s = env ? opendb(env, "db", nullptr) : nullptr;
  if (!s) {
    return 2;
  }
  for (;;) {
    uint64_t v;
    if (sm1::nextseq(s, "s", &v) != 0 || write(1, &v, sizeof(v)) != sizeof(v)) {
      return 3;
    }
  }
}

TEST_CASE("a sequence starts at its start and steps by its increment") {
  // Verify the basic contract, and that sequences are separate.
  // 1. The first value of a sequence is its start; each later
  //    value is one increment higher, with or without an active
  //    transaction.
  // 2. A second sequence keeps its own position.
  // 3. NULL arguments fail with SM_ERR_INVALID.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  commit_mkseq(s, SEQ, 1000, 1);
  commit_mkseq(s, "by7", 5, 7);
  uint64_t v = next(s);
  CHECK(v == 1000);
  for (int i = 0; i < 100; i++) {
    uint64_t w = next(s);
    CHECK(w == v + 1);
    v = w;
  }
  CHECK(next(s, "by7") == 5);
  CHECK(next(s, "by7") == 12);
  CHECK(next(s, "by7") == 19);
  CHECK(next(s) == v + 1);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::serializable) == 0);
  CHECK(next(s) == v + 2);
  REQUIRE(sm1::commit(s) == 0);
  uint64_t out;
  CHECK(sm1::nextseq(nullptr, SEQ, &out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::nextseq(s, SEQ, nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::nextseq(s, nullptr, &out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("sm1_mkseq and sm1_rmseq are transactional; the values are not") {
  // Verify the split: the definition follows the transaction,
  // the position does not.
  // 1. A sequence created but not committed is usable at once
  //    by its own transaction, and gone after an abort.
  // 2. Created and committed, it is usable by another session.
  // 3. Dropped and committed, sm1::nextseq fails with
  //    SM_ERR_NOTFOUND; created again under the same name, it
  //    starts over at its start.
  // 4. sm1::mkseq of a live name fails with SM_ERR_EXISTS,
  //    sm1::rmseq of an unknown one with SM_ERR_NOTFOUND, and
  //    increment 0 with SM_ERR_INVALID.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  sm1::session_t *o = f.open();
  uint64_t v = 0;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::mkseq(s, SEQ, 100, 1) == 0);
  CHECK(next(s) == 100);
  CHECK(sm1::nextseq(o, SEQ, &v) == -1); // not committed yet
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::abort(s) == 0);
  CHECK(sm1::nextseq(s, SEQ, &v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  commit_mkseq(s, SEQ, 100, 1);
  CHECK(next(o) == 100);
  CHECK(next(s) == 101);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm1::mkseq(s, SEQ, 1, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
  CHECK(sm1::rmseq(s, "nope") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm1::mkseq(s, "zero", 1, 0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::rmseq(s, SEQ) == 0);
  REQUIRE(sm1::commit(s) == 0);
  CHECK(sm1::nextseq(s, SEQ, &v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  commit_mkseq(s, SEQ, 100, 1);
  CHECK(next(s) == 100); // a new sequence, starting over
  sm1::closedb(o);
}

TEST_CASE("dropping a sequence deletes its lease from CF:meta") {
  // Verify the commit-time cleanup of a dropped sequence.
  // 1. Draw a value, which persists a lease keyed by seqid.
  // 2. Drop the sequence and commit: the lease is gone, so
  //    CF:meta keeps nothing for it.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  commit_mkseq(s, SEQ, 1000, 1);
  CHECK(next(s) == 1000);
  CHECK(meta_seq_keys(s) == 1);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::rmseq(s, SEQ) == 0);
  CHECK(meta_seq_keys(s) == 1); // not until it commits
  REQUIRE(sm1::commit(s) == 0);
  CHECK(meta_seq_keys(s) == 0);
}

TEST_CASE("a sequence hands out its last value, then fails") {
  // Verify the end of the range, where a step would wrap.
  // 1. A sequence starting two steps below UINT64_MAX hands
  //    out both values.
  // 2. The next call fails with SM_ERR_INVALID, and keeps
  //    failing.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  commit_mkseq(s, "end", UINT64_MAX - 1, 1);
  CHECK(next(s, "end") == UINT64_MAX - 1);
  CHECK(next(s, "end") == UINT64_MAX);
  uint64_t v = 0;
  CHECK(sm1::nextseq(s, "end", &v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::nextseq(s, "end", &v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("an abort does not give a sequence value back") {
  // Verify the values are not transactional.
  // 1. Take a value inside a transaction, then abort it.
  // 2. The next value is still greater.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  commit_mkseq(s, SEQ, 1000, 1);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t v = next(s);
  REQUIRE(sm1::abort(s) == 0);
  CHECK(next(s) > v);
}

TEST_CASE("sequences of the same name in two databases are separate") {
  // Verify a sequence belongs to its database.
  // 1. Both databases have a sequence "s" starting at 1000.
  // 2. Taking values from one does not advance the other.
  seq_fixture_t f;
  REQUIRE(sm1::mkdb(f.home().env(), "db2", nullptr) == 0);
  sm1::session_t *t = opendb(f.home().env(), "db2", nullptr);
  REQUIRE(t);
  commit_mkseq(f.s(), SEQ, 1000, 1);
  commit_mkseq(t, SEQ, 1000, 1);
  CHECK(next(f.s()) == 1000);
  CHECK(next(f.s()) == 1001);
  CHECK(next(t) == 1000);
  sm1::closedb(t);
}

TEST_CASE("the sequence survives reopening, across many leases") {
  // Verify persistence, with a tiny lease so it renews often.
  // 1. With ts_lease = 3, take 20 values, then reopen, five
  //    times over.
  // 2. Every value is greater than every value before it.
  sm1::options_t opt;
  opt.ts_lease = 3;
  seq_fixture_t f(&opt);
  commit_mkseq(f.s(), SEQ, 1000, 1);
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
  commit_mkseq(f.s(), SEQ, 1000, 1);
  std::vector<sm1::session_t *> sessions;
  for (int t = 0; t < 4; t++) {
    sessions.push_back(f.open());
  }
  std::mutex mu;
  std::set<uint64_t> all;
  std::vector<std::thread> threads;
  int out_of_order = 0;
  for (sm1::session_t *s : sessions) {
    threads.emplace_back([&, s] {
      std::vector<uint64_t> mine;
      for (int i = 0; i < 2000; i++) {
        uint64_t v = 0;
        sm1::nextseq(s, SEQ, &v);
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
  for (sm1::session_t *s : sessions) {
    sm1::closedb(s);
  }
}

TEST_CASE("a stopped database hands out no sequence values") {
  // Verify the failed state (§8.2) covers the sequence.
  // 1. Make a commit's status write fail.
  // 2. sm1::nextseq then fails with SM_ERR_IO.
  seq_fixture_t f;
  sm1::session_t *s = f.s();
  commit_mkseq(s, SEQ, 1000, 1);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t storeid = 0;
  REQUIRE(sm1::mkstore(s, &storeid) == 0);
  s->db->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  CHECK(sm1::commit(s) == -1);
  s->db->test_before_commit = nullptr;
  uint64_t v;
  CHECK(sm1::nextseq(s, SEQ, &v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_IO);
}

TEST_CASE("values handed out before a kill are never handed out again") {
  // Verify persistence against SIGKILL.
  // 1. A child process takes values as fast as it can and
  //    reports each; kill it after 10000.
  // 2. Reattach: the next value is greater than every value the
  //    child reported.
  seq_fixture_t f;
  commit_mkseq(f.s(), SEQ, 1000, 1);
  REQUIRE(sm1::closedb(f.s()) == 0);
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
  sm1::session_t *s = f.open();
  CHECK(next(s) > highest);
  sm1::closedb(s);
}
