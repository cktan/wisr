/*
 * test_pin.cc -- sm3's pins (../ROW.md §4): the pool and its refs,
 * which a release makes stale; when the session releases them; a
 * cursor's pin, which each row moves on; and a row deformed through
 * a ref, as far as its caller reads.
 */

#include "../src/table/pin.h"
#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <optional>
#include <string>
#include <vector>

namespace {

/*
 * A catalog with t (a int4 not null, b text, c int4), rows (1, "one",
 * 10), (2, "two", null) and (3, "three", 30) committed, and a
 * session over it.
 */
class pins_t : public db_t {
public:
  /* Initialize and load the catalog, create t and its rows. */
  pins_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, open(), s) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"a", sm2::type_int4.code, -1, true},
        {"b", sm2::type_text.code, -1, false},
        {"c", sm2::type_int4.code, -1, false}};
    REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(s, sm3::nsp_public, "t", cols, oid) == 0);
    REQUIRE(sm3::commit(s) == 0);
    REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
    sm3::table_t *t = open_t();
    rows[0] = put(t, 1, "one", 10);
    rows[1] = put(t, 2, "two", std::nullopt);
    rows[2] = put(t, 3, "three", 30);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(s) == 0);
  }

  /* Close the session and the catalog. */
  ~pins_t() {
    sm3::session_close(s);
    sm3::cat_close(cat);
  }

  /* t, opened to write in s's transaction. */
  sm3::table_t *open_t() {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IX, t) == 0,
                    sm1::errmsg());
    return t;
  }

  /* Insert (a, b, c) into t; its rowno. */
  uint64_t put(sm3::table_t *t, int32_t a, const std::string &b,
               std::optional<int32_t> c) {
    std::string vb = sm2::make_varlena(b);
    sm2::datum_t d[3] = {sm2::datum_from_int32(a),
                         sm2::datum_from_ptr(vb.data()),
                         sm2::datum_from_int32(c.value_or(0))};
    bool n[3] = {false, false, !c};
    uint64_t rowno = 0;
    REQUIRE_MESSAGE(sm3::tbl_insert(t, d, n, rowno) == 0, sm1::errmsg());
    return rowno;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *s = nullptr;
  uint64_t oid = 0;
  uint64_t rows[3] = {};
};

/* The row ref pins, deformed whole, as "a|b|c", "-" for null. */
std::string show(sm3::pin_ref_t ref) {
  sm2::datum_t d[3];
  bool n[3];
  REQUIRE_MESSAGE(sm3::pin_deform(ref, 0, 3, d, n) == 0, sm1::errmsg());
  std::string_view b;
  if (!n[1]) {
    REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], b) == 0);
  }
  return std::to_string(sm2::datum_to_int32(d[0])) + "|" +
         (n[1] ? "-" : std::string(sm2::varlena_payload(b))) + "|" +
         (n[2] ? "-" : std::to_string(sm2::datum_to_int32(d[2])));
}

/*
 * The pins on the list head, following next and checking prev; -1
 * if it is not a well-formed list of at most limit pins.
 */
int list_len(const sm3::pin_t *head, int limit = 100) {
  int n = 0;
  for (const sm3::pin_t *p = head, *prev = nullptr; p; prev = p, p = p->next) {
    if (p->prev != prev || ++n > limit) {
      return -1;
    }
  }
  return n;
}

/* Whether ref is stale: pin_tuple refuses it as SM_ERR_INVALID. */
bool stale(sm3::pin_ref_t ref) {
  std::string_view v;
  return sm3::pin_tuple(ref, v) == -1 && sm1::errnum() == sm1::SM_ERR_INVALID;
}

} // namespace

TEST_CASE("a pinned row outlives the session's other calls") {
  // Verify two rows pinned at once stay valid through other calls.
  // 1. Read rows 1 and 2, each into a ref of its own.
  // 2. Read row 3 lent, insert a row, update row 3, scan the table.
  // 3. Both pinned rows still deform as they did, from the same bytes.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::pin_ref_t r1, r2;
  std::string_view one, two;
  REQUIRE(sm3::tbl_read(t, f.rows[0], r1, one) == 0);
  REQUIRE(sm3::tbl_read(t, f.rows[1], r2, two) == 0);

  std::string_view lent;
  REQUIRE(sm3::tbl_read(t, f.rows[2], lent) == 0);
  f.put(t, 4, "four", 40);
  std::string vb = sm2::make_varlena("tres");
  sm2::datum_t d[3] = {sm2::datum_from_int32(3), sm2::datum_from_ptr(vb.data()),
                       sm2::datum_from_int32(33)};
  bool n[3] = {false, false, false};
  REQUIRE(sm3::tbl_update(t, f.rows[2], d, n) == 0);
  sm3::scan_t *c = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, c) == 0);
  uint64_t rowno;
  std::string_view v;
  while (sm3::tbl_scannxt(c, rowno, v) == 0) {
  }
  sm3::tbl_scanend(c);

  std::string_view now;
  REQUIRE(sm3::pin_tuple(r1, now) == 0);
  CHECK(now.data() == one.data());
  CHECK(show(r1) == "1|one|10");
  CHECK(show(r2) == "2|two|-");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("a stale ref names nothing, though its pin is reused") {
  // Verify a generation keeps an old ref from releasing a new row.
  // 1. Read row 1 into r1, keep a copy of r1, and unpin r1.
  // 2. Read row 2 into r2: it takes the same pin, at a new generation.
  // 3. The stale copy is refused, and unpinning it leaves r2's row.
  // 4. Unpin r2 twice: the second does nothing.
  // 5. A read into a ref releases the pin it held: the next read
  //    into r2 takes that pin again.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::pin_ref_t r1, r2;
  std::string_view v;
  REQUIRE(sm3::tbl_read(t, f.rows[0], r1, v) == 0);
  sm3::pin_ref_t copy = r1;
  sm3::unpin(r1);
  CHECK(r1.pin == nullptr);
  CHECK(stale(copy));

  REQUIRE(sm3::tbl_read(t, f.rows[1], r2, v) == 0);
  CHECK(r2.pin == copy.pin);
  CHECK(r2.gen != copy.gen);
  CHECK(stale(copy));
  sm3::unpin(copy);
  CHECK(show(r2) == "2|two|-");

  sm3::pin_ref_t again = r2;
  sm3::unpin(r2);
  sm3::unpin(r2);
  CHECK(stale(again));

  REQUIRE(sm3::tbl_read(t, f.rows[0], r2, v) == 0);
  sm3::pin_t *held = r2.pin;
  REQUIRE(sm3::tbl_read(t, f.rows[2], r2, v) == 0);
  CHECK(r2.pin == held);
  CHECK(show(r2) == "3|three|30");
  CHECK(sm3::tbl_read(t, f.rows[2] + 100, r2, v) == 1);
  CHECK(r2.pin == nullptr);
  CHECK(v.empty());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("a read into a ref rereads into its pin; its copies go stale") {
  // Verify a read into a live ref keeps the ref's pin, whose
  // iterator sm1 may seek again, and still makes old refs stale.
  // 1. Read row 1 into r and keep a copy of r.
  // 2. Read row 2 into r: the same pin, at a new generation; the
  //    copy is refused, and r deforms row 2.
  // 3. Read row 3 into r3, then row 1 into r again: r3 is untouched.
  // 4. Commit: no pin is held, and every pin of the pool is on
  //    its free list once; the next transaction's two reads take
  //    two pins.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::pin_ref_t r, r3;
  std::string_view v;
  REQUIRE(sm3::tbl_read(t, f.rows[0], r, v) == 0);
  sm3::pin_ref_t copy = r;

  REQUIRE(sm3::tbl_read(t, f.rows[1], r, v) == 0);
  CHECK(r.pin == copy.pin);
  CHECK(r.gen != copy.gen);
  CHECK(stale(copy));
  CHECK(show(r) == "2|two|-");

  REQUIRE(sm3::tbl_read(t, f.rows[2], r3, v) == 0);
  REQUIRE(sm3::tbl_read(t, f.rows[0], r, v) == 0);
  CHECK(r.pin != r3.pin);
  CHECK(show(r) == "1|one|10");
  CHECK(show(r3) == "3|three|30");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
  CHECK(stale(r));
  CHECK(stale(r3));
  CHECK(list_len(f.s->pins_held) == 0);
  CHECK(list_len(f.s->pins_free) == (int)f.s->pins.size());

  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  t = f.open_t();
  sm3::pin_ref_t a, b;
  REQUIRE(sm3::tbl_read(t, f.rows[0], a, v) == 0);
  REQUIRE(sm3::tbl_read(t, f.rows[1], b, v) == 0);
  CHECK(a.pin != b.pin);
  CHECK(show(a) == "1|one|10");
  CHECK(show(b) == "2|two|-");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("the end of a transaction releases its pins") {
  // Verify commit, abort and prepare release every pin, and
  // release_all_pins does without ending the transaction.
  // 1. For each way to end: begin, pin row 1 by a read and row 2 by
  //    an update, and take a cursor's row.
  // 2. End: every ref is stale.
  // 3. release_all_pins: the refs are stale, the cursor reads on,
  //    and the transaction commits.
  // 4. A rollback to a savepoint releases no pin.
  pins_t f;
  for (int how = 0; how < 4; how++) {
    CAPTURE(how);
    REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::repeatable_read) == 0);
    sm3::table_t *t = f.open_t();
    sm3::pin_ref_t rr, ru, rc;
    std::string_view v;
    REQUIRE(sm3::tbl_read(t, f.rows[0], rr, v) == 0);
    std::string vb = sm2::make_varlena("dos");
    sm2::datum_t d[3] = {sm2::datum_from_int32(2),
                         sm2::datum_from_ptr(vb.data()), 0};
    bool n[3] = {false, false, true};
    REQUIRE(sm3::tbl_update(t, f.rows[1], d, n, ru, v) == 0);
    // The first round commits its update; the others undo theirs.
    CHECK(show(ru) == (how == 0 ? "2|two|-" : "2|dos|-"));
    sm3::scan_t *c = nullptr;
    REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, c) == 0);
    uint64_t rowno;
    REQUIRE(sm3::tbl_scannxt(c, rowno, rc, v) == 0);
    if (how == 3) {
      sm3::release_all_pins(f.s);
      CHECK(stale(rr));
      CHECK(stale(ru));
      CHECK(stale(rc));
      REQUIRE(sm3::tbl_scannxt(c, rowno, rc, v) == 0);
      CHECK(show(rc) == "2|dos|-");
      REQUIRE(sm3::savepoint(f.s) == 1);
      REQUIRE(sm3::tbl_read(t, f.rows[0], rr, v) == 0);
      REQUIRE(sm3::rollback(f.s, 1) == 0);
      CHECK(show(rr) == "1|one|10");
      sm3::tbl_scanend(c);
      sm3::tbl_close(t);
      REQUIRE(sm3::abort(f.s) == 0);
      continue;
    }
    sm3::tbl_scanend(c);
    rc = {};
    sm3::tbl_close(t);
    if (how == 0) {
      REQUIRE(sm3::commit(f.s) == 0);
    } else if (how == 1) {
      REQUIRE(sm3::abort(f.s) == 0);
    } else {
      REQUIRE(sm3::prepare(f.s, "g") == 0);
      REQUIRE(sm1::commit_prepared(sm3::session_sm1(f.s), "g") == 0);
    }
    CHECK(stale(rr));
    CHECK(stale(ru));
  }
}

TEST_CASE("a cursor's row is a pin, moved on by each row") {
  // Verify tbl_scannxt hands its row back in the cursor's pin.
  // 1. Take the first row's ref: it deforms as row 1.
  // 2. Take the next row: the first ref is stale, the new one reads
  //    row 2, from the same pin.
  // 3. Unpin a copy of the cursor's ref: the cursor's row stays.
  // 4. At the end the last ref is stale; so, after tbl_scanend, is
  //    the pin, which the pool hands to the next read.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::scan_t *c = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, c) == 0);
  uint64_t rowno;
  sm3::pin_ref_t r1, r2;
  std::string_view v;
  REQUIRE(sm3::tbl_scannxt(c, rowno, r1, v) == 0);
  CHECK(rowno == f.rows[0]);
  CHECK(show(r1) == "1|one|10");
  REQUIRE(sm3::tbl_scannxt(c, rowno, r2, v) == 0);
  CHECK(stale(r1));
  CHECK(r2.pin == r1.pin);
  CHECK(show(r2) == "2|two|-");

  sm3::pin_ref_t copy = r2;
  sm3::unpin(copy);
  CHECK(show(r2) == "2|two|-");
  REQUIRE(sm3::tbl_scannxt(c, rowno, r2, v) == 0);
  sm3::pin_ref_t last = r2;
  CHECK(sm3::tbl_scannxt(c, rowno, r2, v) == 1);
  CHECK(r2.pin == nullptr);
  CHECK(stale(last));
  sm3::tbl_scanend(c);

  sm3::pin_ref_t r;
  REQUIRE(sm3::tbl_read(t, f.rows[0], r, v) == 0);
  CHECK(r.pin == last.pin);
  CHECK(show(r) == "1|one|10");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("a rollback that closes a cursor makes its row's ref stale") {
  // Verify a cursor sm1 closes takes its row's ref with it, and only it.
  // 1. Open cursor c1 and take a row, then set a savepoint.
  // 2. Open cursor c2 and take a row.
  // 3. Roll back to the savepoint: c2's ref is stale, c1's reads on.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::scan_t *c1 = nullptr, *c2 = nullptr;
  uint64_t rowno;
  sm3::pin_ref_t r1, r2;
  std::string_view v;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, c1) == 0);
  REQUIRE(sm3::tbl_scannxt(c1, rowno, r1, v) == 0);
  REQUIRE(sm3::savepoint(f.s) == 1);
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, c2) == 0);
  REQUIRE(sm3::tbl_scannxt(c2, rowno, r2, v) == 0);
  REQUIRE(sm3::rollback(f.s, 1) == 0);
  CHECK(stale(r2));
  CHECK(show(r1) == "1|one|10");
  sm3::tbl_scanend(c2);
  sm3::tbl_scanend(c1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("pin_deform resumes where it stopped, and reads only that far") {
  // Verify a row deformed in steps through a ref.
  // 1. Deform attribute 0, then 1 to 2: the values are row 1's.
  // 2. Deform from 0 again into other arrays: the pin starts over.
  // 3. pin_decode reads one attribute; pin_tuple the bytes.
  // 4. Past the schema, or through a stale ref, is SM_ERR_INVALID.
  pins_t f;
  REQUIRE(sm3::begin(f.s, sm1::xctlevel_t::read_committed) == 0);
  sm3::table_t *t = f.open_t();
  sm3::pin_ref_t r;
  std::string_view v;
  REQUIRE(sm3::tbl_read(t, f.rows[2], r, v) == 0);
  sm2::datum_t d[3];
  bool n[3];
  REQUIRE(sm3::pin_deform(r, 0, 1, d, n) == 0);
  CHECK(sm2::datum_to_int32(d[0]) == 3);
  REQUIRE(sm3::pin_deform(r, 1, 3, d, n) == 0);
  std::string_view b;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], b) == 0);
  CHECK(sm2::varlena_payload(b) == "three");
  CHECK(sm2::datum_to_int32(d[2]) == 30);

  sm2::datum_t e[3] = {};
  bool m[3] = {};
  REQUIRE(sm3::pin_deform(r, 0, 3, e, m) == 0);
  CHECK(sm2::datum_to_int32(e[0]) == 3);
  CHECK(sm2::datum_to_int32(e[2]) == 30);

  sm2::datum_t one;
  bool isnull;
  REQUIRE(sm3::pin_decode(r, 2, one, isnull) == 0);
  CHECK(!isnull);
  CHECK(sm2::datum_to_int32(one) == 30);
  std::string_view bytes;
  REQUIRE(sm3::pin_tuple(r, bytes) == 0);
  CHECK(bytes == v);

  CHECK(sm3::pin_deform(r, 0, 4, d, n) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::pin_ref_t copy = r;
  sm3::unpin(r);
  CHECK(sm3::pin_deform(copy, 0, 1, d, n) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::pin_decode(copy, 0, one, isnull) == -1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.s) == 0);
}
