/*
 * test_limits.cc -- keys of sm1::max_keylen bytes and values of
 * sm1::max_datalen bytes: accepted by every call, stored, and read
 * back intact, while one byte more is rejected.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm1.h"

#include <string>
#include <utility>
#include <vector>

namespace {

/* A database "db" with store 1, reopenable. */
class limits_fixture_t {
public:
  /* Create "db" and store 1. */
  limits_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_s = open();
    REQUIRE(sm1::begin(m_s, sm1::xctlevel_t::repeatable_read) == 0);
    m_store = mkstore(m_s);
    REQUIRE(sm1::commit(m_s) == 0);
  }

  /* The fixture's session. */
  sm1::session_t *s() const { return m_s; }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* Close the session, reattach, and reopen "db". */
  void reopen() {
    REQUIRE(sm1::closedb(m_s) == 0);
    m_home.reattach();
    m_s = open();
  }

  /* Force the store through a full compaction, so its versions are
     stamped. */
  void compact() {
    rocksdb::CompactRangeOptions opt;
    opt.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
    REQUIRE(m_s->db->rdb
                ->CompactRange(opt, wisr::db_store(m_s->db, m_store).cf,
                               nullptr, nullptr)
                .ok());
  }

private:
  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

  home_t m_home;
  sm1::session_t *m_s = nullptr;
  uint64_t m_store = 0;
};

/* A value of n bytes whose content depends on seed and position. */
std::string pattern(size_t n, unsigned seed) {
  std::string v(n, '\0');
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<char>((i * 31 + seed) % 251);
  }
  return v;
}

/* Every (key, value) a full scan of store returns, through s. */
std::vector<std::pair<std::string, std::string>> scan_all(sm1::session_t *s,
                                                          uint64_t store) {
  sm1::scan_t *c = scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  REQUIRE_MESSAGE(c, sm1::errmsg());
  std::vector<std::pair<std::string, std::string>> rows;
  std::string_view k, d;
  while (sm1::scannxt(c, k, d) == 0) {
    rows.emplace_back(std::string(k), std::string(d));
  }
  sm1::scanend(c);
  return rows;
}

} // namespace

TEST_CASE(
    "keys of sm1::max_keylen bytes work in every call and survive reopen") {
  // Verify the key limit end to end, including the SST path.
  // 1. Insert two maximum-length keys differing in the last
  //    byte, update one, and commit.
  // 2. Reopen, so the data is read back from SST files.
  // 3. Another transaction reads both, and a scan bounded by
  //    the two maximum-length keys returns both, whole.
  // 4. Delete one and commit; it is gone, the other remains.
  limits_fixture_t f;
  sm1::session_t *s = f.s();
  std::string ka(sm1::max_keylen, 'k'), kb(sm1::max_keylen, 'k');
  ka.back() = 'a';
  kb.back() = 'b';
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), ka, "va");
  put(s, f.store(), kb, "vb");
  REQUIRE(sm1::update(s, f.store(), ka, "va2") == 0);
  REQUIRE(sm1::commit(s) == 0);

  f.reopen();
  s = f.s();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, f.store(), ka) == "va2");
  CHECK(get(s, f.store(), kb) == "vb");
  sm1::scan_t *c =
      scan(s, f.store(), ka.data(), ka.size(), 1, kb.data(), kb.size(), 1);
  REQUIRE_MESSAGE(c, sm1::errmsg());
  std::string_view k, d;
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == ka);
  CHECK(std::string(d) == "va2");
  REQUIRE(sm1::scannxt(c, k, d) == 0);
  CHECK(std::string(k) == kb);
  CHECK(sm1::scannxt(c, k, d) == 1);
  sm1::scanend(c);
  REQUIRE(sm1::erase(s, f.store(), ka) == 0);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, f.store(), ka) == "<none>");
  CHECK(get(s, f.store(), kb) == "vb");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a key one byte over sm1::max_keylen is rejected by every call") {
  // Verify the key limit is enforced everywhere a key is taken.
  // 1. Insert, update, delete, read, and a scan with an
  //    over-long lower or upper bound all fail with
  //    SM_ERR_INVALID.
  // 2. None of them disturbs the transaction: an earlier write
  //    commits and reads back.
  limits_fixture_t f;
  sm1::session_t *s = f.s();
  std::string over(sm1::max_keylen + 1, 'k');
  std::string_view d;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "ok", "v");
  CHECK(sm1::insert(s, f.store(), over, "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::update(s, f.store(), over, "v") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::erase(s, f.store(), over) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::read(s, f.store(), over, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(scan(s, f.store(), over.data(), over.size(), 1, nullptr, 0, 0) ==
        nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(scan(s, f.store(), nullptr, 0, 0, over.data(), over.size(), 1) ==
        nullptr);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, f.store(), "ok") == "v");
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("values of sm1::max_datalen bytes are stored and read back intact") {
  // Verify the value limit end to end, through GC stamping and SSTs.
  // 1. Insert a 64 MiB patterned value and commit.
  // 2. Compact, which stamps the version by rewriting its value,
  //    then reopen.
  // 3. sm1::read and sm1::scannxt both return it byte for byte.
  // 4. Update it to another 64 MiB pattern; that reads back too.
  limits_fixture_t f;
  sm1::session_t *s = f.s();
  std::string v1 = pattern(sm1::max_datalen, 1);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "big", v1);
  REQUIRE(sm1::commit(s) == 0);

  f.compact();
  f.reopen();
  s = f.s();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK((get(s, f.store(), "big") == v1)); // no 64 MiB dump on failure
  auto rows = scan_all(s, f.store());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].first == "big");
  CHECK((rows[0].second == v1));

  std::string v2 = pattern(sm1::max_datalen, 2);
  REQUIRE(sm1::update(s, f.store(), "big", v2) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK((get(s, f.store(), "big") == v2));
  REQUIRE(sm1::commit(s) == 0);
}

TEST_CASE("a value one byte over sm1::max_datalen is rejected") {
  // Verify the value limit on both calls that take a value.
  // 1. Insert and update with sm1::max_datalen + 1 bytes fail with
  //    SM_ERR_INVALID.
  // 2. The key keeps its earlier value, and the transaction
  //    still commits.
  limits_fixture_t f;
  sm1::session_t *s = f.s();
  std::string over(sm1::max_datalen + 1, 'x');
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "k", "v");
  CHECK(sm1::insert(s, f.store(), "k2", over) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm1::update(s, f.store(), "k", over) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(get(s, f.store(), "k") == "v");
  CHECK(get(s, f.store(), "k2") == "<none>");
  REQUIRE(sm1::commit(s) == 0);
}
