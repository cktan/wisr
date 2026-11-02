/*
 * walfs.cc -- a file system for RocksDB that zero-fills a new
 * write-ahead log to its size before RocksDB writes it; see walfs.h.
 *
 * RocksDB appends to its log, so each commit's sync would also commit
 * the file's new size to the file system's journal; written over
 * zeros the file already has, the sync leaves the size alone, at a
 * fifth of the cost on ext4 (WALPREALLOC.md section 1). So no write
 * waits for those zeros, the next log is made ahead, as a spare a
 * thread zeroes, and a new log takes its place (section 2).
 */
#include "walfs.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rocksdb/file_system.h>

namespace wisr {

namespace {

/* Whether fname names a write-ahead log: digits, then ".log". */
bool is_wal(const std::string &fname) {
  size_t slash = fname.rfind('/');
  std::string base =
      slash == std::string::npos ? fname : fname.substr(slash + 1);
  const std::string ext = ".log";
  if (base.size() <= ext.size() ||
      0 != base.compare(base.size() - ext.size(), ext.size(), ext)) {
    return false;
  }
  for (size_t i = 0; i < base.size() - ext.size(); i++) {
    if (base[i] < '0' || base[i] > '9') {
      return false;
    }
  }
  return true;
}

/*
 * Write bytes of zeros into fname, opened with flags, and sync them:
 * whether all of it was. A log left short grows as RocksDB writes it,
 * which costs a sync more, not a record.
 */
bool zero_fill(const std::string &fname, uint64_t bytes, int flags) {
  static const char zeros[1 << 20] = {};
  // A descriptor of our own: RocksDB's writes from offset 0 on its.
  int fd = open(fname.c_str(), O_WRONLY | O_CLOEXEC | flags, 0644);
  if (fd < 0) {
    return false;
  }
  uint64_t off = 0;
  while (off < bytes) {
    size_t n = bytes - off < sizeof(zeros) ? bytes - off : sizeof(zeros);
    ssize_t w = pwrite(fd, zeros, n, static_cast<off_t>(off));
    if (w <= 0) {
      break;
    }
    off += static_cast<uint64_t>(w);
  }
  // Make the size durable once, so no commit's sync must.
  bool ok = off == bytes && 0 == fdatasync(fd);
  close(fd);
  return ok;
}

/* RocksDB's default file system, its new logs zero-filled first. */
class walfs_t : public rocksdb::FileSystemWrapper {
public:
  /* Wrap target, zero-filling each new log to prealloc_bytes. */
  walfs_t(const std::shared_ptr<rocksdb::FileSystem> &target,
          uint64_t prealloc_bytes)
      : FileSystemWrapper(target), m_prealloc_bytes(prealloc_bytes) {}

  /* The name RocksDB knows this file system by. */
  static const char *kClassName() { return "WisrWalFileSystem"; }

  /* The name RocksDB knows this file system by. */
  const char *Name() const override { return kClassName(); }

  /* Wait for the spare's thread, and remove what it made. */
  ~walfs_t() override {
    std::string spare = take_spare();
    if (!spare.empty()) {
      unlink(spare.c_str());
    }
  }

  /* Make the file: if a log, from the spare, or zero-filled here. */
  rocksdb::IOStatus
  NewWritableFile(const std::string &fname, const rocksdb::FileOptions &opts,
                  std::unique_ptr<rocksdb::FSWritableFile> *result,
                  rocksdb::IODebugContext *dbg) override {
    if (!is_wal(fname)) {
      return target()->NewWritableFile(fname, opts, result, dbg);
    }
    std::string spare = take_spare();
    if (!spare.empty()) {
      // Renamed to fname and opened untruncated: RocksDB writes from
      // offset 0 over the zeros.
      rocksdb::IOStatus s =
          target()->ReuseWritableFile(fname, spare, opts, result, dbg);
      if (s.ok()) {
        make_spare(fname);
        return s;
      }
      // Its database dropped, say: make the log the long way.
      unlink(spare.c_str());
    }
    rocksdb::IOStatus s = target()->NewWritableFile(fname, opts, result, dbg);
    if (s.ok()) {
      (void)zero_fill(fname, m_prealloc_bytes, 0);
      make_spare(fname);
    }
    return s;
  }

private:
  /* The finished spare's path, waiting for one being made; or empty. */
  std::string take_spare() {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_zeroer.joinable()) {
      return {};
    }
    // Waits at most what zeroing the log here would.
    m_zeroer.join();
    return m_spare_ok ? m_spare : std::string();
  }

  /*
   * Start zeroing a spare beside log, a log just made.
   * 1. If log is its directory's first log since the open, remove what
   *    a crash left there, and start no spare: a database that never
   *    switches logs writes no second 64 MB.
   * 2. Else start a thread that zeroes the spare. With no thread to be
   *    had, the next log is zeroed in place.
   */
  void make_spare(const std::string &log) {
    size_t slash = log.rfind('/');
    std::string dir = slash == std::string::npos ? "." : log.substr(0, slash);
    std::string path = dir + "/" + wal_spare_name;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_dirs.insert(dir).second) {
      unlink(path.c_str());
      return;
    }
    if (m_zeroer.joinable()) {
      return;
    }
    m_spare = path;
    m_spare_ok = false;
    try {
      // On a thread of its own: 64 MB and a sync are what no write
      // should wait for.
      m_zeroer = std::thread([this, path] {
        m_spare_ok = zero_fill(path, m_prealloc_bytes, O_CREAT | O_TRUNC);
      });
    } catch (const std::system_error &) {
    }
  }

  uint64_t m_prealloc_bytes;
  std::mutex m_mutex;           /* guards all below, but m_spare_ok */
  std::thread m_zeroer;         /* the spare's, joined to take it */
  std::string m_spare;          /* the spare's path */
  bool m_spare_ok = false;      /* written by m_zeroer, read after join */
  std::set<std::string> m_dirs; /* directories that have had a log */
};

/* Monotonic microseconds. */
double now_us() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/* The median of v, which is not empty. */
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

} // namespace

/* Time append and overwrite syncs in a scratch file; see walfs.h. */
int wal_probe(const std::string &dir, double *append_us, double *overwrite_us) {
  const int rounds = 8;
  const char rec[100] = {};
  std::string path = dir + "/sm_walprobe";
  // A file of our own, gone however the probe ends.
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    return -1;
  }
  std::vector<double> append, overwrite;
  bool ok = true;
  // Appends: each sync grows the file.
  for (int i = 0; ok && i < rounds; i++) {
    double t = now_us();
    ok = write(fd, rec, sizeof(rec)) == sizeof(rec) && fdatasync(fd) == 0;
    append.push_back(now_us() - t);
  }
  // Overwrites of the same bytes: each sync leaves its size alone.
  for (int i = 0; ok && i < rounds; i++) {
    double t = now_us();
    ok = pwrite(fd, rec, sizeof(rec), static_cast<off_t>(i) * sizeof(rec)) ==
             sizeof(rec) &&
         fdatasync(fd) == 0;
    overwrite.push_back(now_us() - t);
  }
  close(fd);
  unlink(path.c_str());
  if (!ok) {
    return -1;
  }
  *append_us = median(append);
  *overwrite_us = median(overwrite);
  return 0;
}

/* Whether release names Microsoft, in either case; see walfs.h. */
bool wal_release_is_wsl(const std::string &release) {
  std::string lower(release);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower.find("microsoft") != std::string::npos;
}

/* Whether this kernel is WSL's; see walfs.h. */
bool wal_on_wsl() {
  // WSL's kernels name themselves here, in both of its versions.
  char buf[256];
  int fd = open("/proc/sys/kernel/osrelease", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  return n > 0 && wal_release_is_wsl(std::string(buf, static_cast<size_t>(n)));
}

/* Zero-fill when growing a file makes a sync half again as dear. */
bool wal_prealloc_pays(double append_us, double overwrite_us) {
  return overwrite_us > 0 && append_us >= 1.5 * overwrite_us;
}

/* An Env over the default file system, new logs zero-filled. */
std::unique_ptr<rocksdb::Env> walfs_env(uint64_t prealloc_bytes) {
  return rocksdb::NewCompositeEnv(std::make_shared<walfs_t>(
      rocksdb::FileSystem::Default(), prealloc_bytes));
}

} // namespace wisr
