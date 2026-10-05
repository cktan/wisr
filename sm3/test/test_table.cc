/*
 * test_table.cc -- a table's writes and its indexes (SM3_DESIGN.md
 * §3): every write keeps each index in step with the heap, an
 * update leaves an unchanged index alone, and at READ COMMITTED
 * the entries removed are the newest image's.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

/* A row of jobs as the Datums and null flags a write takes. */
struct row_t {
  /* The row (id, state, owner), a nullopt being NULL. */
  row_t(int32_t id, std::optional<std::string> state,
        std::optional<std::string> owner) {
    m_state = sm2::make_varlena(state.value_or(""));
    m_owner = sm2::make_varlena(owner.value_or(""));
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_state.data());
    m_d[2] = sm2::datum_from_ptr(m_owner.data());
    m_null[0] = false;
    m_null[1] = !state;
    m_null[2] = !owner;
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_state, m_owner;
  sm2::datum_t m_d[3];
  bool m_null[3];
};

/*
 * A catalog with jobs (id int4, state text, owner text), its
 * indexes jobs_state on (state) INCLUDE (owner) and jobs_id, unique,
 * on (id), and sessions a and b over it.
 */
class jobs_t : public db_t {
public:
  /* Initialize and load the catalog, create jobs, open a and b. */
  jobs_t() {
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_init(s) == 0);
    REQUIRE(sm3::cat_open(s, cat) == 0);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"state", sm2::type_text.code, -1, false},
        {"owner", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_state = {
        {1, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_id = {
        {0, false, false, std::nullopt}};
    std::vector<int32_t> owner = {2};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::cat_create_table(a, sm3::nsp_public, "jobs", cols, jobs) == 0);
    REQUIRE(sm3::cat_create_index(a, jobs, "jobs_state", by_state, owner, false,
                                  jobs_state) == 0);
    REQUIRE(sm3::cat_create_index(a, jobs, "jobs_id", by_id, {}, true,
                                  jobs_id) == 0);
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~jobs_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::cat_close(cat);
  }

  /* jobs, opened to write in s's transaction; it must open. */
  sm3::table_t *
  write(sm3::session_t *s,
        sm1::lockmode_t mode = sm1::lockmode_t::intent_exclusive) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, jobs, mode, t) == 0, sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t jobs = 0, jobs_state = 0, jobs_id = 0;
};

/* Begin a transaction on s, at READ COMMITTED unless told. */
void start(sm3::session_t *s,
           sm1::xctlevel_t level = sm1::xctlevel_t::read_committed) {
  REQUIRE(sm3::begin(s, level) == 0);
}

/* The three isolation levels, for a test that runs at each. */
const sm1::xctlevel_t all_levels[] = {sm1::xctlevel_t::read_committed,
                                      sm1::xctlevel_t::repeatable_read,
                                      sm1::xctlevel_t::serializable};

/* The two levels at which a lost race aborts the loser. */
const sm1::xctlevel_t snapshot_levels[] = {sm1::xctlevel_t::repeatable_read,
                                           sm1::xctlevel_t::serializable};

/* A text datum's string, or "-" for NULL. */
std::string text_of(sm2::datum_t d, bool isnull) {
  if (isnull) {
    return "-";
  }
  std::string_view v;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, d, v) == 0);
  return std::string(sm2::varlena_payload(v));
}

/* A jobs tuple as "id|state|owner". */
std::string show(const sm3::table_t *t, std::string_view tuple) {
  sm2::datum_t d[3];
  bool n[3];
  REQUIRE(sm2::tup_deform(sm3::tbl_def(t).desc, tuple, d, n) == 0);
  return std::to_string(sm2::datum_to_int32(d[0])) + "|" + text_of(d[1], n[1]) +
         "|" + text_of(d[2], n[2]);
}

/* The row at rowno as t's transaction sees it, or "<none>". */
std::string get(sm3::table_t *t, uint64_t rowno) {
  std::string_view tuple;
  int rc = sm3::tbl_read(t, rowno, tuple);
  REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
  return rc == 1 ? "<none>" : show(t, tuple);
}

/* The newest committed version of the row at rowno, or "<none>". */
std::string latest(sm3::table_t *t, uint64_t rowno) {
  std::string_view tuple;
  int rc = sm3::tbl_read_latest(t, rowno, tuple);
  REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
  return rc == 1 ? "<none>" : show(t, tuple);
}

/* Every row of t, as (rowno, tuple) in rowno order. */
std::vector<std::pair<uint64_t, std::string>> rows(sm3::table_t *t) {
  std::vector<std::pair<uint64_t, std::string>> out;
  sm2::rel_scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
  uint64_t rowno;
  std::string_view tuple;
  int rc;
  while ((rc = sm2::rel_scannxt(scan, rowno, tuple)) == 0) {
    out.emplace_back(rowno, std::string(tuple));
  }
  sm2::rel_scanend(scan);
  REQUIRE(rc == 1);
  return out;
}

/*
 * Check every index of t against a brute-force scan of its heap:
 * an index holds as many entries as the heap has rows, and each row
 * has, under its own key, an entry with its rowno whose INCLUDE
 * columns are the row's.
 */
void check_agree(sm3::table_t *t) {
  const sm3::table_def_t &def = sm3::tbl_def(t);
  std::vector<std::pair<uint64_t, std::string>> heap = rows(t);
  for (const sm3::index_def_t &x : def.indexes) {
    CAPTURE(x.name);
    // The entry's value: rowno, cut, then the INCLUDE columns.
    std::vector<sm2::attribute_t> vschema = {
        {&sm2::type_int8, -1, false, std::nullopt},
        {&sm2::type_bool, -1, false, std::nullopt}};
    vschema.insert(vschema.end(), x.include.begin(), x.include.end());
    sm2::tup_desc_t vdesc(vschema);
    // As many entries as rows.
    sm2::index_scan_t *scan = nullptr;
    REQUIRE(sm3::tbl_idx_scan(t, x.oid, nullptr, nullptr, scan) == 0);
    size_t entries = 0;
    uint64_t rowno;
    bool cut;
    std::string_view value;
    while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
      entries++;
    }
    sm2::idx_scanend(scan);
    CHECK(entries == heap.size());
    // Each row is found under its own key, with its own INCLUDEs.
    for (const auto &[want, tuple] : heap) {
      CAPTURE(want);
      std::vector<sm2::datum_t> d(def.schema.size());
      std::unique_ptr<bool[]> n(new bool[def.schema.size()]);
      REQUIRE(sm2::tup_deform(def.desc, tuple, d,
                              {n.get(), def.schema.size()}) == 0);
      std::vector<sm2::datum_t> kd;
      std::unique_ptr<bool[]> kn(new bool[x.key_attnums.size()]);
      for (size_t i = 0; i < x.key_attnums.size(); i++) {
        kd.push_back(d[x.key_attnums[i]]);
        kn[i] = n[x.key_attnums[i]];
      }
      sm2::bound_t eq{kd, {kn.get(), kd.size()}, true};
      REQUIRE(sm3::tbl_idx_scan(t, x.oid, &eq, &eq, scan) == 0);
      bool found = false;
      while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
        if (rowno != want) {
          continue;
        }
        found = true;
        for (size_t i = 0; i < x.include_attnums.size(); i++) {
          sm2::datum_t inc;
          bool inc_null;
          REQUIRE(sm2::tup_decode(vdesc, value, 2 + i, inc, inc_null) == 0);
          int32_t att = x.include_attnums[i];
          CHECK(text_of(inc, inc_null) == text_of(d[att], n[att]));
        }
      }
      sm2::idx_scanend(scan);
      CHECK(found);
    }
  }
}

/* The rownos index oid of t holds under the text key, in order. */
std::vector<uint64_t> under(sm3::table_t *t, uint64_t oid,
                            const std::string &key) {
  std::string v = sm2::make_varlena(key);
  sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
  bool n[1] = {false};
  sm2::bound_t eq{d, n, true};
  sm2::index_scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_idx_scan(t, oid, &eq, &eq, scan) == 0);
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

/* Insert (id, state, owner) into t and return its rowno. */
uint64_t put(sm3::table_t *t, int32_t id, std::optional<std::string> state,
             std::optional<std::string> owner) {
  row_t r(id, std::move(state), std::move(owner));
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d(), r.null(), rowno) == 0,
                  sm1::errmsg());
  return rowno;
}

/* Replace the row at rowno with (id, state, owner); tbl_update's result. */
int set(sm3::table_t *t, uint64_t rowno, int32_t id,
        std::optional<std::string> state, std::optional<std::string> owner,
        std::string *old = nullptr) {
  row_t r(id, std::move(state), std::move(owner));
  return sm3::tbl_update(t, rowno, r.d(), r.null(), old);
}

} // namespace

TEST_CASE("a table's writes keep every index in step with its heap") {
  // Verify insert, update and delete maintain both indexes.
  // 1. Insert three rows, one with a NULL state: the indexes agree
  //    with the heap.
  // 2. Update a key column: the entry moves from the old key to the
  //    new one, and the old image is handed back.
  // 3. Update an INCLUDE column only: the entry carries the new value.
  // 4. Delete a row: its entries go, and the old image is handed back.
  // 5. All of it holds after commit, in a new transaction.
  // 6. The same at each isolation level.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a, level);
    sm3::table_t *t = f.write(f.a);
    uint64_t r7 = put(t, 7, "free", std::nullopt);
    uint64_t r8 = put(t, 8, "free", std::nullopt);
    uint64_t r9 = put(t, 9, std::nullopt, "w9");
    check_agree(t);
    CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r7, r8});

    std::string old;
    REQUIRE_MESSAGE(set(t, r7, 7, "taken", "w1", &old) == 0, sm1::errmsg());
    CHECK(show(t, old) == "7|free|-");
    CHECK(get(t, r7) == "7|taken|w1");
    check_agree(t);
    CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r8});
    CHECK(under(t, f.jobs_state, "taken") == std::vector<uint64_t>{r7});

    REQUIRE(set(t, r7, 7, "taken", "w2") == 0);
    check_agree(t);

    REQUIRE(sm3::tbl_delete(t, r9, &old) == 0);
    CHECK(show(t, old) == "9|-|w9");
    CHECK(get(t, r9) == "<none>");
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.b, level);
    t = f.write(f.b, sm1::lockmode_t::intent_shared);
    CHECK(rows(t).size() == 2);
    CHECK(get(t, r7) == "7|taken|w2");
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("an update leaves alone an index whose columns did not change") {
  // Verify the skip, seen through the lock a rewritten entry takes:
  // a session set qa_nowait that inserts a duplicate id is told
  // SM_ERR_DUPKEY if jobs_id's entry was left alone, and
  // SM_ERR_BLOCKED if A rewrote it and holds its lock.
  // 1. A inserts row 7 and commits.
  // 2. A updates state and owner, not id. B's insert of id 7 fails
  //    with SM_ERR_DUPKEY: jobs_id was not touched.
  // 3. A updates id to 70. B's insert of id 7 now fails with
  //    SM_ERR_BLOCKED: the entry for 7 is A's to remove.
  // 4. A commits; the indexes agree with the heap.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r7 = put(ta, 7, "free", std::nullopt);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  ta = f.write(f.a);
  REQUIRE(set(ta, r7, 7, "taken", "w1") == 0);
  row_t dup(7, "free", std::nullopt);
  uint64_t rowno;
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  CHECK(sm3::tbl_insert(tb, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);

  REQUIRE(set(ta, r7, 70, "taken", "w1") == 0);
  start(f.b);
  tb = f.write(f.b);
  CHECK(sm3::tbl_insert(tb, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);

  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a write that fails partway is the caller's to roll back") {
  // Verify the contract: no savepoint is set inside a write.
  // 1. A inserts row 7, then sets a savepoint.
  // 2. A inserts a second row with id 7: SM_ERR_DUPKEY from jobs_id,
  //    after the heap row and its jobs_state entry went in, so the
  //    heap holds two rows.
  // 3. A rolls back to its savepoint: one row, and the indexes agree.
  // 4. A table opened intent_shared refuses a write.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 7, "free", std::nullopt);
  int sp = sm3::savepoint(f.a);
  REQUIRE(sp > 0);
  row_t dup(7, "free", std::nullopt);
  uint64_t rowno;
  CHECK(sm3::tbl_insert(t, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(rows(t).size() == 2);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  REQUIRE(sm3::forget(f.a, sp) == 0);
  CHECK(rows(t).size() == 1);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a, sm1::lockmode_t::intent_shared);
  CHECK(sm3::tbl_insert(t, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::tbl_delete(t, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("read committed: the entry removed is the newest image's") {
  // Verify the index fix-up after SM_ERR_UPDATED (SM3_DESIGN.md §3).
  // 1. Row 7 is free. A and B both read it.
  // 2. A sets it taken and holds the row; B, set qa_nowait, sets it
  //    done: SM_ERR_BLOCKED.
  // 3. A commits. B repeats: SM_ERR_UPDATED, with no index touched.
  // 4. B reads the newest image, taken, which still passes its
  //    filter state <> 'done', and repeats the write: it goes
  //    through, and the old image handed back is taken, not free.
  // 5. B commits: jobs_state holds the row under done and nowhere
  //    else, and the indexes agree with the heap.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r7 = put(ta, 7, "free", std::nullopt);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  start(f.b);
  ta = f.write(f.a);
  sm3::table_t *tb = f.write(f.b);
  CHECK(get(ta, r7) == "7|free|-");
  CHECK(get(tb, r7) == "7|free|-");
  REQUIRE(set(ta, r7, 7, "taken", "w1") == 0);
  CHECK(set(tb, r7, 7, "done", std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  CHECK(set(tb, r7, 7, "done", std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(latest(tb, r7) == "7|taken|w1");
  std::string old;
  REQUIRE_MESSAGE(set(tb, r7, 7, "done", "w1", &old) == 0, sm1::errmsg());
  CHECK(show(tb, old) == "7|taken|w1");
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  ta = f.write(f.a, sm1::lockmode_t::intent_shared);
  CHECK(get(ta, r7) == "7|done|w1");
  CHECK(under(ta, f.jobs_state, "free").empty());
  CHECK(under(ta, f.jobs_state, "taken").empty());
  CHECK(under(ta, f.jobs_state, "done") == std::vector<uint64_t>{r7});
  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("read committed: a row that stopped qualifying is claimed once") {
  // Verify the skip side of the re-check leaves the indexes alone.
  // 1. Row 7 is free. A claims it for w1 and holds the row.
  // 2. B, set qa_nowait, claims it for w2: SM_ERR_BLOCKED; A commits;
  //    B repeats: SM_ERR_UPDATED.
  // 3. B reads the newest image: taken, which fails state = 'free',
  //    so B writes nothing and commits.
  // 4. The row is w1's, under taken only, and the indexes agree.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r7 = put(ta, 7, "free", std::nullopt);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  start(f.b);
  ta = f.write(f.a);
  sm3::table_t *tb = f.write(f.b);
  REQUIRE(set(ta, r7, 7, "taken", "w1") == 0);
  CHECK(set(tb, r7, 7, "taken", "w2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(set(tb, r7, 7, "taken", "w2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(latest(tb, r7) == "7|taken|w1");
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  ta = f.write(f.a, sm1::lockmode_t::intent_shared);
  CHECK(get(ta, r7) == "7|taken|w1");
  CHECK(under(ta, f.jobs_state, "taken") == std::vector<uint64_t>{r7});
  CHECK(under(ta, f.jobs_state, "free").empty());
  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("above read committed the loser of a row race writes nothing") {
  // Verify the indexes at REPEATABLE READ and SERIALIZABLE, where a
  // lost race is a retryable error and not a re-check.
  // 1. Row 7 is free. A and B both read it.
  // 2. A sets it taken and holds the row; B, set qa_nowait, sets it
  //    done: SM_ERR_BLOCKED.
  // 3. A commits. B repeats: SM_ERR_CONFLICT, retryable, and B's
  //    transaction is gone with nothing written.
  // 4. The row is taken, under taken only, and the indexes agree.
  // 5. B retries in a new transaction, reads taken and sets it done:
  //    the entry removed is taken's, and the indexes agree.
  for (sm1::xctlevel_t level : snapshot_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a);
    sm3::table_t *ta = f.write(f.a);
    uint64_t r7 = put(ta, 7, "free", std::nullopt);
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);
    REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

    start(f.a, level);
    start(f.b, level);
    ta = f.write(f.a);
    sm3::table_t *tb = f.write(f.b);
    CHECK(get(ta, r7) == "7|free|-");
    CHECK(get(tb, r7) == "7|free|-");
    REQUIRE(set(ta, r7, 7, "taken", "w1") == 0);
    CHECK(set(tb, r7, 7, "done", std::nullopt) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);

    CHECK(set(tb, r7, 7, "done", std::nullopt) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
    CHECK(sm1::retryable() == 1);
    sm3::tbl_close(tb);
    sm3::abort(f.b);

    start(f.a, level);
    ta = f.write(f.a, sm1::lockmode_t::intent_shared);
    CHECK(get(ta, r7) == "7|taken|w1");
    CHECK(under(ta, f.jobs_state, "taken") == std::vector<uint64_t>{r7});
    CHECK(under(ta, f.jobs_state, "done").empty());
    check_agree(ta);
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.b, level);
    tb = f.write(f.b);
    CHECK(get(tb, r7) == "7|taken|w1");
    std::string old;
    REQUIRE_MESSAGE(set(tb, r7, 7, "done", "w1", &old) == 0, sm1::errmsg());
    CHECK(show(tb, old) == "7|taken|w1");
    CHECK(under(tb, f.jobs_state, "taken").empty());
    CHECK(under(tb, f.jobs_state, "done") == std::vector<uint64_t>{r7});
    check_agree(tb);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("two inserters of one unique key leave one row, at every level") {
  // Verify a race that is lost on an index, after the heap write.
  // 1. A inserts id 7 and holds its entry in jobs_id.
  // 2. B, set qa_nowait, inserts id 7: its heap row and its jobs_state
  //    entry go in, then jobs_id says SM_ERR_BLOCKED. The write set no
  //    savepoint, so B sees its own orphan row, and aborts to undo it.
  // 3. A commits. B inserts id 7 in a new transaction:
  //    SM_ERR_DUPKEY, and B aborts again.
  // 4. One row holds id 7, and the indexes agree.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
    start(f.a, level);
    sm3::table_t *ta = f.write(f.a);
    uint64_t r7 = put(ta, 7, "free", std::nullopt);

    row_t dup(7, "free", "w2");
    uint64_t rowno;
    start(f.b, level);
    sm3::table_t *tb = f.write(f.b);
    CHECK(sm3::tbl_insert(tb, dup.d(), dup.null(), rowno) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
    CHECK(rows(tb).size() == 1);
    sm3::tbl_close(tb);
    REQUIRE(sm3::abort(f.b) == 0);

    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.b, level);
    tb = f.write(f.b);
    CHECK(sm3::tbl_insert(tb, dup.d(), dup.null(), rowno) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    sm3::tbl_close(tb);
    REQUIRE(sm3::abort(f.b) == 0);

    start(f.b, level);
    tb = f.write(f.b, sm1::lockmode_t::intent_shared);
    CHECK(rows(tb).size() == 1);
    CHECK(get(tb, r7) == "7|free|-");
    check_agree(tb);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("an inserter that began before its rival committed, at every level") {
  // Verify a unique key committed after the loser began.
  // 1. B begins and reads the empty table.
  // 2. A inserts id 7 and commits.
  // 3. B inserts id 7: at READ COMMITTED it is judged against the
  //    newest version, SM_ERR_DUPKEY; above it, the key was committed
  //    after B's snapshot, SM_ERR_CONFLICT, and B is gone.
  // 4. One row holds id 7, and the indexes agree.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.b, level);
    sm3::table_t *tb = f.write(f.b);
    CHECK(rows(tb).empty());

    start(f.a, level);
    sm3::table_t *ta = f.write(f.a);
    uint64_t r7 = put(ta, 7, "free", std::nullopt);
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);

    row_t dup(7, "free", "w2");
    uint64_t rowno;
    CHECK(sm3::tbl_insert(tb, dup.d(), dup.null(), rowno) == -1);
    if (level == sm1::xctlevel_t::read_committed) {
      CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
    } else {
      CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
    }
    sm3::tbl_close(tb);
    sm3::abort(f.b);

    start(f.b, level);
    tb = f.write(f.b, sm1::lockmode_t::intent_shared);
    CHECK(rows(tb).size() == 1);
    CHECK(get(tb, r7) == "7|free|-");
    check_agree(tb);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}
