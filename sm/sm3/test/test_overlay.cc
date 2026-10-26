/*
 * test_overlay.cc -- a session's overlay (CATALOG_INMEM.md §1): its
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
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
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
  REQUIRE_MESSAGE(sm3::create_table(s, sm3::nsp_public, name, cols, oid) == 0,
                  sm1::errmsg());
  return oid;
}

/* True if s sees a table name in public. */
bool sees(sm3::session_t *s, const char *name) {
  return sm3::cat_get_table(s, sm3::nsp_public, name) != nullptr;
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
  CHECK(sm3::cat_get_table(db.cat, t) == nullptr);
  REQUIRE(sm3::add_column(db.a, t, {"y", sm2::type_text.code, -1, false},
                          std::nullopt) == 0);
  CHECK(sm3::cat_get_table(db.a, t)->columns.size() == 2);
  REQUIRE(sm3::commit(db.a) == 0);
  CHECK(sees(db.b, "t"));
  CHECK(sm3::cat_get_table(db.b, t)->columns.size() == 2);
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "t")->oid == t);
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
  REQUIRE(sm3::drop_table(db.a, t) == 0);
  CHECK_FALSE(sees(db.a, "t"));
  CHECK(sm3::cat_get_table(db.a, t) == nullptr);
  begin(db.b);
  CHECK(sees(db.b, "t"));
  REQUIRE(sm3::commit(db.b) == 0);
  REQUIRE(sm3::abort(db.a) == 0);
  begin(db.a);
  CHECK(sees(db.a, "t"));
  CHECK(sm3::cat_get_table(db.cat, t) != nullptr);
  REQUIRE(sm3::commit(db.a) == 0);
}

TEST_CASE("a table dropped in a transaction cannot be locked in it") {
  // Verify cat_lock_table's lookup after the lock: the transaction's own
  // drop hides the definition the shared catalog still holds, and a
  // table is fresh only if the shared catalog has none.
  // 1. Commit t and u; a drops t and makes v.
  // 2. a's lock of t fails with SM_ERR_NOTFOUND, and so does tbl_open.
  // 3. a's lock of v reports it fresh, of u not.
  // 4. a aborts: t is there to lock again.
  two_t db;
  begin(db.a);
  uint64_t t = create(db.a, "t");
  uint64_t u = create(db.a, "u");
  REQUIRE(sm3::commit(db.a) == 0);
  begin(db.a);
  REQUIRE(sm3::drop_table(db.a, t) == 0);
  uint64_t v = create(db.a, "v");
  std::shared_ptr<const sm3::table_def_t> def;
  CHECK(sm3::cat_lock_table(db.a, t, sm1::SM_LOCKMODE_IS, def) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::table_t *tbl = nullptr;
  CHECK(sm3::tbl_open(db.a, t, sm1::SM_LOCKMODE_IS, tbl) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  bool fresh = false;
  REQUIRE(sm3::cat_lock_table(db.a, v, sm1::SM_LOCKMODE_IS, def, &fresh) == 0);
  CHECK(fresh);
  REQUIRE(sm3::cat_lock_table(db.a, u, sm1::SM_LOCKMODE_IS, def, &fresh) == 0);
  CHECK_FALSE(fresh);
  REQUIRE(sm3::abort(db.a) == 0);
  begin(db.a);
  REQUIRE(sm3::cat_lock_table(db.a, t, sm1::SM_LOCKMODE_IS, def) == 0);
  CHECK(def->oid == t);
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
  uint64_t a = sm3::cat_get_table(db.a, sm3::nsp_public, "a")->oid;
  create(db.a, "b");
  REQUIRE(sm3::rename_table(db.a, a, "a2") == 0);
  CHECK(sees(db.a, "a2"));
  CHECK_FALSE(sees(db.a, "a"));
  REQUIRE(sm3::rollback(db.a, sp) == 0);
  CHECK(sees(db.a, "a"));
  CHECK_FALSE(sees(db.a, "a2"));
  CHECK_FALSE(sees(db.a, "b"));
  CHECK(sm3::cat_get_table(db.a, a)->name == "a");
  REQUIRE(sm3::commit(db.a) == 0);
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "a") != nullptr);
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "b") == nullptr);
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "a2") == nullptr);
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
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "x") != nullptr);
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
    REQUIRE(sm3::create_collation(db.a, sm3::nsp_public, "ci",
                                  "und-u-ks-level2", false, ci) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"s", sm2::type_text.code, -1, false}};
    REQUIRE(sm3::create_table(db.a, sm3::nsp_public, "w", cols, t) == 0);
    std::vector<sm3::index_key_spec_t> keys = {{0, false, false, ci}};
    REQUIRE(sm3::create_index(db.a, t, "w_s", keys, {}, false, ix) == 0);
    auto def = sm3::cat_get_table(db.a, t);
    REQUIRE(def->indexes.size() == 1);
    CHECK(def->indexes[0].key[0].collation != nullptr);
    if (!keep) {
      REQUIRE(sm3::abort(db.a) == 0);
      CHECK(sm3::cat_get_table(db.cat, t) == nullptr);
      continue;
    }
    REQUIRE(sm3::commit(db.a) == 0);
    begin(db.b);
    auto seen = sm3::cat_get_table(db.b, t);
    REQUIRE(seen != nullptr);
    CHECK(seen->indexes[0].key[0].collation != nullptr);
    REQUIRE(sm3::commit(db.b) == 0);
  }
}

TEST_CASE("a transaction that made DDL cannot be prepared") {
  // Verify prepare refuses DDL, whose overlay could not outlive the session.
  // 1. A creates a table, and prepare fails with SM_ERR_INVALID.
  // 2. A aborts; a transaction without DDL prepares, and commits.
  two_t db;
  begin(db.a);
  create(db.a, "x");
  CHECK(sm3::prepare(db.a, "g1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.a) == 0);

  begin(db.a);
  REQUIRE(sm3::prepare(db.a, "g2") == 0);
  REQUIRE(sm1::commit_prepared(sm3::session_sm1(db.b), "g2") == 0);
  CHECK(sm3::cat_get_table(db.cat, sm3::nsp_public, "x") == nullptr);
}

TEST_CASE("an index is found by name as the session sees it") {
  // Verify cat_get_index finds an index through the overlay, then shared.
  // 1. A creates table x and index x_i: A finds it, B does not.
  // 2. A commits: B finds it, with its table.
  // 3. A drops x, uncommitted: A no longer finds x_i; B still does.
  two_t db;
  begin(db.a);
  uint64_t t = create(db.a, "x"), ix = 0, table = 0, index = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.a, t, "x_i", keys, {}, false, ix) == 0);
  CHECK(sm3::cat_get_index(db.a, sm3::nsp_public, "x_i", table, index));
  CHECK(table == t);
  CHECK(index == ix);
  begin(db.b);
  CHECK_FALSE(sm3::cat_get_index(db.b, sm3::nsp_public, "x_i", table, index));
  REQUIRE(sm3::commit(db.b) == 0);
  REQUIRE(sm3::commit(db.a) == 0);

  begin(db.b);
  table = index = 0;
  CHECK(sm3::cat_get_index(db.b, sm3::nsp_public, "x_i", table, index));
  CHECK(table == t);
  CHECK(index == ix);
  REQUIRE(sm3::commit(db.b) == 0);

  begin(db.a);
  REQUIRE(sm3::drop_table(db.a, t) == 0);
  CHECK_FALSE(sm3::cat_get_index(db.a, sm3::nsp_public, "x_i", table, index));
  begin(db.b);
  CHECK(sm3::cat_get_index(db.b, sm3::nsp_public, "x_i", table, index));
  REQUIRE(sm3::commit(db.b) == 0);
  REQUIRE(sm3::abort(db.a) == 0);
}

TEST_CASE("a session lists the tables its transaction sees") {
  // Verify cat_list_tables merges the overlay's names over the shared ones.
  // 1. A creates b and a and commits: both list them, sorted.
  // 2. A creates c, renames a to d and drops b, uncommitted: A lists
  //    c and d; B still lists a and b.
  // 3. A aborts: A lists a and b again.
  two_t db;
  begin(db.a);
  uint64_t tb = create(db.a, "b"), ta = create(db.a, "a");
  REQUIRE(sm3::commit(db.a) == 0);
  using names = std::vector<std::string>;
  CHECK(sm3::cat_list_tables(db.a, sm3::nsp_public) == names{"a", "b"});
  CHECK(sm3::cat_list_tables(db.b, sm3::nsp_public) == names{"a", "b"});

  begin(db.a);
  create(db.a, "c");
  REQUIRE(sm3::rename_table(db.a, ta, "d") == 0);
  REQUIRE(sm3::drop_table(db.a, tb) == 0);
  CHECK(sm3::cat_list_tables(db.a, sm3::nsp_public) == names{"c", "d"});
  CHECK(sm3::cat_list_tables(db.b, sm3::nsp_public) == names{"a", "b"});
  CHECK(sm3::cat_list_tables(db.a, sm3::nsp_pg_catalog).empty());

  REQUIRE(sm3::abort(db.a) == 0);
  CHECK(sm3::cat_list_tables(db.a, sm3::nsp_public) == names{"a", "b"});
}
