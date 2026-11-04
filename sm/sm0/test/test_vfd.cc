/*
 * test_vfd.cc -- sm_vfd.h's virtual file descriptors and the
 * process's ceiling (VFD.md).
 */
#include "doctest.h"

#include "sm0.h"
#include "sm_vfd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A directory of the test's own, removed with its files at scope end. */
struct temp_dir_t {
  std::string path;

  temp_dir_t() {
    const char *base = getenv("TMPDIR");
    std::string pattern =
        std::string(base != nullptr ? base : "/tmp") + "/sm0_vfd_XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    REQUIRE(mkdtemp(buf.data()) != nullptr);
    path = buf.data();
  }
  ~temp_dir_t() { std::filesystem::remove_all(path); }

  /* The path of file name in the directory. */
  std::string file(const std::string &name) const { return path + "/" + name; }
};

/* Write text at offset through vfd, whole. */
void put(sm_vfd_cache_t *c, sm_vfd_t vfd, const std::string &text,
         off_t offset) {
  struct iovec iov = {const_cast<char *>(text.data()), text.size()};
  REQUIRE(sm_vfd_writev(c, vfd, &iov, 1, offset) ==
          static_cast<ssize_t>(text.size()));
}

/* Read n bytes at offset through vfd. */
std::string get(sm_vfd_cache_t *c, sm_vfd_t vfd, size_t n, off_t offset) {
  std::string out(n, '\0');
  struct iovec iov = {out.data(), n};
  ssize_t got = sm_vfd_readv(c, vfd, &iov, 1, offset);
  REQUIRE(got >= 0);
  out.resize(static_cast<size_t>(got));
  return out;
}

/* Whether any fd of the process is open on path. */
bool open_on(const std::string &path) {
  DIR *d = opendir("/proc/self/fd");
  REQUIRE(d != nullptr);
  bool found = false;
  while (struct dirent *e = readdir(d)) {
    char target[PATH_MAX];
    std::string link = std::string("/proc/self/fd/") + e->d_name;
    ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    if (n > 0 && std::string(target, static_cast<size_t>(n)) == path)
      found = true;
  }
  closedir(d);
  return found;
}

/* A ceiling high enough to bound nothing, for a test to leave behind. */
const int unbounded = 1 << 30;

} // namespace

TEST_CASE("a virtual fd reads back what it wrote") {
  // Verify the plain round trip through one virtual fd.
  // 1. Open a new file through a cache, write to it, read it back.
  // 2. Its size and path are what was written and opened.
  // 3. Truncate it; its size is the new one.
  // 4. Close it: the cache holds no fd.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(4);
  REQUIRE(c != nullptr);
  std::string path = dir.file("a");
  sm_vfd_t v = sm_vfd_open(c, path.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(v > 0);

  put(c, v, "hello, world", 0);
  CHECK(get(c, v, 12, 0) == "hello, world");
  CHECK(sm_vfd_size(c, v) == 12);
  CHECK(std::string(sm_vfd_path(c, v)) == path);

  REQUIRE(sm_vfd_truncate(c, v, 5) == 0);
  CHECK(sm_vfd_size(c, v) == 5);
  CHECK(get(c, v, 100, 0) == "hello");
  CHECK(sm_vfd_sync(c, v) == 0);

  REQUIRE(sm_vfd_close(c, v) == 0);
  CHECK(sm_vfd_cache_held(c) == 0);
  sm_vfd_cache_free(c);
}

TEST_CASE("a real fd's open refuses no cache or no path, as a virtual one's") {
  // Verify sm_vfd_open_real() checks its arguments as sm_vfd_open() does,
  // with SM_ERR_INVALID, rather than read through a null pointer.
  // 1. Open with no cache, with no path, and with an empty path.
  // 2. Each fails with SM_ERR_INVALID and EINVAL, and opens nothing.
  sm_vfd_cache_t *c = sm_vfd_cache_make(4);
  for (int k = 0; k < 3; k++) {
    CAPTURE(k);
    errno = 0;
    int fd = 0 == k   ? sm_vfd_open_real(nullptr, "/dev/null", O_RDONLY, 0)
             : 1 == k ? sm_vfd_open_real(c, nullptr, O_RDONLY, 0)
                      : sm_vfd_open_real(c, "", O_RDONLY, 0);
    CHECK(fd == -1);
    CHECK(errno == EINVAL);
    CHECK(sm0::errnum() == SM_ERR_INVALID);
  }
  sm_vfd_cache_free(c);
}

TEST_CASE("a cache keeps its share open and reopens the rest") {
  // Verify many virtual fds work over a share of two real ones.
  // 1. Open six files through a cache whose share is two, writing each
  //    its own text; the cache never holds more than two fds.
  // 2. Read each back, in order and in reverse: each reopens as needed
  //    and gives its own text.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(2);
  std::vector<sm_vfd_t> v;
  for (int i = 0; i < 6; i++) {
    std::string p = dir.file("f" + std::to_string(i));
    v.push_back(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600));
    REQUIRE(v.back() > 0);
    put(c, v.back(), "file " + std::to_string(i), 0);
    CHECK(sm_vfd_cache_held(c) <= 2);
  }
  for (int i = 0; i < 6; i++)
    CHECK(get(c, v[i], 6, 0) == "file " + std::to_string(i));
  for (int i = 5; i >= 0; i--) {
    CHECK(get(c, v[i], 6, 0) == "file " + std::to_string(i));
    CHECK(sm_vfd_cache_held(c) <= 2);
  }
  sm_vfd_cache_free(c);
}

TEST_CASE("a reopen keeps what the first open created") {
  // Verify the first open's O_CREAT and O_TRUNC are not done again.
  // 1. Open a file with O_CREAT and O_TRUNC, and write to it.
  // 2. Open two more through a cache whose share is one, which evicts it.
  // 3. Read it: it reopens without truncating, and its flags for a
  //    reopen have neither O_CREAT nor O_TRUNC.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(1);
  std::string first = dir.file("first");
  sm_vfd_t v = sm_vfd_open(c, first.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(v > 0);
  put(c, v, "keep me", 0);
  for (const char *name : {"x", "y"}) {
    std::string p = dir.file(name);
    REQUIRE(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600) > 0);
  }
  CHECK_FALSE(open_on(first));
  CHECK(get(c, v, 7, 0) == "keep me");
  CHECK((sm_vfd_raw_flags(c, v) & (O_CREAT | O_TRUNC)) == 0);
  CHECK(sm_vfd_raw_mode(c, v) == 0600);
  sm_vfd_cache_free(c);
}

TEST_CASE("the least recently used fd is closed first") {
  // Verify eviction order follows use, not opening.
  // 1. Open a and b through a cache whose share is two.
  // 2. Use a again, then open c: b, used least recently, is closed.
  // 3. a and c are open; no fd of the process is on b.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(2);
  std::string a = dir.file("a"), b = dir.file("b"), cc = dir.file("c");
  sm_vfd_t va = sm_vfd_open(c, a.c_str(), O_RDWR | O_CREAT, 0600);
  sm_vfd_t vb = sm_vfd_open(c, b.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(va > 0);
  REQUIRE(vb > 0);
  put(c, va, "a", 0);
  REQUIRE(sm_vfd_open(c, cc.c_str(), O_RDWR | O_CREAT, 0600) > 0);
  CHECK(open_on(a));
  CHECK(open_on(cc));
  CHECK_FALSE(open_on(b));
  sm_vfd_cache_free(c);
}

TEST_CASE("real fds and reservations count against the share") {
  // Verify what the caller holds outside the virtual fds is counted.
  // 1. Fill a share of three with virtual fds.
  // 2. Reserve two: the cache closes two to make room.
  // 3. Open a real fd through the cache: it closes the last one.
  // 4. Reserving one more fails with SM_ERR_NOFDS and EMFILE: nothing is
  //    left to close. Once the reservation is undone, it succeeds.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(3);
  std::vector<sm_vfd_t> v;
  for (int i = 0; i < 3; i++) {
    std::string p = dir.file("v" + std::to_string(i));
    v.push_back(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600));
  }
  CHECK(sm_vfd_cache_held(c) == 3);

  REQUIRE(sm_vfd_reserve(c, 2) == 0);
  CHECK(sm_vfd_cache_held(c) == 3);
  std::string real = dir.file("real");
  int fd = sm_vfd_open_real(c, real.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(fd >= 0);
  CHECK(sm_vfd_cache_held(c) == 3);

  errno = 0;
  CHECK(sm_vfd_reserve(c, 1) == -1);
  CHECK(sm0::errnum() == SM_ERR_NOFDS);
  CHECK(errno == EMFILE);

  sm_vfd_unreserve(c, 2);
  CHECK(sm_vfd_reserve(c, 1) == 0);
  sm_vfd_unreserve(c, 1);
  REQUIRE(sm_vfd_close_real(c, fd) == 0);
  CHECK(get(c, v[0], 1, 0).empty());
  sm_vfd_cache_free(c);
}

TEST_CASE("fds already open are counted past the share") {
  // Verify sm_vfd_count() makes room, then counts what it cannot refuse.
  // 1. Fill a share of two with virtual fds.
  // 2. Count three fds the caller has opened: the cache closes its two,
  //    and holds the three, over its share.
  // 3. Opening a virtual fd now fails: the share is over, and nothing is
  //    left to close. Undoing the count, it succeeds.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(2);
  std::vector<sm_vfd_t> v;
  for (int i = 0; i < 2; i++) {
    std::string p = dir.file("c" + std::to_string(i));
    v.push_back(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600));
  }
  int before = sm_fd_held();
  sm_vfd_count(c, 3);
  CHECK(sm_vfd_cache_held(c) == 3);
  CHECK(sm_fd_held() == before + 1);

  std::string p = dir.file("more");
  CHECK(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600) == -1);
  CHECK(sm0::errnum() == SM_ERR_NOFDS);
  sm_vfd_unreserve(c, 3);
  CHECK(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600) > 0);
  sm_vfd_cache_free(c);
}

TEST_CASE("the ceiling bounds the caches together") {
  // Verify one cache cannot take an fd another holds under the ceiling.
  // 1. With nothing held, set a ceiling of four.
  // 2. Cache A, share four, opens four virtual fds.
  // 3. Cache B's open fails with SM_ERR_NOFDS and EMFILE: B has nothing
  //    of its own to close, and A's are A's.
  // 4. A closes one; B's open succeeds.
  // 5. Set while four are held, a ceiling of five counts them: B opens
  //    one more, and cache C, with nothing of its own to close, none.
  temp_dir_t dir;
  REQUIRE(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(4, 0) == 0);
  CHECK(sm_fd_ceiling() == 4);

  sm_vfd_cache_t *a = sm_vfd_cache_make(4);
  sm_vfd_cache_t *b = sm_vfd_cache_make(4);
  std::vector<sm_vfd_t> va;
  for (int i = 0; i < 4; i++) {
    std::string p = dir.file("a" + std::to_string(i));
    va.push_back(sm_vfd_open(a, p.c_str(), O_RDWR | O_CREAT, 0600));
    REQUIRE(va.back() > 0);
  }
  CHECK(sm_fd_held() == 4);

  std::string pb = dir.file("b");
  errno = 0;
  CHECK(sm_vfd_open(b, pb.c_str(), O_RDWR | O_CREAT, 0600) == -1);
  CHECK(sm0::errnum() == SM_ERR_NOFDS);
  CHECK(errno == EMFILE);

  REQUIRE(sm_vfd_close(a, va[0]) == 0);
  CHECK(sm_vfd_open(b, pb.c_str(), O_RDWR | O_CREAT, 0600) > 0);

  REQUIRE(sm_fd_ceiling_set(5, 0) == 0);
  std::string pb2 = dir.file("b2");
  CHECK(sm_vfd_open(b, pb2.c_str(), O_RDWR | O_CREAT, 0600) > 0);
  CHECK(sm_fd_held() == 5);
  sm_vfd_cache_t *c = sm_vfd_cache_make(4);
  std::string pc = dir.file("c");
  CHECK(sm_vfd_open(c, pc.c_str(), O_RDWR | O_CREAT, 0600) == -1);
  CHECK(sm0::errnum() == SM_ERR_NOFDS);

  sm_vfd_cache_free(a);
  sm_vfd_cache_free(b);
  sm_vfd_cache_free(c);
  REQUIRE(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(unbounded, 0) == 0);
}

TEST_CASE("a failed call leaves errno and sets the error") {
  // Verify failures are reported as the system call reported them.
  // 1. Opening a missing file fails with ENOENT and SM_ERR_NOTFOUND, and
  //    counts nothing.
  // 2. A virtual fd that is not open fails with EBADF and
  //    SM_ERR_INVALID.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(2);
  std::string missing = dir.file("missing");
  int held_before = sm_fd_held();

  errno = 0;
  CHECK(sm_vfd_open(c, missing.c_str(), O_RDONLY, 0) == -1);
  CHECK(errno == ENOENT);
  CHECK(sm0::errnum() == SM_ERR_NOTFOUND);
  CHECK(sm_fd_held() == held_before);
  CHECK(sm_vfd_cache_held(c) == 0);

  errno = 0;
  CHECK(sm_vfd_size(c, 42) == -1);
  CHECK(errno == EBADF);
  CHECK(sm0::errnum() == SM_ERR_INVALID);
  CHECK(sm_vfd_path(c, 42) == nullptr);
  sm_vfd_cache_free(c);
}

TEST_CASE("zero, fallocate and prefetch shape and hint a file") {
  // Verify the calls that write or reserve space without data.
  // 1. Zero 100 KB past the file's end: the file grows, and reads zeros.
  // 2. Allocate 10 KB beyond that: the size grows to cover it.
  // 3. A prefetch and a writeback succeed as hints.
  temp_dir_t dir;
  sm_vfd_cache_t *c = sm_vfd_cache_make(2);
  std::string p = dir.file("z");
  sm_vfd_t v = sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600);
  REQUIRE(v > 0);
  put(c, v, "abc", 0);

  REQUIRE(sm_vfd_zero(c, v, 3, 100 * 1024) == 0);
  CHECK(sm_vfd_size(c, v) == 3 + 100 * 1024);
  CHECK(get(c, v, 4, 100 * 1024 - 1) == std::string(4, '\0'));

  REQUIRE(sm_vfd_fallocate(c, v, 3 + 100 * 1024, 10 * 1024) == 0);
  CHECK(sm_vfd_size(c, v) == 3 + 110 * 1024);

  CHECK(sm_vfd_prefetch(c, v, 0, 4096) == 0);
  sm_vfd_writeback(c, v, 0, 4096);
  sm_vfd_cache_free(c);
}

TEST_CASE("threads with caches of their own stay under the ceiling") {
  // Verify the ceiling holds while many threads open, evict and reopen.
  // 1. Set a ceiling of 32; eight threads each make a cache of share 4.
  // 2. Each opens twenty files of its own and, many times over, writes
  //    and reads them in a scrambled order.
  // 3. No thread ever sees more than 32 fds held, and every read gives
  //    what that thread last wrote there.
  REQUIRE(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(32, 0) == 0);
  temp_dir_t dir;
  std::atomic<int> over{0}, wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      sm_vfd_cache_t *c = sm_vfd_cache_make(4);
      std::vector<sm_vfd_t> v;
      std::vector<std::string> last(20);
      for (int i = 0; i < 20; i++) {
        std::string p =
            dir.file("t" + std::to_string(t) + "_" + std::to_string(i));
        v.push_back(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600));
      }
      for (int round = 0; round < 2000; round++) {
        int i = (round * 7 + t) % 20;
        last[i] = std::to_string(round);
        std::string text = last[i] + "        ";
        struct iovec iov = {text.data(), 8};
        if (sm_vfd_writev(c, v[i], &iov, 1, 0) != 8)
          wrong++;
        int j = (round * 13 + 3) % 20;
        if (!last[j].empty()) {
          char buf[8];
          struct iovec in = {buf, 8};
          if (sm_vfd_readv(c, v[j], &in, 1, 0) != 8 ||
              std::string(buf, last[j].size()) != last[j])
            wrong++;
        }
        if (sm_fd_held() > 32)
          over++;
      }
      sm_vfd_cache_free(c);
    });
  }
  for (auto &th : threads)
    th.join();
  CHECK(over == 0);
  CHECK(wrong == 0);
  CHECK(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(unbounded, 0) == 0);
}

TEST_CASE("a ceiling of zero is measured from the open-files limit") {
  // Verify the probe gives a ceiling within the process's limit.
  // 1. With nothing held, set the ceiling to be measured, leaving 10.
  // 2. It is above zero and below the soft open-files limit.
  REQUIRE(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(0, 10) == 0);
  struct rlimit rl;
  REQUIRE(getrlimit(RLIMIT_NOFILE, &rl) == 0);
  CHECK(sm_fd_ceiling() > 0);
  CHECK(static_cast<rlim_t>(sm_fd_ceiling()) < rl.rlim_cur);
  REQUIRE(sm_fd_ceiling_set(unbounded, 0) == 0);
}

TEST_CASE("a measured ceiling counts the fds held inside it") {
  // Verify fds held while the ceiling is measured are not taken off it.
  // 1. Measure the ceiling with nothing held.
  // 2. Hold three real fds through a cache, and measure it again.
  // 3. The two are the same: the three are open, and held -- taken off
  //    as open, they would make it three less.
  temp_dir_t dir;
  REQUIRE(sm_fd_held() == 0);
  REQUIRE(sm_fd_ceiling_set(0, 10) == 0);
  int empty = sm_fd_ceiling();

  sm_vfd_cache_t *c = sm_vfd_cache_make(4);
  for (int i = 0; i < 3; i++) {
    std::string p = dir.file("h" + std::to_string(i));
    REQUIRE(sm_vfd_open(c, p.c_str(), O_RDWR | O_CREAT, 0600) > 0);
  }
  REQUIRE(sm_fd_held() == 3);
  REQUIRE(sm_fd_ceiling_set(0, 10) == 0);
  CHECK(sm_fd_ceiling() == empty);

  sm_vfd_cache_free(c);
  REQUIRE(sm_fd_ceiling_set(unbounded, 0) == 0);
}
