/*
 * test_ssi.cc -- SERIALIZABLE via SSI: the anomalies it must
 * prevent, and the same anomalies observed at REPEATABLE READ
 * (DESIGN.md §7, §12.5 "Isolation").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A database "db" with relation "t" and the given rows. */
class ssi_fixture_t {
public:
  /* Create "db", "t", and each key -> "1". */
  explicit ssi_fixture_t(const std::vector<std::string> &keys) {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    sm_session_t *s = open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "t") == 0);
    for (const std::string &k : keys) {
      put(s, "t", k, "1");
    }
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_close(s) == 0);
  }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_open(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

private:
  home_t m_home;
};

/* Set key in "t" to val; the result of sm_update. */
int set(sm_session_t *s, const std::string &key, const std::string &val) {
  return sm_update(s, "t", key.data(), key.size(), val.data(), val.size());
}

/* Insert key -> val into "t"; the result of sm_insert. */
int ins(sm_session_t *s, const std::string &key, const std::string &val) {
  return sm_insert(s, "t", key.data(), key.size(), val.data(), val.size());
}

/* The number of rows a scan of [lo, hi] in "t" returns. */
int count_range(sm_session_t *s, const std::string &lo, const std::string &hi) {
  sm_scan_t *c =
      sm_scan(s, "t", lo.data(), lo.size(), 1, hi.data(), hi.size(), 1);
  REQUIRE_MESSAGE(c, sm_errmsg());
  int n = 0;
  const char *k, *d;
  size_t kl, dl;
  while (sm_scan_next(c, &k, &kl, &d, &dl) == 0) {
    n++;
  }
  sm_scan_end(c);
  return n;
}

/* Commit s; true if it committed, false on a serialization failure. */
bool try_commit(sm_session_t *s) {
  if (sm_commit(s) == 0) {
    return true;
  }
  CHECK(sm_errno() == SM_ERR_SERIALIZATION);
  CHECK(sm_retryable() == 1);
  CHECK(sm_abort(s) == -1); // already aborted by the engine
  return false;
}

} // namespace

TEST_CASE(
    "write skew fails at SERIALIZABLE and is observable at REPEATABLE READ") {
  // Verify the on-call doctors anomaly (SIREAD.md).
  // 1. Doctors "alice" and "bob" are both on call ("1").
  // 2. T1 and T2 each read both, then each takes a different
  //    doctor off call.
  // 3. At SERIALIZABLE they must not both commit; at
  //    REPEATABLE READ both do, leaving nobody on call.
  for (sm_xctlevel_t level : {SM_XCT_SERIALIZABLE, SM_XCT_REPEATABLE_READ}) {
    CAPTURE(level);
    ssi_fixture_t f({"alice", "bob"});
    sm_session_t *t1 = f.open();
    sm_session_t *t2 = f.open();
    REQUIRE(sm_begin(t1, level) == 0);
    REQUIRE(sm_begin(t2, level) == 0);
    for (sm_session_t *s : {t1, t2}) {
      CHECK(get(s, "t", "alice") == "1");
      CHECK(get(s, "t", "bob") == "1");
    }
    REQUIRE(set(t1, "alice", "0") == 0);
    REQUIRE(set(t2, "bob", "0") == 0);
    bool c1 = try_commit(t1);
    bool c2 = try_commit(t2);
    if (level == SM_XCT_SERIALIZABLE) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK((c1 && c2));
    }
    sm_close(t1);
    sm_close(t2);
  }
}

TEST_CASE("write skew through phantoms fails at SERIALIZABLE") {
  // Verify range SIREAD entries catch inserts into a scanned range.
  // 1. T1 and T2 each count the rows in [a, z] (one row, "m").
  // 2. Each inserts a different new row into that range.
  // 3. At SERIALIZABLE they must not both commit; at
  //    REPEATABLE READ both do.
  for (sm_xctlevel_t level : {SM_XCT_SERIALIZABLE, SM_XCT_REPEATABLE_READ}) {
    CAPTURE(level);
    ssi_fixture_t f({"m"});
    sm_session_t *t1 = f.open();
    sm_session_t *t2 = f.open();
    REQUIRE(sm_begin(t1, level) == 0);
    REQUIRE(sm_begin(t2, level) == 0);
    CHECK(count_range(t1, "a", "z") == 1);
    CHECK(count_range(t2, "a", "z") == 1);
    REQUIRE(ins(t1, "c", "1") == 0);
    REQUIRE(ins(t2, "x", "1") == 0);
    bool c1 = try_commit(t1);
    bool c2 = try_commit(t2);
    if (level == SM_XCT_SERIALIZABLE) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK((c1 && c2));
    }
    sm_close(t1);
    sm_close(t2);
  }
}

TEST_CASE("reading a missing key counts: inserting it later conflicts") {
  // Verify a point read that finds nothing leaves a SIREAD entry (§7.1).
  // 1. T1 reads "p" (missing) and inserts "q"; T2 reads "q"
  //    (missing) and inserts "p".
  // 2. They must not both commit at SERIALIZABLE.
  ssi_fixture_t f({});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t1, "t", "p") == "<none>");
  CHECK(get(t2, "t", "q") == "<none>");
  REQUIRE(ins(t1, "q", "1") == 0);
  REQUIRE(ins(t2, "p", "1") == 0);
  bool c1 = try_commit(t1);
  bool c2 = try_commit(t2);
  CHECK_FALSE((c1 && c2));
  sm_close(t1);
  sm_close(t2);
}

TEST_CASE(
    "a point read past a concurrent writer's newer version creates an edge") {
  // Verify on_skip and the committed-pivot rule (§7.2, §10.1).
  // 1. T1 begins. T2 reads "y", updates "x", and commits.
  // 2. T1 reads "x": it steps past T2's newer version, giving
  //    T1 -> T2, so T2 is a committed transaction with an
  //    edge in.
  // 3. T1 updates "y", which T2 read: T2 -> T1 would make T2 a
  //    committed pivot, so T1 is doomed and fails at commit.
  ssi_fixture_t f({"x", "y"});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t2, "t", "y") == "1");
  REQUIRE(set(t2, "x", "2") == 0);
  REQUIRE(try_commit(t2));

  CHECK(get(t1, "t", "x") == "1");
  REQUIRE(set(t1, "y", "2") == 0);
  CHECK_FALSE(try_commit(t1));
  sm_close(t1);
  sm_close(t2);
}

TEST_CASE("an edge onto a committed pivot dooms the transaction making it") {
  // Verify Cahill's rule for a pivot that gains its second edge
  // after committing, where the newcomer gets only one flag.
  // 1. T3 reads "k"; T2 reads "y", updates "k" (T3 -> T2), and
  //    commits with an edge in.
  // 2. T1 updates "y", which T2 read: T2 -> T1 makes the
  //    committed T2 a pivot, so T1 -- with no edge out of its
  //    own -- is doomed and fails at commit.
  ssi_fixture_t f({"k", "y"});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  sm_session_t *t3 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t3, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t3, "t", "k") == "1");
  CHECK(get(t2, "t", "y") == "1");
  REQUIRE(set(t2, "k", "2") == 0);
  REQUIRE(try_commit(t2));

  REQUIRE(set(t1, "y", "2") == 0);
  CHECK_FALSE(try_commit(t1));
  sm_close(t1);
  sm_close(t2);
  sm_close(t3);
}

TEST_CASE("a phantom inserted while the reader's cursor is open is caught") {
  // Verify the range is registered before the cursor's snapshot (§10.2).
  // 1. T1 opens a scan over [a, z] and reads one row.
  // 2. T2 reads "q", inserts "n" into T1's range, and commits:
  //    T1 -> T2 through the open range.
  // 3. T1 finishes the scan (it does not see "n"), then
  //    updates "q", which T2 read: T1 must fail.
  ssi_fixture_t f({"b", "q"});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  sm_scan_t *c = sm_scan(t1, "t", "a", 1, 1, "z", 1, 1);
  REQUIRE(c);
  const char *k, *d;
  size_t kl, dl;
  REQUIRE(sm_scan_next(c, &k, &kl, &d, &dl) == 0);

  CHECK(get(t2, "t", "q") == "1");
  REQUIRE(ins(t2, "n", "1") == 0);
  REQUIRE(try_commit(t2));

  int rest = 0;
  while (sm_scan_next(c, &k, &kl, &d, &dl) == 0) {
    CHECK(std::string(k, kl) != "n");
    rest++;
  }
  CHECK(rest == 1); // "q"
  sm_scan_end(c);
  REQUIRE(set(t1, "q", "2") == 0);
  CHECK_FALSE(try_commit(t1));
  sm_close(t1);
  sm_close(t2);
}

TEST_CASE("a scan closed early covers only what it read") {
  // Verify §7.1's narrowing: no edge for keys the scan never reached.
  // 1. T1 opens a scan over [a, z], reads only "b", closes it.
  // 2. T2 reads "q", then updates "y" (past where T1 stopped).
  // 3. T1 updates "q", which T2 read: T2 -> T1 only.
  // 4. Both commit: with the range narrowed to [a, b], T1 has
  //    no edge out, so it is no pivot.
  ssi_fixture_t f({"b", "q", "y"});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  sm_scan_t *c = sm_scan(t1, "t", "a", 1, 1, "z", 1, 1);
  REQUIRE(c);
  const char *k, *d;
  size_t kl, dl;
  REQUIRE(sm_scan_next(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == "b");
  sm_scan_end(c);

  CHECK(get(t2, "t", "q") == "1");
  REQUIRE(set(t2, "y", "2") == 0);
  REQUIRE(set(t1, "q", "2") == 0);
  CHECK(try_commit(t1));
  CHECK(try_commit(t2));
  sm_close(t1);
  sm_close(t2);
}

TEST_CASE("Fekete's read-only anomaly fails the pivot") {
  // Verify SSI prevents the read-only transaction anomaly (§7.4).
  // Checking "x" and savings "y" start at 0. T1 withdraws from
  // checking, charging a penalty if x + y would go negative;
  // T2 deposits 20 into savings; T3 reports both.
  // 1. T2 reads y; T1 reads x and y; T2 writes y = 20, commits.
  // 2. T3 reads x = 0 and y = 20 and commits.
  // 3. T1 writes x = -11 (penalty applied, having seen y = 0):
  //    T3 -> T1 -> T2 makes T1 a pivot, so T1 must fail.
  ssi_fixture_t f({});
  {
    sm_session_t *s = f.open();
    REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
    put(s, "t", "x", "0");
    put(s, "t", "y", "0");
    REQUIRE(sm_commit(s) == 0);
    sm_close(s);
  }
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  sm_session_t *t3 = f.open();
  REQUIRE(sm_begin(t2, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t2, "t", "y") == "0");
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t1, "t", "x") == "0");
  CHECK(get(t1, "t", "y") == "0");
  REQUIRE(set(t2, "y", "20") == 0);
  REQUIRE(try_commit(t2));

  REQUIRE(sm_begin(t3, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t3, "t", "x") == "0");
  CHECK(get(t3, "t", "y") == "20");
  REQUIRE(try_commit(t3));

  REQUIRE(set(t1, "x", "-11") == 0);
  CHECK_FALSE(try_commit(t1));
  sm_close(t1);
  sm_close(t2);
  sm_close(t3);
}

TEST_CASE("a reader that outlives every concurrent writer can then write and "
          "commit") {
  // Verify reads alone never doom a transaction (§7.4, §12.5).
  // 1. T reads "x"; a concurrent writer U updates "y" and
  //    commits.
  // 2. T then updates "x" and commits, also on an idle database
  //    with no other transaction at all.
  ssi_fixture_t f({"x", "y"});
  sm_session_t *t = f.open();
  sm_session_t *u = f.open();
  REQUIRE(sm_begin(t, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t, "t", "x") == "1");
  REQUIRE(sm_begin(u, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(set(u, "y", "2") == 0);
  REQUIRE(try_commit(u));
  REQUIRE(set(t, "x", "2") == 0);
  CHECK(try_commit(t));

  REQUIRE(sm_begin(t, SM_XCT_SERIALIZABLE) == 0);
  CHECK(get(t, "t", "x") == "2");
  CHECK(count_range(t, "a", "z") == 2);
  REQUIRE(set(t, "x", "3") == 0);
  CHECK(try_commit(t));
  sm_close(t);
  sm_close(u);
}

TEST_CASE("a REPEATABLE READ writer does not take part in SSI") {
  // Verify the PostgreSQL rule: only SERIALIZABLE transactions
  // are tracked, so write skew with an RR partner is allowed.
  // 1. T1 (SERIALIZABLE) and T2 (REPEATABLE READ) both read
  //    "alice" and "bob", then each takes one off call.
  // 2. Both commit.
  ssi_fixture_t f({"alice", "bob"});
  sm_session_t *t1 = f.open();
  sm_session_t *t2 = f.open();
  REQUIRE(sm_begin(t1, SM_XCT_SERIALIZABLE) == 0);
  REQUIRE(sm_begin(t2, SM_XCT_REPEATABLE_READ) == 0);
  for (sm_session_t *s : {t1, t2}) {
    CHECK(get(s, "t", "alice") == "1");
    CHECK(get(s, "t", "bob") == "1");
  }
  REQUIRE(set(t1, "alice", "0") == 0);
  REQUIRE(set(t2, "bob", "0") == 0);
  CHECK(try_commit(t1));
  CHECK(try_commit(t2));
  sm_close(t1);
  sm_close(t2);
}

TEST_CASE("SSI nodes are freed once no transaction overlaps them") {
  // Verify retention by the watermark (§7.1).
  // 1. Run a few SERIALIZABLE transactions that read and write.
  // 2. With none active, run GC: no node and no SIREAD entry
  //    remains.
  ssi_fixture_t f({"x", "y"});
  sm_session_t *s = f.open();
  for (int i = 0; i < 5; i++) {
    REQUIRE(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
    CHECK(get(s, "t", "x") != "<none>");
    CHECK(count_range(s, "a", "z") == 2);
    REQUIRE(set(s, "y", std::to_string(i)) == 0);
    REQUIRE(sm_commit(s) == 0);
  }
  wisr::gc_run_once(s->db);
  {
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    CHECK(s->db->ssi_nodes.empty());
    CHECK(s->db->ssi_points.empty());
    CHECK(s->db->ssi_by_xid.empty());
    for (auto &kv : s->db->ssi_ranges) {
      CHECK(kv.second.empty());
    }
  }
  sm_close(s);
}

TEST_CASE("concurrent SERIALIZABLE doctors always leave someone on call") {
  // Verify serializability under contention, with retries.
  // 1. Four doctors d0..d3 start on call. Four threads, one per
  //    doctor, repeatedly: count those on call; if at least two
  //    are, go off call; else go back on call; commit, retrying
  //    serialization failures and conflicts.
  // 2. Every committed snapshot a checker thread reads has at
  //    least one doctor on call.
  ssi_fixture_t f({"d0", "d1", "d2", "d3"});
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0}, violations{0}, commits{0};
  std::vector<std::thread> threads;
  std::vector<sm_session_t *> sessions;
  for (int i = 0; i < 5; i++) {
    sessions.push_back(f.open());
  }
  for (int i = 0; i < 4; i++) {
    threads.emplace_back([&, i] {
      sm_session_t *s = sessions[i];
      std::string me = "d" + std::to_string(i);
      for (int round = 0; round < 50; round++) {
        if (sm_begin(s, SM_XCT_SERIALIZABLE) != 0) {
          errors++;
          return;
        }
        sm_scan_t *c = sm_scan(s, "t", "d0", 2, 1, "d3", 2, 1);
        int on = 0;
        const char *k, *d;
        size_t kl, dl;
        while (c && sm_scan_next(c, &k, &kl, &d, &dl) == 0) {
          on += std::string(d, dl) == "1";
        }
        sm_scan_end(c);
        const char *v = on >= 2 ? "0" : "1";
        if (sm_update(s, "t", me.data(), me.size(), v, 1) == 0 &&
            sm_commit(s) == 0) {
          commits++;
        } else if (!sm_retryable()) {
          errors++;
          sm_abort(s);
          return;
        }
      }
    });
  }
  std::thread checker([&] {
    sm_session_t *s = sessions[4];
    while (!stop) {
      if (sm_begin(s, SM_XCT_SERIALIZABLE) != 0) {
        errors++;
        return;
      }
      int on = 0;
      for (int i = 0; i < 4; i++) {
        on += get(s, "t", "d" + std::to_string(i)) == "1";
      }
      if (on == 0) {
        violations++;
      }
      if (sm_commit(s) != 0 && !sm_retryable()) {
        errors++;
      }
    }
  });
  for (std::thread &t : threads) {
    t.join();
  }
  stop = true;
  checker.join();
  CHECK(errors == 0);
  CHECK(violations == 0);
  CHECK(commits > 0);
  MESSAGE("commits: " << commits.load());
  for (sm_session_t *s : sessions) {
    sm_close(s);
  }
}
