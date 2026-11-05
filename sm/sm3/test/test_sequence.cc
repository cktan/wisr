/*
 * test_sequence.cc -- a Sequence against a snapshot (../SEQUENCE.md): it
 * is not transactional, so what a draw sees is the newest state, not the
 * transaction's snapshot.
 */
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>

namespace {

/* A catalog and sessions a and b on it. */
struct cat_t : db_t {
  cat_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, open(), a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
  }
  ~cat_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }
  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
};

} // namespace

TEST_CASE("a sequence dropped since the snapshot is not drawn") {
  // Verify a transaction whose snapshot predates a drop gets no value:
  // a draw reads the newest state, so it does not restart the sequence.
  // 1. Make a sequence and draw three values.
  // 2. a begins at repeatable read, and sees it.
  // 3. b drops it and commits.
  // 4. a draws: SM_ERR_NOTFOUND.
  cat_t f;
  uint64_t q = 0, v = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_sequence(f.a, sm3::nsp_public, "s", 1, 1, q) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  for (int i = 0; i < 3; i++) {
    REQUIRE(sm3::nextval(f.a, q, v) == 0);
  }
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(3u == v);

  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t seen = 0;
  REQUIRE(sm3::cat_get_sequence(f.a, sm3::nsp_public, "s", seen) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::drop_sequence(f.b, q) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  CHECK(-1 == sm3::nextval(f.a, q, v));
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  sm3::abort(f.a);
}

TEST_CASE("a sequence committed since the snapshot is drawn") {
  // Verify the catalog and a draw agree on a sequence newer than the
  // transaction's snapshot.
  // 1. a begins at repeatable read, and reads something.
  // 2. b makes a sequence and commits.
  // 3. a finds it by name, and draws its first value.
  cat_t f;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<std::string> names;
  REQUIRE(sm3::cat_list_sequences(f.a, sm3::nsp_public, names) == 0);
  uint64_t q = 0;
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_sequence(f.b, sm3::nsp_public, "late", 1, 1, q) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  uint64_t seen = 0, v = 0;
  CHECK(0 == sm3::cat_get_sequence(f.a, sm3::nsp_public, "late", seen));
  REQUIRE_MESSAGE(0 == sm3::nextval(f.a, q, v), sm1::errmsg());
  CHECK(1u == v);
  sm3::abort(f.a);
}

TEST_CASE("drop_table passes over a sequence newer than its snapshot") {
  // Verify a sequence the snapshot cannot see does not fail a drop:
  // drop_owned_sequences lists from the shared catalog.
  // 1. Make a table.
  // 2. a begins at repeatable read, and reads.
  // 3. b makes a sequence in the namespace and commits.
  // 4. a drops the table: it must succeed.
  cat_t f;
  std::vector<sm3::column_spec_t> cols = {
      {"id", sm2::type_int4.code, -1, true}};
  uint64_t t = 0, q = 0;
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "t", cols, t) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::begin(f.a, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<std::string> names;
  REQUIRE(sm3::cat_list_sequences(f.a, sm3::nsp_public, names) == 0);
  REQUIRE(sm3::begin(f.b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_sequence(f.b, sm3::nsp_public, "other", 1, 1, q) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  CHECK_MESSAGE(0 == sm3::drop_table(f.a, t), sm1::errmsg());
  sm3::abort(f.a);
}
