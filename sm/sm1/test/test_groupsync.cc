/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_groupsync.cc -- a commit's WAL sync, made by sm1 outside
 * RocksDB's write queue, one commit syncing for all that wait
 * (../DBMGR.md, ../XCTMGR.md §8.2).
 */

#include "doctest.h"
#include "util.h"

#include "sm0_defer.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rocksdb/env.h>
#include <rocksdb/file_system.h>

namespace {

/*
 * A file system that, at a power cut, loses what its files did not
 * sync. From cut() on, every write, sync, delete and rename fails, as
 * the machine is off, so a close cannot make anything durable; then
 * lose_unsynced() truncates every file it made to the size its last
 * sync before the cut saw.
 */
class powercut_fs_t : public rocksdb::FileSystemWrapper {
public:
  /* Over the default file system. */
  powercut_fs_t() : FileSystemWrapper(rocksdb::FileSystem::Default()) {}

  /* The name RocksDB's registry knows it by. */
  const char *Name() const override { return "powercut_fs_t"; }

  /* Make the file, and count its syncs; none after the cut. */
  rocksdb::IOStatus
  NewWritableFile(const std::string &fname, const rocksdb::FileOptions &opts,
                  std::unique_ptr<rocksdb::FSWritableFile> *result,
                  rocksdb::IODebugContext *dbg) override {
    if (m_cut) {
      return off();
    }
    rocksdb::IOStatus s = target()->NewWritableFile(fname, opts, result, dbg);
    if (s.ok()) {
      std::lock_guard<std::mutex> guard(m_mu);
      m_synced[fname] = 0;
      *result = std::make_unique<file_t>(std::move(*result), this, fname);
    }
    return s;
  }

  /* Delete the file; not after the cut. */
  rocksdb::IOStatus DeleteFile(const std::string &fname,
                               const rocksdb::IOOptions &o,
                               rocksdb::IODebugContext *dbg) override {
    return m_cut ? off() : target()->DeleteFile(fname, o, dbg);
  }

  /* Rename the file; not after the cut. */
  rocksdb::IOStatus RenameFile(const std::string &src, const std::string &dst,
                               const rocksdb::IOOptions &o,
                               rocksdb::IODebugContext *dbg) override {
    return m_cut ? off() : target()->RenameFile(src, dst, o, dbg);
  }

  /* The power goes. */
  void cut() { m_cut = true; }

  /* Truncate each file made to what it had synced before the cut. */
  void lose_unsynced() {
    std::lock_guard<std::mutex> guard(m_mu);
    for (const auto &kv : m_synced) {
      // A file deleted before the cut is gone anyway.
      if (::access(kv.first.c_str(), F_OK) == 0) {
        REQUIRE(::truncate(kv.first.c_str(), (off_t)kv.second) == 0);
      }
    }
  }

private:
  /* What every call fails with once the power is cut. */
  static rocksdb::IOStatus off() {
    return rocksdb::IOStatus::IOError("the power is cut");
  }

  /* A file whose syncs record its size; after the cut it writes nothing. */
  class file_t : public rocksdb::FSWritableFileOwnerWrapper {
  public:
    file_t(std::unique_ptr<rocksdb::FSWritableFile> &&t, powercut_fs_t *fs,
           std::string name)
        : FSWritableFileOwnerWrapper(std::move(t)), m_fs(fs),
          m_name(std::move(name)) {}

    /* Append; not after the cut. */
    rocksdb::IOStatus Append(const rocksdb::Slice &data,
                             const rocksdb::IOOptions &o,
                             rocksdb::IODebugContext *dbg) override {
      return m_fs->m_cut ? off() : target()->Append(data, o, dbg);
    }

    /* Append with a checksum; not after the cut. */
    rocksdb::IOStatus Append(const rocksdb::Slice &data,
                             const rocksdb::IOOptions &o,
                             const rocksdb::DataVerificationInfo &info,
                             rocksdb::IODebugContext *dbg) override {
      return m_fs->m_cut ? off() : target()->Append(data, o, info, dbg);
    }

    /* Flush to the OS; not after the cut. */
    rocksdb::IOStatus Flush(const rocksdb::IOOptions &o,
                            rocksdb::IODebugContext *dbg) override {
      return m_fs->m_cut ? off() : target()->Flush(o, dbg);
    }

    /* Sync, and record the size it made durable. */
    rocksdb::IOStatus Sync(const rocksdb::IOOptions &o,
                           rocksdb::IODebugContext *dbg) override {
      if (m_fs->m_cut) {
        return off();
      }
      rocksdb::IOStatus s = target()->Sync(o, dbg);
      record(s, dbg);
      return s;
    }

    /* Fsync, and record the size it made durable. */
    rocksdb::IOStatus Fsync(const rocksdb::IOOptions &o,
                            rocksdb::IODebugContext *dbg) override {
      if (m_fs->m_cut) {
        return off();
      }
      rocksdb::IOStatus s = target()->Fsync(o, dbg);
      record(s, dbg);
      return s;
    }

  private:
    /* Keep the file's size as synced. */
    void record(const rocksdb::IOStatus &s, rocksdb::IODebugContext *dbg) {
      if (s.ok()) {
        uint64_t size = target()->GetFileSize(rocksdb::IOOptions(), dbg);
        std::lock_guard<std::mutex> guard(m_fs->m_mu);
        m_fs->m_synced[m_name] = size;
      }
    }

    powercut_fs_t *m_fs;
    std::string m_name;
  };

  std::mutex m_mu;
  std::map<std::string, uint64_t> m_synced; /* file -> synced size */
  std::atomic<bool> m_cut{false};
};

/* A database "db" with store 1, opened with opt. */
class sync_fixture_t {
public:
  /* Create "db" and store 1; the first session opens "db" with opt. */
  explicit sync_fixture_t(const sm1::options_t *opt = nullptr) {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    m_first = opendb(m_home.env(), "db", opt);
    REQUIRE(m_first);
    REQUIRE(sm1::begin(m_first, sm1::xctlevel_t::repeatable_read) == 0);
    m_store = mkstore(m_first);
    REQUIRE(sm1::commit(m_first) == 0);
  }

  /* The store's storeid. */
  uint64_t store() const { return m_store; }

  /* The session the fixture opened. */
  sm1::session_t *first() const { return m_first; }

  /* Another session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

  /* The shared database object behind the sessions. */
  wisr::db_t *db() const { return m_first->db; }

private:
  home_t m_home;
  sm1::session_t *m_first = nullptr;
  uint64_t m_store = 0;
};

/* Insert key -> "v" into store in one committed transaction. */
int commit_one(sm1::session_t *s, uint64_t store, const std::string &key) {
  if (sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
      sm1::insert(s, store, key, "v") != 0) {
    return -1;
  }
  return sm1::commit(s);
}

/*
 * How many commits wait in the commit queue, not yet in a leader's
 * batch, read under commit_mu.
 */
int sync_line_length(wisr::db_t *db) {
  // Hold commit_mu to walk the queue the commits keep under it.
  std::lock_guard<std::mutex> guard(db->commit_mu);
  int n = 0;
  for (wisr::commit_entry_t *e = db->commit_head; e; e = e->next) {
    n++;
  }
  return n;
}

/* Wait up to 10 seconds for n commits to wait in the commit queue. */
bool wait_for_waiters(wisr::db_t *db, int n) {
  auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < until) {
    if (sync_line_length(db) == n) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

} // namespace

TEST_CASE("a commit survives a power cut once sm1::commit returns") {
  // Verify the commit's WAL sync makes what sm1::commit returned for
  // durable, its version with its record (../DBMGR.md §8.2).
  // 1. Make "db" on a file system that loses unsynced data at a
  //    power cut.
  // 2. Commit "a" in a new store.
  // 3. Cut the power, close the database, and lose what was not
  //    synced.
  // 4. Open "db" again: "a" is there.
  home_t home;
  auto fs = std::make_shared<powercut_fs_t>();
  // Every file of "db" through the power-cut file system.
  home.env()->wal_env = rocksdb::NewCompositeEnv(fs);
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(commit_one(s, store, "a") == 0);

  fs->cut();
  REQUIRE(sm1::closedb(s) == 0);
  // Detach closes the database; lose its unsynced data before the
  // attach, whose recovery opens it again.
  home.detach();
  fs->lose_unsynced();
  home.attach();

  s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "a") == "v");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a commit survives a power cut under manual_wal_flush") {
  // Verify the commit's sync writes RocksDB's WAL buffer out before
  // it syncs: SyncWAL() alone syncs the file without the buffer.
  // 1. Make "db" on a file system that loses unsynced data at a
  //    power cut, opened with manual_wal_flush=true.
  // 2. Commit "a" in a new store.
  // 3. Cut the power, close the database, and lose what was not
  //    synced.
  // 4. Open "db" again: "a" is there.
  home_t home;
  auto fs = std::make_shared<powercut_fs_t>();
  // Every file of "db" through the power-cut file system.
  home.env()->wal_env = rocksdb::NewCompositeEnv(fs);
  sm1::options_t opt;
  opt.rocksdb_options = "manual_wal_flush=true";
  REQUIRE(sm1::mkdb(home.env(), "db", &opt) == 0);
  sm1::session_t *s = opendb(home.env(), "db", &opt);
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = mkstore(s);
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(commit_one(s, store, "a") == 0);

  fs->cut();
  REQUIRE(sm1::closedb(s) == 0);
  // Detach closes the database; lose its unsynced data before the
  // attach, whose recovery opens it again.
  home.detach();
  fs->lose_unsynced();
  home.attach();

  s = opendb(home.env(), "db", &opt);
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "a") == "v");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a commit is visible only once the WAL is synced past it") {
  // Verify a commit publishes after its sync, not before: until then
  // its xid is in flight (../DBMGR.md §8.2).
  // 1. Session A commits "a"; its sync stalls.
  // 2. A's commit has not returned, and a new snapshot of B's does
  //    not see "a".
  // 3. Let the sync go: A's commit returns, and B's next snapshot
  //    sees "a".
  sync_fixture_t f;
  sm1::session_t *a = f.first();
  sm1::session_t *b = f.open();

  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> calls{0};
  f.db()->test_before_sync = [&] {
    if (calls++ == 0) {
      entered.set_value();
      released.wait();
    }
    return rocksdb::Status::OK();
  };
  // Declared after the state the hook uses and before the guards,
  // as locals go in reverse: on every way out the guards let the
  // stalls go, then the futures wait for their commits, and only
  // then does the hook's state go.
  std::future<int> a_rc;
  // Let the stalled sync go on every way out of the test: a failed
  // REQUIRE would otherwise hang in the destructor of an
  // std::async future, which waits for its commit to return.
  sm0::scope_guard_t let_go([&] { release.set_value(); });

  a_rc = std::async(std::launch::async,
                    [&] { return commit_one(a, f.store(), "a"); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  // Time for A's commit to have returned, were it not to wait.
  CHECK(a_rc.wait_for(std::chrono::milliseconds(300)) !=
        std::future_status::ready);

  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "a") == "<none>");
  REQUIRE(sm1::commit(b) == 0);

  let_go.dismiss();
  release.set_value();
  CHECK(a_rc.get() == 0);
  f.db()->test_before_sync = nullptr;
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "a") == "v");
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::closedb(b) == 0);
}

/*
 * The failed-sync child: make a store and write its storeid to
 * stdout, then commit "a" with its WAL sync failing, which aborts
 * the process. Returns only on a setback.
 */
int sync_fail_child_main(const char *homedir) {
  sm1::env_t *env = attach(homedir);
  sm1::session_t *s = env ? opendb(env, "db") : nullptr;
  uint64_t store = 0;
  if (!s || sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
      sm1::mkstore(s, store) != 0 || sm1::commit(s) != 0) {
    return 2;
  }
  if (write(1, &store, sizeof(store)) != (ssize_t)sizeof(store)) {
    return 3;
  }
  s->db->test_before_sync = [] { return rocksdb::Status::IOError("injected"); };
  commit_one(s, store, "a");
  return 4;
}

TEST_CASE("a failed sync aborts the process") {
  // Verify a failed WAL sync ends the process, so no commit it was
  // to cover returns, and the reopen decides them from the WAL
  // (../XCTMGR.md §8.2).
  // 1. Make "db", and leave the home directory to a child.
  // 2. The child commits "a", and its sync fails: it dies of
  //    SIGABRT, having said *** FATAL *** on stderr.
  // 3. Attach again: "a" is committed, as its record reached the
  //    OS before the abort, though the sync failed.
  home_t home;
  REQUIRE(sm1::mkdb(home.env(), "db", nullptr) == 0);
  home.detach();
  std::string out, err;
  int status = run_child("--sync-fail-child", home.path(), out, err);
  CAPTURE(err);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
  CHECK(err.find("*** FATAL *** database db: WAL sync failed") !=
        std::string::npos);
  uint64_t store = 0;
  REQUIRE(out.size() == sizeof(store));
  memcpy(&store, out.data(), sizeof(store));

  home.attach();
  sm1::session_t *s = opendb(home.env(), "db");
  REQUIRE(s);
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(s, store, "a") == "v");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("one sync serves the commits that wait while another runs") {
  // Verify the commits waiting behind a sync share the next one, as
  // PostgreSQL's XLogFlush() does (../XCTMGR.md §8.2).
  // 1. Session A commits "a"; its sync stalls.
  // 2. Fifteen sessions commit meanwhile: each joins the commit
  //    queue and waits for A's sync.
  // 3. Let A's sync go: the fifteen share a second batch and sync,
  //    the oldest of them leading it.
  sync_fixture_t f;
  sm1::session_t *a = f.first();
  std::vector<sm1::session_t *> others;
  for (int i = 0; i < 15; i++) {
    others.push_back(f.open());
  }

  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> calls{0};
  f.db()->test_before_sync = [&] {
    if (calls++ == 0) {
      entered.set_value();
      released.wait();
    }
    return rocksdb::Status::OK();
  };
  // Declared after the state the hook uses and before the guards,
  // as locals go in reverse: on every way out the guards let the
  // stalls go, then the futures wait for their commits, and only
  // then does the hook's state go.
  std::future<int> a_rc;
  std::vector<std::future<int>> rcs;
  // Let the stalled sync go on every way out of the test: a failed
  // REQUIRE would otherwise hang in the destructor of an
  // std::async future, which waits for its commit to return.
  sm0::scope_guard_t let_go([&] { release.set_value(); });

  a_rc = std::async(std::launch::async,
                    [&] { return commit_one(a, f.store(), "a"); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  for (size_t i = 0; i < others.size(); i++) {
    rcs.push_back(std::async(std::launch::async, [&, i] {
      return commit_one(others[i], f.store(), "k" + std::to_string(i));
    }));
  }
  // Every one is in the queue once it waits for the sync.
  REQUIRE(wait_for_waiters(f.db(), (int)others.size()));

  let_go.dismiss();
  release.set_value();
  CHECK(a_rc.get() == 0);
  for (std::future<int> &rc : rcs) {
    CHECK(rc.get() == 0);
  }
  CHECK(calls == 2);
  f.db()->test_before_sync = nullptr;
  for (sm1::session_t *s : others) {
    REQUIRE(sm1::closedb(s) == 0);
  }
}

TEST_CASE("the next sync goes to the oldest commit waiting") {
  // Verify the leader that ends a sync hands the next batch to the
  // oldest commit in the queue, and returns no other before that
  // batch's sync (../XCTMGR.md §8.2).
  // 1. Session A commits "a"; its sync stalls.
  // 2. Five sessions commit in turn, each in the queue before the
  //    next starts.
  // 3. Let A's sync go; the second sync stalls.
  // 4. The first of the five leads it, its batch all five: the queue
  //    is empty, and none of the five has returned.
  // 5. Let it go: every commit returns, after two syncs in all.
  sync_fixture_t f;
  sm1::session_t *a = f.first();
  std::vector<sm1::session_t *> others;
  for (int i = 0; i < 5; i++) {
    others.push_back(f.open());
  }

  std::promise<void> entered1, entered2, release1, release2;
  std::shared_future<void> released1 = release1.get_future().share();
  std::shared_future<void> released2 = release2.get_future().share();
  std::atomic<int> calls{0};
  // Which of the five leads the second sync: its index, or -1.
  std::atomic<int> second_leader{-1};
  // Each commit's thread knows its index; A's is -1.
  static thread_local int t_index = -1;
  f.db()->test_before_sync = [&] {
    int n = calls++;
    if (n == 0) {
      entered1.set_value();
      released1.wait();
    } else if (n == 1) {
      second_leader = t_index;
      entered2.set_value();
      released2.wait();
    }
    return rocksdb::Status::OK();
  };
  // Declared after the state the hook uses and before the guards,
  // as locals go in reverse: on every way out the guards let the
  // stalls go, then the futures wait for their commits, and only
  // then does the hook's state go.
  std::future<int> a_rc;
  std::vector<std::future<int>> rcs;
  // Let each stalled sync go on every way out of the test: a failed
  // REQUIRE would otherwise hang in the destructor of an
  // std::async future, which waits for its commit to return.
  sm0::scope_guard_t let_go1([&] { release1.set_value(); });
  sm0::scope_guard_t let_go2([&] { release2.set_value(); });

  a_rc = std::async(std::launch::async,
                    [&] { return commit_one(a, f.store(), "a"); });
  REQUIRE(entered1.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  for (size_t i = 0; i < others.size(); i++) {
    rcs.push_back(std::async(std::launch::async, [&, i] {
      t_index = (int)i;
      return commit_one(others[i], f.store(), "k" + std::to_string(i));
    }));
    // In the queue before the next one starts, so it is in order.
    REQUIRE(wait_for_waiters(f.db(), (int)i + 1));
  }

  let_go1.dismiss();
  release1.set_value();
  REQUIRE(entered2.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  CHECK(a_rc.get() == 0);
  CHECK(second_leader == 0);
  CHECK(sync_line_length(f.db()) == 0);
  for (std::future<int> &rc : rcs) {
    CHECK(rc.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready);
  }

  let_go2.dismiss();
  release2.set_value();
  for (std::future<int> &rc : rcs) {
    CHECK(rc.get() == 0);
  }
  CHECK(calls == 2);
  f.db()->test_before_sync = nullptr;
  for (sm1::session_t *s : others) {
    REQUIRE(sm1::closedb(s) == 0);
  }
}

TEST_CASE("a commit without durability_sync does not sync") {
  // Verify durability_sync = 0 skips the commit's sync (../DBMGR.md §10).
  // 1. Open "db" with durability_sync = 0.
  // 2. Commit "a": no sync is made, and "a" is visible.
  sm1::options_t opt;
  opt.durability_sync = 0;
  sync_fixture_t f(&opt);
  sm1::session_t *a = f.first();
  std::atomic<int> calls{0};
  f.db()->test_before_sync = [&] {
    calls++;
    return rocksdb::Status::OK();
  };
  REQUIRE(commit_one(a, f.store(), "a") == 0);
  CHECK(calls == 0);
  f.db()->test_before_sync = nullptr;
  REQUIRE(sm1::begin(a, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(a, f.store(), "a") == "v");
  REQUIRE(sm1::commit(a) == 0);
}
