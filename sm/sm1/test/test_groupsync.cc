/*
 * test_groupsync.cc -- a commit's WAL sync, made by sm1 outside
 * RocksDB's write queue, one commit syncing for all that wait
 * (GROUPSYNC.md, SM1_DESIGN.md §8.2).
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "env.h"
#include "session.h"
#include "sm1.h"

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

} // namespace

TEST_CASE("a commit survives a power cut once sm1::commit returns") {
  // Verify the commit's WAL sync makes what sm1::commit returned for
  // durable, its version with its record (GROUPSYNC.md §3).
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
  // its xid is in flight (GROUPSYNC.md §3).
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

  std::future<int> a_rc = std::async(
      std::launch::async, [&] { return commit_one(a, f.store(), "a"); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  // Time for A's commit to have returned, were it not to wait.
  CHECK(a_rc.wait_for(std::chrono::milliseconds(300)) !=
        std::future_status::ready);

  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "a") == "<none>");
  REQUIRE(sm1::commit(b) == 0);

  release.set_value();
  CHECK(a_rc.get() == 0);
  f.db()->test_before_sync = nullptr;
  REQUIRE(sm1::begin(b, sm1::xctlevel_t::repeatable_read) == 0);
  CHECK(get(b, f.store(), "a") == "v");
  REQUIRE(sm1::commit(b) == 0);
  REQUIRE(sm1::closedb(b) == 0);
}

TEST_CASE("a failed sync fails the database stop, with the commits behind "
          "it") {
  // Verify a failed WAL sync is a failed status write (§8.2): the
  // outcome of every commit it was to cover is unknown.
  // 1. Session A commits "a"; its sync stalls.
  // 2. Session B commits "b" meanwhile: its record is written, and it
  //    waits for a sync.
  // 3. A's sync fails: both commits return SM_ERR_IO, and the
  //    database has failed stop.
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
      return rocksdb::Status::IOError("injected");
    }
    return rocksdb::Status::OK();
  };

  // A commit's return code and error, read on its own thread.
  auto commit_err = [&](sm1::session_t *s, const char *key) {
    int rc = commit_one(s, f.store(), key);
    return rc == 0 ? 0 : sm1::errnum();
  };
  std::future<int> a_err =
      std::async(std::launch::async, [&] { return commit_err(a, "a"); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  std::future<int> b_err =
      std::async(std::launch::async, [&] { return commit_err(b, "b"); });
  // Time for B to write its record and wait for A's sync.
  CHECK(b_err.wait_for(std::chrono::milliseconds(300)) !=
        std::future_status::ready);

  release.set_value();
  CHECK(a_err.get() == sm1::SM_ERR_IO);
  CHECK(b_err.get() == sm1::SM_ERR_IO);
  CHECK(calls == 1);
  CHECK(f.db()->failed.load());
  f.db()->test_before_sync = nullptr;
  REQUIRE(sm1::closedb(b) == 0);
}

TEST_CASE("one sync serves the commits that wait while another runs") {
  // Verify the commits waiting behind a sync share the next one, as
  // PostgreSQL's XLogFlush() does (GROUPSYNC.md §3).
  // 1. Session A commits "a"; its sync stalls.
  // 2. Fifteen sessions commit meanwhile, one at a time, so each
  //    record has a sequence number of its own; each waits.
  // 3. Let A's sync go: the fifteen share a second sync, whichever
  //    of them leads it.
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

  std::future<int> a_rc = std::async(
      std::launch::async, [&] { return commit_one(a, f.store(), "a"); });
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) ==
          std::future_status::ready);
  std::vector<std::future<int>> rcs;
  for (size_t i = 0; i < others.size(); i++) {
    rcs.push_back(std::async(std::launch::async, [&, i] {
      return commit_one(others[i], f.store(), "k" + std::to_string(i));
    }));
    // Time for its record to be written alone.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  // Time for the last to write its record and wait.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

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

TEST_CASE("a commit without durability_sync does not sync") {
  // Verify durability_sync = 0 skips the commit's sync (§12.2).
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
