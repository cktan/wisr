/*
 * test_env.cc -- sm1_init, sm1_attach, sm1_detach, and the error
 * state they report through.
 */

#include "doctest.h"
#include "util.h"

#include "codec.h"
#include "env.h"
#include "sm1.h"

#include <sys/stat.h>

#include <filesystem>
#include <new>
#include <string>

#include <rocksdb/db.h>

TEST_CASE("sm1_init refuses a missing, non-directory, or empty homedir") {
  // Verify sm1_init validates homedir before creating anything.
  // 1. A path that does not exist fails with SM_ERR_NOTFOUND.
  // 2. A regular file fails with SM_ERR_INVALID.
  // 3. NULL and "" fail with SM_ERR_INVALID.
  tmpdir_t tmp;
  std::string missing = std::string(tmp.path()) + "/nope";
  CHECK(sm1_init(missing.c_str()) == -1);
  CHECK(sm1_errno() == SM_ERR_NOTFOUND);

  std::string file = std::string(tmp.path()) + "/file";
  FILE *f = fopen(file.c_str(), "w");
  REQUIRE(f);
  fclose(f);
  CHECK(sm1_init(file.c_str()) == -1);
  CHECK(sm1_errno() == SM_ERR_INVALID);

  CHECK(sm1_init(nullptr) == -1);
  CHECK(sm1_errno() == SM_ERR_INVALID);
  CHECK(sm1_init("") == -1);
  CHECK(sm1_errno() == SM_ERR_INVALID);
}

namespace {

/* A host process's own out-of-memory policy, for the test below. */
void host_oom_handler() { throw std::bad_alloc(); }

} // namespace

TEST_CASE(
    "sm1_attach makes running out of memory fatal, unless the host decided") {
  // Verify sm1_attach installs its out-of-memory handler only
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

TEST_CASE("sm1_init runs once per home directory") {
  // Verify a second sm1_init reports the home as existing.
  // 1. Initialize a fresh directory.
  // 2. Initialize it again and expect SM_ERR_EXISTS.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  CHECK(sm1_init(tmp.path()) == -1);
  CHECK(sm1_errno() == SM_ERR_EXISTS);
}

TEST_CASE("sm1_attach requires an initialized home directory") {
  // Verify attach creates nothing on an uninitialized directory.
  // 1. Attach to an empty directory; expect SM_ERR_NOTFOUND.
  // 2. Confirm no global/ was created as a side effect.
  tmpdir_t tmp;
  CHECK(sm1_attach(tmp.path()) == nullptr);
  CHECK(sm1_errno() == SM_ERR_NOTFOUND);
  CHECK_FALSE(std::filesystem::exists(std::string(tmp.path()) + "/global"));
}

TEST_CASE("a home directory can be attached again after detach") {
  // Verify detach releases everything a later attach needs.
  // 1. Initialize and attach.
  // 2. Detach, then attach again successfully.
  // One environment at a time is the caller's rule to keep;
  // the library does not enforce it, so it is not tested here.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  sm1_env_t *env = sm1_attach(tmp.path());
  REQUIRE(env);

  CHECK(sm1_detach(env, 0) == 0);
  env = sm1_attach(tmp.path());
  CHECK(env);
  CHECK(sm1_detach(env, 0) == 0);
}

TEST_CASE("sm1_detach of NULL is a no-op") {
  // Verify sm1_detach(NULL) succeeds whatever force says.
  // 1. Detach NULL with force 0 and force 1; both return 0.
  CHECK(sm1_detach(nullptr, 0) == 0);
  CHECK(sm1_detach(nullptr, 1) == 0);
}

TEST_CASE("sm1_attach rejects an unknown format version") {
  // Verify attach refuses a home written by another format.
  // 1. Initialize a home directory.
  // 2. Overwrite the root db's version key with 99.
  // 3. Attach and expect SM_ERR_VERSION.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  {
    rocksdb::DB *root = nullptr;
    std::string rootpath = std::string(tmp.path()) + "/global/root";
    REQUIRE(rocksdb::DB::Open(rocksdb::Options(), rootpath, &root).ok());
    std::string v;
    wisr::put_be64(v, 99);
    REQUIRE(root->Put(rocksdb::WriteOptions(), WISR_ROOT_VERSION, v).ok());
    delete root;
  }
  CHECK(sm1_attach(tmp.path()) == nullptr);
  CHECK(sm1_errno() == SM_ERR_VERSION);
}

TEST_CASE("sm1_attach sweeps database directories no name points at") {
  // Verify attach destroys unnamed dbid directories and nothing else.
  // 1. Initialize, then plant base/7 with a file, and base/notes.
  // 2. Attach.
  // 3. Confirm base/7 is gone and base/notes is untouched.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  std::string base = std::string(tmp.path()) + "/base";
  REQUIRE(mkdir((base + "/7").c_str(), 0700) == 0);
  FILE *f = fopen((base + "/7/junk").c_str(), "w");
  REQUIRE(f);
  fclose(f);
  REQUIRE(mkdir((base + "/notes").c_str(), 0700) == 0);

  sm1_env_t *env = sm1_attach(tmp.path());
  REQUIRE(env);
  CHECK_FALSE(std::filesystem::exists(base + "/7"));
  CHECK(std::filesystem::exists(base + "/notes"));
  CHECK(sm1_detach(env, 0) == 0);
}

TEST_CASE("a successful call leaves the error state untouched") {
  // Verify success does not clear sm1_errno or sm1_errmsg.
  // 1. Provoke SM_ERR_INVALID and save the message.
  // 2. Make a successful call.
  // 3. Confirm the code and message are unchanged.
  tmpdir_t tmp;
  CHECK(sm1_init(nullptr) == -1);
  std::string msg = sm1_errmsg();
  REQUIRE(sm1_init(tmp.path()) == 0);
  CHECK(sm1_errno() == SM_ERR_INVALID);
  CHECK(msg == sm1_errmsg());
  CHECK(sm1_retryable() == 0);
}
