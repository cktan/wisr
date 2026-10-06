/*
 * test_ssi.cc -- SSI's abort rule with the refinements of Ports
 * and Grittner (SM1_DESIGN.md §7.3): a dangerous structure
 * T1 -> T2 -> T3 fails a transaction only if T3 committed first,
 * and, where T1 committed read-only, only if T3 committed before
 * T1's snapshot.
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <string>

namespace {

/* The isolation level every transaction here runs at. */
constexpr sm1::xctlevel_t SER = sm1::xctlevel_t::serializable;

/* A database with one store holding x = 0 and y = 0, and three sessions. */
class ssi_fixture_t {
public:
  /* Create "db", the store and its rows; open a, b and c. */
  ssi_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    a = opendb(m_home.env(), "db", nullptr);
    b = opendb(m_home.env(), "db", nullptr);
    c = opendb(m_home.env(), "db", nullptr);
    REQUIRE((a && b && c));
    REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
    store = mkstore(a);
    put(a, store, "x", "0");
    put(a, store, "y", "0");
    REQUIRE(sm1::commit(a) == 0);
  }

  /* Close the sessions. */
  ~ssi_fixture_t() {
    sm1::closedb(a);
    sm1::closedb(b);
    sm1::closedb(c);
  }

  /* The row's value as s reads it now. */
  std::string read(sm1::session_t *s, const std::string &k) {
    return get(s, store, k);
  }

  /* s writes v into row k, which must succeed. */
  void write(sm1::session_t *s, const std::string &k, const std::string &v) {
    REQUIRE_MESSAGE(sm1::update(s, store, k, v) == 0, sm1::errmsg());
  }

  sm1::session_t *a = nullptr, *b = nullptr, *c = nullptr;
  uint64_t store = 0;

private:
  home_t m_home;
};

/* Commit s, returning 0 or the error number. */
int finish(sm1::session_t *s) {
  return sm1::commit(s) == 0 ? 0 : sm1::errnum();
}

} // namespace

TEST_CASE("a write skew fails only the second to commit") {
  // Verify the commit-order rule on the shortest cycle, T1 -> T2 -> T1.
  // 1. A and B each read x and y; A writes x, B writes y.
  // 2. A commits: the cycle is not yet closed by a commit, so A
  //    succeeds, where the two-flag rule failed it.
  // 3. B commits: A, the other end, committed first, so B fails.
  ssi_fixture_t f;
  REQUIRE(sm1::begin(f.a, SER) == 0);
  REQUIRE(sm1::begin(f.b, SER) == 0);
  f.read(f.a, "x");
  f.read(f.a, "y");
  f.read(f.b, "x");
  f.read(f.b, "y");
  f.write(f.a, "x", "a");
  f.write(f.b, "y", "b");
  CHECK(finish(f.a) == 0);
  CHECK(finish(f.b) == sm1::SM_ERR_SERIALIZATION);
}

TEST_CASE("a chain whose last link commits last fails no one") {
  // Verify a structure T1 -> T2 -> T3 is safe unless T3 commits first.
  // 1. A reads x and B writes it: A -> B. B reads y and C writes
  //    it: B -> C. B is a pivot.
  // 2. A, then B, then C commit: T3, C, commits last, so all
  //    three succeed, where the two-flag rule failed B. A, B, C is
  //    a serial order that explains what each read.
  ssi_fixture_t f;
  REQUIRE(sm1::begin(f.a, SER) == 0);
  REQUIRE(sm1::begin(f.b, SER) == 0);
  REQUIRE(sm1::begin(f.c, SER) == 0);
  f.read(f.a, "x");
  f.read(f.b, "y");
  f.write(f.b, "x", "b");
  f.write(f.c, "y", "c");
  CHECK(finish(f.a) == 0);
  CHECK(finish(f.b) == 0);
  CHECK(finish(f.c) == 0);
}

TEST_CASE("a chain whose last link commits first fails the pivot") {
  // Verify the rule still fails the pivot when T3 commits first.
  // 1. A -> B -> C as above.
  // 2. C commits first, then B: T3 committed before T2 and T1 is
  //    still running, so B fails.
  // 3. A commits.
  ssi_fixture_t f;
  REQUIRE(sm1::begin(f.a, SER) == 0);
  REQUIRE(sm1::begin(f.b, SER) == 0);
  REQUIRE(sm1::begin(f.c, SER) == 0);
  f.read(f.a, "x");
  f.read(f.b, "y");
  f.write(f.b, "x", "b");
  f.write(f.c, "y", "c");
  CHECK(finish(f.c) == 0);
  CHECK(finish(f.b) == sm1::SM_ERR_SERIALIZATION);
  CHECK(finish(f.a) == 0);
}

TEST_CASE("a read-only T1 whose snapshot precedes T3's commit makes no "
          "anomaly") {
  // Verify the read-only rule (Ports and Grittner §4).
  // 1. A reads x, and B writes it: A -> B. B reads y, and C writes
  //    it: B -> C.
  // 2. C commits, then A commits having written nothing.
  // 3. B commits: T3 committed first, but after A's snapshot, and
  //    A is read-only, so A, B, C is still a serial order and B
  //    succeeds.
  ssi_fixture_t f;
  REQUIRE(sm1::begin(f.a, SER) == 0);
  REQUIRE(sm1::begin(f.b, SER) == 0);
  REQUIRE(sm1::begin(f.c, SER) == 0);
  f.read(f.a, "x");
  f.read(f.b, "y");
  f.write(f.b, "x", "b");
  f.write(f.c, "y", "c");
  CHECK(finish(f.c) == 0);
  CHECK(finish(f.a) == 0);
  CHECK(finish(f.b) == 0);
}

TEST_CASE("a read-only T1 that saw T3's commit is still an anomaly") {
  // Verify the read-only rule does not excuse Fekete's anomaly.
  // 1. B reads y; C writes y and commits: B -> C, C committed.
  // 2. A begins after C's commit, reads y (C's value) and x (not
  //    B's write): A -> B.
  // 3. A commits having written nothing.
  // 4. B writes x and commits: T3 committed before A's snapshot,
  //    so A saw C but not B, while B must precede C: B fails.
  ssi_fixture_t f;
  REQUIRE(sm1::begin(f.b, SER) == 0);
  f.read(f.b, "y");
  REQUIRE(sm1::begin(f.c, SER) == 0);
  f.write(f.c, "y", "c");
  CHECK(finish(f.c) == 0);
  REQUIRE(sm1::begin(f.a, SER) == 0);
  CHECK(f.read(f.a, "y") == "c");
  CHECK(f.read(f.a, "x") == "0");
  f.write(f.b, "x", "b");
  CHECK(finish(f.a) == 0);
  CHECK(finish(f.b) == sm1::SM_ERR_SERIALIZATION);
}

TEST_CASE("an edge onto a committed pivot dooms the reader only if T3 "
          "committed first") {
  // Verify the rule where the pivot has already committed.
  // 1. A begins. B reads y and writes x, and commits; C then
  //    writes y and commits: B -> C, with C committing after B.
  // 2. A reads x, stepping past B's version: A -> B. B, the
  //    pivot, committed before C, so A is not doomed, where the
  //    two-flag rule doomed it.
  // 3. The same with C committing before B: A is doomed and fails
  //    at its commit.
  for (bool c_first : {false, true}) {
    CAPTURE(c_first);
    ssi_fixture_t f;
    REQUIRE(sm1::begin(f.a, SER) == 0);
    f.read(f.a, "z");
    REQUIRE(sm1::begin(f.b, SER) == 0);
    REQUIRE(sm1::begin(f.c, SER) == 0);
    f.read(f.b, "y");
    f.write(f.b, "x", "b");
    if (c_first) {
      f.write(f.c, "y", "c");
      CHECK(finish(f.c) == 0);
      CHECK(finish(f.b) == 0);
    } else {
      CHECK(finish(f.b) == 0);
      f.write(f.c, "y", "c");
      CHECK(finish(f.c) == 0);
    }
    CHECK(f.read(f.a, "x") == "0");
    CHECK(finish(f.a) == (c_first ? sm1::SM_ERR_SERIALIZATION : 0));
  }
}
