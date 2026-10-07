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
  //    values; the index its oid, name, first relid, key collation,
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
  CHECK(d->next_relid == 3);
  REQUIRE(d->indexes.size() == 1);
  const sm3::index_def_t &x = d->indexes[0];
  CHECK(x.oid == ix);
  CHECK(x.name == "t_b");
  CHECK(x.relid == 2);
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
  //    a new relid, never the old one.
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
  CHECK(d->indexes[0].relid == 3);
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
  // Verify cat_drop_index clears the index's relid.
  // 1. Put a row under the heap's relid and entries under the
  //    relids of two indexes in the table's store.
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
  auto key = [](uint16_t relid, char tail) {
    return std::string{static_cast<char>(relid >> 8),
                       static_cast<char>(relid & 0xFF), tail};
  };
  REQUIRE(sm1::insert(db.s, storeid, key(sm3::heap_relid, 'r'), "row") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(2, 'a'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(2, 'b'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(3, 'a'), "e") == 0);
  REQUIRE(sm3::cat_drop_index(db.ss, t, i1) == 0);
  std::string_view v;
  CHECK(sm1::read(db.s, storeid, key(sm3::heap_relid, 'r'), v) == 0);
  CHECK(sm1::read(db.s, storeid, key(2, 'a'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(2, 'b'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(3, 'a'), v) == 0);
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

TEST_CASE("an index's state is recorded, and survives a restart") {
  // Verify the state in an index's record (SM3_DESIGN.md §3).
  // 1. An index made in the transaction that made its table is valid.
  // 2. One made on a table committed earlier is building.
  // 3. Both states load back after a restart.
  // 4. cat_set_index_state changes one, seen at once by its own
  //    transaction, and that loads back too.
  // 5. An index the table does not have is SM_ERR_NOTFOUND.
  cat_db_t db;
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  uint64_t fresh = 0, late = 0;
  db.begin();
  uint64_t t = make_t(db);
  REQUIRE(sm3::cat_create_index(db.ss, t, "fresh", on_a, {}, false, fresh) ==
          0);
  db.commit();
  db.begin();
  REQUIRE(sm3::cat_create_index(db.ss, t, "late", on_a, {}, false, late) == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->indexes.size() == 2);
  CHECK(d->indexes[0].state == sm3::index_state_t::valid);
  CHECK(d->indexes[1].state == sm3::index_state_t::building);

  db.begin();
  REQUIRE(sm3::cat_set_index_state(db.ss, t, late, sm3::index_state_t::valid) ==
          0);
  REQUIRE(sm3::cat_set_index_state(db.ss, t, fresh, sm3::index_state_t::failed,
                                   sm1::SM_ERR_DUPKEY, "two rows") == 0);
  d = sm3::cat_table(db.ss, t);
  CHECK(d->indexes[0].state == sm3::index_state_t::failed);
  CHECK(d->indexes[0].error == sm1::SM_ERR_DUPKEY);
  CHECK(d->indexes[0].reason == "two rows");
  CHECK(d->indexes[1].state == sm3::index_state_t::valid);
  CHECK(d->indexes[1].error == sm1::SM_OK);
  CHECK(d->indexes[1].reason.empty());
  CHECK(sm3::cat_set_index_state(db.ss, t, late + 999,
                                 sm3::index_state_t::valid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();

  cat = db.reload();
  d = sm3::cat_table(cat, t);
  REQUIRE(d != nullptr);
  CHECK(d->indexes[0].state == sm3::index_state_t::failed);
  CHECK(d->indexes[0].error == sm1::SM_ERR_DUPKEY);
  CHECK(d->indexes[0].reason == "two rows");
  CHECK(d->indexes[1].state == sm3::index_state_t::valid);

  // A state other than failed keeps no reason.
  db.begin();
  REQUIRE(sm3::cat_set_index_state(db.ss, t, fresh,
                                   sm3::index_state_t::building) == 0);
  db.commit();
  d = sm3::cat_table(db.reload(), t);
  CHECK(d->indexes[0].state == sm3::index_state_t::building);
  CHECK(d->indexes[0].error == sm1::SM_OK);
  CHECK(d->indexes[0].reason.empty());
}

namespace {

/* An evaluator that answers nothing: for a test that only records. */
int no_eval(const sm3::table_def_t &, const sm3::index_def_t &,
            std::span<const sm2::datum_t>, std::span<const bool>, bool &belongs,
            std::vector<std::string> &, std::span<bool>) {
  belongs = true;
  return 0;
}

} // namespace

TEST_CASE("an evaluated index keeps its blob, its columns and its types") {
  // Verify what the catalog records for a partial or expression
  // index (SM3_DESIGN.md §3).
  // 1. On t (a int4, b text), committed, create an index keyed by an
  //    expression of type text, descending, then by column a, with a
  //    blob that holds a NUL byte, reading columns b and a.
  // 2. A partial index over a plain column is recorded the same way,
  //    with no expression column.
  // 3. After a restart: each index's blob byte for byte, the columns
  //    it reads, -1 for the expression's place, and the key built
  //    for sm2 with the expression's own type.
  // 4. A plain index has an empty blob and reads nothing.
  cat_db_t db;
  REQUIRE(sm3::cat_set_evaluator(db.cat, no_eval) == 0);
  db.begin();
  uint64_t t = make_t(db);
  db.commit();

  std::string blob("lower(b)\0a > 0", 14);
  std::vector<sm3::index_key_spec_t> expr_then_a = {
      {-1, true, false, std::nullopt, sm2::type_text.code, -1},
      {0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<int32_t> reads = {1, 0}, reads_b = {1};
  uint64_t expr = 0, partial = 0, plain = 0;
  db.begin();
  REQUIRE_MESSAGE(sm3::cat_create_index(db.ss, t, "t_expr", expr_then_a, {},
                                        false, blob, reads, expr) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_partial", on_a, {}, true,
                                "b is not null", reads_b, partial) == 0);
  REQUIRE(sm3::cat_create_index(db.ss, t, "t_plain", on_a, {}, false, plain) ==
          0);
  db.commit();

  auto d = sm3::cat_table(db.reload(), t);
  REQUIRE(d != nullptr);
  REQUIRE(d->indexes.size() == 3);
  const sm3::index_def_t &x = d->indexes[0];
  CHECK(x.eval == blob);
  CHECK(x.reads == reads);
  CHECK(x.key_attnums == std::vector<int32_t>{-1, 0});
  REQUIRE(x.key.size() == 2);
  CHECK(x.key[0].type == &sm2::type_text);
  CHECK(x.key[0].typmod == -1);
  CHECK(x.key[0].desc);
  CHECK(x.key[1].type == &sm2::type_int4);
  const sm3::index_def_t &p = d->indexes[1];
  CHECK(p.eval == "b is not null");
  CHECK(p.reads == reads_b);
  CHECK(p.key_attnums == std::vector<int32_t>{0});
  CHECK(p.unique);
  CHECK(d->indexes[2].eval.empty());
  CHECK(d->indexes[2].reads.empty());
}

TEST_CASE("the catalog refuses an evaluated index it cannot keep") {
  // Verify cat_create_index's errors for partial and expression
  // indexes; each is SM_ERR_INVALID.
  // 1. With no evaluator given to the catalog, any index with a blob.
  // 2. An expression key column, or columns read, with no blob.
  // 3. An expression of a type sm2 does not have, and a collation on
  //    an expression that is not text.
  // 4. A column read that is not a live column of the table.
  // 5. A column an index reads cannot be dropped.
  // 6. Giving the catalog a second evaluator, or an empty one.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ci = 0, ix = 0;
  REQUIRE(sm3::cat_create_collation(db.ss, sm3::nsp_public, "ci",
                                    "und-u-ks-level2", false, ci) == 0);
  db.commit();
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> expr = {
      {-1, false, false, std::nullopt, sm2::type_text.code, -1}};
  std::vector<sm3::index_key_spec_t> expr_no_type = {
      {-1, false, false, std::nullopt, 200, -1}};
  std::vector<sm3::index_key_spec_t> int_expr_coll = {
      {-1, false, false, ci, sm2::type_int4.code, -1}};
  std::vector<int32_t> reads_b = {1}, reads_far = {7};
  auto refused = [&](std::span<const sm3::index_key_spec_t> keys,
                     std::string_view blob, std::span<const int32_t> reads) {
    int rc =
        sm3::cat_create_index(db.ss, t, "x", keys, {}, false, blob, reads, ix);
    return rc == -1 && sm1::errnum() == sm1::SM_ERR_INVALID;
  };

  db.begin();
  CHECK(refused(on_a, "a > 0", {}));
  CHECK(sm3::cat_set_evaluator(db.cat, nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::cat_set_evaluator(db.cat, no_eval) == 0);
  CHECK(sm3::cat_set_evaluator(db.cat, no_eval) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  CHECK(refused(expr, "", {}));
  CHECK(refused(on_a, "", reads_b));
  CHECK(refused(expr_no_type, "f(b)", reads_b));
  CHECK(refused(int_expr_coll, "f(b)", reads_b));
  CHECK(refused(on_a, "c > 0", reads_far));

  REQUIRE_MESSAGE(sm3::cat_create_index(db.ss, t, "t_partial", on_a, {}, false,
                                        "b is not null", reads_b, ix) == 0,
                  sm1::errmsg());
  CHECK(sm3::cat_drop_column(db.ss, t, "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();
}
