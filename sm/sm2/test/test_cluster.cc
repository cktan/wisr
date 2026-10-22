/*
 * test_cluster.cc -- a rel keyed by its primary key (../CLUSTER.md
 * §2 to §5): rows in key order, found by _id through the _id index,
 * moved when their key changes, and named by their key in an index.
 */

#include "db.h"
#include "doctest.h"

#include "sm1.h"
#include "sm2.h"

#include <string>
#include <vector>

namespace {

/* (a int4, b text), keyed by a. */
struct table_t {
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt},
      {&sm2::type_text, -1, false, std::nullopt}};
  std::vector<sm2::key_column_t> key = {
      {&sm2::type_int4, -1, nullptr, false, false}};
  std::vector<int32_t> atts = {0};

  /* Open the rel over db's store, under relid 1. */
  sm2::rel_t *open(db_t &db, sm1::session_t *s, const char *toast = "") {
    sm2::rel_t *rel = nullptr;
    REQUIRE_MESSAGE(sm2::rel_open(s, db.store(), 1, schema, {key, atts},
                                  "rowno", toast, rel) == 0,
                    sm1::errmsg());
    return rel;
  }
};

/* A row of the table: a, and b as a varlena. */
struct row_t {
  row_t(int32_t a, const std::string &b) : b(sm2::make_varlena(b)) {
    d[0] = sm2::datum_from_int32(a);
    d[1] = sm2::datum_from_ptr(this->b.data());
  }
  std::string b;
  sm2::datum_t d[2];
  bool null[2] = {false, false};
};

/* The key of a row whose primary key is a. */
std::string key_of(int32_t a) {
  sm2::key_column_t col = {&sm2::type_int4, -1, nullptr, false, false};
  sm2::datum_t d[1] = {sm2::datum_from_int32(a)};
  bool n[1] = {false};
  std::string k;
  REQUIRE(sm2::encode_key({&col, 1}, d, n, k) == 0);
  return k;
}

/* Column a of a tuple of the table. */
int32_t a_of(const table_t &t, std::string_view tuple) {
  sm2::tup_desc_t desc(t.schema);
  sm2::datum_t d;
  bool isnull;
  REQUIRE(sm2::tup_decode(desc, tuple, 0, d, isnull) == 0);
  return sm2::datum_to_int32(d);
}

/* Insert a row, which must go in; its _id. */
uint64_t insert(sm2::rel_t *rel, int32_t a, const std::string &b) {
  row_t r(a, b);
  uint64_t id = 0;
  std::string key;
  REQUIRE_MESSAGE(sm2::rel_insert(rel, r.d, r.null, id, &key) == 0,
                  sm1::errmsg());
  CHECK(key == key_of(a));
  return id;
}

} // namespace

TEST_CASE("a keyed rel keeps its rows by key, and finds them by _id") {
  // Verify a rel keyed by a primary key: rows found by key and by
  // _id, scanned in _id order, and a duplicate key refused.
  // 1. Insert rows with keys 30, 10 and 20, which get _ids 1 to 3.
  // 2. Each row's key is found by its _id, and the row by its key,
  //    with its _id.
  // 3. rel_scan walks them in _id order, each with its key.
  // 4. A second row with key 10 fails with SM_ERR_DUPKEY.
  // 5. rel_row_key encodes a row's key; a NULL in it is refused.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  CHECK(sm2::rel_keyed(rel));
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<int32_t> keys = {30, 10, 20};
  std::vector<uint64_t> ids;
  for (int32_t a : keys) {
    ids.push_back(insert(rel, a, "row " + std::to_string(a)));
  }
  CHECK(ids == std::vector<uint64_t>{1, 2, 3});
  for (size_t i = 0; i < keys.size(); i++) {
    std::string key;
    REQUIRE(sm2::rel_find(rel, ids[i], key) == 0);
    CHECK(key == key_of(keys[i]));
    std::string_view tuple;
    uint64_t id = 0;
    REQUIRE(sm2::rel_read(rel, key, tuple, &id) == 0);
    CHECK(id == ids[i]);
    CHECK(a_of(t, tuple) == keys[i]);
  }
  std::string none;
  CHECK(sm2::rel_find(rel, 99, none) == 1);
  sm2::rel_scan_t *scan = nullptr;
  REQUIRE(sm2::rel_scan(rel, 0, UINT64_MAX, scan) == 0);
  std::vector<int32_t> seen;
  uint64_t id;
  std::string_view tuple;
  while (sm2::rel_scannxt(scan, id, tuple) == 0) {
    seen.push_back(a_of(t, tuple));
    CHECK(sm2::rel_scan_key(scan) == key_of(seen.back()));
  }
  sm2::rel_scanend(scan);
  CHECK(seen == keys);
  row_t dup(10, "again");
  CHECK(sm2::rel_insert(rel, dup.d, dup.null, id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  std::string key;
  REQUIRE(sm2::rel_row_key(rel, 0, dup.d, dup.null, key) == 0);
  CHECK(key == key_of(10));
  dup.null[0] = true;
  CHECK(sm2::rel_row_key(rel, 0, dup.d, dup.null, key) == -1);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a keyed rel moves a row whose key changes, keeping its _id") {
  // Verify rel_update in place and as a move, and rel_delete.
  // 1. A row with key 1 is updated without changing its key: it
  //    stays where it is.
  // 2. It is updated to key 5: the old key is gone, the row is at
  //    5 with its _id, and its _id finds it there.
  // 3. A move onto key 7, another row's, fails with SM_ERR_DUPKEY.
  // 4. Deleting it removes the row and its _id entry.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t id = insert(rel, 1, "one");
  insert(rel, 7, "seven");
  rawpin_ptr_t pin;
  std::string_view old;
  std::string at;
  row_t same(1, "uno");
  REQUIRE(sm2::rel_update(rel, key_of(1), id, same.d, same.null, pin.get(), old,
                          false, &at) == 0);
  CHECK(at == key_of(1));
  row_t moved(5, "five");
  REQUIRE_MESSAGE(sm2::rel_update(rel, key_of(1), id, moved.d, moved.null,
                                  pin.get(), old, false, &at) == 0,
                  sm1::errmsg());
  CHECK(at == key_of(5));
  CHECK(a_of(t, old) == 1);
  std::string_view tuple;
  CHECK(sm2::rel_read(rel, key_of(1), tuple) == 1);
  uint64_t got = 0;
  REQUIRE(sm2::rel_read(rel, key_of(5), tuple, &got) == 0);
  CHECK(got == id);
  std::string key;
  REQUIRE(sm2::rel_find(rel, id, key) == 0);
  CHECK(key == key_of(5));
  int mark = sm1::savepoint(s);
  REQUIRE(mark >= 0);
  row_t clash(7, "clash");
  CHECK(sm2::rel_update(rel, key_of(5), id, clash.d, clash.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm1::rollback(s, mark) == 0);
  REQUIRE(sm2::rel_delete(rel, key_of(5)) == 0);
  CHECK(sm2::rel_read(rel, key_of(5), tuple) == 1);
  CHECK(sm2::rel_find(rel, id, key) == 1);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a writer at READ COMMITTED follows a row another moved") {
  // Verify rel_find_latest finds a row moved since the snapshot.
  // 1. Session a inserts key 1 and commits.
  // 2. Session b begins at READ COMMITTED and reads the row's key
  //    by its _id: 1.
  // 3. a moves the row to key 2 and commits.
  // 4. b's snapshot still finds key 1; under rel_lock_id, the
  //    newest version of the _id entry finds key 2.
  db_t db;
  table_t t;
  sm1::session_t *a = db.open();
  sm1::session_t *b = db.open();
  sm2::rel_t *ra = t.open(db, a);
  sm2::rel_t *rb = t.open(db, b);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  uint64_t id = insert(ra, 1, "one");
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(b) == 0);
  std::string key;
  REQUIRE(sm2::rel_find(rb, id, key) == 0);
  CHECK(key == key_of(1));
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  row_t moved(2, "two");
  REQUIRE(sm2::rel_update(ra, key_of(1), id, moved.d, moved.null) == 0);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm2::rel_find(rb, id, key) == 0);
  CHECK(key == key_of(1));
  REQUIRE(sm2::rel_lock_id(rb, id, sm1::SM_LOCKMODE_SH) == 0);
  REQUIRE_MESSAGE(sm2::rel_find_latest(rb, id, key) == 0, sm1::errmsg());
  CHECK(key == key_of(2));
  REQUIRE(sm1::commit(b) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}

TEST_CASE("a keyed table's index entries name their row by its key") {
  // Verify a secondary index of a keyed table, and its primary key
  // read as an index.
  // 1. Rows (10, "x"), (20, "y"), (30, "x") get entries in a
  //    non-unique index on b, each naming its row's _id and key.
  // 2. A scan of it gives each entry's _id and row key.
  // 3. A unique index on b finds "y"'s row, _id and key.
  // 4. The primary key, opened as an index with b INCLUDEd, finds
  //    key 20's row, and scans keys 15 to 30 in key order, each
  //    entry's value its _id, uncut, and b.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  std::vector<sm2::key_column_t> bcol = {
      {&sm2::type_text, -1, nullptr, false, false}};
  sm2::index_t *ix = nullptr, *ux = nullptr, *px = nullptr;
  REQUIRE(sm2::idx_open(s, db.store(), 2, bcol, false, {}, true, ix) == 0);
  REQUIRE(sm2::idx_open(s, db.store(), 3, bcol, true, {}, true, ux) == 0);
  std::vector<sm2::attribute_t> inc = {t.schema[1]};
  std::vector<int32_t> inc_atts = {1};
  REQUIRE(sm2::idx_open_primary(rel, inc, inc_atts, px) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  struct entry_t {
    int32_t a;
    std::string b;
    uint64_t id;
  };
  std::vector<entry_t> rows = {{10, "x", 0}, {20, "y", 0}, {30, "x", 0}};
  for (entry_t &e : rows) {
    e.id = insert(rel, e.a, e.b);
    row_t r(e.a, e.b);
    REQUIRE(sm2::idx_insert(ix, {&r.d[1], 1}, {&r.null[1], 1}, e.id,
                            key_of(e.a), {}, {}) == 0);
    if (e.b == "y") {
      REQUIRE(sm2::idx_insert(ux, {&r.d[1], 1}, {&r.null[1], 1}, e.id,
                              key_of(e.a), {}, {}) == 0);
    }
  }
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  std::vector<std::pair<uint64_t, std::string>> seen;
  uint64_t id;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    seen.push_back({id, std::string(sm2::idx_scan_row(scan))});
  }
  sm2::idx_scanend(scan);
  CHECK(seen == std::vector<std::pair<uint64_t, std::string>>{
                    {rows[0].id, key_of(10)},
                    {rows[2].id, key_of(30)},
                    {rows[1].id, key_of(20)}});
  row_t y(0, "y");
  std::string row;
  REQUIRE(sm2::idx_find(ux, {&y.d[1], 1}, {&y.null[1], 1}, id, &row) == 0);
  CHECK(id == rows[1].id);
  CHECK(row == key_of(20));
  sm2::datum_t k20[1] = {sm2::datum_from_int32(20)};
  bool kn[1] = {false};
  REQUIRE(sm2::idx_find(px, k20, kn, id, &row) == 0);
  CHECK(id == rows[1].id);
  CHECK(row == key_of(20));
  sm2::datum_t k15[1] = {sm2::datum_from_int32(15)};
  sm2::bound_t lo{k15, kn, true};
  REQUIRE(sm2::idx_scan(px, &lo, nullptr, scan) == 0);
  sm2::tup_desc_t vdesc(sm2::idx_value_schema(px));
  std::vector<std::pair<uint64_t, std::string>> prim;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    CHECK_FALSE(cut);
    sm2::datum_t d;
    bool isnull;
    REQUIRE(sm2::tup_decode(vdesc, value, 0, d, isnull) == 0);
    CHECK(static_cast<uint64_t>(sm2::datum_to_int64(d)) == id);
    REQUIRE(sm2::tup_decode(vdesc, value, 2, d, isnull) == 0);
    std::string_view b;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d, b) == 0);
    prim.push_back({id, std::string(sm2::varlena_payload(b))});
    CHECK(sm2::idx_scan_row(scan) == key_of(id == rows[1].id ? 20 : 30));
  }
  sm2::idx_scanend(scan);
  CHECK(prim == std::vector<std::pair<uint64_t, std::string>>{
                    {rows[1].id, "y"}, {rows[2].id, "x"}});
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(px);
  sm2::idx_close(ux);
  sm2::idx_close(ix);
  sm2::rel_close(rel);
}

TEST_CASE("a keyed rel loads rows out of key order") {
  // Verify rel_load into a keyed rel: its rows sorted at the close,
  // its _id entries in order, and a duplicate key found at the
  // close.
  // 1. Load keys 3, 1, 2; the rows come out by key, and by _id.
  // 2. A load with key 4 twice fails at its close with
  //    SM_ERR_DUPKEY.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm1::loader_t *load = nullptr;
  size_t rows = 0, ids = 0;
  REQUIRE(sm1::load_open(s, db.store(), load) == 0);
  REQUIRE(sm1::load_stream(load, false, rows) == 0);
  REQUIRE(sm1::load_stream(load, true, ids) == 0);
  for (int32_t a : {3, 1, 2}) {
    row_t r(a, "loaded");
    uint64_t id;
    REQUIRE_MESSAGE(sm2::rel_load(rel, load, rows, ids, r.d, r.null, id) == 0,
                    sm1::errmsg());
  }
  REQUIRE_MESSAGE(sm1::load_close(load) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (uint64_t id = 1; id <= 3; id++) {
    std::string key;
    REQUIRE(sm2::rel_find(rel, id, key) == 0);
    std::string_view tuple;
    REQUIRE(sm2::rel_read(rel, key, tuple) == 0);
    CHECK(a_of(t, tuple) == std::vector<int32_t>{3, 1, 2}[id - 1]);
  }
  REQUIRE(sm1::commit(s) == 0);
  // A load goes only into an empty store: a second one.
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = 0;
  REQUIRE(sm1::mkstore(s, store) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_t *other = nullptr;
  REQUIRE(sm2::rel_open(s, store, 1, t.schema, {t.key, t.atts}, "rowno", "",
                        other) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::load_open(s, store, load) == 0);
  REQUIRE(sm1::load_stream(load, false, rows) == 0);
  REQUIRE(sm1::load_stream(load, true, ids) == 0);
  for (int i = 0; i < 2; i++) {
    row_t r(4, "twice");
    uint64_t id;
    REQUIRE(sm2::rel_load(other, load, rows, ids, r.d, r.null, id) == 0);
  }
  CHECK(sm1::load_close(load) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm1::abort(s);
  sm2::rel_close(other);
  sm2::rel_close(rel);
}

TEST_CASE("a primary key past max_pk_len is refused") {
  // Verify a key too long to name a row fails before it is written.
  // 1. A rel keyed by a text column refuses a row whose key is
  //    2000 bytes, with SM_ERR_KEYTOOLONG.
  // 2. A short key goes in.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_text, -1, false, std::nullopt}};
  std::vector<sm2::key_column_t> key = {
      {&sm2::type_text, -1, nullptr, false, false}};
  std::vector<int32_t> atts = {0};
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 1, schema, {key, atts}, "rowno", "toast",
                        rel) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::string longer = sm2::make_varlena(std::string(2000, 'k'));
  sm2::datum_t d[1] = {sm2::datum_from_ptr(longer.data())};
  bool n[1] = {false};
  uint64_t id;
  CHECK(sm2::rel_insert(rel, d, n, id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_KEYTOOLONG);
  std::string shorter = sm2::make_varlena("short");
  d[0] = sm2::datum_from_ptr(shorter.data());
  REQUIRE(sm2::rel_insert(rel, d, n, id) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}
