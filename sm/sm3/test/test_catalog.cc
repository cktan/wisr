/*
 * test_catalog.cc -- init, load and table creation (../CATALOG.md
 * ../CATALOG.md §6).
 */

#include "catalog.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>
#include <vector>

namespace {

/* decimal's typmod for (p, s), as PostgreSQL encodes numeric's. */
int32_t decimal_typmod(int p, int s) { return ((p << 16) | s) + 4; }

/* The columns of a table with a column of several kinds. */
std::vector<sm3::column_spec_t> order_columns() {
  return {{"id", sm2::type_int8.code, -1, true},
          {"note", sm2::type_text.code, -1, false},
          {"amount", sm2::type_decimal64.code, decimal_typmod(10, 2), false}};
}

/* Load db's catalog; it must load. */
sm3::catalog_t *load(db_t &db) {
  sm3::catalog_t *cat = nullptr;
  REQUIRE_MESSAGE(sm3::cat_open(db.env(), "db", cat) == 0, sm1::errmsg());
  return cat;
}

} // namespace

TEST_CASE("cat_init: makes the sequence trees take their page ids from") {
  // Verify a new database has the sequence, and that it never gives 0.
  // 1. cat_init a fresh database.
  // 2. Two draws give 1 and 2: page 0 is every tree's root.
  db_t db;
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  sm1::session_t *s = db.open();
  uint64_t a = 0, b = 0;
  REQUIRE_MESSAGE(sm1::nextseq(s, sm2::tree_page_seq, a) == 0, sm1::errmsg());
  REQUIRE(sm1::nextseq(s, sm2::tree_page_seq, b) == 0);
  CHECK(1 == a);
  CHECK(2 == b);
}

TEST_CASE("a catalog is made once, and loaded only once made") {
  // Verify cat_init and cat_open on a new database.
  // 1. Before cat_init, cat_open fails with SM_ERR_NOTFOUND. Neither
  //    call is given a session: the test opens none.
  // 2. cat_init makes the catalog; a second fails with SM_ERR_EXISTS.
  // 3. The loaded catalog has no tables.
  db_t db;
  sm3::catalog_t *cat = nullptr;
  CHECK(sm3::cat_open(db.env(), "db", cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE_MESSAGE(sm3::cat_init(db.env(), "db") == 0, sm1::errmsg());

  CHECK(sm3::cat_init(db.env(), "db") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
  cat = load(db);
  CHECK(sm3::cat_get_table(cat, sm3::nsp_public, "anything") == nullptr);
  sm3::cat_close(cat);
}

TEST_CASE("a cat_init that fails partway leaves no catalog behind") {
  // Verify cat_init makes the catalog whole or not at all.
  // 1. The sequence cat_init would make, sm3.oid, is made first, so
  //    cat_init fails on it after it has written the format version.
  // 2. cat_init fails, and the database still has no catalog:
  //    cat_open is SM_ERR_NOTFOUND, not a catalog half made.
  // 3. With the sequence out of the way cat_init succeeds, and the
  //    catalog loads with its two namespaces.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::mkseq(s, "sm3.oid", 1, 1) == 0);
  REQUIRE(sm1::commit(s) == 0);

  CHECK(sm3::cat_init(db.env(), "db") == -1);
  sm3::catalog_t *cat = nullptr;
  CHECK(sm3::cat_open(db.env(), "db", cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::rmseq(s, "sm3.oid") == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE_MESSAGE(sm3::cat_init(db.env(), "db") == 0, sm1::errmsg());
  cat = load(db);
  CHECK(sm3::cat_list_namespaces(cat) ==
        std::vector<std::string>{"pg_catalog", "public"});
  sm3::cat_close(cat);
}

TEST_CASE("a created table loads back, across a restart, as it was made") {
  // Verify create_table and cat_open together.
  // 1. Create a table of int8, text and decimal64(10, 2) in public,
  //    and commit.
  // 2. After a restart, the loaded catalog finds it by name and by
  //    oid, the first at least 16384, with its columns, types, typmods
  //    and not-null flags as declared, and a layout sm2 accepts.
  // 3. Its store exists and holds nothing, and its rowno sequence
  //    hands out 1.
  // 4. A table created and aborted is not there.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  std::vector<sm3::column_spec_t> cols = order_columns();
  uint64_t oid = 0, gone = 0;
  sm3::catalog_t *first = load(db);
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::session_open(first, s, ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::create_table(ss, sm3::nsp_public, "orders", cols, oid) ==
                      0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "gone", cols, gone) == 0);
  REQUIRE(sm3::abort(ss) == 0);
  sm3::session_close(ss);
  sm3::cat_close(first);

  db.restart();
  s = db.open();
  sm3::catalog_t *cat = load(db);
  auto t = sm3::cat_get_table(cat, sm3::nsp_public, "orders");
  REQUIRE(t != nullptr);
  CHECK(sm3::cat_get_table(cat, oid) == t);
  CHECK(t->oid == oid);
  CHECK(oid >= sm3::first_oid);
  CHECK(t->storeid >= 1000);
  CHECK(t->name == "orders");
  CHECK(t->nspoid == sm3::nsp_public);
  REQUIRE(t->columns.size() == 3);
  for (size_t i = 0; i < cols.size(); i++) {
    CAPTURE(i);
    CHECK(t->columns[i].name == cols[i].name);
    CHECK(t->columns[i].type->code == cols[i].type_code);
    CHECK(t->columns[i].typmod == cols[i].typmod);
    CHECK(t->columns[i].not_null == cols[i].not_null);
    CHECK_FALSE(t->columns[i].dropped);
  }
  CHECK(t->desc.bad == SIZE_MAX);
  CHECK(t->desc.schema.size() == 3);
  CHECK(t->indexes.empty());
  CHECK(sm3::cat_get_table(cat, sm3::nsp_public, "gone") == nullptr);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::scan_t *c = nullptr;
  std::string_view k, v;
  REQUIRE(sm1::scan(s, t->storeid, nullptr, 0, 1, nullptr, 0, 0, c) == 0);
  CHECK(sm1::scannxt(c, k, v) == 1);
  sm1::scanend(c);
  uint64_t rowno = 0;
  REQUIRE(sm1::nextseq(s, t->id_seq, rowno) == 0);
  CHECK(rowno == 1);
  REQUIRE(sm1::commit(s) == 0);
  sm3::cat_close(cat);
}

TEST_CASE("cat_get_table_by_store finds a committed table by its store") {
  // Verify the lookup a lock's table is named by (sm1::lock_status).
  // 1. Create orders and commit: its store finds it, and a store no
  //    table has finds none.
  // 2. Truncate it, not yet committed: the new store the session sees
  //    is no committed table's, and the old one still finds it.
  // 3. Commit: the new store finds it, and the old one none.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  sm3::catalog_t *cat = load(db);
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::session_open(cat, s, ss) == 0);
  uint64_t oid = 0;
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "orders", order_columns(),
                            oid) == 0);
  REQUIRE(sm3::commit(ss) == 0);
  uint64_t old_store = sm3::cat_get_table(cat, oid)->storeid;
  CHECK(sm3::cat_get_table_by_store(cat, old_store) ==
        sm3::cat_get_table(cat, oid));
  CHECK(sm3::cat_get_table_by_store(cat, old_store + 999) == nullptr);

  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::truncate_table(ss, oid) == 0);
  uint64_t new_store = sm3::cat_get_table(ss, oid)->storeid;
  REQUIRE(new_store != old_store);
  CHECK(sm3::cat_get_table_by_store(cat, new_store) == nullptr);
  CHECK(sm3::cat_get_table_by_store(cat, old_store) != nullptr);
  REQUIRE(sm3::commit(ss) == 0);
  CHECK(sm3::cat_get_table_by_store(cat, new_store) ==
        sm3::cat_get_table(cat, oid));
  CHECK(sm3::cat_get_table_by_store(cat, old_store) == nullptr);
  sm3::session_close(ss);
  sm3::cat_close(cat);
}

TEST_CASE("create_table refuses what it cannot make") {
  // Verify create_table's errors.
  // 1. A name already taken in the namespace is SM_ERR_DUPKEY; the
  //    same name in another namespace is not.
  // 2. An unknown namespace is SM_ERR_NOTFOUND.
  // 3. No name, two columns of one name, the retired type code 7,
  //    and a decimal64 with no typmod are SM_ERR_INVALID.
  // 4. A catalog of a format this code does not read is
  //    SM_ERR_VERSION to cat_open.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  sm3::catalog_t *loaded = load(db);
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::session_open(loaded, s, ss) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  std::vector<sm3::column_spec_t> cols = order_columns();
  uint64_t oid;
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "t", cols, oid) == 0);
  CHECK(sm3::create_table(ss, sm3::nsp_public, "t", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_table(ss, sm3::nsp_pg_catalog, "t", cols, oid) == 0);
  CHECK(sm3::create_table(ss, 999, "u", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  std::vector<std::vector<sm3::column_spec_t>> bad = {
      {{"a", sm2::type_int4.code, -1, false},
       {"a", sm2::type_int4.code, -1, false}},
      {{"a", 7, -1, false}},
      {{"a", sm2::type_decimal64.code, -1, false}},
  };
  CHECK(sm3::create_table(ss, sm3::nsp_public, "", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  for (const auto &b : bad) {
    CHECK(sm3::create_table(ss, sm3::nsp_public, "v", b, oid) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }

  REQUIRE(sm3::abort(ss) == 0);

  // cat_open reads what is committed: commit the other format.
  sm2::catkv_t *kv = nullptr;
  std::string key, value;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::catkv_open(s, kv) == 0);
  REQUIRE(sm3::meta_key(key) == 0);
  REQUIRE(sm3::encode_meta(sm3::format_version + 1, value) == 0);
  REQUIRE(sm2::catkv_update(kv, key, value) == 0);
  sm2::catkv_close(kv);
  REQUIRE(sm1::commit(s) == 0);
  sm3::catalog_t *cat = nullptr;
  CHECK(sm3::cat_open(db.env(), "db", cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VERSION);
  sm3::session_close(ss);
  sm3::cat_close(loaded);
}

namespace {

/* How many keys of kind the catalog store holds, as s's transaction sees. */
size_t count_keys(sm1::session_t *s, sm3::kind_t kind) {
  sm2::catkv_t *kv = nullptr;
  sm2::catkv_scan_t *c = nullptr;
  std::string prefix;
  REQUIRE(sm2::catkv_open(s, kv) == 0);
  REQUIRE(sm3::kind_prefix(kind, prefix) == 0);
  REQUIRE(sm2::catkv_scan(kv, prefix, c) == 0);
  std::string_view k, v;
  size_t n = 0;
  while (sm2::catkv_scannxt(c, k, v) == 0) {
    n++;
  }
  sm2::catkv_scanend(c);
  sm2::catkv_close(kv);
  return n;
}

} // namespace

TEST_CASE("a table's columns and indexes each have a key of their own") {
  // Verify the layout of a table's record (../CATALOG.md §3.3).
  // 1. A table of three columns is created: the catalog holds one
  //    table key, three column keys and no index key.
  // 2. A column is added, and an index created: one more of each.
  // 3. A column is dropped: it keeps its key, as it keeps its place.
  // 4. The index is dropped: its key goes, the columns' stay.
  // 5. The table is dropped: none of its keys is left.
  db_t db;
  REQUIRE(sm3::cat_init(db.env(), "db") == 0);
  sm3::catalog_t *cat = load(db);
  sm1::session_t *s = db.open();
  sm3::session_t *ss = nullptr;
  REQUIRE(sm3::session_open(cat, s, ss) == 0);
  REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  size_t tables = count_keys(s, sm3::kind_t::table);
  uint64_t t = 0, x = 0;
  REQUIRE(sm3::create_table(ss, sm3::nsp_public, "orders", order_columns(),
                            t) == 0);
  CHECK(count_keys(s, sm3::kind_t::table) == tables + 1);
  CHECK(count_keys(s, sm3::kind_t::column) == 3);
  CHECK(count_keys(s, sm3::kind_t::index) == 0);

  std::vector<sm3::index_key_spec_t> on_id = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::add_column(ss, t, {"extra", sm2::type_int4.code, -1, false},
                          std::nullopt) == 0);
  REQUIRE(sm3::create_index(ss, t, "orders_id", on_id, {}, false, x) == 0);
  CHECK(count_keys(s, sm3::kind_t::column) == 4);
  CHECK(count_keys(s, sm3::kind_t::index) == 1);

  REQUIRE(sm3::drop_column(ss, t, "note") == 0);
  CHECK(count_keys(s, sm3::kind_t::column) == 4);

  REQUIRE(sm3::drop_index(ss, t, x) == 0);
  CHECK(count_keys(s, sm3::kind_t::column) == 4);
  CHECK(count_keys(s, sm3::kind_t::index) == 0);

  REQUIRE(sm3::drop_table(ss, t) == 0);
  CHECK(count_keys(s, sm3::kind_t::table) == tables);
  CHECK(count_keys(s, sm3::kind_t::column) == 0);
  CHECK(count_keys(s, sm3::kind_t::index) == 0);
  REQUIRE(sm3::commit(ss) == 0);
  sm3::session_close(ss);
  sm3::cat_close(cat);
}
