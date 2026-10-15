# VFD -- virtual file descriptors, under one ceiling

A design for opening more files than a thread may hold real file
descriptors for, built in `src/vfd.cc`, with its interface in
`include/sm_vfd.h` and its tests in `test/test_vfd.cc`. digres
opens its temporary files, its COPY files and the rest through
it; sm can count the files it opens itself against the same
ceiling.

## 1. Goals

- **More open files than real descriptors.** A thread keeps any
  number of files open, and only a share of them really open.
- **One ceiling for the process.** Every real fd held through
  here counts against it, so threads together cannot run the
  process out of descriptors, whatever each one's share.
- **A share for each thread**, so one busy thread cannot take
  what others need.
- **No lock where a thread works on its own files**, and errors
  as sm0's: -1, errno as the system call left it, and the
  thread's error state. Nothing throws.

## 2. The ceiling

`sm_fd_ceiling_set(total, reserve)` sets the most real fds the
process may hold through here. With `total` 0 it is measured:
the soft open-files limit, less the fds open now, less
`reserve`, left for what opens descriptors without asking -- a
socket the postmaster accepts, a library's files. The fds held
already are open, and count against it, not against the room
left. Until it is set there is none. It may be set while fds
are held: digres's postmaster holds its wake fd before it sets
the ceiling.

The fds held are one atomic count: taking one is a
compare-and-swap that fails rather than pass the ceiling, and
letting one go is a decrement. `sm_fd_held()` reads it.

## 3. A thread's cache

`sm_vfd_cache_make(share)` makes a cache for one thread: its
`share` is the most real fds it holds, counted against the
ceiling as well. The cache is a table of virtual fds -- a
virtual fd is its slot's number plus one, and a closed one's
slot is given out again -- and a `std::list` of the slots whose
file is really open, most recently used first. Each open slot
keeps its place in the list, so a use moves it to the front by
`splice()`, without a search or an allocation.

- **Use** of a virtual fd moves it to the front of the list; if
  its file was closed, it is opened again first, with the flags
  and mode it was opened with, less `O_CREAT`, `O_TRUNC` and
  `O_EXCL`, which were for the first open alone.
- **Room** for another real fd is made by closing the fd at the
  back of the list, the one used least recently, until the cache
  is within its share and the ceiling takes one more. A cache
  closes only its own fds: one that has none open and cannot get
  room fails with `SM_ERR_NOFDS` and `EMFILE`.
- **The system's limit** is room too: an `open()` that fails
  with `EMFILE` or `ENFILE` closes another of the cache's fds
  and tries again.

All I/O is at an offset, with `preadv()` and `pwritev()`: a
virtual fd has no file position to keep across a close.

## 4. Real fds, counted

Code that needs a real descriptor, not a virtual one, still
counts it: `sm_vfd_open_real()` opens one for the caller to use
and close itself by `sm_vfd_close_real()`, and
`sm_vfd_reserve(n)` counts fds the caller opens some other way --
`fopen()`, `opendir()`, a socket. Both make room first, as an
open of a virtual fd does, and hold their place in the share
until they are let go: the cache cannot close them.
`sm_vfd_count(n)` is for fds the caller has opened already, a
socket it accepted, say: it makes what room it can, and counts
them even past the share and the ceiling, as they exist.

## 5. What it leaves to its caller

It knows open, close, evict and reopen, and nothing of what the
files are for. digres keeps, in its storage/file `fd.c`:

- reporting a failure: `ereport()` from errno;
- the files a transaction or a resource owner holds, closed at
  its end;
- temporary files -- deleted at close, counted against
  `temp_file_limit` -- and their tablespaces;
- the policy for a failed `fsync()`.

## 6. Tests

`test/test_vfd.cc`, each with its synopsis and steps, run by
`make test` and under the sanitizers by `make tsan` and
`make asan`:

- a round trip: write, read back, size, truncate, sync;
- six files over a share of two, read back in either order;
- a reopen does not truncate or create again;
- the fd used least recently is the one closed -- with the
  newest closed instead, the test fails;
- real fds and reservations hold their place in the share, and
  a reservation with nothing left to close fails;
- one cache cannot take another's fds under the ceiling -- with
  the ceiling not enforced, the test fails;
- errors keep errno and set the thread's error;
- zero, allocate, prefetch and writeback;
- eight threads, each with a cache, keep under one ceiling;
- a measured ceiling is within the open-files limit.
