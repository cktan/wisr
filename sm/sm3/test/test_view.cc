/*
 * test_view.cc -- views in the catalog (VIEW.md at the top level):
 * kept and loaded back, their names a relation's, what they read
 * checked and held against a drop, and their locks.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/*
 * A catalog with table t (x int4, y int4), and sessions a and b over
 * it.
 */
class views_t : public db_t {
public:
  /* Initialize and load the catalog, create t, open a and b. */
  views_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    load();
    std::vector<sm3::column_spec_t> cols = {
        {"x", sm2::type_int4.code, -1, false},
        {"y", sm2::type_int4.code, -1, false}};
    begin(a);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "t", cols, t) == 0);
    commit(a);
  }

  /* Close the sessions and the catalog. */
  ~views_t() { close(); }

  /* Restart, then load the catalog and open the sessions again. */
  void reload() {
    close();
    restart();
    load();
  }

  /* Begin a READ COMMITTED transaction on s. */
  void begin(sm3::session_t *s) {
    REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
  }

  /* Commit s's transaction. */
  void commit(sm3::session_t *s) {
    REQUIRE_MESSAGE(sm3::commit(s) == 0, sm1::errmsg());
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t t = 0;

private:
  /* Load the catalog; open a and b over sessions of their own. */
  void load() {
    REQUIRE_MESSAGE(sm3::cat_open(env(), "db", cat) == 0, sm1::errmsg());
    REQUIRE(sm3::session_open(cat, open(), a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
  }

  /* Close a, b and the catalog. */
  void close() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cat_close(cat);
    a = b = nullptr;
    cat = nullptr;
  }
};

/* A view's definition: a query, options, and what it reads. */
sm3::view_def_t view_of(std::vector<sm3::view_read_t> reads,
                        const std::string &query = "query") {
  sm3::view_def_t def;
  def.query = query;
  def.reads = std::move(reads);
  return def;
}

/* Make the view name reading reads in s's transaction; its oid. */
uint64_t make_view(sm3::session_t *s, const std::string &name,
                   std::vector<sm3::view_read_t> reads) {
  uint64_t oid = 0;
  REQUIRE_MESSAGE(sm3::create_view(s, sm3::nsp_public, name,
                                   view_of(std::move(reads)), oid) == 0,
                  sm1::errmsg());
  return oid;
}

/* Expect rc to be a failure with err. */
void fails(int rc, sm1::errnum_t err) {
  CHECK(rc == -1);
  CHECK(sm1::errnum() == err);
}

/* Give a blocking call time to reach its wait. */
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("a view is kept, and loads back") {
  // Verify create_view's record and the load (VIEW.md §3, §4).
  // 1. A makes v, reading t's y and t whole, with a query and options:
  //    A sees it by name and oid at once, B not until A commits.
  // 2. Restart: v loads back, every field as given, and is listed.
  views_t f;
  f.begin(f.a);
  sm3::view_def_t def = view_of({{f.t, 1}, {f.t, -1}}, "the query");
  def.options = "check_option=local";
  uint64_t v = 0, found = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_public, "v", def, v) == 0);
  CHECK(sm3::cat_get_view(f.a, sm3::nsp_public, "v", found));
  CHECK(found == v);
  CHECK(sm3::cat_view_version(f.a, v) == sm3::table_version_own);
  CHECK_FALSE(sm3::cat_get_view(f.b, sm3::nsp_public, "v", found));
  f.commit(f.a);
  CHECK(sm3::cat_get_view(f.b, sm3::nsp_public, "v", found));

  f.reload();
  std::shared_ptr<const sm3::view_def_t> d = sm3::cat_get_view(f.a, v);
  REQUIRE(d != nullptr);
  CHECK(d->nspoid == sm3::nsp_public);
  CHECK(d->name == "v");
  CHECK(d->query == "the query");
  CHECK(d->options == "check_option=local");
  CHECK(d->reads == std::vector<sm3::view_read_t>{{f.t, 1}, {f.t, -1}});
  CHECK(sm3::cat_view_version(f.a, v) == 0);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_public) ==
        std::vector<std::string>{"v"});
  CHECK(sm3::cat_list_tables(f.a, sm3::nsp_public) ==
        std::vector<std::string>{"t"});
}

TEST_CASE("a view's name is a relation's") {
  // Verify a view shares its namespace's names with the relations.
  // 1. A view named t, the table's name, is SM_ERR_DUPKEY.
  // 2. With v made, a table, a sequence and a second view named v are
  //    SM_ERR_DUPKEY, as is renaming t to v.
  views_t f;
  f.begin(f.a);
  uint64_t oid = 0;
  fails(sm3::create_view(f.a, sm3::nsp_public, "t", view_of({}), oid),
        sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);

  f.begin(f.a);
  make_view(f.a, "v", {{f.t, 0}});
  f.commit(f.a);
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  f.begin(f.a);
  fails(sm3::create_table(f.a, sm3::nsp_public, "v", cols, oid),
        sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  fails(sm3::create_sequence(f.a, sm3::nsp_public, "v", 1, 1, oid),
        sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  fails(sm3::create_view(f.a, sm3::nsp_public, "v", view_of({}), oid),
        sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  fails(sm3::rename_table(f.a, f.t, "v"), sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("what a view reads is checked") {
  // Verify create_view refuses a read that is not a relation it may
  // read (VIEW.md §5).
  // 1. No name, and a temporary view, are SM_ERR_INVALID.
  // 2. A relation that is not there is SM_ERR_NOTFOUND.
  // 3. A column t has not, and one dropped, are SM_ERR_INVALID.
  // 4. A column of a view is SM_ERR_INVALID; the view whole is not.
  // 5. A temporary table is SM_ERR_INVALID.
  views_t f;
  uint64_t oid = 0, tmp = 0;
  auto refused = [&](std::vector<sm3::view_read_t> reads, sm1::errnum_t err) {
    f.begin(f.a);
    fails(sm3::create_view(f.a, sm3::nsp_public, "w", view_of(reads), oid),
          err);
    REQUIRE(sm3::abort(f.a) == 0);
  };
  f.begin(f.a);
  fails(sm3::create_view(f.a, sm3::nsp_public, "", view_of({}), oid),
        sm1::SM_ERR_INVALID);
  fails(sm3::create_view(f.a, sm3::nsp_temp, "w", view_of({}), oid),
        sm1::SM_ERR_INVALID);
  REQUIRE(sm3::abort(f.a) == 0);

  refused({{999999, -1}}, sm1::SM_ERR_NOTFOUND);
  refused({{f.t, 2}}, sm1::SM_ERR_INVALID);
  f.begin(f.a);
  REQUIRE(sm3::drop_column(f.a, f.t, "y") == 0);
  f.commit(f.a);
  refused({{f.t, 1}}, sm1::SM_ERR_INVALID);

  f.begin(f.a);
  uint64_t v = make_view(f.a, "v", {{f.t, 0}});
  f.commit(f.a);
  refused({{v, 0}}, sm1::SM_ERR_INVALID);
  f.begin(f.a);
  make_view(f.a, "w", {{v, -1}});
  REQUIRE(sm3::abort(f.a) == 0);

  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  f.begin(f.a);
  REQUIRE(sm3::create_temp_table(f.a, "tt", cols, tmp) == 0);
  f.commit(f.a);
  refused({{tmp, -1}}, sm1::SM_ERR_INVALID);
}

TEST_CASE("what a view reads is not dropped from under it") {
  // Verify the drops a view refuses (VIEW.md §5).
  // 1. v reads t's x, and w reads v.
  // 2. Dropping t, its column x, v, and a namespace holding a view
  //    are SM_ERR_INUSE; dropping t's column y, which no view reads,
  //    is not.
  // 3. Dropped in the order readers first, w, v, then t, all go.
  // 4. In one transaction, a view made and dropped holds nothing.
  views_t f;
  f.begin(f.a);
  uint64_t v = make_view(f.a, "v", {{f.t, 0}});
  uint64_t w = make_view(f.a, "w", {{v, -1}});
  f.commit(f.a);
  CHECK(sm3::cat_view_readers(f.a, f.t) == std::vector<uint64_t>{v});
  CHECK(sm3::cat_view_readers(f.a, v) == std::vector<uint64_t>{w});

  f.begin(f.a);
  fails(sm3::drop_table(f.a, f.t), sm1::SM_ERR_INUSE);
  fails(sm3::drop_column(f.a, f.t, "x"), sm1::SM_ERR_INUSE);
  fails(sm3::drop_view(f.a, v), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::drop_column(f.a, f.t, "y") == 0);
  REQUIRE(sm3::abort(f.a) == 0);

  uint64_t app = 0, in_app = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_namespace(f.a, "app", app) == 0);
  REQUIRE(sm3::create_view(f.a, app, "u", view_of({}), in_app) == 0);
  f.commit(f.a);
  f.begin(f.a);
  fails(sm3::drop_namespace(f.a, app), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.a) == 0);

  f.begin(f.a);
  REQUIRE(sm3::drop_view(f.a, w) == 0);
  REQUIRE(sm3::drop_view(f.a, v) == 0);
  REQUIRE(sm3::drop_view(f.a, in_app) == 0);
  REQUIRE(sm3::drop_namespace(f.a, app) == 0);
  REQUIRE(sm3::drop_table(f.a, f.t) == 0);
  f.commit(f.a);
  CHECK(sm3::cat_get_view(f.a, v) == nullptr);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_public).empty());

  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t t2 = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "t2", cols, t2) == 0);
  uint64_t u = make_view(f.a, "u", {{t2, 0}});
  REQUIRE(sm3::drop_view(f.a, u) == 0);
  REQUIRE(sm3::drop_table(f.a, t2) == 0);
  f.commit(f.a);
}

TEST_CASE("a view is replaced and renamed") {
  // Verify replace_view and rename_view (VIEW.md §5).
  // 1. v reads t's x; w reads v. Replacing v with a query reading y
  //    changes its query and reads, and moves its version.
  // 2. A replace of v reading v, or w, which reads v, is
  //    SM_ERR_INVALID.
  // 3. Renaming v to t is SM_ERR_DUPKEY; to v2, the old name is free
  //    and the new one found, in the transaction and after.
  // 4. A rename rolled back to a savepoint leaves the name as it was.
  views_t f;
  f.begin(f.a);
  uint64_t v = make_view(f.a, "v", {{f.t, 0}});
  uint64_t w = make_view(f.a, "w", {{v, -1}});
  f.commit(f.a);
  uint64_t before = sm3::cat_view_version(f.a, v);

  f.begin(f.a);
  REQUIRE(sm3::replace_view(f.a, v, view_of({{f.t, 1}}, "new query")) == 0);
  f.commit(f.a);
  std::shared_ptr<const sm3::view_def_t> d = sm3::cat_get_view(f.a, v);
  REQUIRE(d != nullptr);
  CHECK(d->name == "v");
  CHECK(d->query == "new query");
  CHECK(d->reads == std::vector<sm3::view_read_t>{{f.t, 1}});
  CHECK(sm3::cat_view_version(f.a, v) > before);
  f.begin(f.a);
  REQUIRE(sm3::drop_column(f.a, f.t, "x") == 0);
  REQUIRE(sm3::abort(f.a) == 0);

  f.begin(f.a);
  fails(sm3::replace_view(f.a, v, view_of({{v, -1}})), sm1::SM_ERR_INVALID);
  fails(sm3::replace_view(f.a, v, view_of({{w, -1}})), sm1::SM_ERR_INVALID);
  fails(sm3::rename_view(f.a, v, "t"), sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);

  uint64_t found = 0;
  f.begin(f.a);
  REQUIRE(sm3::rename_view(f.a, v, "v2") == 0);
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_public, "v", found));
  CHECK(sm3::cat_get_view(f.a, sm3::nsp_public, "v2", found));
  CHECK(sm3::cat_get_view(f.b, sm3::nsp_public, "v", found));
  f.commit(f.a);
  CHECK_FALSE(sm3::cat_get_view(f.b, sm3::nsp_public, "v", found));
  CHECK(sm3::cat_get_view(f.b, sm3::nsp_public, "v2", found));
  CHECK(found == v);

  f.begin(f.a);
  int sp = sm3::savepoint(f.a);
  REQUIRE(sp > 0);
  REQUIRE(sm3::rename_view(f.a, v, "v3") == 0);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  f.commit(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_public) ==
        std::vector<std::string>{"v2", "w"});
}

TEST_CASE("a table's drop waits for a view's create on it, and then refuses") {
  // Verify a drop cannot miss a view made on its table (VIEW.md §7).
  // 1. A makes v on t, holding t intent shared.
  // 2. B drops t, in a thread: it waits.
  // 3. A commits: B's drop fails with SM_ERR_INUSE.
  views_t f;
  f.begin(f.a);
  make_view(f.a, "v", {{f.t, 0}});
  f.begin(f.b);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  std::thread tb([&] {
    rc = sm3::drop_table(f.b, f.t);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  f.commit(f.a);
  tb.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.b) == 0);
}

TEST_CASE("a view's create waits for its table's drop, and then finds none") {
  // Verify a view cannot be made on a table dropped under it.
  // 1. B drops t, holding it exclusive.
  // 2. A makes v on t, in a thread: it waits.
  // 3. B commits: A's create fails with SM_ERR_NOTFOUND.
  views_t f;
  f.begin(f.b);
  REQUIRE(sm3::drop_table(f.b, f.t) == 0);
  f.begin(f.a);
  std::atomic<bool> returned{false};
  int rc = 0, err = 0;
  uint64_t oid = 0;
  std::thread ta([&] {
    rc = sm3::create_view(f.a, sm3::nsp_public, "v", view_of({{f.t, 0}}), oid);
    err = sm1::errnum();
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  f.commit(f.b);
  ta.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("a view's DDL waits for its readers") {
  // Verify a view's lock (VIEW.md §7).
  // 1. A locks v intent shared, as a query does, and reads it.
  // 2. B drops v, in a thread: it waits.
  // 3. A commits: B's drop goes through. Once B commits, A's lock of
  //    v fails with SM_ERR_NOTFOUND.
  views_t f;
  f.begin(f.a);
  uint64_t v = make_view(f.a, "v", {{f.t, 0}});
  f.commit(f.a);

  std::shared_ptr<const sm3::view_def_t> d;
  f.begin(f.a);
  REQUIRE(sm3::cat_lock_view(f.a, v, sm1::SM_LOCKMODE_IS, d) == 0);
  CHECK(d->name == "v");
  f.begin(f.b);
  std::atomic<bool> returned{false};
  int rc = 0;
  std::thread tb([&] {
    rc = sm3::drop_view(f.b, v);
    returned = true;
  });
  settle();
  CHECK_FALSE(returned);
  f.commit(f.a);
  tb.join();
  CHECK(rc == 0);
  f.commit(f.b);

  f.begin(f.a);
  fails(sm3::cat_lock_view(f.a, v, sm1::SM_LOCKMODE_IS, d),
        sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(f.a) == 0);
}
