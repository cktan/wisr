/*
 * test_env.cc -- sm_init, sm_attach, sm_detach, and the error
 * state they report through.
 */

#include "doctest.h"
#include "util.h"

#include "codec.h"
#include "env.h"
#include "sm.h"

#include <sys/stat.h>

#include <filesystem>
#include <string>

#include <rocksdb/db.h>

TEST_CASE("sm_init refuses a missing, non-directory, or empty homedir")
{
    // Verify sm_init validates homedir before creating anything.
    // 1. A path that does not exist fails with SM_ERR_NOTFOUND.
    // 2. A regular file fails with SM_ERR_INVALID.
    // 3. NULL and "" fail with SM_ERR_INVALID.
    tmpdir_t tmp;
    std::string missing = std::string(tmp.path()) + "/nope";
    CHECK(sm_init(missing.c_str()) == -1);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);

    std::string file = std::string(tmp.path()) + "/file";
    FILE* f = fopen(file.c_str(), "w");
    REQUIRE(f);
    fclose(f);
    CHECK(sm_init(file.c_str()) == -1);
    CHECK(sm_errno() == SM_ERR_INVALID);

    CHECK(sm_init(nullptr) == -1);
    CHECK(sm_errno() == SM_ERR_INVALID);
    CHECK(sm_init("") == -1);
    CHECK(sm_errno() == SM_ERR_INVALID);
}

TEST_CASE("sm_init runs once per home directory")
{
    // Verify a second sm_init reports the home as existing.
    // 1. Initialize a fresh directory.
    // 2. Initialize it again and expect SM_ERR_EXISTS.
    tmpdir_t tmp;
    REQUIRE(sm_init(tmp.path()) == 0);
    CHECK(sm_init(tmp.path()) == -1);
    CHECK(sm_errno() == SM_ERR_EXISTS);
}

TEST_CASE("sm_attach requires an initialized home directory")
{
    // Verify attach creates nothing on an uninitialized directory.
    // 1. Attach to an empty directory; expect SM_ERR_NOTFOUND.
    // 2. Confirm no global/ was created as a side effect.
    tmpdir_t tmp;
    CHECK(sm_attach(tmp.path()) == nullptr);
    CHECK(sm_errno() == SM_ERR_NOTFOUND);
    CHECK_FALSE(std::filesystem::exists(std::string(tmp.path()) + "/global"));
}

TEST_CASE("a home directory can be attached again after detach")
{
    // Verify detach releases everything a later attach needs.
    // 1. Initialize and attach.
    // 2. Detach, then attach again successfully.
    // One environment at a time is the caller's rule to keep;
    // the library does not enforce it, so it is not tested here.
    tmpdir_t tmp;
    REQUIRE(sm_init(tmp.path()) == 0);
    sm_env_t* env = sm_attach(tmp.path());
    REQUIRE(env);

    CHECK(sm_detach(env, 0) == 0);
    env = sm_attach(tmp.path());
    CHECK(env);
    CHECK(sm_detach(env, 0) == 0);
}

TEST_CASE("sm_detach of NULL is a no-op")
{
    // Verify sm_detach(NULL) succeeds whatever force says.
    // 1. Detach NULL with force 0 and force 1; both return 0.
    CHECK(sm_detach(nullptr, 0) == 0);
    CHECK(sm_detach(nullptr, 1) == 0);
}

TEST_CASE("sm_attach rejects an unknown format version")
{
    // Verify attach refuses a home written by another format.
    // 1. Initialize a home directory.
    // 2. Overwrite the root db's version key with 99.
    // 3. Attach and expect SM_ERR_VERSION.
    tmpdir_t tmp;
    REQUIRE(sm_init(tmp.path()) == 0);
    {
        rocksdb::DB* root = nullptr;
        std::string rootpath = std::string(tmp.path()) + "/global/root";
        REQUIRE(rocksdb::DB::Open(rocksdb::Options(), rootpath, &root).ok());
        std::string v;
        wisr::put_be64(v, 99);
        REQUIRE(root->Put(rocksdb::WriteOptions(), WISR_ROOT_VERSION, v).ok());
        delete root;
    }
    CHECK(sm_attach(tmp.path()) == nullptr);
    CHECK(sm_errno() == SM_ERR_VERSION);
}

TEST_CASE("sm_attach sweeps database directories no name points at")
{
    // Verify attach destroys unnamed dbid directories and nothing else.
    // 1. Initialize, then plant base/7 with a file, and base/notes.
    // 2. Attach.
    // 3. Confirm base/7 is gone and base/notes is untouched.
    tmpdir_t tmp;
    REQUIRE(sm_init(tmp.path()) == 0);
    std::string base = std::string(tmp.path()) + "/base";
    REQUIRE(mkdir((base + "/7").c_str(), 0700) == 0);
    FILE* f = fopen((base + "/7/junk").c_str(), "w");
    REQUIRE(f);
    fclose(f);
    REQUIRE(mkdir((base + "/notes").c_str(), 0700) == 0);

    sm_env_t* env = sm_attach(tmp.path());
    REQUIRE(env);
    CHECK_FALSE(std::filesystem::exists(base + "/7"));
    CHECK(std::filesystem::exists(base + "/notes"));
    CHECK(sm_detach(env, 0) == 0);
}

TEST_CASE("a successful call leaves the error state untouched")
{
    // Verify success does not clear sm_errno or sm_errmsg.
    // 1. Provoke SM_ERR_INVALID and save the message.
    // 2. Make a successful call.
    // 3. Confirm the code and message are unchanged.
    tmpdir_t tmp;
    CHECK(sm_init(nullptr) == -1);
    std::string msg = sm_errmsg();
    REQUIRE(sm_init(tmp.path()) == 0);
    CHECK(sm_errno() == SM_ERR_INVALID);
    CHECK(msg == sm_errmsg());
    CHECK(sm_retryable() == 0);
}
