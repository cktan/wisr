/*
 * walfs.cc -- a file system for RocksDB that zero-fills a new
 * write-ahead log to its size before RocksDB writes it; see walfs.h.
 *
 * RocksDB appends to its log, so each commit's sync would also commit
 * the file's new size to the file system's journal; written over
 * zeros the file already has, the sync leaves the size alone, at a
 * fifth of the cost on ext4 (WALPREALLOC.md section 1).
 */
#include "walfs.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
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
 * Write bytes of zeros into fname, a log RocksDB has just made empty,
 * and sync them. A failure leaves the log to grow as RocksDB writes
 * it, which costs a sync more, not a record.
 */
void zero_fill(const std::string &fname, uint64_t bytes) {
  static const char zeros[1 << 20] = {};
  // A descriptor of our own: RocksDB's writes from offset 0 on its.
  int fd = open(fname.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
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
  (void)fdatasync(fd);
  close(fd);
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

  /* Make the file, and zero-fill it first if it is a log. */
  rocksdb::IOStatus
  NewWritableFile(const std::string &fname, const rocksdb::FileOptions &opts,
                  std::unique_ptr<rocksdb::FSWritableFile> *result,
                  rocksdb::IODebugContext *dbg) override {
    rocksdb::IOStatus s = target()->NewWritableFile(fname, opts, result, dbg);
    if (s.ok() && is_wal(fname)) {
      zero_fill(fname, m_prealloc_bytes);
    }
    return s;
  }

private:
  uint64_t m_prealloc_bytes;
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
