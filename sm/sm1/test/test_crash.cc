/*
 * test_crash.cc -- kill a committing process with SIGKILL and
 * check that recovery leaves exactly a prefix of its commits,
 * including every commit it acknowledged (SM1_DESIGN.md §9, §12.5).
 */

#include "doctest.h"
#include "util.h"

#include "env.h"
#include "sm1.h"
#include "walfs.h"

#include <rocksdb/db.h>
#include <rocksdb/file_system.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace wisr;

namespace {

/* The key of the i-th commit; fixed width, so memcmp order is i order. */
std::string crash_key(uint32_t i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "k%08u", i);
  return buf;
}

/*
 * The database's one store, as s's transaction sees it: the
 * lowest storeid listed, which is the first store created.
 * Returns 0 if there is none.
 */
uint64_t first_store(sm1::session_t *s) {
  std::vector<uint64_t> list;
  if (sm1::lsstore(s, list) != 0 || list.empty()) {
    return 0;
  }
  return list[0];
}

/* The number of rows in store, as s reads them. */
uint32_t count_rows(sm1::session_t *s, uint64_t store) {
  sm1::scan_t *c = scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  if (!c) {
    return UINT32_MAX;
  }
  uint32_t n = 0;
  std::string_view k, d;
  while (sm1::scannxt(c, k, d) == 0) {
    n++;
  }
  sm1::scanend(c);
  return n;
}

/* The write-ahead logs, NNNNNN.log, under dir, by path. */
std::vector<std::filesystem::path> wal_files(const std::string &dir) {
  std::vector<std::filesystem::path> out;
  for (const auto &e : std::filesystem::recursive_directory_iterator(dir)) {
    std::string name = e.path().filename().string();
    if (e.is_regular_file() && name.size() > 4 &&
        name.compare(name.size() - 4, 4, ".log") == 0 &&
        name.find_first_not_of("0123456789") == name.size() - 4) {
      out.push_back(e.path());
    }
  }
  return out;
}

/* The size of the largest log under dir. */
uintmax_t largest_wal(const std::string &dir) {
  uintmax_t most = 0;
  for (const auto &p : wal_files(dir)) {
    most = std::max(most, std::filesystem::file_size(p));
  }
  return most;
}

/* Read exactly n bytes from fd; false at EOF or on error. */
bool read_full(int fd, void *buf, size_t n) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) {
      return false;
    }
    p += r;
    n -= r;
  }
  return true;
}

} // namespace

/*
 * The child: append crash_key(n), crash_key(n + 1), ... to "t",
 * one per transaction, and after each commit returns write its
 * index to stdout. Runs until killed.
 */
int crash_child_main(const char *homedir) {
  sm1::env_t *env = attach(homedir);
  if (!env) {
    return 2;
  }
  sm1::session_t *s = opendb(env, "db", nullptr);
  if (!s || sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
    return 3;
  }
  // Find the store, then leave the transaction that read it.
  uint64_t store = first_store(s);
  uint32_t next = store ? count_rows(s, store) : UINT32_MAX;
  if (next == UINT32_MAX || sm1::commit(s) != 0) {
    return 4;
  }
  for (uint32_t i = next;; i++) {
    std::string k = crash_key(i);
    if (sm1::begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
        sm1::insert(s, store, k, "v") != 0 || sm1::commit(s) != 0) {
      return 5;
    }
    if (write(1, &i, sizeof(i)) != sizeof(i)) {
      return 6;
    }
  }
}

TEST_CASE("a killed process leaves a prefix of its commits, acknowledged ones "
          "included") {
  // Verify crash recovery against SIGKILL at arbitrary points.
  // 1. Create database "db" with store 1.
  // 2. Each round, start a child that commits one key per
  //    transaction and reports each acknowledged commit.
  // 3. After a random number of acknowledgements, SIGKILL it
  //    and collect every acknowledgement it sent.
  // 4. Reattach: the keys present must be exactly k0..k(n-1)
  //    for some n covering every acknowledged commit.
  // 5. Repeat, each child continuing where recovery left off.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  {
    sm1::env_t *env = attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
    sm1::session_t *s = opendb(env, "db", nullptr);
    REQUIRE(s);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    mkstore(s);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::detach(env, 1) == 0);
  }

  std::mt19937 rng(20260918);
  uint32_t acked = 0; // commits acknowledged across all rounds
  for (int round = 0; round < 5; round++) {
    CAPTURE(round);
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      // In the child: stdout becomes the pipe, then exec.
      dup2(fds[1], 1);
      close(fds[0]);
      close(fds[1]);
      execl("/proc/self/exe", "test_sm1", "--crash-child", tmp.path(),
            (char *)nullptr);
      _exit(127);
    }
    close(fds[1]);

    uint32_t target = acked + 20 + rng() % 300;
    uint32_t i;
    while (acked < target && read_full(fds[0], &i, sizeof(i))) {
      CHECK(i == acked);
      acked = i + 1;
    }
    REQUIRE_MESSAGE(acked >= target, "child stopped early");
    // Kill the child mid-workload, then collect what it acked.
    kill(pid, SIGKILL);
    int status;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    while (read_full(fds[0], &i, sizeof(i))) {
      acked = i + 1;
    }
    close(fds[0]);

    sm1::env_t *env = attach(tmp.path());
    REQUIRE_MESSAGE(env, sm1::errmsg());
    sm1::session_t *s = opendb(env, "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    uint64_t store = first_store(s);
    REQUIRE(store != 0);
    uint32_t n = count_rows(s, store);
    CHECK(n >= acked);
    CHECK(n <= acked + 1); // at most the one in flight at the kill
    for (uint32_t j = 0; j < n; j++) {
      std::string k = crash_key(j);
      CHECK(get(s, store, k) == "v");
    }
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::detach(env, 1) == 0);
    acked = n;
  }
}

namespace {

/* The home's decision, as its root db keeps it; UINT64_MAX if none. */
uint64_t prealloc_kept(sm1::env_t *env) {
  std::string v;
  if (!env->root->Get(rocksdb::ReadOptions(), WISR_ROOT_WAL_PREALLOC, &v)
           .ok() ||
      v.size() != 8) {
    return UINT64_MAX;
  }
  uint64_t n = 0;
  for (char c : v) {
    n = n << 8 | static_cast<uint8_t>(c);
  }
  return n;
}

/* Keep bytes as the home's decision, as a first attach would. */
void prealloc_keep(sm1::env_t *env, uint64_t bytes) {
  std::string v(8, '\0');
  for (int k = 7; k >= 0; k--, bytes >>= 8) {
    v[k] = static_cast<char>(bytes & 0xff);
  }
  REQUIRE(
      env->root->Put(rocksdb::WriteOptions(), WISR_ROOT_WAL_PREALLOC, v).ok());
}

} // namespace

TEST_CASE("a home's first attach decides whether new logs are zero-filled") {
  // Verify the decision of WALPREALLOC.md: made once, kept, obeyed.
  // 1. A new home's first attach times its syncs and keeps the
  //    answer, 0 or 64 MB; a second attach keeps it unchanged.
  // 2. With 4 MB kept, a database made after the next attach has its
  //    log zero-filled to 4 MB; with 0 kept, its log is its records.
  for (uint64_t kept : {uint64_t(4) << 20, uint64_t(0)}) {
    CAPTURE(kept);
    tmpdir_t tmp;
    REQUIRE(sm1::init(tmp.path()) == 0);
    sm1::env_t *env = attach(tmp.path());
    REQUIRE(env);
    uint64_t first = prealloc_kept(env);
    CHECK((first == 0 || first == wal_prealloc_bytes));
    REQUIRE(sm1::detach(env, 1) == 0);
    env = attach(tmp.path());
    REQUIRE(env);
    CHECK(prealloc_kept(env) == first);
    prealloc_keep(env, kept);
    REQUIRE(sm1::detach(env, 1) == 0);

    env = attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
    sm1::session_t *s = opendb(env, "db", nullptr);
    REQUIRE(s);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    uint64_t store = mkstore(s);
    REQUIRE(sm1::insert(s, store, "k", "v") == 0);
    REQUIRE(sm1::commit(s) == 0);
    uintmax_t size = largest_wal(tmp.path());
    if (kept > 0) {
      CHECK(size == kept);
    } else {
      CHECK(size > 0);
      CHECK(size < 1 << 20);
    }
    REQUIRE(sm1::detach(env, 1) == 0);
  }
}

TEST_CASE("zero-filling pays where a sync that grows a file costs more") {
  // Verify the probe and its rule (WALPREALLOC.md).
  // 1. The probe times both syncs in a directory, and leaves no file.
  // 2. A sync half again as dear after an append zero-fills; one
  //    about as dear, or none timed at all, does not.
  tmpdir_t tmp;
  double append_us = 0, overwrite_us = 0;
  REQUIRE(wal_probe(tmp.path(), &append_us, &overwrite_us) == 0);
  CHECK(append_us > 0);
  CHECK(overwrite_us > 0);
  CHECK(std::filesystem::is_empty(tmp.path()));
  CHECK(wal_prealloc_pays(2776, 545));
  CHECK(wal_prealloc_pays(150, 100));
  CHECK(!wal_prealloc_pays(140, 100));
  CHECK(!wal_prealloc_pays(0, 0));
}

TEST_CASE(
    "a new log takes the spare a thread zeroed, and the next spare starts") {
  // Verify walfs's spare (WALPREALLOC.md section 2): each log after a
  // directory's first starts a thread that zeroes the next log ahead,
  // the next log takes it, and the Env's end removes what is left.
  // 1. The first log is zero-filled in place, and starts no spare.
  // 2. The second log starts a spare, zeroed to the size.
  // 3. The third log takes the spare, and a new spare starts.
  // 4. With the Env gone, the logs stay and the spare does not.
  tmpdir_t tmp;
  const uint64_t bytes = 4096;
  std::string spare = std::string(tmp.path()) + "/" + wal_spare_name;
  auto size_of = [](const std::string &path) -> int64_t {
    struct stat st;
    return 0 == stat(path.c_str(), &st) ? st.st_size : -1;
  };
  auto spare_ready = [&] {
    for (int i = 0; i < 10000 && size_of(spare) != int64_t(bytes); i++) {
      usleep(1000);
    }
    return size_of(spare) == int64_t(bytes);
  };
  std::unique_ptr<rocksdb::Env> env = walfs_env(bytes);
  auto make_log = [&](const char *name) {
    std::unique_ptr<rocksdb::FSWritableFile> f;
    std::string path = std::string(tmp.path()) + "/" + name;
    REQUIRE(env->GetFileSystem()
                ->NewWritableFile(path, rocksdb::FileOptions(), &f, nullptr)
                .ok());
    REQUIRE(f->Close(rocksdb::IOOptions(), nullptr).ok());
    CHECK(size_of(path) == int64_t(bytes));
  };
  make_log("000001.log");
  usleep(20000);
  CHECK(size_of(spare) == -1);

  // 2.
  make_log("000002.log");
  CHECK(spare_ready());

  // 3.
  make_log("000003.log");
  CHECK(spare_ready());

  // 4.
  env.reset();
  CHECK(size_of(spare) == -1);
  for (const char *name : {"000001.log", "000002.log", "000003.log"}) {
    CHECK(size_of(std::string(tmp.path()) + "/" + name) == int64_t(bytes));
  }
}

TEST_CASE("only WSL's kernels zero-fill new logs") {
  // Verify the first gate of a home's decision (WALPREALLOC.md §2).
  // 1. WSL's two kernels' releases are known as WSL's.
  // 2. A cloud's kernel, and a desktop's, are not.
  // 3. Off WSL, a new home keeps 0 without timing a sync.
  CHECK(wal_release_is_wsl("5.15.167.4-microsoft-standard-WSL2"));
  CHECK(wal_release_is_wsl("4.4.0-19041-Microsoft"));
  CHECK(!wal_release_is_wsl("7.3.0-1003-gcp"));
  CHECK(!wal_release_is_wsl("6.8.0-45-generic"));
  if (!wal_on_wsl()) {
    tmpdir_t tmp;
    REQUIRE(sm1::init(tmp.path()) == 0);
    sm1::env_t *env = attach(tmp.path());
    REQUIRE(env);
    CHECK(prealloc_kept(env) == 0);
    REQUIRE(sm1::detach(env, 1) == 0);
  }
}

TEST_CASE("a new log is the spare a background thread zeroed") {
  // Verify the next log made ahead (WALPREALLOC.md §2).
  // 1. A directory's first log is zeroed in place, and makes no spare:
  //    a database that never switches logs pays for none.
  // 2. Its second log starts a spare, zeroed beside it.
  // 3. Its third log is that spare, the same inode, its size, all
  //    zeros, and written from offset 0; another spare follows.
  // 4. With the spare deleted, a new log is still made, its size.
  // 5. Closing the file system removes the spare.
  const uint64_t bytes = 1 << 20;
  tmpdir_t tmp;
  const std::string dir(tmp.path());
  const std::string spare = dir + "/" + wal_spare_name;
  {
    std::unique_ptr<rocksdb::Env> env = walfs_env(bytes);
    std::shared_ptr<rocksdb::FileSystem> fs = env->GetFileSystem();
    rocksdb::FileOptions fo;
    rocksdb::IOOptions io;
    std::unique_ptr<rocksdb::FSWritableFile> f;
    auto make_log = [&](const char *name) {
      std::string path = dir + "/" + name;
      REQUIRE(fs->NewWritableFile(path, fo, &f, nullptr).ok());
      return path;
    };
    auto wait_spare = [&]() {
      for (int i = 0; i < 5000; i++) {
        if (std::filesystem::exists(spare)) {
          return;
        }
        usleep(1000);
      }
      FAIL("no spare was made");
    };

    CHECK(std::filesystem::file_size(make_log("000001.log")) == bytes);
    usleep(50000);
    CHECK(!std::filesystem::exists(spare));

    make_log("000002.log");
    wait_spare();
    struct stat before;
    REQUIRE(stat(spare.c_str(), &before) == 0);

    std::string third = make_log("000003.log");
    struct stat after;
    REQUIRE(stat(third.c_str(), &after) == 0);
    CHECK(after.st_ino == before.st_ino);
    CHECK(static_cast<uint64_t>(after.st_size) == bytes);
    REQUIRE(f->Append("record", io, nullptr).ok());
    REQUIRE(f->Sync(io, nullptr).ok());
    REQUIRE(f->Close(io, nullptr).ok());
    {
      std::ifstream in(third, std::ios::binary);
      std::string got((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
      REQUIRE(got.size() == bytes);
      CHECK(got.compare(0, 6, "record") == 0);
      CHECK(got.find_first_not_of('\0', 6) == std::string::npos);
    }
    wait_spare();

    REQUIRE(std::filesystem::remove(spare));
    CHECK(std::filesystem::file_size(make_log("000004.log")) == bytes);
    wait_spare();
  }
  CHECK(!std::filesystem::exists(spare));
}

TEST_CASE("logs made from spares replay after a crash") {
  // Verify recovery reads a log that was a spare as any other.
  // 1. Open a RocksDB instance through the file system, writing and
  //    syncing keys across five logs, each ended by a flush.
  // 2. Copy its directory while it is open, as a crash would leave it.
  // 3. Open the copy: every synced key is there, in the newest log's
  //    too, and the spare it inherited does no harm.
  tmpdir_t tmp;
  const std::string dir = std::string(tmp.path()) + "/db";
  const std::string copy = std::string(tmp.path()) + "/copy";
  std::unique_ptr<rocksdb::Env> env = walfs_env(1 << 20);
  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.env = env.get();
  // Through a raw pointer, the one form RocksDB 8.9 and 9 share.
  rocksdb::DB *raw = nullptr;
  REQUIRE(rocksdb::DB::Open(opts, dir, &raw).ok());
  std::unique_ptr<rocksdb::DB> db(raw);
  rocksdb::WriteOptions wo;
  wo.sync = true;
  for (int log = 0; log < 5; log++) {
    for (int k = 0; k < 10; k++) {
      std::string key = std::to_string(log) + "/" + std::to_string(k);
      REQUIRE(db->Put(wo, key, "v").ok());
    }
    if (log < 4) {
      REQUIRE(db->Flush(rocksdb::FlushOptions()).ok());
    }
  }
  std::filesystem::copy(dir, copy);
  db.reset();

  rocksdb::Options plain;
  raw = nullptr;
  REQUIRE(rocksdb::DB::Open(plain, copy, &raw).ok());
  std::unique_ptr<rocksdb::DB> again(raw);
  for (int log = 0; log < 5; log++) {
    for (int k = 0; k < 10; k++) {
      std::string key = std::to_string(log) + "/" + std::to_string(k);
      std::string v;
      CHECK(again->Get(rocksdb::ReadOptions(), key, &v).ok());
    }
  }
}

TEST_CASE("a record torn in a zero-filled log's tail is the end of the log") {
  // Verify recovery past a torn record in the zeros (WALPREALLOC.md).
  // 1. Create database "db" with a store; a child commits keys
  //    until killed, as the test above, its log zero-filled to 4 MB,
  //    kept by hand as a home off WSL keeps 0.
  // 2. Write a torn record after the log's last: a header that
  //    claims 100 bytes, a bad checksum, and 20 bytes of it.
  // 3. Reattach: the database opens, and its keys are k0..k(n-1)
  //    with every acknowledged commit among them.
  tmpdir_t tmp;
  REQUIRE(sm1::init(tmp.path()) == 0);
  {
    sm1::env_t *env = attach(tmp.path());
    REQUIRE(env);
    prealloc_keep(env, uint64_t(4) << 20);
    REQUIRE(sm1::detach(env, 1) == 0);
    env = attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm1::mkdb(env, "db", nullptr) == 0);
    sm1::session_t *s = opendb(env, "db", nullptr);
    REQUIRE(s);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    mkstore(s);
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::detach(env, 1) == 0);
  }

  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: stdout becomes the pipe, then exec.
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    execl("/proc/self/exe", "test_sm1", "--crash-child", tmp.path(),
          (char *)nullptr);
    _exit(127);
  }
  close(fds[1]);
  uint32_t acked = 0, i;
  while (acked < 50 && read_full(fds[0], &i, sizeof(i))) {
    acked = i + 1;
  }
  REQUIRE(acked >= 50);
  kill(pid, SIGKILL);
  int status;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  while (read_full(fds[0], &i, sizeof(i))) {
    acked = i + 1;
  }
  close(fds[0]);

  // The newest log: its records, then zeros.
  std::vector<std::filesystem::path> logs = wal_files(tmp.path());
  REQUIRE(!logs.empty());
  std::sort(logs.begin(), logs.end(), [](const auto &a, const auto &b) {
    return a.filename().string() < b.filename().string();
  });
  std::string path = logs.back().string();
  std::string bytes;
  {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  size_t end = bytes.find_last_not_of('\0');
  REQUIRE(end != std::string::npos);
  REQUIRE(bytes.size() > end + 64);
  // checksum (bad), length 100, type 1 (a full record), 20 bytes.
  std::string torn("\xde\xad\xbe\xef\x64\x00\x01", 7);
  torn.append(20, 'x');
  {
    std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(end + 1));
    out.write(torn.data(), static_cast<std::streamsize>(torn.size()));
  }

  sm1::env_t *env = attach(tmp.path());
  REQUIRE_MESSAGE(env, sm1::errmsg());
  sm1::session_t *s = opendb(env, "db", nullptr);
  REQUIRE_MESSAGE(s, sm1::errmsg());
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
  uint64_t store = first_store(s);
  REQUIRE(store != 0);
  uint32_t n = count_rows(s, store);
  CHECK(n >= acked);
  CHECK(n <= acked + 1);
  for (uint32_t j = 0; j < n; j++) {
    CHECK(get(s, store, crash_key(j)) == "v");
  }
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::detach(env, 1) == 0);
}
