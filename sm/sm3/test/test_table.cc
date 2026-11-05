/*
 * test_table.cc -- a table's writes and its indexes (SM3_DESIGN.md
 * ../INDEXBUILD.md §2): every write keeps each index in step with the heap, an
 * update leaves an unchanged index alone, and at READ COMMITTED
 * the entries removed are the newest image's.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"
#include "sm_sys.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

/* How many times the test's evaluator has been called. */
std::atomic<int> g_evals{0};

/*
 * The evaluator the tests give the catalog, over a table of
 * (id int4, state text, owner text). An index's blob is words:
 * "free" makes it partial, on state = 'free'; "lower" gives it one
 * expression column, lower(owner); "fail" makes every call fail.
 * It asserts nothing, since a build's worker calls it too.
 */
int jobs_eval(const sm3::table_def_t &, const sm3::index_def_t &index,
              std::span<const sm2::datum_t> d, std::span<const bool> n,
              bool &belongs, std::vector<std::string> &values,
              std::span<bool> isnull) {
  g_evals++;
  if (index.eval.find("fail") != std::string::npos) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "the evaluator failed");
  }
  auto text = [&](size_t i) {
    std::string_view v;
    sm2::datum_bytes(&sm2::type_text, -1, d[i], v);
    return std::string(sm2::varlena_payload(v));
  };
  belongs = index.eval.find("free") == std::string::npos ||
            (!n[1] && text(1) == "free");
  if (index.eval.find("lower") != std::string::npos) {
    isnull[0] = n[2];
    if (!n[2]) {
      std::string o = text(2);
      for (char &c : o) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      values[0] = sm2::make_varlena(o);
    }
  }
  return 0;
}

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
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    REQUIRE(sm3::cat_set_evaluator(cat, jobs_eval) == 0);
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
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "jobs", cols, jobs) == 0);
    REQUIRE(sm3::create_index(a, jobs, "jobs_state", by_state, owner, false,
                              jobs_state) == 0);
    REQUIRE(sm3::create_index(a, jobs, "jobs_id", by_id, {}, true, jobs_id) ==
            0);
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~jobs_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }

  /*
   * Restart the database, and load the catalog and a and b again,
   * with the evaluator unless told to leave the catalog without.
   */
  void reload(bool with_evaluator = true) {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
    restart();
    sm1::session_t *s = open();
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::session_open(cat, s, a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    if (with_evaluator) {
      REQUIRE(sm3::cat_set_evaluator(cat, jobs_eval) == 0);
    }
  }

  /*
   * Attach the catalog to the builder, with sessions from this
   * database, and queue what is pending, as opening a database does.
   */
  void builder(size_t max_workers = 4, size_t batch_rows = 1000) {
    sm3::builder_set_max_workers(max_workers);
    REQUIRE(sm3::builder_attach(
                cat, [this] { return try_open(); }, batch_rows) == 0);
    REQUIRE(sm3::enqueue_pending_indexes(cat) == 0);
  }

  /* jobs, opened to write in s's transaction; it must open. */
  sm3::table_t *write(sm3::session_t *s,
                      sm1::lockmode_t mode = sm1::SM_LOCKMODE_IX) {
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
  sm3::scan_t *scan = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, scan) == 0);
  uint64_t rowno;
  std::string_view tuple;
  int rc;
  while ((rc = sm3::tbl_scannxt(scan, rowno, tuple)) == 0) {
    out.emplace_back(rowno, std::string(tuple));
  }
  sm3::tbl_scanend(scan);
  REQUIRE(rc == 1);
  return out;
}

/*
 * A scan of index oid of t read straight from sm2, in the table's
 * own session, whatever the index's state: a test looks into a
 * pending index, and into one its snapshot may not use, both of
 * which tbl_idx_scan refuses.
 */
class peek_t {
public:
  /* Open the index and a cursor between lo and hi. */
  peek_t(sm3::table_t *t, uint64_t oid, const sm2::bound_t *lo,
         const sm2::bound_t *hi) {
    const sm3::table_def_t &def = sm3::tbl_def(t);
    bool keyed = false;
    for (const sm3::index_def_t &x : def.indexes) {
      keyed |= x.primary;
    }
    for (const sm3::index_def_t &x : def.indexes) {
      if (x.oid == oid) {
        REQUIRE_FALSE(x.primary);
        REQUIRE(sm2::idx_open(sm3::session_sm1(sm3::tbl_session(t)),
                              def.storeid, x.relid, x.key, x.unique, x.include,
                              keyed, m_ix) == 0);
      }
    }
    REQUIRE(m_ix);
    REQUIRE(sm2::idx_scan(m_ix, lo, hi, m_scan) == 0);
  }
  peek_t(const peek_t &) = delete;
  peek_t &operator=(const peek_t &) = delete;

  /* Close the cursor and the index. */
  ~peek_t() {
    sm2::idx_scanend(m_scan);
    sm2::idx_close(m_ix);
  }

  /* The next entry, as sm2::idx_scannxt gives it. */
  int next(uint64_t &rowno, bool &cut, std::string_view &value) {
    return sm2::idx_scannxt(m_scan, rowno, cut, value);
  }

private:
  sm2::index_t *m_ix = nullptr;
  sm2::index_scan_t *m_scan = nullptr;
};

/*
 * Check every index of t against a brute-force scan of its heap:
 * an index holds as many entries as the heap has rows that belong
 * in it, and each such row has, under its own key, an entry with
 * its rowno whose INCLUDE columns are the row's. For a partial or
 * an expression index, whether a row belongs and what its
 * expression columns are come from the tests' evaluator.
 */
void check_agree(sm3::table_t *t) {
  const sm3::table_def_t &def = sm3::tbl_def(t);
  std::vector<std::pair<uint64_t, std::string>> heap = rows(t);
  for (const sm3::index_def_t &x : def.indexes) {
    CAPTURE(x.name);
    // A failed index is not maintained, and cannot be scanned.
    if (sm3::tbl_index_state(t, x.oid) == sm3::index_state_t::failed) {
      continue;
    }
    // The entry's value: cut, the INCLUDE columns, then the row key.
    std::vector<sm2::attribute_t> vschema = {
        {&sm2::type_bool, -1, false, std::nullopt}};
    vschema.insert(vschema.end(), x.include.begin(), x.include.end());
    vschema.push_back({&sm2::type_bytea, -1, false, std::nullopt});
    sm2::tup_desc_t vdesc(vschema);
    size_t entries = 0, belonging = 0;
    uint64_t rowno;
    bool cut;
    std::string_view value;
    {
      peek_t all(t, x.oid, nullptr, nullptr);
      while (all.next(rowno, cut, value) == 0) {
        entries++;
      }
    }
    size_t nexpr = 0;
    for (int32_t att : x.key_attnums) {
      nexpr += att < 0;
    }
    // Each row that belongs is found under its own key, with its
    // own INCLUDEs.
    for (const auto &[want, tuple] : heap) {
      CAPTURE(want);
      std::vector<sm2::datum_t> d(def.schema.size());
      std::unique_ptr<bool[]> n(new bool[def.schema.size()]);
      REQUIRE(sm2::tup_deform(def.desc, tuple, d,
                              {n.get(), def.schema.size()}) == 0);
      bool belongs = true;
      std::vector<std::string> ev(nexpr);
      std::unique_ptr<bool[]> en(new bool[nexpr]());
      if (!x.eval.empty()) {
        REQUIRE(jobs_eval(def, x, d, {n.get(), def.schema.size()}, belongs, ev,
                          {en.get(), nexpr}) == 0);
      }
      if (!belongs) {
        continue;
      }
      belonging++;
      std::vector<sm2::datum_t> kd;
      std::unique_ptr<bool[]> kn(new bool[x.key_attnums.size()]);
      size_t e = 0;
      for (size_t i = 0; i < x.key_attnums.size(); i++) {
        if (x.key_attnums[i] < 0) {
          kd.push_back(sm2::datum_from_ptr(ev[e].data()));
          kn[i] = en[e++];
        } else {
          kd.push_back(d[x.key_attnums[i]]);
          kn[i] = n[x.key_attnums[i]];
        }
      }
      sm2::bound_t eq{kd, {kn.get(), kd.size()}, true};
      peek_t scan(t, x.oid, &eq, &eq);
      bool found = false;
      while (scan.next(rowno, cut, value) == 0) {
        if (rowno != want) {
          continue;
        }
        found = true;
        for (size_t i = 0; i < x.include_attnums.size(); i++) {
          sm2::datum_t inc;
          bool inc_null;
          REQUIRE(sm2::tup_decode(vdesc, value, 1 + i, inc, inc_null) == 0);
          int32_t att = x.include_attnums[i];
          CHECK(text_of(inc, inc_null) == text_of(d[att], n[att]));
        }
      }
      CHECK(found);
    }
    CHECK(entries == belonging);
  }
}

/* The rownos index oid of t holds under the text key, in order. */
std::vector<uint64_t> under(sm3::table_t *t, uint64_t oid,
                            const std::string &key) {
  std::string v = sm2::make_varlena(key);
  sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
  bool n[1] = {false};
  sm2::bound_t eq{d, n, true};
  peek_t scan(t, oid, &eq, &eq);
  std::vector<uint64_t> out;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  while (scan.next(rowno, cut, value) == 0) {
    out.push_back(rowno);
  }
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

/*
 * Replace the row at rowno with (id, state, owner); tbl_update's
 * result. old, if not null, gets a copy of the row replaced.
 */
int set(sm3::table_t *t, uint64_t rowno, int32_t id,
        std::optional<std::string> state, std::optional<std::string> owner,
        std::string *old = nullptr) {
  row_t r(id, std::move(state), std::move(owner));
  if (!old) {
    return sm3::tbl_update(t, rowno, r.d(), r.null());
  }
  sm3::pin_ref_t ref;
  std::string_view was;
  int rc = sm3::tbl_update(t, rowno, r.d(), r.null(), ref, was);
  old->assign(was);
  sm3::unpin(ref);
  return rc;
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

    sm3::pin_ref_t ref;
    std::string_view gone;
    REQUIRE(0 == sm3::tbl_delete(t, r9, ref, gone));
    CHECK("9|-|w9" == show(t, gone));
    CHECK(get(t, r9) == "<none>");
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.b, level);
    t = f.write(f.b, sm1::SM_LOCKMODE_IS);
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
  //    which errdup() names, after the heap row and its jobs_state
  //    entry went in, so the heap holds two rows. An update of row
  //    8 to id 7 fails the same way.
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
  CHECK(sm1::errdup().index == f.jobs_id);
  CHECK(sm1::errdup().row.empty());
  CHECK(rows(t).size() == 2);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  uint64_t r8 = put(t, 8, "free", std::nullopt);
  CHECK(set(t, r8, 7, "free", std::nullopt) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(sm1::errdup().index == f.jobs_id);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  REQUIRE(sm3::forget(f.a, sp) == 0);
  CHECK(rows(t).size() == 1);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_insert(t, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::tbl_delete(t, 1) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("read committed: the entry removed is the newest image's") {
  // Verify the index fix-up after SM_ERR_UPDATED (../INDEXBUILD.md §2).
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
  ta = f.write(f.a, sm1::SM_LOCKMODE_IS);
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
  ta = f.write(f.a, sm1::SM_LOCKMODE_IS);
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
    ta = f.write(f.a, sm1::SM_LOCKMODE_IS);
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
    tb = f.write(f.b, sm1::SM_LOCKMODE_IS);
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
    tb = f.write(f.b, sm1::SM_LOCKMODE_IS);
    CHECK(rows(tb).size() == 1);
    CHECK(get(tb, r7) == "7|free|-");
    check_agree(tb);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

TEST_CASE("a row read into a pin outlives other calls on its session") {
  // Verify how long a row read into a pin is good for (include/sm.h).
  // 1. Three rows are committed.
  // 2. Rows 1 and 2 are read, each into a pin ref of its own.
  // 3. The session reads row 3 lent, inserts a row and updates row 3.
  // 4. Both pinned rows still read as they did, from the same bytes.
  // 5. Unpinning one leaves the other.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "a");
  uint64_t r2 = put(t, 2, "taken", "b");
  uint64_t r3 = put(t, 3, "free", "c");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  sm3::pin_ref_t p1, p2;
  std::string_view one, two;
  REQUIRE(sm3::tbl_read(t, r1, p1, one) == 0);
  REQUIRE(sm3::tbl_read(t, r2, p2, two) == 0);
  const char *was = one.data();

  CHECK(get(t, r3) == "3|free|c");
  put(t, 4, "free", "d");
  REQUIRE(set(t, r3, 3, "taken", "c") == 0);

  CHECK(show(t, one) == "1|free|a");
  CHECK(one.data() == was);
  CHECK(show(t, two) == "2|taken|b");
  sm3::unpin(p1);
  CHECK(show(t, two) == "2|taken|b");
  sm3::unpin(p2);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a write's old row, pinned, outlives other calls on its session") {
  // Verify tbl_update and tbl_delete hand back the row replaced pinned.
  // 1. Three rows are committed, on a table with two indexes.
  // 2. Row 1 is updated into ref pu, and row 2 deleted into ref pd:
  //    each hands back the row as it was, its indexes maintained.
  // 3. The session reads row 3, inserts a row and updates row 3.
  // 4. Both old rows still read as they did, from the same bytes.
  // 5. A failed delete into pd, of a row gone, leaves pd's row.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "a");
  uint64_t r2 = put(t, 2, "taken", "b");
  uint64_t r3 = put(t, 3, "free", "c");
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.a));

  start(f.a);
  t = f.write(f.a);
  sm3::pin_ref_t pu, pd;
  std::string_view one, two;
  row_t r(1, "taken", "a");
  REQUIRE(0 == sm3::tbl_update(t, r1, r.d(), r.null(), pu, one));
  REQUIRE(0 == sm3::tbl_delete(t, r2, pd, two));
  CHECK("1|free|a" == show(t, one));
  CHECK("2|taken|b" == show(t, two));
  const char *was = one.data();
  check_agree(t);

  CHECK("3|free|c" == get(t, r3));
  put(t, 4, "free", "d");
  REQUIRE(0 == set(t, r3, 3, "taken", "c"));

  CHECK("1|free|a" == show(t, one));
  CHECK(one.data() == was);
  CHECK("2|taken|b" == show(t, two));

  std::string_view none = "unset";
  CHECK(-1 == sm3::tbl_delete(t, r2, pd, none));
  CHECK(none.empty());
  CHECK("2|taken|b" == show(t, two));
  sm3::unpin(pu);
  sm3::unpin(pd);
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.a));
}

TEST_CASE("set_not_null makes a column NOT NULL once every row holds a "
          "value") {
  // Verify set_not_null, as ADD COLUMN with a volatile default uses it.
  // 1. Two rows are committed; then a nullable int4 column n is added,
  //    with no value for the rows already there.
  // 2. set_not_null fails with SM_ERR_NOTNULL, as both read n as null,
  //    and n stays nullable.
  // 3. Each row is given a value of n: set_not_null succeeds, and again.
  // 4. A row inserted with n null is refused; a column not there is
  //    SM_ERR_NOTFOUND.
  // 5. After the commit, the column is NOT NULL to another session.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "a");
  uint64_t r2 = put(t, 2, "taken", "b");
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.a));

  start(f.a);
  REQUIRE(0 == sm3::add_column(f.a, f.jobs,
                               {"n", sm2::type_int4.code, -1, false},
                               std::nullopt));
  CHECK(-1 == sm3::set_not_null(f.a, f.jobs, "n"));
  CHECK(sm1::SM_ERR_NOTNULL == sm1::errnum());
  t = f.write(f.a);
  CHECK(!sm3::tbl_def(t).columns[3].not_null);

  std::string state = sm2::make_varlena("s"), owner = sm2::make_varlena("o");
  auto row = [&](int32_t id, std::optional<int32_t> n) {
    return std::vector<sm2::datum_t>{sm2::datum_from_int32(id),
                                     sm2::datum_from_ptr(state.data()),
                                     sm2::datum_from_ptr(owner.data()),
                                     sm2::datum_from_int32(n.value_or(0))};
  };
  const bool some[4] = {false, false, false, false};
  const bool none[4] = {false, false, false, true};
  for (auto [rowno, id] : {std::pair{r1, 1}, std::pair{r2, 2}}) {
    std::vector<sm2::datum_t> v = row(id, id * 10);
    REQUIRE_MESSAGE(0 == sm3::tbl_update(t, rowno, v, some), sm1::errmsg());
  }
  sm3::tbl_close(t);
  REQUIRE_MESSAGE(0 == sm3::set_not_null(f.a, f.jobs, "n"), sm1::errmsg());
  CHECK(0 == sm3::set_not_null(f.a, f.jobs, "n"));

  t = f.write(f.a);
  CHECK(sm3::tbl_def(t).columns[3].not_null);
  std::vector<sm2::datum_t> v = row(3, std::nullopt);
  uint64_t r3 = 0;
  CHECK(-1 == sm3::tbl_insert(t, v, none, r3));
  CHECK(sm1::SM_ERR_NOTNULL == sm1::errnum());
  CHECK(-1 == sm3::set_not_null(f.a, f.jobs, "nope"));
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.a));

  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_def(t).columns[3].not_null);
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.b));
}

TEST_CASE("set_identity makes a filled NOT NULL column an identity column") {
  // Verify set_identity, as ADD COLUMN of an identity column uses it.
  // 1. A row is committed; a nullable int8 column g is added, and a
  //    sequence s made in public.
  // 2. While g is nullable, set_identity is SM_ERR_INVALID.
  // 3. The row is given g, g made NOT NULL: set_identity with 'a'
  //    succeeds, g is identity 'a' over s, and s is its for good:
  //    giving s another owner is SM_ERR_INUSE.
  // 4. A second set_identity is SM_ERR_INVALID; one of a column not
  //    there SM_ERR_NOTFOUND.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::commit(f.a));

  start(f.a);
  uint64_t seq = 0;
  REQUIRE(0 == sm3::add_column(f.a, f.jobs,
                               {"g", sm2::type_int8.code, -1, false},
                               std::nullopt));
  REQUIRE(0 == sm3::create_sequence(f.a, sm3::nsp_public, "s", 1, 1, seq));
  CHECK(-1 == sm3::set_identity(f.a, f.jobs, "g", 'a', seq));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());

  t = f.write(f.a);
  std::string state = sm2::make_varlena("s"), owner = sm2::make_varlena("o");
  std::vector<sm2::datum_t> v = {
      sm2::datum_from_int32(1), sm2::datum_from_ptr(state.data()),
      sm2::datum_from_ptr(owner.data()), sm2::datum_from_int64(1)};
  const bool some[4] = {false, false, false, false};
  REQUIRE_MESSAGE(0 == sm3::tbl_update(t, r1, v, some), sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(0 == sm3::set_not_null(f.a, f.jobs, "g"));
  REQUIRE_MESSAGE(0 == sm3::set_identity(f.a, f.jobs, "g", 'a', seq),
                  sm1::errmsg());
  t = f.write(f.a);
  CHECK('a' == sm3::tbl_def(t).columns[3].identity);
  CHECK(seq == sm3::tbl_def(t).columns[3].identity_seq);
  sm3::tbl_close(t);
  CHECK(-1 == sm3::own_sequence(f.a, seq, 0, -1));
  CHECK(sm1::SM_ERR_INUSE == sm1::errnum());

  CHECK(-1 == sm3::set_identity(f.a, f.jobs, "g", 'a', seq));
  CHECK(sm1::SM_ERR_INVALID == sm1::errnum());
  CHECK(-1 == sm3::set_identity(f.a, f.jobs, "nope", 'a', seq));
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  REQUIRE(0 == sm3::commit(f.a));
}

TEST_CASE("a scanned row outlives other calls on its session") {
  // Verify how long a cursor's row is good for (include/sm.h).
  // 1. Three rows are committed. A cursor over the heap is opened,
  //    and one over an index, and each gives its first row.
  // 2. The session then reads another row, inserts one, and moves a
  //    second heap cursor: none of them is a call on the first two.
  // 3. The two rows are still, byte for byte, what they were.
  // 4. Moving a cursor on gives its next row, as before.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  uint64_t r2 = put(t, 2, "taken", "b");
  put(t, 3, "free", "c");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  sm3::scan_t *heap = nullptr, *other = nullptr;
  sm2::index_scan_t *idx = nullptr;
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, heap) == 0);
  REQUIRE(sm3::tbl_idx_scan(t, f.jobs_id, nullptr, nullptr, idx) == 0);
  uint64_t rowno = 0, xrow = 0, orow = 0;
  bool cut = false;
  std::string_view row, entry, tmp;
  REQUIRE(sm3::tbl_scannxt(heap, rowno, row) == 0);
  REQUIRE(sm2::idx_scannxt(idx, xrow, cut, entry) == 0);
  const std::string row_was(row), entry_was(entry);

  REQUIRE(sm3::tbl_read(t, r2, tmp) == 0);
  put(t, 4, "free", "d");
  REQUIRE(sm3::tbl_scan(t, 0, UINT64_MAX, other) == 0);
  REQUIRE(sm3::tbl_scannxt(other, orow, tmp) == 0);
  REQUIRE(sm3::tbl_scannxt(other, orow, tmp) == 0);

  CHECK(row == row_was);
  CHECK(entry == entry_was);

  uint64_t next = 0;
  REQUIRE(sm3::tbl_scannxt(heap, next, row) == 0);
  CHECK(next > rowno);
  sm3::tbl_scanend(heap);
  sm3::tbl_scanend(other);
  sm2::idx_scanend(idx);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an index cursor begun after a savepoint fails once rolled back "
          "to it") {
  // Verify a cursor a rollback closed fails with SM_ERR_SCANCLOSED,
  // the code digres reports as the scan's old view lost.
  // 1. Two rows are committed.
  // 2. Cursor A over the index gives its first entry; a savepoint is
  //    set; cursor B over the index, begun after it, gives its first.
  // 3. The session rolls back to the savepoint.
  // 4. B's next read fails with SM_ERR_SCANCLOSED, every time, and
  //    gives no entry.
  // 5. A, begun before the savepoint, gives its second entry.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  put(t, 2, "taken", "b");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  sm2::index_scan_t *a = nullptr, *b = nullptr;
  uint64_t rowno = 0;
  bool cut = false;
  std::string_view entry;
  REQUIRE(sm3::tbl_idx_scan(t, f.jobs_id, nullptr, nullptr, a) == 0);
  REQUIRE(sm2::idx_scannxt(a, rowno, cut, entry) == 0);
  int depth = sm3::savepoint(f.a);
  REQUIRE(depth > 0);
  REQUIRE(sm3::tbl_idx_scan(t, f.jobs_id, nullptr, nullptr, b) == 0);
  REQUIRE(sm2::idx_scannxt(b, rowno, cut, entry) == 0);

  REQUIRE(sm3::rollback(f.a, depth) == 0);
  for (int i = 0; i < 2; i++) {
    entry = "unset";
    CHECK(sm2::idx_scannxt(b, rowno, cut, entry) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_SCANCLOSED);
    CHECK(std::string(sm1::errname()) == "ERR_SCANCLOSED");
    CHECK(entry.empty());
  }
  CHECK(sm2::idx_scannxt(a, rowno, cut, entry) == 0);
  CHECK(sm2::idx_scannxt(a, rowno, cut, entry) == 1);
  sm2::idx_scanend(b);
  sm2::idx_scanend(a);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("while an index is pending, rows it has not reached are written") {
  // Verify maintenance of a pending index (../INDEXBUILD.md §2).
  // 1. Rows 7, 8 and 10 are committed; then an index on owner is
  //    made, on a table committed earlier, so it is pending and
  //    holds no entry for them.
  // 2. Updating row 7 succeeds though its old entry is not there,
  //    and the row is covered from then on.
  // 3. Deleting row 8 succeeds the same way; inserting row 9 adds
  //    its entry.
  // 4. Row 10 was never touched: the index lacks it, and that is the
  //    backfill's to add.
  // 5. Once the index is marked valid, a missing old entry is an
  //    error again: updating row 10 fails with SM_ERR_NOTFOUND.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r7 = put(t, 7, "free", "a");
  uint64_t r8 = put(t, 8, "free", "b");
  uint64_t r10 = put(t, 10, "free", "c");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::vector<sm3::index_key_spec_t> by_owner = {
      {2, false, false, std::nullopt}};
  uint64_t jobs_owner = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.jobs, "jobs_owner", by_owner, {}, false,
                            jobs_owner) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  CHECK(sm3::tbl_index_state(t, sm3::tbl_def(t).indexes[2].oid) ==
        sm3::index_state_t::pending);
  CHECK(under(t, jobs_owner, "a").empty());
  REQUIRE_MESSAGE(set(t, r7, 7, "free", "z") == 0, sm1::errmsg());
  CHECK(under(t, jobs_owner, "z") == std::vector<uint64_t>{r7});
  REQUIRE_MESSAGE(sm3::tbl_delete(t, r8) == 0, sm1::errmsg());
  uint64_t r9 = put(t, 9, "free", "y");
  CHECK(under(t, jobs_owner, "y") == std::vector<uint64_t>{r9});
  CHECK(under(t, jobs_owner, "c").empty());
  sm3::tbl_close(t);
  REQUIRE(sm3::set_index_state(f.a, f.jobs, jobs_owner,
                               sm3::index_state_t::valid) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  CHECK(set(t, r10, 10, "free", "x") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);
}

TEST_CASE("a failed index is not maintained, and cannot be scanned") {
  // Verify what a failed build leaves behind (../INDEXBUILD.md §2).
  // 1. Row 7 is committed, owned by "a"; a unique index on owner is
  //    made, pending, and is marked failed.
  // 2. A second row owned by "a" inserts: the index no longer
  //    refuses it.
  // 3. Scanning the index fails with SM_ERR_INVALID.
  // 4. jobs_state is maintained as before, and agrees with the heap.
  // 5. A failed index's state cannot be set again.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 7, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::vector<sm3::index_key_spec_t> by_owner = {
      {2, false, false, std::nullopt}};
  uint64_t owner_u = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.jobs, "jobs_owner_u", by_owner, {}, true,
                            owner_u) == 0);
  REQUIRE(sm3::set_index_state(f.a, f.jobs, owner_u,
                               sm3::index_state_t::failed) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a);
  put(t, 8, "free", "a");
  CHECK(rows(t).size() == 2);
  sm2::index_scan_t *scan = nullptr;
  CHECK(sm3::tbl_idx_scan(t, owner_u, nullptr, nullptr, scan) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::set_index_state(f.a, f.jobs, owner_u, sm3::index_state_t::valid) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

namespace {

/* An index on owner, made on jobs after its rows: pending. */
uint64_t late_index(jobs_t &f, bool unique = false) {
  std::vector<sm3::index_key_spec_t> by_owner = {
      {2, false, false, std::nullopt}};
  uint64_t oid = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.jobs, "jobs_owner", by_owner, {}, unique,
                            oid) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  return oid;
}

/* Mark index oid of jobs valid, in a transaction of s's own. */
void validate(jobs_t &f, sm3::session_t *s, uint64_t oid) {
  start(s);
  REQUIRE(sm3::set_index_state(s, f.jobs, oid, sm3::index_state_t::valid) == 0);
  REQUIRE(sm3::commit(s) == 0);
}

} // namespace

TEST_CASE("a build fills an index in batches, and it then agrees") {
  // Verify the backfill on a table nobody else is writing.
  // 1. Five rows are committed, then an index on owner is made.
  // 2. Batches of two cover rows 1-2, 3-4, then 5: the last says done.
  // 3. A batch run again over rows already covered adds nothing and
  //    fails nothing.
  // 4. Marked valid, the index agrees with the heap.
  // 5. A batch on an index that is not pending is SM_ERR_INVALID,
  //    and on one the table does not have, SM_ERR_NOTFOUND.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 5; i++) {
    put(t, i, "free", "w" + std::to_string(i));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);

  std::string next;
  bool done = true;
  start(f.b);
  t = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_index_backfill(t, jobs_owner, "", 2, next, done) ==
                      0,
                  sm1::errmsg());
  CHECK(next == sm2::id_key(2) + '\0');
  CHECK_FALSE(done);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
  start(f.b);
  t = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(t, jobs_owner, next, 2, next, done) == 0);
  CHECK(next == sm2::id_key(4) + '\0');
  CHECK_FALSE(done);
  REQUIRE(sm3::tbl_index_backfill(t, jobs_owner, next, 2, next, done) == 0);
  CHECK(done);
  REQUIRE(sm3::tbl_index_backfill(t, jobs_owner, "", 100, next, done) == 0);
  CHECK(done);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
  validate(f, f.b, jobs_owner);

  start(f.a);
  t = f.write(f.a);
  CHECK(under(t, jobs_owner, "w3") == std::vector<uint64_t>{3});
  check_agree(t);
  CHECK(sm3::tbl_index_backfill(t, jobs_owner, "", 2, next, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm3::tbl_index_backfill(t, jobs_owner + 999, "", 2, next, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a build waits for a writer, and finds the row covered") {
  // Verify the race the row lock settles, the writer first.
  // 1. Rows 1 and 2 are committed; an index on owner is pending.
  // 2. A updates row 1's owner and holds the row. B, the build, set
  //    qa_nowait, runs a batch: SM_ERR_BLOCKED on row 1, and it
  //    aborts the batch.
  // 3. A commits. B runs the batch again: row 1's entry is the one A
  //    made, row 2's is added.
  // 4. Marked valid, the index agrees with the heap, and holds
  //    nothing under row 1's old owner.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "old");
  uint64_t r2 = put(ta, 2, "free", "two");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  ta = f.write(f.a);
  REQUIRE(set(ta, r1, 1, "free", "new") == 0);
  std::string next;
  bool done = false;
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  CHECK(sm3::tbl_index_backfill(tb, jobs_owner, "", 10, next, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  tb = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_index_backfill(tb, jobs_owner, "", 10, next, done) ==
                      0,
                  sm1::errmsg());
  CHECK(done);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  validate(f, f.b, jobs_owner);

  start(f.a);
  ta = f.write(f.a);
  CHECK(under(ta, jobs_owner, "new") == std::vector<uint64_t>{r1});
  CHECK(under(ta, jobs_owner, "old").empty());
  CHECK(under(ta, jobs_owner, "two") == std::vector<uint64_t>{r2});
  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a writer waits for a build, and removes the entry it added") {
  // Verify the race the row lock settles, the build first.
  // 1. Row 1 is committed; an index on owner is pending.
  // 2. A, a writer set qa_nowait, begins. B, the build, covers row 1
  //    and holds its lock: A's update of row 1 is SM_ERR_BLOCKED.
  // 3. B commits. A's update goes through: the heap row had not
  //    changed, and the entry B added after A began is removed all
  //    the same.
  // 4. Marked valid, the index holds row 1 under its new owner only.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "old");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.a), true) == 0);

  start(f.a);
  ta = f.write(f.a);
  CHECK(get(ta, r1) == "1|free|old");
  std::string next;
  bool done = false;
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(tb, jobs_owner, "", 10, next, done) == 0);
  CHECK(set(ta, r1, 1, "free", "new") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE_MESSAGE(set(ta, r1, 1, "free", "new") == 0, sm1::errmsg());
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  validate(f, f.b, jobs_owner);

  start(f.b);
  tb = f.write(f.b);
  CHECK(under(tb, jobs_owner, "new") == std::vector<uint64_t>{r1});
  CHECK(under(tb, jobs_owner, "old").empty());
  check_agree(tb);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a unique build fails on a duplicate it reaches late") {
  // Verify what a pending unique index cannot refuse at once.
  // 1. Rows 1 and 2 are committed with one owner; a unique index on
  //    owner is made, and is pending.
  // 2. A third row with that owner inserts: no entry stands against
  //    it yet.
  // 3. The backfill reaches the rows: SM_ERR_DUPKEY, and the batch
  //    is aborted.
  // 4. A row whose own entry a writer already made is no duplicate:
  //    with a table of distinct owners the same build succeeds.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "same");
  put(t, 2, "free", "same");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f, true);

  start(f.a);
  t = f.write(f.a);
  put(t, 3, "free", "same");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::string next;
  bool done = false;
  start(f.b);
  t = f.write(f.b);
  CHECK(sm3::tbl_index_backfill(t, jobs_owner, "", 10, next, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.b) == 0);

  jobs_t g;
  start(g.a);
  t = g.write(g.a);
  uint64_t r1 = put(t, 1, "free", "one");
  put(t, 2, "free", "two");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.a) == 0);
  jobs_owner = late_index(g, true);
  start(g.a);
  t = g.write(g.a);
  REQUIRE(set(t, r1, 1, "taken", "uno") == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.a) == 0);
  start(g.b);
  t = g.write(g.b);
  REQUIRE_MESSAGE(sm3::tbl_index_backfill(t, jobs_owner, "", 10, next, done) ==
                      0,
                  sm1::errmsg());
  CHECK(done);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.b) == 0);
  validate(g, g.b, jobs_owner);
  start(g.b);
  t = g.write(g.b);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.b) == 0);
}

TEST_CASE("an index made in its table's transaction is filled at once") {
  // Verify the inline build, at every isolation level.
  // 1. In one transaction: create a table, insert three rows, then
  //    create an index on it.
  // 2. The index is valid at once, and agrees with the heap, before
  //    the transaction commits and after.
  // 3. A unique index over two of those rows with one key fails
  //    with SM_ERR_DUPKEY.
  // 4. A table committed earlier cannot be built above READ
  //    COMMITTED: the batch is SM_ERR_INVALID.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"state", sm2::type_text.code, -1, false},
        {"owner", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_owner = {
        {2, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_state = {
        {1, false, false, std::nullopt}};
    uint64_t tasks = 0, tasks_owner = 0, tasks_state = 0;
    start(f.a, level);
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "tasks", cols, tasks) == 0);
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(f.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
    put(t, 1, "free", "a");
    put(t, 2, "free", "b");
    put(t, 3, "done", "c");
    sm3::tbl_close(t);
    REQUIRE_MESSAGE(sm3::create_index(f.a, tasks, "tasks_owner", by_owner, {},
                                      true, tasks_owner) == 0,
                    sm1::errmsg());
    CHECK(sm3::create_index(f.a, tasks, "tasks_state", by_state, {}, true,
                            tasks_state) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  }
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"state", sm2::type_text.code, -1, false},
        {"owner", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_owner = {
        {2, false, false, std::nullopt}};
    uint64_t tasks = 0, tasks_owner = 0;
    start(f.a, level);
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "tasks", cols, tasks) == 0);
    sm3::table_t *t = nullptr;
    REQUIRE(sm3::tbl_open(f.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
    put(t, 1, "free", "a");
    put(t, 2, "free", "b");
    sm3::tbl_close(t);
    REQUIRE(sm3::create_index(f.a, tasks, "tasks_owner", by_owner, {}, true,
                              tasks_owner) == 0);
    REQUIRE(sm3::tbl_open(f.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
    CHECK(sm3::tbl_index_state(t, sm3::tbl_def(t).indexes[0].oid) ==
          sm3::index_state_t::valid);
    check_agree(t);
    REQUIRE(set(t, 1, 1, "taken", "z") == 0);
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.b, level);
    REQUIRE(sm3::tbl_open(f.b, tasks, sm1::SM_LOCKMODE_IS, t) == 0);
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.b) == 0);
  }
  for (sm1::xctlevel_t level : snapshot_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    uint64_t jobs_owner = late_index(f);
    std::string next;
    bool done = false;
    start(f.b, level);
    sm3::table_t *t = f.write(f.b);
    CHECK(sm3::tbl_index_backfill(t, jobs_owner, "", 10, next, done) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.b) == 0);
  }
}

TEST_CASE("a build skips a row deleted under it, and adds back nothing") {
  // Verify deletes against a pending index, on four rows.
  // 1. Row 1: the build covers it and commits; A then deletes it, and
  //    its entry goes with it.
  // 2. Row 2: A deletes it and has not committed. The build, set
  //    qa_nowait, is SM_ERR_BLOCKED on it; A commits, and the row is
  //    gone before the build comes back.
  // 3. Row 3: a writer's update made its entry. A deletes it and has
  //    not committed; the build is blocked again; A aborts, so the
  //    row and its entry both stand.
  // 4. Row 4: the build's statement snapshot still lists it when A
  //    deletes it and commits. The build visits it, finds it gone at
  //    its newest version, and skips it.
  // 5. The build ends: only row 3 is left, with one entry, and the
  //    index agrees with the heap.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "o1");
  uint64_t r2 = put(ta, 2, "free", "o2");
  uint64_t r3 = put(ta, 3, "free", "o3");
  uint64_t r4 = put(ta, 4, "free", "o4");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
  std::string next, unused;
  bool done = false;

  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(tb, jobs_owner, "", 1, next, done) == 0);
  CHECK(next == sm2::id_key(r1) + '\0');
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  start(f.a);
  ta = f.write(f.a);
  CHECK(under(ta, jobs_owner, "o1") == std::vector<uint64_t>{r1});
  REQUIRE_MESSAGE(sm3::tbl_delete(ta, r1) == 0, sm1::errmsg());
  CHECK(under(ta, jobs_owner, "o1").empty());

  REQUIRE(sm3::tbl_delete(ta, r2) == 0);
  start(f.b);
  tb = f.write(f.b);
  CHECK(sm3::tbl_index_backfill(tb, jobs_owner, next, 10, unused, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  ta = f.write(f.a);
  REQUIRE(set(ta, r3, 3, "free", "trois") == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  ta = f.write(f.a);
  REQUIRE(sm3::tbl_delete(ta, r3) == 0);
  start(f.b);
  tb = f.write(f.b);
  CHECK(sm3::tbl_index_backfill(tb, jobs_owner, next, 10, unused, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::abort(f.a) == 0);

  start(f.b);
  tb = f.write(f.b);
  REQUIRE(sm1::stmt_begin(sm3::session_sm1(f.b)) == 0);
  CHECK(get(tb, r4) == "4|free|o4");
  start(f.a);
  ta = f.write(f.a);
  REQUIRE(sm3::tbl_delete(ta, r4) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(get(tb, r4) == "4|free|o4");
  REQUIRE_MESSAGE(
      sm3::tbl_index_backfill(tb, jobs_owner, next, 10, unused, done) == 0,
      sm1::errmsg());
  CHECK(done);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  validate(f, f.b, jobs_owner);

  start(f.a);
  ta = f.write(f.a);
  CHECK(rows(ta).size() == 1);
  CHECK(get(ta, r3) == "3|free|trois");
  CHECK(under(ta, jobs_owner, "trois") == std::vector<uint64_t>{r3});
  CHECK(under(ta, jobs_owner, "o2").empty());
  CHECK(under(ta, jobs_owner, "o3").empty());
  CHECK(under(ta, jobs_owner, "o4").empty());
  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a deleter waits for a build, then removes the entry it added") {
  // Verify a delete of a row in the build's uncommitted batch, with
  // the deleter at each isolation level.
  // 1. Row 1 is committed; an index on owner is pending. A, the
  //    deleter, set qa_nowait, begins and reads the row.
  // 2. B, the build, covers row 1 and holds its lock: A's delete is
  //    SM_ERR_BLOCKED.
  // 3. B commits. At READ COMMITTED A's delete goes through, the
  //    entry B added after A began removed with the row. Above it,
  //    removing that entry is SM_ERR_CONFLICT; A retries in a new
  //    transaction and the delete goes through.
  // 4. Marked valid, the index and the heap are both empty.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a);
    sm3::table_t *ta = f.write(f.a);
    uint64_t r1 = put(ta, 1, "free", "old");
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);
    uint64_t jobs_owner = late_index(f);
    REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.a), true) == 0);

    start(f.a, level);
    ta = f.write(f.a);
    CHECK(get(ta, r1) == "1|free|old");
    std::string next;
    bool done = false;
    start(f.b);
    sm3::table_t *tb = f.write(f.b);
    REQUIRE(sm3::tbl_index_backfill(tb, jobs_owner, "", 10, next, done) == 0);
    CHECK(sm3::tbl_delete(ta, r1) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);

    if (level == sm1::xctlevel_t::read_committed) {
      REQUIRE_MESSAGE(sm3::tbl_delete(ta, r1) == 0, sm1::errmsg());
    } else {
      CHECK(sm3::tbl_delete(ta, r1) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
      CHECK(sm1::retryable() == 1);
      sm3::tbl_close(ta);
      sm3::abort(f.a);
      start(f.a, level);
      ta = f.write(f.a);
      REQUIRE_MESSAGE(sm3::tbl_delete(ta, r1) == 0, sm1::errmsg());
    }
    sm3::tbl_close(ta);
    REQUIRE(sm3::commit(f.a) == 0);
    validate(f, f.b, jobs_owner);

    start(f.b);
    tb = f.write(f.b);
    CHECK(rows(tb).empty());
    CHECK(under(tb, jobs_owner, "old").empty());
    check_agree(tb);
    sm3::tbl_close(tb);
    REQUIRE(sm3::commit(f.b) == 0);
  }
}

namespace {

/* How many entries tbl_idx_scan yields from index oid of t, or -1. */
int scanned(sm3::table_t *t, uint64_t oid) {
  sm2::index_scan_t *scan = nullptr;
  if (sm3::tbl_idx_scan(t, oid, nullptr, nullptr, scan) != 0) {
    return -1;
  }
  int n = 0;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  while (sm2::idx_scannxt(scan, rowno, cut, value) == 0) {
    n++;
  }
  sm2::idx_scanend(scan);
  return n;
}

/* Build index oid of jobs to its end in b, and mark it valid. */
void build(jobs_t &f, uint64_t oid) {
  std::string next;
  bool done = false;
  start(f.b);
  sm3::table_t *t = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(t, oid, "", 1000, next, done) == 0);
  REQUIRE(done);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
  validate(f, f.b, oid);
}

} // namespace

TEST_CASE("a snapshot older than a build is kept off the index") {
  // Verify the guard at REPEATABLE READ and SERIALIZABLE.
  // 1. Two rows are committed; an index on owner is pending.
  // 2. A begins, which takes its snapshot, and does not touch the
  //    table: marking an index valid waits out every transaction
  //    that holds the table's oid lock, so the guard is for those
  //    that do not hold it yet. B builds the index and marks it valid.
  // 3. A opens the table and is given the valid index, but may not
  //    use it: read at A's snapshot it holds nothing, though the heap
  //    holds two rows. tbl_idx_scan refuses with SM_ERR_INVALID.
  // 4. A still maintains it: a row A inserts gets its entry.
  // 5. A's next transaction may use it, and it agrees with the heap.
  for (sm1::xctlevel_t level : snapshot_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    put(t, 1, "free", "a");
    put(t, 2, "free", "b");
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    uint64_t jobs_owner = late_index(f);

    start(f.a, level);
    build(f, jobs_owner);

    t = f.write(f.a);
    CHECK(sm3::tbl_index_state(t, sm3::tbl_def(t).indexes[2].oid) ==
          sm3::index_state_t::valid);
    CHECK_FALSE(sm3::tbl_index_usable(t, jobs_owner));
    CHECK(scanned(t, jobs_owner) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
    CHECK(under(t, jobs_owner, "a").empty());
    CHECK(rows(t).size() == 2);
    CHECK(sm3::tbl_index_usable(t, f.jobs_state));
    uint64_t r3 = put(t, 3, "free", "c");
    CHECK(under(t, jobs_owner, "c") == std::vector<uint64_t>{r3});
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);

    start(f.a, level);
    t = f.write(f.a);
    CHECK(sm3::tbl_index_usable(t, jobs_owner));
    CHECK(scanned(t, jobs_owner) == 3);
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("read committed may use an index from its next statement on") {
  // Verify the guard at READ COMMITTED, statement by statement.
  // 1. An index on owner is pending. A begins a statement and reads
  //    another table, which takes the statement's snapshot without
  //    the table-oid lock of jobs. B builds the index and marks it valid.
  // 2. In that statement A may not use it. In its next statement,
  //    begun after the index became valid, it may.
  // 3. A transaction that begins no statements reads each call at a
  //    fresh snapshot, and may use the index as soon as it is valid.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t pin = 0;
  start(f.a);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "pin", cols, pin) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  REQUIRE(sm3::stmt_begin(f.a) == 0);
  REQUIRE(sm3::tbl_open(f.a, pin, sm1::SM_LOCKMODE_IS, t) == 0);
  CHECK(rows(t).empty());
  sm3::tbl_close(t);
  build(f, jobs_owner);
  t = f.write(f.a);
  CHECK_FALSE(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == -1);
  REQUIRE(sm3::stmt_begin(f.a) == 0);
  CHECK(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == 1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  jobs_t g;
  start(g.a);
  t = g.write(g.a);
  put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.a) == 0);
  jobs_owner = late_index(g);
  start(g.a);
  build(g, jobs_owner);
  t = g.write(g.a);
  CHECK(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == 1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(g.a) == 0);
}

TEST_CASE("an index is usable by its state, and by who made it valid") {
  // Verify the rest of tbl_index_usable.
  // 1. A pending index is not usable, and its scan is refused.
  // 2. The transaction that marks it valid may use it at once,
  //    before it commits; another whose snapshot is older may not.
  // 3. A failed index is not usable.
  // 4. An index made valid in its table's own transaction is usable
  //    there at once, at every level.
  // 5. An index the table does not have is not usable.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);

  start(f.a, sm1::xctlevel_t::repeatable_read);
  t = f.write(f.a);
  CHECK_FALSE(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK_FALSE(sm3::tbl_index_usable(t, jobs_owner + 999));
  CHECK(scanned(t, jobs_owner + 999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::string next;
  bool done = false;
  start(f.b);
  t = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(t, jobs_owner, "", 10, next, done) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::set_index_state(f.b, f.jobs, jobs_owner,
                               sm3::index_state_t::valid) == 0);
  t = f.write(f.b);
  CHECK(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == 1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);

  // A second index, pending, to mark failed: a state is set once.
  std::vector<sm3::index_key_spec_t> by_state = {
      {1, false, false, std::nullopt}};
  uint64_t bad = 0;
  start(f.b);
  REQUIRE(sm3::create_index(f.b, f.jobs, "jobs_state2", by_state, {}, false,
                            bad) == 0);
  REQUIRE(sm3::set_index_state(f.b, f.jobs, bad, sm3::index_state_t::failed) ==
          0);
  t = f.write(f.b);
  CHECK_FALSE(sm3::tbl_index_usable(t, bad));
  CHECK(scanned(t, bad) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);

  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t g;
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"state", sm2::type_text.code, -1, false},
        {"owner", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_owner = {
        {2, false, false, std::nullopt}};
    uint64_t tasks = 0, tasks_owner = 0;
    start(g.a, level);
    REQUIRE(sm3::create_table(g.a, sm3::nsp_public, "tasks", cols, tasks) == 0);
    REQUIRE(sm3::tbl_open(g.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
    put(t, 1, "free", "a");
    sm3::tbl_close(t);
    REQUIRE(sm3::create_index(g.a, tasks, "tasks_owner", by_owner, {}, false,
                              tasks_owner) == 0);
    REQUIRE(sm3::tbl_open(g.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
    CHECK(sm3::tbl_index_usable(t, tasks_owner));
    CHECK(scanned(t, tasks_owner) == 1);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(g.a) == 0);
  }
}

TEST_CASE("after a restart every valid index is usable") {
  // Verify the count of publishes is not needed across a restart:
  // no snapshot survives one.
  // 1. An index is built and marked valid; the database restarts.
  // 2. A session at each level may use it at once.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_index(f);
  build(f, jobs_owner);
  f.reload();
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    start(f.a, level);
    t = f.write(f.a);
    CHECK(sm3::tbl_index_usable(t, jobs_owner));
    CHECK(scanned(t, jobs_owner) == 1);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

namespace {

/* An index on column att of jobs, named name, made after its rows. */
uint64_t late_on(jobs_t &f, const std::string &name, int32_t att,
                 bool unique = false) {
  std::vector<sm3::index_key_spec_t> key = {{att, false, false, std::nullopt}};
  uint64_t oid = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.jobs, name, key, {}, unique, oid) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  return oid;
}

/* The state of index oid of jobs in the committed catalog. */
sm3::index_state_t state_of(jobs_t &f, uint64_t oid) {
  return sm3::cat_get_index_state(f.cat, oid).state;
}

} // namespace

TEST_CASE("an index on a table with rows is built in the background") {
  // Verify the queue, a worker and builder_wait end to end.
  // 1. The builder is started. 250 rows are committed.
  // 2. An index on owner is created and committed: it is pending,
  //    and its build is queued at that commit.
  // 3. builder_wait returns 0: the index is valid, the caller's
  //    next transaction may use it, and it agrees with the heap.
  // 4. Waiting again on a valid index returns at once.
  jobs_t f;
  f.builder(4, 40);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 250; i++) {
    put(t, i, "free", "w" + std::to_string(i % 17));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);

  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::valid);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0);
  start(f.a, sm1::xctlevel_t::repeatable_read);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_index_usable(t, jobs_owner));
  CHECK(scanned(t, jobs_owner) == 250);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a failed build keeps its reason, for whoever waits for it") {
  // Verify what is kept of a build that failed.
  // 1. Two rows with one owner are committed; the builder is started.
  // 2. A unique index on owner is created: its build fails.
  // 3. builder_wait returns the build's own error, SM_ERR_DUPKEY,
  //    with its message, which names the table and the index, and
  //    with the index and one of the two rows' keys in errdup().
  // 4. The index is failed, and its definition holds the code, the
  //    message and the row.
  // 5. All survive a restart, and the wait gives them again.
  jobs_t f;
  f.builder();
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "same");
  uint64_t r2 = put(t, 2, "free", "same");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2, true);

  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  std::string why = sm1::errmsg();
  CHECK(why.find("jobs_owner") != std::string::npos);
  CHECK(why.find("of jobs") != std::string::npos);
  // Either row: the build meets one, finding the other's entry.
  auto both = [&](const sm1::dupkey_t &d) {
    return d.row == sm2::id_key(r1) || d.row == sm2::id_key(r2);
  };
  CHECK(sm1::errdup().index == jobs_owner);
  CHECK(both(sm1::errdup()));
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::failed);
  auto d = sm3::cat_get_table(f.cat, f.jobs);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).error ==
        sm1::SM_ERR_DUPKEY);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).reason == why);
  CHECK(both(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).dup));

  f.reload();
  d = sm3::cat_get_table(f.cat, f.jobs);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).state ==
        sm3::index_state_t::failed);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).error ==
        sm1::SM_ERR_DUPKEY);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).reason == why);
  sm1::reseterr();
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  CHECK(why == sm1::errmsg());
  CHECK(sm1::errdup().index == jobs_owner);
  CHECK(both(sm1::errdup()));
}

TEST_CASE("a build fails on any error the rows cause, and says which") {
  // Verify a failure that is not a duplicate.
  // 1. A row whose owner is 5000 bytes is committed: too long a key
  //    for a unique index, which may not cut one.
  // 2. A unique index on owner is created. Its build fails, and the
  //    wait returns SM_ERR_KEYTOOLONG with the encoder's message.
  // 3. The index is failed with that code kept; the valid indexes
  //    are untouched and agree with the heap.
  jobs_t f;
  f.builder();
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", std::string(5000, 'k'));
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2, true);

  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_KEYTOOLONG);
  std::string why = sm1::errmsg();
  CHECK_FALSE(why.empty());
  auto d = sm3::cat_get_table(f.cat, f.jobs);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).state ==
        sm3::index_state_t::failed);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).error ==
        sm1::SM_ERR_KEYTOOLONG);
  CHECK(sm3::cat_get_index_state(f.cat, d->indexes[2].oid).reason == why);
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a build left unfinished is queued when the builder starts") {
  // Verify the restart path, and the wait's other answers.
  // 1. With no builder, an index is created on a table with rows:
  //    it stays pending, and waiting for it is SM_ERR_INVALID,
  //    since no build of it is queued.
  // 2. Waiting inside a transaction is SM_ERR_INTRANS, and for an
  //    index or a table that is not there, SM_ERR_NOTFOUND.
  // 3. The database restarts and the builder is started: the index
  //    found pending is queued, and waiting for it returns 0.
  // 4. Attaching the catalog twice is SM_ERR_INVALID.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 30; i++) {
    put(t, i, "free", "w" + std::to_string(i));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  start(f.a);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INTRANS);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_owner + 999) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  CHECK(sm3::builder_wait(f.a, f.jobs + 999, jobs_owner) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);

  f.reload();
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::pending);
  f.builder(2, 7);
  CHECK(sm3::builder_attach(f.cat, [&f] { return f.try_open(); }) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(scanned(t, jobs_owner) == 30);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a pending index is building only while a worker fills it") {
  // Verify the builder's claim, and that a detached catalog queues
  // nothing.
  // 1. 20 rows are committed. The catalog is attached with an open
  //    that waits to be let go, so a worker that has taken a build
  //    stays in it.
  // 2. An index is created and committed, which queues it: it
  //    becomes building while the catalog still records it pending.
  // 3. The worker is let go: the wait returns 0 and the index is
  //    valid. Once the catalog is detached nothing is building.
  // 4. Detached, a second index stays pending, and waiting for it
  //    is SM_ERR_INVALID.
  // 5. Attached again, it is still not built, and waiting is still
  //    SM_ERR_INVALID: the builder does not look for work. Once the
  //    pending indexes are queued for it, it is built.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 20; i++) {
    put(t, i, "free", "w" + std::to_string(i));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::atomic<bool> go{false};
  REQUIRE(sm3::builder_attach(f.cat, [&] {
            while (!go.load()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return f.try_open();
          }) == 0);

  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);
  while (!sm3::builder_building(f.cat, jobs_owner)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::pending);

  go.store(true);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::valid);
  sm3::builder_detach(f.cat);
  CHECK_FALSE(sm3::builder_building(f.cat, jobs_owner));

  uint64_t jobs_state2 = late_on(f, "jobs_state2", 1);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_state2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(state_of(f, jobs_state2) == sm3::index_state_t::pending);

  REQUIRE(sm3::builder_attach(f.cat, [&f] { return f.try_open(); }) == 0);
  CHECK(sm3::builder_wait(f.a, f.jobs, jobs_state2) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm3::enqueue_pending_indexes(f.cat) == 0);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_state2) == 0,
                  sm1::errmsg());
  CHECK(state_of(f, jobs_state2) == sm3::index_state_t::valid);
}

TEST_CASE("a build that waits on a transaction holds no one else up") {
  // Verify the builder's waits can be cut short.
  // 1. A row is committed and an index made on its table, pending.
  //    A then updates the row and does not commit.
  // 2. The catalog is attached and the index queued: its build
  //    reaches the row and waits for A.
  // 3. The catalog is detached. The detach returns, though A has
  //    not ended: it interrupts the build's wait. The index is
  //    still pending, not failed.
  // 4. Attached and queued again, the build waits for A again. B
  //    waits for the build, in a thread; an interrupt to that
  //    thread ends the wait with SM_ERR_INTERRUPTED, and the bit
  //    stays pending, for the thread to take.
  // 5. A commits: the build ends, and waiting for it returns 0.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);

  start(f.a);
  t = f.write(f.a);
  REQUIRE_MESSAGE(set(t, r1, 1, "free", "b") == 0, sm1::errmsg());
  sm3::tbl_close(t);

  auto build_waits = [&] {
    f.builder(1, 10);
    while (!sm3::builder_building(f.cat, jobs_owner)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Time for the batch to reach the row A holds.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  };
  build_waits();
  sm3::builder_detach(f.cat);
  CHECK_FALSE(sm3::builder_building(f.cat, jobs_owner));
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::pending);

  build_waits();
  std::atomic<bool> returned{false};
  sm_thread_ref_t waiter;
  std::mutex waiter_mu;
  int rc = 0, err = 0;
  uint32_t taken = 0;
  std::thread tb([&] {
    {
      std::lock_guard<std::mutex> guard(waiter_mu);
      waiter = sm_thread_self();
    }
    rc = sm3::builder_wait(f.b, f.jobs, jobs_owner);
    err = sm1::errnum();
    returned = true;
    taken = sm_take(1);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(returned);
  {
    std::lock_guard<std::mutex> guard(waiter_mu);
    sm_interrupt(waiter, 1);
  }
  tb.join();
  CHECK(rc == -1);
  CHECK(err == sm1::SM_ERR_INTERRUPTED);
  // The bit is the caller's: the wait ends on it and leaves it set.
  CHECK(taken == 1);

  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE_MESSAGE(sm3::builder_wait(f.b, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::valid);
}

TEST_CASE("more builds than workers all end, a few at a time") {
  // Verify the queue holds what the workers cannot take yet.
  // 1. The builder is started with two workers. 60 rows are committed.
  // 2. Six indexes are created, one transaction each.
  // 3. Waiting for each returns 0, and all agree with the heap.
  jobs_t f;
  f.builder(2, 10);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 60; i++) {
    put(t, i, "s" + std::to_string(i % 5), "w" + std::to_string(i % 7));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  std::vector<uint64_t> made;
  for (int i = 0; i < 6; i++) {
    made.push_back(late_on(f, "extra" + std::to_string(i), 1 + i % 2));
  }
  for (uint64_t oid : made) {
    REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, oid) == 0, sm1::errmsg());
  }
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_def(t).indexes.size() == 8);
  for (uint64_t oid : made) {
    CHECK(scanned(t, oid) == 60);
  }
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("writers go on while an index is built, and it still agrees") {
  // Verify the build under real concurrency: the test's thread writes
  // while a worker backfills.
  // 1. The builder is started with small batches. 300 rows are
  //    committed, and an index on owner is created.
  // 2. While it is being built, the test updates, deletes and inserts
  //    rows, one transaction each, retrying on a retryable error.
  // 3. After the wait the index is valid and agrees with the heap.
  jobs_t f;
  f.builder(1, 5);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 300; i++) {
    put(t, i, "free", "w" + std::to_string(i % 23));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);

  for (int i = 0; i < 150; i++) {
    for (;;) {
      start(f.a);
      t = f.write(f.a);
      uint64_t r = 1 + (static_cast<uint64_t>(i) * 37) % 300;
      int rc = 0;
      if (i % 3 == 0) {
        rc = set(t, r, static_cast<int32_t>(r), "taken",
                 "x" + std::to_string(i));
      } else if (i % 3 == 1) {
        rc = sm3::tbl_delete(t, r);
      } else {
        row_t n(1000 + i, "free", "n" + std::to_string(i));
        uint64_t rowno;
        rc = sm3::tbl_insert(t, n.d(), n.null(), rowno);
      }
      bool retry = rc != 0 && sm1::retryable() == 1;
      // A row deleted by an earlier round is not there to change.
      bool gone = rc != 0 && sm1::errnum() == sm1::SM_ERR_NOTFOUND;
      sm3::tbl_close(t);
      if (rc == 0) {
        REQUIRE(sm3::commit(f.a) == 0);
        break;
      }
      sm3::abort(f.a);
      REQUIRE((retry || gone));
      if (gone) {
        break;
      }
    }
  }
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_index_usable(t, jobs_owner));
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

namespace {

/* An index on jobs with a blob, made after its rows: pending. */
uint64_t late_eval(jobs_t &f, const std::string &name, const std::string &blob,
                   std::vector<sm3::index_key_spec_t> keys,
                   std::vector<int32_t> reads, bool unique = false) {
  uint64_t oid = 0;
  start(f.a);
  REQUIRE_MESSAGE(sm3::create_index(f.a, f.jobs, name, keys, {}, unique, blob,
                                    reads, oid) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  return oid;
}

/* The key of an index on owner, and of one on lower(owner). */
const std::vector<sm3::index_key_spec_t> by_owner_key = {
    {2, false, false, std::nullopt}};
const std::vector<sm3::index_key_spec_t> by_lower_key = {
    {-1, false, false, std::nullopt, sm2::type_text.code, -1}};

} // namespace

TEST_CASE("a partial index holds the rows its predicate takes") {
  // Verify maintenance of a partial index, at every isolation level:
  // jobs_free, on owner, of the rows whose state is 'free'.
  // 1. The index is built over three rows, one of them not free.
  // 2. A free row inserted goes in; a taken one does not.
  // 3. A row that stops being free leaves it, and one that becomes
  //    free joins it.
  // 4. A free row whose owner changes moves to its new key.
  // 5. An update that changes only id, which neither the predicate
  //    nor the key reads, leaves the index alone and does not call
  //    the evaluator.
  // 6. A deleted row's entry goes. The index agrees with the heap
  //    after each step.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a);
    sm3::table_t *t = f.write(f.a);
    uint64_t r1 = put(t, 1, "free", "a");
    uint64_t r2 = put(t, 2, "taken", "b");
    uint64_t r3 = put(t, 3, "free", "c");
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
    uint64_t jobs_free = late_eval(f, "jobs_free", "free", by_owner_key, {1});
    build(f, jobs_free);

    start(f.a, level);
    t = f.write(f.a);
    CHECK(under(t, jobs_free, "a") == std::vector<uint64_t>{r1});
    CHECK(under(t, jobs_free, "b").empty());
    check_agree(t);
    uint64_t r4 = put(t, 4, "free", "d");
    uint64_t r5 = put(t, 5, "done", "e");
    CHECK(under(t, jobs_free, "d") == std::vector<uint64_t>{r4});
    CHECK(under(t, jobs_free, "e").empty());
    check_agree(t);

    REQUIRE_MESSAGE(set(t, r1, 1, "taken", "a") == 0, sm1::errmsg());
    REQUIRE(set(t, r2, 2, "free", "b") == 0);
    CHECK(under(t, jobs_free, "a").empty());
    CHECK(under(t, jobs_free, "b") == std::vector<uint64_t>{r2});
    check_agree(t);

    REQUIRE(set(t, r3, 3, "free", "z") == 0);
    CHECK(under(t, jobs_free, "c").empty());
    CHECK(under(t, jobs_free, "z") == std::vector<uint64_t>{r3});
    check_agree(t);

    int before = g_evals;
    REQUIRE(set(t, r3, 33, "free", "z") == 0);
    CHECK(g_evals == before);
    check_agree(t);

    REQUIRE(sm3::tbl_delete(t, r4) == 0);
    REQUIRE(sm3::tbl_delete(t, r5) == 0);
    CHECK(under(t, jobs_free, "d").empty());
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("an expression index is keyed by what the evaluator computes") {
  // Verify maintenance of an expression index: jobs_lower, unique,
  // on lower(owner).
  // 1. The index is built over two rows; each is found under its
  //    owner in lower case, not as written.
  // 2. A row whose owner differs from another's only in case is a
  //    duplicate: SM_ERR_DUPKEY.
  // 3. An update of the owner moves the entry; one that changes
  //    only its case leaves the key where it was.
  // 4. A NULL owner is a NULL key, and two of them are no duplicate.
  // 5. The index agrees with the heap throughout.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r1 = put(t, 1, "free", "Alice");
  uint64_t r2 = put(t, 2, "free", "BOB");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_lower =
      late_eval(f, "jobs_lower", "lower", by_lower_key, {2}, true);
  build(f, jobs_lower);

  start(f.a);
  t = f.write(f.a);
  CHECK(under(t, jobs_lower, "alice") == std::vector<uint64_t>{r1});
  CHECK(under(t, jobs_lower, "Alice").empty());
  CHECK(under(t, jobs_lower, "bob") == std::vector<uint64_t>{r2});
  check_agree(t);

  int sp = sm3::savepoint(f.a);
  row_t dup(3, "free", "bob");
  uint64_t rowno;
  CHECK(sm3::tbl_insert(t, dup.d(), dup.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_DUPKEY);
  REQUIRE(sm3::rollback(f.a, sp) == 0);
  REQUIRE(sm3::forget(f.a, sp) == 0);

  REQUIRE_MESSAGE(set(t, r2, 2, "free", "Carol") == 0, sm1::errmsg());
  CHECK(under(t, jobs_lower, "bob").empty());
  CHECK(under(t, jobs_lower, "carol") == std::vector<uint64_t>{r2});
  REQUIRE(set(t, r2, 2, "free", "CAROL") == 0);
  CHECK(under(t, jobs_lower, "carol") == std::vector<uint64_t>{r2});
  check_agree(t);

  put(t, 4, "free", std::nullopt);
  put(t, 5, "free", std::nullopt);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("read committed: an evaluated entry is the newest image's") {
  // Verify the fix-up after SM_ERR_UPDATED with a partial index on
  // owner, of free rows, and an expression index on lower(owner).
  // 1. Row 1 is free, owned by Ann. A and B both read it.
  // 2. A sets it taken, owned by BEA, and holds the row; B, set
  //    qa_nowait, is SM_ERR_BLOCKED; A commits; B is SM_ERR_UPDATED.
  // 3. B reads the newest image and writes: free again, owned by Cy.
  //    The entries removed are the newest image's: none in the
  //    partial index, where it was not, and 'bea' in the other.
  // 4. Both indexes agree with the heap.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "Ann");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_free = late_eval(f, "jobs_free", "free", by_owner_key, {1});
  uint64_t jobs_lower = late_eval(f, "jobs_lower", "lower", by_lower_key, {2});
  build(f, jobs_free);
  build(f, jobs_lower);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  start(f.b);
  ta = f.write(f.a);
  sm3::table_t *tb = f.write(f.b);
  CHECK(get(ta, r1) == "1|free|Ann");
  CHECK(get(tb, r1) == "1|free|Ann");
  REQUIRE(set(ta, r1, 1, "taken", "BEA") == 0);
  CHECK(set(tb, r1, 1, "free", "Cy") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(set(tb, r1, 1, "free", "Cy") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_UPDATED);
  CHECK(latest(tb, r1) == "1|taken|BEA");
  REQUIRE_MESSAGE(set(tb, r1, 1, "free", "Cy") == 0, sm1::errmsg());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  ta = f.write(f.a);
  CHECK(under(ta, jobs_free, "Cy") == std::vector<uint64_t>{r1});
  CHECK(under(ta, jobs_free, "Ann").empty());
  CHECK(under(ta, jobs_lower, "cy") == std::vector<uint64_t>{r1});
  CHECK(under(ta, jobs_lower, "bea").empty());
  CHECK(under(ta, jobs_lower, "ann").empty());
  check_agree(ta);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an evaluated index is built in the background, and inline") {
  // Verify the build calls the evaluator, from a worker and inline.
  // 1. 120 rows are committed, a third of them free. A partial and
  //    an expression index are created, and the builder fills them:
  //    after the wait each agrees with the heap.
  // 2. In one transaction: a table, three rows, then an expression
  //    index on it. It is valid at once and agrees with the heap.
  jobs_t f;
  f.builder(2, 25);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 120; i++) {
    put(t, i, i % 3 == 0 ? "free" : "taken", "Owner" + std::to_string(i));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_free = late_eval(f, "jobs_free", "free", by_owner_key, {1});
  uint64_t jobs_lower = late_eval(f, "jobs_lower", "lower", by_lower_key, {2});
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_free) == 0,
                  sm1::errmsg());
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_lower) == 0,
                  sm1::errmsg());
  start(f.a);
  t = f.write(f.a);
  CHECK(scanned(t, jobs_free) == 40);
  CHECK(scanned(t, jobs_lower) == 120);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::vector<sm3::column_spec_t> cols = {
      {"id", sm2::type_int4.code, -1, true},
      {"state", sm2::type_text.code, -1, false},
      {"owner", sm2::type_text.code, -1, false}};
  uint64_t tasks = 0, tasks_lower = 0;
  start(f.a);
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "tasks", cols, tasks) == 0);
  REQUIRE(sm3::tbl_open(f.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
  put(t, 1, "free", "Dee");
  put(t, 2, "free", "Eve");
  put(t, 3, "free", std::nullopt);
  sm3::tbl_close(t);
  REQUIRE_MESSAGE(sm3::create_index(f.a, tasks, "tasks_lower", by_lower_key, {},
                                    true, "lower", {{2}}, tasks_lower) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::tbl_open(f.a, tasks, sm1::SM_LOCKMODE_IX, t) == 0);
  CHECK(sm3::tbl_index_state(t, sm3::tbl_def(t).indexes[0].oid) ==
        sm3::index_state_t::valid);
  CHECK(under(t, tasks_lower, "dee") == std::vector<uint64_t>{1});
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an evaluator that fails fails the write, and the build") {
  // Verify the evaluator's errors are the call's.
  // 1. A row is committed; an index whose blob makes the evaluator
  //    fail is created, and the builder is running.
  // 2. Its build fails: the wait returns the evaluator's own error
  //    and message, and the index is failed, so writes leave it
  //    alone from then on.
  // 3. With such an index still pending, a write fails with the
  //    evaluator's error.
  // 4. A catalog loaded with no evaluator refuses a write to a table
  //    that has an evaluated index, with SM_ERR_INVALID.
  jobs_t f;
  f.builder();
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 1, "free", "a");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t bad = late_eval(f, "jobs_bad", "fail", by_owner_key, {1});
  CHECK(sm3::builder_wait(f.a, f.jobs, bad) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(std::string(sm1::errmsg()) == "the evaluator failed");
  CHECK(state_of(f, bad) == sm3::index_state_t::failed);
  start(f.a);
  t = f.write(f.a);
  put(t, 2, "free", "b");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  jobs_t g;
  uint64_t bad2 = late_eval(g, "jobs_bad", "fail", by_owner_key, {1});
  (void)bad2;
  row_t r(1, "free", "a");
  uint64_t rowno;
  start(g.a);
  t = g.write(g.a);
  CHECK(sm3::tbl_insert(t, r.d(), r.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(std::string(sm1::errmsg()) == "the evaluator failed");
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(g.a) == 0);

  jobs_t h;
  late_eval(h, "jobs_free", "free", by_owner_key, {1});
  h.reload(false);
  start(h.a);
  t = h.write(h.a);
  CHECK(sm3::tbl_insert(t, r.d(), r.null(), rowno) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(std::string(sm1::errmsg()).find("no evaluator") != std::string::npos);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(h.a) == 0);
}

TEST_CASE("an evaluated build and a writer settle on the row's lock") {
  // Verify both orders of the race for a partial index, jobs_free,
  // on owner, of free rows, one thread playing both sessions.
  // 1. The writer first. A makes row 1 taken and holds it; the
  //    build, set qa_nowait, is SM_ERR_BLOCKED and aborts its batch;
  //    A commits. The build runs again and shows the evaluator the
  //    row's newest image, which is not free: no entry for row 1.
  // 2. The build first. It covers row 3, which is free, and holds
  //    its lock; A, set qa_nowait, makes row 3 taken:
  //    SM_ERR_BLOCKED. The build commits, and A's update goes
  //    through, removing the entry the build added after A began.
  // 3. Marked valid, the index holds row 2 alone, and agrees with
  //    the heap.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "a");
  uint64_t r2 = put(ta, 2, "free", "b");
  uint64_t r3 = put(ta, 3, "free", "c");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_free = late_eval(f, "jobs_free", "free", by_owner_key, {1});
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);
  std::string next, unused;
  bool done = false;

  start(f.a);
  ta = f.write(f.a);
  REQUIRE(set(ta, r1, 1, "taken", "a") == 0);
  start(f.b);
  sm3::table_t *tb = f.write(f.b);
  CHECK(sm3::tbl_index_backfill(tb, jobs_free, "", 2, next, done) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::abort(f.b) == 0);
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  tb = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_index_backfill(tb, jobs_free, "", 2, next, done) ==
                      0,
                  sm1::errmsg());
  CHECK(next == sm2::id_key(r2) + '\0');
  CHECK(under(tb, jobs_free, "a").empty());
  CHECK(under(tb, jobs_free, "b") == std::vector<uint64_t>{r2});
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.a), true) == 0);
  start(f.a);
  ta = f.write(f.a);
  CHECK(get(ta, r3) == "3|free|c");
  start(f.b);
  tb = f.write(f.b);
  REQUIRE(sm3::tbl_index_backfill(tb, jobs_free, next, 10, unused, done) == 0);
  CHECK(done);
  CHECK(under(tb, jobs_free, "c") == std::vector<uint64_t>{r3});
  CHECK(set(ta, r3, 3, "taken", "c") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE_MESSAGE(set(ta, r3, 3, "taken", "c") == 0, sm1::errmsg());
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  validate(f, f.b, jobs_free);

  start(f.b);
  tb = f.write(f.b);
  CHECK(under(tb, jobs_free, "a").empty());
  CHECK(under(tb, jobs_free, "b") == std::vector<uint64_t>{r2});
  CHECK(under(tb, jobs_free, "c").empty());
  check_agree(tb);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("writers go on while evaluated indexes are built, and they agree") {
  // Verify the evaluated build under real concurrency: the test's
  // thread writes while workers backfill a partial and an expression
  // index, calling the evaluator from both sides.
  // 1. The builder is started with small batches. 300 rows are
  //    committed, half of them free, and both indexes are created.
  // 2. While they build, the test flips rows between free and
  //    taken with a new owner, deletes rows and inserts rows, one
  //    transaction each, retrying on a retryable error.
  // 3. After the waits both are valid and agree with the heap.
  jobs_t f;
  f.builder(2, 5);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 300; i++) {
    put(t, i, i % 2 ? "free" : "taken", "Own" + std::to_string(i));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t jobs_free = late_eval(f, "jobs_free", "free", by_owner_key, {1});
  uint64_t jobs_lower = late_eval(f, "jobs_lower", "lower", by_lower_key, {2});

  for (int i = 0; i < 150; i++) {
    for (;;) {
      start(f.a);
      t = f.write(f.a);
      uint64_t r = 1 + (static_cast<uint64_t>(i) * 37) % 300;
      int rc = 0;
      if (i % 3 == 0) {
        rc = set(t, r, static_cast<int32_t>(r), i % 2 ? "free" : "taken",
                 "New" + std::to_string(i));
      } else if (i % 3 == 1) {
        rc = sm3::tbl_delete(t, r);
      } else {
        row_t n(1000 + i, i % 2 ? "free" : "taken", "Ins" + std::to_string(i));
        uint64_t rowno;
        rc = sm3::tbl_insert(t, n.d(), n.null(), rowno);
      }
      bool retry = rc != 0 && sm1::retryable() == 1;
      // A row deleted by an earlier round is not there to change.
      bool gone = rc != 0 && sm1::errnum() == sm1::SM_ERR_NOTFOUND;
      sm3::tbl_close(t);
      if (rc == 0) {
        REQUIRE(sm3::commit(f.a) == 0);
        break;
      }
      sm3::abort(f.a);
      REQUIRE((retry || gone));
      if (gone) {
        break;
      }
    }
  }
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_free) == 0,
                  sm1::errmsg());
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_lower) == 0,
                  sm1::errmsg());
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_index_usable(t, jobs_free));
  CHECK(sm3::tbl_index_usable(t, jobs_lower));
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE(
    "a NULL in a NOT NULL column is refused, before anything is written") {
  // Verify the one constraint sm3 enforces (../INDEXBUILD.md §2):
  // jobs.id is NOT NULL, jobs.owner is not.
  // 1. Inserting a row with a NULL id fails with SM_ERR_NOTNULL, in
  //    a message that names the column. Nothing was written, so the
  //    transaction goes on with no rollback: a good row inserts.
  // 2. Updating a row's id to NULL fails the same way, and the row
  //    and its index entries are as they were.
  // 3. A NULL in a column that allows it is written.
  // 4. The same at every isolation level.
  for (sm1::xctlevel_t level : all_levels) {
    CAPTURE(static_cast<int>(level));
    jobs_t f;
    start(f.a, level);
    sm3::table_t *t = f.write(f.a);
    std::string state = sm2::make_varlena("free");
    sm2::datum_t d[3] = {0, sm2::datum_from_ptr(state.data()), 0};
    bool n[3] = {true, false, true};
    uint64_t rowno = 0;
    CHECK(sm3::tbl_insert(t, d, n, rowno) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
    CHECK(std::string(sm1::errname()) == "ERR_NOTNULL");
    CHECK(sm1::retryable() == 0);
    CHECK(std::string(sm1::errmsg()).find("id") != std::string::npos);
    CHECK(rows(t).empty());

    uint64_t r1 = put(t, 1, "free", std::nullopt);
    CHECK(get(t, r1) == "1|free|-");
    CHECK(sm3::tbl_update(t, r1, d, n) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_NOTNULL);
    CHECK(get(t, r1) == "1|free|-");
    check_agree(t);
    sm3::tbl_close(t);
    REQUIRE(sm3::commit(f.a) == 0);
  }
}

TEST_CASE("a row is locked without being written") {
  // Verify tbl_lock, the stack's share of a foreign key check and of
  // SELECT ... FOR SHARE and FOR UPDATE (DESIGN.md §5).
  // 1. A locks row 1 shared. B, set qa_nowait, locks it shared too,
  //    as a second child of one parent does; B's update of the row
  //    and its exclusive lock on it are both SM_ERR_BLOCKED.
  // 2. A commits, and with it the lock ends: B's update goes through,
  //    with no SM_ERR_UPDATED, for A wrote nothing.
  // 3. A locks row 1 exclusive: B's shared lock is SM_ERR_BLOCKED.
  // 4. A row that is not there can be locked.
  // 5. An exclusive lock needs the table opened to write; a shared
  //    one does not.
  jobs_t f;
  start(f.a);
  sm3::table_t *ta = f.write(f.a);
  uint64_t r1 = put(ta, 1, "free", "a");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.b), true) == 0);

  start(f.a);
  start(f.b);
  ta = f.write(f.a, sm1::SM_LOCKMODE_IS);
  sm3::table_t *tb = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_lock(ta, r1, sm1::SM_LOCKMODE_SH) == 0,
                  sm1::errmsg());
  CHECK(sm3::tbl_lock(tb, r1, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(set(tb, r1, 1, "taken", "b") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(sm3::tbl_lock(tb, r1, sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(get(ta, r1) == "1|free|a");
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE_MESSAGE(set(tb, r1, 1, "taken", "b") == 0, sm1::errmsg());
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  start(f.b);
  ta = f.write(f.a);
  tb = f.write(f.b, sm1::SM_LOCKMODE_IS);
  REQUIRE(sm3::tbl_lock(ta, r1, sm1::SM_LOCKMODE_EX) == 0);
  CHECK(sm3::tbl_lock(tb, r1, sm1::SM_LOCKMODE_SH) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);
  CHECK(sm3::tbl_lock(ta, r1 + 999, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm3::tbl_lock(tb, r1 + 500, sm1::SM_LOCKMODE_SH) == 0);
  CHECK(sm3::tbl_lock(tb, r1 + 500, sm1::SM_LOCKMODE_EX) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  sm3::tbl_close(ta);
  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.a) == 0);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a truncate empties the table and its indexes, with its "
          "transaction") {
  // Verify a truncate is transactional and leaves a working table.
  // 1. Commit three rows.
  // 2. Truncate in a new transaction: no rows, the indexes agree,
  //    and the table has a new store.
  // 3. Insert a row: its rowno follows the old ones, and the
  //    indexes hold it alone.
  // 4. Abort: the three rows are back, in the old store.
  // 5. Truncate again and commit: a new transaction finds the
  //    table empty, and the old store is gone.
  // 6. After a restart the table is still empty and writable.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 7, "free", std::nullopt);
  put(t, 8, "free", "w8");
  uint64_t r9 = put(t, 9, "taken", "w9");
  const uint64_t old_store = sm3::tbl_def(t).storeid;
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_EX);
  REQUIRE_MESSAGE(sm3::tbl_truncate(t) == 0, sm1::errmsg());
  CHECK(rows(t).empty());
  CHECK(sm3::tbl_def(t).storeid != old_store);
  check_agree(t);

  uint64_t r10 = put(t, 10, "free", std::nullopt);
  CHECK(r10 > r9);
  CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r10});
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(rows(t).size() == 3);
  CHECK(sm3::tbl_def(t).storeid == old_store);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_EX);
  REQUIRE(sm3::tbl_truncate(t) == 0);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(rows(t).empty());
  CHECK(sm3::tbl_def(t).storeid != old_store);
  std::vector<uint64_t> stores;
  REQUIRE(sm1::lsstore(sm3::session_sm1(f.b), stores) == 0);
  CHECK(std::find(stores.begin(), stores.end(), old_store) == stores.end());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);

  f.reload();
  start(f.a);
  t = f.write(f.a);
  CHECK(rows(t).empty());
  uint64_t r11 = put(t, 11, "free", std::nullopt);
  CHECK(r11 > r10);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a truncate is undone by a rollback to a savepoint before it") {
  // Verify the truncate is undone like any DDL inside a savepoint.
  // 1. Commit two rows.
  // 2. Set a savepoint, truncate, and roll back to it.
  // 3. The handle is reopened and finds both rows, in the old
  //    store, and the indexes agree.
  // 4. Commit; a new transaction finds the same.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 7, "free", std::nullopt);
  put(t, 8, "taken", "w8");
  const uint64_t old_store = sm3::tbl_def(t).storeid;
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_EX);
  int depth = sm3::savepoint(f.a);
  REQUIRE(depth > 0);
  REQUIRE(sm3::tbl_truncate(t) == 0);
  CHECK(rows(t).empty());
  sm3::tbl_close(t);
  REQUIRE(sm3::rollback(f.a, depth) == 0);

  t = f.write(f.a, sm1::SM_LOCKMODE_EX);
  CHECK(rows(t).size() == 2);
  CHECK(sm3::tbl_def(t).storeid == old_store);
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(rows(t).size() == 2);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a truncate needs the table opened exclusive, which waits for "
          "its readers") {
  // Verify the lock a truncate needs, and what it waits for.
  // 1. Opened to write, not exclusive: the truncate is refused
  //    with SM_ERR_INVALID and nothing changes.
  // 2. b reads the table; a, not waiting, cannot open it
  //    exclusive: SM_ERR_BLOCKED.
  // 3. Once b commits, a opens it exclusive and truncates.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  put(t, 7, "free", std::nullopt);
  CHECK(sm3::tbl_truncate(t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(rows(t).size() == 1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  sm3::table_t *tb = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(rows(tb).size() == 1);

  REQUIRE(sm1::set_qa_nowait(sm3::session_sm1(f.a), true) == 0);
  start(f.a);
  sm3::table_t *ta = nullptr;
  CHECK(sm3::tbl_open(f.a, f.jobs, sm1::SM_LOCKMODE_EX, ta) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_BLOCKED);

  sm3::tbl_close(tb);
  REQUIRE(sm3::commit(f.b) == 0);
  REQUIRE_MESSAGE(sm3::tbl_open(f.a, f.jobs, sm1::SM_LOCKMODE_EX, ta) == 0,
                  sm1::errmsg());
  REQUIRE(sm3::tbl_truncate(ta) == 0);
  CHECK(rows(ta).empty());
  sm3::tbl_close(ta);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a statement does not see its own table writes, nor change a row "
          "twice") {
  // Verify the statement mark through the table writes and the indexes.
  // 1. Commit row 7, free.
  // 2. In one statement claim it: the statement's reads, by the
  //    heap and by the index, still find it free.
  // 3. Updating or deleting it again fails with
  //    SM_ERR_SELFMODIFIED and changes nothing.
  // 4. The next statement finds it taken, and the indexes agree.
  // 5. Commit: it is taken for a new transaction.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r7 = put(t, 7, "free", std::nullopt);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  REQUIRE(sm3::stmt_begin(f.a) == 0);
  t = f.write(f.a);
  REQUIRE(set(t, r7, 7, "taken", "w1") == 0);
  CHECK(get(t, r7) == "7|free|-");
  CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r7});
  CHECK(under(t, f.jobs_state, "taken").empty());

  CHECK(set(t, r7, 7, "done", "w2") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);
  CHECK(sm3::tbl_delete(t, r7) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_SELFMODIFIED);

  REQUIRE(sm3::stmt_begin(f.a) == 0);
  CHECK(get(t, r7) == "7|taken|w1");
  CHECK(under(t, f.jobs_state, "taken") == std::vector<uint64_t>{r7});
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(get(t, r7) == "7|taken|w1");
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a statement inside another sees the outer's table writes, and the "
          "outer resumes") {
  // Verify sm3::stmt_end, as a trigger's commands inside a statement.
  // 1. A statement inserts row 7.
  // 2. An inner statement sees it, by heap and index, and
  //    inserts row 8.
  // 3. The inner ends: the outer sees neither row.
  // 4. The next statement sees both, and the indexes agree.
  jobs_t f;
  start(f.a);
  REQUIRE(sm3::stmt_begin(f.a) == 0);
  sm3::table_t *t = f.write(f.a);
  uint64_t r7 = put(t, 7, "free", std::nullopt);

  sm3::stmt_t outer;
  REQUIRE(sm3::stmt_begin(f.a, outer) == 0);
  CHECK(get(t, r7) == "7|free|-");
  CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r7});
  uint64_t r8 = put(t, 8, "free", std::nullopt);

  REQUIRE(sm3::stmt_end(f.a, outer) == 0);
  CHECK(rows(t).empty());
  CHECK(under(t, f.jobs_state, "free").empty());

  REQUIRE(sm3::stmt_begin(f.a) == 0);
  CHECK(under(t, f.jobs_state, "free") == std::vector<uint64_t>{r7, r8});
  check_agree(t);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("a table's sizes, rowno limit and probes are read live") {
  // Verify the live statistics follow the rows and the store.
  // 1. A new table: N is 0, the heap and an index are 0 bytes,
  //    and there is nothing to probe.
  // 2. Commit 20,000 rows: N is the last rowno, nothing is on
  //    disk yet, the memtable is a probe, and the heap's and an
  //    index's sizes are their relids' in the table's store.
  //    The memtable amounts are counted as written (sm1/API.md),
  //    and checked there.
  // 3. An index the table does not have fails with SM_ERR_NOTFOUND.
  // 4. Truncate: the handle sizes the new store, 0 bytes and no
  //    probe, and N carries over.
  // 5. Abort: a new handle sizes the old store again.
  jobs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t n = 1, disk = 1, mem = 1, probes = 1;
  REQUIRE(sm3::tbl_id_limit(t, n) == 0);
  CHECK(n == 0);
  REQUIRE(sm3::tbl_size(t, disk, mem) == 0);
  CHECK(disk == 0);
  CHECK(mem == 0);
  REQUIRE(sm3::tbl_idx_size(t, f.jobs_id, disk, mem) == 0);
  CHECK(mem == 0);
  REQUIRE(sm3::tbl_probes(t, probes) == 0);
  CHECK(probes == 0);

  // 2. Load and commit.
  uint64_t last = 0;
  for (int i = 0; i < 20000; i++) {
    last = put(t, i, "free", std::nullopt);
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  t = f.write(f.a);
  sm1::session_t *s1 = sm3::session_sm1(f.a);
  const uint64_t old_store = sm3::tbl_def(t).storeid;
  REQUIRE(sm3::tbl_id_limit(t, n) == 0);
  CHECK(n == last);
  uint64_t d2 = 1, m2 = 1;
  REQUIRE(sm3::tbl_size(t, disk, mem) == 0);
  REQUIRE(sm2::relid_size(s1, old_store, sm3::heap_relid, d2, m2) == 0);
  CHECK(disk == 0);
  CHECK(disk == d2);
  CHECK(mem == m2);
  uint16_t id_relid = 0;
  for (const sm3::index_def_t &x : sm3::tbl_def(t).indexes) {
    if (x.oid == f.jobs_id) {
      id_relid = x.relid;
    }
  }
  REQUIRE(sm3::tbl_idx_size(t, f.jobs_id, disk, mem) == 0);
  REQUIRE(sm2::relid_size(s1, old_store, id_relid, d2, m2) == 0);
  CHECK(disk == 0);
  CHECK(mem == m2);
  REQUIRE(sm3::tbl_probes(t, probes) == 0);
  CHECK(probes == 1);

  // 3. No such index.
  CHECK(sm3::tbl_idx_size(t, f.jobs + 1000, disk, mem) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  // 4. Truncate, uncommitted.
  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_EX);
  REQUIRE(sm3::tbl_truncate(t) == 0);
  REQUIRE(sm3::tbl_size(t, disk, mem) == 0);
  CHECK(disk == 0);
  CHECK(mem == 0);
  REQUIRE(sm3::tbl_probes(t, probes) == 0);
  CHECK(probes == 0);
  REQUIRE(sm3::tbl_id_limit(t, n) == 0);
  CHECK(n == last);
  sm3::tbl_close(t);
  REQUIRE(sm3::abort(f.a) == 0);

  // 5. The old store, after the abort.
  start(f.b);
  t = f.write(f.b, sm1::SM_LOCKMODE_IS);
  CHECK(sm3::tbl_def(t).storeid == old_store);
  REQUIRE(sm3::tbl_size(t, disk, mem) == 0);
  REQUIRE(sm2::relid_size(sm3::session_sm1(f.b), old_store, sm3::heap_relid, d2,
                          m2) == 0);
  CHECK(mem == m2);
  REQUIRE(sm3::tbl_probes(t, probes) == 0);
  CHECK(probes == 1);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.b) == 0);
}

TEST_CASE("a table's version moves with its definition, statistics and "
          "indexes") {
  // Verify cat_table_version counts what a host's cache of the table
  // depends on, and nothing else, and the namespaces' epoch likewise.
  // 1. B reads jobs' version, v0, and the epoch.
  // 2. A renames a column and has not committed: A reads its own, B
  //    v0. A commits: B reads v1, above v0.
  // 3. A creates another table, and inserts 200 rows into jobs: jobs'
  //    version stays v1.
  // 4. A analyzes jobs: its own until it commits, then v2, above v1.
  // 5. With the builder started, an index on owner is created: v3,
  //    above v2; once it is built and valid, v4, above v3.
  // 6. The epoch has not moved; a namespace created and committed
  //    moves it.
  jobs_t f;
  uint64_t epoch0 = sm3::cat_namespace_epoch(f.b);
  uint64_t v0 = sm3::cat_table_version(f.b, f.jobs);
  CHECK(v0 != sm3::table_version_own);

  start(f.a);
  REQUIRE(sm3::rename_column(f.a, f.jobs, "state", "status") == 0);
  CHECK(sm3::cat_table_version(f.a, f.jobs) == sm3::table_version_own);
  CHECK(sm3::cat_table_version(f.b, f.jobs) == v0);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t v1 = sm3::cat_table_version(f.b, f.jobs);
  CHECK(v1 > v0);

  start(f.a);
  std::vector<sm3::column_spec_t> cols = {
      {"x", sm2::type_int4.code, -1, false}};
  uint64_t other = 0;
  REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "other", cols, other) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 200; i++) {
    put(t, i, "free", "w" + std::to_string(i % 13));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(sm3::cat_table_version(f.b, f.jobs) == v1);

  start(f.a);
  t = f.write(f.a, sm1::SM_LOCKMODE_IS);
  REQUIRE_MESSAGE(sm3::tbl_analyze(t, 10) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  CHECK(sm3::cat_table_version(f.a, f.jobs) == sm3::table_version_own);
  CHECK(sm3::cat_table_version(f.b, f.jobs) == v1);
  REQUIRE(sm3::commit(f.a) == 0);
  uint64_t v2 = sm3::cat_table_version(f.b, f.jobs);
  CHECK(v2 > v1);

  f.builder(4, 40);
  uint64_t jobs_owner = late_on(f, "jobs_owner", 2);
  uint64_t v3 = sm3::cat_table_version(f.b, f.jobs);
  CHECK(v3 > v2);
  REQUIRE_MESSAGE(sm3::builder_wait(f.a, f.jobs, jobs_owner) == 0,
                  sm1::errmsg());
  CHECK(state_of(f, jobs_owner) == sm3::index_state_t::valid);
  CHECK(sm3::cat_table_version(f.b, f.jobs) > v3);

  CHECK(sm3::cat_namespace_epoch(f.b) == epoch0);
  start(f.a);
  uint64_t nsp = 0;
  REQUIRE(sm3::create_namespace(f.a, "s1", nsp) == 0);
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(sm3::cat_namespace_epoch(f.b) > epoch0);
}
