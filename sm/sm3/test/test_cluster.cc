/*
 * test_cluster.cc -- a table keyed by its primary key (../CLUSTER.md):
 * its rows found by _id and scanned by key, moved when their key
 * changes, followed by a writer at READ COMMITTED, and rewritten
 * when a primary key is added or dropped.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A row of acct (id int4, owner text, n int4) as a write takes it. */
struct row_t {
  /* The row (id, owner, n). */
  row_t(int32_t id, const std::string &owner, int32_t n)
      : m_owner(sm2::make_varlena(owner)) {
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_owner.data());
    m_d[2] = sm2::datum_from_int32(n);
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_owner;
  sm2::datum_t m_d[3];
  bool m_null[3] = {false, false, false};
};

/*
 * A catalog with acct (id int4 NOT NULL, owner text, n int4) and
 * its index acct_owner on (owner) INCLUDE (n); with keyed, its
 * primary key acct_pkey on (id), made with the table. Sessions a
 * and b over it.
 */
class acct_t : public db_t {
public:
  /* Initialize and load the catalog, create acct, open a and b. */
  explicit acct_t(bool keyed = true) {
    sm1::session_t *s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"owner", sm2::type_text.code, -1, false},
        {"n", sm2::type_int4.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_owner = {
        {1, false, false, std::nullopt}};
    std::vector<int32_t> n = {2};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "acct", cols, acct) == 0);
    REQUIRE(sm3::create_index(a, acct, "acct_owner", by_owner, n, false,
                              owner) == 0);
    if (keyed) {
      uint64_t store = sm3::cat_get_table(a, acct)->storeid;
      add_pkey(a);
      // A table made in this transaction, with no row, is not
      // rewritten.
      CHECK(sm3::cat_get_table(a, acct)->storeid == store);
    }
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~acct_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cat_close(cat);
  }

  /* Add acct_pkey on (id) in s's transaction; it must go in. */
  void add_pkey(sm3::session_t *s) {
    std::vector<sm3::index_key_spec_t> by_id = {
        {0, false, false, std::nullopt}};
    REQUIRE_MESSAGE(sm3::create_index(s, acct, "acct_pkey", by_id, {}, true,
                                      pkey, true, true) == 0,
                    sm1::errmsg());
  }

  /* acct, opened to write in s's transaction; it must open. */
  sm3::table_t *write(sm3::session_t *s) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, acct, sm1::SM_LOCKMODE_IX, t) == 0,
                    sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t acct = 0, owner = 0, pkey = 0;
};

/* Begin a transaction on s, at READ COMMITTED unless told. */
void start(sm3::session_t *s,
           sm1::xctlevel_t level = sm1::xctlevel_t::read_committed) {
  REQUIRE(sm3::begin(s, level) == 0);
}

/* Insert (id, owner, n) into t; it must go in. Its _id. */
uint64_t put(sm3::table_t *t, int32_t id, const std::string &owner,
             int32_t n = 0) {
  row_t r(id, owner, n);
  uint64_t got = 0;
  REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d(), r.null(), got) == 0, sm1::errmsg());
  return got;
}

/* An acct tuple as "id|owner|n". */
std::string show(const sm3::table_t *t, std::string_view tuple) {
  sm2::datum_t d[3];
  bool n[3];
  REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, n) == 0);
  std::string_view o;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d[1], o) == 0);
  return std::to_string(sm2::datum_to_int32(d[0])) + "|" +
         std::string(sm2::varlena_payload(o)) + "|" +
         std::to_string(sm2::datum_to_int32(d[2]));
}

/* The row with _id id as t's transaction sees it, or "<none>". */
std::string get(sm3::table_t *t, uint64_t id) {
  std::string_view tuple;
  int rc = sm3::tbl_read(t, id, tuple);
  REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
  return rc == 1 ? "<none>" : show(t, tuple);
}

/* The name of the row with id k of a table keyed by id (../ROWSTORE.md §2.1).
 */
struct at_t {
  sm2::datum_t d;
  bool null = false;
  explicit at_t(int32_t k) : d(sm2::datum_from_int32(k)) {}
  operator sm3::row_key_t() const { return {{&d, 1}, {&null, 1}}; }
};

/* The row key names, as get gives one. */
std::string get_named(sm3::table_t *t, const sm3::row_key_t &key) {
  std::string_view tuple;
  int rc = sm3::tbl_read(t, key, tuple);
  REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
  return rc == 1 ? "<none>" : show(t, tuple);
}

/* The row with id k of a table keyed by id, as get gives one. */
std::string get_at(sm3::table_t *t, int32_t k) { return get_named(t, at_t(k)); }

/*
 * Every row of t, as "_id:id|owner|n", in its key's order: a keyed
 * row's _id is 0, as it has none.
 */
std::vector<std::string> rows(sm3::table_t *t) {
  std::vector<std::string> out;
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(t, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    out.push_back(std::to_string(id) + ":" + show(t, tuple));
  }
  sm3::tbl_scanend(scan);
  REQUIRE(rc == 1);
  return out;
}

/*
 * The entries of t's index oid, in its order, as (_id, key) with key
 * the row's key in the rel: for a table keyed by id, encoded, and
 * its _id 0.
 */
std::vector<std::pair<uint64_t, std::string>> entries(sm3::table_t *t,
                                                      uint64_t oid) {
  std::vector<std::pair<uint64_t, std::string>> out;
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, oid, nullptr, nullptr, scan) == 0,
                  sm1::errmsg());
  uint64_t id;
  bool cut;
  std::string_view value;
  int rc;
  while ((rc = sm2::idx_scannxt(scan, id, cut, value)) == 0) {
    out.push_back({id, std::string(sm2::idx_scan_row(scan))});
  }
  sm2::idx_scanend(scan);
  REQUIRE(rc == 1);
  return out;
}

/*
 * The keys under relid in store, as s's transaction sees them: sm1's
 * own keys, versioned and, in a chained store, chained, whatever rel
 * holds them.
 */
size_t under_relid(sm3::session_t *s, uint64_t store, uint16_t relid) {
  // Big-endian, as a store's keys begin.
  auto be16 = [](unsigned r) {
    return std::string{static_cast<char>(r >> 8), static_cast<char>(r & 0xFF)};
  };
  std::string lo = be16(relid), hi = be16(relid + 1u);
  bool chained = false;
  REQUIRE(sm1::store_chained(sm3::session_sm1(s), store, chained) == 0);
  size_t n = 0;
  for (bool chain : {false, true}) {
    if (chain && !chained) {
      continue;
    }
    sm1::scan_t *c = nullptr;
    REQUIRE((chain ? sm1::chain_scan(sm3::session_sm1(s), store, lo.data(),
                                     lo.size(), 1, hi.data(), hi.size(), 0, c)
                   : sm1::scan(sm3::session_sm1(s), store, lo.data(), lo.size(),
                               1, hi.data(), hi.size(), 0, c)) == 0);
    std::string_view k, v;
    int rc;
    while ((rc = sm1::scannxt(c, k, v)) == 0) {
      n++;
    }
    sm1::scanend(c);
    REQUIRE(rc == 1);
  }
  return n;
}

/* The key of a row of a table keyed by id. */
std::string key_of(int32_t id) {
  sm2::key_column_t col = {&sm2::type_int4, -1, nullptr, false, false};
  sm2::datum_t d[1] = {sm2::datum_from_int32(id)};
  bool n[1] = {false};
  std::string k;
  REQUIRE(sm2::encode_key({&col, 1}, d, n, k) == 0);
  return k;
}

} // namespace

TEST_CASE("a table with a primary key keeps its rows by key") {
  // Verify the rows of a keyed table are found by key.
  // 1. Rows with ids 30, 10 and 20 go in, with no _id, and each is
  //    read by its key.
  // 2. A scan of the table, and of the primary key, gives them in
  //    key order.
  // 3. The index on owner names each row by its key.
  // 4. A second row with id 10 fails with SM_ERR_DUPKEY, the
  //    primary key in errdup().
  // 5. A read by _id is refused: a keyed row is named by its key.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  CHECK(put(t, 30, "c") == 0);
  CHECK(put(t, 10, "a") == 0);
  CHECK(put(t, 20, "b") == 0);
  CHECK(get_at(t, 10) == "10|a|0");
  CHECK(get_at(t, 40) == "<none>");
  CHECK(rows(t) ==
        std::vector<std::string>{"0:10|a|0", "0:20|b|0", "0:30|c|0"});
  CHECK(entries(t, f.pkey) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(10)}, {0, key_of(20)}, {0, key_of(30)}});
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(10)}, {0, key_of(20)}, {0, key_of(30)}});
  row_t dup(10, "z", 0);
  uint64_t id;
  CHECK(sm3::tbl_insert(t, dup.d(), dup.null(), id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm1::errdup().index == f.pkey);
  std::string_view tuple;
  CHECK(sm3::tbl_read(t, 2, tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("an update of a primary key moves the row, and its entries") {
  // Verify a move: the row, its _id, and its index entries.
  // 1. The row with id 10 is changed to id 15: its new key finds
  //    it, the primary key's scan has it at 15, and its owner entry
  //    names its new key.
  // 2. Changing it to 20, another row's, fails with SM_ERR_DUPKEY.
  // 3. Deleting it removes it, and its owner entry.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 10, "a");
  put(t, 20, "b");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  t = f.write(f.a);
  row_t moved(15, "a2", 5);
  REQUIRE_MESSAGE(sm3::tbl_update(t, at_t(10), moved.d(), moved.null()) == 0,
                  sm1::errmsg());
  CHECK(get_at(t, 15) == "15|a2|5");
  CHECK(entries(t, f.pkey) == std::vector<std::pair<uint64_t, std::string>>{
                                  {0, key_of(15)}, {0, key_of(20)}});
  CHECK(entries(t, f.owner) == std::vector<std::pair<uint64_t, std::string>>{
                                   {0, key_of(15)}, {0, key_of(20)}});
  int mark = sm3::savepoint(f.a);
  REQUIRE(mark >= 0);
  row_t clash(20, "a3", 0);
  CHECK(sm3::tbl_update(t, at_t(15), clash.d(), clash.null()) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm1::errdup().index == f.pkey);
  REQUIRE(sm3::rollback(f.a, mark) == 0);
  REQUIRE(sm3::tbl_delete(t, at_t(15)) == 0);
  CHECK(get_at(t, 15) == "<none>");
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{{0, key_of(20)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a writer that meets a row moved since fails with SM_ERR_MOVED") {
  // Verify a write by key at a key another transaction moved the row
  // from is refused, not followed (../ROWSTORE.md §2.5).
  // 1. a inserts id 1 and commits; b's statement reads it by key.
  // 2. a moves it to id 2 by key and commits.
  // 3. At READ COMMITTED b's update by key meets SM_ERR_UPDATED, and
  //    its newest read by key SM_ERR_MOVED.
  // 4. Above READ COMMITTED the same update fails with
  //    SM_ERR_CONFLICT.
  // 5. The row is at id 2, as a left it.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read}) {
    CAPTURE(static_cast<int>(level));
    acct_t f;
    sm2::datum_t one = sm2::datum_from_int32(1), two = sm2::datum_from_int32(2);
    bool null = false;
    sm3::row_key_t at1{{&one, 1}, {&null, 1}}, at2{{&two, 1}, {&null, 1}};
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    put(t, 1, "one");
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    start(f.b, level);
    REQUIRE(sm3::stmt_begin(f.b) == 0);
    sm3::table_t *tb = f.write(f.b);
    std::string_view tuple;
    REQUIRE(sm3::tbl_read(tb, at1, tuple) == 0);
    CHECK(show(tb, tuple) == "1|one|0");
    start(f.a);
    t = f.write(f.a);
    row_t moved(2, "one", 0);
    REQUIRE_MESSAGE(sm3::tbl_update(t, at1, moved.d(), moved.null()) == 0,
                    sm1::errmsg());
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    row_t mine(1, "mine", 7);
    CHECK(sm3::tbl_update(tb, at1, mine.d(), mine.null()) == -1);
    if (level != sm1::xctlevel_t::read_committed) {
      CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
      sm3::tbl_close(tb);
      sm3::abort(f.b);
      continue;
    }
    REQUIRE(sm1::errnum() == sm1::SM_ERR_UPDATED);
    CHECK(sm3::tbl_read_latest(tb, at1, tuple) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_MOVED);
    sm3::tbl_close(tb);
    REQUIRE(sm3::abort(f.b) == 0);
    start(f.a);
    t = f.write(f.a);
    REQUIRE(sm3::tbl_read(t, at2, tuple) == 0);
    CHECK(show(t, tuple) == "2|one|0");
    CHECK(sm3::tbl_read(t, at1, tuple) == 1);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("an insert that conflicts on the primary key says whose row") {
  // Verify tbl_insert_arbiter with the primary key as an arbiter,
  // at READ COMMITTED, which confirms against the newest version,
  // and REPEATABLE READ, which reads at its snapshot.
  // 1. With id 10 in the table, an insert of id 10 arbitrated by
  //    the primary key returns 1, no _id, 0, and that row's tuple,
  //    from which a caller takes its key, writing nothing.
  // 2. So does one arbitrated by every unique index.
  // 3. An insert of id 11 goes in.
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read}) {
    CAPTURE(static_cast<int>(level));
    acct_t f;
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    put(t, 10, "a");
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    start(f.b, level);
    t = f.write(f.b);
    row_t again(10, "b", 0);
    uint64_t id = 0;
    std::string tuple;
    uint64_t arbiters[1] = {f.pkey};
    CHECK(sm3::tbl_insert_arbiter(t, again.d(), again.null(), arbiters, id,
                                  &tuple) == 1);
    CHECK(id == 0);
    CHECK(show(t, tuple) == "10|a|0");
    CHECK(sm3::tbl_insert_arbiter(t, again.d(), again.null(), {}, id) == 1);
    CHECK(id == 0);
    row_t eleven(11, "c", 0);
    REQUIRE(sm3::tbl_insert_arbiter(t, eleven.d(), eleven.null(), {}, id) == 0);
    CHECK(get_at(t, 11) == "11|c|0");
    CHECK(rows(t).size() == 2);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("adding or dropping a primary key rewrites the table") {
  // Verify the rewrite (../CLUSTER.md §8).
  // 1. A table without a primary key gets rows with ids 3, 1 and 2,
  //    _ids 1 to 3, committed; its owner index names them by _id.
  // 2. Adding the primary key moves the table to a new store: the
  //    rows lose their _ids, its scan has them in key order, and
  //    the owner index names them by key.
  // 3. Dropping it moves the table again: the rows draw _ids 4 to 6
  //    from the table's sequence, in key order, and the owner index
  //    names them by _id once more.
  // 4. A primary key added over two rows with one id fails with
  //    SM_ERR_DUPKEY, naming the later row, _id 7, by its key in the
  //    old store, the rows' key order.
  acct_t f(false);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 3, "c");
  put(t, 1, "a");
  put(t, 2, "b");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t store = sm3::cat_get_table(f.cat, f.acct)->storeid;
  start(f.a);
  f.add_pkey(f.a);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(sm3::cat_get_table(f.cat, f.acct)->storeid != store);
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t) == std::vector<std::string>{"0:1|a|0", "0:2|b|0", "0:3|c|0"});
  CHECK(entries(t, f.pkey) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(1)}, {0, key_of(2)}, {0, key_of(3)}});
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(1)}, {0, key_of(2)}, {0, key_of(3)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  REQUIRE_MESSAGE(sm3::drop_index(f.a, f.acct, f.pkey) == 0, sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t) == std::vector<std::string>{"4:1|a|0", "5:2|b|0", "6:3|c|0"});
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {4, sm2::id_key(4)}, {5, sm2::id_key(5)}, {6, sm2::id_key(6)}});
  put(t, 2, "again");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  std::vector<sm3::index_key_spec_t> by_id = {{0, false, false, std::nullopt}};
  uint64_t oid = 0;
  CHECK(sm3::create_index(f.a, f.acct, "acct_pkey", by_id, {}, true, oid, true,
                          true) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm1::errdup().index != 0);
  CHECK(sm1::errdup().row == sm2::id_key(7));
  sm3::abort(f.a);
}

TEST_CASE("adding a primary key takes away the rows' _id") {
  // Verify a table without a primary key has an _id in every row,
  // and that adding one leaves none, nor anything of an _id index
  // (../ROWSTORE.md §2.2).
  // 1. A table without a primary key gets rows with ids 3, 1 and 2:
  //    _ids 1 to 3, each row read by its _id, its key, and its owner
  //    entry naming it by _id.
  // 2. Adding the primary key on id rewrites the table.
  // 3. The rows have no _id: they come out in key order with 0, a
  //    read by _id is refused, and a new row draws none.
  // 4. Nothing is under the relid the _id index had, 0xFFFE, in the
  //    new store, which holds the four rows; the owner index names
  //    each row by its key, with no _id.
  acct_t f(false);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  CHECK(put(t, 3, "c") == 1);
  CHECK(put(t, 1, "a") == 2);
  CHECK(put(t, 2, "b") == 3);
  CHECK(get(t, 2) == "1|a|0");
  CHECK(rows(t) == std::vector<std::string>{"1:3|c|0", "2:1|a|0", "3:2|b|0"});
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {2, sm2::id_key(2)}, {3, sm2::id_key(3)}, {1, sm2::id_key(1)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  f.add_pkey(f.a);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t) == std::vector<std::string>{"0:1|a|0", "0:2|b|0", "0:3|c|0"});
  std::string_view tuple;
  CHECK(sm3::tbl_read(t, 2, tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(put(t, 4, "d") == 0);
  uint64_t store = sm3::cat_get_table(f.a, f.acct)->storeid;
  CHECK(under_relid(f.a, store, sm2::id_relid) == 0);
  CHECK(under_relid(f.a, store, sm3::heap_relid) == 4);
  CHECK(entries(t, f.owner) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(1)}, {0, key_of(2)}, {0, key_of(3)}, {0, key_of(4)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a load and an index build on a keyed table name rows by key") {
  // Verify a bulk load and a backfill into a table keyed by id.
  // 1. A load of ids 3, 1 and 2 into the empty table: the rows come
  //    out by key, with no _id.
  // 2. An index on n made after them is pending; its backfill names
  //    each row by its key.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  sm3::loader_t *load = nullptr;
  REQUIRE(sm3::tbl_load_open(t, load) == 0);
  for (int32_t id : {3, 1, 2}) {
    row_t r(id, "l", id * 10);
    uint64_t got;
    REQUIRE_MESSAGE(sm3::tbl_load_add(load, r.d(), r.null(), got) == 0,
                    sm1::errmsg());
  }
  REQUIRE_MESSAGE(sm3::tbl_load_close(load) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t) ==
        std::vector<std::string>{"0:1|l|10", "0:2|l|20", "0:3|l|30"});
  CHECK(entries(t, f.pkey) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(1)}, {0, key_of(2)}, {0, key_of(3)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::vector<sm3::index_key_spec_t> by_n = {{2, false, false, std::nullopt}};
  uint64_t acct_n = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.acct, "acct_n", by_n, {}, true, acct_n) ==
          0);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.b);
  t = f.write(f.b);
  std::string next;
  bool done = false;
  while (!done) {
    REQUIRE_MESSAGE(sm3::tbl_index_backfill(t, acct_n, next, 2, next, done) ==
                        0,
                    sm1::errmsg());
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
  start(f.b);
  REQUIRE(sm3::set_index_state(f.b, f.acct, acct_n,
                               sm3::index_state_t::valid) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
  start(f.a);
  t = f.write(f.a);
  CHECK(entries(t, acct_n) ==
        std::vector<std::pair<uint64_t, std::string>>{
            {0, key_of(1)}, {0, key_of(2)}, {0, key_of(3)}});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a table's rows are read by key, and scanned by the primary key") {
  // Verify tbl_read_key and tbl_idx_scan_rows.
  // 1. Rows with ids 30, 10 and 20 go in.
  // 2. The owner index's entry for "b", its second, names its row's
  //    key, and the row read by that key is (20, b).
  // 3. The primary key's rows from 15 on are 20 and 30, in key
  //    order, each with no _id.
  // 4. The owner index is no primary key: its rows are refused.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 30, "c");
  put(t, 10, "a");
  put(t, 20, "b");
  std::string key = entries(t, f.owner)[1].second;
  REQUIRE(key == key_of(20));
  sm3::pin_ref_t ref;
  std::string_view tuple;
  REQUIRE(sm3::tbl_read_key(t, key, ref, tuple) == 0);
  CHECK(show(t, tuple) == "20|b|0");
  sm3::unpin(ref);
  CHECK(sm3::tbl_read_key(t, key_of(99), ref, tuple) == 1);
  sm2::datum_t d15[1] = {sm2::datum_from_int32(15)};
  bool n[1] = {false};
  sm2::bound_t lo{d15, n, true};
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan_rows(t, f.pkey, &lo, nullptr, scan) == 0);
  std::vector<std::string> got;
  uint64_t id;
  while (sm3::tbl_scannxt(scan, id, ref, tuple) == 0) {
    got.push_back(std::to_string(id) + ":" + show(t, tuple));
  }
  sm3::tbl_scanend(scan);
  CHECK(got == std::vector<std::string>{"0:20|b|0", "0:30|c|0"});
  CHECK(sm3::tbl_idx_scan_rows(t, f.owner, nullptr, nullptr, scan) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a row named by its key is read, written and locked") {
  // Verify the calls by key (../ROWSTORE.md §2.4), in a table with a
  // primary key, named by id, and in one without, named by _id.
  // 1. Rows with ids 10, 20 and 30 go in, committed.
  // 2. A read by key gives the row; a key no row has gives 1.
  // 3. An update by key changes a row in place, and one that
  //    changes its key moves it, with its owner entry.
  // 4. A lock by key holds the row: b cannot take it.
  // 5. A delete by key removes the row and its entry; again, it is
  //    SM_ERR_NOTFOUND.
  // 6. A null in a key, or a key of the wrong count, is
  //    SM_ERR_INVALID.
  for (bool keyed : {true, false}) {
    CAPTURE(keyed);
    acct_t f(keyed);
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    uint64_t ids[3] = {put(t, 10, "a"), put(t, 20, "b"), put(t, 30, "c")};
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    // The name of the row with id i, at _id ids[j].
    sm2::datum_t d;
    bool null = false;
    auto name = [&](int32_t i, size_t j) {
      d = keyed ? sm2::datum_from_int32(i)
                : sm2::datum_from_int64(static_cast<int64_t>(ids[j]));
      return sm3::row_key_t{{&d, 1}, {&null, 1}};
    };
    start(f.a);
    t = f.write(f.a);
    std::string_view tuple;
    REQUIRE_MESSAGE(sm3::tbl_read(t, name(20, 1), tuple) == 0, sm1::errmsg());
    CHECK(show(t, tuple) == "20|b|0");
    sm2::datum_t none =
        keyed ? sm2::datum_from_int32(99) : sm2::datum_from_int64(99);
    CHECK(sm3::tbl_read(t, {{&none, 1}, {&null, 1}}, tuple) == 1);
    row_t same(20, "b2", 7);
    REQUIRE_MESSAGE(sm3::tbl_update(t, name(20, 1), same.d(), same.null()) == 0,
                    sm1::errmsg());
    CHECK(get_named(t, name(20, 1)) == "20|b2|7");
    if (keyed) {
      row_t moved(15, "a", 1);
      REQUIRE_MESSAGE(
          sm3::tbl_update(t, name(10, 0), moved.d(), moved.null()) == 0,
          sm1::errmsg());
      CHECK(get_named(t, name(15, 0)) == "15|a|1");
      CHECK(
          entries(t, f.owner) ==
          std::vector<std::pair<uint64_t, std::string>>{{ids[0], key_of(15)},
                                                        {ids[1], key_of(20)},
                                                        {ids[2], key_of(30)}});
    }
    REQUIRE(sm3::tbl_lock(t, name(30, 2), sm1::SM_LOCKMODE_EX) == 0);
    start(f.b);
    sm3::table_t *tb = f.write(f.b);
    CHECK(sm3::tbl_trylock(tb, name(30, 2), sm1::SM_LOCKMODE_EX) == 1);
    sm3::tbl_close(tb);
    REQUIRE(sm3::abort(f.b) == 0);
    REQUIRE_MESSAGE(sm3::tbl_delete(t, name(20, 1)) == 0, sm1::errmsg());
    CHECK(get_named(t, name(20, 1)) == "<none>");
    CHECK(entries(t, f.owner).size() == 2);
    CHECK(sm3::tbl_delete(t, name(20, 1)) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
    null = true;
    CHECK(sm3::tbl_read(t, name(30, 2), tuple) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    null = false;
    sm2::datum_t two[2] = {d, d};
    bool nulls[2] = {false, false};
    CHECK(sm3::tbl_lock(t, {two, nulls}, sm1::SM_LOCKMODE_EX) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("a writer at READ COMMITTED writes by key after a re-read") {
  // Verify a write by key that meets a newer version goes through
  // once the newest is read by key, as dg's re-check does.
  // 1. a inserts id 1 and commits; b's statement reads it.
  // 2. a updates its owner and commits.
  // 3. b's update by key meets SM_ERR_UPDATED; its newest version,
  //    read by key, is a's, and the update then goes through.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "one");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.b);
  REQUIRE(sm3::stmt_begin(f.b) == 0);
  sm3::table_t *tb = f.write(f.b);
  CHECK(get_at(tb, 1) == "1|one|0");
  start(f.a);
  t = f.write(f.a);
  row_t theirs(1, "theirs", 0);
  REQUIRE(sm3::tbl_update(t, at_t(1), theirs.d(), theirs.null()) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  sm2::datum_t d = sm2::datum_from_int32(1);
  bool null = false;
  sm3::row_key_t key{{&d, 1}, {&null, 1}};
  row_t mine(1, "mine", 7);
  CHECK(sm3::tbl_update(tb, key, mine.d(), mine.null()) == -1);
  REQUIRE(sm1::errnum() == sm1::SM_ERR_UPDATED);
  std::string_view tuple;
  REQUIRE_MESSAGE(sm3::tbl_read_latest(tb, key, tuple) == 0, sm1::errmsg());
  CHECK(show(tb, tuple) == "1|theirs|0");
  REQUIRE_MESSAGE(sm3::tbl_update(tb, key, mine.d(), mine.null()) == 0,
                  sm1::errmsg());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t) == std::vector<std::string>{"0:1|mine|7"});
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an index build goes on past a row moved meanwhile") {
  // Verify a row moved between a batch's scan and its read is gone from
  // its key, as its mover made its entry (../INDEXBUILD.md §5).
  // 1. Fill a keyed table with 4,000 rows.
  // 2. Start a build of an index on n, in small batches.
  // 3. Meanwhile a writer moves each row to another primary key.
  // 4. The build ends valid, and the index gives each row once.
  acct_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  const int rows = 4000;
  for (int i = 1; i <= rows; i++) {
    put(t, i, "o", i);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::vector<sm3::index_key_spec_t> by_n = {{2, false, false, std::nullopt}};
  uint64_t ix = 0;
  start(f.a);
  REQUIRE_MESSAGE(sm3::create_index(f.a, f.acct, "by_n", by_n, {}, false, ix) ==
                      0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  std::atomic<int> moved{0}, failed{0};
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int i = 1; i <= rows && !stop; i++) {
      if (sm3::begin(f.b, sm1::xctlevel_t::read_committed) != 0) {
        failed++;
        return;
      }
      sm3::table_t *w = nullptr;
      if (sm3::tbl_open(f.b, f.acct, sm1::SM_LOCKMODE_IX, w) != 0) {
        failed++;
        sm3::abort(f.b);
        return;
      }
      row_t r(i + 1000000, "o", i);
      bool ok = sm3::tbl_update(w, at_t(i), r.d(), r.null()) == 0;
      sm3::tbl_close(w);
      if (ok && sm3::commit(f.b) == 0) {
        moved++;
      } else {
        sm3::abort(f.b);
        failed++;
      }
    }
  });
  REQUIRE(sm3::builder_attach(f.cat, [&f] { return f.try_open(); }, 7) == 0);
  REQUIRE(sm3::enqueue_pending_indexes(f.cat) == 0);
  int rc = sm3::builder_wait(f.a, f.acct, ix);
  std::string err = 0 == rc ? "" : std::string(sm1::errmsg());
  stop = true;
  writer.join();
  // The fixture leaves the builder alone: its workers are joined here.
  sm3::builder_detach(f.cat);
  CHECK_MESSAGE(0 == rc, err);
  CHECK(0 == failed);
  CHECK(moved > 0);
  start(f.a);
  t = f.write(f.a);
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, ix, nullptr, nullptr, scan) == 0,
                  sm1::errmsg());
  int seen = 0;
  uint64_t id = 0;
  bool cut = false;
  std::string_view value;
  while (0 == sm2::idx_scannxt(scan, id, cut, value)) {
    seen++;
  }
  sm2::idx_scanend(scan);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(rows == seen);
}

TEST_CASE(
    "issue: an arbiter meets a row moved since its statement's snapshot") {
  // Reproduce: find_now reads the row its snapshot's entry names, and a
  // row moved since gives SM_ERR_MOVED.
  // 1. Make a keyed table with a unique index on name, of each kind, and
  //    a row (1, "x").
  // 2. a begins a statement at READ COMMITTED, and takes its snapshot.
  // 3. b moves the row to primary key 2, and commits.
  // 4. a's insert of (5, "x") by the arbiter must report the conflict,
  //    not fail.
  for (bool tree : {false, true}) {
    CAPTURE(tree);
    acct_t f;
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"owner", sm2::type_text.code, -1, false},
        {"n", sm2::type_int4.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_id = {
        {0, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_owner = {
        {1, false, false, std::nullopt}};
    uint64_t tb = 0, pk = 0, uq = 0;
    start(f.a);
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "m", cols, tb) == 0);
    REQUIRE(sm3::create_index(f.a, tb, "m_pkey", by_id, {}, true, pk, true,
                              true) == 0);
    REQUIRE(sm3::create_index(f.a, tb, "m_owner", by_owner, {}, true, uq, false,
                              false, tree) == 0);
    REQUIRE(sm3::commit(f.a) == 0);
    auto open = [&](sm3::session_t *s) {
      sm3::table_t *t = nullptr;
      REQUIRE(sm3::tbl_open(s, tb, sm1::SM_LOCKMODE_IX, t) == 0);
      return t;
    };
    start(f.a);
    sm3::table_t *t = open(f.a);
    put(t, 1, "x");
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.a);
    t = open(f.a);
    REQUIRE(sm3::stmt_begin(f.a) == 0);
    std::vector<uint64_t> arb = {uq};
    row_t other(50, "y", 0);
    uint64_t id = 0;
    REQUIRE(sm3::tbl_insert_arbiter(t, other.d(), other.null(), arb, id) == 0);

    start(f.b);
    sm3::table_t *w = open(f.b);
    row_t moved(2, "x", 0);
    REQUIRE_MESSAGE(sm3::tbl_update(w, at_t(1), moved.d(), moved.null()) == 0,
                    sm1::errmsg());
    sm3::tbl_close(w);
    REQUIRE(sm3::commit(f.b) == 0);

    row_t clash(5, "x", 0);
    int rc = sm3::tbl_insert_arbiter(t, clash.d(), clash.null(), arb, id);
    std::string err = rc < 0 ? std::string(sm1::errmsg()) : "";
    MESSAGE("tree ", tree, ": rc ", rc, " err '", err, "'");
    CHECK(rc >= 0);
    sm3::tbl_close(t);
    sm3::abort(f.a);
  }
}
