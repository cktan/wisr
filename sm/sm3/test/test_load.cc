/*
 * test_load.cc -- a bulk load into a table (BULKLOAD.md §5, §13):
 * tbl_load_open, tbl_load_add and tbl_load_close, into an empty
 * table from files and into any other row by row.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <optional>
#include <string>
#include <vector>

namespace {

/* Options under which every load is written as files, not a batch. */
sm1::options_t files_options() {
  sm1::options_t opt;
  opt.load_small_bytes = 1;
  return opt;
}

/* A row of items as the datums a write takes. */
struct row_t {
  /* The row (id, name, note); a nullopt name is NULL. */
  row_t(int32_t id, std::optional<std::string> name, const std::string &note)
      : m_name(sm2::make_varlena(name.value_or(""))),
        m_note(sm2::make_varlena(note)) {
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_name.data());
    m_d[2] = sm2::datum_from_ptr(m_note.data());
    m_null[1] = !name;
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_name, m_note;
  sm2::datum_t m_d[3];
  bool m_null[3] = {false, false, false};
};

/*
 * A catalog with items (id int4, name text NOT NULL, note text),
 * its indexes items_id, unique, on (id) and items_name on (name)
 * INCLUDE (note), and sessions a and b over it. With toast, the
 * catalog's host reads TOAST pointers.
 */
class items_t : public db_t {
public:
  /* Initialize and load the catalog, create items, open a and b. */
  explicit items_t(const sm1::options_t &opt = sm1::options_t(),
                   bool toast = false)
      : db_t(opt) {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::cat_set_toast(cat, toast) == 0);
    REQUIRE(sm3::session_open(cat, open(), a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"name", sm2::type_text.code, -1, true},
        {"note", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_id = {
        {0, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_name = {
        {1, false, false, std::nullopt}};
    std::vector<int32_t> note = {2};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "items", cols, items) == 0);
    REQUIRE(sm3::create_index(a, items, "items_id", by_id, {}, true,
                              items_id) == 0);
    REQUIRE(sm3::create_index(a, items, "items_name", by_name, note, false,
                              items_name) == 0);
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~items_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }

  /* items, opened in mode in s's transaction; it must open. */
  sm3::table_t *open_items(sm3::session_t *s,
                           sm1::lockmode_t mode = sm1::SM_LOCKMODE_IX) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, items, mode, t) == 0, sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t items = 0, items_id = 0, items_name = 0;
};

/* Begin a READ COMMITTED transaction on s. */
void start(sm3::session_t *s) {
  REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
}

/* Open a load into t; it must open. */
sm3::loader_t *open_load(sm3::table_t *t) {
  sm3::loader_t *l = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_load_open(t, l) == 0, sm1::errmsg());
  return l;
}

/* Hand the rows with ids first..last to l; their rownos. */
std::vector<uint64_t> add_rows(sm3::loader_t *l, int first, int last) {
  std::vector<uint64_t> out;
  for (int i = first; i <= last; i++) {
    row_t r(i, "name-" + std::to_string(i % 10), "note-" + std::to_string(i));
    uint64_t rowno = 0;
    REQUIRE_MESSAGE(sm3::tbl_load_add(l, r.d(), r.null(), rowno) == 0,
                    sm1::errmsg());
    out.push_back(rowno);
  }
  return out;
}

/* How many rows t's transaction sees in its heap. */
size_t count(sm3::table_t *t) {
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
  size_t n = 0;
  uint64_t rowno;
  std::string_view tuple;
  while (sm3::tbl_scannxt(scan, rowno, tuple) == 0) {
    n++;
  }
  sm3::tbl_scanend(scan);
  return n;
}

/* The rownos index oid of t has under the int4 key id. */
std::vector<uint64_t> by_id(sm3::table_t *t, uint64_t oid, int32_t id) {
  sm2::datum_t d[1] = {sm2::datum_from_int32(id)};
  bool null[1] = {false};
  sm2::bound_t eq{d, null, true};
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, oid, &eq, &eq, scan) == 0,
                  sm1::errmsg());
  std::vector<uint64_t> out;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
    out.push_back(rowno);
  }
  sm2::idx_scanend(scan);
  return out;
}

/* How many entries index oid of t has under the text key name. */
size_t by_name(sm3::table_t *t, uint64_t oid, const std::string &name) {
  std::string v = sm2::make_varlena(name);
  sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
  bool null[1] = {false};
  sm2::bound_t eq{d, null, true};
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, oid, &eq, &eq, scan) == 0,
                  sm1::errmsg());
  size_t n = 0;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
    n++;
  }
  sm2::idx_scanend(scan);
  return n;
}

/* Whether another session is kept from reading items, right now. */
bool readers_wait(items_t &f, sm3::session_t *s) {
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(s), true) == 0);
  start(s);
  sm3::table_t *t = nullptr;
  int rc = sm3::tbl_open(s, f.items, sm1::SM_LOCKMODE_IS, t);
  bool blocked = rc != 0 && sm1::errnum() == sm1::SM_ERR_BLOCKED;
  if (rc == 0) {
    sm3::tbl_close(t);
  }
  sm3::abort(s);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(s), false) == 0);
  return blocked;
}

} // namespace

TEST_CASE("a load into an empty table fills its heap and its indexes") {
  // Verify a load, as one batch and as files.
  // 1. Open a load into items, which is empty: readers of items
  //    now wait.
  // 2. Hand over 300 rows: each gets a rowno, ascending; the
  //    transaction's own scan sees none yet.
  // 3. Close: the transaction sees 300 rows, finds one by its id
  //    under the rowno it was given, and thirty under one name.
  // 4. Commit: readers pass, and another session sees the same.
  for (bool files : {false, true}) {
    CAPTURE(files);
    items_t f(files ? files_options() : sm1::options_t());
    start(f.a);
    sm3::table_t *t = f.open_items(f.a);
    sm3::loader_t *l = open_load(t);
    CHECK(readers_wait(f, f.b));
    std::vector<uint64_t> rownos = add_rows(l, 1, 300);
    for (size_t i = 1; i < rownos.size(); i++) {
      CHECK(rownos[i] > rownos[i - 1]);
    }
    CHECK(count(t) == 0);
    REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
    CHECK(count(t) == 300);
    CHECK(by_id(t, f.items_id, 42) == std::vector<uint64_t>{rownos[41]});
    CHECK(by_name(t, f.items_name, "name-7") == 30);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    CHECK_FALSE(readers_wait(f, f.b));
    start(f.b);
    t = f.open_items(f.b, sm1::SM_LOCKMODE_IS);
    CHECK(count(t) == 300);
    CHECK(by_id(t, f.items_id, 300) == std::vector<uint64_t>{rownos[299]});
    CHECK(by_name(t, f.items_name, "name-0") == 30);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("a load checks each row as an insert does, and unique keys at its "
          "close") {
  // Verify what a load refuses, and when.
  // 1. A row with NULL in a NOT NULL column fails its add with
  //    SM_ERR_NOTNULL.
  // 2. Two rows with one id are both taken, and the close fails
  //    with SM_ERR_DUPKEY, as a batch and as files.
  // 3. After the abort the table is empty, and loads again.
  for (bool files : {false, true}) {
    CAPTURE(files);
    items_t f(files ? files_options() : sm1::options_t());
    start(f.a);
    sm3::table_t *t = f.open_items(f.a);
    sm3::loader_t *l = open_load(t);
    row_t bad(1, std::nullopt, "n");
    uint64_t rowno = 0;
    CHECK(sm3::tbl_load_add(l, bad.d(), bad.null(), rowno) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
    add_rows(l, 1, 20);
    add_rows(l, 7, 7);
    CHECK(sm3::tbl_load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);

    start(f.a);
    t = f.open_items(f.a);
    CHECK(count(t) == 0);
    l = open_load(t);
    add_rows(l, 1, 20);
    REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
    CHECK(count(t) == 20);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("a load into a table with rows inserts them, and keeps no one out") {
  // Verify the other way a load goes.
  // 1. items has ten rows, committed.
  // 2. A load opened on it keeps no reader out, and each row it
  //    is handed is in the table at once.
  // 3. A row whose id is taken fails its add with SM_ERR_DUPKEY.
  items_t f(files_options());
  start(f.a);
  sm3::table_t *t = f.open_items(f.a);
  sm3::loader_t *l = open_load(t);
  add_rows(l, 1, 10);
  REQUIRE(sm3::tbl_load_close(l) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.open_items(f.a);
  l = open_load(t);
  CHECK_FALSE(readers_wait(f, f.b));
  add_rows(l, 11, 15);
  CHECK(count(t) == 15);
  CHECK(by_id(t, f.items_id, 13).size() == 1);
  row_t dup(3, "x", "y");
  uint64_t rowno = 0;
  CHECK(sm3::tbl_load_add(l, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::tbl_load_free(l);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("a row to be stored out of line ends the files, and the rest are "
          "inserted") {
  // Verify what a load does with a row it does not take.
  // 1. A catalog whose host reads TOAST pointers. Hand a load 50
  //    rows, then one with a 5000-byte note, then 50 more.
  // 2. At the long row the first 50 are in the table, and the long
  //    row is too, its note out of line.
  // 3. The rows after it are in the table as they are handed over,
  //    and one whose id is taken fails at once with SM_ERR_DUPKEY.
  // 4. Every row is found by its id after the close.
  items_t f(files_options(), true);
  start(f.a);
  sm3::table_t *t = f.open_items(f.a);
  sm3::loader_t *l = open_load(t);
  add_rows(l, 1, 50);
  CHECK(count(t) == 0);
  std::string note(5000, 'n');
  row_t wide(51, "wide", note);
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(sm3::tbl_load_add(l, wide.d(), wide.null(), rowno) == 0,
                  sm1::errmsg());
  CHECK(count(t) == 51);
  std::string_view tuple;
  REQUIRE(sm3::tbl_read(t, rowno, tuple) == 0);
  sm2::datum_t val;
  bool isnull;
  REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, 2, val, isnull) == 0);
  std::string_view held;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, val, held) == 0);
  CHECK(sm2::varlena_is_toast(held));

  add_rows(l, 52, 60);
  CHECK(count(t) == 60);
  row_t dup(7, "x", "y");
  CHECK(sm3::tbl_load_add(l, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::tbl_load_free(l);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  start(f.a);
  t = f.open_items(f.a);
  l = open_load(t);
  add_rows(l, 1, 50);
  REQUIRE(sm3::tbl_load_add(l, wide.d(), wide.null(), rowno) == 0);
  add_rows(l, 52, 60);
  REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
  for (int id : {1, 50, 51, 52, 60}) {
    CAPTURE(id);
    CHECK(by_id(t, f.items_id, id).size() == 1);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a load into an empty table builds its unique tree whole") {
  // Verify a unique B-tree Index no longer makes a load insert its rows:
  // the tree is built from the load's sorted entries at its close.
  // 1. Make two tables, each with a unique tree on name, each in the
  //    transaction that makes its table, so the tree is valid at once.
  // 2. A load into it, empty, keeps readers out, as one from files.
  // 3. After the close and the commit, the tree finds each row by name.
  // 4. A load of a name twice into it, empty again, fails its close
  //    with SM_ERR_DUPKEY, the tree in errdup(); after the abort the
  //    table and its tree are empty.
  // 5. Into the table with rows, a load inserts: a name there fails
  //    its add with SM_ERR_DUPKEY.
  for (bool files : {false, true}) {
    CAPTURE(files);
    items_t f(files ? files_options() : sm1::options_t());
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"name", sm2::type_text.code, -1, true},
        {"note", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> key = {{1, false, false, std::nullopt}};
    uint64_t table = 0, tree = 0, other = 0, tree2 = 0;
    start(f.a);
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "uitems", cols, table) ==
            0);
    REQUIRE_MESSAGE(sm3::create_index(f.a, table, "uitems_name", key, {}, true,
                                      tree, false, false, true) == 0,
                    sm1::errmsg());
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "other", cols, other) == 0);
    REQUIRE_MESSAGE(sm3::create_index(f.a, other, "other_name", key, {}, true,
                                      tree2, false, false, true) == 0,
                    sm1::errmsg());
    REQUIRE(sm3::commit(f.a) == 0);

    auto open = [&](sm3::session_t *s, uint64_t oid) {
      sm3::table_t *t = nullptr;
      REQUIRE_MESSAGE(sm3::tbl_open(s, oid, sm1::SM_LOCKMODE_IX, t) == 0,
                      sm1::errmsg());
      return t;
    };
    start(f.a);
    sm3::table_t *t = open(f.a, table);
    sm3::loader_t *l = open_load(t);
    REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
    start(f.b);
    sm3::table_t *seen = nullptr;
    CHECK(sm3::tbl_open(f.b, table, sm1::SM_LOCKMODE_IS, seen) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
    REQUIRE(sm3::abort(f.b) == 0);
    add_rows(l, 1, 10);
    REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    start(f.a);
    t = open(f.a, table);
    CHECK(count(t) == 10);
    for (int32_t i = 1; i <= 10; i++) {
      CHECK(by_name(t, tree, "name-" + std::to_string(i % 10)) == 1);
    }
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    // The same, into a second table of the same columns and its own
    // unique tree, with a name twice.
    start(f.a);
    t = open(f.a, other);
    l = open_load(t);
    add_rows(l, 1, 12);
    CHECK(sm3::tbl_load_close(l) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    CHECK(sm1::errdup().index == tree2);
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);
    start(f.a);
    t = open(f.a, other);
    CHECK(count(t) == 0);
    for (int32_t i = 0; i < 10; i++) {
      CHECK(by_name(t, tree2, "name-" + std::to_string(i)) == 0);
    }
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.a);
    t = open(f.a, table);
    l = open_load(t);
    row_t dup(99, "name-3", "y");
    uint64_t rowno = 0;
    CHECK(sm3::tbl_load_add(l, dup.d(), dup.null(), rowno) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    sm3::tbl_load_free(l);
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);
  }
}

TEST_CASE("a commit refused with a load open can still abort") {
  // Verify a transaction whose commit a load refused is not left broken:
  // sm3 tears down its side before sm1 refuses.
  // 1. Make a table in a transaction, and open a load on another.
  // 2. Commit fails with SM_ERR_INVALID.
  // 3. Abort: 0, and the table made in it is gone.
  // 4. A new transaction works, and items is empty.
  items_t f(files_options());
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t made = 0;
  start(f.a);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "made", cols, made) == 0);
  sm3::table_t *t = f.open_items(f.a);
  sm3::loader_t *l = open_load(t);
  add_rows(l, 1, 3);
  CHECK(-1 == sm3::commit(f.a));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  sm3::tbl_load_free(l);
  sm3::tbl_close(t);
  CHECK_MESSAGE(0 == sm3::abort(f.a), sm1::errmsg());

  start(f.a);
  CHECK(nullptr == sm3::cat_get_table(f.cat, made));
  t = f.open_items(f.a);
  CHECK(0 == count(t));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a load does not wait for an empty table another is using") {
  // Verify the lock a load takes is one it can have at once.
  // 1. Session b reads items, which is empty, and stays in its
  //    transaction: it holds the table's lock to read.
  // 2. Session a opens a load: the call returns, having waited for
  //    no one, and each row it is handed is in the table at once,
  //    inserted and not on its way to a file.
  // 3. b is not kept out: it opens the table again.
  // 4. a closes and commits; b, in a new transaction, sees the rows.
  items_t f(files_options());
  start(f.b);
  sm3::table_t *reader = f.open_items(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(count(reader) == 0);

  start(f.a);
  sm3::table_t *t = f.open_items(f.a);
  sm3::loader_t *l = open_load(t);
  add_rows(l, 1, 25);
  CHECK(count(t) == 25);
  sm3::table_t *again = f.open_items(f.b, sm1::SM_LOCKMODE_IS);
  sm3::tbl_close(again);
  REQUIRE_MESSAGE(sm3::tbl_load_close(l) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  sm3::tbl_close(reader);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.b);
  reader = f.open_items(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(count(reader) == 25);
  sm3::tbl_close(reader);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("an aborted load leaves its table empty, and ready for another") {
  // Verify an abort after the close, on a table committed empty.
  // 1. Load 100 rows as files, close, and abort.
  // 2. Another session reads no row, by scan or by index.
  // 3. A row is then inserted and found, as in any table.
  items_t f(files_options());
  start(f.a);
  sm3::table_t *t = f.open_items(f.a);
  sm3::loader_t *l = open_load(t);
  add_rows(l, 1, 100);
  REQUIRE(sm3::tbl_load_close(l) == 0);
  CHECK(count(t) == 100);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  start(f.b);
  t = f.open_items(f.b);
  CHECK(count(t) == 0);
  CHECK(by_id(t, f.items_id, 5).empty());
  row_t one(5, "five", "note");
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(sm3::tbl_insert(t, one.d(), one.null(), rowno) == 0,
                  sm1::errmsg());
  CHECK(by_id(t, f.items_id, 5) == std::vector<uint64_t>{rowno});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
}
