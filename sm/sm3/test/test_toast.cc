/*
 * test_toast.cc -- TOAST through a table (SM3_DESIGN.md §3,
 * sm2/SM2_DESIGN.md §4.2): a row's long datums out of line once
 * the host reads pointers, and every index still made of values.
 */

#include "db.h"
#include "doctest.h"
#include "sm3.h"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace {

/* Whether the tests' evaluator was ever shown a TOAST pointer. */
std::atomic<bool> g_saw_pointer{false};

/* How many times the tests' evaluator has been called. */
std::atomic<int> g_evals{0};

/* How many bytes of body an index of its prefix keys. */
constexpr size_t prefix_len = 8;

/*
 * The evaluator of docs' expression index, on the first
 * prefix_len bytes of body. It notes a pointer among the texts it
 * is shown, which it must never be.
 */
int docs_eval(const sm3::table_def_t &, const sm3::index_def_t &,
              std::span<const sm2::datum_t> d, std::span<const bool> n,
              bool &belongs, std::vector<std::string> &values,
              std::span<bool> isnull) {
  g_evals++;
  belongs = true;
  isnull[0] = n[2];
  if (n[2]) {
    return 0;
  }
  std::string_view body;
  sm2::datum_bytes(&sm2::type_text, -1, d[2], body);
  if (sm2::varlena_is_toast(body)) {
    g_saw_pointer = true;
    return sm1::seterr(sm1::SM_ERR_INVALID, "the evaluator saw a pointer");
  }
  values[0] =
      sm2::make_varlena(sm2::varlena_payload(body).substr(0, prefix_len));
  return 0;
}

/* A text of n bytes beginning with tag, which no other tag gives. */
std::string text_of(size_t n, const std::string &tag) {
  std::string s = tag;
  s.resize(n, 'x');
  return s;
}

/* A row of docs as the datums a write takes; a text given raw is as it is. */
struct row_t {
  /* The row (id, title, body); raw texts are varlenas already. */
  row_t(int32_t id, const std::string &title, const std::string &body,
        bool raw_title = false, bool raw_body = false)
      : m_title(raw_title ? title : sm2::make_varlena(title)),
        m_body(raw_body ? body : sm2::make_varlena(body)) {
    m_d[0] = sm2::datum_from_int32(id);
    m_d[1] = sm2::datum_from_ptr(m_title.data());
    m_d[2] = sm2::datum_from_ptr(m_body.data());
  }
  row_t(const row_t &) = delete;
  row_t &operator=(const row_t &) = delete;

  std::span<const sm2::datum_t> d() const { return m_d; }
  std::span<const bool> null() const { return m_null; }

private:
  std::string m_title, m_body;
  sm2::datum_t m_d[3];
  bool m_null[3] = {false, false, false};
};

/*
 * A catalog with docs (id int4, title text, body text) and its
 * indexes: docs_title on (title) INCLUDE (body), and docs_pre on
 * the first bytes of body, which the evaluator computes. With
 * toast, the catalog's host reads TOAST pointers.
 */
class docs_t : public db_t {
public:
  /* Initialize and load the catalog, create docs, open a and b. */
  explicit docs_t(bool toast = true) {
    g_saw_pointer = false;
    g_evals = 0;
    REQUIRE(sm3::cat_init(env(), "db") == 0);
    REQUIRE(sm3::cat_open(env(), "db", cat) == 0);
    REQUIRE(sm3::cat_set_toast(cat, toast) == 0);
    REQUIRE(sm3::cat_set_evaluator(cat, docs_eval) == 0);
    REQUIRE(sm3::session_open(cat, open(), a) == 0);
    REQUIRE(sm3::session_open(cat, open(), b) == 0);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"title", sm2::type_text.code, -1, false},
        {"body", sm2::type_text.code, -1, false}};
    std::vector<sm3::index_key_spec_t> by_title = {
        {1, false, false, std::nullopt}};
    std::vector<sm3::index_key_spec_t> by_expr = {
        {-1, false, false, std::nullopt, sm2::type_text.code, -1}};
    std::vector<int32_t> body = {2};
    REQUIRE(sm3::begin(a, sm1::xctlevel_t::read_committed) == 0);
    REQUIRE(sm3::create_table(a, sm3::nsp_public, "docs", cols, docs) == 0);
    REQUIRE(sm3::create_index(a, docs, "docs_title", by_title, body, false,
                              docs_title) == 0);
    REQUIRE_MESSAGE(sm3::create_index(a, docs, "docs_pre", by_expr, {}, false,
                                      "prefix", body, docs_pre) == 0,
                    sm1::errmsg());
    REQUIRE(sm3::commit(a) == 0);
  }

  /* Close the sessions and the catalog. */
  ~docs_t() {
    sm3::session_close(a);
    sm3::session_close(b);
    sm3::builder_detach(cat);
    sm3::cat_close(cat);
  }

  /* docs, opened to write in s's transaction; it must open. */
  sm3::table_t *write(sm3::session_t *s) {
    sm3::table_t *t = nullptr;
    REQUIRE_MESSAGE(sm3::tbl_open(s, docs, sm1::SM_LOCKMODE_IX, t) == 0,
                    sm1::errmsg());
    return t;
  }

  sm3::catalog_t *cat = nullptr;
  sm3::session_t *a = nullptr, *b = nullptr;
  uint64_t docs = 0, docs_title = 0, docs_pre = 0;
};

/* Begin a READ COMMITTED transaction on s. */
void start(sm3::session_t *s) {
  REQUIRE(sm3::begin(s, sm1::xctlevel_t::read_committed) == 0);
}

/* Insert the row through t; its rowno. */
uint64_t put(sm3::table_t *t, const row_t &r) {
  uint64_t rowno = 0;
  REQUIRE_MESSAGE(sm3::tbl_insert(t, r.d(), r.null(), rowno) == 0,
                  sm1::errmsg());
  return rowno;
}

/* Text attribute ordinal of the row at rowno, as its tuple holds it. */
std::string held(sm3::table_t *t, uint64_t rowno, size_t ordinal) {
  std::string_view tuple;
  REQUIRE_MESSAGE(sm3::tbl_read(t, rowno, tuple) == 0, sm1::errmsg());
  sm2::datum_t val;
  bool isnull;
  REQUIRE(sm2::tup_decode(sm3::tbl_def(t).desc, tuple, ordinal, val, isnull) ==
          0);
  REQUIRE_FALSE(isnull);
  std::string_view v;
  REQUIRE(sm2::datum_bytes(&sm2::type_text, -1, val, v) == 0);
  return std::string(v);
}

/* Text attribute ordinal of the row at rowno, fetched if out of line. */
std::string value(sm3::table_t *t, uint64_t rowno, size_t ordinal) {
  std::string v = held(t, rowno, ordinal);
  if (!sm2::varlena_is_toast(v)) {
    return std::string(sm2::varlena_payload(v));
  }
  std::string out;
  REQUIRE_MESSAGE(sm3::tbl_fetch(t, v, out) == 0, sm1::errmsg());
  return out;
}

/*
 * The rownos index oid of t has under the text key, in order, and
 * with them in values each entry's value bytes, if asked for.
 */
std::vector<uint64_t> find(sm3::table_t *t, uint64_t oid,
                           const std::string &key,
                           std::vector<std::string> *values = nullptr) {
  std::string v = sm2::make_varlena(key);
  sm2::datum_t d[1] = {sm2::datum_from_ptr(v.data())};
  bool null[1] = {false};
  sm2::bound_t eq{d, null, true};
  sm2::index_scan_t *scan = nullptr;
  REQUIRE_MESSAGE(sm3::tbl_idx_scan(t, oid, &eq, &eq, scan) == 0,
                  sm1::errmsg());
  std::vector<uint64_t> out;
  uint64_t rowno;
  bool cut;
  std::string_view value;
  int rc;
  while ((rc = sm2::idx_scannxt(scan, rowno, cut, value)) == 0) {
    out.push_back(rowno);
    if (values) {
      values->push_back(std::string(value));
    }
  }
  sm2::idx_scanend(scan);
  REQUIRE(rc == 1);
  return out;
}

/* How many TOAST chunks t's transaction sees in its table's store. */
size_t chunks(sm3::table_t *t) {
  std::string lo = "\xff\xff", hi(14, '\xff');
  sm1::scan_t *scan = nullptr;
  REQUIRE(sm1::scan(sm3::session_sm1(sm3::tbl_session(t)),
                    sm3::tbl_def(t).storeid, lo.data(), lo.size(), 1, hi.data(),
                    hi.size(), 1, scan) == 0);
  size_t n = 0;
  std::string_view key, val;
  while (sm1::scannxt(scan, key, val) == 0) {
    n++;
  }
  sm1::scanend(scan);
  return n;
}

} // namespace

TEST_CASE("a table's long datum is out of line only if the host reads "
          "pointers") {
  // Verify cat_set_toast decides whether a table's rows hold
  // pointers.
  // 1. A catalog whose host does not: a 5000-byte body stays in
  //    its tuple, and no chunk is written. In a table with no
  //    index, a 100000-byte datum is refused with
  //    SM_ERR_ROWTOOLONG.
  // 2. A catalog whose host does: the 5000-byte body is a pointer,
  //    and tbl_fetch reads it. The 100000-byte datum is stored and
  //    read back.
  // 3. Either way, docs refuses a 100000-byte body: its index
  //    holds the body whole, as an INCLUDE column, and an entry is
  //    held to 64 KB.
  for (bool toast : {false, true}) {
    CAPTURE(toast);
    docs_t f(toast);
    start(f.a);
    std::vector<sm3::column_spec_t> cols = {
        {"id", sm2::type_int4.code, -1, true},
        {"note", sm2::type_text.code, -1, false}};
    uint64_t notes = 0;
    REQUIRE(sm3::create_table(f.a, sm3::nsp_public, "notes", cols, notes) == 0);
    sm3::table_t *t = f.write(f.a);
    std::string body = text_of(5000, "body-one"), huge = text_of(100000, "h");
    uint64_t r = put(t, row_t(1, "t1", body));
    CHECK(sm2::varlena_is_toast(held(t, r, 2)) == toast);
    CHECK(value(t, r, 2) == body);
    CHECK(chunks(t) == (toast ? 1 : 0));
    row_t big(2, "t2", huge);
    uint64_t r2 = 0;
    CHECK(sm3::tbl_insert(t, big.d(), big.null(), r2) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_ROWTOOLONG);
    sm3::tbl_close(t);

    REQUIRE(sm3::tbl_open(f.a, notes, sm1::SM_LOCKMODE_IX, t) == 0);
    std::string note = sm2::make_varlena(huge);
    sm2::datum_t d[2] = {sm2::datum_from_int32(1),
                         sm2::datum_from_ptr(note.data())};
    bool null[2] = {false, false};
    if (toast) {
      REQUIRE_MESSAGE(sm3::tbl_insert(t, d, null, r2) == 0, sm1::errmsg());
      CHECK(value(t, r2, 1) == huge);
    } else {
      CHECK(sm3::tbl_insert(t, d, null, r2) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_ROWTOOLONG);
    }
    sm3::tbl_close(t);
    REQUIRE(sm3::abort(f.a) == 0);
  }
}

TEST_CASE("an index of a column out of line is made of its value") {
  // Verify each write keeps the indexes in step when the columns
  // they need are out of line, and the evaluator is shown values.
  // 1. Insert a row whose title, 3000 bytes, and body, 5000, are
  //    both out of line. docs_title finds it by its title, with the
  //    body's bytes in its entry, not a pointer; docs_pre by the
  //    body's first bytes.
  // 2. Update the id alone, giving both pointers back: no chunk is
  //    written or erased, and the evaluator is not asked.
  // 3. Update the body, giving the title's pointer back: docs_title
  //    still finds the row, now with the new body; docs_pre finds
  //    it under the new prefix and not the old.
  // 4. Update the title to another long one, giving the body's
  //    pointer back: the old title finds nothing, the new one the
  //    row, its entry holding the body's bytes still, and the old
  //    title's chunk is gone.
  // 5. Update the title to the empty text: the long title finds
  //    nothing, the empty one the row. A pointer is not its value,
  //    which here is not empty.
  // 6. Delete the row: no index finds it, and no chunk is left.
  // 7. The evaluator never saw a pointer.
  docs_t f;
  std::string t1 = text_of(3000, "title-one"), t2 = text_of(3000, "title-two");
  std::string b1 = text_of(5000, "body-one"), b2 = text_of(5000, "BODY-TWO");
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  uint64_t r = put(t, row_t(1, t1, b1));
  std::string title_ptr = held(t, r, 1), body_ptr = held(t, r, 2);
  REQUIRE(sm2::varlena_is_toast(title_ptr));
  REQUIRE(sm2::varlena_is_toast(body_ptr));
  std::vector<std::string> entries;
  CHECK(find(t, f.docs_title, t1, &entries) == std::vector<uint64_t>{r});
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].find(b1) != std::string::npos);
  CHECK(find(t, f.docs_pre, b1.substr(0, prefix_len)) ==
        std::vector<uint64_t>{r});
  CHECK(chunks(t) == 2);

  int evals = g_evals;
  row_t renumber(2, title_ptr, body_ptr, true, true);
  REQUIRE_MESSAGE(sm3::tbl_update(t, r, renumber.d(), renumber.null()) == 0,
                  sm1::errmsg());
  CHECK(held(t, r, 1) == title_ptr);
  CHECK(held(t, r, 2) == body_ptr);
  CHECK(chunks(t) == 2);
  CHECK(g_evals == evals);

  row_t rebody(2, title_ptr, b2, true, false);
  REQUIRE_MESSAGE(sm3::tbl_update(t, r, rebody.d(), rebody.null()) == 0,
                  sm1::errmsg());
  entries.clear();
  CHECK(find(t, f.docs_title, t1, &entries) == std::vector<uint64_t>{r});
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].find(b2) != std::string::npos);
  CHECK(find(t, f.docs_pre, b1.substr(0, prefix_len)).empty());
  CHECK(find(t, f.docs_pre, b2.substr(0, prefix_len)) ==
        std::vector<uint64_t>{r});
  CHECK(chunks(t) == 2);

  row_t retitle(2, t2, held(t, r, 2), false, true);
  REQUIRE_MESSAGE(sm3::tbl_update(t, r, retitle.d(), retitle.null()) == 0,
                  sm1::errmsg());
  CHECK(find(t, f.docs_title, t1).empty());
  entries.clear();
  CHECK(find(t, f.docs_title, t2, &entries) == std::vector<uint64_t>{r});
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].find(b2) != std::string::npos);
  CHECK(value(t, r, 1) == t2);
  CHECK(chunks(t) == 2);

  row_t untitle(2, "", held(t, r, 2), false, true);
  REQUIRE_MESSAGE(sm3::tbl_update(t, r, untitle.d(), untitle.null()) == 0,
                  sm1::errmsg());
  CHECK(find(t, f.docs_title, t2).empty());
  CHECK(find(t, f.docs_title, "") == std::vector<uint64_t>{r});
  CHECK(chunks(t) == 1);

  REQUIRE_MESSAGE(sm3::tbl_delete(t, r) == 0, sm1::errmsg());
  CHECK(find(t, f.docs_title, "").empty());
  CHECK(find(t, f.docs_pre, b2.substr(0, prefix_len)).empty());
  CHECK(chunks(t) == 0);
  CHECK_FALSE(g_saw_pointer);
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("an index built after its rows reads their values out of line") {
  // Verify the backfill fetches what the index is made of.
  // 1. Three rows with long titles are committed, then an index on
  //    title is made: pending.
  // 2. Another session fills it in one batch and marks it valid.
  // 3. The index finds each row by its title.
  docs_t f;
  std::vector<uint64_t> rownos;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 3; i++) {
    rownos.push_back(
        put(t, row_t(i, text_of(3000, "late-" + std::to_string(i)), "short")));
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  std::vector<sm3::index_key_spec_t> by_title = {
      {1, false, false, std::nullopt}};
  uint64_t late = 0;
  start(f.a);
  REQUIRE(sm3::create_index(f.a, f.docs, "docs_late", by_title, {}, false,
                            late) == 0);
  REQUIRE(sm3::commit(f.a) == 0);

  uint64_t next = 0;
  bool done = false;
  start(f.b);
  t = f.write(f.b);
  REQUIRE_MESSAGE(sm3::tbl_index_backfill(t, late, 0, 100, next, done) == 0,
                  sm1::errmsg());
  CHECK(done);
  sm3::tbl_close(t);
  REQUIRE(sm3::set_index_state(f.b, f.docs, late, sm3::index_state_t::valid) ==
          0);
  REQUIRE(sm3::commit(f.b) == 0);

  start(f.a);
  t = f.write(f.a);
  for (int i = 1; i <= 3; i++) {
    CHECK(find(t, late, text_of(3000, "late-" + std::to_string(i))) ==
          std::vector<uint64_t>{rownos[i - 1]});
  }
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("ANALYZE counts a datum out of line as wide, and fetches none but "
          "what an expression reads") {
  // Verify the statistics of a table with datums out of line.
  // 1. Forty rows with a short title and a 5000-byte body.
  // 2. tbl_analyze succeeds; body's width is the value's, about
  //    5000, not a pointer's; and it has no common value or bound,
  //    as a value too wide to sort has none.
  // 3. docs_pre's expression has statistics: the evaluator was
  //    shown the bodies, and never a pointer.
  docs_t f;
  start(f.a);
  sm3::table_t *t = f.write(f.a);
  for (int i = 1; i <= 40; i++) {
    put(t, row_t(i, "t" + std::to_string(i),
                 text_of(5000, "b" + std::to_string(i % 4))));
  }
  REQUIRE_MESSAGE(sm3::tbl_analyze(t, 10) == 0, sm1::errmsg());
  sm3::tbl_close(t);
  REQUIRE(sm3::commit(f.a) == 0);

  start(f.a);
  std::shared_ptr<const sm3::stats_t> st;
  REQUIRE(sm3::cat_get_stats(f.a, f.docs, st) == 0);
  REQUIRE(st);
  bool body = false, expr = false;
  for (const sm3::stat_t &s : st->stats) {
    if (!s.index && s.position == 2) {
      body = true;
      CHECK(s.width > 4900);
      CHECK(s.width < 5100);
      CHECK(s.mcv.empty());
      CHECK(s.bounds.empty());
    }
    if (s.index && *s.index == f.docs_pre) {
      expr = true;
      CHECK(s.distinct == 4);
    }
  }
  CHECK(body);
  CHECK(expr);
  CHECK_FALSE(g_saw_pointer);
  REQUIRE(sm3::commit(f.a) == 0);
}

TEST_CASE("dropping a table drops the sequence of its TOAST ids") {
  // Verify create_table and drop_table keep the sequence in step.
  // 1. docs has a sequence for its ids.
  // 2. Drop docs: the sequence is gone.
  docs_t f;
  sm1::session_t *s = sm3::session_sm1(f.a);
  std::string name = "sm3.toast." + std::to_string(f.docs);
  uint64_t last = 0;
  CHECK(sm1::lastseq(s, name, last) >= 0);
  start(f.a);
  REQUIRE_MESSAGE(sm3::drop_table(f.a, f.docs) == 0, sm1::errmsg());
  REQUIRE(sm3::commit(f.a) == 0);
  CHECK(sm1::lastseq(s, name, last) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_NOTFOUND);
}
