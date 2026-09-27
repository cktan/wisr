/*
 * test_ddl.cc -- the DDL beyond cat_create_table (CATALOG.md §5):
 * each change written, then read back after a restart.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* v's little-endian bytes. */
template <typename T> std::string le(T v) {
  return std::string(reinterpret_cast<const char *>(&v), sizeof(T));
}

/*
 * A catalog with cat_init committed, on a fresh database, loaded,
 * with an sm3 session ss over the sm1 session s.
 */
class cat_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  cat_db_t() {
    s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_init(s) == 0);
    REQUIRE(sm1::commit(s) == 0);
    load();
  }

  /* Close the session and the catalog. */
  ~cat_db_t() { sm3::session_close(ss); }

  /* Restart, then load the catalog and open a session again. */
  sm3::catalog_t *reload() {
    sm3::session_close(ss);
    ss = nullptr;
    cat = nullptr;
    restart();
    s = open();
    load();
    return cat;
  }

  /* Begin a transaction on ss. */
  void begin() {
    REQUIRE(sm3::begin(ss, sm1::xctlevel_t::read_committed) == 0);
  }

  /* Commit ss's transaction. */
  void commit() { REQUIRE_MESSAGE(sm3::commit(ss) == 0, sm1::errmsg()); }

  sm1::session_t *s = nullptr;
  sm3::session_t *ss = nullptr;
  sm3::catalog_t *cat = nullptr;

private:
  /* Load the catalog in a transaction of its own; open ss over s. */
  void load() {
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE_MESSAGE(sm3::cat_open(s, cat) == 0, sm1::errmsg());
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm3::session_open(cat, s, ss) == 0);
  }
};

/* Make the table t (a int4, b text) in public; its oid. */
uint64_t make_t(cat_db_t &db) {
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"b", sm2::type_text.code, -1, false}};
  uint64_t oid = 0;
  REQUIRE(sm3::cat_create_table(db.ss, sm3::nsp_public, "t", cols, oid) == 0);
  return oid;
}

} // namespace

TEST_CASE("columns, indexes and collations are added and load back") {
  // Verify cat_add_column, cat_create_collation, cat_create_index.
  // 1. Add c int8 NOT NULL with missing 7, and d text with none.
  // 2. Create a non-deterministic collation, and a unique index on
  //    (b, under it, DESC NULLS FIRST) INCLUDE (a).
  // 3. After a restart, the columns hold their types and missing
  //    values; the index its oid, name, first prefix, key collation,
  //    direction, null order and INCLUDE column, built for sm2.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ci = 0, ix = 0;
  REQUIRE(sm3::cat_add_column(db.ss, t, {"c", sm2::type_int8.code, -1, true},
                              le(int64_t{7})) == 0);
  REQUIRE(sm3::cat_add_column(db.ss, t, {"d", sm2::type_text.code, -1, false},
                              std::nullopt) == 0);
  REQUIRE_MESSAGE(sm3::cat_create_collation(db.ss, sm3::nsp_public, "ci",
                                            "und-u-ks-level2", false, ci) == 0,
                  sm1::errmsg());
  std::vector<sm3::index_key_spec_t> keys = {{1, true, true, ci}};
  std::vector<int32_t> inc = {0};
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_b", keys, inc, true, ix) == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->columns.size() == 4);
  CHECK(d->columns[2].name == "c");
  CHECK(d->columns[2].not_null);
  CHECK(d->columns[2].missing == le(int64_t{7}));
  CHECK_FALSE(d->columns[3].missing);
  CHECK(d->desc.bad == SIZE_MAX);
  CHECK(d->next_prefix == 2);
  REQUIRE(d->indexes.size() == 1);
  const sm3::index_def_t &x = d->indexes[0];
  CHECK(x.oid == ix);
  CHECK(x.name == "t_b");
  CHECK(x.prefix == 1);
  CHECK(x.unique);
  CHECK(x.key_attnums == std::vector<int32_t>{1});
  CHECK(x.include_attnums == std::vector<int32_t>{0});
  REQUIRE(x.key.size() == 1);
  CHECK(x.key[0].type == &sm2::type_text);
  CHECK(x.key[0].collation != nullptr);
  CHECK(x.key[0].desc);
  CHECK(x.key[0].nulls_first);
  REQUIRE(x.include.size() == 1);
  CHECK(x.include[0].type == &sm2::type_int4);
}

TEST_CASE("columns, indexes and tables are dropped and renamed") {
  // Verify cat_drop_column, cat_drop_index, cat_rename_table and
  // cat_drop_table.
  // 1. A dropped column keeps its place, marked dropped; its name
  //    can be added again as a new column.
  // 2. A dropped index frees its name, and one made after it takes
  //    a new prefix, never the old one.
  // 3. A renamed table is found by its new name only.
  // 4. A dropped table is gone, with its store and its rowno
  //    sequence, and its name can be taken again.
  // 5. A drop that aborts leaves the table as it was.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ix = 0, ix2 = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_a", keys, {}, false, ix) == 0);
  REQUIRE(sm3::cat_drop_column(db.ss, t, "b") == 0);
  REQUIRE(sm3::cat_add_column(db.ss, t, {"b", sm2::type_int8.code, -1, false},
                              std::nullopt) == 0);
  REQUIRE(sm3::cat_drop_index(db.ss, t, ix) == 0);
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_a", keys, {}, false, ix2) == 0);
  REQUIRE(sm3::cat_rename_table(db.ss, t, "t2") == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_table(cat, sm3::nsp_public, "t2");
  REQUIRE(d != nullptr);
  CHECK(sm3::cat_table(cat, sm3::nsp_public, "t") == nullptr);
  REQUIRE(d->columns.size() == 3);
  CHECK(d->columns[1].dropped);
  CHECK(d->columns[2].name == "b");
  CHECK(d->columns[2].type == &sm2::type_int8);
  REQUIRE(d->indexes.size() == 1);
  CHECK(d->indexes[0].oid == ix2);
  CHECK(d->indexes[0].prefix == 2);
  uint64_t storeid = d->storeid;
  std::string seq = d->rowno_seq;

  db.begin();
  REQUIRE(sm3::cat_drop_table(db.ss, t) == 0);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  REQUIRE(sm3::cat_drop_table(db.ss, t) == 0);
  db.commit();
  cat = db.reload();
  CHECK(sm3::cat_table(cat, t) == nullptr);
  db.begin();
  std::vector<uint64_t> stores;
  REQUIRE(sm1::lsstore(db.s, stores) == 0);
  CHECK(std::find(stores.begin(), stores.end(), storeid) == stores.end());
  uint64_t v;
  CHECK(sm1::nextseq(db.s, seq, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  // The name is free again: make_t requires its create to succeed.
  make_t(db);
  db.commit();
}

TEST_CASE("dropping an index erases its entries and nothing else") {
  // Verify cat_drop_index clears the index's prefix.
  // 1. Put a row under prefix 0 and entries under the prefixes of
  //    two indexes in the table's store.
  // 2. Drop the first index: its entries are gone; the row and the
  //    other index's entry remain.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), i1 = 0, i2 = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::cat_create_index(db.ss, t, "i1", keys, {}, false, i1) == 0);
  REQUIRE(sm3::cat_create_index(db.ss, t, "i2", keys, {}, false, i2) == 0);
  db.commit();
  sm3::catalog_t *cat = db.reload();
  uint64_t storeid = sm3::cat_table(cat, t)->storeid;

  db.begin();
  auto key = [](uint16_t prefix, char tail) {
    return std::string{static_cast<char>(prefix >> 8),
                       static_cast<char>(prefix & 0xFF), tail};
  };
  REQUIRE(sm1::insert(db.s, storeid, key(0, 'r'), "row") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(1, 'a'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(1, 'b'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(2, 'a'), "e") == 0);
  REQUIRE(sm3::cat_drop_index(db.ss, t, i1) == 0);
  std::string_view v;
  CHECK(sm1::read(db.s, storeid, key(0, 'r'), v) == 0);
  CHECK(sm1::read(db.s, storeid, key(1, 'a'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(1, 'b'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(2, 'a'), v) == 0);
  db.commit();
}

TEST_CASE("the DDL refuses what the catalog cannot hold") {
  // Verify each call's errors.
  // 1. add column: a live name again, NOT NULL with no missing
  //    value, a missing value of the wrong width or not a varlena
  //    are SM_ERR_INVALID; an unknown table SM_ERR_NOTFOUND.
  // 2. drop column: an unknown one is SM_ERR_NOTFOUND; one an index
  //    uses is SM_ERR_INVALID.
  // 3. create index: a dropped or unknown column, and a collation
  //    on an int4 column, are SM_ERR_INVALID; an unknown collation
  //    is SM_ERR_NOTFOUND; a taken name is SM_ERR_DUPKEY.
  // 4. drop index and drop table of an unknown oid, and a rename to
  //    a taken name, fail as SM_ERR_NOTFOUND and SM_ERR_DUPKEY.
  // 5. create collation: a locale ICU lacks is refused.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ix = 0, other = 0;
  using col = sm3::column_spec_t;
  CHECK(sm3::cat_add_column(db.ss, t, col{"a", sm2::type_int8.code, -1, false},
                            std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_add_column(db.ss, t, col{"x", sm2::type_int8.code, -1, true},
                            std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_add_column(db.ss, t, col{"x", sm2::type_int8.code, -1, false},
                            le(int32_t{1})) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_add_column(db.ss, t, col{"x", sm2::type_text.code, -1, false},
                            std::string_view("\x09"
                                             "ab")) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_add_column(db.ss, 999,
                            col{"x", sm2::type_int8.code, -1, false},
                            std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_a", on_a, {}, false, ix) == 0);
  CHECK(sm3::cat_drop_column(db.ss, t, "zz") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::cat_drop_column(db.ss, t, "a") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::cat_drop_column(db.ss, t, "b") == 0);

  std::vector<sm3::index_key_spec_t> on_b = {{1, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_7 = {{7, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> a_coll = {{0, false, false, 1000}};
  CHECK(sm3::cat_create_index(db.ss, t, "x", on_b, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_create_index(db.ss, t, "x", on_7, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_create_index(db.ss, t, "x", a_coll, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_create_index(db.ss, t, "t_a", on_a, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::cat_add_column(db.ss, t,
                              col{"s", sm2::type_text.code, -1, false},
                              std::nullopt) == 0);
  std::vector<sm3::index_key_spec_t> s_unknown = {{2, false, false, 999999}};
  CHECK(sm3::cat_create_index(db.ss, t, "x", s_unknown, {}, false, other) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  CHECK(sm3::cat_drop_index(db.ss, t, 999999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::cat_drop_table(db.ss, 999999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  std::vector<sm3::column_spec_t> one = {{"a", sm2::type_int4.code, -1, false}};
  uint64_t u = 0;
  REQUIRE(sm3::cat_create_table(db.ss, sm3::nsp_public, "u", one, u) == 0);
  CHECK(sm3::cat_rename_table(db.ss, t, "u") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  uint64_t co = 0;
  CHECK(sm3::cat_create_collation(db.ss, sm3::nsp_public, "bad", "zz-QQ-bogus!",
                                  true, co) == -1);
  REQUIRE(sm3::abort(db.ss) == 0);
}
