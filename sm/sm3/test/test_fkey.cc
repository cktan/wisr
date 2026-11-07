/*
 * test_fkey.cc -- foreign keys in the catalog (FOREIGNKEY.md at the
 * top level): kept, loaded back with their parents' referenced_by
 * derived, and kept true by the DDL on the tables they join.
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
class fk_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  fk_db_t() {
    s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
  }

  /* Close the session and the catalog. */
  ~fk_db_t() { sm3::session_close(ss); }

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

/* The oids of a parent p, its unique index on id, and a child c. */
struct pair_t {
  uint64_t p = 0, p_id = 0, c = 0;
};

/*
 * Make p (id int4 NOT NULL, x text) with the unique index p_id on id,
 * and c (a int4, pid int4), in the open transaction: an index on a
 * table its transaction made is valid at once.
 */
pair_t make_pair(fk_db_t &db) {
  pair_t t;
  std::vector<sm3::column_spec_t> pcols = {
      {"id", sm2::type_int4.code, -1, true},
      {"x", sm2::type_text.code, -1, false}};
  std::vector<sm3::column_spec_t> ccols = {
      {"a", sm2::type_int4.code, -1, false},
      {"pid", sm2::type_int4.code, -1, false}};
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "p", pcols, t.p) == 0);
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t.p, "p_id", keys, {}, true, t.p_id) == 0);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "c", ccols, t.c) == 0);
  return t;
}

/* A foreign key c(pid) -> p(id) named name, by p_id. */
sm3::fkey_rec_t c_to_p(const pair_t &t, const std::string &name = "c_pid") {
  sm3::fkey_rec_t fk;
  fk.name = name;
  fk.ref_table = t.p;
  fk.ref_index = t.p_id;
  fk.attnums = {1};
  fk.ref_attnums = {0};
  fk.on_delete = 'c';
  fk.match = 'f';
  fk.deferrable = true;
  fk.host = "operators";
  return fk;
}

} // namespace

TEST_CASE("a foreign key is kept, and loads back with its parent's referrer") {
  // Verify create_fkey and the load (FOREIGNKEY.md §3, §4).
  // 1. Make p, its unique index, and c; add c(pid) -> p(id), ON DELETE
  //    CASCADE, MATCH FULL, deferrable, with a host blob.
  // 2. Commit and restart: c's definition has the foreign key, every
  //    field as given; p's referenced_by names it, derived on load.
  // 3. cat_get_fkey finds it by its oid alone, with its child.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  uint64_t fk = 0;
  REQUIRE_MESSAGE(sm3::create_fkey(db.ss, t.c, c_to_p(t), fk) == 0,
                  sm1::errmsg());
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto c = sm3::cat_get_table(cat, t.c);
  auto p = sm3::cat_get_table(cat, t.p);
  REQUIRE(c != nullptr);
  REQUIRE(p != nullptr);
  REQUIRE(c->rec.fkeys.size() == 1);
  const sm3::fkey_rec_t &k = c->rec.fkeys[0];
  CHECK(k.oid == fk);
  CHECK(k.name == "c_pid");
  CHECK(k.ref_table == t.p);
  CHECK(k.ref_index == t.p_id);
  CHECK(k.attnums == std::vector<int32_t>{1});
  CHECK(k.ref_attnums == std::vector<int32_t>{0});
  CHECK(k.on_update == 'a');
  CHECK(k.on_delete == 'c');
  CHECK(k.match == 'f');
  CHECK(k.deferrable);
  CHECK_FALSE(k.deferred);
  CHECK(k.validated);
  CHECK(k.host == "operators");
  CHECK(p->rec.referenced_by == std::vector<sm3::fkey_ref_t>{{t.c, fk}});
  CHECK(c->rec.referenced_by.empty());
  uint64_t child = 0;
  sm3::fkey_rec_t got;
  REQUIRE(sm3::cat_get_fkey(db.ss, fk, child, got) == 0);
  CHECK(child == t.c);
  CHECK(got == k);
  CHECK(sm3::cat_get_fkey(db.ss, fk + 1000, child, got) == 1);
}

TEST_CASE("create_fkey refuses a foreign key that does not fit its tables") {
  // Verify create_fkey's checks (FOREIGNKEY.md §5).
  // 1. A name already a foreign key of c's is SM_ERR_DUPKEY.
  // 2. Each of these is SM_ERR_INVALID: no pairs, unequal ends, a
  //    column out of range, an unknown action, an index not p's, a
  //    non-unique index, a key that is not the index's, and a
  //    temporary child of a permanent parent.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  uint64_t fk = 0, ix = 0, tmp = 0;
  REQUIRE(sm3::create_fkey(db.ss, t.c, c_to_p(t), fk) == 0);
  CHECK(sm3::create_fkey(db.ss, t.c, c_to_p(t), fk) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  std::vector<sm3::index_key_spec_t> keys = {{1, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t.p, "p_x", keys, {}, false, ix) == 0);
  std::vector<sm3::column_spec_t> cols = {
      {"pid", sm2::type_int4.code, -1, false}};
  REQUIRE(sm3::create_temp_table(db.ss, "tc", cols, tmp) == 0);

  auto refused = [&](uint64_t child, sm3::fkey_rec_t k) {
    k.name = "other";
    CHECK(sm3::create_fkey(db.ss, child, k, fk) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  };
  sm3::fkey_rec_t k = c_to_p(t);
  k.attnums = {};
  k.ref_attnums = {};
  refused(t.c, k);
  k = c_to_p(t);
  k.ref_attnums = {0, 1};
  refused(t.c, k);
  k = c_to_p(t);
  k.attnums = {5};
  refused(t.c, k);
  k = c_to_p(t);
  k.on_update = 'z';
  refused(t.c, k);
  k = c_to_p(t);
  k.ref_index = t.p_id + 1000;
  refused(t.c, k);
  k = c_to_p(t);
  k.ref_index = ix;
  k.ref_attnums = {1};
  refused(t.c, k);
  k = c_to_p(t);
  k.ref_attnums = {1};
  refused(t.c, k);
  k = c_to_p(t);
  k.attnums = {0};
  refused(tmp, k);
  db.commit();
}

TEST_CASE("a foreign key's tables, index and columns cannot be dropped") {
  // Verify the DDL that keeps a foreign key true (FOREIGNKEY.md §5).
  // 1. With c(pid) -> p(id): dropping p, p_id or c.pid is SM_ERR_INUSE;
  //    p.id is refused as p_id's key.
  // 2. Dropping c takes the foreign key out of p's referenced_by.
  // 3. Then p, its index and its column can go.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  uint64_t fk = 0;
  REQUIRE(sm3::create_fkey(db.ss, t.c, c_to_p(t), fk) == 0);
  db.commit();

  db.begin();
  CHECK(sm3::drop_table(db.ss, t.p) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  db.commit();
  db.begin();
  CHECK(sm3::drop_index(db.ss, t.p, t.p_id) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  db.commit();
  db.begin();
  // p.id is a key of p_id, whose check refuses it first.
  CHECK(sm3::drop_column(db.ss, t.p, "id") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();
  db.begin();
  CHECK(sm3::drop_column(db.ss, t.c, "pid") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  db.commit();

  db.begin();
  REQUIRE(sm3::drop_table(db.ss, t.c) == 0);
  db.commit();
  auto p = sm3::cat_get_table(db.cat, t.p);
  REQUIRE(p != nullptr);
  CHECK(p->rec.referenced_by.empty());
  db.begin();
  REQUIRE(sm3::drop_index(db.ss, t.p, t.p_id) == 0);
  REQUIRE(sm3::drop_column(db.ss, t.p, "id") == 0);
  REQUIRE(sm3::drop_table(db.ss, t.p) == 0);
  db.commit();
}

TEST_CASE("a foreign key is dropped, validated and renamed") {
  // Verify drop_fkey, validate_fkey and rename_fkey.
  // 1. Add c_pid NOT VALID, and c_pid2 over the same columns.
  // 2. Renaming c_pid to c_pid2 is SM_ERR_DUPKEY; to c_fk it is done.
  // 3. Validating c_fk marks it validated, after a restart too.
  // 4. Dropping c_pid2 leaves p one referrer; dropping c_fk, none.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  uint64_t f1 = 0, f2 = 0;
  sm3::fkey_rec_t k = c_to_p(t);
  k.validated = false;
  REQUIRE(sm3::create_fkey(db.ss, t.c, k, f1) == 0);
  REQUIRE(sm3::create_fkey(db.ss, t.c, c_to_p(t, "c_pid2"), f2) == 0);
  CHECK(sm3::rename_fkey(db.ss, t.c, f1, "c_pid2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::rename_fkey(db.ss, t.c, f1, "c_fk") == 0);
  REQUIRE(sm3::validate_fkey(db.ss, t.c, f1) == 0);
  db.commit();

  db.reload();
  auto c = sm3::cat_get_table(db.cat, t.c);
  REQUIRE(c->rec.fkeys.size() == 2);
  CHECK(c->rec.fkeys[0].name == "c_fk");
  CHECK(c->rec.fkeys[0].validated);
  db.begin();
  REQUIRE(sm3::drop_fkey(db.ss, t.c, f2) == 0);
  db.commit();
  CHECK(sm3::cat_get_table(db.cat, t.p)->rec.referenced_by ==
        std::vector<sm3::fkey_ref_t>{{t.c, f1}});
  db.begin();
  REQUIRE(sm3::drop_fkey(db.ss, t.c, f1) == 0);
  CHECK(sm3::drop_fkey(db.ss, t.c, f1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();
  CHECK(sm3::cat_get_table(db.cat, t.p)->rec.referenced_by.empty());
  CHECK(sm3::cat_get_table(db.cat, t.c)->rec.fkeys.empty());
}

TEST_CASE("validating a foreign key does not wait for the child's writers") {
  // Verify validate_fkey takes the child's lock IX, publishes the flag
  // into the newest definition, and loses no other validation.
  // 1. Add two NOT VALID keys f1 and f2 on c, and commit.
  // 2. A second session holds c's lock IX, as a writer does.
  // 3. The first validates f1 with no wait; the second validates f2
  //    through its own transaction.
  // 4. Both commit: both keys are validated in memory, and after a
  //    restart.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  uint64_t f1 = 0, f2 = 0;
  sm3::fkey_rec_t k = c_to_p(t);
  k.validated = false;
  REQUIRE(sm3::create_fkey(db.ss, t.c, k, f1) == 0);
  k.name = "c_pid2";
  REQUIRE(sm3::create_fkey(db.ss, t.c, k, f2) == 0);
  db.commit();

  sm1::session_t *s2 = db.open();
  sm3::session_t *ss2 = nullptr;
  REQUIRE(sm3::session_open(db.cat, s2, ss2) == 0);
  REQUIRE(sm3::begin(ss2, sm1::xctlevel_t::read_committed) == 0);
  std::shared_ptr<const sm3::table_def_t> d;
  REQUIRE(sm3::cat_lock_table(ss2, t.c, sm1::SM_LOCKMODE_IX, d) == 0);

  db.begin();
  uint64_t v0 = sm3::cat_table_version(db.ss, t.c);
  REQUIRE_MESSAGE(sm3::validate_fkey(db.ss, t.c, f1) == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm3::validate_fkey(ss2, t.c, f2) == 0, sm1::errmsg());
  CHECK(sm3::validate_fkey(db.ss, t.c, 999999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();
  REQUIRE_MESSAGE(sm3::commit(ss2) == 0, sm1::errmsg());
  sm3::session_close(ss2);

  auto c = sm3::cat_get_table(db.cat, t.c);
  REQUIRE(c->rec.fkeys.size() == 2);
  CHECK(c->rec.fkeys[0].validated);
  CHECK(c->rec.fkeys[1].validated);
  CHECK(sm3::cat_table_version(db.ss, t.c) != v0);

  db.reload();
  c = sm3::cat_get_table(db.cat, t.c);
  REQUIRE(c->rec.fkeys.size() == 2);
  CHECK(c->rec.fkeys[0].validated);
  CHECK(c->rec.fkeys[1].validated);
}

TEST_CASE("a table's foreign key to itself goes with the table") {
  // Verify a self-referencing foreign key (FOREIGNKEY.md §5).
  // 1. Make p, its unique index, and a column boss; add p(boss) ->
  //    p(id).
  // 2. p is its own referrer, after a restart too; dropping p is not
  //    refused, and leaves nothing.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  REQUIRE(sm3::add_column(db.ss, t.p, {"boss", sm2::type_int4.code, -1, false},
                          std::nullopt) == 0);
  sm3::fkey_rec_t k = c_to_p(t, "p_boss");
  k.attnums = {2};
  uint64_t fk = 0;
  REQUIRE_MESSAGE(sm3::create_fkey(db.ss, t.p, k, fk) == 0, sm1::errmsg());
  db.commit();
  db.reload();
  auto p = sm3::cat_get_table(db.cat, t.p);
  CHECK(p->rec.referenced_by == std::vector<sm3::fkey_ref_t>{{t.p, fk}});
  db.begin();
  REQUIRE_MESSAGE(sm3::drop_table(db.ss, t.p) == 0, sm1::errmsg());
  db.commit();
  uint64_t child = 0;
  sm3::fkey_rec_t got;
  CHECK(sm3::cat_get_fkey(db.ss, fk, child, got) == 1);
}

TEST_CASE("adding a foreign key moves the parent's version") {
  // Verify both definitions are published (FOREIGNKEY.md §4).
  // 1. Commit p, p_id and c, and note p's version.
  // 2. Add c(pid) -> p(id) and commit: p's version is greater.
  fk_db_t db;
  db.begin();
  pair_t t = make_pair(db);
  db.commit();
  uint64_t before = sm3::cat_table_version(db.ss, t.p);
  db.begin();
  uint64_t fk = 0;
  REQUIRE(sm3::create_fkey(db.ss, t.c, c_to_p(t), fk) == 0);
  db.commit();
  CHECK(sm3::cat_table_version(db.ss, t.p) > before);
}
