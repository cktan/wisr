/*
 * mvcc.h -- the vocabulary every scenario in mvcc/ is written
 * in. MVCC.md describes the model; this header is its
 * executable form.
 *
 * Transactions are T1, T2, T3 and are addressed by number.
 * Rows are R1, R2, R3, plus R4 and R5 where a scenario needs
 * keys outside the predicate it scans. A scenario names the
 * rows it seeds and then drives each transaction call by call,
 * so the interleaving is exactly what the file says.
 *
 * Every scenario runs in each pairing of isolation levels:
 *
 *   RR    both transactions REPEATABLE READ
 *   RR+S  one of each, in both orders (P_RR_S and P_S_RR)
 *   S+S   both SERIALIZABLE
 *
 * A scenario's expected outcome is stated per pairing, because
 * that is the whole question these tests answer: which
 * conflicts each level detects, and which it lets through.
 */
#ifndef WISR_MVCC_H
#define WISR_MVCC_H

#include "doctest.h"

#include "sm1.h"

#include <stdlib.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

/* The rows a scenario may seed and address. */
#define R1 "r1"
#define R2 "r2"
#define R3 "r3"
#define R4 "r4"
#define R5 "r5"

/* The isolation level of each transaction in a scenario. */
enum pairing_t {
  P_RR_RR, /* T1 REPEATABLE READ,  T2 REPEATABLE READ */
  P_RR_S,  /* T1 REPEATABLE READ,  T2 SERIALIZABLE    */
  P_S_RR,  /* T1 SERIALIZABLE,     T2 REPEATABLE READ */
  P_S_S    /* T1 SERIALIZABLE,     T2 SERIALIZABLE    */
};

/* Every pairing, for the loop each scenario opens with. */
#define MVCC_PAIRINGS                                                          \
  { P_RR_RR, P_RR_S, P_S_RR, P_S_S }

/* Name the pairing in the failure output of everything below. */
#define PAIRING(p) INFO("pairing: " << pairing_name(p))

/* The pairings in which SSI tracks both transactions. */
inline bool both_serializable(pairing_t p) { return p == P_S_S; }

/* The name MVCC.md and the test output use for a pairing. */
inline std::string pairing_name(pairing_t p) {
  switch (p) {
  case P_RR_RR:
    return "RR: T1=RR, T2=RR";
  case P_RR_S:
    return "RR+S: T1=RR, T2=SERIALIZABLE";
  case P_S_RR:
    return "RR+S: T1=SERIALIZABLE, T2=RR";
  default:
    return "S+S: T1=SERIALIZABLE, T2=SERIALIZABLE";
  }
}

/* The level transaction n runs at in pairing p. T3 follows T2. */
inline sm1::xctlevel_t level_of(pairing_t p, int n) {
  bool ser = n == 1 ? (p == P_S_RR || p == P_S_S) : (p == P_RR_S || p == P_S_S);
  return ser ? sm1::xctlevel_t::serializable : sm1::xctlevel_t::repeatable_read;
}

/* A fresh directory under the system temp dir, removed at scope exit. */
class mvcc_tmpdir_t {
public:
  /* Create the directory. */
  mvcc_tmpdir_t() {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "wisr-mvcc-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
  }

  /* Remove the directory and everything in it. */
  ~mvcc_tmpdir_t() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  /* The directory's path. */
  const char *path() const { return m_path.c_str(); }

private:
  std::string m_path;
};

/*
 * One scenario run: a database holding one store, seeded
 * with rows, and up to three transactions driven by number.
 */
class scenario_t {
public:
  /* Create the database and seed the rows, each row -> "0". */
  explicit scenario_t(const std::vector<std::string> &rows = {}) {
    REQUIRE(sm1_init(m_tmp.path()) == 0);
    m_env = sm1_attach(m_tmp.path());
    REQUIRE(m_env);
    REQUIRE(sm1_mkdb(m_env, "db", nullptr) == 0);
    sm1::session_t *s = session(1);
    REQUIRE(sm1_begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE_MESSAGE(sm1_mkstore(s, &m_store) == 0, sm1::errmsg());
    // Seed each row in the transaction that creates the store.
    for (const std::string &row : rows) {
      REQUIRE(sm1_insert(s, m_store, row.data(), row.size(), "0", 1) == 0);
    }
    REQUIRE(sm1_commit(s) == 0);
  }

  /* Close the sessions and detach. */
  ~scenario_t() {
    for (sm1::session_t *s : m_sessions) {
      sm1_closedb(s);
    }
    sm1_detach(m_env, 1);
  }

  /* Transaction n's session, opened on first use. */
  sm1::session_t *session(int n) {
    while (static_cast<int>(m_sessions.size()) < n) {
      sm1::session_t *s = sm1_opendb(m_env, "db", nullptr);
      REQUIRE_MESSAGE(s, sm1::errmsg());
      m_sessions.push_back(s);
    }
    return m_sessions[n - 1];
  }

  /* The store every operation below works on. */
  uint64_t store() const { return m_store; }

  /* Begin T<n> at the level pairing p gives it. */
  void begin(int n, pairing_t p) {
    REQUIRE_MESSAGE(sm1_begin(session(n), level_of(p, n == 3 ? 2 : n)) == 0,
                    sm1::errmsg());
  }

  /* Begin T<n> at an explicit level. */
  void begin_at(int n, sm1::xctlevel_t level) {
    REQUIRE_MESSAGE(sm1_begin(session(n), level) == 0, sm1::errmsg());
  }

  /* r(row): the value T<n> reads, or "-" if no row is visible. */
  std::string r(int n, const std::string &row) {
    const char *data = nullptr;
    size_t len = 0;
    int rc = sm1_read(session(n), m_store, row.data(), row.size(), &data, &len);
    REQUIRE_MESSAGE(rc >= 0, sm1::errmsg());
    return rc == 1 ? "-" : std::string(data, len);
  }

  /* w(row): T<n> updates row; the result of sm1_update. */
  int w(int n, const std::string &row, const std::string &val) {
    return sm1_update(session(n), m_store, row.data(), row.size(), val.data(),
                      val.size(), nullptr, nullptr);
  }

  /* i(row): T<n> inserts row; the result of sm1_insert. */
  int i(int n, const std::string &row, const std::string &val) {
    return sm1_insert(session(n), m_store, row.data(), row.size(), val.data(),
                      val.size());
  }

  /* d(row): T<n> deletes row; the result of sm1_delete. */
  int d(int n, const std::string &row) {
    return sm1_delete(session(n), m_store, row.data(), row.size(), nullptr,
                      nullptr);
  }

  /* s(lo, hi): the keys T<n> sees in the closed range [lo, hi]. */
  std::vector<std::string> s(int n, const std::string &lo,
                             const std::string &hi) {
    sm1::scan_t *c = sm1_scan(session(n), m_store, lo.data(), lo.size(), 1,
                              hi.data(), hi.size(), 1);
    REQUIRE_MESSAGE(c, sm1::errmsg());
    std::vector<std::string> keys;
    const char *k, *d;
    size_t kl, dl;
    while (sm1_scannxt(c, &k, &kl, &d, &dl) == 0) {
      keys.emplace_back(k, kl);
    }
    sm1_scanend(c);
    return keys;
  }

  /* s(lo, hi) as (key, value) pairs. */
  std::vector<std::pair<std::string, std::string>>
  srows(int n, const std::string &lo, const std::string &hi) {
    sm1::scan_t *c = sm1_scan(session(n), m_store, lo.data(), lo.size(), 1,
                              hi.data(), hi.size(), 1);
    REQUIRE_MESSAGE(c, sm1::errmsg());
    std::vector<std::pair<std::string, std::string>> rows;
    const char *k, *d;
    size_t kl, dl;
    while (sm1_scannxt(c, &k, &kl, &d, &dl) == 0) {
      rows.emplace_back(std::string(k, kl), std::string(d, dl));
    }
    sm1_scanend(c);
    return rows;
  }

  /*
   * c: commit T<n>, reporting whether it committed. A failure
   * must be SM_ERR_SERIALIZATION -- the only commit-time
   * failure the design allows (SM1_DESIGN.md §7.3) -- and must be
   * retryable.
   */
  bool c(int n) {
    if (sm1_commit(session(n)) == 0) {
      return true;
    }
    CHECK(sm1::errnum() == sm1::SM_ERR_SERIALIZATION);
    CHECK(sm1::retryable());
    return false;
  }

  /* a: abort T<n>. */
  void a(int n) { REQUIRE(sm1_abort(session(n)) == 0); }

  /* The rows a fresh REPEATABLE READ transaction sees. */
  std::vector<std::pair<std::string, std::string>> final_rows() {
    sm1::session_t *s = session(static_cast<int>(m_sessions.size()) + 1);
    REQUIRE(sm1_begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    sm1::scan_t *cur = sm1_scan(s, m_store, nullptr, 0, 0, nullptr, 0, 0);
    REQUIRE_MESSAGE(cur, sm1::errmsg());
    std::vector<std::pair<std::string, std::string>> rows;
    const char *k, *d;
    size_t kl, dl;
    while (sm1_scannxt(cur, &k, &kl, &d, &dl) == 0) {
      rows.emplace_back(std::string(k, kl), std::string(d, dl));
    }
    sm1_scanend(cur);
    REQUIRE(sm1_commit(s) == 0);
    return rows;
  }

  /* The keys a fresh REPEATABLE READ transaction sees. */
  std::vector<std::string> final_keys() {
    std::vector<std::string> keys;
    for (const auto &kv : final_rows()) {
      keys.push_back(kv.first);
    }
    return keys;
  }

  /* The value of row as a fresh transaction reads it. */
  std::string final_value(const std::string &row) {
    for (const auto &kv : final_rows()) {
      if (kv.first == row) {
        return kv.second;
      }
    }
    return "-";
  }

private:
  mvcc_tmpdir_t m_tmp;
  sm1::env_t *m_env = nullptr;
  std::vector<sm1::session_t *> m_sessions;
  uint64_t m_store = 0;
};

/* Expect rc to be -1 with error e, the way a scenario states it. */
inline void fails(int rc, sm1::errnum_t e) {
  CHECK(rc == -1);
  CHECK(sm1::errnum() == e);
}

/* Expect rc to be -1 with SM_ERR_CONFLICT, which is retryable. */
inline void conflicts(int rc) {
  CHECK(rc == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CONFLICT);
  CHECK(sm1::retryable());
}

#endif /* WISR_MVCC_H */
