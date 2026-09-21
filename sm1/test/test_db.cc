/*
 * test_db.cc -- sm_mkdb, sm_rmdb, sm_lsdb, and sm_options_init.
 */

#include "doctest.h"
#include "util.h"

#include "sm.h"

#include <string.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

/* The databases sm_lsdb reports, in its order. */
std::vector<std::string> lsdb(sm_env_t *env) {
  sm_strlist_t *sl = nullptr;
  REQUIRE(sm_lsdb(env, &sl) == 0);
  std::vector<std::string> out;
  for (size_t i = 0; i < sm_strlist_count(sl); i++) {
    out.push_back(sm_strlist_at(sl, i));
  }
  CHECK(sm_strlist_at(sl, sm_strlist_count(sl)) == nullptr);
  sm_strlist_free(sl);
  return out;
}

} // namespace

TEST_CASE("sm_options_init fills the documented defaults") {
  // Verify every option starts at the default sm.h documents.
  // 1. Initialize an options struct over garbage.
  // 2. Check each field.
  sm_options_t opt;
  memset(&opt, 0xab, sizeof(opt));
  sm_options_init(&opt);
  CHECK(opt.durability_sync == 1);
  CHECK(opt.group_commit_us == 200);
  CHECK(opt.deadlock_ms == 50);
  CHECK(opt.ssi_max_entries == (1 << 20));
  CHECK(opt.ts_lease == 1000000);
  CHECK(opt.rocksdb_options == nullptr);
  CHECK(opt.commit_cache_entries == 65536);
}

TEST_CASE("databases are listed in bytewise order and survive reattach") {
  // Verify mkdb names persist and lsdb orders them bytewise.
  // 1. Create "foo", "Foo", and "a/../b" (a name, not a path).
  // 2. List them: bytewise order, case not folded.
  // 3. Reattach and list again; nothing was swept.
  // 4. Confirm base/ holds only numbered directories.
  home_t home;
  CHECK(lsdb(home.env()).empty());
  REQUIRE(sm_mkdb(home.env(), "foo", nullptr) == 0);
  REQUIRE(sm_mkdb(home.env(), "Foo", nullptr) == 0);
  REQUIRE(sm_mkdb(home.env(), "a/../b", nullptr) == 0);
  std::vector<std::string> want = {"Foo", "a/../b", "foo"};
  CHECK(lsdb(home.env()) == want);

  home.reattach();
  CHECK(lsdb(home.env()) == want);
  CHECK(home.base_entries() == std::set<std::string>{"1", "2", "3"});
}

TEST_CASE("sm_mkdb rejects a duplicate or badly sized name") {
  // Verify mkdb's name checks.
  // 1. Create "db"; creating it again fails with SM_ERR_EXISTS.
  // 2. A name of SM_MAX_DBNAME bytes is accepted.
  // 3. Names of SM_MAX_DBNAME + 1 bytes, "", and NULL fail
  //    with SM_ERR_INVALID.
  home_t home;
  REQUIRE(sm_mkdb(home.env(), "db", nullptr) == 0);
  CHECK(sm_mkdb(home.env(), "db", nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_EXISTS);

  std::string max(SM_MAX_DBNAME, 'x');
  CHECK(sm_mkdb(home.env(), max.c_str(), nullptr) == 0);
  std::string over(SM_MAX_DBNAME + 1, 'x');
  CHECK(sm_mkdb(home.env(), over.c_str(), nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_mkdb(home.env(), "", nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_mkdb(home.env(), nullptr, nullptr) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
}

TEST_CASE("sm_mkdb rejects a malformed rocksdb_options string") {
  // Verify a bad pass-through option fails before anything is made.
  // 1. Create with an unknown option; expect SM_ERR_INVALID.
  // 2. Confirm no database and no directory was created.
  // 3. Create with a valid option string; it succeeds.
  home_t home;
  sm_options_t opt;
  sm_options_init(&opt);
  opt.rocksdb_options = "no_such_option=1";
  CHECK(sm_mkdb(home.env(), "db", &opt) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(lsdb(home.env()).empty());
  CHECK(home.base_entries().empty());

  opt.rocksdb_options = "max_open_files=100";
  CHECK(sm_mkdb(home.env(), "db", &opt) == 0);
}

TEST_CASE("sm_rmdb removes the name and the directory, never reusing a dbid") {
  // Verify rmdb deletes a database completely.
  // 1. Create "db" and remove it; lsdb is empty and base/ too.
  // 2. Removing it again fails with SM_ERR_NOTFOUND.
  // 3. Create "db" again; it gets a new dbid (directory 2).
  home_t home;
  REQUIRE(sm_mkdb(home.env(), "db", nullptr) == 0);
  REQUIRE(sm_rmdb(home.env(), "db") == 0);
  CHECK(lsdb(home.env()).empty());
  CHECK(home.base_entries().empty());

  CHECK(sm_rmdb(home.env(), "db") == -1);
  CHECK(sm_errno() == SM_ERR_NOTFOUND);

  REQUIRE(sm_mkdb(home.env(), "db", nullptr) == 0);
  CHECK(home.base_entries() == std::set<std::string>{"2"});
}
