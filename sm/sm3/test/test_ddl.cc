/*
 * test_ddl.cc -- the DDL beyond create_table (CATALOG.md §5):
 * each change written, then read back after a restart.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* v's little-endian bytes. */
template <typename T> std::string le(T v) {
  return std::string(reinterpret_cast<const char *>(&v), sizeof(T));
}

/*
 * A catalog with cat_init committed, on a fresh database, loaded,
 * with an sm3 session ss over the sm1 session s.
 */
class cat_db_t : public db_t {
public:
  /* Initialize the catalog of "db", load it, and open a session. */
  cat_db_t() {
    s = open();
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
  }

  /* Close the session and the catalog. */
  ~cat_db_t() { sm3::session_close(ss); }

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

/* Make the table t (a int4, b text) in public; its oid. */
uint64_t make_t(cat_db_t &db) {
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, true},
      {"b", sm2::type_text.code, -1, false}};
  uint64_t oid = 0;
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, oid) == 0);
  return oid;
}

} // namespace

TEST_CASE("columns, indexes and collations are added and load back") {
  // Verify add_column, create_collation, create_index.
  // 1. Add c int8 NOT NULL with missing 7, and d text with none.
  // 2. Create a non-deterministic collation, and a unique index on
  //    (b, under it, DESC NULLS FIRST) INCLUDE (a).
  // 3. After a restart, the columns hold their types and missing
  //    values; the index its oid, name, first relid, key collation,
  //    direction, null order and INCLUDE column, built for sm2.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ci = 0, ix = 0;
  REQUIRE(sm3::add_column(db.ss, t, {"c", sm2::type_int8.code, -1, true},
                          le(int64_t{7})) == 0);
  REQUIRE(sm3::add_column(db.ss, t, {"d", sm2::type_text.code, -1, false},
                          std::nullopt) == 0);
  REQUIRE_MESSAGE(sm3::create_collation(db.ss, sm3::nsp_public, "ci",
                                        "und-u-ks-level2", false, ci) == 0,
                  sm1::errmsg());
  std::vector<sm3::index_key_spec_t> keys = {{1, true, true, ci}};
  std::vector<int32_t> inc = {0};
  REQUIRE(sm3::create_index(db.ss, t, "t_b", keys, inc, true, ix) == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->columns.size() == 4);
  CHECK(d->columns[2].name == "c");
  CHECK(d->columns[2].not_null);
  CHECK(d->columns[2].missing == le(int64_t{7}));
  CHECK_FALSE(d->columns[3].missing);
  CHECK(d->desc.bad == SIZE_MAX);
  CHECK(d->next_relid == 3);
  REQUIRE(d->indexes.size() == 1);
  const sm3::index_def_t &x = d->indexes[0];
  CHECK(x.oid == ix);
  CHECK(x.name == "t_b");
  CHECK(x.relid == 2);
  CHECK(x.unique);
  CHECK(x.key_attnums == std::vector<int32_t>{1});
  CHECK(x.include_attnums == std::vector<int32_t>{0});
  REQUIRE(x.key.size() == 1);
  CHECK(x.key[0].type == &sm2::type_text);
  CHECK(x.key[0].collation != nullptr);
  CHECK(x.key[0].desc);
  CHECK(x.key[0].nulls_first);
  REQUIRE(x.include.size() == 1);
  CHECK(x.include[0].type == &sm2::type_int4);
}

TEST_CASE("the catalog stores a table's record and makes nothing else") {
  // Verify cat_put_table, cat_get_table_rec and cat_del_table work
  // on records alone.
  // 1. A record is put under a new oid, with a store id that names
  //    no store: the catalog takes it, since it makes and checks no
  //    store. The table is then found by name and by oid.
  // 2. cat_get_table_rec reads it back as it was put.
  // 3. Another oid with the same name is SM_ERR_DUPKEY, and a
  //    record in a namespace that is not there SM_ERR_NOTFOUND.
  // 4. Put again under a new name, it frees the old name and takes
  //    the new.
  // 5. cat_del_table erases it and its name; reading it after is
  //    SM_ERR_NOTFOUND.
  cat_db_t db;
  uint64_t oid = 0, other = 0;
  sm3::table_rec_t rec{
      sm3::nsp_public,        "raw", 424242, "no.such.sequence",
      sm3::first_index_relid, {},    {}};
  rec.columns.push_back(
      {"x", sm2::type_int4.code, -1, false, false, std::nullopt, std::nullopt});
  db.begin();
  REQUIRE(sm3::cat_new_oid(db.ss, oid) == 0);
  REQUIRE(sm3::cat_new_oid(db.ss, other) == 0);
  CHECK(other != oid);
  REQUIRE_MESSAGE(sm3::cat_put_table(db.ss, oid, rec) == 0, sm1::errmsg());
  auto d = sm3::cat_get_table(db.ss, sm3::nsp_public, "raw");
  REQUIRE(d != nullptr);
  CHECK(d->oid == oid);
  CHECK(d->storeid == 424242);
  CHECK(sm3::cat_get_table(db.ss, oid) == d);

  sm3::table_rec_t back;
  REQUIRE(sm3::cat_get_table_rec(db.ss, oid, back) == 0);
  CHECK(back.name == "raw");
  CHECK(back.storeid == 424242);
  CHECK(back.id_seq == "no.such.sequence");
  CHECK(back.columns.size() == 1);

  CHECK(sm3::cat_put_table(db.ss, other, rec) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::table_rec_t lost = rec;
  lost.nspoid = 999;
  CHECK(sm3::cat_put_table(db.ss, other, lost) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  back.name = "cooked";
  REQUIRE(sm3::cat_put_table(db.ss, oid, back) == 0);
  CHECK(sm3::cat_get_table(db.ss, sm3::nsp_public, "raw") == nullptr);
  d = sm3::cat_get_table(db.ss, sm3::nsp_public, "cooked");
  REQUIRE(d != nullptr);
  CHECK(d->oid == oid);

  REQUIRE(sm3::cat_del_table(db.ss, oid) == 0);
  CHECK(sm3::cat_get_table(db.ss, sm3::nsp_public, "cooked") == nullptr);
  CHECK(sm3::cat_get_table_rec(db.ss, oid, back) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();
}

TEST_CASE("a create that fails leaves no store behind") {
  // Verify create_table takes back what it made when the catalog
  // refuses the record.
  // 1. A table is created, then another of the same name, which is
  //    SM_ERR_DUPKEY, and one in a namespace that is not there,
  //    which is SM_ERR_NOTFOUND. The transaction goes on to commit.
  // 2. The database has one store more than before, not three.
  cat_db_t db;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  std::vector<uint64_t> before, after;
  uint64_t t = 0, u = 0;
  db.begin();
  REQUIRE(sm1::lsstore(db.s, before) == 0);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, t) == 0);
  CHECK(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, u) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::create_table(db.ss, 999, "v", cols, u) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();
  db.begin();
  REQUIRE(sm1::lsstore(db.s, after) == 0);
  CHECK(after.size() == before.size() + 1);
  db.commit();
}

TEST_CASE("a namespace is created like a table, and found by name") {
  // Verify create_namespace, cat_get_namespace and
  // cat_list_namespaces.
  // 1. The built-in namespaces are found by name at their fixed
  //    oids, and an unknown name is not found.
  // 2. A namespace created and aborted is not there afterwards.
  // 3. One made under a savepoint goes with a rollback to it; one
  //    made before it stays, and its transaction finds and lists
  //    it at once while another session does not until the commit.
  // 4. A table is created in it, and the same table name in public
  //    does not clash. The catalog's own listings, which need no
  //    session, show the namespace and its table only once the
  //    transaction has committed.
  // 5. A name already taken is SM_ERR_DUPKEY, and no name
  //    SM_ERR_INVALID.
  // 6. After a restart it is still found, with the same oid, and
  //    its table is in it.
  cat_db_t db;
  uint64_t oid = 0, app = 0, tmp = 0, t = 0, u = 0;
  db.begin();
  CHECK(sm3::cat_get_namespace(db.ss, "public", oid));
  CHECK(oid == sm3::nsp_public);
  CHECK(sm3::cat_get_namespace(db.ss, "pg_catalog", oid));
  CHECK(oid == sm3::nsp_pg_catalog);
  CHECK_FALSE(sm3::cat_get_namespace(db.ss, "app", oid));

  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  CHECK(sm3::cat_get_namespace(db.ss, "app", oid));
  REQUIRE(sm3::abort(db.ss) == 0);
  CHECK_FALSE(sm3::cat_get_namespace(db.ss, "app", oid));

  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  db.begin();
  REQUIRE_MESSAGE(sm3::create_namespace(db.ss, "app", app) == 0, sm1::errmsg());
  CHECK(app >= sm3::first_oid);
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE(sm3::create_namespace(db.ss, "tmp", tmp) == 0);
  CHECK(sm3::cat_get_namespace(db.ss, "tmp", oid));
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK_FALSE(sm3::cat_get_namespace(db.ss, "tmp", oid));
  CHECK(sm3::cat_get_namespace(db.ss, "app", oid));
  CHECK(oid == app);
  CHECK_FALSE(sm3::cat_get_namespace(other, "app", oid));
  using names_t = std::vector<std::string>;
  CHECK(sm3::cat_list_namespaces(db.ss) ==
        names_t{"app", "pg_catalog", "public"});
  CHECK(sm3::cat_list_namespaces(other) == names_t{"pg_catalog", "public"});
  CHECK(sm3::cat_list_namespaces(db.cat) == names_t{"pg_catalog", "public"});
  CHECK_FALSE(sm3::cat_get_namespace(db.cat, "app", oid));

  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  REQUIRE_MESSAGE(sm3::create_table(db.ss, app, "orders", cols, t) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "orders", cols, u) == 0);
  CHECK(t != u);
  CHECK(sm3::create_namespace(db.ss, "app", oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  db.commit();
  CHECK(sm3::cat_get_namespace(other, "app", oid));
  CHECK(oid == app);
  CHECK(sm3::cat_list_namespaces(other) ==
        names_t{"app", "pg_catalog", "public"});
  CHECK(sm3::cat_list_namespaces(db.cat) ==
        names_t{"app", "pg_catalog", "public"});
  CHECK(sm3::cat_get_namespace(db.cat, "app", oid));
  CHECK(oid == app);
  CHECK(sm3::cat_list_tables(db.cat, app) == names_t{"orders"});
  CHECK(sm3::cat_list_tables(db.cat, sm3::nsp_public) == names_t{"orders"});
  CHECK(sm3::cat_list_tables(db.cat, sm3::nsp_pg_catalog).empty());
  sm3::session_close(other);

  db.begin();
  CHECK(sm3::create_namespace(db.ss, "", oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_namespace(db.ss, "public", oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.reload();
  CHECK(sm3::cat_get_namespace(db.ss, "app", oid));
  CHECK(oid == app);
  auto d = sm3::cat_get_table(db.cat, app, "orders");
  REQUIRE(d != nullptr);
  CHECK(d->oid == t);
  CHECK(d->nspoid == app);
}

TEST_CASE("a namespace's name is found by its oid") {
  // Verify cat_get_namespace_name.
  // 1. The built-in namespaces are found by their fixed oids, and an
  //    unknown oid is not found.
  // 2. One created is found by its transaction at once, by another
  //    session only after the commit.
  // 3. Dropped, it is not found by its transaction.
  cat_db_t db;
  uint64_t app = 0;
  std::string name;
  db.begin();
  CHECK(sm3::cat_get_namespace_name(db.ss, sm3::nsp_public, name));
  CHECK(name == "public");
  CHECK(sm3::cat_get_namespace_name(db.ss, sm3::nsp_pg_catalog, name));
  CHECK(name == "pg_catalog");
  CHECK_FALSE(sm3::cat_get_namespace_name(db.ss, 999999, name));

  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  CHECK(sm3::cat_get_namespace_name(db.ss, app, name));
  CHECK(name == "app");
  CHECK_FALSE(sm3::cat_get_namespace_name(other, app, name));
  db.commit();
  CHECK(sm3::cat_get_namespace_name(other, app, name));
  CHECK(name == "app");

  db.begin();
  REQUIRE(sm3::drop_namespace(db.ss, app) == 0);
  CHECK_FALSE(sm3::cat_get_namespace_name(db.ss, app, name));
  db.commit();
  sm3::session_close(other);
}

TEST_CASE("a column's default is kept with the table, opaque") {
  // Verify a column spec's default_expr is stored and loaded as given.
  // 1. Create t with a default on one column of two, and commit.
  // 2. The loaded definition has it on that column and none on the other.
  // 3. After a restart it is still there, byte for byte.
  cat_db_t db;
  std::string expr("{CONST :consttype 23}\0x", 23);
  std::vector<sm3::column_spec_t> cols = {
      {"a", sm2::type_int4.code, -1, false, expr},
      {"b", sm2::type_int4.code, -1, false}};
  uint64_t t = 0;
  db.begin();
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, t) == 0);
  db.commit();
  db.begin();
  auto def = sm3::cat_get_table(db.ss, t);
  REQUIRE(def);
  CHECK(def->columns[0].default_expr == expr);
  CHECK_FALSE(def->columns[1].default_expr);
  db.commit();
  db.reload();
  db.begin();
  def = sm3::cat_get_table(db.ss, t);
  REQUIRE(def);
  CHECK(def->columns[0].default_expr == expr);
  CHECK_FALSE(def->columns[1].default_expr);
  db.commit();
}

TEST_CASE("no oid past last_oid is handed out") {
  // Verify the oids stop at last_oid, PostgreSQL's 32 bits, rather
  // than go on to one that would wrap onto a built-in oid.
  // 1. Make the oid sequence, sm3.oid, again at last_oid - 1.
  // 2. Two tables take last_oid - 1 and last_oid.
  // 3. A third fails with SM_ERR_INVALID, and so do a namespace and
  //    an oid taken plainly; the two tables remain.
  cat_db_t db;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t t1 = 0, t2 = 0, t3 = 0, n = 0, o = 0;
  db.begin();
  REQUIRE(sm1::rmseq(db.s, "sm3.oid") == 0);
  REQUIRE(sm1::mkseq(db.s, "sm3.oid", sm3::last_oid - 1, 1) == 0);
  db.commit();

  db.begin();
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t1", cols, t1) == 0);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t2", cols, t2) == 0);
  CHECK(t1 == sm3::last_oid - 1);
  CHECK(t2 == sm3::last_oid);
  db.commit();

  db.begin();
  CHECK(sm3::create_table(db.ss, sm3::nsp_public, "t3", cols, t3) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_namespace(db.ss, "app", n) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::cat_new_oid(db.ss, o) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  auto def = sm3::cat_get_table(db.ss, sm3::nsp_public, "t2");
  REQUIRE(def);
  CHECK(def->oid == t2);
  db.commit();
}

TEST_CASE("a namespace is dropped only when it is empty") {
  // Verify drop_namespace.
  // 1. The built-in namespaces are SM_ERR_INVALID to drop, and an
  //    unknown oid SM_ERR_NOTFOUND.
  // 2. A namespace holding a table is SM_ERR_INUSE; once the table
  //    is dropped, in the same transaction, it can be dropped.
  // 3. Dropped, it is not found or listed by its transaction, a
  //    table cannot be created in it, and its name can be taken
  //    again; a rollback to a savepoint brings it back.
  // 4. Another session still finds it until the drop commits, and
  //    after a restart it is gone.
  // 5. A namespace holding a collation is SM_ERR_INUSE, before and
  //    after a restart.
  cat_db_t db;
  uint64_t oid = 0, app = 0, lang = 0, t = 0, c = 0;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  db.begin();
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  REQUIRE(sm3::create_namespace(db.ss, "lang", lang) == 0);
  REQUIRE(sm3::create_table(db.ss, app, "orders", cols, t) == 0);
  REQUIRE_MESSAGE(
      sm3::create_collation(db.ss, lang, "plain", "en-US", true, c) == 0,
      sm1::errmsg());
  db.commit();

  db.begin();
  CHECK(sm3::drop_namespace(db.ss, sm3::nsp_public) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::drop_namespace(db.ss, sm3::nsp_pg_catalog) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::drop_namespace(db.ss, app + 999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::drop_namespace(db.ss, app) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  CHECK(sm3::drop_namespace(db.ss, lang) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);

  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE_MESSAGE(sm3::drop_namespace(db.ss, app) == 0, sm1::errmsg());
  using names_t = std::vector<std::string>;
  CHECK_FALSE(sm3::cat_get_namespace(db.ss, "app", oid));
  CHECK(sm3::cat_list_namespaces(db.ss) ==
        names_t{"lang", "pg_catalog", "public"});
  CHECK(sm3::create_table(db.ss, app, "late", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE_MESSAGE(sm3::create_namespace(db.ss, "app", oid) == 0, sm1::errmsg());
  CHECK(oid != app);
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK(sm3::cat_get_namespace(db.ss, "app", oid));
  CHECK(oid == app);

  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  REQUIRE(sm3::drop_namespace(db.ss, app) == 0);
  CHECK(sm3::cat_get_namespace(other, "app", oid));
  db.commit();
  CHECK_FALSE(sm3::cat_get_namespace(other, "app", oid));
  sm3::session_close(other);

  db.reload();
  CHECK(sm3::cat_list_namespaces(db.ss) ==
        names_t{"lang", "pg_catalog", "public"});
  db.begin();
  CHECK(sm3::drop_namespace(db.ss, lang) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);
}

TEST_CASE("columns, indexes and tables are dropped and renamed") {
  // Verify drop_column, drop_index, rename_table and
  // drop_table.
  // 1. A dropped column keeps its place, marked dropped; its name
  //    can be added again as a new column.
  // 2. A dropped index frees its name, and one made after it takes
  //    a new relid, never the old one.
  // 3. A renamed table is found by its new name only.
  // 4. A dropped table is gone, with its store and its rowno
  //    sequence, and its name can be taken again.
  // 5. A drop that aborts leaves the table as it was.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ix = 0, ix2 = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "t_a", keys, {}, false, ix) == 0);
  REQUIRE(sm3::drop_column(db.ss, t, "b") == 0);
  REQUIRE(sm3::add_column(db.ss, t, {"b", sm2::type_int8.code, -1, false},
                          std::nullopt) == 0);
  REQUIRE(sm3::drop_index(db.ss, t, ix) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "t_a", keys, {}, false, ix2) == 0);
  REQUIRE(sm3::rename_table(db.ss, t, "t2") == 0);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, sm3::nsp_public, "t2");
  REQUIRE(d != nullptr);
  CHECK(sm3::cat_get_table(cat, sm3::nsp_public, "t") == nullptr);
  REQUIRE(d->columns.size() == 3);
  CHECK(d->columns[1].dropped);
  CHECK(d->columns[2].name == "b");
  CHECK(d->columns[2].type == &sm2::type_int8);
  REQUIRE(d->indexes.size() == 1);
  CHECK(d->indexes[0].oid == ix2);
  CHECK(d->indexes[0].relid == 3);
  uint64_t storeid = d->storeid;
  std::string seq = d->id_seq;

  db.begin();
  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  db.commit();
  cat = db.reload();
  CHECK(sm3::cat_get_table(cat, t) == nullptr);
  db.begin();
  std::vector<uint64_t> stores;
  REQUIRE(sm1::lsstore(db.s, stores) == 0);
  CHECK(std::find(stores.begin(), stores.end(), storeid) == stores.end());
  uint64_t v;
  CHECK(sm1::nextseq(db.s, seq, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  // The name is free again: make_t requires its create to succeed.
  make_t(db);
  db.commit();
}

TEST_CASE("dropping an index erases its entries and nothing else") {
  // Verify drop_index clears the index's relid.
  // 1. Put a row under the heap's relid and entries under the
  //    relids of two indexes in the table's store.
  // 2. Drop the first index: its entries are gone; the row and the
  //    other index's entry remain.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), i1 = 0, i2 = 0;
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "i1", keys, {}, false, i1) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "i2", keys, {}, false, i2) == 0);
  db.commit();
  sm3::catalog_t *cat = db.reload();
  uint64_t storeid = sm3::cat_get_table(cat, t)->storeid;

  db.begin();
  auto key = [](uint16_t relid, char tail) {
    return std::string{static_cast<char>(relid >> 8),
                       static_cast<char>(relid & 0xFF), tail};
  };
  REQUIRE(sm1::insert(db.s, storeid, key(sm3::heap_relid, 'r'), "row") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(2, 'a'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(2, 'b'), "e") == 0);
  REQUIRE(sm1::insert(db.s, storeid, key(3, 'a'), "e") == 0);
  REQUIRE(sm3::drop_index(db.ss, t, i1) == 0);
  std::string_view v;
  CHECK(sm1::read(db.s, storeid, key(sm3::heap_relid, 'r'), v) == 0);
  CHECK(sm1::read(db.s, storeid, key(2, 'a'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(2, 'b'), v) == 1);
  CHECK(sm1::read(db.s, storeid, key(3, 'a'), v) == 0);
  db.commit();
}

TEST_CASE("a column is renamed, and its index goes on using it") {
  // Verify rename_column.
  // 1. b is renamed c: the index on b is still on place 1.
  // 2. A rename to a live column's name is SM_ERR_DUPKEY; of an
  //    unknown or a dropped column, SM_ERR_NOTFOUND.
  // 3. A rename that aborts leaves the old name.
  // 4. After a restart t has a and c, and its index is on c.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ix = 0;
  std::vector<sm3::index_key_spec_t> on_b = {{1, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "t_b", on_b, {}, false, ix) == 0);
  REQUIRE(sm3::rename_column(db.ss, t, "b", "c") == 0);
  db.commit();

  db.begin();
  CHECK(sm3::rename_column(db.ss, t, "c", "a") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::rename_column(db.ss, t, "b", "d") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::add_column(db.ss, t, {"e", sm2::type_int8.code, -1, false},
                          std::nullopt) == 0);
  REQUIRE(sm3::drop_column(db.ss, t, "e") == 0);
  CHECK(sm3::rename_column(db.ss, t, "e", "f") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  REQUIRE(sm3::rename_column(db.ss, t, "c", "x") == 0);
  REQUIRE(sm3::abort(db.ss) == 0);

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->columns.size() == 2);
  CHECK(d->columns[0].name == "a");
  CHECK(d->columns[1].name == "c");
  REQUIRE(d->indexes.size() == 1);
  REQUIRE(d->indexes[0].key_attnums.size() == 1);
  CHECK(d->indexes[0].key_attnums[0] == 1);
}

TEST_CASE("a primary key is marked, one a table, on NOT NULL columns") {
  // Verify create_index's primary mark and its rules.
  // 1. A primary key on a, which is NOT NULL, and a plain index on b.
  // 2. A second primary key, one on b, which may be null, and one not
  //    unique are SM_ERR_INVALID.
  // 3. After a restart the primary key keeps its mark, and the plain
  //    index has none.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), pk = 0, ib = 0, other = 0;
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_b = {{1, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "t_pkey", on_a, {}, true, pk, true) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "t_b", on_b, {}, false, ib) == 0);

  CHECK(sm3::create_index(db.ss, t, "t_pk2", on_a, {}, true, other, true) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_index(db.ss, t, "t_pk3", on_b, {}, true, other, true) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_index(db.ss, t, "t_pk4", on_a, {}, false, other, true) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->indexes.size() == 2);
  CHECK(d->indexes[0].oid == pk);
  CHECK(d->indexes[0].primary);
  CHECK(d->indexes[1].oid == ib);
  CHECK_FALSE(d->indexes[1].primary);
}

TEST_CASE("a constraint's index is marked, unique and on columns") {
  // Verify create_index's constraint mark and its rule.
  // 1. A unique index a UNIQUE constraint made on b, and one CREATE
  //    UNIQUE INDEX made on a.
  // 2. A constraint's index that is not unique is SM_ERR_INVALID.
  // 3. After a restart the constraint's index keeps its mark, and the
  //    other has none.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ub = 0, ua = 0, other = 0;
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_b = {{1, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "t_b_key", on_b, {}, true, ub, false,
                            true) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "t_a", on_a, {}, true, ua) == 0);

  CHECK(sm3::create_index(db.ss, t, "t_b_key2", on_b, {}, false, other, false,
                          true) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();

  sm3::catalog_t *cat = db.reload();
  auto d = sm3::cat_get_table(cat, t);
  REQUIRE(d != nullptr);
  REQUIRE(d->indexes.size() == 2);
  CHECK(d->indexes[0].oid == ub);
  CHECK(d->indexes[0].constraint);
  CHECK_FALSE(d->indexes[0].primary);
  CHECK(d->indexes[1].oid == ua);
  CHECK_FALSE(d->indexes[1].constraint);
}

TEST_CASE("the DDL refuses what the catalog cannot hold") {
  // Verify each call's errors.
  // 1. add column: a live name again is SM_ERR_DUPKEY; a missing
  //    value of the wrong width or not a varlena SM_ERR_INVALID; an
  //    unknown table SM_ERR_NOTFOUND.
  // 2. drop column: an unknown one is SM_ERR_NOTFOUND; one an index
  //    uses is SM_ERR_INVALID.
  // 3. create index: a dropped or unknown column, and a collation
  //    on an int4 column, are SM_ERR_INVALID; an unknown collation
  //    is SM_ERR_NOTFOUND; a taken name is SM_ERR_DUPKEY.
  // 4. drop index and drop table of an unknown oid, and a rename to
  //    a taken name, fail as SM_ERR_NOTFOUND and SM_ERR_DUPKEY.
  // 5. create collation: a locale ICU lacks is refused.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ix = 0, other = 0;
  using col = sm3::column_spec_t;
  CHECK(sm3::add_column(db.ss, t, col{"a", sm2::type_int8.code, -1, false},
                        std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::add_column(db.ss, t, col{"x", sm2::type_int8.code, -1, false},
                        le(int32_t{1})) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::add_column(db.ss, t, col{"x", sm2::type_text.code, -1, false},
                        std::string_view("\x09"
                                         "ab")) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::add_column(db.ss, 999, col{"x", sm2::type_int8.code, -1, false},
                        std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  REQUIRE(sm3::create_index(db.ss, t, "t_a", on_a, {}, false, ix) == 0);
  CHECK(sm3::drop_column(db.ss, t, "zz") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::drop_column(db.ss, t, "a") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::drop_column(db.ss, t, "b") == 0);

  std::vector<sm3::index_key_spec_t> on_b = {{1, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_7 = {{7, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> a_coll = {{0, false, false, 1000}};
  CHECK(sm3::create_index(db.ss, t, "x", on_b, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_index(db.ss, t, "x", on_7, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_index(db.ss, t, "x", a_coll, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_index(db.ss, t, "t_a", on_a, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::add_column(db.ss, t, col{"s", sm2::type_text.code, -1, false},
                          std::nullopt) == 0);
  std::vector<sm3::index_key_spec_t> s_unknown = {{2, false, false, 999999}};
  CHECK(sm3::create_index(db.ss, t, "x", s_unknown, {}, false, other) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  CHECK(sm3::drop_index(db.ss, t, 999999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::drop_table(db.ss, 999999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  std::vector<sm3::column_spec_t> one = {{"a", sm2::type_int4.code, -1, false}};
  uint64_t u = 0;
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "u", one, u) == 0);
  CHECK(sm3::rename_table(db.ss, t, "u") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  uint64_t co = 0;
  CHECK(sm3::create_collation(db.ss, sm3::nsp_public, "bad", "zz-QQ-bogus!",
                              true, co) == -1);
  REQUIRE(sm3::abort(db.ss) == 0);
}

TEST_CASE("an index's state is recorded, and survives a restart") {
  // Verify an index's state, kept by index (SM3_DESIGN.md §3.3).
  // 1. An index made in the transaction that made its table is valid.
  // 2. Two made on a table committed earlier are pending.
  // 3. The states load back after a restart.
  // 4. set_index_state marks one valid and one failed, seen at once
  //    by its own transaction and by the catalog only at commit,
  //    and those load back too, the failed one with its reason.
  // 5. A state is set once: an index that is valid or failed is
  //    SM_ERR_INVALID to set again, whatever to; setting a pending
  //    one pending changes nothing.
  // 6. An index the table does not have is SM_ERR_NOTFOUND.
  cat_db_t db;
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  uint64_t fresh = 0, late = 0, bad = 0, idle = 0;
  db.begin();
  uint64_t t = make_t(db);
  REQUIRE(sm3::create_index(db.ss, t, "fresh", on_a, {}, false, fresh) == 0);
  db.commit();
  db.begin();
  REQUIRE(sm3::create_index(db.ss, t, "late", on_a, {}, false, late) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "bad", on_a, {}, false, bad) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "idle", on_a, {}, false, idle) == 0);
  db.commit();

  using sm3::index_state_t;
  sm3::catalog_t *cat = db.reload();
  CHECK(sm3::cat_get_index_state(cat, fresh).state == index_state_t::valid);
  CHECK(sm3::cat_get_index_state(cat, late).state == index_state_t::pending);
  CHECK(sm3::cat_get_index_state(cat, bad).state == index_state_t::pending);

  db.begin();
  REQUIRE(sm3::set_index_state(db.ss, t, late, index_state_t::valid) == 0);
  REQUIRE(sm3::set_index_state(db.ss, t, bad, index_state_t::failed,
                               sm1::SM_ERR_DUPKEY, "two rows") == 0);
  REQUIRE(sm3::set_index_state(db.ss, t, idle, index_state_t::pending) == 0);
  sm3::index_status_t st = sm3::cat_get_index_state(db.ss, bad);
  CHECK(st.state == index_state_t::failed);
  CHECK(st.error == sm1::SM_ERR_DUPKEY);
  CHECK(st.reason == "two rows");
  st = sm3::cat_get_index_state(db.ss, late);
  CHECK(st.state == index_state_t::valid);
  CHECK(st.error == sm1::SM_OK);
  CHECK(st.reason.empty());
  // Not yet committed: the catalog still has them as they were.
  CHECK(sm3::cat_get_index_state(cat, late).state == index_state_t::pending);
  // Set once, even by the transaction that set it.
  CHECK(sm3::set_index_state(db.ss, t, late, index_state_t::failed) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::set_index_state(db.ss, t, late + 999, index_state_t::valid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  db.commit();
  CHECK(sm3::cat_get_index_state(cat, late).state == index_state_t::valid);

  cat = db.reload();
  st = sm3::cat_get_index_state(cat, bad);
  CHECK(st.state == index_state_t::failed);
  CHECK(st.error == sm1::SM_ERR_DUPKEY);
  CHECK(st.reason == "two rows");
  CHECK(sm3::cat_get_index_state(cat, late).state == index_state_t::valid);
  CHECK(sm3::cat_get_index_state(cat, idle).state == index_state_t::pending);

  db.begin();
  for (uint64_t x : {fresh, late, bad}) {
    for (index_state_t to : {index_state_t::pending, index_state_t::valid,
                             index_state_t::failed}) {
      CHECK(sm3::set_index_state(db.ss, t, x, to) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    }
  }
  db.commit();
}

namespace {

/* An evaluator that answers nothing: for a test that only records. */
int no_eval(const sm3::table_def_t &, const sm3::index_def_t &,
            std::span<const sm2::datum_t>, std::span<const bool>, bool &belongs,
            std::vector<std::string> &, std::span<bool>) {
  belongs = true;
  return 0;
}

} // namespace

TEST_CASE("an evaluated index keeps its blob, its columns and its types") {
  // Verify what the catalog records for a partial or expression
  // index (SM3_DESIGN.md §3).
  // 1. On t (a int4, b text), committed, create an index keyed by an
  //    expression of type text, descending, then by column a, with a
  //    blob that holds a NUL byte, reading columns b and a.
  // 2. A partial index over a plain column is recorded the same way,
  //    with no expression column.
  // 3. After a restart: each index's blob byte for byte, the columns
  //    it reads, -1 for the expression's place, and the key built
  //    for sm2 with the expression's own type.
  // 4. A plain index has an empty blob and reads nothing.
  cat_db_t db;
  REQUIRE(sm3::cat_set_evaluator(db.cat, no_eval) == 0);
  db.begin();
  uint64_t t = make_t(db);
  db.commit();

  std::string blob("lower(b)\0a > 0", 14);
  std::vector<sm3::index_key_spec_t> expr_then_a = {
      {-1, true, false, std::nullopt, sm2::type_text.code, -1},
      {0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<int32_t> reads = {1, 0}, reads_b = {1};
  uint64_t expr = 0, partial = 0, plain = 0;
  db.begin();
  REQUIRE_MESSAGE(sm3::create_index(db.ss, t, "t_expr", expr_then_a, {}, false,
                                    blob, reads, expr) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::create_index(db.ss, t, "t_partial", on_a, {}, true,
                            "b is not null", reads_b, partial) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "t_plain", on_a, {}, false, plain) == 0);
  db.commit();

  auto d = sm3::cat_get_table(db.reload(), t);
  REQUIRE(d != nullptr);
  REQUIRE(d->indexes.size() == 3);
  const sm3::index_def_t &x = d->indexes[0];
  CHECK(x.eval == blob);
  CHECK(x.reads == reads);
  CHECK(x.key_attnums == std::vector<int32_t>{-1, 0});
  REQUIRE(x.key.size() == 2);
  CHECK(x.key[0].type == &sm2::type_text);
  CHECK(x.key[0].typmod == -1);
  CHECK(x.key[0].desc);
  CHECK(x.key[1].type == &sm2::type_int4);
  const sm3::index_def_t &p = d->indexes[1];
  CHECK(p.eval == "b is not null");
  CHECK(p.reads == reads_b);
  CHECK(p.key_attnums == std::vector<int32_t>{0});
  CHECK(p.unique);
  CHECK(d->indexes[2].eval.empty());
  CHECK(d->indexes[2].reads.empty());
}

TEST_CASE("the catalog refuses an evaluated index it cannot keep") {
  // Verify create_index's errors for partial and expression
  // indexes; each is SM_ERR_INVALID.
  // 1. With no evaluator given to the catalog, any index with a blob.
  // 2. An expression key column, or columns read, with no blob.
  // 3. An expression of a type sm2 does not have, and a collation on
  //    an expression that is not text.
  // 4. A column read that is not a live column of the table.
  // 5. A column an index reads cannot be dropped.
  // 6. Giving the catalog a second evaluator, or an empty one.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ci = 0, ix = 0;
  REQUIRE(sm3::create_collation(db.ss, sm3::nsp_public, "ci", "und-u-ks-level2",
                                false, ci) == 0);
  db.commit();
  std::vector<sm3::index_key_spec_t> on_a = {{0, false, false, std::nullopt}};
  std::vector<sm3::index_key_spec_t> expr = {
      {-1, false, false, std::nullopt, sm2::type_text.code, -1}};
  std::vector<sm3::index_key_spec_t> expr_no_type = {
      {-1, false, false, std::nullopt, 200, -1}};
  std::vector<sm3::index_key_spec_t> int_expr_coll = {
      {-1, false, false, ci, sm2::type_int4.code, -1}};
  std::vector<int32_t> reads_b = {1}, reads_far = {7};
  auto refused = [&](std::span<const sm3::index_key_spec_t> keys,
                     std::string_view blob, std::span<const int32_t> reads) {
    int rc = sm3::create_index(db.ss, t, "x", keys, {}, false, blob, reads, ix);
    return rc == -1 && sm1::errnum() == sm1::SM_ERR_INVALID;
  };

  db.begin();
  CHECK(refused(on_a, "a > 0", {}));
  CHECK(sm3::cat_set_evaluator(db.cat, nullptr) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::cat_set_evaluator(db.cat, no_eval) == 0);
  CHECK(sm3::cat_set_evaluator(db.cat, no_eval) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  CHECK(refused(expr, "", {}));
  CHECK(refused(on_a, "", reads_b));
  CHECK(refused(expr_no_type, "f(b)", reads_b));
  CHECK(refused(int_expr_coll, "f(b)", reads_b));
  CHECK(refused(on_a, "c > 0", reads_far));

  REQUIRE_MESSAGE(sm3::create_index(db.ss, t, "t_partial", on_a, {}, false,
                                    "b is not null", reads_b, ix) == 0,
                  sm1::errmsg());
  CHECK(sm3::drop_column(db.ss, t, "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();
}

TEST_CASE("a sequence is made in a namespace, found by name, and counts") {
  // Verify create_sequence, cat_get_sequence, cat_list_sequences and
  // nextval.
  // 1. A sequence made in public starting at 10 by 5 is found and
  //    listed by its transaction, and hands out 10, then 15; another
  //    session does not find it before the commit.
  // 2. Its name is a relation's: a table cannot take it, and it
  //    cannot take a table's; the same name in another namespace
  //    is free.
  // 3. An increment of 0, or no name, is SM_ERR_INVALID.
  // 4. After the commit the other session finds it, and its next
  //    value is 20.
  // 5. One made under a savepoint goes with a rollback to it, and
  //    one made in an aborted transaction is not there: its name is
  //    not found and nextval is SM_ERR_NOTFOUND.
  // 6. After a restart it is found again, and goes on counting by 5
  //    past 20.
  cat_db_t db;
  uint64_t ids = 0, oid = 0, t = 0, app = 0, other_ids = 0, tmp = 0, v = 0;
  using names_t = std::vector<std::string>;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  db.begin();
  REQUIRE_MESSAGE(
      sm3::create_sequence(db.ss, sm3::nsp_public, "ids", 10, 5, ids) == 0,
      sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "ids", oid) == 0);
  CHECK(oid == ids);
  std::string seqname;
  uint64_t seqnsp = 0;
  REQUIRE(sm3::cat_get_sequence_name(db.ss, ids, seqnsp, seqname) == 0);
  CHECK(seqnsp == sm3::nsp_public);
  CHECK(seqname == "ids");
  names_t names;
  REQUIRE(sm3::cat_list_sequences(db.ss, sm3::nsp_public, names) == 0);
  CHECK(names == names_t{"ids"});
  REQUIRE(sm3::nextval(db.ss, ids, v) == 0);
  CHECK(v == 10);
  REQUIRE(sm3::nextval(db.ss, ids, v) == 0);
  CHECK(v == 15);
  REQUIRE(sm3::begin(other, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::cat_get_sequence(other, sm3::nsp_public, "ids", oid) == 1);
  REQUIRE(sm3::commit(other) == 0);

  CHECK(sm3::create_table(db.ss, sm3::nsp_public, "ids", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "orders", cols, t) == 0);
  CHECK(sm3::create_sequence(db.ss, sm3::nsp_public, "orders", 1, 1, oid) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "orders", oid) == 1);
  CHECK(sm3::cat_get_sequence_name(db.ss, t, seqnsp, seqname) == 1);
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  REQUIRE(sm3::create_sequence(db.ss, app, "ids", 1, 1, other_ids) == 0);
  CHECK(other_ids != ids);

  CHECK(sm3::create_sequence(db.ss, app, "zero", 1, 0, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::create_sequence(db.ss, app, "", 1, 1, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  db.commit();

  REQUIRE(sm3::begin(other, sm1::xctlevel_t::read_committed) == 0);
  CHECK(sm3::cat_get_sequence(other, sm3::nsp_public, "ids", oid) == 0);
  CHECK(oid == ids);
  REQUIRE(sm3::nextval(other, ids, v) == 0);
  CHECK(v == 20);
  REQUIRE(sm3::commit(other) == 0);
  sm3::session_close(other);

  db.begin();
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE(sm3::create_sequence(db.ss, app, "tmp", 1, 1, tmp) == 0);
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK(sm3::cat_get_sequence(db.ss, app, "tmp", oid) == 1);
  REQUIRE(sm3::create_sequence(db.ss, app, "gone", 1, 1, tmp) == 0);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  CHECK(sm3::cat_get_sequence(db.ss, app, "gone", oid) == 1);
  CHECK(sm3::nextval(db.ss, tmp, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.reload();
  db.begin();
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "ids", oid) == 0);
  CHECK(oid == ids);
  REQUIRE(sm3::nextval(db.ss, ids, v) == 0);
  CHECK(v > 20);
  CHECK((v - 10) % 5 == 0);
  db.commit();
}

TEST_CASE("a sequence's definition is read back by its oid") {
  // Verify cat_get_sequence_def.
  // 1. Make ids, START 10 INCREMENT 5, and table t.
  // 2. ids reads back its namespace, name, START and INCREMENT, and no
  //    owner; once t's column b owns it, that owner.
  // 3. A table's oid, and an unknown one, are 1: no sequence.
  cat_db_t db;
  db.begin();
  uint64_t t = make_t(db), ids = 0;
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "ids", 10, 5, ids) == 0);
  sm3::sequence_def_t d;
  REQUIRE(sm3::cat_get_sequence_def(db.ss, ids, d) == 0);
  CHECK(d.nspoid == sm3::nsp_public);
  CHECK(d.name == "ids");
  CHECK(d.start == 10);
  CHECK(d.increment == 5);
  CHECK_FALSE(d.owner.has_value());
  REQUIRE(sm3::own_sequence(db.ss, ids, t, 1) == 0);
  REQUIRE(sm3::cat_get_sequence_def(db.ss, ids, d) == 0);
  CHECK(d.owner == t);
  CHECK(d.owner_attnum == 1);
  CHECK(sm3::cat_get_sequence_def(db.ss, t, d) == 1);
  CHECK(sm3::cat_get_sequence_def(db.ss, 99999, d) == 1);
  db.commit();
}

TEST_CASE("a sequence is dropped, and holds its namespace until it is") {
  // Verify drop_sequence and a namespace's drop with one in it.
  // 1. A namespace holding a sequence is SM_ERR_INUSE to drop, also
  //    after a restart, when only memory's count of it is reloaded.
  // 2. A dropped sequence is not found or listed, and nextval on it
  //    is SM_ERR_NOTFOUND; a rollback to a savepoint brings it back
  //    and it counts on.
  // 3. Dropping an unknown oid is SM_ERR_NOTFOUND.
  // 4. With the sequence dropped, the namespace can be dropped in
  //    the same transaction, and both stay gone after a restart.
  // 5. A sequence made and dropped in one transaction leaves its
  //    namespace free to drop.
  cat_db_t db;
  uint64_t app = 0, s = 0, oid = 0, v = 0, scratch = 0, q = 0;
  using names_t = std::vector<std::string>;
  names_t names;
  db.begin();
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  REQUIRE(sm3::create_sequence(db.ss, app, "s", 1, 1, s) == 0);
  db.commit();
  db.begin();
  CHECK(sm3::drop_namespace(db.ss, app) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.reload();
  db.begin();
  CHECK(sm3::drop_namespace(db.ss, app) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  REQUIRE(sm3::nextval(db.ss, s, v) == 0);
  CHECK(v == 1);
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE_MESSAGE(sm3::drop_sequence(db.ss, s) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, app, "s", oid) == 1);
  REQUIRE(sm3::cat_list_sequences(db.ss, app, names) == 0);
  CHECK(names.empty());
  CHECK(sm3::nextval(db.ss, s, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK(sm3::cat_get_sequence(db.ss, app, "s", oid) == 0);
  REQUIRE(sm3::nextval(db.ss, s, v) == 0);
  CHECK(v == 2);
  CHECK(sm3::drop_sequence(db.ss, s + 999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm3::drop_sequence(db.ss, s) == 0);
  REQUIRE_MESSAGE(sm3::drop_namespace(db.ss, app) == 0, sm1::errmsg());
  db.commit();
  db.reload();
  db.begin();
  CHECK_FALSE(sm3::cat_get_namespace(db.ss, "app", oid));
  CHECK(sm3::nextval(db.ss, s, v) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  REQUIRE(sm3::create_namespace(db.ss, "scratch", scratch) == 0);
  db.commit();
  db.begin();
  REQUIRE(sm3::create_sequence(db.ss, scratch, "q", 1, 1, q) == 0);
  CHECK(sm3::drop_namespace(db.ss, scratch) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::drop_sequence(db.ss, q) == 0);
  REQUIRE_MESSAGE(sm3::drop_namespace(db.ss, scratch) == 0, sm1::errmsg());
  db.commit();
}

TEST_CASE("a sequence's name is found as committed, not as of a snapshot") {
  // Verify cat_get_sequence and cat_get_sequence_name find a
  // sequence as a table is found.
  // 1. other begins at repeatable read and reads the catalog, so
  //    its snapshot is taken.
  // 2. db makes the sequence late and commits: other finds it by
  //    name and its name by oid.
  // 3. db drops it: db finds it no longer either way, other still
  //    does.
  // 4. db commits: other finds it no longer, though its snapshot
  //    still holds it.
  cat_db_t db;
  uint64_t late = 0, oid = 0, nsp = 0;
  std::string name;
  std::vector<std::string> names;
  sm3::session_t *other = nullptr;
  REQUIRE(sm3::session_open(db.cat, db.open(), other) == 0);
  REQUIRE(sm3::begin(other, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm3::cat_list_sequences(other, sm3::nsp_public, names) == 0);
  CHECK(names.empty());

  db.begin();
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "late", 1, 1, late) ==
          0);
  db.commit();
  CHECK(sm3::cat_get_sequence(other, sm3::nsp_public, "late", oid) == 0);
  CHECK(oid == late);
  REQUIRE(sm3::cat_get_sequence_name(other, late, nsp, name) == 0);
  CHECK(nsp == sm3::nsp_public);
  CHECK(name == "late");

  db.begin();
  REQUIRE_MESSAGE(sm3::drop_sequence(db.ss, late) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "late", oid) == 1);
  CHECK(sm3::cat_get_sequence_name(db.ss, late, nsp, name) == 1);
  CHECK(sm3::cat_get_sequence(other, sm3::nsp_public, "late", oid) == 0);
  CHECK(sm3::cat_get_sequence_name(other, late, nsp, name) == 0);

  db.commit();
  CHECK(sm3::cat_get_sequence(other, sm3::nsp_public, "late", oid) == 1);
  CHECK(sm3::cat_get_sequence_name(other, late, nsp, name) == 1);
  REQUIRE(sm3::commit(other) == 0);
  sm3::session_close(other);
}

TEST_CASE("an index's name follows its commits, and loads back") {
  // Verify the catalog's map of index names, which cat_get_index
  // reads for a table the transaction has not changed.
  // 1. t and u are made, with t's index i, and committed: i is
  //    found on t.
  // 2. One transaction drops i from t and makes i on u: once
  //    committed, i is found on u.
  // 3. After a restart, i is still found on u, by the map the load
  //    built.
  // 4. u is dropped and committed: i is found nowhere.
  cat_db_t db;
  uint64_t t = 0, u = 0, ti = 0, ui = 0, table = 0, index = 0;
  std::vector<sm3::column_spec_t> cols = {{"a", sm2::type_int4.code, -1, true}};
  std::vector<sm3::index_key_spec_t> keys = {{0, false, false, std::nullopt}};
  db.begin();
  t = make_t(db);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "u", cols, u) == 0);
  REQUIRE(sm3::create_index(db.ss, t, "i", keys, {}, false, ti) == 0);
  db.commit();
  db.begin();
  CHECK(sm3::cat_get_index(db.ss, sm3::nsp_public, "i", table, index));
  CHECK(table == t);
  CHECK(index == ti);
  db.commit();

  // 2. Move the name from t to u.
  db.begin();
  REQUIRE_MESSAGE(sm3::drop_index(db.ss, t, ti) == 0, sm1::errmsg());
  REQUIRE(sm3::create_index(db.ss, u, "i", keys, {}, false, ui) == 0);
  db.commit();
  db.begin();
  CHECK(sm3::cat_get_index(db.ss, sm3::nsp_public, "i", table, index));
  CHECK(table == u);
  CHECK(index == ui);
  db.commit();

  // 3. Restart.
  db.reload();
  db.begin();
  table = index = 0;
  CHECK(sm3::cat_get_index(db.ss, sm3::nsp_public, "i", table, index));
  CHECK(table == u);
  CHECK(index == ui);
  db.commit();

  // 4. Drop u.
  db.begin();
  REQUIRE(sm3::drop_table(db.ss, u) == 0);
  db.commit();
  db.begin();
  CHECK_FALSE(sm3::cat_get_index(db.ss, sm3::nsp_public, "i", table, index));
  db.commit();
}

TEST_CASE("a column owns a sequence, which goes with its table or column") {
  // Verify own_sequence, and the drops it makes a sequence follow.
  // 1. A sequence owned by t's column a is SM_ERR_INUSE to drop, also
  //    after a restart, the owner kept in its record.
  // 2. Owning it by a column t does not have, or by a table in
  //    another namespace, is SM_ERR_INVALID.
  // 3. Owned by none again, it can be dropped; a rollback brings
  //    the owner back.
  // 4. Dropping column b drops the sequence b owns, not a's.
  // 5. Dropping t drops the sequences it still owns; one it does not
  //    own is left, and a rollback brings them back.
  cat_db_t db;
  uint64_t t = 0, sa = 0, sb = 0, free_q = 0, app = 0, oid = 0, v = 0;
  db.begin();
  t = make_t(db);
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "t_a_seq", 1, 1, sa) ==
          0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "t_b_seq", 1, 1, sb) ==
          0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "free", 1, 1, free_q) ==
          0);
  REQUIRE_MESSAGE(sm3::own_sequence(db.ss, sa, t, 0) == 0, sm1::errmsg());
  REQUIRE(sm3::own_sequence(db.ss, sb, t, 1) == 0);
  db.commit();
  db.reload();

  db.begin();
  CHECK(sm3::drop_sequence(db.ss, sa) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  CHECK(sm3::own_sequence(db.ss, free_q, t, 7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  uint64_t other = 0;
  REQUIRE(sm3::create_sequence(db.ss, app, "q", 1, 1, other) == 0);
  CHECK(sm3::own_sequence(db.ss, other, t, 0) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE(sm3::own_sequence(db.ss, sa, 0, -1) == 0);
  REQUIRE_MESSAGE(sm3::drop_sequence(db.ss, sa) == 0, sm1::errmsg());
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK(sm3::drop_sequence(db.ss, sa) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  REQUIRE_MESSAGE(sm3::drop_column(db.ss, t, "b") == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_b_seq", oid) == 1);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 0);
  db.commit();

  db.begin();
  REQUIRE(sm3::savepoint(db.ss) == 1);
  REQUIRE_MESSAGE(sm3::drop_table(db.ss, t) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 1);
  CHECK(sm3::nextval(db.ss, sa, v) == -1);
  std::vector<std::string> names;
  REQUIRE(sm3::cat_list_sequences(db.ss, sm3::nsp_public, names) == 0);
  CHECK(names == std::vector<std::string>{"free"});
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "free", oid) == 0);
  REQUIRE(sm3::rollback(db.ss, 1) == 0);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 0);
  REQUIRE(sm3::drop_table(db.ss, t) == 0);
  db.commit();
  db.reload();
  db.begin();
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 1);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "free", oid) == 0);
  db.commit();
}

TEST_CASE("two tables that own sequences are dropped in one transaction") {
  // Verify that a drop finds the sequences a later drop must skip:
  // DROP TABLE a, b of two serial columns, digres's sequence test.
  // 1. Tables t and u each own a sequence, committed.
  // 2. In one statement drop t, then u: both succeed, though the
  //    catalog's scan in it does not see the first drop's erase.
  // 3. Neither sequence is found, then or after a restart.
  cat_db_t db;
  std::vector<sm3::column_spec_t> cols = {{"a", sm2::type_int4.code, -1, true}};
  uint64_t t = 0, u = 0, st = 0, su = 0, oid = 0;
  db.begin();
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, t) == 0);
  REQUIRE(sm3::create_table(db.ss, sm3::nsp_public, "u", cols, u) == 0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "t_a_seq", 1, 1, st) ==
          0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "u_a_seq", 1, 1, su) ==
          0);
  REQUIRE(sm3::own_sequence(db.ss, st, t, 0) == 0);
  REQUIRE(sm3::own_sequence(db.ss, su, u, 0) == 0);
  db.commit();

  db.begin();
  REQUIRE(sm3::stmt_begin(db.ss) == 0);
  REQUIRE_MESSAGE(sm3::drop_table(db.ss, t) == 0, sm1::errmsg());
  REQUIRE_MESSAGE(sm3::drop_table(db.ss, u) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 1);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "u_a_seq", oid) == 1);
  db.commit();
  db.reload();
  db.begin();
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "t_a_seq", oid) == 1);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "u_a_seq", oid) == 1);
  db.commit();
}

TEST_CASE("an identity column owns its sequence from its CREATE TABLE") {
  // Verify create_table's identity columns, and that their sequence
  // stays theirs.
  // 1. Create t with an ALWAYS identity id on sequence q: after a
  //    restart, id keeps its kind and sequence, and q is owned by
  //    t's id, so dropping q is SM_ERR_INUSE.
  // 2. Changing q's owner, to none or to another column, is
  //    SM_ERR_INUSE.
  // 3. An identity column that is nullable, of a non-integer type,
  //    with a default, an unknown kind, a sequence already owned or
  //    in another namespace, or in a temporary table, is
  //    SM_ERR_INVALID; so is one added by add_column.
  // 4. Dropping t drops q.
  cat_db_t db;
  uint64_t q = 0, q2 = 0, other = 0, app = 0, t = 0, oid = 0;
  db.begin();
  REQUIRE(sm3::create_namespace(db.ss, "app", app) == 0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "q", 1, 1, q) == 0);
  REQUIRE(sm3::create_sequence(db.ss, sm3::nsp_public, "q2", 1, 1, q2) == 0);
  REQUIRE(sm3::create_sequence(db.ss, app, "o", 1, 1, other) == 0);
  std::vector<sm3::column_spec_t> cols = {
      {"id", sm2::type_int4.code, -1, true, std::nullopt, 'a', q},
      {"v", sm2::type_text.code, -1, false}};
  REQUIRE_MESSAGE(sm3::create_table(db.ss, sm3::nsp_public, "t", cols, t) == 0,
                  sm1::errmsg());
  db.commit();
  db.reload();
  db.begin();
  auto def = sm3::cat_get_table(db.ss, t);
  REQUIRE(def);
  CHECK(def->columns[0].identity == 'a');
  CHECK(def->columns[0].identity_seq == q);
  CHECK(def->columns[1].identity == 0);
  CHECK(sm3::drop_sequence(db.ss, q) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(db.ss) == 0);

  for (int32_t attnum : {-1, 1}) {
    db.begin();
    CHECK(sm3::own_sequence(db.ss, q, attnum < 0 ? 0 : t, attnum) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INUSE);
    REQUIRE(sm3::abort(db.ss) == 0);
  }

  std::vector<sm3::column_spec_t> bad[] = {
      {{"id", sm2::type_int4.code, -1, false, std::nullopt, 'a', q2}},
      {{"id", sm2::type_text.code, -1, true, std::nullopt, 'a', q2}},
      {{"id", sm2::type_int4.code, -1, true, std::string("x"), 'd', q2}},
      {{"id", sm2::type_int4.code, -1, true, std::nullopt, 'x', q2}},
      {{"id", sm2::type_int4.code, -1, true, std::nullopt, 'd', q}},
      {{"id", sm2::type_int4.code, -1, true, std::nullopt, 'd', other}}};
  for (const auto &c : bad) {
    db.begin();
    CHECK(sm3::create_table(db.ss, sm3::nsp_public, "u", c, oid) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    REQUIRE(sm3::abort(db.ss) == 0);
  }
  db.begin();
  CHECK(sm3::create_temp_table(db.ss, "u", cols, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.ss) == 0);
  db.begin();
  CHECK(sm3::add_column(
            db.ss, t,
            {"w", sm2::type_int8.code, -1, false, std::nullopt, 'd', q2},
            std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(db.ss) == 0);

  db.begin();
  REQUIRE_MESSAGE(sm3::drop_table(db.ss, t) == 0, sm1::errmsg());
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "q", oid) == 1);
  CHECK(sm3::cat_get_sequence(db.ss, sm3::nsp_public, "q2", oid) == 0);
  db.commit();
}
