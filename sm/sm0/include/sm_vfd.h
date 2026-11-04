/*
 * sm_vfd.h -- virtual file descriptors: many open files over a few
 * real ones (VFD.md).
 *
 * A cache holds any number of virtual fds, keeps at most its share
 * of them really open, and closes the least recently used one to
 * make room, reopening a closed one at its next use. A cache is
 * its thread's: one thread uses it, so it takes no lock. Every real
 * fd a cache holds, or a caller reserves through it, counts against
 * one ceiling for the process, so threads together cannot run it
 * out; a cache's share bounds what one thread may hold.
 *
 * A call that fails returns -1 with errno as the system call left
 * it -- EMFILE when it is the ceiling or the share that is reached
 * -- and the thread's error set as every sm call sets it
 * (sm_errnum(), sm_errmsg()). Nothing here throws.
 */
#ifndef SM_VFD_H
#define SM_VFD_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

/* ---- the process's ceiling ---- */

/*
 * Set the most real fds the process's caches and reservations may
 * hold together. total 0 measures it: the open-files limit, less
 * the fds already open but not held through here, less reserve,
 * which is left for what opens fds without asking (sockets, a
 * library). The fds held already count against the new ceiling.
 * Until it is set there is no ceiling. Any thread.
 */
int sm_fd_ceiling_set(int total, int reserve);

/* The ceiling, 0 if none is set. Any thread. */
int sm_fd_ceiling(void);

/* How many fds are held against the ceiling. Any thread. */
int sm_fd_held(void);

/* ---- a thread's cache ---- */

typedef struct sm_vfd_cache_t sm_vfd_cache_t;

/* A virtual fd: > 0. One that is closed may be given out again. */
typedef int sm_vfd_t;

/*
 * A cache that keeps at most share of its fds really open, and
 * reserved: the thread's share, so one thread cannot hold the
 * whole ceiling. share must be at least 1.
 */
sm_vfd_cache_t *sm_vfd_cache_make(int share);

/* Close every fd of the cache, then free it. NULL is a no-op. */
void sm_vfd_cache_free(sm_vfd_cache_t *cache);

/* How many real fds the cache holds now: open, and reserved. */
int sm_vfd_cache_held(const sm_vfd_cache_t *cache);

/* Close every real fd the cache holds open; the virtual fds stay. */
void sm_vfd_close_all(sm_vfd_cache_t *cache);

/* ---- virtual fds ---- */

/*
 * Open path with open(2)'s flags and mode, as a virtual fd. O_CREAT,
 * O_TRUNC and O_EXCL apply to this open only: a reopen after an
 * eviction drops them.
 */
sm_vfd_t sm_vfd_open(sm_vfd_cache_t *cache, const char *path, int flags,
                     mode_t mode);

/* Close a virtual fd, and its real one if open. */
int sm_vfd_close(sm_vfd_cache_t *cache, sm_vfd_t vfd);

/*
 * Read or write at offset, reopening the fd if it was evicted: the
 * bytes moved, which may be fewer than asked, or -1. An interrupted
 * system call is made again.
 */
ssize_t sm_vfd_readv(sm_vfd_cache_t *cache, sm_vfd_t vfd,
                     const struct iovec *iov, int iovcnt, off_t offset);
ssize_t sm_vfd_writev(sm_vfd_cache_t *cache, sm_vfd_t vfd,
                      const struct iovec *iov, int iovcnt, off_t offset);

/* fsync(2) the file. */
int sm_vfd_sync(sm_vfd_cache_t *cache, sm_vfd_t vfd);

/* The file's size in bytes, or -1. */
off_t sm_vfd_size(sm_vfd_cache_t *cache, sm_vfd_t vfd);

/* Set the file's size: cut it, or extend it with zeros. */
int sm_vfd_truncate(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t size);

/* Write amount zero bytes at offset. */
int sm_vfd_zero(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                off_t amount);

/* Allocate amount bytes of space at offset, without writing them. */
int sm_vfd_fallocate(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                     off_t amount);

/* Advise the kernel the range will be read soon; a hint only. */
int sm_vfd_prefetch(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                    off_t amount);

/*
 * Start writing the range's dirty pages back, without waiting; a
 * hint only, and none if the fd is evicted.
 */
void sm_vfd_writeback(sm_vfd_cache_t *cache, sm_vfd_t vfd, off_t offset,
                      off_t amount);

/* The path a virtual fd was opened with; valid while it is open. */
const char *sm_vfd_path(const sm_vfd_cache_t *cache, sm_vfd_t vfd);

/*
 * The real fd behind a virtual fd, opened if it was evicted. Good
 * until the next call on the cache, which may evict it.
 */
int sm_vfd_raw_fd(sm_vfd_cache_t *cache, sm_vfd_t vfd);

/* The flags and mode a reopen uses. */
int sm_vfd_raw_flags(const sm_vfd_cache_t *cache, sm_vfd_t vfd);
mode_t sm_vfd_raw_mode(const sm_vfd_cache_t *cache, sm_vfd_t vfd);

/* ---- real fds, counted ---- */

/*
 * open(2) a real fd for the caller to keep and to close by
 * sm_vfd_close_real(), counted against the cache's share and the
 * ceiling: to make room, the cache closes its least recently used
 * fds, on EMFILE or ENFILE as well, and tries again.
 */
int sm_vfd_open_real(sm_vfd_cache_t *cache, const char *path, int flags,
                     mode_t mode);

/* Close a real fd sm_vfd_open_real() gave, and stop counting it. */
int sm_vfd_close_real(sm_vfd_cache_t *cache, int fd);

/*
 * Count n real fds the caller opens some other way -- fopen(),
 * opendir(), a socket, a pipe -- making room for them first: 0, or
 * -1 with SM_ERR_NOFDS if neither the share nor the ceiling has
 * room once the cache has closed what it can. Undone by
 * sm_vfd_unreserve().
 */
int sm_vfd_reserve(sm_vfd_cache_t *cache, int n);

/*
 * Count n real fds the caller has already opened -- a socket it
 * accepted, a pipe -- so that they cannot be refused: the cache
 * closes what it can to make room, and counts them even past its
 * share and the ceiling, as they exist. Undone by
 * sm_vfd_unreserve().
 */
void sm_vfd_count(sm_vfd_cache_t *cache, int n);

/* Stop counting n fds sm_vfd_reserve() or sm_vfd_count() counted. */
void sm_vfd_unreserve(sm_vfd_cache_t *cache, int n);

#endif /* SM_VFD_H */
