/*
 * test_check.cc -- check constraints in the catalog (CHECK.md at the
 * top level): kept with the columns they read and the host's bytes,
 * loaded back, and kept true by the DDL on those columns.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <string>
#include <vector>

namespace {

/*
 * A catalog with cat_init committed, on a fresh database, loaded,
 * with an sm3 session ss over the sm1 session s.
 */
class check_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  check_db_t() {
    s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
  }

  /* Close the session and the catalog. */
  ~check_db_t() { sm3::session_close(ss); }

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
  /* Load the catalog; open ss over s. */
  void load() {
    REQUIRE_MESSAGE(sm3::cat_open(env(), "db", cat) == 0, sm1::errmsg());
    REQUIRE(sm3::session_open(cat, s, ss) == 0);
  }
};

/* The columns of t: a int4, b int4, c text. */
const std::vector<sm3::column_spec_t> t_cols = {
    {"a", sm2::type_int4.code, -1, false},
    {"b", sm2::type_int4.code, -1, false},
    {"c", sm2::type_text.code, -1, false}};

/* Make t in the open transaction; its oid. */
uint64_t make_t(check_db_t &db) {
  uint64_t t = 0;
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", t_cols, t) == 0);
  return t;
}

/* A check named name that reads the columns reads, with a host blob. */
sm3::check_rec_t check_of(const std::string &name, std::vector<int32_t> reads) {
  sm3::check_rec_t c;
  c.name = name;
  c.reads = std::move(reads);
  c.host = "expr:" + name;
  return c;
}

} // namespace

TEST_CASE("a check is kept, and loads back with its columns and bytes") {
  // Verify create_check and the load (CHECK.md §3, §4).
  // 1. Make t; add t_ac reading a and c, NOT VALID, and t_b reading b.
  // 2. Commit and restart: t's definition has both, every field as
  //    given, in the order of their oids.
  // 3. Add a check that reads no column: it is kept with none.
  check_db_t db;
  db.begin();
  uint64_t t = make_t(db), ac = 0, b = 0;
  sm3::check_rec_t c1 = check_of("t_ac", {0, 2});
  c1.validated = false;
  REQUIRE_MESSAGE(sm3::create_check(db.ss, t, c1, ac) == 0, sm1::errmsg());
  REQUIRE(sm3::create_check(db.ss, t, check_of("t_b", {1}), b) == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->rec.checks.size() == 2);
  const sm3::check_rec_t &k = d->rec.checks[0];
  CHECK(k.oid == ac);
  CHECK(k.name == "t_ac");
  CHECK(k.reads == std::vector<int32_t>{0, 2});
  CHECK_FALSE(k.validated);
  CHECK(k.host == "expr:t_ac");
  CHECK(d->rec.checks[1].oid == b);
  CHECK(d->rec.checks[1].validated);

  db.begin();
  uint64_t none = 0;
  REQUIRE(sm3::create_check(db.ss, t, check_of("t_true", {}), none) == 0);
  db.commit();
  d = sm3::cat_get_table(db.reload(), t);
  REQUIRE(d->rec.checks.size() == 3);
  CHECK(d->rec.checks[2].reads.empty());
}

TEST_CASE("create_check refuses a column not there and a name taken") {
  // Verify the refusals of create_check and rename_check (CHECK.md §5.1).
  // 1. A check reading a column t has not is SM_ERR_INVALID; one with
  //    no name too.
  // 2. A name another check of t has is SM_ERR_DUPKEY, at create and
  //    at rename.
  // 3. A name a foreign key of t has is SM_ERR_DUPKEY, and a foreign
  //    key cannot take a check's name.
  // 4. A unique constraint's index cannot take a check's name; a plain
  //    unique index can.
  check_db_t db;
  db.begin();
  uint64_t t = make_t(db), oid = 0, k1 = 0, k2 = 0;
  CHECK(sm3::create_check(db.ss, t, check_of("bad", {7}), oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_check(db.ss, t, check_of("", {0}), oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::create_check(db.ss, t, check_of("k1", {0}), k1) == 0);
  CHECK(sm3::create_check(db.ss, t, check_of("k1", {1}), oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::create_check(db.ss, t, check_of("k2", {1}), k2) == 0);
  CHECK(sm3::rename_check(db.ss, t, k2, "k1") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);

  // t as a parent: a unique index on a, and a foreign key of t to it.
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  uint64_t t_a = 0, fk = 0;
  REQUIRE(sm3::create_index(db.ss, t, "t_a", keys, {}, true, t_a) == 0);
  sm3::fkey_rec_t f;
  f.name = "t_fk";
  f.ref_table = t;
  f.ref_index = t_a;
  f.attnums = {1};
  f.ref_attnums = {0};
  REQUIRE_MESSAGE(sm3::create_fkey(db.ss, t, f, fk) == 0, sm1::errmsg());
  CHECK(sm3::create_check(db.ss, t, check_of("t_fk", {0}), oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  f.name = "k1";
  CHECK(sm3::create_fkey(db.ss, t, f, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);

  // A unique constraint's index cannot take a check's name; a plain
  // unique index, no constraint, can.
  CHECK(sm3::create_index(db.ss, t, "k1", keys, {}, true, oid, false, true) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_index(db.ss, t, "k1", keys, {}, true, oid) == 0);
  db.commit();
}

TEST_CASE("a column a check reads cannot be dropped or retyped") {
  // Verify the DDL that keeps a check true (CHECK.md §5.1).
  // 1. With t_a reading a: dropping a is SM_ERR_INUSE, and column_use
  //    names the check, so ALTER COLUMN TYPE refuses it.
  // 2. b, which no check reads, is dropped.
  // 3. Once t_a is dropped, a is dropped too.
  check_db_t db;
  db.begin();
  uint64_t t = make_t(db), k = 0;
  REQUIRE(sm3::create_check(db.ss, t, check_of("t_a", {0}), k) == 0);
  db.commit();

  db.begin();
  CHECK(sm3::drop_column(db.ss, t, "a") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  sm3::column_use_t use = sm3::column_use_t::none;
  std::string by;
  REQUIRE(sm3::column_use(db.ss, t, 0, use, by) == 0);
  CHECK(use == sm3::column_use_t::check);
  CHECK(by == "t_a");
  REQUIRE(sm3::column_use(db.ss, t, 1, use, by) == 0);
  CHECK(use == sm3::column_use_t::none);
  REQUIRE(sm3::drop_column(db.ss, t, "b") == 0);
  db.commit();

  db.begin();
  REQUIRE(sm3::drop_check(db.ss, t, k) == 0);
  REQUIRE(sm3::drop_column(db.ss, t, "a") == 0);
  db.commit();
  CHECK(sm3::cat_get_table(db.reload(), t)->rec.checks.empty());
}

TEST_CASE(
    "a check is validated, renamed and dropped, and goes with its table") {
  // Verify validate_check, rename_check, drop_check and drop_table.
  // 1. Add t_a NOT VALID; validate it and rename it t_pos: after a
  //    restart it is t_pos, validated.
  // 2. Drop it: a second drop is SM_ERR_NOTFOUND.
  // 3. Add another, drop t: the catalog loads after a restart, so no
  //    entry of t's checks is left.
  check_db_t db;
  db.begin();
  uint64_t t = make_t(db), k = 0;
  sm3::check_rec_t c = check_of("t_a", {0});
  c.validated = false;
  REQUIRE(sm3::create_check(db.ss, t, c, k) == 0);
  db.commit();
  db.begin();
  REQUIRE(sm3::validate_check(db.ss, t, k) == 0);
  REQUIRE(sm3::rename_check(db.ss, t, k, "t_pos") == 0);
  db.commit();
  auto d = sm3::cat_get_table(db.reload(), t);
  REQUIRE(d->rec.checks.size() == 1);
  CHECK(d->rec.checks[0].name == "t_pos");
  CHECK(d->rec.checks[0].validated);

  db.begin();
  REQUIRE(sm3::drop_check(db.ss, t, k) == 0);
  CHECK(sm3::drop_check(db.ss, t, k) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::create_check(db.ss, t, check_of("t_b", {1}), k) == 0);
  db.commit();
  db.begin();
  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  db.commit();
  sm3::catalog_t *cat = db.reload();
  CHECK(sm3::cat_get_table(cat, t) == nullptr);
}

TEST_CASE("a check made in an aborted transaction leaves nothing") {
  // Verify a check is a catalog change of its transaction.
  // 1. Make t, and commit.
  // 2. Add t_a, then abort: t has no check, after a restart too.
  check_db_t db;
  db.begin();
  uint64_t t = make_t(db), k = 0;
  db.commit();
  db.begin();
  REQUIRE(sm3::create_check(db.ss, t, check_of("t_a", {0}), k) == 0);
  REQUIRE(sm3::abort(db.ss) == 0);
  CHECK(sm3::cat_get_table(db.cat, t)->rec.checks.empty());
  CHECK(sm3::cat_get_table(db.reload(), t)->rec.checks.empty());
}

TEST_CASE("a temporary table's check is kept for its session") {
  // Verify a temporary table keeps its checks in the session's catalog.
  // 1. Make temporary u, add u_a reading a, and commit.
  // 2. The session's definition of u has it.
  check_db_t db;
  db.begin();
  uint64_t u = 0, k = 0;
  REQUIRE(sm3::create_temp_table(db.ss, "u", t_cols, u) == 0);
  REQUIRE_MESSAGE(sm3::create_check(db.ss, u, check_of("u_a", {0}), k) == 0,
                  sm1::errmsg());
  db.commit();
  auto d = sm3::cat_get_table(db.ss, u);
  REQUIRE(d != nullptr);
  REQUIRE(d->rec.checks.size() == 1);
  CHECK(d->rec.checks[0].name == "u_a");
}
