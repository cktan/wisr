/*
 * test_env.cc -- sm1::init, sm1::attach, sm1::detach, and the error
 * state they report through.
 */

#include "doctest.h"
#include "util.h"

#include "env.h"
#include "oom.h"
#include "sm1.h"
#include "sm1_codec.h"

#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <new>
#include <string>

#include <rocksdb/db.h>

TEST_CASE("sm1_init refuses a missing, non-directory, or empty homedir") {
  // Verify sm1::init validates homedir before creating anything.
  // 1. A path that does not exist fails with SM_ERR_NOTFOUND.
  // 2. A regular file fails with SM_ERR_INVALID.
  // 3. A homedir holding a NUL byte, and "", fail with
  //    SM_ERR_INVALID.
  tmpdir_t tmp;
  std::string missing = std::string(tmp.path()) + "/nope";
  CHECK(sm1::init(missing.c_str()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  std::string file = std::string(tmp.path()) + "/file";
  FILE *f = fopen(file.c_str(), "w");
  REQUIRE(f);
  fclose(f);
  CHECK(sm1::init(file.c_str()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  CHECK(sm1::init(std::string_view("a\0b", 3)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::init("") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

namespace {

/* A host process's own out-of-memory policy, for the test below. */
void host_oom_handler() { throw std::bad_alloc(); }

/*
 * Run `test_wisr --oom-child homedir` and wait for it; returns its
 * wait status, with everything it wrote to stderr in err.
 */
int run_oom_child(const char *homedir, std::string &err) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: stderr becomes the pipe, then exec.
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    execl("/proc/self/exe", "test_wisr", "--oom-child", homedir,
          (char *)nullptr);
    _exit(127);
  }
  close(fds[1]);
  // Collect its stderr until it exits.
  char buf[512];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
    err.append(buf, n);
  }
  close(fds[0]);
  int status;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  return status;
}

} // namespace

/*
 * The out-of-memory child: attach, so sm1's new-handler is
 * installed, then run out of memory inside sm1. Never returns.
 */
int oom_child_main(const char *homedir) {
  sm1::env_t *env;
  if (sm1::attach(homedir, env) != 0) {
    return 2;
  }
  // Fail an allocation while marked inside sm1.
  wisr::in_sm1_t in_sm1;
  std::get_new_handler()();
  return 3;
}

TEST_CASE(
    "sm1_attach makes running out of memory fatal, unless the host decided") {
  // Verify sm1::attach installs its out-of-memory handler only
  // when the process has none (the top-level DESIGN.md §5).
  // 1. With no new-handler set, attaching installs one.
  // 2. With the host's own handler set, attaching keeps it.
  // 3. Restore the handler the test binary had.
  std::new_handler saved = std::get_new_handler();
  std::set_new_handler(nullptr);
  {
    home_t home;
    CHECK(std::get_new_handler() != nullptr);
  }
  std::set_new_handler(host_oom_handler);
  {
    home_t home;
    CHECK(std::get_new_handler() == host_oom_handler);
  }
  std::set_new_handler(saved);
}

TEST_CASE("running out of memory outside sm1 throws, for sm2 to report") {
  // Verify the handler sm1::attach installs leaves a failure
  // outside sm1 to the caller (the top-level DESIGN.md §5).
  // 1. Attach with no new-handler set, so sm1's is installed.
  // 2. Outside any sm1 call, the handler throws std::bad_alloc.
  // 3. Restore the handler the test binary had.
  std::new_handler saved = std::get_new_handler();
  std::set_new_handler(nullptr);
  {
    home_t home;
    std::new_handler h = std::get_new_handler();
    REQUIRE(h != nullptr);
    CHECK_THROWS_AS(h(), std::bad_alloc);
  }
  std::set_new_handler(saved);
}

TEST_CASE("running out of memory inside sm1 aborts the process, loudly") {
  // Verify the handler is fatal on a thread inside sm1, in a child
  // process, since it aborts.
  // 1. Initialize a home directory for the child.
  // 2. The child attaches, marks itself inside sm1, and runs out
  //    of memory.
  // 3. It dies of SIGABRT, having said *** FATAL *** on stderr.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  std::string err;
  int status = run_oom_child(tmp.path(), err);
  CAPTURE(err);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
  CHECK(err.find("*** FATAL *** out of memory") != std::string::npos);
}

TEST_CASE("sm1_init runs once per home directory") {
  // Verify a second sm1::init reports the home as existing.
  // 1. Initialize a fresh directory.
  // 2. Initialize it again and expect SM_ERR_EXISTS.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  CHECK(sm1::init(tmp.path()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
}

TEST_CASE("sm1_attach requires an initialized home directory") {
  // Verify attach creates nothing on an uninitialized directory.
  // 1. Attach to an empty directory; expect SM_ERR_NOTFOUND.
  // 2. Confirm no global/ was created as a side effect.
  tmpdir_t tmp;
  CHECK(attach(tmp.path()) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK_FALSE(std::filesystem::exists(std::string(tmp.path()) + "/global"));
}

TEST_CASE("a home directory can be attached again after detach") {
  // Verify detach releases everything a later attach needs.
  // 1. Initialize and attach.
  // 2. Detach, then attach again successfully.
  // One environment at a time is the caller's rule to keep;
  // the library does not enforce it, so it is not tested here.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  sm1::env_t *env = attach(tmp.path());
  REQUIRE(env);

  CHECK(sm1::detach(env, 0) == 0);
  env = attach(tmp.path());
  CHECK(env);
  CHECK(sm1::detach(env, 0) == 0);
}

TEST_CASE("sm1_detach of NULL is a no-op") {
  // Verify sm1::detach(NULL) succeeds whatever force says.
  // 1. Detach NULL with force 0 and force 1; both return 0.
  CHECK(sm1::detach(nullptr, 0) == 0);
  CHECK(sm1::detach(nullptr, 1) == 0);
}

TEST_CASE("sm1_attach rejects an unknown format version") {
  // Verify attach refuses a home written by another format.
  // 1. Initialize a home directory.
  // 2. Overwrite the root db's version key with 99.
  // 3. Attach and expect SM_ERR_VERSION.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  {
    rocksdb::DB *root = nullptr;
    std::string rootpath = std::string(tmp.path()) + "/global/root";
    REQUIRE(rocksdb::DB::Open(rocksdb::Options(), rootpath, &root).ok());
    std::string v;
    sm1::put_be64(v, 99);
    REQUIRE(root->Put(rocksdb::WriteOptions(), WISR_ROOT_VERSION, v).ok());
    delete root;
  }
  CHECK(attach(tmp.path()) == nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_VERSION);
}

TEST_CASE("sm1_attach sweeps database directories no name points at") {
  // Verify attach destroys unnamed dbid directories and nothing else.
  // 1. Initialize, then plant base/7 with a file, and base/notes.
  // 2. Attach.
  // 3. Confirm base/7 is gone and base/notes is untouched.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  std::string base = std::string(tmp.path()) + "/base";
  REQUIRE(mkdir((base + "/7").c_str(), 0700) == 0);
  FILE *f = fopen((base + "/7/junk").c_str(), "w");
  REQUIRE(f);
  fclose(f);
  REQUIRE(mkdir((base + "/notes").c_str(), 0700) == 0);

  sm1::env_t *env = attach(tmp.path());
  REQUIRE(env);
  CHECK_FALSE(std::filesystem::exists(base + "/7"));
  CHECK(std::filesystem::exists(base + "/notes"));
  CHECK(sm1::detach(env, 0) == 0);
}

TEST_CASE("a successful call leaves the error state untouched") {
  // Verify success does not clear sm1::errnum or sm1::errmsg.
  // 1. Provoke SM_ERR_INVALID and save the message.
  // 2. Make a successful call.
  // 3. Confirm the code and message are unchanged.
  tmpdir_t tmp;
  CHECK(sm1::init("") == -1);
  std::string msg = sm1::errmsg();
  REQUIRE(sm1::init(tmp.path()) == 0);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(msg == sm1::errmsg());
  CHECK(sm1::retryable() == 0);
}

TEST_CASE("seterr, reseterr and errname drive the thread's error state") {
  // Verify an upper level can record, clear and name an error
  // through sm1's one errno namespace (the top-level DESIGN.md §5).
  // 1. seterr records a code and message and returns -1; errname
  //    names the code without its SM_ prefix, and retryable
  //    follows the code.
  // 2. A message longer than the buffer is cut short, and stays
  //    NUL-terminated.
  // 3. Every code from SM_ERR_INVALID to SM_ERR_UPDATED has a name.
  // 4. reseterr restores SM_OK, "no error", and the name "OK".
  CHECK(sm1::seterr(sm1::SM_ERR_UPDATED, "newer commit") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(std::string(sm1::errmsg()) == "newer commit");
  CHECK(std::string(sm1::errname()) == "ERR_UPDATED");
  CHECK(sm1::retryable() == 0);
  CHECK(sm1::seterr(sm1::SM_ERR_DEADLOCK, "victim") == -1);
  CHECK(std::string(sm1::errname()) == "ERR_DEADLOCK");
  CHECK(sm1::retryable() == 1);

  const std::string big(100000, 'x');
  sm1::seterr(sm1::SM_ERR_IO, big);
  size_t n = strlen(sm1::errmsg());
  CHECK(n > 0);
  CHECK(n < big.size());

  for (int c = sm1::SM_ERR_INVALID; c <= sm1::SM_ERR_KEYTOOLONG; c++) {
    sm1::seterr(static_cast<sm1::errnum_t>(c), "x");
    CHECK(std::string(sm1::errname()).rfind("ERR_", 0) == 0);
  }

  sm1::reseterr();
  CHECK(sm1::errnum() == sm1::SM_OK);
  CHECK(std::string(sm1::errmsg()) == "no error");
  CHECK(std::string(sm1::errname()) == "OK");
}
