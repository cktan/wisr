/*
 * vfd.cc -- virtual file descriptors over a few real ones, under a
 * ceiling for the process (VFD.md, include/sm_vfd.h).
 */
#include "sm_vfd.h"

#include "sm0_error.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <string>
#include <vector>

namespace {

/* The process's ceiling, 0 for none, and the fds held against it. */
std::atomic<int> ceiling{0};
std::atomic<int> held{0};

/* Count n fds against the ceiling: false, and nothing counted, if over. */
bool ceiling_take(int n) {
  int cap = ceiling.load();
  if (cap == 0) {
    held.fetch_add(n);
    return true;
  }
  int now = held.load();
  while (now + n <= cap) {
    if (held.compare_exchange_weak(now, now + n))
      return true;
  }
  return false;
}

/* Stop counting n fds against the ceiling. */
void ceiling_give(int n) { held.fetch_sub(n); }

/* The flags a reopen leaves out: they were for the first open alone. */
constexpr int first_open_only = O_CREAT | O_TRUNC | O_EXCL;

/* How many fds the process has open now, or -1 if it cannot tell. */
int fds_open_now() {
  DIR *d = opendir("/proc/self/fd");
  if (d == nullptr)
    return -1;
  int n = 0;
  while (readdir(d) != nullptr)
    n++;
  closedir(d);
  // Less ".", "..", and the directory's own fd.
  return n - 3;
}

/*
 * Fail a call with errno err as its system call left it, the thread's
 * error set from it: -1, with errno err again for the caller.
 */
int fail_errno(int err, const std::string &what) {
  sm0::set_errno_error(err, what.c_str());
  errno = err;
  return -1;
}

} // namespace

/* One virtual fd of a cache. */
struct vfd_slot_t {
  std::string path; /* empty while the slot is free */
  int flags = 0;    /* for a reopen */
  mode_t mode = 0;
  int fd = -1; /* the real fd, -1 while evicted */
  bool used = false;
  std::list<int>::iterator lru; /* its place in the cache's lru, if fd >= 0 */
};

/* A thread's cache. A virtual fd is its slot's index plus one. */
struct sm_vfd_cache_t {
  int share;
  int reserved = 0; /* fds counted for the caller: sm_vfd_open_real, reserve */
  std::vector<vfd_slot_t> slots;
  std::vector<int> free_slots;
  std::list<int> lru; /* the slots with a real fd, most recently used first */
};

namespace {

/* The real fds the cache's slots hold. */
int open_count(const sm_vfd_cache_t *c) {
  return static_cast<int>(c->lru.size());
}

/* Close the real fd of the slot used least recently: false if none is open. */
bool evict_one(sm_vfd_cache_t *c) {
  if (c->lru.empty())
    return false;
  vfd_slot_t &s = c->slots[c->lru.back()];
  c->lru.pop_back();
  // A close that fails has still let go of the fd (close(2)).
  (void)close(s.fd);
  s.fd = -1;
  ceiling_give(1);
  return true;
}

/*
 * Count n more fds for the cache, within its share and the ceiling,
 * closing its least recently used fds until they fit: 0, or -1 with
 * SM_ERR_NOFDS if nothing is left to close.
 */
int make_room(sm_vfd_cache_t *c, int n) {
  for (;;) {
    if (open_count(c) + c->reserved + n <= c->share && ceiling_take(n))
      return 0;
    if (!evict_one(c)) {
      sm0::set_error(SM_ERR_NOFDS,
                     "sm_vfd: no room for {} fd(s): share {}, ceiling {}", n,
                     c->share, ceiling.load());
      errno = EMFILE;
      return -1;
    }
  }
}

/*
 * open(2) path, with room made for it first and counted: the fd, or -1.
 * Out of fds at the system's limit, close another of the cache's and try
 * again.
 */
int open_counted(sm_vfd_cache_t *c, const char *path, int flags, mode_t mode) {
  if (make_room(c, 1) != 0)
    return -1;
  for (;;) {
    int fd = open(path, flags, mode);
    if (fd >= 0)
      return fd;
    int err = errno;
    if (err == EINTR)
      continue;
    if ((err == EMFILE || err == ENFILE) && evict_one(c))
      continue;
    ceiling_give(1);
    if (err == EMFILE || err == ENFILE) {
      // Out at the system's limit, with nothing of the cache's to close.
      sm0::set_error(SM_ERR_NOFDS, "open \"{}\": {}", path, strerror(err));
      errno = err;
      return -1;
    }
    return fail_errno(err, std::string("open \"") + path + "\"");
  }
}

/* Slot vfd's index if it is a virtual fd in use, else -1 with the error set. */
int slot_of(const sm_vfd_cache_t *c, sm_vfd_t vfd) {
  int i = vfd - 1;
  if (c == nullptr || i < 0 || i >= static_cast<int>(c->slots.size()) ||
      !c->slots[i].used) {
    sm0::set_error(SM_ERR_INVALID, "sm_vfd: {} is not an open virtual fd", vfd);
    errno = EBADF;
    return -1;
  }
  return i;
}

/* The real fd of vfd, reopened if it was evicted, and used now; or -1. */
int real_fd(sm_vfd_cache_t *c, sm_vfd_t vfd) {
  int i = slot_of(c, vfd);
  if (i < 0)
    return -1;
  if (c->slots[i].fd >= 0) {
    c->lru.splice(c->lru.begin(), c->lru, c->slots[i].lru);
    return c->slots[i].fd;
  }
  // The slot is looked up again: the open may evict others, not resize.
  int fd = open_counted(c, c->slots[i].path.c_str(), c->slots[i].flags,
                        c->slots[i].mode);
  if (fd < 0)
    return -1;
  c->slots[i].fd = fd;
  c->slots[i].lru = c->lru.insert(c->lru.begin(), i);
  return fd;
}

/* A failed call on vfd's fd: -1, the error set from errno. */
int fail_on(const sm_vfd_cache_t *c, sm_vfd_t vfd, const char *call) {
  return fail_errno(errno,
                    std::string(call) + " \"" + c->slots[vfd - 1].path + "\"");
}

} // namespace

/* Set the process's ceiling. */
int sm_fd_ceiling_set(int total, int reserve) {
  if (total < 0 || reserve < 0)
    return sm0::set_error(SM_ERR_INVALID, "sm_fd_ceiling_set: negative count");
  if (total == 0) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
      return fail_errno(errno, "getrlimit");
    int open_now = fds_open_now();
    if (open_now < 0)
      return fail_errno(errno, "opendir \"/proc/self/fd\"");
    long limit =
        rl.rlim_cur == RLIM_INFINITY
            ? INT32_MAX
            : static_cast<long>(std::min<rlim_t>(rl.rlim_cur, INT32_MAX));
    // The fds held already are open, and inside the ceiling.
    long room = limit - open_now + held.load() - reserve;
    if (room < 1)
      return sm0::set_error(SM_ERR_NOFDS,
                            "sm_fd_ceiling_set: no fds left: limit {}, "
                            "{} open, {} held, {} reserved",
                            limit, open_now, held.load(), reserve);
    total = static_cast<int>(room);
  }
  ceiling.store(total);
  return 0;
}

/* The ceiling, 0 if none. */
int sm_fd_ceiling(void) { return ceiling.load(); }

/* The fds held against the ceiling. */
int sm_fd_held(void) { return held.load(); }

/* A cache with a share of real fds. */
sm_vfd_cache_t *sm_vfd_cache_make(int share) {
  if (share < 1) {
    sm0::set_error(SM_ERR_INVALID, "sm_vfd_cache_make: share {} below 1",
                   share);
    return nullptr;
  }
  sm_vfd_cache_t *c = new sm_vfd_cache_t;
  c->share = share;
  return c;
}

/* Close the cache's fds, uncount what it reserved, and free it. */
void sm_vfd_cache_free(sm_vfd_cache_t *cache) {
  if (cache == nullptr)
    return;
  sm_vfd_close_all(cache);
  ceiling_give(cache->reserved);
  delete cache;
}

/* The real fds the cache holds. */
int sm_vfd_cache_held(const sm_vfd_cache_t *cache) {
  return open_count(cache) + cache->reserved;
}

/* Close every real fd the cache holds open. */
void sm_vfd_close_all(sm_vfd_cache_t *cache) {
  while (evict_one(cache)) {
  }
}

/* Open path as a virtual fd. */
sm_vfd_t sm_vfd_open(sm_vfd_cache_t *cache, const char *path, int flags,
                     mode_t mode) {
  if (cache == nullptr || path == nullptr || *path == '\0') {
    sm0::set_error(SM_ERR_INVALID, "sm_vfd_open: no cache or no path");
    errno = EINVAL;
    return -1;
  }
  int fd = open_counted(cache, path, flags, mode);
  if (fd < 0)
    return -1;
  int i;
  if (!cache->free_slots.empty()) {
    i = cache->free_slots.back();
    cache->free_slots.pop_back();
  } else {
    i = static_cast<int>(cache->slots.size());
    cache->slots.emplace_back();
  }
  vfd_slot_t &s = cache->slots[i];
  s.path = path;
  s.flags = flags & ~first_open_only;
  s.mode = mode;
  s.fd = fd;
  s.used = true;
  s.lru = cache->lru.insert(cache->lru.begin(), i);
  return i + 1;
}

/* Close a virtual fd. */
int sm_vfd_close(sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int i = slot_of(cache, vfd);
  if (i < 0)
    return -1;
  vfd_slot_t &s = cache->slots[i];
  int rc = 0, err = 0;
  if (s.fd >= 0) {
    cache->lru.erase(s.lru);
    if (close(s.fd) != 0) {
      rc = -1;
      err = errno;
    }
    ceiling_give(1);
  }
  std::string path = std::move(s.path);
  s = vfd_slot_t{};
  cache->free_slots.push_back(i);
  if (rc != 0)
    return fail_errno(err, "close \"" + path + "\"");
  return 0;
}

/* Read into iov at offset. */
ssize_t sm_vfd_readv(sm_vfd_cache_t *cache, sm_vfd_t vfd,
                     const struct iovec *iov, int iovcnt, off_t offset) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  for (;;) {
    ssize_t n = preadv(fd, iov, iovcnt, offset);
    if (n >= 0)
      return n;
    if (errno != EINTR)
      return fail_on(cache, vfd, "preadv");
  }
}

/* Write iov at offset. */
ssize_t sm_vfd_writev(sm_vfd_cache_t *cache, sm_vfd_t vfd,
                      const struct iovec *iov, int iovcnt, off_t offset) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  for (;;) {
    ssize_t n = pwritev(fd, iov, iovcnt, offset);
    if (n >= 0)
      return n;
    if (errno != EINTR)
      return fail_on(cache, vfd, "pwritev");
  }
}

/* fsync the file. */
int sm_vfd_sync(sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  for (;;) {
    if (fsync(fd) == 0)
      return 0;
    if (errno != EINTR)
      return fail_on(cache, vfd, "fsync");
  }
}

/* The file's size. */
off_t sm_vfd_size(sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) != 0)
    return fail_on(cache, vfd, "fstat");
  return st.st_size;
}

/* Set the file's size. */
int sm_vfd_truncate(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t size) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  for (;;) {
    if (ftruncate(fd, size) == 0)
      return 0;
    if (errno != EINTR)
      return fail_on(cache, vfd, "ftruncate");
  }
}

/* Write amount zero bytes at offset, a block at a time. */
int sm_vfd_zero(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                off_t amount) {
  static const char zeros[64 * 1024] = {};
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  while (amount > 0) {
    size_t chunk = static_cast<size_t>(
        std::min<off_t>(amount, static_cast<off_t>(sizeof(zeros))));
    ssize_t n = pwrite(fd, zeros, chunk, offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return fail_on(cache, vfd, "pwrite");
    }
    // A short write moves on by what it wrote.
    offset += n;
    amount -= n;
  }
  return 0;
}

/* Allocate space at offset. */
int sm_vfd_fallocate(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                     off_t amount) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  for (;;) {
    // posix_fallocate returns the error rather than setting errno.
    int err = posix_fallocate(fd, offset, amount);
    if (err == 0)
      return 0;
    if (err != EINTR) {
      errno = err;
      return fail_on(cache, vfd, "posix_fallocate");
    }
  }
}

/* Advise the kernel the range will be read soon. */
int sm_vfd_prefetch(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                    off_t amount) {
  int fd = real_fd(cache, vfd);
  if (fd < 0)
    return -1;
  int err = posix_fadvise(fd, offset, amount, POSIX_FADV_WILLNEED);
  if (err != 0) {
    errno = err;
    return fail_on(cache, vfd, "posix_fadvise");
  }
  return 0;
}

/* Start writing the range back, if the fd is open. */
void sm_vfd_writeback(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                      off_t amount) {
  int i = slot_of(cache, vfd);
  if (i < 0 || cache->slots[i].fd < 0)
    return;
#ifdef __linux__
  // A hint: what it cannot do, it leaves to the kernel's own writeback.
  (void)sync_file_range(cache->slots[i].fd, offset, amount,
                        SYNC_FILE_RANGE_WRITE);
#else
  (void)offset;
  (void)amount;
#endif
}

/* The path vfd was opened with. */
const char *sm_vfd_path(const sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int i = slot_of(cache, vfd);
  return i < 0 ? nullptr : cache->slots[i].path.c_str();
}

/* The real fd behind vfd. */
int sm_vfd_raw_fd(sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  return real_fd(cache, vfd);
}

/* The flags a reopen of vfd uses. */
int sm_vfd_raw_flags(const sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int i = slot_of(cache, vfd);
  return i < 0 ? -1 : cache->slots[i].flags;
}

/* The mode a reopen of vfd uses. */
mode_t sm_vfd_raw_mode(const sm_vfd_cache_t *cache, sm_vfd_t vfd) {
  int i = slot_of(cache, vfd);
  return i < 0 ? 0 : cache->slots[i].mode;
}

/* open(2) a real fd for the caller, counted. */
int sm_vfd_open_real(sm_vfd_cache_t *cache, const char *path, int flags,
                     mode_t mode) {
  int fd = open_counted(cache, path, flags, mode);
  if (fd < 0)
    return -1;
  cache->reserved++;
  return fd;
}

/* Close a real fd sm_vfd_open_real() gave. */
int sm_vfd_close_real(sm_vfd_cache_t *cache, int fd) {
  int rc = close(fd);
  int err = errno;
  cache->reserved--;
  ceiling_give(1);
  if (rc != 0)
    return fail_errno(err, "close");
  return 0;
}

/* Count n fds the caller opens itself. */
int sm_vfd_reserve(sm_vfd_cache_t *cache, int n) {
  if (n < 1)
    return sm0::set_error(SM_ERR_INVALID, "sm_vfd_reserve: n {} below 1", n);
  if (make_room(cache, n) != 0)
    return -1;
  cache->reserved += n;
  return 0;
}

/* Count n fds the caller has opened, past the share and ceiling if must be. */
void sm_vfd_count(sm_vfd_cache_t *cache, int n) {
  // Room is made where it can be; what is left over is counted anyway.
  while (open_count(cache) + cache->reserved + n > cache->share ||
         !ceiling_take(n)) {
    if (!evict_one(cache)) {
      held.fetch_add(n);
      cache->reserved += n;
      return;
    }
  }
  cache->reserved += n;
}

/* Stop counting n reserved fds. */
void sm_vfd_unreserve(sm_vfd_cache_t *cache, int n) {
  cache->reserved -= n;
  ceiling_give(n);
}
