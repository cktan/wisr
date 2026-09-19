/*
 * test_limits.cc -- keys of SM_MAX_KEYLEN bytes and values of
 * SM_MAX_DATALEN bytes: accepted by every call, stored, and read
 * back intact, while one byte more is rejected.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"

#include <string>
#include <utility>
#include <vector>

namespace {

/* A database "db" with relation 1, reopenable. */
class limits_fixture_t {
public:
  /* Create "db" and relation 1. */
  limits_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(m_s, 1) == 0);
    REQUIRE(sm_commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm_session_t *s() const { return m_s; }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm_closedb(m_s) == 0);
    m_home.reattach();
    m_s = open();
  }

  /* Force relation 1 through a full compaction, so its versions are stamped. */
  void compact() {
    REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
    uint64_t cfid = 0;
    REQUIRE(wisr::catalog_lookup(m_s, wisr::xct_view(m_s), 1, &cfid) == 0);
    REQUIRE(sm_commit(m_s) == 0);
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(m_s->db->rdb
                ->CompactRange(opt, wisr::db_rel_cf(m_s->db, cfid), nullptr,
                               nullptr)
                .ok());
  }

private:
  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm_errmsg());
    return s;
  }

  home_t m_home;
  sm_session_t *m_s = nullptr;
};

/* A value of n bytes whose content depends on seed and position. */
std::string pattern(size_t n, unsigned seed) {
  std::string v(n, '\0');
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<char>((i * 31 + seed) % 251);
  }
  return v;
}

/* Every (key, value) a full scan of relation 1 returns, through s. */
std::vector<std::pair<std::string, std::string>> scan_all(sm_session_t *s) {
  sm_scan_t *c = sm_scan(s, 1, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE_MESSAGE(c, sm_errmsg());
  std::vector<std::pair<std::string, std::string>> rows;
  const char *k, *d;
  size_t kl, dl;
  while (sm_scannxt(c, &k, &kl, &d, &dl) == 0) {
    rows.emplace_back(std::string(k, kl), std::string(d, dl));
  }
  sm_scanend(c);
  return rows;
}

} // namespace

TEST_CASE("keys of SM_MAX_KEYLEN bytes work in every call and survive reopen") {
  // Verify the key limit end to end, including the SST path.
  // 1. Insert two maximum-length keys differing in the last
  //    byte, update one, and commit.
  // 2. Reopen, so the data is read back from SST files.
  // 3. Another transaction reads both, and a scan bounded by
  //    the two maximum-length keys returns both, whole.
  // 4. Delete one and commit; it is gone, the other remains.
  limits_fixture_t f;
  sm_session_t *s = f.s();
  std::string ka(SM_MAX_KEYLEN, 'k'), kb(SM_MAX_KEYLEN, 'k');
  ka.back() = 'a';
  kb.back() = 'b';
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, 1, ka, "va");
  put(s, 1, kb, "vb");
  REQUIRE(sm_update(s, 1, ka.data(), ka.size(), "va2", 3) == 0);
  REQUIRE(sm_commit(s) == 0);

  f.reopen();
  s = f.s();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, 1, ka) == "va2");
  CHECK(get(s, 1, kb) == "vb");
  sm_scan_t *c =
      sm_scan(s, 1, ka.data(), ka.size(), 1, kb.data(), kb.size(), 1);
  REQUIRE_MESSAGE(c, sm_errmsg());
  const char *k, *d;
  size_t kl, dl;
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == ka);
  CHECK(std::string(d, dl) == "va2");
  REQUIRE(sm_scannxt(c, &k, &kl, &d, &dl) == 0);
  CHECK(std::string(k, kl) == kb);
  CHECK(sm_scannxt(c, &k, &kl, &d, &dl) == 1);
  sm_scanend(c);
  REQUIRE(sm_delete(s, 1, ka.data(), ka.size()) == 0);
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, 1, ka) == "<none>");
  CHECK(get(s, 1, kb) == "vb");
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a key one byte over SM_MAX_KEYLEN is rejected by every call") {
  // Verify the key limit is enforced everywhere a key is taken.
  // 1. Insert, update, delete, read, and a scan with an
  //    over-long lower or upper bound all fail with
  //    SM_ERR_INVALID.
  // 2. None of them disturbs the transaction: an earlier write
  //    commits and reads back.
  limits_fixture_t f;
  sm_session_t *s = f.s();
  std::string over(SM_MAX_KEYLEN + 1, 'k');
  const char *d;
  size_t dl;
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, 1, "ok", "v");
  CHECK(sm_insert(s, 1, over.data(), over.size(), "v", 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_update(s, 1, over.data(), over.size(), "v", 1) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_delete(s, 1, over.data(), over.size()) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_read(s, 1, over.data(), over.size(), &d, &dl) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_scan(s, 1, over.data(), over.size(), 1, nullptr, 0, 0) == nullptr);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_scan(s, 1, nullptr, 0, 0, over.data(), over.size(), 1) == nullptr);
  CHECK(sm_errno() == SM_ERR_INVALID);
  REQUIRE(sm_commit(s) == 0);

  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(get(s, 1, "ok") == "v");
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("values of SM_MAX_DATALEN bytes are stored and read back intact") {
  // Verify the value limit end to end, through GC stamping and SSTs.
  // 1. Insert a 64 MiB patterned value and commit.
  // 2. Compact, which stamps the version by rewriting its value,
  //    then reopen.
  // 3. sm_read and sm_scannxt both return it byte for byte.
  // 4. Update it to another 64 MiB pattern; that reads back too.
  limits_fixture_t f;
  sm_session_t *s = f.s();
  std::string v1 = pattern(SM_MAX_DATALEN, 1);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, 1, "big", v1);
  REQUIRE(sm_commit(s) == 0);

  f.compact();
  f.reopen();
  s = f.s();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK((get(s, 1, "big") == v1)); // no 64 MiB dump on failure
  auto rows = scan_all(s);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "big");
  CHECK((rows[0].second == v1));

  std::string v2 = pattern(SM_MAX_DATALEN, 2);
  REQUIRE(sm_update(s, 1, "big", 3, v2.data(), v2.size()) == 0);
  REQUIRE(sm_commit(s) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK((get(s, 1, "big") == v2));
  REQUIRE(sm_commit(s) == 0);
}

TEST_CASE("a value one byte over SM_MAX_DATALEN is rejected") {
  // Verify the value limit on both calls that take a value.
  // 1. Insert and update with SM_MAX_DATALEN + 1 bytes fail with
  //    SM_ERR_INVALID.
  // 2. The key keeps its earlier value, and the transaction
  //    still commits.
  limits_fixture_t f;
  sm_session_t *s = f.s();
  std::string over(SM_MAX_DATALEN + 1, 'x');
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, 1, "k", "v");
  CHECK(sm_insert(s, 1, "k2", 2, over.data(), over.size()) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(sm_update(s, 1, "k", 1, over.data(), over.size()) == -1);
  CHECK(sm_errno() == SM_ERR_INVALID);
  CHECK(get(s, 1, "k") == "v");
  CHECK(get(s, 1, "k2") == "<none>");
  REQUIRE(sm_commit(s) == 0);
}
