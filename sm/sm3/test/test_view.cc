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

/* The oids of the views that read oid, as s sees them. */
std::vector<uint64_t> readers(const sm3::session_t *s, uint64_t oid) {
  std::vector<uint64_t> out;
  for (const sm3::view_reader_t &r : sm3::cat_view_readers(s, oid)) {
    out.push_back(r.oid);
  }
  return out;
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

TEST_CASE("a view's query longer than a tuple is kept whole") {
  // Verify a query past a part's 65535 bytes is kept in pieces
  // (../CATALOG.md §3.5).
  // 1. Make v with a query of 200,000 bytes, and one of 60,000, a
  //    piece's length exactly.
  // 2. Restart: both load back byte for byte.
  views_t f;
  std::string longq(200000, 'q'), exact(60000, 'e');
  for (size_t i = 0; i < longq.size(); i += 7) {
    longq[i] = static_cast<char>('a' + i % 26);
  }
  uint64_t v = 0, w = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_public, "v",
                           view_of({{f.t, 0}}, longq), v) == 0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_public, "w", view_of({}, exact), w) ==
          0);
  f.commit(f.a);

  f.reload();
  REQUIRE(sm3::cat_get_view(f.a, v) != nullptr);
  REQUIRE(sm3::cat_get_view(f.a, w) != nullptr);
  CHECK(sm3::cat_get_view(f.a, v)->query == longq);
  CHECK(sm3::cat_get_view(f.a, w)->query == exact);
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
  // 1. No name is SM_ERR_INVALID.
  // 2. A relation that is not there is SM_ERR_NOTFOUND.
  // 3. A column t has not, and one dropped, are SM_ERR_INVALID.
  // 4. A column of a view is SM_ERR_INVALID; the view whole is not.
  // 5. A temporary table is SM_ERR_INVALID, for a permanent view.
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
  CHECK(readers(f.a, f.t) == std::vector<uint64_t>{v});
  CHECK(readers(f.a, v) == std::vector<uint64_t>{w});

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

TEST_CASE("a temporary view is the session's") {
  // Verify a temporary view is kept in the session's temporary
  // catalog (VIEW.md §12.2).
  // 1. A makes tv in namespace 0 over t: A finds it by name at once,
  //    and after its commit; B never does.
  // 2. A rolled-back create, and one under a savepoint rolled back,
  //    leave nothing.
  // 3. A session opened afresh on the catalog has none.
  views_t f;
  uint64_t found = 0;
  f.begin(f.a);
  uint64_t tv = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{f.t, 0}}), tv) ==
          0);
  CHECK(sm3::cat_get_view(f.a, sm3::nsp_temp, "tv", found));
  CHECK(found == tv);
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_public, "tv", found));
  f.commit(f.a);
  CHECK(sm3::cat_get_view(f.a, sm3::nsp_temp, "tv", found));
  CHECK(sm3::cat_get_view(f.a, tv)->nspoid == sm3::nsp_temp);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp) ==
        std::vector<std::string>{"tv"});
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_public).empty());
  CHECK_FALSE(sm3::cat_get_view(f.b, sm3::nsp_temp, "tv", found));
  CHECK(sm3::cat_get_view(f.b, tv) == nullptr);

  uint64_t gone = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "gone", view_of({}), gone) == 0);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  int sp = sm3::savepoint(f.a);
  REQUIRE(sp > 0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "gone", view_of({}), gone) == 0);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  f.commit(f.a);
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_temp, "gone", found));

  f.reload();
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_temp, "tv", found));
  CHECK(sm3::cat_get_view(f.a, tv) == nullptr);
}

TEST_CASE("a temporary view's name and what it reads") {
  // Verify a temporary view's name is one of the session's temporary
  // names, and what it may read (VIEW.md §12.2).
  // 1. A temporary view named as a temporary table, and a temporary
  //    table named as a temporary view, are SM_ERR_DUPKEY; a permanent
  //    view of that name is not.
  // 2. A temporary view reads a temporary table, a temporary view and
  //    t; a permanent view of a temporary view is SM_ERR_INVALID.
  // 3. It is renamed and replaced as a permanent view is.
  views_t f;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t tt = 0, tv = 0, tw = 0, oid = 0, found = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_temp_table(f.a, "tt", cols, tt) == 0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{tt, 0}}), tv) ==
          0);
  f.commit(f.a);

  f.begin(f.a);
  fails(sm3::create_view(f.a, sm3::nsp_temp, "tt", view_of({}), oid),
        sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  fails(sm3::create_temp_table(f.a, "tv", cols, oid), sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_public, "tv", view_of({{f.t, 0}}),
                           oid) == 0);
  REQUIRE(sm3::abort(f.a) == 0);

  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tw",
                           view_of({{tv, -1}, {tt, 0}, {f.t, 1}}), tw) == 0);
  fails(sm3::create_view(f.a, sm3::nsp_public, "pw", view_of({{tv, -1}}), oid),
        sm1::SM_ERR_INVALID);
  f.commit(f.a);

  f.begin(f.a);
  REQUIRE(sm3::rename_view(f.a, tw, "tw2") == 0);
  REQUIRE(sm3::replace_view(f.a, tw, view_of({{tt, 0}}, "new")) == 0);
  f.commit(f.a);
  CHECK(sm3::cat_get_view(f.a, sm3::nsp_temp, "tw2", found));
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_temp, "tw", found));
  CHECK(sm3::cat_get_view(f.a, tw)->query == "new");
}

TEST_CASE("what a temporary view reads is kept in its session") {
  // Verify the session's own drops see its temporary views (VIEW.md
  // §12.2).
  // 1. A's tv reads its temporary table tt and t's x.
  // 2. In A, dropping tt, t, t's x and tv's reader tw is SM_ERR_INUSE.
  // 3. Dropped readers first, tw, tv, then tt and t, all go.
  views_t f;
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t tt = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_temp_table(f.a, "tt", cols, tt) == 0);
  uint64_t tv = 0, tw = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv",
                           view_of({{tt, 0}, {f.t, 0}}), tv) == 0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tw", view_of({{tv, -1}}), tw) ==
          0);
  f.commit(f.a);
  CHECK(readers(f.a, f.t) == std::vector<uint64_t>{tv});

  f.begin(f.a);
  fails(sm3::drop_table(f.a, tt), sm1::SM_ERR_INUSE);
  fails(sm3::drop_table(f.a, f.t), sm1::SM_ERR_INUSE);
  fails(sm3::drop_column(f.a, f.t, "x"), sm1::SM_ERR_INUSE);
  fails(sm3::drop_view(f.a, tv), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.a) == 0);

  f.begin(f.a);
  REQUIRE(sm3::drop_view(f.a, tw) == 0);
  REQUIRE(sm3::drop_view(f.a, tv) == 0);
  REQUIRE(sm3::drop_table(f.a, tt) == 0);
  REQUIRE(sm3::drop_table(f.a, f.t) == 0);
  f.commit(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp).empty());
}

TEST_CASE("another session's drop finds a temporary view") {
  // Verify the register of temporary readers (VIEW.md §12.3).
  // 1. A makes pv over t's x, and temporary tv over t's x and pv.
  // 2. B finds tv among t's readers, but cannot look it up; its drops
  //    of t, t's x and pv are SM_ERR_INUSE; its drop of t's y is not.
  // 3. A's drop of tv rolled back leaves it in the register.
  // 4. A replaces tv to read nothing: B drops pv and t.
  views_t f;
  f.begin(f.a);
  uint64_t pv = make_view(f.a, "pv", {{f.t, 0}});
  uint64_t tv = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv",
                           view_of({{f.t, 0}, {pv, -1}}), tv) == 0);
  f.commit(f.a);

  CHECK(readers(f.b, f.t) == std::vector<uint64_t>{pv, tv});
  CHECK(sm3::cat_view_readers(f.b, f.t)[1].other);
  CHECK(sm3::cat_view_readers(f.b, f.t)[1].def->name == "tv");
  CHECK_FALSE(sm3::cat_view_readers(f.a, f.t)[1].other);
  CHECK(sm3::cat_get_view(f.b, tv) == nullptr);
  f.begin(f.b);
  fails(sm3::drop_table(f.b, f.t), sm1::SM_ERR_INUSE);
  fails(sm3::drop_column(f.b, f.t, "x"), sm1::SM_ERR_INUSE);
  fails(sm3::drop_view(f.b, pv), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.b) == 0);
  f.begin(f.b);
  REQUIRE_MESSAGE(sm3::drop_column(f.b, f.t, "y") == 0, sm1::errmsg());
  REQUIRE(sm3::abort(f.b) == 0);

  f.begin(f.a);
  REQUIRE(sm3::drop_view(f.a, tv) == 0);
  REQUIRE(sm3::abort(f.a) == 0);
  f.begin(f.b);
  fails(sm3::drop_view(f.b, pv), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::abort(f.b) == 0);

  f.begin(f.a);
  REQUIRE(sm3::replace_view(f.a, tv, view_of({})) == 0);
  f.commit(f.a);
  CHECK(readers(f.b, f.t) == std::vector<uint64_t>{pv});
  f.begin(f.b);
  REQUIRE(sm3::drop_view(f.b, pv) == 0);
  REQUIRE(sm3::drop_table(f.b, f.t) == 0);
  f.commit(f.b);
}

TEST_CASE("a temporary view's create and another session's drop wait") {
  // Verify no drop misses a temporary view made on its table, and no
  // temporary view is made on a table dropped (VIEW.md §12.3).
  // 1. A makes temporary tv on t; B drops t, in a thread: it waits,
  //    and once A commits fails with SM_ERR_INUSE.
  // 2. A drops tv; B drops t; A makes temporary tw on t, in a thread:
  //    it waits, and once B commits fails with SM_ERR_NOTFOUND.
  views_t f;
  uint64_t tv = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{f.t, 0}}), tv) ==
          0);
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

  f.begin(f.a);
  REQUIRE(sm3::drop_view(f.a, tv) == 0);
  f.commit(f.a);
  f.begin(f.b);
  REQUIRE(sm3::drop_table(f.b, f.t) == 0);
  f.begin(f.a);
  returned = false;
  uint64_t tw = 0;
  std::thread ta([&] {
    rc = sm3::create_view(f.a, sm3::nsp_temp, "tw", view_of({{f.t, 0}}), tw);
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

TEST_CASE("a session's close takes its temporary views out of the register") {
  // Verify a closed session's temporary views hold nothing (VIEW.md
  // §12.3).
  // 1. A makes temporary tv on t, and commits.
  // 2. A closes: B finds no reader of t, and drops it.
  views_t f;
  uint64_t tv = 0;
  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{f.t, 0}}), tv) ==
          0);
  f.commit(f.a);
  CHECK(readers(f.b, f.t) == std::vector<uint64_t>{tv});

  sm3::session_close(f.a);
  f.a = nullptr;
  CHECK(readers(f.b, f.t).empty());
  f.begin(f.b);
  REQUIRE(sm3::drop_table(f.b, f.t) == 0);
  f.commit(f.b);
}

TEST_CASE("another session's drop cascades to its temporary views") {
  // Verify another session drops a temporary view, as PostgreSQL's
  // DROP ... CASCADE does (VIEW.md §12.4).
  // 1. A makes pv over t; temporary tv over pv, a permanent view;
  //    temporary tw over tv, a temporary view; and temporary tx
  //    over t.
  // 2. B finds them as readers; it cannot drop tv while tw reads it,
  //    nor pv, nor t; its drops rolled back, by abort and to a
  //    savepoint, leave A's views as they were.
  // 3. B drops tw, tv, pv, tx and t, readers first, and commits: A,
  //    in a transaction begun before, sees its views no more, by oid,
  //    by name or listed; its next transaction has none; their names
  //    are free again.
  views_t f;
  f.begin(f.a);
  uint64_t pv = make_view(f.a, "pv", {{f.t, 0}});
  uint64_t tv = 0, tw = 0, tx = 0, oid = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{pv, -1}}), tv) ==
          0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tw", view_of({{tv, -1}}), tw) ==
          0);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tx", view_of({{f.t, 1}}), tx) ==
          0);
  f.commit(f.a);

  CHECK(readers(f.b, f.t) == std::vector<uint64_t>{pv, tx});
  CHECK(readers(f.b, pv) == std::vector<uint64_t>{tv});
  CHECK(readers(f.b, tv) == std::vector<uint64_t>{tw});
  f.begin(f.b);
  fails(sm3::drop_view(f.b, tv), sm1::SM_ERR_INUSE);
  fails(sm3::drop_view(f.b, pv), sm1::SM_ERR_INUSE);
  REQUIRE(sm3::drop_view(f.b, tw) == 0);
  REQUIRE(sm3::drop_view(f.b, tv) == 0);
  CHECK(readers(f.b, pv).empty());
  fails(sm3::drop_view(f.b, tv), sm1::SM_ERR_NOTFOUND);
  REQUIRE(sm3::abort(f.b) == 0);
  f.begin(f.b);
  int sp = sm3::savepoint(f.b);
  REQUIRE(sp > 0);
  REQUIRE(sm3::drop_view(f.b, tw) == 0);
  REQUIRE(sm3::rollback(f.b, sp) == 0);
  f.commit(f.b);
  CHECK(readers(f.b, tv) == std::vector<uint64_t>{tw});
  f.begin(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp) ==
        std::vector<std::string>{"tv", "tw", "tx"});
  f.commit(f.a);

  f.begin(f.a);
  f.begin(f.b);
  REQUIRE(sm3::drop_view(f.b, tw) == 0);
  REQUIRE(sm3::drop_view(f.b, tv) == 0);
  REQUIRE(sm3::drop_view(f.b, pv) == 0);
  REQUIRE(sm3::drop_view(f.b, tx) == 0);
  REQUIRE_MESSAGE(sm3::drop_table(f.b, f.t) == 0, sm1::errmsg());
  f.commit(f.b);
  CHECK(sm3::cat_get_view(f.a, tw) == nullptr);
  CHECK_FALSE(sm3::cat_get_view(f.a, sm3::nsp_temp, "tv", oid));
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp).empty());
  f.commit(f.a);
  f.begin(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp).empty());
  CHECK(sm3::cat_get_view(f.a, tv) == nullptr);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({}), oid) == 0);
  f.commit(f.a);
  CHECK(readers(f.b, pv).empty());
}

TEST_CASE("a temporary view made on one another session drops goes too") {
  // Verify a temporary view made on a doomed one, while the drop was
  // under way, goes with it (VIEW.md §12.4).
  // 1. A makes pv over t and temporary tv over pv.
  // 2. A makes temporary tw over tv, and has not committed; B drops tv
  //    and pv, and commits; then A commits.
  // 3. A sees neither tv nor tw at once, nor in its next transaction,
  //    and B finds no reader of t.
  views_t f;
  f.begin(f.a);
  uint64_t pv = make_view(f.a, "pv", {{f.t, 0}});
  uint64_t tv = 0, tw = 0;
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tv", view_of({{pv, -1}}), tv) ==
          0);
  f.commit(f.a);

  f.begin(f.a);
  REQUIRE(sm3::create_view(f.a, sm3::nsp_temp, "tw", view_of({{tv, -1}}), tw) ==
          0);
  f.begin(f.b);
  REQUIRE(sm3::drop_view(f.b, tv) == 0);
  REQUIRE(sm3::drop_view(f.b, pv) == 0);
  f.commit(f.b);
  f.commit(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp).empty());
  CHECK(sm3::cat_get_view(f.a, tw) == nullptr);

  f.begin(f.a);
  CHECK(sm3::cat_list_views(f.a, sm3::nsp_temp).empty());
  f.commit(f.a);
  CHECK(readers(f.b, tv).empty());
  CHECK(readers(f.b, f.t).empty());
}
