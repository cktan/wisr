/*
 * test_catalog.cc -- init, load and table creation (CATALOG.md
 * §5).
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

/* Load s's catalog in a transaction of its own; it must load. */
sm3::catalog_t *load(sm1::session_t *s) {
  sm3::catalog_t *cat = nullptr;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(sm3::cat_open(s, cat) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);
  return cat;
}

} // namespace

TEST_CASE("a catalog is made once, and loaded only once made") {
  // Verify cat_init and cat_open on a new database.
  // 1. Before cat_init, cat_open fails with SM_ERR_NOTFOUND.
  // 2. cat_init commits; a second fails with SM_ERR_EXISTS.
  // 3. The loaded catalog has no tables.
  // 4. An aborted cat_init leaves no catalog behind.
  db_t db;
  sm1::session_t *s = db.open();
  sm3::catalog_t *cat = nullptr;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::cat_open(s, cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::cat_init(s) == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::cat_open(s, cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE_MESSAGE(sm3::cat_init(s) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::cat_init(s) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_EXISTS);
  REQUIRE(sm1::commit(s) == 0);
  cat = load(s);
  CHECK(sm3::cat_table(cat, sm3::nsp_public, "anything") == nullptr);
  sm3::cat_close(cat);
}

TEST_CASE("a created table loads back, across a restart, as it was made") {
  // Verify cat_create_table and cat_open together.
  // 1. Create a table of int8, text and decimal64(10, 2) in public,
  //    and commit.
  // 2. After a restart, the loaded catalog finds it by name and by
  //    oid, both at least 1000, with its columns, types, typmods
  //    and not-null flags as declared, and a layout sm2 accepts.
  // 3. Its store exists and holds nothing, and its rowno sequence
  //    hands out 1.
  // 4. A table created and aborted is not there.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_init(s) == 0);
  REQUIRE(sm1::commit(s) == 0);
  std::vector<sm3::column_spec_t> cols = order_columns();
  uint64_t oid = 0, gone = 0;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE_MESSAGE(
      sm3::cat_create_table(s, sm3::nsp_public, "orders", cols, oid) == 0,
      sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_create_table(s, sm3::nsp_public, "gone", cols, gone) == 0);
  REQUIRE(sm1::abort(s) == 0);

  db.restart();
  s = db.open();
  sm3::catalog_t *cat = load(s);
  auto t = sm3::cat_table(cat, sm3::nsp_public, "orders");
  REQUIRE(t != nullptr);
  CHECK(sm3::cat_table(cat, oid) == t);
  CHECK(t->oid == oid);
  CHECK(oid >= 1000);
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
  CHECK(sm3::cat_table(cat, sm3::nsp_public, "gone") == nullptr);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::scan_t *c = nullptr;
  std::string_view k, v;
  REQUIRE(sm1::scan(s, t->storeid, nullptr, 0, 1, nullptr, 0, 0, c) == 0);
  CHECK(sm1::scannxt(c, k, v) == 1);
  sm1::scanend(c);
  uint64_t rowno = 0;
  REQUIRE(sm1::nextseq(s, t->rowno_seq, rowno) == 0);
  CHECK(rowno == 1);
  REQUIRE(sm1::commit(s) == 0);
  sm3::cat_close(cat);
}

TEST_CASE("cat_create_table refuses what it cannot make") {
  // Verify cat_create_table's errors.
  // 1. A name already taken in the namespace is SM_ERR_DUPKEY; the
  //    same name in another namespace is not.
  // 2. An unknown namespace is SM_ERR_NOTFOUND.
  // 3. No name, two columns of one name, the retired type code 7,
  //    and a decimal64 with no typmod are SM_ERR_INVALID.
  // 4. A catalog of a format this code does not read is
  //    SM_ERR_VERSION to cat_open.
  db_t db;
  sm1::session_t *s = db.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm3::cat_init(s) == 0);
  std::vector<sm3::column_spec_t> cols = order_columns();
  uint64_t oid;
  REQUIRE(sm3::cat_create_table(s, sm3::nsp_public, "t", cols, oid) == 0);
  CHECK(sm3::cat_create_table(s, sm3::nsp_public, "t", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::cat_create_table(s, sm3::nsp_pg_catalog, "t", cols, oid) == 0);
  CHECK(sm3::cat_create_table(s, 999, "u", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  std::vector<std::vector<sm3::column_spec_t>> bad = {
      {{"a", sm2::type_int4.code, -1, false},
       {"a", sm2::type_int4.code, -1, false}},
      {{"a", 7, -1, false}},
      {{"a", sm2::type_decimal64.code, -1, false}},
  };
  CHECK(sm3::cat_create_table(s, sm3::nsp_public, "", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  for (const auto &b : bad) {
    CHECK(sm3::cat_create_table(s, sm3::nsp_public, "v", b, oid) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }

  sm2::catkv_t *kv = nullptr;
  std::string key, value;
  REQUIRE(sm2::catkv_open(s, kv) == 0);
  REQUIRE(sm3::meta_key(key) == 0);
  REQUIRE(sm3::encode_meta(sm3::format_version + 1, value) == 0);
  REQUIRE(sm2::catkv_update(kv, key, value) == 0);
  sm2::catkv_close(kv);
  sm3::catalog_t *cat = nullptr;
  CHECK(sm3::cat_open(s, cat) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_VERSION);
  REQUIRE(sm1::abort(s) == 0);
}
