/*
 * test_altertype.cc -- a column's type changed (alter_column_type,
 * ../dg/ALTERTYPE.md at the top level): its rows converted into a
 * new store by the host's converter, a key column's rows keyed
 * anew, the record alone changed when the values stay, and what
 * would read the column stale refused.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

/* An evaluator that puts every row in its index and computes nothing. */
int all_rows(const sm3::table_def_t &, const sm3::index_def_t &,
             std::span<const sm2::datum_t>, std::span<const bool>,
             bool &belongs, std::vector<std::string> &, std::span<bool>) {
  belongs = true;
  return 0;
}

/*
 * A catalog with t (a int4 NOT NULL, b text, c int4), its primary
 * key t_pkey on (a), made with it, and t_b on (b). Session s over it,
 * the catalog given all_rows as its evaluator.
 */
class alt_t : public db_t {
public:
  /* Initialize and load the catalog, open s, and make t. */
  alt_t() {
    sm1::session_t *raw = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::cat_set_evaluator(cat, all_rows) == 0);
    REQUIRE(sm3::session_open(cat, raw, s) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"a", sm2::type_int4.code, -1, true},
        {"b", sm2::type_text.code, -1, false},
        {"c", sm2::type_int4.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_a = {{0, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_b = {{1, false, false, std::nullopt}};
    begin();
    REQUIRE(sm3::create_table(s, sm3::nsp_public, "t", cols, t) == 0);
    REQUIRE(sm3::create_index(s, t, "t_pkey", by_a, {}, true, pkey, true,
                              true) == 0);
    REQUIRE(sm3::create_index(s, t, "t_b", by_b, {}, false, by_b_oid) == 0);
    REQUIRE(sm3::commit(s) == 0);
  }

  /* Close the session and the catalog. */
  ~alt_t() {
    sm3::session_close(s);
    sm3::cat_close(cat);
  }

  /* Begin a transaction on s. */
  void begin() { REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0); }

  /* t, opened to write; it must open. */
  sm3::table_t *write() {
    sm3::table_t *x = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, t, sm1::SM_LOCKMODE_IX, x) == 0,
                    sm1::errmsg());
    return x;
  }

  /* Insert (a, b, c) into t and commit; b "" is a null. */
  void put(int32_t a, const std::string &b, int32_t c) {
    begin();
    sm3::table_t *x = write();
    std::string vb = sm2::make_varlena(b);
    sm2::datum_t d[3] = {sm2::datum_from_int32(a),
                         sm2::datum_from_ptr(vb.data()),
                         sm2::datum_from_int32(c)};
    bool n[3] = {false, b.empty(), false};
    uint64_t id = 0;
    REQUIRE_MESSAGE(sm3::tbl_insert(x, d, n, id) == 0, sm1::errmsg());
    sm3::tbl_close(x);
    REQUIRE(sm3::commit(s) == 0);
  }

  /* t's store now. */
  uint64_t store() { return sm3::cat_get_table(cat, t)->storeid; }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *s = nullptr;
  uint64_t t = 0, pkey = 0, by_b_oid = 0;
};

/* A value of column i of a deformed row, as text: "-" for a null. */
std::string show_value(const sm3::column_def_t &c, sm2::datum_t d, bool null) {
  if (null) {
    return "-";
  }
  if (c.type == &sm2::type_int4) {
    return std::to_string(sm2::datum_to_int32(d));
  }
  if (c.type == &sm2::type_int8) {
    return std::to_string(sm2::datum_to_int64(d));
  }
  std::string_view bytes;
  REQUIRE(sm2::datum_bytes(c.type, c.typmod, d, bytes) == 0);
  return std::string(sm2::varlena_payload(bytes));
}

/* Every row of t in s's transaction, as "a|b|c", in its key's order. */
std::vector<std::string> rows(alt_t &f) {
  sm3::table_t *x = nullptr;
  REQUIRE(sm3::tbl_open(f.s, f.t, sm1::SM_LOCKMODE_IS, x) == 0);
  const sm3::table_def_t &def = sm3::tbl_def(x);
  std::vector<std::string> out;
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan_rows(x, scan) == 0);
  uint64_t id;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, id, tuple)) == 0) {
    sm2::datum_t d[3];
    bool n[3];
    REQUIRE(sm2::tup_deform(def.desc, tuple, d, n) == 0);
    std::string r;
    for (size_t i = 0; i < 3; i++) {
      r += (i ? "|" : "") + show_value(def.columns[i], d[i], n[i]);
    }
    out.push_back(r);
  }
  sm3::tbl_scanend(scan);
  REQUIRE(rc == 1);
  sm3::tbl_close(x);
  return out;
}

/* Column i's value of a row as the converter is shown it, as text. */
std::string text_of(std::span<const sm2::datum_t> v, size_t i) {
  std::string_view bytes;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, v[i], bytes) == 0);
  return std::string(sm2::varlena_payload(bytes));
}

/* An int4's bytes, as sm2::datum_bytes gives a by-value type's. */
std::string int4_bytes(int32_t v) {
  return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
}

/* t's type of column i now, as the catalog has it. */
const sm2::type_t *type_of(alt_t &f, size_t i) {
  return sm3::cat_get_table(f.cat, f.t)->columns[i].type;
}

} // namespace

TEST_CASE("a column's type changed converts its rows into a new store") {
  // Verify a rewrite with the host's converter (ALTERTYPE.md §3).
  // 1. t has rows (1, "30", 1), (2, "4", 2) and (3, "200", 3).
  // 2. b goes from text to int4, each value parsed by the converter.
  // 3. t is in a new store, b is int4, and the rows read back
  //    converted; t_b's entries are in int4's order, 4 before 30.
  alt_t f;
  f.put(1, "30", 1);
  f.put(2, "4", 2);
  f.put(3, "200", 3);
  uint64_t store = f.store();
  sm3::column_conv_fn conv = [](std::span<const sm2::datum_t> v,
                                std::span<const bool> n, std::string &out,
                                bool &out_null) {
    out_null = n[1];
    if (!out_null) {
      out = int4_bytes(std::stoi(text_of(v, 1)));
    }
    return 0;
  };
  f.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(f.s, f.t, "b",
                                         {sm2::type_int4.code, -1}, &conv) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  CHECK(f.store() != store);
  CHECK(type_of(f, 1) == &sm2::type_int4);
  f.begin();
  CHECK(rows(f) == std::vector<std::string>{"1|30|1", "2|4|2", "3|200|3"});
  sm3::table_t *x = nullptr;
  REQUIRE(sm3::tbl_open(f.s, f.t, sm1::SM_LOCKMODE_IS, x) == 0);
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan(x, f.by_b_oid, nullptr, nullptr, scan) == 0);
  std::vector<std::string> order;
  uint64_t id;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, id, cut, value) == 0) {
    sm3::pin_ref_t ref{};
    std::string_view tuple;
    REQUIRE(sm3::tbl_read_key(x, sm2::idx_scan_row(scan), ref, tuple) == 0);
    sm2::datum_t d[3];
    bool n[3];
    REQUIRE(sm2::tup_deform(sm3::tbl_def(x).desc, tuple, d, n) == 0);
    order.push_back(std::to_string(sm2::datum_to_int32(d[1])));
    sm3::unpin(ref);
  }
  sm2::idx_scanend(scan);
  sm3::tbl_close(x);
  CHECK(order == std::vector<std::string>{"4", "30", "200"});
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("a key column's type changed keys the rows anew") {
  // Verify a key column's rewrite (CLUSTER.md §8, ALTERTYPE.md §5).
  // 1. t has rows with a = 1, 2 and 10.
  // 2. a goes from int4 to text: the rows are keyed by text, so a
  //    scan has "1", "10", "2".
  // 3. A conversion of c to a / 10 within a key: two rows with one
  //    key once a goes back to int4 by c's value fail with
  //    SM_ERR_DUPKEY, and the abort leaves t as it was.
  alt_t f;
  f.put(1, "x", 0);
  f.put(2, "y", 0);
  f.put(10, "z", 0);
  sm3::column_conv_fn to_text = [](std::span<const sm2::datum_t> v,
                                   std::span<const bool>, std::string &out,
                                   bool &out_null) {
    out_null = false;
    out = sm2::make_varlena(std::to_string(sm2::datum_to_int32(v[0])));
    return 0;
  };
  f.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(
                      f.s, f.t, "a", {sm2::type_text.code, -1}, &to_text) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  f.begin();
  CHECK(rows(f) == std::vector<std::string>{"1|x|0", "10|z|0", "2|y|0"});
  REQUIRE(sm3::commit(f.s) == 0);

  // Every row to one key: the second fails the rewrite.
  sm3::column_conv_fn to_one = [](std::span<const sm2::datum_t>,
                                  std::span<const bool>, std::string &out,
                                  bool &out_null) {
    out_null = false;
    out = int4_bytes(1);
    return 0;
  };
  uint64_t store = f.store();
  f.begin();
  CHECK(sm3::alter_column_type(f.s, f.t, "a", {sm2::type_int4.code, -1},
                               &to_one) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::abort(f.s);
  CHECK(f.store() == store);
  CHECK(type_of(f, 0) == &sm2::type_text);
  f.begin();
  CHECK(rows(f) == std::vector<std::string>{"1|x|0", "10|z|0", "2|y|0"});
  REQUIRE(sm3::commit(f.s) == 0);
}

TEST_CASE("a duplicate in a unique index names that index and the row") {
  // Verify the rewrite says where the converted rows collide, for the
  // host's error (ALTERTYPE.md §5).
  // 1. u (a int4 NOT NULL, c int4), keyed by a, has rows with c = 1
  //    and 2, and a unique index u_c on c, made with it.
  // 2. c goes to int4, every value 0: SM_ERR_DUPKEY, naming u_c, not
  //    the primary key, and the later row by its key in the old
  //    store.
  alt_t f;
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"c", sm2::type_int4.code, -1, false}};
  std::vector<sm3::index_key_spec_t> by_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> by_c = {{1, false, false, std::nullopt}};
  uint64_t u = 0, u_pkey = 0, t_c = 0;
  f.begin();
  REQUIRE(sm3::create_table(f.s, sm3::nsp_public, "u", cols, u) == 0);
  REQUIRE(sm3::create_index(f.s, u, "u_pkey", by_a, {}, true, u_pkey, true,
                            true) == 0);
  REQUIRE(sm3::create_index(f.s, u, "u_c", by_c, {}, true, t_c) == 0);
  sm3::table_t *x = nullptr;
  REQUIRE(sm3::tbl_open(f.s, u, sm1::SM_LOCKMODE_IX, x) == 0);
  for (int32_t i : {1, 2}) {
    sm2::datum_t d[2] = {sm2::datum_from_int32(i), sm2::datum_from_int32(i)};
    bool n[2] = {false, false};
    uint64_t id = 0;
    REQUIRE(sm3::tbl_insert(x, d, n, id) == 0);
  }
  sm3::tbl_close(x);
  REQUIRE(sm3::commit(f.s) == 0);
  sm3::column_conv_fn to_zero = [](std::span<const sm2::datum_t>,
                                   std::span<const bool>, std::string &out,
                                   bool &out_null) {
    out_null = false;
    out = int4_bytes(0);
    return 0;
  };
  f.begin();
  CHECK(sm3::alter_column_type(f.s, u, "c", {sm2::type_int4.code, -1},
                               &to_zero) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm1::errdup().index == t_c);
  sm2::key_column_t col = {&sm2::type_int4, -1, nullptr, false, false};
  sm2::datum_t d[1] = {sm2::datum_from_int32(2)};
  bool n[1] = {false};
  std::string key;
  REQUIRE(sm2::encode_key({&col, 1}, d, n, key) == 0);
  CHECK(sm1::errdup().row == key);
  sm3::abort(f.s);
}

TEST_CASE("a converter's failure, or a null in a NOT NULL column, fails it") {
  // Verify the change is all or nothing (ALTERTYPE.md §5).
  // 1. t has two rows.
  // 2. A converter that fails at the second row fails the call with
  //    its error; the abort leaves t as it was.
  // 3. A converter that gives a, NOT NULL, a null fails with
  //    SM_ERR_NOTNULL.
  alt_t f;
  f.put(1, "x", 0);
  f.put(2, "y", 0);
  uint64_t store = f.store();
  int calls = 0;
  sm3::column_conv_fn fails = [&calls](std::span<const sm2::datum_t>,
                                       std::span<const bool>, std::string &out,
                                       bool &out_null) {
    if (++calls == 2) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "the converter failed");
    }
    out_null = false;
    out = int4_bytes(0);
    return 0;
  };
  f.begin();
  CHECK(sm3::alter_column_type(f.s, f.t, "c", {sm2::type_int4.code, -1},
                               &fails) == -1);
  CHECK(std::string(sm1::errmsg()) == "the converter failed");
  sm3::abort(f.s);
  CHECK(f.store() == store);
  f.begin();
  CHECK(rows(f) == std::vector<std::string>{"1|x|0", "2|y|0"});
  REQUIRE(sm3::commit(f.s) == 0);

  sm3::column_conv_fn to_null = [](std::span<const sm2::datum_t>,
                                   std::span<const bool>, std::string &,
                                   bool &out_null) {
    out_null = true;
    return 0;
  };
  f.begin();
  CHECK(sm3::alter_column_type(f.s, f.t, "a", {sm2::type_int8.code, -1},
                               &to_null) == -1);
  CHECK_MESSAGE(sm1::errnum() == sm1::SM_ERR_NOTNULL,
                std::string(sm1::errmsg()));
  sm3::abort(f.s);
  CHECK(type_of(f, 0) == &sm2::type_int4);
}

TEST_CASE("a change whose values stay changes the record alone") {
  // Verify the change without a rewrite (ALTERTYPE.md §4).
  // 1. b goes from text to varchar with no limit, with no converter:
  //    the store stays, b is varchar, and the rows read as before.
  // 2. With no converter, SM_ERR_INVALID: c from int4 to int8; b to
  //    varchar(10), which a value may be longer than.
  // 3. An empty table's column changes with a converter that is
  //    never called, in its store.
  // 4. With no converter, on e's columns: varchar(10) to varchar(20)
  //    is taken; to varchar(5), and a decimal's scale from 2 to 4,
  //    are SM_ERR_INVALID.
  alt_t f;
  f.put(1, "x", 0);
  uint64_t store = f.store();
  f.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(f.s, f.t, "b",
                                         {sm2::type_varchar.code, -1},
                                         nullptr) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  CHECK(f.store() == store);
  CHECK(type_of(f, 1) == &sm2::type_varchar);
  f.begin();
  CHECK(rows(f) == std::vector<std::string>{"1|x|0"});
  CHECK(sm3::alter_column_type(f.s, f.t, "c", {sm2::type_int8.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::alter_column_type(f.s, f.t, "b", {sm2::type_varchar.code, 14},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::abort(f.s);

  // A decimal's typmod: (precision << 16 | scale) + 4, as PostgreSQL's.
  auto dec = [](int p, int s) { return ((p << 16) | s) + 4; };
  std::vector<sm3::column_spec_t> cols = {
      {"k", sm2::type_int4.code, -1, true},
      {"v", sm2::type_varchar.code, 14, false},
      {"d", sm2::type_decimal64.code, dec(10, 2), false}};
  uint64_t e = 0;
  f.begin();
  REQUIRE(sm3::create_table(f.s, sm3::nsp_public, "e", cols, e) == 0);
  REQUIRE(sm3::commit(f.s) == 0);
  uint64_t e_store = sm3::cat_get_table(f.cat, e)->storeid;
  int calls = 0;
  sm3::column_conv_fn counted = [&calls](std::span<const sm2::datum_t>,
                                         std::span<const bool>, std::string &,
                                         bool &) { return ++calls, -1; };
  f.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(f.s, e, "k", {sm2::type_int8.code, -1},
                                         &counted) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  CHECK(calls == 0);
  CHECK(sm3::cat_get_table(f.cat, e)->storeid == e_store);
  CHECK(sm3::cat_get_table(f.cat, e)->columns[0].type == &sm2::type_int8);

  f.begin();
  CHECK_MESSAGE(sm3::alter_column_type(
                    f.s, e, "v", {sm2::type_varchar.code, 24}, nullptr) == 0,
                sm1::errmsg());
  CHECK(sm3::alter_column_type(f.s, e, "v", {sm2::type_varchar.code, 9},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::alter_column_type(f.s, e, "d",
                               {sm2::type_decimal64.code, dec(10, 4)},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::abort(f.s);
}

TEST_CASE("a column something would read stale keeps its type") {
  // Verify the refusals (ALTERTYPE.md §8, question 3).
  // 1. A partial index reading c: SM_ERR_INUSE.
  // 2. A foreign key of p's naming p's column, and one referencing
  //    t's a: SM_ERR_INUSE.
  // 3. A view reading b: SM_ERR_INUSE.
  // 4. A column not there: SM_ERR_NOTFOUND.
  alt_t f;
  std::vector<int32_t> reads_c = {2};
  std::vector<sm3::index_key_spec_t> by_b = {{1, false, false, std::nullopt}};
  uint64_t part = 0;
  f.begin();
  REQUIRE_MESSAGE(sm3::create_index(f.s, f.t, "t_part", by_b, {}, false,
                                    "c > 0", reads_c, part) == 0,
                  sm1::errmsg());
  CHECK(sm3::alter_column_type(f.s, f.t, "c", {sm2::type_int8.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  sm3::abort(f.s);

  std::vector<sm3::column_spec_t> pcols = {
      {"k", sm2::type_int4.code, -1, false}};
  uint64_t p = 0, fk = 0;
  f.begin();
  REQUIRE(sm3::create_table(f.s, sm3::nsp_public, "p", pcols, p) == 0);
  sm3::fkey_rec_t def;
  def.name = "p_k";
  def.ref_table = f.t;
  def.ref_index = f.pkey;
  def.attnums = {0};
  def.ref_attnums = {0};
  REQUIRE_MESSAGE(sm3::create_fkey(f.s, p, def, fk) == 0, sm1::errmsg());
  CHECK(sm3::alter_column_type(f.s, p, "k", {sm2::type_int8.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm3::alter_column_type(f.s, f.t, "a", {sm2::type_int8.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  sm3::abort(f.s);

  sm3::view_def_t view;
  view.name = "v";
  view.query = "the query";
  view.reads = {{f.t, 1}};
  uint64_t v = 0;
  f.begin();
  REQUIRE_MESSAGE(sm3::create_view(f.s, sm3::nsp_public, "v", view, v) == 0,
                  sm1::errmsg());
  CHECK(sm3::alter_column_type(f.s, f.t, "b", {sm2::type_varchar.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm3::alter_column_type(f.s, f.t, "nope", {sm2::type_int8.code, -1},
                               nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::abort(f.s);
}

TEST_CASE("a column's type changed drops its statistics alone") {
  // Verify the stale statistics go (ALTERTYPE.md §5).
  // 1. Record statistics of b and of c.
  // 2. b goes from text to varchar, the record alone.
  // 3. c's statistics are there, b's are not.
  alt_t f;
  f.put(1, "x", 0);
  sm3::stats_t stats;
  stats.target = 100;
  sm3::stat_t sb, sc;
  sb.position = 1;
  sc.position = 2;
  stats.stats = {sb, sc};
  f.begin();
  REQUIRE_MESSAGE(sm3::cat_put_stats(f.s, f.t, stats) == 0, sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  f.begin();
  REQUIRE_MESSAGE(sm3::alter_column_type(f.s, f.t, "b",
                                         {sm2::type_varchar.code, -1},
                                         nullptr) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.s) == 0);
  f.begin();
  std::shared_ptr<const sm3::stats_t> got;
  REQUIRE(sm3::cat_get_stats(f.s, f.t, got) == 0);
  REQUIRE(got);
  REQUIRE(got->stats.size() == 1);
  CHECK(got->stats[0].position == 2);
  REQUIRE(sm3::commit(f.s) == 0);
}
