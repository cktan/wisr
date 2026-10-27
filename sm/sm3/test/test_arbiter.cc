/*
 * test_arbiter.cc -- tbl_insert_arbiter (SM3_DESIGN.md §3): an
 * insert that finds a row with its key before it writes, as SQL's
 * INSERT ... ON CONFLICT needs.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A row of kv as the datums a write takes. */
struct row_t {
  /* The row (id, name, v); a nullopt name is NULL. */
  row_t(int32_t id, std::optional<std::string> name, int32_t v)
      : m_name(sm2::make_varlena(name.value_or(""))) {
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_name.data());
    m_d[2] = sm2::datum_from_int32(v);
    m_null[1] = !name;
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_name;
  sm2::datum_t m_d[3];
  bool m_null[3] = {false, false, false};
};

/*
 * A catalog with kv (id int4, name text, v int4), its unique
 * indexes kv_id on (id) and kv_name on (name), an index kv_v on
 * (v) that is not unique, and sessions a and b over it.
 */
class kv_t : public db_t {
public:
  /* Initialize and load the catalog, create kv, open a and b. */
  kv_t() {
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, open(), a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"name", sm2::type_text.code, -1, false},
        {"v", sm2::type_int4.code, -1, false}};
    auto on = [](int32_t att) {
      return std::vector<sm3::index_key_spec_t>{
          {att, false, false, std::nullopt}};
    };
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "kv", cols, kv) == 0);
    REQUIRE(sm3::create_index(a, kv, "kv_id", on(0), {}, true, kv_id) == 0);
    REQUIRE(sm3::create_index(a, kv, "kv_name", on(1), {}, true, kv_name) == 0);
    REQUIRE(sm3::create_index(a, kv, "kv_v", on(2), {}, false, kv_v) == 0);
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~kv_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }

  /* kv, opened to write in s's transaction; it must open. */
  sm3::table_t *write(sm3::session_t *s) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, kv, sm1::SM_LOCKMODE_IX, t) == 0,
                    sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t kv = 0, kv_id = 0, kv_name = 0, kv_v = 0;
};

/* Begin a transaction on s, at READ COMMITTED unless told. */
void start(sm3::session_t *s,
           sm1::xctlevel_t level = sm1::xctlevel_t::read_committed) {
  REQUIRE(sm3::begin(s, level) == 0);
}

/* tbl_insert_arbiter of the row through t, with arbiters; its return. */
int upsert(sm3::table_t *t, int32_t id, std::optional<std::string> name,
           uint64_t &rowno, std::vector<uint64_t> arbiters = {}) {
  row_t r(id, name, id * 10);
  return sm3::tbl_insert_arbiter(t, r.d(), r.null(), arbiters, rowno);
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

} // namespace

TEST_CASE("an insert with arbiters finds the row that has its key, and "
          "writes nothing") {
  // Verify tbl_insert_arbiter's two answers.
  // 1. A row whose keys are free is inserted: 0, and a new rowno.
  // 2. A row with that id and another name is not: 1, with the
  //    first row's rowno, and the table still has one row.
  // 3. With no arbiter named, every unique index is one: a row
  //    with another id and that name is 1, with the same rowno.
  // 4. With kv_id alone the arbiter, that row passes kv_id and
  //    fails kv_name as any insert does: SM_ERR_DUPKEY.
  // 5. A NULL name is no other row's: two rows with NULL names
  //    are both inserted.
  // 6. An arbiter that is not a unique index of the table is
  //    SM_ERR_INVALID.
  kv_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t first = 0, rowno = 0;
  CHECK(upsert(t, 1, "one", first) == 0);
  CHECK(upsert(t, 1, "uno", rowno) == 1);
  CHECK(rowno == first);
  CHECK(count(t) == 1);
  CHECK(upsert(t, 2, "one", rowno) == 1);
  CHECK(rowno == first);
  CHECK(count(t) == 1);
  CHECK(upsert(t, 3, std::nullopt, rowno) == 0);
  CHECK(upsert(t, 4, std::nullopt, rowno) == 0);
  CHECK(count(t) == 3);
  CHECK(upsert(t, 5, "five", rowno, {f.kv_v}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(upsert(t, 2, "one", rowno, {f.kv_id}) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("an insert with arbiters waits for a key's writer, and then sees "
          "its row") {
  // Verify what the lock on the key does between two sessions.
  // 1. Session a inserts id 5 and does not commit. Session b's
  //    insert of id 5 would wait: under qa_nowait it is
  //    SM_ERR_BLOCKED, and b's transaction goes on.
  // 2. a commits: b's insert is 1, with a's rowno.
  // 3. a inserts id 6 and aborts: b's insert of id 6 is 0.
  // 4. b, not committed, holds key 6: a's plain insert of id 6
  //    would wait.
  kv_t f;
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
  start(f.a);
  start(f.b);
  sm3::table_t *ta = f.write(f.a), *tb = f.write(f.b);
  uint64_t ra = 0, rb = 0;
  REQUIRE(upsert(ta, 5, "five", ra) == 0);
  CHECK(upsert(tb, 5, "cinq", rb) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(upsert(tb, 5, "cinq", rb) == 1);
  CHECK(rb == ra);

  start(f.a);
  ta = f.write(f.a);
  REQUIRE(upsert(ta, 6, "six", ra) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::abort(f.a) == 0);
  CHECK(upsert(tb, 6, "six", rb) == 0);

  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.a), true) == 0);
  start(f.a);
  ta = f.write(f.a);
  row_t six(6, "sechs", 0);
  CHECK(sm3::tbl_insert(ta, six.d(), six.null(), ra) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(ta);
  sm3::abort(f.a);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a conflict leaves nothing locked, and a key lost in a race is "
          "found") {
  // Verify what an insert with arbiters holds, and what it does
  // when the key is taken while it waits.
  // 1. A row with id 1 is committed. Session a's insert of id 1 is
  //    a conflict. Session b, which waits for nothing, then deletes
  //    that row: a held neither the row nor its key.
  // 2. Session a inserts name "x" under id 50 and does not commit.
  //    A thread running b inserts id 51 with name "x", every unique
  //    index an arbiter: it passes kv_id, and waits at kv_name.
  // 3. a commits: b's call is a conflict, with a's rowno, and what
  //    it had put into kv_id is taken back: b's later insert of id
  //    51 under another name is inserted, not a duplicate.
  // 4. A row committed before b's statement began is deleted by a,
  //    and committed, while b's statement runs: b's insert of its
  //    key does not go on finding the row its snapshot shows, but
  //    inserts.
  kv_t f;
  uint64_t first = 0, rowno = 0;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  REQUIRE(upsert(ta, 1, "one", first) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  ta = f.write(f.a);
  CHECK(upsert(ta, 1, "uno", rowno) == 1);
  CHECK(rowno == first);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_delete(tb, first) == 0, sm1::errmsg());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), false) == 0);

  uint64_t held = 0;
  REQUIRE(upsert(ta, 50, "x", held) == 0);
  int rc = -2;
  uint64_t found = 0;
  std::string error;
  start(f.b);
  tb = f.write(f.b);
  std::thread racer([&] {
    rc = upsert(tb, 51, "x", found);
    if (rc < 0) {
      error = sm1::errmsg();
    }
  });
  // Long enough for the racer to reach its wait at kv_name.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  racer.join();
  CHECK_MESSAGE(rc == 1, error);
  CHECK(found == held);
  CHECK(upsert(tb, 51, "y", rowno) == 0);
  CHECK(count(tb) == 2);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  // b's statement begins, and reads, while id 50 is there.
  start(f.b);
  REQUIRE(sm3::stmt_begin(f.b) == 0);
  tb = f.write(f.b);
  CHECK(count(tb) == 2);
  start(f.a);
  ta = f.write(f.a);
  REQUIRE_MESSAGE(sm3::tbl_delete(ta, held) == 0, sm1::errmsg());
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(count(tb) == 2);
  CHECK(upsert(tb, 50, "fifty", rowno) == 0);
  CHECK(rowno != held);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("above READ COMMITTED a key taken since the snapshot fails the "
          "insert") {
  // Verify the isolation an arbiter's lock keeps.
  // 1. Session a begins at REPEATABLE READ and reads the table.
  // 2. Session b inserts id 9 and commits.
  // 3. a's insert of id 9 cannot say "a row has it", which a's
  //    snapshot does not show, nor insert a second: it fails with
  //    SM_ERR_CONFLICT, and a's transaction is aborted.
  // 4. A row there before the snapshot is found, at each level
  //    above READ COMMITTED, and a free key inserted.
  kv_t f;
  uint64_t old = 0;
  {
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    REQUIRE(upsert(t, 1, "one", old) == 0);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::repeatable_read, sm1::xctlevel_t::serializable}) {
    start(f.a, level);
    sm3::table_t *t = f.write(f.a);
    uint64_t found = 0;
    CHECK(upsert(t, 1, "uno", found) == 1);
    CHECK(found == old);
    CHECK(upsert(t, 100 + static_cast<int>(level), std::nullopt, found) == 0);
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);
  }
  start(f.a, sm1::xctlevel_t::repeatable_read);
  sm3::table_t *ta = f.write(f.a);
  CHECK(count(ta) == 1);
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  uint64_t rowno = 0;
  REQUIRE(upsert(tb, 9, "nine", rowno) == 0);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  CHECK(upsert(ta, 9, "neuf", rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  sm3::tbl_close(ta);
}

TEST_CASE("a row the statement inserted is found by an insert with "
          "arbiters, and refused to a lock as read") {
  // Verify that the statement's own later change of a row stops
  // SQL's row lock (tbl_lock, sm1::lock's as_read) and not the
  // lock an insert with arbiters takes to read the key's newest
  // version: digres's FOR UPDATE cursor and ON CONFLICT (dg
  // WORKNOTE.md step 305).
  // At each level:
  // 1. In a statement, insert id 1, then insert with arbiters a
  //    row of id 1 again: 1, with the first row's rowno; above
  //    READ COMMITTED too, where the arbiter reads the newest
  //    version only as the transaction's own (step 306).
  // 2. Lock the row as SQL does: SM_ERR_SELFMODIFIED, and the
  //    transaction goes on.
  // 3. In a new statement, lock it again: 0.
  kv_t f;
  for (sm1::xctlevel_t level :
       {sm1::xctlevel_t::read_committed, sm1::xctlevel_t::repeatable_read,
        sm1::xctlevel_t::serializable}) {
    CAPTURE(static_cast<int>(level));
    start(f.a, level);
    sm3::table_t *t = f.write(f.a);
    uint64_t first = 0, found = 0;
    REQUIRE(sm3::stmt_begin(f.a) == 0);
    REQUIRE(upsert(t, 1, "one", first) == 0);
    CHECK_MESSAGE(upsert(t, 1, "uno", found) == 1, std::string(sm1::errmsg()));
    CHECK(found == first);
    CHECK(sm3::tbl_lock(t, first, sm1::SM_LOCKMODE_EX) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
    REQUIRE(sm3::stmt_begin(f.a) == 0);
    CHECK_MESSAGE(sm3::tbl_lock(t, first, sm1::SM_LOCKMODE_EX) == 0,
                  std::string(sm1::errmsg()));
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);
  }
}
