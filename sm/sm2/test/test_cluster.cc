/*
 * test_cluster.cc -- a rel keyed by its primary key (../CLUSTER.md
 * §2 to §5, ../ROWKEY.md §2): rows in key order, with no _id, moved
 * when their key changes, and named by their key in an index.
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

/* Insert a row, which must go in; its _id, 0 in a keyed rel. */
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

TEST_CASE("a keyed rel keeps its rows by key, with no _id") {
  // Verify a rel keyed by a primary key (../../ROWKEY.md §2.1): rows
  // found by key, none with an _id, and a duplicate key refused.
  // 1. Insert rows with keys 30, 10 and 20: none draws an _id.
  // 2. Each row is read by its key, its _id 0.
  // 3. A scan by _id, and an _id reserved, are refused.
  // 4. A second row with key 10 fails with SM_ERR_DUPKEY.
  // 5. rel_row_key encodes a row's key; a NULL in it is refused.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  CHECK(sm2::rel_keyed(rel));
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<int32_t> keys = {30, 10, 20};
  for (int32_t a : keys) {
    CHECK(insert(rel, a, "row " + std::to_string(a)) == 0);
  }
  for (int32_t a : keys) {
    std::string_view tuple;
    uint64_t id = 99;
    REQUIRE(sm2::rel_read(rel, key_of(a), tuple, &id) == 0);
    CHECK(id == 0);
    CHECK(a_of(t, tuple) == a);
  }
  sm2::rel_scan_t *scan = nullptr;
  CHECK(sm2::rel_scan(rel, 0, UINT64_MAX, scan) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  uint64_t id;
  CHECK(sm2::rel_reserve(rel, id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
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

TEST_CASE("a keyed rel moves a row whose key changes") {
  // Verify rel_update in place and as a move, and rel_delete.
  // 1. A row with key 1 is updated without changing its key: it
  //    stays where it is.
  // 2. It is updated to key 5: the old key is gone, the row is at
  //    5.
  // 3. A move onto key 7, another row's, fails with SM_ERR_DUPKEY.
  // 4. Deleting it removes the row.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  insert(rel, 1, "one");
  insert(rel, 7, "seven");
  rawpin_ptr_t pin;
  std::string_view old;
  std::string at;
  row_t same(1, "uno");
  REQUIRE(sm2::rel_update(rel, key_of(1), 0, same.d, same.null, pin.get(), old,
                          false, &at) == 0);
  CHECK(at == key_of(1));
  row_t moved(5, "five");
  REQUIRE_MESSAGE(sm2::rel_update(rel, key_of(1), 0, moved.d, moved.null,
                                  pin.get(), old, false, &at) == 0,
                  sm1::errmsg());
  CHECK(at == key_of(5));
  CHECK(a_of(t, old) == 1);
  std::string_view tuple;
  CHECK(sm2::rel_read(rel, key_of(1), tuple) == 1);
  REQUIRE(sm2::rel_read(rel, key_of(5), tuple) == 0);
  CHECK(a_of(t, tuple) == 5);
  int mark = sm1::savepoint(s);
  REQUIRE(mark >= 0);
  row_t clash(7, "clash");
  CHECK(sm2::rel_update(rel, key_of(5), 0, clash.d, clash.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm1::rollback(s, mark) == 0);
  REQUIRE(sm2::rel_delete(rel, key_of(5)) == 0);
  CHECK(sm2::rel_read(rel, key_of(5), tuple) == 1);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a writer at READ COMMITTED meets a row another moved") {
  // Verify a move leaves a moved tombstone at the old key, which a
  // writer there meets (../../ROWKEY.md §2.3).
  // 1. Session a inserts key 1 and commits.
  // 2. Session b begins at READ COMMITTED and reads the row at 1.
  // 3. a moves the row to key 2 and commits.
  // 4. b's update at key 1 meets SM_ERR_UPDATED, and its newest
  //    read there SM_ERR_MOVED; a delete of a row is no move: its
  //    newest read finds none.
  db_t db;
  table_t t;
  sm1::session_t *a = db.open();
  sm1::session_t *b = db.open();
  sm2::rel_t *ra = t.open(db, a);
  sm2::rel_t *rb = t.open(db, b);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  insert(ra, 1, "one");
  insert(ra, 3, "three");
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::stmt_begin(b) == 0);
  std::string_view tuple;
  REQUIRE(sm2::rel_read(rb, key_of(1), tuple) == 0);
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  row_t moved(2, "two");
  REQUIRE(sm2::rel_update(ra, key_of(1), 0, moved.d, moved.null) == 0);
  REQUIRE(sm2::rel_delete(ra, key_of(3)) == 0);
  REQUIRE(sm1::commit(a) == 0);
  row_t mine(1, "mine");
  CHECK(sm2::rel_update(rb, key_of(1), 0, mine.d, mine.null) == -1);
  REQUIRE(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(sm2::rel_read_latest(rb, key_of(1), tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_MOVED);
  row_t gone(3, "gone");
  CHECK(sm2::rel_update(rb, key_of(3), 0, gone.d, gone.null) == -1);
  REQUIRE(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(sm2::rel_read_latest(rb, key_of(3), tuple) == 1);
  REQUIRE(sm1::commit(b) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}

TEST_CASE("a keyed table's index entries name their row by its key") {
  // Verify a secondary index of a keyed table, and its primary key
  // read as an index.
  // 1. Rows (10, "x"), (20, "y"), (30, "x") get entries in a
  //    non-unique index on b, each naming its row's key.
  // 2. A scan of it gives each entry's row key, and no _id.
  // 3. A unique index on b finds "y"'s row key.
  // 4. The primary key, opened as an index with b INCLUDEd, finds
  //    key 20's row, and scans keys 15 to 30 in key order, each
  //    entry's value uncut, b, then the row's key.
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
  };
  std::vector<entry_t> rows = {{10, "x"}, {20, "y"}, {30, "x"}};
  for (entry_t &e : rows) {
    insert(rel, e.a, e.b);
    row_t r(e.a, e.b);
    REQUIRE(sm2::idx_insert(ix, {&r.d[1], 1}, {&r.null[1], 1}, key_of(e.a), {},
                            {}) == 0);
    if (e.b == "y") {
      REQUIRE(sm2::idx_insert(ux, {&r.d[1], 1}, {&r.null[1], 1}, key_of(e.a),
                              {}, {}) == 0);
    }
  }
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm2::idx_scan(ix, nullptr, nullptr, scan) == 0);
  std::vector<std::string> seen;
  uint64_t id;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    CHECK(id == 0);
    seen.push_back(std::string(sm2::idx_scan_row(scan)));
  }
  sm2::idx_scanend(scan);
  CHECK(seen == std::vector<std::string>{key_of(10), key_of(30), key_of(20)});
  row_t y(0, "y");
  std::string row;
  REQUIRE(sm2::idx_find(ux, {&y.d[1], 1}, {&y.null[1], 1}, id, &row) == 0);
  CHECK(id == 0);
  CHECK(row == key_of(20));
  sm2::datum_t k20[1] = {sm2::datum_from_int32(20)};
  bool kn[1] = {false};
  REQUIRE(sm2::idx_find(px, k20, kn, id, &row) == 0);
  CHECK(row == key_of(20));
  sm2::datum_t k15[1] = {sm2::datum_from_int32(15)};
  sm2::bound_t lo{k15, kn, true};
  REQUIRE(sm2::idx_scan(px, &lo, nullptr, scan) == 0);
  sm2::tup_desc_t vdesc(sm2::idx_value_schema(px));
  std::vector<std::pair<std::string, std::string>> prim;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    CHECK_FALSE(cut);
    sm2::datum_t d;
    bool isnull;
    REQUIRE(sm2::tup_decode(vdesc, value, 1, d, isnull) == 0);
    std::string_view b, k;
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d, b) == 0);
    REQUIRE(sm2::tup_decode(vdesc, value, 2, d, isnull) == 0);
    REQUIRE(sm2::datum_bytes(&sm2::type_bytea, -1, d, k) == 0);
    CHECK(sm2::varlena_payload(k) == sm2::idx_scan_row(scan));
    prim.push_back({std::string(sm2::idx_scan_row(scan)),
                    std::string(sm2::varlena_payload(b))});
  }
  sm2::idx_scanend(scan);
  CHECK(prim == std::vector<std::pair<std::string, std::string>>{
                    {key_of(20), "y"}, {key_of(30), "x"}});
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(px);
  sm2::idx_close(ux);
  sm2::idx_close(ix);
  sm2::rel_close(rel);
}

TEST_CASE("a keyed rel loads rows out of key order") {
  // Verify rel_load into a keyed rel: its rows sorted at the close,
  // and a duplicate key found at the close.
  // 1. Load keys 3, 1, 2; each row is read by its key.
  // 2. A load with key 4 twice fails at its close with
  //    SM_ERR_DUPKEY.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm1::loader_t *load = nullptr;
  size_t rows = 0;
  REQUIRE(sm1::load_open(s, db.store(), load) == 0);
  REQUIRE(sm1::load_stream(load, false, rows) == 0);
  for (int32_t a : {3, 1, 2}) {
    row_t r(a, "loaded");
    uint64_t id = 99;
    REQUIRE_MESSAGE(sm2::rel_load(rel, load, rows, r.d, r.null, id) == 0,
                    sm1::errmsg());
    CHECK(id == 0);
  }
  REQUIRE_MESSAGE(sm1::load_close(load) == 0, sm1::errmsg());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int32_t a : {1, 2, 3}) {
    std::string_view tuple;
    REQUIRE(sm2::rel_read(rel, key_of(a), tuple) == 0);
    CHECK(a_of(t, tuple) == a);
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
  for (int i = 0; i < 2; i++) {
    row_t r(4, "twice");
    uint64_t id;
    REQUIRE(sm2::rel_load(other, load, rows, r.d, r.null, id) == 0);
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

TEST_CASE("a primary key's scan hands out its rows, in key order") {
  // Verify idx_scan_rows: the rows between two bounds, by key.
  // 1. Rows with keys 30, 10, 40 and 20 go in.
  // 2. A scan from 15 to 30, both included, gives keys 20 and 30,
  //    each with no _id, its tuple and its key.
  // 3. An unbounded one gives all four, in key order.
  // 4. Seeking such a cursor to an _id fails.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  sm2::index_t *px = nullptr;
  REQUIRE(sm2::idx_open_primary(rel, {}, {}, px) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  for (int32_t a : {30, 10, 40, 20}) {
    insert(rel, a, "row");
  }
  auto scan = [&](const sm2::bound_t *lo, const sm2::bound_t *hi) {
    sm2::rel_scan_t *c = nullptr;
    REQUIRE(sm2::idx_scan_rows(px, lo, hi, c) == 0);
    std::vector<std::pair<uint64_t, int32_t>> out;
    uint64_t id;
    std::string_view tuple;
    while (sm2::rel_scannxt(c, id, tuple) == 0) {
      out.push_back({id, a_of(t, tuple)});
      CHECK(sm2::rel_scan_key(c) == key_of(out.back().second));
    }
    CHECK(sm2::rel_scan_seek(c, 1) == -1);
    sm2::rel_scanend(c);
    return out;
  };
  sm2::datum_t d15[1] = {sm2::datum_from_int32(15)};
  sm2::datum_t d30[1] = {sm2::datum_from_int32(30)};
  bool n[1] = {false};
  sm2::bound_t lo{d15, n, true}, hi{d30, n, true};
  CHECK(scan(&lo, &hi) ==
        std::vector<std::pair<uint64_t, int32_t>>{{0, 20}, {0, 30}});
  CHECK(scan(nullptr, nullptr) == std::vector<std::pair<uint64_t, int32_t>>{
                                      {0, 10}, {0, 20}, {0, 30}, {0, 40}});
  REQUIRE(sm1::commit(s) == 0);
  sm2::idx_close(px);
  sm2::rel_close(rel);
}

TEST_CASE("a rel's keys at fractions of its bytes split its rows so") {
  // Verify rel_keys_at and rel_scan_seek_key (../ROWKEY.md §2.4).
  // 1. 20,000 rows of equal width go in, keys 0 to 19,999, and are
  //    committed; in memtables, the keys found ascend.
  // 2. Compacted into sorted files, the keys at 0, 1/4, 1/2 and 3/4
  //    have that share of the rows below them, to 1% of the rows.
  // 3. A cursor in key order seeks to the key at 1/2, and its next
  //    row is the first at or past it; seeking back fails.
  // 4. Fractions out of [0, 1) or descending are refused.
  db_t db;
  table_t t;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = t.open(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  const int32_t n = 20000;
  for (int32_t a = 0; a < n; a++) {
    insert(rel, a, std::string(100, 'x'));
  }
  REQUIRE(sm1::commit(s) == 0);
  const double fs[] = {0, 0.25, 0.5, 0.75};
  std::vector<std::string> keys;
  REQUIRE_MESSAGE(sm2::rel_keys_at(rel, fs, keys) == 0, sm1::errmsg());
  REQUIRE(keys.size() == 4);
  for (size_t i = 1; i < keys.size(); i++) {
    CHECK(keys[i - 1] <= keys[i]);
  }
  REQUIRE(sm1::compact(s, db.store()) == 0);
  REQUIRE_MESSAGE(sm2::rel_keys_at(rel, fs, keys) == 0, sm1::errmsg());
  // The rows whose key is below k: key_of ascends with a.
  auto below = [&](const std::string &k) {
    int32_t lo = 0, hi = n;
    while (lo < hi) {
      int32_t mid = lo + (hi - lo) / 2;
      if (key_of(mid) < k) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  };
  for (size_t i = 0; i < keys.size(); i++) {
    CAPTURE(fs[i]);
    CHECK(std::abs(below(keys[i]) - fs[i] * n) <= 0.01 * n);
  }
  sm2::index_t *px = nullptr;
  REQUIRE(sm2::idx_open_primary(rel, {}, {}, px) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm2::rel_scan_t *c = nullptr;
  REQUIRE(sm2::idx_scan_rows(px, nullptr, nullptr, c) == 0);
  REQUIRE(sm2::rel_scan_seek_key(c, keys[2]) == 0);
  uint64_t id;
  std::string_view tuple;
  REQUIRE(sm2::rel_scannxt(c, id, tuple) == 0);
  CHECK(a_of(t, tuple) == below(keys[2]));
  CHECK(sm2::rel_scan_seek_key(c, keys[1]) == -1);
  sm2::rel_scanend(c);
  REQUIRE(sm1::commit(s) == 0);
  const double bad[] = {0.5, 0.25};
  CHECK(sm2::rel_keys_at(rel, bad, keys) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  const double one[] = {1.0};
  CHECK(sm2::rel_keys_at(rel, one, keys) == -1);
  sm2::idx_close(px);
  sm2::rel_close(rel);
}
