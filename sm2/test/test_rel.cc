/*
 * test_rel.cc -- the heap rel over sm1 (SM2_DESIGN.md §5, §8.2 step
 * 4): rows in, rows out, under every isolation level, with sm1's
 * behaviour showing through untranslated.
 */

#include "datums.h"
#include "db.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <stdlib.h>

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

/* A row as optional owned bytes, one per attribute. */
using row_t = std::vector<std::optional<std::string>>;

/* row's values under schema, for the calls that take Datums. */
values_t datums_of(std::span<const sm2::attribute_t> schema, const row_t &row) {
  return values_of(types_of(schema), row, false);
}

/* tuple decoded under schema, as owned bytes. */
row_t decode(std::span<const sm2::attribute_t> schema, std::string_view tuple) {
  row_t out;
  REQUIRE_MESSAGE(bytes_of(schema, tuple, out) == 0, sm1::errmsg());
  return out;
}

/* A schema of every type, and a random row of it, nulls included. */
std::vector<sm2::attribute_t> wide_schema() {
  std::vector<sm2::attribute_t> schema;
  for (const typed_t &t : all_types()) {
    schema.push_back({t.type, t.typmod, false, std::nullopt});
  }
  return schema;
}

/* A random row of schema, a variable datum as the codec takes it. */
row_t gen_row(std::span<const sm2::attribute_t> schema, std::mt19937_64 &rng) {
  row_t row;
  for (const auto &a : schema) {
    if (rng() % 5 == 0) {
      row.push_back(std::nullopt);
      continue;
    }
    std::string d = gen_datum({a.type, a.typmod}, rng);
    row.push_back(sm2::datum_width(a.type, a.typmod) < 0 ? sm2::make_varlena(d)
                                                         : d);
  }
  return row;
}

/* Open a heap rel over db's store under relid; it must open. */
sm2::rel_t *open_rel(db_t &db, sm1::session_t *s,
                     std::span<const sm2::attribute_t> schema,
                     uint16_t relid = 0) {
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), relid, schema, "rowno", rel) == 0);
  return rel;
}

} // namespace

TEST_CASE("rows round trip through a heap rel at every isolation level") {
  // Verify rel_insert, rel_read and rel_scan, from test_seed().
  // 1. At each isolation level, insert random rows of a schema of
  //    every type, nulls included, and commit; each gets a new,
  //    increasing rowno.
  // 2. In a new transaction, rel_read of each rowno decodes to its
  //    row.
  // 3. rel_scan over the whole range yields every row once, in
  //    rowno order.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = wide_schema();
  sm2::rel_t *rel = open_rel(db, s, schema);
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    std::vector<uint64_t> rownos;
    std::vector<row_t> rows;
    REQUIRE(sm1::begin(s, level) == 0);
    for (int i = 0; i < 50; i++) {
      row_t row = gen_row(schema, rng);
      values_t d = datums_of(schema, row);
      uint64_t rowno;
      REQUIRE_MESSAGE(sm2::rel_insert(rel, d.d(), d.null(), rowno) == 0,
                      sm1::errmsg());
      if (!rownos.empty()) {
        CHECK(rowno > rownos.back());
      }
      rownos.push_back(rowno);
      rows.push_back(row);
    }
    REQUIRE(sm1::commit(s) == 0);

    REQUIRE(sm1::begin(s, level) == 0);
    for (size_t i = 0; i < rows.size(); i++) {
      std::string_view tuple;
      REQUIRE(sm2::rel_read(rel, rownos[i], tuple) == 0);
      CHECK(decode(schema, tuple) == rows[i]);
    }
    sm2::rel_scan_t *scan = nullptr;
    REQUIRE(sm2::rel_scan(rel, rownos.front(), rownos.back(), scan) == 0);
    size_t n = 0;
    uint64_t rowno;
    std::string_view tuple;
    while (sm2::rel_scannxt(scan, rowno, tuple) == 0) {
      REQUIRE(n < rows.size());
      CHECK(rowno == rownos[n]);
      CHECK(decode(schema, tuple) == rows[n]);
      n++;
    }
    CHECK(n == rows.size());
    sm2::rel_scanend(scan);
    REQUIRE(sm1::commit(s) == 0);
  }
  sm2::rel_close(rel);
}

TEST_CASE("an update keeps its rowno, and update and delete hand back the "
          "old row") {
  // Verify rel_update and rel_delete.
  // 1. Updating a row keeps its rowno, and hands back the tuple it
  //    replaced.
  // 2. The row then reads as the new one.
  // 3. Deleting it hands back that new tuple, and it reads as
  //    absent: rel_read returns 1.
  // 4. Updating or deleting a rowno with no row is sm1's
  //    SM_ERR_NOTFOUND, passed through.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt},
      {&sm2::type_text, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  row_t before = {le(int32_t{1}), sm2::make_varlena("before")};
  row_t after = {le(int32_t{2}), sm2::make_varlena("after")};
  values_t d = datums_of(schema, before);
  uint64_t rowno;
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), rowno) == 0);
  d = datums_of(schema, after);
  std::string_view old;
  REQUIRE(sm2::rel_update(rel, rowno, d.d(), d.null(), &old) == 0);
  CHECK(decode(schema, old) == before);
  std::string_view tuple;
  REQUIRE(sm2::rel_read(rel, rowno, tuple) == 0);
  CHECK(decode(schema, tuple) == after);
  REQUIRE(sm2::rel_delete(rel, rowno, &old) == 0);
  CHECK(decode(schema, old) == after);
  CHECK(sm2::rel_read(rel, rowno, tuple) == 1);
  CHECK(sm2::rel_update(rel, rowno, d.d(), d.null()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm2::rel_delete(rel, rowno + 100) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a savepoint rollback leaves no sm2 state behind") {
  // Verify sm2 keeps nothing a rollback must undo.
  // 1. Insert row A, set a savepoint, insert row B and update A.
  // 2. Roll back to the savepoint: B is gone and A is as first
  //    inserted.
  // 3. The same rel goes on working: another insert succeeds, and
  //    the commit keeps A and the new row.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int8, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  row_t a = {le(int64_t{10})}, a2 = {le(int64_t{11})}, b = {le(int64_t{20})};
  values_t d = datums_of(schema, a);
  uint64_t ra, rb, rc;
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), ra) == 0);
  int depth = sm1::savepoint(s);
  REQUIRE(depth >= 1);
  d = datums_of(schema, b);
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), rb) == 0);
  d = datums_of(schema, a2);
  REQUIRE(sm2::rel_update(rel, ra, d.d(), d.null()) == 0);
  REQUIRE(sm1::rollback(s, depth) == 0);
  std::string_view tuple;
  CHECK(sm2::rel_read(rel, rb, tuple) == 1);
  REQUIRE(sm2::rel_read(rel, ra, tuple) == 0);
  CHECK(decode(schema, tuple) == a);
  d = datums_of(schema, b);
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), rc) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm2::rel_read(rel, ra, tuple) == 0);
  CHECK(sm2::rel_read(rel, rc, tuple) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("two rels over one store, under two relids, never meet") {
  // Verify the relid separates rels in one sm1 store.
  // 1. Rels under relids 0 and 1 each insert rows; both draw
  //    rownos from the one sequence.
  // 2. Each rel's scan over every rowno yields only its own rows,
  //    and reading the other's rowno finds nothing.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *r0 = open_rel(db, s, schema, 0);
  sm2::rel_t *r1 = open_rel(db, s, schema, 1);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<uint64_t> rows0, rows1;
  for (int i = 0; i < 10; i++) {
    row_t row = {le(int32_t{i})};
    values_t d = datums_of(schema, row);
    uint64_t rowno;
    REQUIRE(sm2::rel_insert(i % 2 ? r1 : r0, d.d(), d.null(), rowno) == 0);
    (i % 2 ? rows1 : rows0).push_back(rowno);
  }
  for (auto [rel, mine, theirs] :
       {std::tuple{r0, &rows0, &rows1}, std::tuple{r1, &rows1, &rows0}}) {
    sm2::rel_scan_t *scan = nullptr;
    REQUIRE(sm2::rel_scan(rel, 0, UINT64_MAX, scan) == 0);
    std::vector<uint64_t> seen;
    uint64_t rowno;
    std::string_view tuple;
    while (sm2::rel_scannxt(scan, rowno, tuple) == 0) {
      seen.push_back(rowno);
    }
    sm2::rel_scanend(scan);
    CHECK(seen == *mine);
    CHECK(sm2::rel_read(rel, theirs->front(), tuple) == 1);
  }
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(r0);
  sm2::rel_close(r1);
}

TEST_CASE("sm1's SM_ERR_UPDATED passes through, and rel_read_latest "
          "answers it") {
  // Verify the READ COMMITTED path of SM2_DESIGN.md §5.
  // 1. A row is committed. Session A begins at READ COMMITTED and
  //    reads it; session B updates it and commits.
  // 2. A's update fails with SM_ERR_UPDATED, untranslated, and its
  //    transaction stays active.
  // 3. rel_read_latest shows A B's version; A's update then goes
  //    through and commits.
  db_t db;
  sm1::session_t *a = db.open(), *b = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *ra = open_rel(db, a, schema), *rb = open_rel(db, b, schema);
  row_t v1 = {le(int32_t{1})}, v2 = {le(int32_t{2})}, v3 = {le(int32_t{3})};
  values_t d = datums_of(schema, v1);
  uint64_t rowno;
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm2::rel_insert(ra, d.d(), d.null(), rowno) == 0);
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  std::string_view tuple;
  REQUIRE(sm2::rel_read(ra, rowno, tuple) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  d = datums_of(schema, v2);
  REQUIRE(sm2::rel_update(rb, rowno, d.d(), d.null()) == 0);
  REQUIRE(sm1::commit(b) == 0);

  d = datums_of(schema, v3);
  CHECK(sm2::rel_update(ra, rowno, d.d(), d.null()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  REQUIRE(sm2::rel_read_latest(ra, rowno, tuple) == 0);
  CHECK(decode(schema, tuple) == v2);
  REQUIRE(sm2::rel_update(ra, rowno, d.d(), d.null()) == 0);
  REQUIRE(sm1::commit(a) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}

TEST_CASE("a rel scan takes a rowno range and seeks forward") {
  // Verify rel_scan's bounds and rel_scan_seek.
  // 1. A scan from the 3rd row to the 7th yields those five, both
  //    ends included.
  // 2. Seeking forward to the 9th row resumes there.
  // 3. Seeking back is SM_ERR_INVALID, as sm1's seek is.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  std::vector<uint64_t> rownos;
  for (int i = 0; i < 10; i++) {
    row_t row = {le(int32_t{i})};
    values_t d = datums_of(schema, row);
    uint64_t rowno;
    REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), rowno) == 0);
    rownos.push_back(rowno);
  }
  sm2::rel_scan_t *scan = nullptr;
  REQUIRE(sm2::rel_scan(rel, rownos[2], rownos[6], scan) == 0);
  std::vector<uint64_t> seen;
  uint64_t rowno;
  std::string_view tuple;
  while (sm2::rel_scannxt(scan, rowno, tuple) == 0) {
    seen.push_back(rowno);
  }
  sm2::rel_scanend(scan);
  CHECK(seen == std::vector<uint64_t>(rownos.begin() + 2, rownos.begin() + 7));

  REQUIRE(sm2::rel_scan(rel, rownos[0], rownos[9], scan) == 0);
  REQUIRE(sm2::rel_scannxt(scan, rowno, tuple) == 0);
  REQUIRE(sm2::rel_scan_seek(scan, rownos[8]) == 0);
  REQUIRE(sm2::rel_scannxt(scan, rowno, tuple) == 0);
  CHECK(rowno == rownos[8]);
  CHECK(sm2::rel_scan_seek(scan, rownos[1]) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm2::rel_scanend(scan);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a row that cannot be encoded draws no rowno") {
  // Verify rel_insert encodes before it draws, and rel_open's
  // checks.
  // 1. An insert with the wrong number of datums fails with
  //    SM_ERR_INVALID.
  // 2. The next good insert's rowno follows the last good one's:
  //    the failure drew none.
  // 3. rel_open refuses a schema with an attribute sm2 cannot
  //    lay out, and a null session.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  row_t good = {le(int32_t{1})};
  values_t d = datums_of(schema, good);
  uint64_t first, second;
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), first) == 0);
  CHECK(sm2::rel_insert(rel, {}, {}, second) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm2::rel_insert(rel, d.d(), d.null(), second) == 0);
  CHECK(second == first + 1);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);

  std::vector<sm2::attribute_t> bad = {
      {&sm2::type_decimal64, 3, false, std::nullopt}};
  sm2::rel_t *out = nullptr;
  CHECK(sm2::rel_open(s, db.store(), 0, bad, "rowno", out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm2::rel_open(nullptr, db.store(), 0, schema, "rowno", out) == -1);
}

TEST_CASE("rel supports raw tuple bytes and datum_t/isnull writes") {
  // Verify a heap rel stores exactly the tuple it is given, from
  // pre-encoded bytes or from Datums and null flags.
  // 1. Insert a pre-encoded tuple and read the same bytes back.
  // 2. Update it with another pre-encoded tuple; the old one comes
  //    back and the new one reads.
  // 3. Insert from Datums and null flags; it reads as the same
  //    tuple encoded directly.
  // 4. Update from Datums and null flags, likewise.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt},
      {&sm2::type_text, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);

  // 1. Raw tuple byte insert and read.
  std::string vl1 = sm2::make_varlena("first");
  sm2::datum_t d1[2] = {sm2::datum_from_int32(100),
                        sm2::datum_from_ptr(vl1.data())};
  bool nulls1[2] = {false, false};
  std::string tuple1;
  REQUIRE(sm2::tup_encode(schema, d1, nulls1, tuple1) == 0);

  uint64_t r1;
  REQUIRE(sm2::rel_insert(rel, tuple1, r1) == 0);

  std::string_view read_t1;
  REQUIRE(sm2::rel_read(rel, r1, read_t1) == 0);
  CHECK(read_t1 == tuple1);

  // 2. Raw tuple byte update.
  std::string vl2 = sm2::make_varlena("second");
  sm2::datum_t d2[2] = {sm2::datum_from_int32(200),
                        sm2::datum_from_ptr(vl2.data())};
  bool nulls2[2] = {false, false};
  std::string tuple2;
  REQUIRE(sm2::tup_encode(schema, d2, nulls2, tuple2) == 0);

  std::string_view old_t;
  REQUIRE(sm2::rel_update(rel, r1, tuple2, &old_t) == 0);
  CHECK(old_t == tuple1);

  std::string_view read_t2;
  REQUIRE(sm2::rel_read(rel, r1, read_t2) == 0);
  CHECK(read_t2 == tuple2);

  // 3. rel_insert taking (datum_t, isnull) directly.
  uint64_t r2;
  REQUIRE(sm2::rel_insert(rel, d1, nulls1, r2) == 0);
  CHECK(r2 > r1);
  REQUIRE(sm2::rel_read(rel, r2, read_t1) == 0);
  CHECK(read_t1 == tuple1);

  // 4. rel_update taking (datum_t, isnull) directly.
  REQUIRE(sm2::rel_update(rel, r2, d2, nulls2, &old_t) == 0);
  CHECK(old_t == tuple1);
  REQUIRE(sm2::rel_read(rel, r2, read_t2) == 0);
  CHECK(read_t2 == tuple2);

  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a raw tuple is checked before it is written") {
  // Verify rel_insert and rel_update refuse a tuple the codec
  // could not have written, without drawing a rowno or changing
  // the row.
  // 1. A raw insert of malformed bytes fails with SM_ERR_INVALID.
  // 2. The next good insert's rowno follows the last good one's.
  // 3. A raw update with malformed bytes fails, and the row reads
  //    as it was.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *rel = open_rel(db, s, schema);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  sm2::datum_t d[1] = {sm2::datum_from_int32(1)};
  bool nulls[1] = {false};
  std::string good;
  REQUIRE(sm2::tup_encode(schema, d, nulls, good) == 0);

  uint64_t first, second;
  REQUIRE(sm2::rel_insert(rel, good, first) == 0);
  CHECK(sm2::rel_insert(rel, std::string_view("garbage"), second) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm2::rel_insert(rel, good, second) == 0);
  CHECK(second == first + 1);

  CHECK(sm2::rel_update(rel, first, std::string_view("garbage")) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string_view now;
  REQUIRE(sm2::rel_read(rel, first, now) == 0);
  CHECK(now == good);

  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("rel_lock locks a row without writing it") {
  // Verify the row lock an index build and SELECT FOR SHARE take.
  // 1. A row is committed. A locks it shared.
  // 2. B, set qa_nowait, locks it shared too, then updates it:
  //    SM_ERR_BLOCKED, for A's shared lock stands in the way.
  // 3. A commits; B's update goes through. The row was never
  //    written by A, so B meets no SM_ERR_UPDATED.
  db_t db;
  sm1::session_t *a = db.open(), *b = db.open();
  std::vector<sm2::attribute_t> schema = {
      {&sm2::type_int4, -1, false, std::nullopt}};
  sm2::rel_t *ra = open_rel(db, a, schema), *rb = open_rel(db, b, schema);
  row_t v1 = {le(int32_t{1})}, v2 = {le(int32_t{2})};
  values_t d = datums_of(schema, v1);
  uint64_t rowno;
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::rel_insert(ra, d.d(), d.null(), rowno) == 0);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE(sm1::set_qa_nowait(b, true) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm2::rel_lock(ra, rowno, sm1::lockmode_t::shared) == 0);
  CHECK(sm2::rel_lock(rb, rowno, sm1::lockmode_t::shared) == 0);
  d = datums_of(schema, v2);
  CHECK(sm2::rel_update(rb, rowno, d.d(), d.null()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  REQUIRE(sm1::commit(a) == 0);
  REQUIRE_MESSAGE(sm2::rel_update(rb, rowno, d.d(), d.null()) == 0,
                  sm1::errmsg());
  REQUIRE(sm1::commit(b) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}
