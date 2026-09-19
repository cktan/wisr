/*
 * test_log.cc -- the process-wide log hook (sm_set_log) and the
 * events wisr reports through it: what no caller would
 * otherwise learn.
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"
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
  log_capture_t() { sm_set_log(hook, this); }

  /* Remove it. */
  ~log_capture_t() { sm_set_log(nullptr, nullptr); }

  /* How many messages at level contain needle. */
  size_t count(sm_loglevel_t level, const std::string &needle) {
    std::lock_guard<std::mutex> guard(m_mu);
    size_t n = 0;
    for (auto &m : m_msgs) {
      n += m.first == level && m.second.find(needle) != std::string::npos;
    }
    return n;
  }

  /* Every message so far, for failure output. */
  std::string all() {
    std::lock_guard<std::mutex> guard(m_mu);
    std::string out;
    for (auto &m : m_msgs) {
      out += std::to_string(m.first) + ": " + m.second + "\n";
    }
    return out;
  }

private:
  /* The hook: record the message under the capture's mutex. */
  static void hook(void *arg, sm_loglevel_t level, const char *msg) {
    log_capture_t *self = static_cast<log_capture_t *>(arg);
    std::lock_guard<std::mutex> guard(self->m_mu);
    self->m_msgs.emplace_back(level, msg);
  }

  std::mutex m_mu;
  std::vector<std::pair<sm_loglevel_t, std::string>> m_msgs;
};

/* A database "db" with relation "t", reopenable. */
class log_fixture_t {
public:
  /* Create "db" and "t". */
  log_fixture_t() {
    REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
    sm_session_t *s = open();
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "t") == 0);
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_closedb(s) == 0);
  }

  /* A new session on "db". */
  sm_session_t *open() const {
    sm_session_t *s = sm_opendb(m_home.env(), "db", nullptr);
    REQUIRE_MESSAGE(s, sm_errmsg());
    return s;
  }

  /* The home directory. */
  home_t &home() { return m_home; }

private:
  home_t m_home;
};

/* Insert key -> "v" into "t" in one committed transaction. */
void commit_put(sm_session_t *s, const std::string &key) {
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, "t", key, "v");
  REQUIRE(sm_commit(s) == 0);
}

} // namespace

TEST_CASE("with no hook nothing is reported, and a hook can be replaced") {
  // Verify installing, replacing, and removing the hook.
  // 1. Capture A sees the attach sweep's message.
  // 2. Capture B replaces A: B sees the next one, A does not.
  // 3. After B is removed, a third event reaches neither.
  tmpdir_t tmp;
  REQUIRE(sm_init(tmp.path()) == 0);
  std::string base = std::string(tmp.path()) + "/base";
  auto sweep_once = [&](const char *dbid) {
    REQUIRE(mkdir((base + "/" + dbid).c_str(), 0700) == 0);
    sm_env_t *env = sm_attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm_detach(env, 0) == 0);
  };
  size_t a_before;
  {
    log_capture_t a;
    sweep_once("7");
    CHECK(a.count(SM_LOG_INFO, "/base/7") == 1);
    {
      log_capture_t b;
      sweep_once("8");
      CHECK(b.count(SM_LOG_INFO, "/base/8") == 1);
      CHECK(a.count(SM_LOG_INFO, "/base/8") == 0);
    }
    a_before = a.count(SM_LOG_INFO, "destroyed");
    sweep_once("9"); // no hook: b removed it
    CHECK(a.count(SM_LOG_INFO, "destroyed") == a_before);
  }
}

TEST_CASE("recovery reports restored transactions and dropped orphans") {
  // Verify §9.1's recovery events reach the hook.
  // 1. Prepare "g-log"; create a relation CF no catalog row
  //    names, as a crash inside sm_mkrel would leave.
  // 2. Restart: an INFO message names "g-log" as restored, and
  //    one reports the orphaned CF dropped.
  log_fixture_t f;
  sm_session_t *s = f.open();
  uint64_t orphan = 0;
  REQUIRE(wisr::db_alloc_cfid(s->db, &orphan) == 0);
  REQUIRE(wisr::db_create_rel_cf(s->db, orphan) == 0);
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, "t", "k", "v");
  REQUIRE(sm_prepare(s, "g-log") == 0);

  log_capture_t cap;
  f.home().detach();
  f.home().attach();
  sm_session_t *v = f.open();
  CHECK_MESSAGE(cap.count(SM_LOG_INFO, "g-log") == 1, cap.all());
  CHECK_MESSAGE(cap.count(SM_LOG_INFO, "relation " + std::to_string(orphan)) ==
                    1,
                cap.all());
  REQUIRE(sm_abort_prepared(v, "g-log") == 0);
  sm_closedb(v);
}

TEST_CASE("a failed status write is reported as the database stopping") {
  // Verify fail-stop is logged, not only returned to one caller.
  // 1. Make the next commit's status write fail.
  // 2. An ERROR message says the database stopped.
  log_fixture_t f;
  sm_session_t *s = f.open();
  log_capture_t cap;
  s->db->test_before_commit = [](uint64_t) {
    return rocksdb::Status::IOError("injected");
  };
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  put(s, "t", "k", "v");
  CHECK(sm_commit(s) == -1);
  s->db->test_before_commit = nullptr;
  CHECK_MESSAGE(cap.count(SM_LOG_ERROR, "stopped") == 1, cap.all());
  sm_closedb(s);
}

TEST_CASE("a CF:txn sweep reports what it deleted") {
  // Verify a completed background sweep is logged.
  // 1. Commit a few transactions, then sweep.
  // 2. An INFO message reports the records deleted.
  log_fixture_t f;
  sm_session_t *s = f.open();
  for (int i = 0; i < 3; i++) {
    commit_put(s, "k" + std::to_string(i));
  }
  log_capture_t cap;
  CHECK(wisr::gc_sweep_txn(s->db, true) > 0);
  CHECK_MESSAGE(cap.count(SM_LOG_INFO, "sweep deleted") == 1, cap.all());
  sm_closedb(s);
}

TEST_CASE("versions the filter cannot judge are reported per compaction") {
  // Verify the filter's doubts, and the sweep refusing to trust them.
  // 1. Plant a version with an undecodable (empty) value in "t".
  // 2. Compact: each compaction job that meets it reports it
  //    once, as one version -- a summary, not a line per version.
  // 3. A sweep's pass meets it again and is abandoned, with a
  //    WARN saying the status records are kept.
  log_fixture_t f;
  sm_session_t *s = f.open();
  REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  uint64_t relid = 0;
  REQUIRE(wisr::catalog_lookup(s, wisr::xct_view(s), "t", &relid) == 0);
  REQUIRE(sm_commit(s) == 0);
  rocksdb::ColumnFamilyHandle *cf = wisr::db_rel_cf(s->db, relid);
  REQUIRE(s->db->rdb
              ->Put(rocksdb::WriteOptions(), cf, wisr::version_key("bad", 1, 1),
                    rocksdb::Slice())
              .ok());

  log_capture_t cap;
  rocksdb::CompactRangeOptions opt;
  opt.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  REQUIRE(s->db->rdb->CompactRange(opt, cf, nullptr, nullptr).ok());
  size_t reports = cap.count(SM_LOG_WARN, "unstamped");
  CHECK_MESSAGE(reports >= 1, cap.all());
  CHECK(cap.count(SM_LOG_WARN, "kept 1 version(s)") == reports);

  CHECK(wisr::gc_sweep_txn(s->db, true) == -1);
  CHECK_MESSAGE(cap.count(SM_LOG_WARN, "sweep abandoned") == 1, cap.all());
  sm_closedb(s);
}

TEST_CASE("a snapshot holding back GC too long is reported once") {
  // Verify the stuck-snapshot warning (the threshold is 60s;
  // lowered here to 1s).
  // 1. T begins and stays idle past the threshold.
  // 2. A GC run warns once about T's snapshot; later runs do
  //    not repeat it.
  // 3. After T ends, no further warning appears.
  log_fixture_t f;
  sm_session_t *t = f.open();
  t->db->stuck_warn_secs = 1;
  log_capture_t cap;
  REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  wisr::gc_run_once(t->db);
  wisr::gc_run_once(t->db);
  CHECK_MESSAGE(cap.count(SM_LOG_WARN, "held back") == 1, cap.all());
  REQUIRE(sm_commit(t) == 0);
  wisr::gc_run_once(t->db);
  CHECK(cap.count(SM_LOG_WARN, "held back") == 1);
  sm_closedb(t);
}
