/*
 * test_log.cc -- the process-wide log hook (sm1::set_log) and the
 * events wisr reports through it: what no caller would
 * otherwise learn.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"
#include "version.h"

#include <sys/stat.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/* Captures log messages while in scope; removes the hook after. */
class log_capture_t {
public:
  /* Install the capturing hook. */
  log_capture_t() { sm1::set_log(hook, this); }

  /* Remove it. */
  ~log_capture_t() { sm1::set_log(nullptr, nullptr); }

  /* How many messages at level contain needle. */
  size_t count(sm1::loglevel_t level, const std::string &needle) {
    std::lock_guard<std::mutex> guard(m_mu);
    size_t n = 0;
    for (auto &m : m_msgs) {
      n += m.first == level && m.second.find(needle) != std::string::npos;
    }
    return n;
  }

  /*
   * The position of the first message at level containing needle,
   * or SIZE_MAX if there is none.
   */
  size_t index_of(sm1::loglevel_t level, const std::string &needle) {
    std::lock_guard<std::mutex> guard(m_mu);
    for (size_t i = 0; i < m_msgs.size(); i++) {
      if (m_msgs[i].first == level &&
          m_msgs[i].second.find(needle) != std::string::npos) {
        return i;
      }
    }
    return SIZE_MAX;
  }

  /* Every message so far, for failure output. */
  std::string all() {
    std::lock_guard<std::mutex> guard(m_mu);
    std::string out;
    for (auto &m : m_msgs) {
      out += std::to_string(static_cast<int>(m.first)) + ": " + m.second + "\n";
    }
    return out;
  }

private:
  /* The hook: record the message under the capture's mutex. */
  static void hook(void *arg, sm1::loglevel_t level, const char *msg) noexcept {
    log_capture_t *self = static_cast<log_capture_t *>(arg);
    std::lock_guard<std::mutex> guard(self->m_mu);
    self->m_msgs.emplace_back(level, msg);
  }

  std::mutex m_mu;
  std::vector<std::pair<sm1::loglevel_t, std::string>> m_msgs;
};

/* A database "db" with store 1, reopenable. */
class log_fixture_t {
public:
  /* Create "db" and store 1. */
  log_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    m_store = mkstore(s);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    return s;
  }

  /* The home directory. */
  home_t &home() { return m_home; }

private:
  home_t m_home;
  uint64_t m_store = 0;
};

/* Insert key -> "v" into store in one committed transaction. */
void commit_put(sm1::session_t *s, uint64_t store, const std::string &key) {
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, store, key, "v");
  REQUIRE(sm1::commit(s) == 0);
}

} // namespace

TEST_CASE("with no hook nothing is reported, and a hook can be replaced") {
  // Verify installing, replacing, and removing the hook.
  // 1. Capture A sees the attach sweep's message.
  // 2. Capture B replaces A: B sees the next one, A does not.
  // 3. After B is removed, a third event reaches neither.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  std::string base = std::string(tmp.path()) + "/base";
  auto sweep_once = [&](const char *dbid) {
    REQUIRE(mkdir((base + "/" + dbid).c_str(), 0700) == 0);
    sm1::env_t *env = attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm1::detach(env, 0) == 0);
  };
  size_t a_before;
  {
    log_capture_t a;
    sweep_once("7");
    CHECK(a.count(sm1::loglevel_t::info, "/base/7") == 1);
    {
      log_capture_t b;
      sweep_once("8");
      CHECK(b.count(sm1::loglevel_t::info, "/base/8") == 1);
      CHECK(a.count(sm1::loglevel_t::info, "/base/8") == 0);
    }
    a_before = a.count(sm1::loglevel_t::info, "destroyed");
    sweep_once("9"); // no hook: b removed it
    CHECK(a.count(sm1::loglevel_t::info, "destroyed") == a_before);
  }
}

TEST_CASE("recovery reports restored transactions and dropped orphans") {
  // Verify §9.1's recovery events reach the hook.
  // 1. Prepare "g-log"; create a store CF no catalog row
  //    names, as a crash inside sm1::mkstore would leave.
  // 2. Restart: the attach's recovery logs an INFO message naming
  //    "g-log" as restored, and one reporting the orphaned CF dropped.
  // 3. Open a session: "g-log" is restored again, still in doubt;
  //    the orphan is gone already.
  log_fixture_t f;
  sm1::session_t *s = f.open();
  uint64_t orphan = 0;
  REQUIRE(wisr::db_alloc_objid(s->db, &orphan) == 0);
  REQUIRE(wisr::db_create_store_cf(s->db, orphan) == 0);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "k", "v");
  REQUIRE(sm1::prepare(s, "g-log") == 0);

  log_capture_t cap;
  f.home().detach();
  f.home().attach();
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::info, "g-log") == 1, cap.all());
  CHECK_MESSAGE(
      cap.count(sm1::loglevel_t::info, "store." + std::to_string(orphan)) == 1,
      cap.all());
  sm1::session_t *v = f.open();
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::info, "g-log") == 2, cap.all());
  CHECK_MESSAGE(
      cap.count(sm1::loglevel_t::info, "store." + std::to_string(orphan)) == 1,
      cap.all());
  REQUIRE(sm1::abort_prepared(v, "g-log") == 0);
  sm1::closedb(v);
}

TEST_CASE("attach logs each database before recovering it") {
  // Verify attach announces a database, then recovers it, then returns.
  // 1. Prepare "g-order" in "db", and detach.
  // 2. Attach: an INFO message names "db" as being recovered.
  // 3. It precedes the recovery's own message restoring "g-order",
  //    and both are logged by the time attach returns.
  // 4. No database is left open: a session's opendb reopens it.
  log_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "k", "v");
  REQUIRE(sm1::prepare(s, "g-order") == 0);

  log_capture_t cap;
  f.home().detach();
  f.home().attach();
  size_t announced =
      cap.index_of(sm1::loglevel_t::info, "recovering database db");
  size_t restored = cap.index_of(sm1::loglevel_t::info, "g-order");
  REQUIRE_MESSAGE(announced != SIZE_MAX, cap.all());
  REQUIRE_MESSAGE(restored != SIZE_MAX, cap.all());
  CHECK_MESSAGE(announced < restored, cap.all());
  CHECK(f.home().env()->dbs.empty());

  sm1::session_t *v = f.open();
  CHECK(f.home().env()->dbs.size() == 1);
  REQUIRE(sm1::abort_prepared(v, "g-order") == 0);
  sm1::closedb(v);
}

TEST_CASE("a failed status write is reported as the database stopping") {
  // Verify fail-stop is logged, not only returned to one caller.
  // 1. Make the next commit's status write fail.
  // 2. An ERROR message says the database stopped.
  log_fixture_t f;
  sm1::session_t *s = f.open();
  log_capture_t cap;
  s->db->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  put(s, f.store(), "k", "v");
  CHECK(sm1::commit(s) == -1);
  s->db->test_before_commit = nullptr;
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::error, "stopped") == 1, cap.all());
  sm1::closedb(s);
}

TEST_CASE("a CF:txn sweep reports what it deleted") {
  // Verify a completed background sweep is logged.
  // 1. Commit a few transactions, then sweep.
  // 2. An INFO message reports the records deleted.
  log_fixture_t f;
  sm1::session_t *s = f.open();
  for (int i = 0; i < 3; i++) {
    commit_put(s, f.store(), "k" + std::to_string(i));
  }
  log_capture_t cap;
  CHECK(wisr::gc_sweep_txn(s->db, true) > 0);
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::info, "sweep deleted") == 1,
                cap.all());
  sm1::closedb(s);
}

TEST_CASE("versions the filter cannot judge are reported per compaction") {
  // Verify the filter's doubts, and the sweep refusing to trust them.
  // 1. Plant a version with an undecodable (empty) value in store 1.
  // 2. Compact: each compaction job that meets it reports it
  //    once, as one version -- a summary, not a line per version.
  // 3. A sweep's pass meets it again and is abandoned, with a
  //    WARN saying the status records are kept.
  log_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  rocksdb::ColumnFamilyHandle *cf = wisr::db_store(s->db, f.store()).cf;
  REQUIRE(s->db->rdb
              ->Put(rocksdb::WriteOptions(), cf, wisr::version_key("bad", 1, 1),
                    rocksdb::Slice())
              .ok());

  log_capture_t cap;
  rocksdb::CompactRangeOptions opt;
  opt.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  REQUIRE(s->db->rdb->CompactRange(opt, cf, nullptr, nullptr).ok());
  size_t reports = cap.count(sm1::loglevel_t::warn, "unstamped");
  CHECK_MESSAGE(reports >= 1, cap.all());
  CHECK(cap.count(sm1::loglevel_t::warn, "kept 1 version(s)") == reports);

  CHECK(wisr::gc_sweep_txn(s->db, true) == -1);
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::warn, "sweep abandoned") == 1,
                cap.all());
  sm1::closedb(s);
}

TEST_CASE("a snapshot holding back GC too long is reported once") {
  // Verify the stuck-snapshot warning (the threshold is 60s;
  // lowered here to 1s).
  // 1. T begins and stays idle past the threshold.
  // 2. A GC run warns once about T's snapshot; later runs do
  //    not repeat it.
  // 3. After T ends, no further warning appears.
  log_fixture_t f;
  sm1::session_t *t = f.open();
  t->db->stuck_warn_secs = 1;
  log_capture_t cap;
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::repeatable_read) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  wisr::gc_run_once(t->db);
  wisr::gc_run_once(t->db);
  CHECK_MESSAGE(cap.count(sm1::loglevel_t::warn, "held back") == 1, cap.all());
  REQUIRE(sm1::commit(t) == 0);
  wisr::gc_run_once(t->db);
  CHECK(cap.count(sm1::loglevel_t::warn, "held back") == 1);
  sm1::closedb(t);
}

TEST_CASE("passing savepoint_undo_bytes is logged, once for each loss") {
  // Verify an operator learns a transaction outgrew its undo list.
  // 1. Open with room for one write's undo; set a savepoint.
  // 2. One write fits: nothing is logged.
  // 3. The second passes the cap: one warning names the option.
  // 4. Later writes log nothing more.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::options_t opt;
  opt.savepoint_undo_bytes =
      static_cast<long long>(sizeof(sm1::session_t::undo_t) + 1);
  sm1::session_t *s = opendb(home.env(), "db", &opt);
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  log_capture_t log;
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  put(s, store, "a", "1");
  CHECK_MESSAGE(log.count(sm1::loglevel_t::warn, "savepoint_undo_bytes") == 0,
                log.all());

  put(s, store, "b", "1");
  CHECK_MESSAGE(log.count(sm1::loglevel_t::warn, "savepoint_undo_bytes") == 1,
                log.all());

  put(s, store, "c", "1");
  put(s, store, "d", "1");
  CHECK_MESSAGE(log.count(sm1::loglevel_t::warn, "savepoint_undo_bytes") == 1,
                log.all());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}
