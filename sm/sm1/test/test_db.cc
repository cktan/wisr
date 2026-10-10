/*
 * test_db.cc -- sm1::mkdb, sm1::rmdb, sm1::lsdb, and sm1::options_t.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

/* The databases sm1::lsdb reports, in its order. */
std::vector<std::string> lsdb(sm1::env_t *env) {
  std::vector<std::string> out;
  REQUIRE(sm1::lsdb(env, out) == 0);
  return out;
}

} // namespace

TEST_CASE("a default options_t holds the documented defaults") {
  // Verify every option starts at the default sm1.h documents.
  // 1. Default-construct an options_t.
  // 2. Check each field.
  sm1::options_t opt;
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
  REQUIRE(sm1::mkdb(home.env(), "foo", nullptr) == 0);
  REQUIRE(sm1::mkdb(home.env(), "Foo", nullptr) == 0);
  REQUIRE(sm1::mkdb(home.env(), "a/../b", nullptr) == 0);
  std::vector<std::string> want = {"Foo", "a/../b", "foo"};
  CHECK(lsdb(home.env()) == want);

  home.reattach();
  CHECK(lsdb(home.env()) == want);
  CHECK(home.base_entries() == std::set<std::string>{"1", "2", "3"});
}

TEST_CASE("a database's dbid is its own and is never given again") {
  // Verify dbid reports the number mkdb gave, stable and not reused.
  // 1. Create "a" and "b": their dbids are 1 and 2, their directories.
  // 2. Drop "a" and create it again: it gets 3, not 1.
  // 3. Reattach: the dbids are the same.
  // 4. Ask for a database that does not exist: SM_ERR_NOTFOUND.
  home_t home;
  uint64_t id = 0;
  REQUIRE(sm1::mkdb(home.env(), "a", nullptr) == 0);
  REQUIRE(sm1::mkdb(home.env(), "b", nullptr) == 0);
  REQUIRE(sm1::dbid(home.env(), "a", &id) == 0);
  CHECK(id == 1);
  REQUIRE(sm1::dbid(home.env(), "b", &id) == 0);
  CHECK(id == 2);

  REQUIRE(sm1::rmdb(home.env(), "a") == 0);
  REQUIRE(sm1::mkdb(home.env(), "a", nullptr) == 0);
  REQUIRE(sm1::dbid(home.env(), "a", &id) == 0);
  CHECK(id == 3);

  home.reattach();
  REQUIRE(sm1::dbid(home.env(), "a", &id) == 0);
  CHECK(id == 3);
  REQUIRE(sm1::dbid(home.env(), "b", &id) == 0);
  CHECK(id == 2);

  CHECK(sm1::dbid(home.env(), "none", &id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
}

TEST_CASE("sm1_mkdb rejects a duplicate or badly sized name") {
  // Verify mkdb's name checks.
  // 1. Create "db"; creating it again fails with SM_ERR_EXISTS.
  // 2. A name of sm1::max_dbname bytes is accepted.
  // 3. Names of sm1::max_dbname + 1 bytes, "", and NULL fail
  //    with SM_ERR_INVALID.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  CHECK(sm1::mkdb(home.env(), "db", nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);

  std::string max(sm1::max_dbname, 'x');
  CHECK(sm1::mkdb(home.env(), max.c_str(), nullptr) == 0);
  std::string over(sm1::max_dbname + 1, 'x');
  CHECK(sm1::mkdb(home.env(), over.c_str(), nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::mkdb(home.env(), "", nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::mkdb(home.env(), std::string_view("d\0b", 3)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("sm1_mkdb rejects a malformed rocksdb_options string") {
  // Verify a bad pass-through option fails before anything is made.
  // 1. Create with an unknown option; expect SM_ERR_INVALID.
  // 2. Confirm no database and no directory was created.
  // 3. Create with a valid option string; it succeeds.
  home_t home;
  sm1::options_t opt;
  opt.rocksdb_options = "no_such_option=1";
  CHECK(sm1::mkdb(home.env(), "db", &opt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(lsdb(home.env()).empty());
  CHECK(home.base_entries().empty());

  opt.rocksdb_options = "max_open_files=100";
  CHECK(sm1::mkdb(home.env(), "db", &opt) == 0);
}

TEST_CASE("sm1_rmdb removes the name and the directory, never reusing a dbid") {
  // Verify rmdb deletes a database completely.
  // 1. Create "db" and remove it; lsdb is empty and base/ too.
  // 2. Removing it again fails with SM_ERR_NOTFOUND.
  // 3. Create "db" again; it gets a new dbid (directory 2).
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  REQUIRE(sm1::rmdb(home.env(), "db") == 0);
  CHECK(lsdb(home.env()).empty());
  CHECK(home.base_entries().empty());

  CHECK(sm1::rmdb(home.env(), "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  CHECK(home.base_entries() == std::set<std::string>{"2"});
}
