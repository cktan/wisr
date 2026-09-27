/*
 * test_overlay.cc -- a session's overlay (CATALOG.md §7): its
 * transaction's DDL, seen by it alone until commit, undone by
 * rollback and abort.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>
#include <vector>

namespace {

/* A loaded catalog with two sessions, a and b, on their own sm1 sessions. */
class two_t : public db_t {
public:
  /* Initialize and load the catalog; open a and b over it. */
  two_t() {
    sm1::session_t *s = open(), *t = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_init(s) == 0);
    REQUIRE(sm3::cat_open(s, cat) == 0);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, t, b) == 0);
  }

  /* Close both sessions and the catalog. */
  ~two_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cat_close(cat);
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
};

/* Begin a READ COMMITTED transaction on s. */
void begin(sm3::session_t *s) {
  REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
}

/* Create the table name (x int4) in public through s; its oid. */
uint64_t create(sm3::session_t *s, const char *name) {
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t oid = 0;
  REQUIRE_MESSAGE(sm3::cat_create_table(s, sm3::nsp_public, name, cols, oid) ==
                      0,
                  sm1::errmsg());
  return oid;
}

/* True if s sees a table name in public. */
bool sees(sm3::session_t *s, const char *name) {
  return sm3::cat_table(s, sm3::nsp_public, name) != nullptr;
}

} // namespace

TEST_CASE("a transaction sees its own DDL, others only once it commits") {
  // Verify the overlay's visibility and publication.
  // 1. a creates t: a sees it; b and the shared catalog do not.
  // 2. a adds a column: a sees two columns; after commit, b and the
  //    shared catalog see the table with both.
  two_t db;
  begin(db.a);
  uint64_t t = create(db.a, "t");
  CHECK(sees(db.a, "t"));
  begin(db.b);
  CHECK_FALSE(sees(db.b, "t"));
  CHECK(sm3::cat_table(db.cat, t) == nullptr);
  REQUIRE(sm3::cat_add_column(db.a, t, {"y", sm2::type_text.code, -1, false},
                              std::nullopt) == 0);
  CHECK(sm3::cat_table(db.a, t)->columns.size() == 2);
  REQUIRE(sm3::commit(db.a) == 0);
  CHECK(sees(db.b, "t"));
  CHECK(sm3::cat_table(db.b, t)->columns.size() == 2);
  CHECK(sm3::cat_table(db.cat, sm3::nsp_public, "t")->oid == t);
  REQUIRE(sm3::commit(db.b) == 0);
}

TEST_CASE("an abort drops the overlay, leaving the catalog as it was") {
  // Verify abort.
  // 1. Commit t; then a drops it: a no longer sees t, b still
  //    does.
  // 2. a aborts: a sees t again, and the shared catalog never lost
  //    it.
  two_t db;
  begin(db.a);
  uint64_t t = create(db.a, "t");
  REQUIRE(sm3::commit(db.a) == 0);
  begin(db.a);
  REQUIRE(sm3::cat_drop_table(db.a, t) == 0);
  CHECK_FALSE(sees(db.a, "t"));
  CHECK(sm3::cat_table(db.a, t) == nullptr);
  begin(db.b);
  CHECK(sees(db.b, "t"));
  REQUIRE(sm3::commit(db.b) == 0);
  REQUIRE(sm3::abort(db.a) == 0);
  begin(db.a);
  CHECK(sees(db.a, "t"));
  CHECK(sm3::cat_table(db.cat, t) != nullptr);
  REQUIRE(sm3::commit(db.a) == 0);
}

TEST_CASE("a rollback to a savepoint undoes only the DDL made under it") {
  // Verify savepoints.
  // 1. Create a; set a savepoint; create b and rename a to a2.
  // 2. Roll back to the savepoint: b is gone, a is back under its
  //    old name, and a2 names nothing.
  // 3. Commit: the shared catalog holds a alone.
  two_t db;
  begin(db.a);
  create(db.a, "a");
  int sp = sm3::savepoint(db.a);
  REQUIRE(sp > 0);
  uint64_t a = sm3::cat_table(db.a, sm3::nsp_public, "a")->oid;
  create(db.a, "b");
  REQUIRE(sm3::cat_rename_table(db.a, a, "a2") == 0);
  CHECK(sees(db.a, "a2"));
  CHECK_FALSE(sees(db.a, "a"));
  REQUIRE(sm3::rollback(db.a, sp) == 0);
  CHECK(sees(db.a, "a"));
  CHECK_FALSE(sees(db.a, "a2"));
  CHECK_FALSE(sees(db.a, "b"));
  CHECK(sm3::cat_table(db.a, a)->name == "a");
  REQUIRE(sm3::commit(db.a) == 0);
  CHECK(sm3::cat_table(db.cat, sm3::nsp_public, "a") != nullptr);
  CHECK(sm3::cat_table(db.cat, sm3::nsp_public, "b") == nullptr);
  CHECK(sm3::cat_table(db.cat, sm3::nsp_public, "a2") == nullptr);
}

TEST_CASE("DDL kept by forget survives a later mark at the same depth") {
  // Verify forget.
  // 1. Set a savepoint, create x, and forget the savepoint.
  // 2. Set another, which reuses its depth; create y; roll back to
  //    it: y is gone, x stays.
  two_t db;
  begin(db.a);
  int d1 = sm3::savepoint(db.a);
  REQUIRE(d1 > 0);
  create(db.a, "x");
  REQUIRE(sm3::forget(db.a, d1) == 0);
  int d2 = sm3::savepoint(db.a);
  CHECK(d2 == d1);
  create(db.a, "y");
  REQUIRE(sm3::rollback(db.a, d2) == 0);
  CHECK(sees(db.a, "x"));
  CHECK_FALSE(sees(db.a, "y"));
  REQUIRE(sm3::commit(db.a) == 0);
  CHECK(sm3::cat_table(db.cat, sm3::nsp_public, "x") != nullptr);
}

TEST_CASE("a collation made in a transaction serves its DDL at once") {
  // Verify a collation in the overlay.
  // 1. Create a collation and a text table, and an index keyed by
  //    the column under it, all in one transaction: the index's key
  //    carries the collation.
  // 2. Abort, and the index and collation are gone with it; commit
  //    them again, and b sees the index's collation.
  two_t db;
  for (bool keep : {false, true}) {
    CAPTURE(keep);
    begin(db.a);
    uint64_t ci = 0, ix = 0, t = 0;
    REQUIRE(sm3::cat_create_collation(db.a, sm3::nsp_public, "ci",
                                      "und-u-ks-level2", false, ci) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"s", sm2::type_text.code, -1, false}};
    REQUIRE(sm3::cat_create_table(db.a, sm3::nsp_public, "w", cols, t) == 0);
    std::vector<sm3::index_key_spec_t> keys = {{0, false, false, ci}};
    REQUIRE(sm3::cat_create_index(db.a, t, "w_s", keys, {}, false, ix) == 0);
    auto def = sm3::cat_table(db.a, t);
    REQUIRE(def->indexes.size() == 1);
    CHECK(def->indexes[0].key[0].collation != nullptr);
    if (!keep) {
      REQUIRE(sm3::abort(db.a) == 0);
      CHECK(sm3::cat_table(db.cat, t) == nullptr);
      continue;
    }
    REQUIRE(sm3::commit(db.a) == 0);
    begin(db.b);
    auto seen = sm3::cat_table(db.b, t);
    REQUIRE(seen != nullptr);
    CHECK(seen->indexes[0].key[0].collation != nullptr);
    REQUIRE(sm3::commit(db.b) == 0);
  }
}
