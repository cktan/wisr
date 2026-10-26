/*
 * test_toast.cc -- TOAST (SM2_DESIGN.md §4.2): a row's long datums
 * out of line, in the table's own store, under the row's
 * transaction.
 */

#include "db.h"
#include "doctest.h"

#include "sm1.h"
#include "sm2.h"

#include <optional>
#include <string>
#include <vector>

namespace {

/* A row of the test table: an int4 and two texts, either null. */
struct row_t {
  int32_t k;
  std::optional<std::string> a, b;
};

/* The test table's schema: k int4, a text, b text. */
const std::vector<sm2::attribute_t> &schema() {
  static const std::vector<sm2::attribute_t> s = {
      {&sm2::type_int4, -1, false, std::nullopt},
      {&sm2::type_text, -1, false, std::nullopt},
      {&sm2::type_text, -1, false, std::nullopt}};
  return s;
}

/* Open a rel over db's store; with toast, one that moves datums out. */
sm2::rel_t *open_rel(db_t &db, sm1::session_t *s, bool toast = true) {
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 0, schema(), {}, "rowno",
                        toast ? "toast" : "", rel) == 0);
  return rel;
}

/* A text of n bytes that no other n gives. */
std::string text_of(size_t n, char c = 'x') {
  std::string s(n, c);
  for (size_t i = 0; i < n; i += 97) {
    s[i] = static_cast<char>('a' + (i / 97) % 26);
  }
  return s;
}

/*
 * The datums of a row, for the calls that take them. a and b are
 * given as varlenas made here, or as they are when raw: a TOAST
 * pointer read from a tuple.
 */
struct datums_t {
  std::string a, b;
  sm2::datum_t d[3];
  bool null[3];

  datums_t(const row_t &r, bool raw_a = false, bool raw_b = false) {
    a = r.a ? (raw_a ? *r.a : sm2::make_varlena(*r.a)) : "";
    b = r.b ? (raw_b ? *r.b : sm2::make_varlena(*r.b)) : "";
    d[0] = sm2::datum_from_int32(r.k);
    d[1] = sm2::datum_from_ptr(a.data());
    d[2] = sm2::datum_from_ptr(b.data());
    null[0] = false;
    null[1] = !r.a;
    null[2] = !r.b;
  }
};

/* Insert r through rel; its rowno. */
uint64_t insert(sm2::rel_t *rel, const row_t &r) {
  datums_t v(r);
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(sm2::rel_insert(rel, v.d, v.null, rowno) == 0, sm1::errmsg());
  return rowno;
}

/*
 * Attribute ordinal of tuple as the tuple holds it: a varlena, a
 * TOAST pointer among them, or nullopt for a null.
 */
std::optional<std::string> held(std::string_view tuple, size_t ordinal) {
  sm2::datum_t val;
  bool isnull;
  REQUIRE_MESSAGE(sm2::tup_decode(schema(), tuple, ordinal, val, isnull) == 0,
                  sm1::errmsg());
  if (isnull) {
    return std::nullopt;
  }
  std::string_view v;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, val, v) == 0);
  return std::string(v);
}

/* The tuple of the row at rowno, copied. */
std::string read(sm2::rel_t *rel, uint64_t rowno) {
  std::string_view tuple;
  REQUIRE_MESSAGE(sm2::rel_read(rel, sm2::id_key(rowno), tuple) == 0,
                  sm1::errmsg());
  return std::string(tuple);
}

/* The text attribute ordinal of tuple holds, fetched if out of line. */
std::string value(sm2::rel_t *rel, std::string_view tuple, size_t ordinal) {
  std::string v = *held(tuple, ordinal);
  if (!sm2::varlena_is_toast(v)) {
    return std::string(sm2::varlena_payload(v));
  }
  std::string out;
  REQUIRE_MESSAGE(sm2::toast_fetch(rel, v, out) == 0, sm1::errmsg());
  return out;
}

/* How many TOAST chunks session s sees in db's store. */
size_t chunks(db_t &db, sm1::session_t *s) {
  std::string lo = "\xff\xff", hi(14, '\xff');
  sm1::scan_t *scan = nullptr;
  REQUIRE(sm1::scan(s, db.store(), lo.data(), lo.size(), 1, hi.data(),
                    hi.size(), 1, scan) == 0);
  size_t n = 0;
  std::string_view key, val;
  while (sm1::scannxt(scan, key, val) == 0) {
    n++;
  }
  sm1::scanend(scan);
  return n;
}

} // namespace

TEST_CASE("a TOAST pointer is a varlena the codec stores as it is") {
  // Verify the pointer's form, with no database.
  // 1. A pointer is toast_ptr_len bytes, begins with varlena_toast
  //    and toast_tag, and parses back to what made it.
  // 2. A varlena that is not one, a pointer cut short, and one with
  //    another tag do not parse.
  // 3. tup_encode stores a pointer unchanged, and tup_decode gives
  //    it back; tup_check accepts the tuple.
  // 4. A pointer with another tag is refused by tup_encode.
  sm2::toast_ptr_t p{20000, 7, 42}, q{};
  std::string v = sm2::make_toast_ptr(p);
  CHECK(v.size() == sm2::toast_ptr_len);
  CHECK(v[0] == sm2::varlena_toast);
  CHECK(static_cast<uint8_t>(v[1]) == sm2::toast_tag);
  REQUIRE(sm2::parse_toast_ptr(v, q));
  CHECK(q.len == 20000);
  CHECK(q.storeid == 7);
  CHECK(q.id == 42);

  CHECK_FALSE(sm2::parse_toast_ptr(sm2::make_varlena("abc"), q));
  CHECK_FALSE(sm2::parse_toast_ptr(v.substr(0, 10), q));
  std::string other = v;
  other[1] = 18;
  CHECK_FALSE(sm2::parse_toast_ptr(other, q));

  datums_t d({1, v, "short"}, true);
  std::string tuple;
  REQUIRE_MESSAGE(sm2::tup_encode(schema(), d.d, d.null, tuple) == 0,
                  sm1::errmsg());
  CHECK(*held(tuple, 1) == v);
  CHECK(*held(tuple, 2) == sm2::make_varlena("short"));
  CHECK(sm2::tup_check(schema(), tuple) == 0);

  datums_t bad({1, other, std::nullopt}, true);
  CHECK(sm2::tup_encode(schema(), bad.d, bad.null, tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a long datum is moved out of line, and read back") {
  // Verify a row's long datum is stored as chunks and a pointer.
  // 1. Insert a row with a 20000-byte text and a short one.
  // 2. The tuple is under toast_threshold; the long attribute is a
  //    pointer of that length and of the store, the short one is
  //    as it was.
  // 3. The datum is three chunks, and toast_fetch gives its bytes.
  // 4. After commit, another session reads the same.
  db_t db;
  sm1::session_t *s = db.open(), *t = db.open();
  sm2::rel_t *rel = open_rel(db, s), *other = open_rel(db, t);
  std::string big = text_of(20000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, big, "short"});
  std::string tuple = read(rel, rowno);
  CHECK(tuple.size() <= sm2::toast_threshold);
  sm2::toast_ptr_t p{};
  REQUIRE(sm2::parse_toast_ptr(*held(tuple, 1), p));
  CHECK(p.len == 20000);
  CHECK(p.storeid == db.store());
  CHECK(*held(tuple, 2) == sm2::make_varlena("short"));
  CHECK(chunks(db, s) == 3);
  CHECK(value(rel, tuple, 1) == big);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  tuple = read(other, rowno);
  CHECK(value(other, tuple, 1) == big);
  CHECK(value(other, tuple, 2) == "short");
  REQUIRE(sm1::commit(t) == 0);
  sm2::rel_close(rel);
  sm2::rel_close(other);
}

TEST_CASE("a pointer is read by its store alone, with no rel") {
  // Verify a TOAST pointer names enough to read its datum without a rel.
  // 1. Insert a row with a 20000-byte text, and commit.
  // 2. Another session, with no rel open, reads the datum by the
  //    pointer alone.
  // 3. A varlena that is no pointer is refused as invalid.
  db_t db;
  sm1::session_t *s = db.open(), *t = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  std::string big = text_of(20000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, big, std::nullopt});
  std::string ptr = *held(read(rel, rowno), 1);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  std::string out;
  REQUIRE_MESSAGE(sm2::toast_fetch(t, ptr, out) == 0, sm1::errmsg());
  CHECK(out == big);
  CHECK(sm2::toast_fetch(t, sm2::make_varlena("short"), out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::commit(t) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("datums move out longest first, and only while the tuple is long") {
  // Verify which datums a long row moves out.
  // 1. A row of 1500 and 900 bytes is over the threshold: the
  //    longer moves out, which is enough, and the shorter stays.
  // 2. A row of 900 and 800 bytes is under it: nothing moves.
  // 3. A row of 3000 and 2500 bytes: both move.
  // 4. A row of 60000 bytes and toast_min bytes: the long one
  //    moves, which is enough, and the other stays.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);

  std::string tuple = read(rel, insert(rel, {1, text_of(900), text_of(1500)}));
  CHECK_FALSE(sm2::varlena_is_toast(*held(tuple, 1)));
  CHECK(sm2::varlena_is_toast(*held(tuple, 2)));

  tuple = read(rel, insert(rel, {2, text_of(900), text_of(800)}));
  CHECK_FALSE(sm2::varlena_is_toast(*held(tuple, 1)));
  CHECK_FALSE(sm2::varlena_is_toast(*held(tuple, 2)));

  tuple = read(rel, insert(rel, {3, text_of(3000), text_of(2500)}));
  CHECK(sm2::varlena_is_toast(*held(tuple, 1)));
  CHECK(sm2::varlena_is_toast(*held(tuple, 2)));
  CHECK(value(rel, tuple, 1) == text_of(3000));
  CHECK(value(rel, tuple, 2) == text_of(2500));

  // 60000 bytes is over the threshold whatever the other holds.
  tuple = read(rel, insert(rel, {4, text_of(60000), text_of(sm2::toast_min)}));
  CHECK(sm2::varlena_is_toast(*held(tuple, 1)));
  CHECK_FALSE(sm2::varlena_is_toast(*held(tuple, 2)));
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a datum shorter than toast_min is never moved out") {
  // Verify the least a datum must be to move, on a table wide
  // enough to pass the threshold with short datums alone.
  // 1. A table of forty texts. A row of forty datums a byte short
  //    of toast_min is over the threshold, and none moves: the
  //    tuple stays long, and no chunk is written.
  // 2. A row of forty datums of toast_min bytes: some move, enough
  //    to bring the tuple under the threshold, and each reads back.
  db_t db;
  sm1::session_t *s = db.open();
  std::vector<sm2::attribute_t> wide(
      40, {&sm2::type_text, -1, false, std::nullopt});
  sm2::rel_t *rel = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 0, wide, {}, "rowno", "toast", rel) ==
          0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  for (size_t n : {sm2::toast_min - 1, sm2::toast_min}) {
    std::vector<std::string> v(40, sm2::make_varlena(text_of(n)));
    std::vector<sm2::datum_t> d;
    for (const std::string &x : v) {
      d.push_back(sm2::datum_from_ptr(x.data()));
    }
    bool null[40] = {};
    uint64_t rowno = 0;
    REQUIRE_MESSAGE(sm2::rel_insert(rel, d, null, rowno) == 0, sm1::errmsg());
    std::string tuple = read(rel, rowno);
    size_t moved = 0;
    for (size_t i = 0; i < 40; i++) {
      sm2::datum_t val;
      bool isnull;
      REQUIRE(sm2::tup_decode(wide, tuple, i, val, isnull) == 0);
      std::string_view x;
      REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, val, x) == 0);
      if (!sm2::varlena_is_toast(x)) {
        CHECK(sm2::varlena_payload(x) == text_of(n));
        continue;
      }
      moved++;
      std::string out;
      REQUIRE(sm2::toast_fetch(rel, x, out) == 0);
      CHECK(out == text_of(n));
    }
    if (n < sm2::toast_min) {
      CHECK(moved == 0);
      CHECK(tuple.size() > sm2::toast_threshold);
      CHECK(chunks(db, s) == 0);
    } else {
      CHECK(moved > 0);
      CHECK(moved < 40);
      CHECK(tuple.size() <= sm2::toast_threshold);
      CHECK(chunks(db, s) == moved);
    }
  }
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a row past 64 KB is stored with TOAST, and refused without") {
  // Verify TOAST lifts the tuple's length limit.
  // 1. A rel with no toast sequence refuses a 100000-byte datum
  //    with SM_ERR_ROWTOOLONG, and writes no chunk.
  // 2. A rel with one stores it, and reads it back.
  // 3. The same row, already encoded short of the datum, goes in by
  //    rel_insert's tuple form and is moved out as well.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *plain = open_rel(db, s, false), *rel = open_rel(db, s);
  std::string big = text_of(100000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  datums_t v({1, big, std::nullopt});
  uint64_t rowno = 0;
  CHECK(sm2::rel_insert(plain, v.d, v.null, rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_ROWTOOLONG);
  CHECK(chunks(db, s) == 0);

  rowno = insert(rel, {1, big, std::nullopt});
  CHECK(value(rel, read(rel, rowno), 1) == big);

  datums_t w({2, text_of(30000), std::nullopt});
  std::string tuple;
  REQUIRE(sm2::tup_encode(schema(), w.d, w.null, tuple) == 0);
  REQUIRE_MESSAGE(sm2::rel_insert(rel, tuple, rowno) == 0, sm1::errmsg());
  tuple = read(rel, rowno);
  CHECK(sm2::varlena_is_toast(*held(tuple, 1)));
  CHECK(value(rel, tuple, 1) == text_of(30000));
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(plain);
  sm2::rel_close(rel);
}

TEST_CASE("an update keeps a datum it is given back, and erases one it "
          "replaces") {
  // Verify what rel_update does with a row's datums out of line.
  // 1. Insert a row whose a is 20000 bytes: three chunks.
  // 2. Update b alone, giving a's pointer back: the pointer is the
  //    same, and no chunk is written or erased.
  // 3. Update a to 10000 other bytes: the old tuple handed back
  //    still holds the old pointer, the new value reads back, and
  //    the store holds the new datum's two chunks alone.
  // 4. Update a to a short text: no chunk is left.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, text_of(20000), "b1"});
  std::string ptr = *held(read(rel, rowno), 1);
  REQUIRE(sm2::varlena_is_toast(ptr));
  CHECK(chunks(db, s) == 3);

  datums_t keep({1, ptr, "b2"}, true);
  REQUIRE_MESSAGE(
      sm2::rel_update(rel, sm2::id_key(rowno), rowno, keep.d, keep.null) == 0,
      sm1::errmsg());
  std::string tuple = read(rel, rowno);
  CHECK(*held(tuple, 1) == ptr);
  CHECK(value(rel, tuple, 2) == "b2");
  CHECK(chunks(db, s) == 3);

  datums_t change({1, text_of(10000, 'y'), "b2"});
  std::string_view old;
  rawpin_ptr_t pin;
  REQUIRE_MESSAGE(0 == sm2::rel_update(rel, sm2::id_key(rowno), rowno, change.d,
                                       change.null, pin.get(), old),
                  sm1::errmsg());
  CHECK(*held(old, 1) == ptr);
  tuple = read(rel, rowno);
  CHECK(*held(tuple, 1) != ptr);
  CHECK(value(rel, tuple, 1) == text_of(10000, 'y'));
  CHECK(chunks(db, s) == 2);

  datums_t shrink({1, "small", "b2"});
  REQUIRE(sm2::rel_update(rel, sm2::id_key(rowno), rowno, shrink.d,
                          shrink.null) == 0);
  CHECK(value(rel, read(rel, rowno), 1) == "small");
  CHECK(chunks(db, s) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a delete erases the row's datums, and hands back its tuple") {
  // Verify rel_delete takes a row's chunks with it.
  // 1. Insert a row with two long datums: five chunks.
  // 2. Delete it: the tuple handed back holds both pointers, the
  //    row is gone, and no chunk is left.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, text_of(20000), text_of(10000)});
  CHECK(chunks(db, s) == 5);
  std::string_view old;
  rawpin_ptr_t pin;
  REQUIRE_MESSAGE(0 == sm2::rel_delete(rel, sm2::id_key(rowno), pin.get(), old),
                  sm1::errmsg());
  CHECK(sm2::varlena_is_toast(*held(old, 1)));
  CHECK(sm2::varlena_is_toast(*held(old, 2)));
  std::string_view tuple;
  CHECK(sm2::rel_read(rel, sm2::id_key(rowno), tuple) == 1);
  CHECK(chunks(db, s) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a write with hold keeps the old datums until rel_release") {
  // Verify the old row's values can be fetched after the write,
  // as a caller with an index entry to remove needs.
  // 1. Insert a row with a long a. Update a with hold: the old
  //    pointer still fetches the first value, and the store holds
  //    both datums' chunks.
  // 2. rel_release: the first datum is gone, the second is there.
  // 3. Delete the row with hold: its datum still fetches; after
  //    rel_release no chunk is left, and a second release is no
  //    error.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  std::string first = text_of(20000), second = text_of(10000, 'y');
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, first, std::nullopt});
  datums_t change({1, second, std::nullopt});
  std::string_view old;
  rawpin_ptr_t pin;
  REQUIRE(0 == sm2::rel_update(rel, sm2::id_key(rowno), rowno, change.d,
                               change.null, pin.get(), old, true));
  std::string ptr = *held(old, 1), out;
  REQUIRE_MESSAGE(sm2::toast_fetch(rel, ptr, out) == 0, sm1::errmsg());
  CHECK(out == first);
  CHECK(chunks(db, s) == 5);
  REQUIRE(sm2::rel_release(rel) == 0);
  CHECK(chunks(db, s) == 2);
  CHECK(sm2::toast_fetch(rel, ptr, out) == -1);

  REQUIRE(0 == sm2::rel_delete(rel, sm2::id_key(rowno), pin.get(), old, true));
  ptr = *held(old, 1);
  REQUIRE(sm2::toast_fetch(rel, ptr, out) == 0);
  CHECK(out == second);
  REQUIRE(sm2::rel_release(rel) == 0);
  CHECK(chunks(db, s) == 0);
  CHECK(sm2::rel_release(rel) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("two rows never share a datum") {
  // Verify a pointer given to a write is copied, or refused.
  // 1. Insert row one with a long a; insert row two giving row
  //    one's pointer for a: row two's pointer is another, and both
  //    read the same bytes.
  // 2. Delete row one: row two still reads its datum.
  // 3. An update of row two giving a pointer it does not hold --
  //    one made up -- fails with SM_ERR_INVALID.
  // 4. An insert giving a pointer of another store fails with
  //    SM_ERR_INVALID.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  std::string big = text_of(20000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t one = insert(rel, {1, big, std::nullopt});
  std::string ptr = *held(read(rel, one), 1);
  datums_t copy({2, ptr, std::nullopt}, true);
  uint64_t two = 0;
  REQUIRE_MESSAGE(sm2::rel_insert(rel, copy.d, copy.null, two) == 0,
                  sm1::errmsg());
  std::string ptr2 = *held(read(rel, two), 1);
  REQUIRE(sm2::varlena_is_toast(ptr2));
  CHECK(ptr2 != ptr);
  CHECK(chunks(db, s) == 6);

  REQUIRE(sm2::rel_delete(rel, sm2::id_key(one)) == 0);
  CHECK(chunks(db, s) == 3);
  CHECK(value(rel, read(rel, two), 1) == big);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  datums_t stale(
      {2, sm2::make_toast_ptr({20000, db.store(), 999999}), std::nullopt},
      true);
  CHECK(sm2::rel_update(rel, sm2::id_key(two), two, stale.d, stale.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::abort(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  datums_t foreign(
      {3, sm2::make_toast_ptr({20000, db.store() + 1, 1}), std::nullopt}, true);
  uint64_t rowno = 0;
  CHECK(sm2::rel_insert(rel, foreign.d, foreign.null, rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(value(rel, read(rel, two), 1) == big);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a datum out of line follows its transaction") {
  // Verify the chunks are written in the row's transaction.
  // 1. Insert a long row and abort: no chunk is left.
  // 2. Insert one, take a savepoint, replace its datum, and roll
  //    back to the savepoint: the first datum reads back, and its
  //    chunks alone are there.
  // 3. An uncommitted row's chunks are not seen by another session.
  db_t db;
  sm1::session_t *s = db.open(), *t = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  insert(rel, {1, text_of(20000), std::nullopt});
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  CHECK(chunks(db, s) == 0);

  uint64_t rowno = insert(rel, {1, text_of(20000), std::nullopt});
  int sp = sm1::savepoint(s);
  REQUIRE(sp >= 0);
  datums_t change({1, text_of(50000, 'z'), std::nullopt});
  REQUIRE(sm2::rel_update(rel, sm2::id_key(rowno), rowno, change.d,
                          change.null) == 0);
  CHECK(chunks(db, s) == 7);
  REQUIRE(sm1::rollback(s, sp) == 0);
  CHECK(chunks(db, s) == 3);
  CHECK(value(rel, read(rel, rowno), 1) == text_of(20000));

  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  CHECK(chunks(db, t) == 0);
  REQUIRE(sm1::commit(t) == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(rel);
}

TEST_CASE("a snapshot reads the datum it saw, and a later one finds it gone") {
  // Verify a pointer names bytes that never change, only go.
  // 1. A row with a long a is committed. Session A, at REPEATABLE
  //    READ, reads its pointer.
  // 2. Session B replaces a and commits.
  // 3. A still fetches the first bytes through its pointer.
  // 4. A's next transaction fetches the same pointer: the datum is
  //    not there, SM_ERR_NOTFOUND.
  db_t db;
  sm1::session_t *a = db.open(), *b = db.open();
  sm2::rel_t *ra = open_rel(db, a), *rb = open_rel(db, b);
  std::string first = text_of(20000), second = text_of(20000, 'q');
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rb, {1, first, std::nullopt});
  REQUIRE(sm1::commit(b) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  std::string ptr = *held(read(ra, rowno), 1);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  datums_t change({1, second, std::nullopt});
  REQUIRE(sm2::rel_update(rb, sm2::id_key(rowno), rowno, change.d,
                          change.null) == 0);
  REQUIRE(sm1::commit(b) == 0);

  std::string out;
  REQUIRE_MESSAGE(sm2::toast_fetch(ra, ptr, out) == 0, sm1::errmsg());
  CHECK(out == first);
  REQUIRE(sm1::commit(a) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(sm2::toast_fetch(ra, ptr, out) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(value(ra, read(ra, rowno), 1) == second);
  REQUIRE(sm1::commit(a) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}

TEST_CASE("an update sm1 refuses writes no chunk") {
  // Verify the row is written before its chunks (SM2_DESIGN.md
  // §4.2), so SM_ERR_UPDATED leaves nothing to roll back.
  // 1. A short row is committed. Session A begins at READ COMMITTED
  //    and reads it; session B updates it and commits.
  // 2. A's update to a long datum fails with SM_ERR_UPDATED, and A
  //    sees no chunk.
  // 3. A reads the newest version and updates again: the datum's
  //    chunks are there, and it commits.
  db_t db;
  sm1::session_t *a = db.open(), *b = db.open();
  sm2::rel_t *ra = open_rel(db, a), *rb = open_rel(db, b);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rb, {1, "v1", std::nullopt});
  REQUIRE(sm1::commit(b) == 0);

  REQUIRE(sm1::begin(a, sm1::xctlevel_t::read_committed) == 0);
  read(ra, rowno);
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::read_committed) == 0);
  datums_t v2({1, "v2", std::nullopt});
  REQUIRE(sm2::rel_update(rb, sm2::id_key(rowno), rowno, v2.d, v2.null) == 0);
  REQUIRE(sm1::commit(b) == 0);

  datums_t big({1, text_of(20000), std::nullopt});
  CHECK(sm2::rel_update(ra, sm2::id_key(rowno), rowno, big.d, big.null) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(chunks(db, a) == 0);
  std::string_view tuple;
  REQUIRE(sm2::rel_read_latest(ra, sm2::id_key(rowno), tuple) == 0);
  REQUIRE_MESSAGE(
      sm2::rel_update(ra, sm2::id_key(rowno), rowno, big.d, big.null) == 0,
      sm1::errmsg());
  CHECK(chunks(db, a) == 3);
  REQUIRE(sm1::commit(a) == 0);
  sm2::rel_close(ra);
  sm2::rel_close(rb);
}

TEST_CASE("a key is never made of a TOAST pointer") {
  // Verify encode_key refuses a pointer rather than key its bytes.
  // 1. Encode a key of one text column from a TOAST pointer.
  // 2. It fails with SM_ERR_INVALID, and the key is left empty.
  std::string ptr = sm2::make_toast_ptr({20000, 1, 1});
  sm2::key_column_t col{&sm2::type_text, -1, nullptr, false, false};
  sm2::datum_t d[1] = {sm2::datum_from_ptr(ptr.data())};
  bool null[1] = {false};
  std::string key;
  CHECK(sm2::encode_key(std::span<const sm2::key_column_t>(&col, 1), d, null,
                        key) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(key.empty());
}

TEST_CASE("a primary key given out of line is keyed by its value") {
  // Verify a key column's TOAST pointer is fetched, then keyed.
  // 1. A keyless rel takes a row whose texts are 1000 and 990
  //    bytes: the longer goes out of line, and its pointer is read.
  // 2. A rel keyed by that text column keys the pointer as it keys
  //    the value itself.
  // 3. A null key column is refused with SM_ERR_INVALID.
  db_t db;
  sm1::session_t *s = db.open();
  sm2::rel_t *rel = open_rel(db, s);
  std::string big = text_of(1000);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t rowno = insert(rel, {1, big, text_of(990, 'y')});
  std::string ptr = *held(read(rel, rowno), 1);
  REQUIRE(sm2::varlena_is_toast(ptr));

  std::vector<sm2::key_column_t> key = {
      {&sm2::type_text, -1, nullptr, false, false}};
  std::vector<int32_t> atts = {1};
  sm2::rel_t *keyed = nullptr;
  REQUIRE(sm2::rel_open(s, db.store(), 1, schema(), {key, atts}, "rowno",
                        "toast", keyed) == 0);
  std::string inline_big = sm2::make_varlena(big);
  sm2::datum_t by_ptr[1] = {sm2::datum_from_ptr(ptr.data())};
  sm2::datum_t by_value[1] = {sm2::datum_from_ptr(inline_big.data())};
  bool isnull[1] = {false};
  std::string from_ptr, from_value;
  REQUIRE_MESSAGE(sm2::rel_key(keyed, by_ptr, isnull, from_ptr) == 0,
                  sm1::errmsg());
  REQUIRE(sm2::rel_key(keyed, by_value, isnull, from_value) == 0);
  CHECK(!from_value.empty());
  CHECK(from_ptr == from_value);

  isnull[0] = true;
  CHECK(sm2::rel_key(keyed, by_value, isnull, from_value) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::commit(s) == 0);
  sm2::rel_close(keyed);
  sm2::rel_close(rel);
}
