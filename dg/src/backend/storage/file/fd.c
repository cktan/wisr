/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fd.c
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 1024 on many modern
 * operating systems, but may be lower on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * The pool is sm0's (sm_vfd.h): each thread has its own cache of virtual
 * fds, tcb->vfd, holding at most its share of real ones,
 * max_safe_fds_per_thread, and
 * every real fd any thread holds through it counts against one ceiling for
 * the process.  A File is an entry of this thread's table here, which
 * keeps what sm0 knows nothing of: the transaction or resource owner that
 * holds the file, and whether it is temporary.
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * INTERFACE ROUTINES
 *
 * PathNameOpenFile and OpenTemporaryFile are used to open virtual files.
 * A File opened with OpenTemporaryFile is automatically deleted when the
 * File is closed, either explicitly or implicitly at end of transaction or
 * process exit. PathNameOpenFile is intended for files that are held open
 * for a long time, like relation files. It is the caller's responsibility
 * to close them, there is no automatic mechanism in fd.c for that.
 *
 * AllocateFile, AllocateDir, OpenPipeStream and OpenTransientFile are
 * wrappers around fopen(3), opendir(3), popen(3) and open(2), respectively.
 * They behave like the corresponding native functions, except that the handle
 * is registered with the current subtransaction, and will be automatically
 * closed at abort. These are intended mainly for short operations like
 * reading a configuration file; there is a limit on the number of files that
 * can be opened using these functions at any one time.
 *
 * If a non-virtual file descriptor needs to be held open for any length of
 * time, report it to fd.c by calling AcquireExternalFD or ReserveExternalFD
 * (and eventually ReleaseExternalFD), so that we can take it into account
 * while deciding how many VFDs can be open.  This applies to FDs obtained
 * without use of any fd.c API.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h> /* for getrlimit */
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/xact.h"
#include "catalog/pg_tablespace.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner.h"
#include "utils/control_block.h"
#include "utils/varlena.h"

#include "sm.h"
#include "sm_vfd.h"

/* sm_vfd.h opens a file with open(2)'s flags alone: there is no F_NOCACHE. */
#ifdef PG_O_DIRECT_USE_F_NOCACHE
#error "PG_O_DIRECT by F_NOCACHE is not supported"
#endif

/* Define PG_FLUSH_DATA_WORKS if we have an implementation for pg_flush_data */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/*
 * We must leave some file descriptors free for system(), the dynamic loader,
 * and other code that tries to open files without consulting fd.c.  This
 * is the number left free.  (While we try fairly hard to prevent EMFILE
 * errors, there's never any guarantee that we won't get ENFILE due to
 * other processes chewing up FDs.  So it's a bad idea to try to open files
 * without consulting fd.c.  Nonetheless we cannot control all code.)
 *
 * Because this is just a fixed setting, we are effectively assuming that
 * no such code will leave FDs open over the long term; otherwise the slop
 * is likely to be insufficient.  Note in particular that we expect that
 * loading a shared library does not result in any permanent increase in
 * the number of open files.  (This appears to be true on most if not
 * all platforms as of Feb 2004.)
 */
#define NUM_RESERVED_FDS		10

/*
 * If we have fewer than this many usable FDs after allowing for the reserved
 * ones, choke.  (This value is chosen to work with "ulimit -n 64", but not
 * much less than that.)  It is also the smallest share of them a thread gets.
 */
#define FD_MINFREE				48

/*
 * A number of platforms allow individual processes to open many more files
 * than they can really support when *many* processes do the same thing.
 * This GUC parameter lets the DBA limit max_safe_fds_per_thread to something
 * less than
 * what the postmaster's initial probe suggests will work.
 */

/*
 * Maximum number of file descriptors a thread opens for operations that fd.c
 * knows about (VFDs, AllocateFile etc, or "external" FDs): its share of the
 * process's ceiling.  This is initialized to a conservative value.  The
 * postmaster, or a standalone backend, calls set_max_safe_fds() late in
 * initialization to update the value, and the session threads it starts
 * then are given it.
 *
 * Note: the value of max_files_per_process is taken into account while
 * setting this variable, and so need not be tested separately.
 *
 * It is pcb->max_safe_fds_per_thread, FD_MINFREE until set.
 */
static_assert(pcb_t{}.max_safe_fds_per_thread == FD_MINFREE,
              "pcb->max_safe_fds_per_thread starts at FD_MINFREE");

/* Whether it is safe to continue running after fsync() fails. */
thread_local /*LOCAL*/ bool data_sync_retry = false;

/* Debugging.... */

#ifdef FDDEBUG
#define DO_DB(A) \
	do { \
		int			_do_db_save_errno = errno; \
		A; \
		errno = _do_db_save_errno; \
	} while (0)
#else
#define DO_DB(A) \
	((void) 0)
#endif

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

/* these are the assigned bits in fdstate below: */
#define FD_DELETE_AT_CLOSE	(1 << 0) /* T = delete when closed */
#define FD_CLOSE_AT_EOXACT	(1 << 1) /* T = close at eoXact */
#define FD_TEMP_FILE_LIMIT	(1 << 2) /* T = respect temp_file_limit */

typedef struct vfd {
  sm_vfd_t vfd;           /* sm0's virtual fd, in tcb->vfd */
  unsigned short fdstate; /* bitflags for VFD's state */
  ResourceOwner resowner; /* owner, for automatic cleanup */
  File nextFree;          /* link to next free VFD, if in freelist */
  off_t fileSize;         /* current size of file (0 if not temporary) */
  char *fileName;         /* name of file, or NULL for unused VFD */
  /* NB: fileName is malloc'd, and must be free'd when closing the VFD */
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.  This grows as
 * needed.  'File' values are indexes into this array.
 * Note that VfdCache[0] is not a usable VFD, just a list header.
 */
static thread_local /*LOCAL*/ Vfd *VfdCache;
static thread_local /*LOCAL*/ Size SizeVfdCache = 0;

/*
 * Flag to tell whether it's worth scanning VfdCache looking for temp files
 * to close
 */
static thread_local /*LOCAL*/ bool have_xact_temporary_files = false;

/*
 * Tracks the total size of all temporary files.  Note: when temp_file_limit
 * is being enforced, this cannot overflow since the limit cannot be more
 * than INT_MAX kilobytes.  When not enforcing, it could theoretically
 * overflow, but we don't care.
 */
static thread_local /*LOCAL*/ uint64 temporary_files_size = 0;

/* Temporary file access initialized and not yet shut down? */
#ifdef USE_ASSERT_CHECKING
static thread_local /*LOCAL*/ bool temporary_files_allowed = false;
#endif

/*
 * List of OS handles opened with AllocateFile, AllocateDir and
 * OpenTransientFile.
 */
typedef enum {
  AllocateDescFile,
  AllocateDescPipe,
  AllocateDescDir,
  AllocateDescRawFD,
} AllocateDescKind;

typedef struct {
  AllocateDescKind kind;
  SubTransactionId create_subid;
  union {
    FILE *file;
    DIR *dir;
    int fd;
  } desc;
  pid_t pid; /* a pipe's program, to wait for as it closes */
} AllocateDesc;

static thread_local /*LOCAL*/ int numAllocatedDescs = 0;
static thread_local /*LOCAL*/ int maxAllocatedDescs = 0;
static thread_local /*LOCAL*/ AllocateDesc *allocatedDescs = NULL;

/*
 * Number of temporary files opened during the current session;
 * this is used in generation of tempfile names.
 */
static thread_local /*LOCAL*/ long tempFileCounter = 0;

/*--------------------
 *
 * Private Routines
 *
 * AllocateVfd	   - grab a free (or new) file record (from VfdCache)
 * FreeVfd		   - free a file record
 *
 * Elements that are open can be recognized by having a non-null fileName
 * field.
 *--------------------
 */
static File AllocateVfd(void);
static void FreeVfd(File file);

static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError);
static bool reserveAllocatedDesc(void);
static int FreeDesc(AllocateDesc *desc);

static void BeforeShmemExit_Files(int code, Datum arg);
static void CleanupTempFiles(bool isCommit, bool isProcExit);

#ifdef PG_FLUSH_DATA_WORKS
#endif

static int fsync_parent_path(const char *fname, int elevel);

/* ResourceOwner callbacks to hold virtual file descriptors */
static void ResOwnerReleaseFile(Datum res);
static char *ResOwnerPrintFile(Datum res);

static const ResourceOwnerDesc file_resowner_desc = {
    .name = "File",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = RELEASE_PRIO_FILES,
    .ReleaseResource = ResOwnerReleaseFile,
    .DebugPrint = ResOwnerPrintFile};

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void ResourceOwnerRememberFile(ResourceOwner owner, File file) {
  ResourceOwnerRemember(owner, Int32GetDatum(file), &file_resowner_desc);
}
static inline void ResourceOwnerForgetFile(ResourceOwner owner, File file) {
  ResourceOwnerForget(owner, Int32GetDatum(file), &file_resowner_desc);
}

/*
 * This thread's cache of virtual fds, made at its first use with the share
 * max_safe_fds_per_thread gives it.  The postmaster's is made before
 * set_max_safe_fds()
 * runs, and keeps the smaller share it was made with.
 */
static sm_vfd_cache_t *vfd_cache(void) {
  if (tcb->vfd == nullptr) {
    sm_vfd_cache_t *cache = sm_vfd_cache_make(pcb->max_safe_fds_per_thread);

    if (cache == NULL)
      ereport(FATAL, (errmsg_internal("%s", sm_errmsg())));
    tcb->vfd.reset(cache);
  }
  return tcb->vfd.get();
}

/* Free a thread's cache of virtual fds: its real fds close, uncounted. */
void sm_vfd_cache_free_t::operator()(sm_vfd_cache_t *cache) const {
  sm_vfd_cache_free(cache);
}

/*
 * pg_fsync --- do fsync with or without writethrough
 */
int pg_fsync(int fd) {
#if !defined(WIN32) && defined(USE_ASSERT_CHECKING)
  struct stat st;

  /*
   * Some operating system implementations of fsync() have requirements
   * about the file access modes that were used when their file descriptor
   * argument was opened, and these requirements differ depending on whether
   * the file descriptor is for a directory.
   *
   * For any file descriptor that may eventually be handed to fsync(), we
   * should have opened it with access modes that are compatible with
   * fsync() on all supported systems, otherwise the code may not be
   * portable, even if it runs ok on the current system.
   *
   * We assert here that a descriptor for a file was opened with write
   * permissions (i.e., not O_RDONLY) and for a directory without write
   * permissions (O_RDONLY).  Notice that the assertion check is made even
   * if fsync() is disabled.
   *
   * If fstat() fails, ignore it and let the follow-up fsync() complain.
   */
  if (fstat(fd, &st) == 0) {
    int desc_flags = fcntl(fd, F_GETFL);

    desc_flags &= O_ACCMODE;

    if (S_ISDIR(st.st_mode))
      Assert(desc_flags == O_RDONLY);
    else
      Assert(desc_flags != O_RDONLY);
  }
  errno = 0;
#endif

  /* #if is to skip the wal_sync_method test if there's no need for it */
#if defined(HAVE_FSYNC_WRITETHROUGH)
  if (wal_sync_method == WAL_SYNC_METHOD_FSYNC_WRITETHROUGH)
    return pg_fsync_writethrough(fd);
  else
#endif
    return pg_fsync_no_writethrough(fd);
}

/*
 * pg_fsync_no_writethrough --- same as fsync except it retries on EINTR
 */
int pg_fsync_no_writethrough(int fd) {
  int rc;

retry:
  rc = fsync(fd);

  if (rc == -1 && errno == EINTR)
    goto retry;

  return rc;
}

/*
 * pg_fsync_writethrough
 */
int pg_fsync_writethrough(int fd) {
#if defined(F_FULLFSYNC)
  return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
  errno = ENOSYS;
  return -1;
#endif
}

/*
 * pg_file_exists -- check that a file exists.
 *
 * This requires an absolute path to the file.  Returns true if the file is
 * not a directory, false otherwise.
 */
bool pg_file_exists(const char *name) {
  struct stat st;

  Assert(name != NULL);

  if (stat(name, &st) == 0)
    return !S_ISDIR(st.st_mode);
  else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
    ereport(ERROR, (errcode_for_file_access(),
                    errmsg("could not access file \"%s\": %m", name)));

  return false;
}

/*
 * pg_flush_data --- advise OS that the described dirty data should be flushed
 *
 * offset of 0 with nbytes 0 means that the entire file should be flushed
 */
void pg_flush_data(int fd, off_t offset, off_t nbytes) {
  /*
   * We compile all alternatives that are supported on the current platform,
   * to find portability problems more easily.
   */
#if defined(HAVE_SYNC_FILE_RANGE)
  {
    int rc;
    static thread_local /*LOCAL*/ bool not_implemented_by_kernel = false;

    if (not_implemented_by_kernel)
      return;

  retry:

    /*
     * sync_file_range(SYNC_FILE_RANGE_WRITE), currently linux specific,
     * tells the OS that writeback for the specified blocks should be
     * started, but that we don't want to wait for completion.  Note that
     * this call might block if too much dirty data exists in the range.
     * This is the preferable method on OSs supporting it, as it works
     * reliably when available (contrast to msync()) and doesn't flush out
     * clean data (like FADV_DONTNEED).
     */
    rc = sync_file_range(fd, offset, nbytes, SYNC_FILE_RANGE_WRITE);
    if (rc != 0) {
      int elevel;

      if (errno == EINTR)
        goto retry;

      /*
       * For systems that don't have an implementation of
       * sync_file_range() such as Windows WSL, generate only one
       * warning and then suppress all further attempts by this process.
       */
      if (errno == ENOSYS) {
        elevel = WARNING;
        not_implemented_by_kernel = true;
      } else
        elevel = data_sync_elevel(WARNING);

      ereport(elevel, (errcode_for_file_access(),
                       errmsg("could not flush dirty data: %m")));
    }

    return;
  }
#endif
#if !defined(WIN32) && defined(MS_ASYNC)
  {
    void *p;
    static thread_local /*LOCAL*/ int pagesize = 0;

    /*
     * On several OSs msync(MS_ASYNC) on a mmap'ed file triggers
     * writeback. On linux it only does so if MS_SYNC is specified, but
     * then it does the writeback synchronously. Luckily all common linux
     * systems have sync_file_range().  This is preferable over
     * FADV_DONTNEED because it doesn't flush out clean data.
     *
     * We map the file (mmap()), tell the kernel to sync back the contents
     * (msync()), and then remove the mapping again (munmap()).
     */

    /* mmap() needs actual length if we want to map whole file */
    if (offset == 0 && nbytes == 0) {
      nbytes = lseek(fd, 0, SEEK_END);
      if (nbytes < 0) {
        ereport(WARNING, (errcode_for_file_access(),
                          errmsg("could not determine dirty data size: %m")));
        return;
      }
    }

    /*
     * Some platforms reject partial-page mmap() attempts.  To deal with
     * that, just truncate the request to a page boundary.  If any extra
     * bytes don't get flushed, well, it's only a hint anyway.
     */

    /* fetch pagesize only once */
    if (pagesize == 0)
      pagesize = sysconf(_SC_PAGESIZE);

    /* align length to pagesize, dropping any fractional page */
    if (pagesize > 0)
      nbytes = (nbytes / pagesize) * pagesize;

    /* fractional-page request is a no-op */
    if (nbytes <= 0)
      return;

    /*
     * mmap could well fail, particularly on 32-bit platforms where there
     * may simply not be enough address space.  If so, silently fall
     * through to the next implementation.
     */
    if (nbytes <= (off_t)SSIZE_MAX)
      p = mmap(NULL, nbytes, PROT_READ, MAP_SHARED, fd, offset);
    else
      p = MAP_FAILED;

    if (p != MAP_FAILED) {
      int rc;

      rc = msync(p, (size_t)nbytes, MS_ASYNC);
      if (rc != 0) {
        ereport(data_sync_elevel(WARNING),
                (errcode_for_file_access(),
                 errmsg("could not flush dirty data: %m")));
        /* NB: need to fall through to munmap()! */
      }

      rc = munmap(p, (size_t)nbytes);
      if (rc != 0) {
        /* FATAL error because mapping would remain */
        ereport(FATAL, (errcode_for_file_access(),
                        errmsg("could not munmap() while flushing data: %m")));
      }

      return;
    }
  }
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
  {
    int rc;

    /*
     * Signal the kernel that the passed in range should not be cached
     * anymore. This has the, desired, side effect of writing out dirty
     * data, and the, undesired, side effect of likely discarding useful
     * clean cached blocks.  For the latter reason this is the least
     * preferable method.
     */

    rc = posix_fadvise(fd, offset, nbytes, POSIX_FADV_DONTNEED);

    if (rc != 0) {
      /* don't error out, this is just a performance optimization */
      ereport(WARNING, (errcode_for_file_access(),
                        errmsg("could not flush dirty data: %m")));
    }

    return;
  }
#endif
}

/*
 * fsync_fname -- fsync a file or directory, handling errors properly
 *
 * Try to fsync a file or directory. When doing the latter, ignore errors that
 * indicate the OS just doesn't allow/require fsyncing directories.
 */
void fsync_fname(const char *fname, bool isdir) {
  fsync_fname_ext(fname, isdir, false, data_sync_elevel(ERROR));
}

/*
 * durable_rename -- rename(2) wrapper, issuing fsyncs required for durability
 *
 * This routine ensures that, after returning, the effect of renaming file
 * persists in case of a crash. A crash while this routine is running will
 * leave you with either the pre-existing or the moved file in place of the
 * new file; no mixed state or truncated files are possible.
 *
 * It does so by using fsync on the old filename and the possibly existing
 * target filename before the rename, and the target file and directory after.
 *
 * Note that rename() cannot be used across arbitrary directories, as they
 * might not be on the same filesystem. Therefore this routine does not
 * support renaming across directories.
 *
 * Log errors with the caller specified severity.
 *
 * Returns 0 if the operation succeeded, -1 otherwise. Note that errno is not
 * valid upon return.
 */
int durable_rename(const char *oldfile, const char *newfile, int elevel) {
  int fd;

  /*
   * First fsync the old and target path (if it exists), to ensure that they
   * are properly persistent on disk. Syncing the target file is not
   * strictly necessary, but it makes it easier to reason about crashes;
   * because it's then guaranteed that either source or target file exists
   * after a crash.
   */
  if (fsync_fname_ext(oldfile, false, false, elevel) != 0)
    return -1;

  fd = OpenTransientFile(newfile, PG_BINARY | O_RDWR);
  if (fd < 0) {
    if (errno != ENOENT) {
      ereport(elevel, (errcode_for_file_access(),
                       errmsg("could not open file \"%s\": %m", newfile)));
      return -1;
    }
  } else {
    if (pg_fsync(fd) != 0) {
      int save_errno;

      /* close file upon error, might not be in transaction context */
      save_errno = errno;
      CloseTransientFile(fd);
      errno = save_errno;

      ereport(elevel, (errcode_for_file_access(),
                       errmsg("could not fsync file \"%s\": %m", newfile)));
      return -1;
    }

    if (CloseTransientFile(fd) != 0) {
      ereport(elevel, (errcode_for_file_access(),
                       errmsg("could not close file \"%s\": %m", newfile)));
      return -1;
    }
  }

  /* Time to do the real deal... */
  if (rename(oldfile, newfile) < 0) {
    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not rename file \"%s\" to \"%s\": %m",
                            oldfile, newfile)));
    return -1;
  }

  /*
   * To guarantee renaming the file is persistent, fsync the file with its
   * new name, and its containing directory.
   */
  if (fsync_fname_ext(newfile, false, false, elevel) != 0)
    return -1;

  if (fsync_parent_path(newfile, elevel) != 0)
    return -1;

  return 0;
}

/*
 * InitFileAccess --- initialize this module during backend startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * Note that this does not initialize temporary file access, that is
 * separately initialized via InitTemporaryFileAccess().
 */
void InitFileAccess(void) {
  Assert(SizeVfdCache == 0); /* call me only once */

  /* initialize cache header entry */
  VfdCache = (Vfd *)malloc(sizeof(Vfd));
  if (VfdCache == NULL)
    ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));

  MemSet(&(VfdCache[0]), 0, sizeof(Vfd));

  SizeVfdCache = 1;
}

/*
 * InitTemporaryFileAccess --- initialize temporary file access during startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 *
 * This is separate from InitFileAccess() because temporary file cleanup can
 * cause pgstat reporting. As pgstat is shut down during before_shmem_exit(),
 * our reporting has to happen before that. Low level file access should be
 * available for longer, hence the separate initialization / shutdown of
 * temporary file handling.
 */
void InitTemporaryFileAccess(void) {
  Assert(SizeVfdCache != 0);        /* InitFileAccess() needs to have run */
  Assert(!temporary_files_allowed); /* call me only once */

  /*
   * Register before-shmem-exit hook to ensure temp files are dropped while
   * we can still report stats.
   */
  before_shmem_exit(BeforeShmemExit_Files, 0);

#ifdef USE_ASSERT_CHECKING
  temporary_files_allowed = true;
#endif
}

/*
 * Raise the process's soft limit on open files to its hard limit. Every
 * session is a thread of this one process, so their files, RocksDB's and
 * the clients' sockets all count against the one limit.
 */
static void raise_open_files_limit(void) {
#ifdef HAVE_GETRLIMIT
  struct rlimit rlim;
  rlim_t soft;

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    ereport(WARNING, (errmsg("getrlimit failed: %m")));
    return;
  }
  if (rlim.rlim_cur >= rlim.rlim_max)
    return;
  soft = rlim.rlim_cur;
  rlim.rlim_cur = rlim.rlim_max;
  /* Raised once, at start, before any thread or file of ours is open. */
  if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    ereport(WARNING,
            (errmsg("could not raise the limit on open files from %lu to "
                    "%lu: %m",
                    (unsigned long)soft, (unsigned long)rlim.rlim_max)));
    return;
  }
  elog(DEBUG1, "raised the limit on open files from %lu to %lu",
       (unsigned long)soft, (unsigned long)rlim.rlim_max);
#endif
}

/*
 * set_max_safe_fds
 *		Determine number of file descriptors that fd.c is allowed to use
 *
 * Every thread of the process shares one ceiling, sm0's: the open-files
 * limit less the fds open now and NUM_RESERVED_FDS.  A thread's share of it,
 * max_safe_fds_per_thread, is the ceiling over the threads there can be, but
 * no more
 * than max_files_per_process, and no less than FD_MINFREE: the ceiling, not
 * the share, keeps the threads together within the limit.
 */
void set_max_safe_fds(void) {
  int threads = pcb->MaxBackends + 1; /* the sessions, and the postmaster */
  int ceiling;

  raise_open_files_limit();

  /* The ceiling all the threads' fds count against, measured once. */
  if (sm_fd_ceiling_set(0, NUM_RESERVED_FDS) != 0)
    ereport(FATAL, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("could not count the file descriptors available: %s",
                           sm_errmsg())));
  ceiling = sm_fd_ceiling();

  /*
   * Make sure we still have enough to get by.
   */
  if (ceiling < FD_MINFREE)
    ereport(
        FATAL,
        (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
         errmsg(
             "insufficient file descriptors available to start server process"),
         errdetail("System allows %d, server needs at least %d.",
                   ceiling + NUM_RESERVED_FDS, FD_MINFREE + NUM_RESERVED_FDS)));

  pcb_main()->max_safe_fds_per_thread =
      Max(Min(ceiling / threads, pcb->max_files_per_process), FD_MINFREE);

  elog(DEBUG2, "max_safe_fds_per_thread = %d, ceiling = %d, threads = %d",
       pcb->max_safe_fds_per_thread, ceiling, threads);
}

/*
 * AcquireExternalFD - attempt to reserve an external file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.
 *
 * The difference between this and the underlying ReserveExternalFD function
 * is that this will report failure (by setting errno and returning false)
 * if "too many" external FDs are already reserved.  This should be used in
 * any code where the total number of FDs to be reserved is not predictable
 * and small.
 */
bool AcquireExternalFD(void) {
  /* Counted within this thread's share and the ceiling, or refused. */
  return sm_vfd_reserve(vfd_cache(), 1) == 0;
}

/*
 * ReserveExternalFD - report external consumption of a file descriptor
 *
 * This should be used by callers that need to hold a file descriptor open
 * over more than a short interval, but cannot use any of the other facilities
 * provided by this module.  This just tracks the use of the FD and closes
 * VFDs if needed to make room for it; it is counted even when no room is
 * left, as it is open.
 *
 * Call this directly only in code where failure to reserve the FD would be
 * fatal; for example, the WAL-writing code does so, since the alternative is
 * session failure.  Also, it's very unwise to do so in code that could
 * consume more than one FD per process.
 *
 * Note: as long as everybody plays nice so that NUM_RESERVED_FDS FDs remain
 * available, it doesn't matter too much whether this is called before or
 * after actually opening the FD; but doing so beforehand reduces the risk of
 * an EMFILE failure if not everybody played nice.  In any case, it's solely
 * caller's responsibility to keep the external-FD count in sync with reality.
 */
void ReserveExternalFD(void) { sm_vfd_count(vfd_cache(), 1); }

/*
 * ReleaseExternalFD - report release of an external file descriptor
 *
 * This is guaranteed not to change errno, so it can be used in failure paths.
 */
void ReleaseExternalFD(void) {
  Assert(tcb->vfd != nullptr && sm_vfd_cache_held(tcb->vfd.get()) > 0);
  sm_vfd_unreserve(tcb->vfd.get(), 1);
}

static File AllocateVfd(void) {
  Index i;
  File file;

  DO_DB(elog(LOG, "AllocateVfd. Size %zu", SizeVfdCache));

  Assert(SizeVfdCache > 0); /* InitFileAccess not called? */

  if (VfdCache[0].nextFree == 0) {
    /*
     * The free list is empty so it is time to increase the size of the
     * array.  We choose to double it each time this happens. However,
     * there's not much point in starting *real* small.
     */
    Size newCacheSize = SizeVfdCache * 2;
    Vfd *newVfdCache;

    if (newCacheSize < 32)
      newCacheSize = 32;

    /*
     * Be careful not to clobber VfdCache ptr if realloc fails.
     */
    newVfdCache = (Vfd *)realloc(VfdCache, sizeof(Vfd) * newCacheSize);
    if (newVfdCache == NULL)
      ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    VfdCache = newVfdCache;

    /*
     * Initialize the new entries and link them into the free list.
     */
    for (i = SizeVfdCache; i < newCacheSize; i++) {
      MemSet(&(VfdCache[i]), 0, sizeof(Vfd));
      VfdCache[i].nextFree = i + 1;
    }
    VfdCache[newCacheSize - 1].nextFree = 0;
    VfdCache[0].nextFree = SizeVfdCache;

    /*
     * Record the new size
     */
    SizeVfdCache = newCacheSize;
  }

  file = VfdCache[0].nextFree;

  VfdCache[0].nextFree = VfdCache[file].nextFree;

  return file;
}

static void FreeVfd(File file) {
  Vfd *vfdP = &VfdCache[file];

  DO_DB(elog(LOG, "FreeVfd: %d (%s)", file,
             vfdP->fileName ? vfdP->fileName : ""));

  if (vfdP->fileName != NULL) {
    free(vfdP->fileName);
    vfdP->fileName = NULL;
  }
  vfdP->fdstate = 0x0;
  vfdP->vfd = 0;

  vfdP->nextFree = VfdCache[0].nextFree;
  VfdCache[0].nextFree = file;
}

/*
 * Called whenever a temporary file is deleted to report its size.
 */
static void ReportTemporaryFileUsage(const char *path, off_t size) {

  if (log_temp_files >= 0) {
    if ((size / 1024) >= log_temp_files)
      ereport(LOG, (errmsg("temporary file: path \"%s\", size %lu", path,
                           (unsigned long)size)));
  }
}

/*
 * Called to register a temporary file for automatic close.
 * ResourceOwnerEnlarge(CurrentResourceOwner) must have been called
 * before the file was opened.
 */
static void RegisterTemporaryFile(File file) {
  ResourceOwnerRememberFile(CurrentResourceOwner, file);
  VfdCache[file].resowner = CurrentResourceOwner;

  /* Backup mechanism for closing at end of xact. */
  VfdCache[file].fdstate |= FD_CLOSE_AT_EOXACT;
  have_xact_temporary_files = true;
}

/*
 * Open a file with PathNameOpenFilePerm() and pass default file mode for the
 * fileMode parameter.
 */
File PathNameOpenFile(const char *fileName, int fileFlags) {
  return PathNameOpenFilePerm(fileName, fileFlags, pcb->pg_file_create_mode);
}

/*
 * open a file in an arbitrary directory
 *
 * NB: if the passed pathname is relative (which it usually is),
 * it will be interpreted relative to the process' working directory
 * (which should always be $PGDATA when this code is running).
 */
File PathNameOpenFilePerm(const char *fileName, int fileFlags,
                          mode_t fileMode) {
  char *fnamecopy;
  File file;
  Vfd *vfdP;

  DO_DB(elog(LOG, "PathNameOpenFilePerm: %s %x %o", fileName, fileFlags,
             fileMode));

  /*
   * We need a malloc'd copy of the file name; fail cleanly if no room.
   */
  fnamecopy = strdup(fileName);
  if (fnamecopy == NULL)
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));

  file = AllocateVfd();
  vfdP = &VfdCache[file];

  /*
   * Descriptors managed by VFDs are implicitly marked O_CLOEXEC.  The
   * client shouldn't be expected to know which kernel descriptors are
   * currently open, so it wouldn't make sense for them to be inherited by
   * executed subprograms.
   */
  fileFlags |= O_CLOEXEC;

  /* sm0 closes the thread's least recently used fds to make room. */
  vfdP->vfd = sm_vfd_open(vfd_cache(), fileName, fileFlags, fileMode);

  if (vfdP->vfd < 0) {
    int save_errno = errno;

    FreeVfd(file);
    free(fnamecopy);
    errno = save_errno;
    return -1;
  }
  DO_DB(elog(LOG, "PathNameOpenFile: success %d", vfdP->vfd));

  vfdP->fileName = fnamecopy;
  vfdP->fileSize = 0;
  vfdP->fdstate = 0x0;
  vfdP->resowner = NULL;

  return file;
}

/*
 * Open a temporary file that will disappear when we close it.
 *
 * This routine takes care of generating an appropriate tempfile name.
 * There's no need to pass in fileFlags or fileMode either, since only
 * one setting makes any sense for a temp file.
 *
 * Unless interXact is true, the file is remembered by CurrentResourceOwner
 * to ensure it's closed and deleted when it's no longer needed, typically at
 * the end-of-transaction. In most cases, you don't want temporary files to
 * outlive the transaction that created them, so this should be false -- but
 * if you need "somewhat" temporary storage, this might be useful. In either
 * case, the file is removed when the File is explicitly closed.
 */
File OpenTemporaryFile(bool interXact) {
  File file = 0;

  Assert(temporary_files_allowed); /* check temp file access is up */

  /*
   * Make sure the current resource owner has space for this File before we
   * open it, if we'll be registering it below.
   */
  if (!interXact)
    ResourceOwnerEnlarge(CurrentResourceOwner);

  /*
   * Create it in the database's default tablespace, there being no others.
   * MyDatabaseTableSpace should normally be set before we get here, but
   * just in case it isn't, fall back to pg_default tablespace.
   */
  file = OpenTemporaryFileInTablespace(
      MyDatabaseTableSpace ? MyDatabaseTableSpace : DEFAULTTABLESPACE_OID,
      true);

  /* Mark it for deletion at close and temporary file size limit */
  VfdCache[file].fdstate |= FD_DELETE_AT_CLOSE | FD_TEMP_FILE_LIMIT;

  /* Register it with the current resource owner */
  if (!interXact)
    RegisterTemporaryFile(file);

  return file;
}

/*
 * Return the path of the temp directory in a given tablespace.
 */
void TempTablespacePath(char *path, Oid tablespace) {
  /*
   * Identify the tempfile directory for this tablespace.
   *
   * If someone tries to specify pg_global, use pg_default instead.
   */
  if (tablespace == InvalidOid || tablespace == DEFAULTTABLESPACE_OID ||
      tablespace == GLOBALTABLESPACE_OID)
    snprintf(path, MAXPGPATH, "base/%s", PG_TEMP_FILES_DIR);
  else {
    /* All other tablespaces are accessed via symlinks */
    snprintf(path, MAXPGPATH, "%s/%u/%s/%s", PG_TBLSPC_DIR, tablespace,
             TABLESPACE_VERSION_DIRECTORY, PG_TEMP_FILES_DIR);
  }
}

/*
 * Open a temporary file in a specific tablespace.
 * Subroutine for OpenTemporaryFile, which see for details.
 */
static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError) {
  char tempdirpath[MAXPGPATH];
  char tempfilepath[MAXPGPATH];
  File file;

  TempTablespacePath(tempdirpath, tblspcOid);

  /*
   * Generate a tempfile name that should be unique within the current
   * database instance.
   */
  snprintf(tempfilepath, sizeof(tempfilepath), "%s/%s%d.%ld", tempdirpath,
           PG_TEMP_FILE_PREFIX, MyProcPid, tempFileCounter++);

  /*
   * Open the file.  Note: we don't use O_EXCL, in case there is an orphaned
   * temp file that can be reused.
   */
  file = PathNameOpenFile(tempfilepath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
  if (file <= 0) {
    /*
     * We might need to create the tablespace's tempfile directory, if no
     * one has yet done so.
     *
     * Don't check for an error from MakePGDirectory; it could fail if
     * someone else just did the same thing.  If it doesn't work then
     * we'll bomb out on the second create attempt, instead.
     */
    (void)MakePGDirectory(tempdirpath);

    file =
        PathNameOpenFile(tempfilepath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
    if (file <= 0 && rejectError)
      elog(ERROR, "could not create temporary file \"%s\": %m", tempfilepath);
  }

  return file;
}

/*
 * close a file when done with it
 */
void FileClose(File file) {
  Vfd *vfdP;

  Assert(FileIsValid(file));

  DO_DB(elog(LOG, "FileClose: %d (%s)", file, VfdCache[file].fileName));

  vfdP = &VfdCache[file];

  /* close the file, and its real fd if it has one */
  if (sm_vfd_close(vfd_cache(), vfdP->vfd) != 0) {
    /* We may need to panic on failure to close non-temporary files. */
    elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
         "could not close file \"%s\": %m", vfdP->fileName);
  }
  vfdP->vfd = 0;

  if (vfdP->fdstate & FD_TEMP_FILE_LIMIT) {
    /* Subtract its size from current usage (do first in case of error) */
    temporary_files_size -= vfdP->fileSize;
    vfdP->fileSize = 0;
  }

  /*
   * Delete the file if it was temporary, and make a log entry if wanted
   */
  if (vfdP->fdstate & FD_DELETE_AT_CLOSE) {
    struct stat filestats;
    int stat_errno;

    /*
     * If we get an error, as could happen within the ereport/elog calls,
     * we'll come right back here during transaction abort.  Reset the
     * flag to ensure that we can't get into an infinite loop.  This code
     * is arranged to ensure that the worst-case consequence is failing to
     * emit log message(s), not failing to attempt the unlink.
     */
    vfdP->fdstate &= ~FD_DELETE_AT_CLOSE;

    /* first try the stat() */
    if (stat(vfdP->fileName, &filestats))
      stat_errno = errno;
    else
      stat_errno = 0;

    /* in any case do the unlink */
    if (unlink(vfdP->fileName))
      ereport(LOG,
              (errcode_for_file_access(),
               errmsg("could not delete file \"%s\": %m", vfdP->fileName)));

    /* and last report the stat results */
    if (stat_errno == 0)
      ReportTemporaryFileUsage(vfdP->fileName, filestats.st_size);
    else {
      errno = stat_errno;
      ereport(LOG, (errcode_for_file_access(),
                    errmsg("could not stat file \"%s\": %m", vfdP->fileName)));
    }
  }

  /* Unregister it from the resource owner */
  if (vfdP->resowner)
    ResourceOwnerForgetFile(vfdP->resowner, file);

  /*
   * Return the Vfd slot to the free list
   */
  FreeVfd(file);
}

ssize_t FileReadV(File file, const struct iovec *iov, int iovcnt, off_t offset,
                  uint32 wait_event_info) {
  Assert(FileIsValid(file));

  DO_DB(elog(LOG, "FileReadV: %d (%s) " INT64_FORMAT " %d", file,
             VfdCache[file].fileName, (int64)offset, iovcnt));

  /* sm0 reopens the file if it was closed, and retries if interrupted. */
  return sm_vfd_readv(vfd_cache(), VfdCache[file].vfd, iov, iovcnt, offset);
}

ssize_t FileWriteV(File file, const struct iovec *iov, int iovcnt, off_t offset,
                   uint32 wait_event_info) {
  ssize_t returnCode;
  Vfd *vfdP;

  Assert(FileIsValid(file));

  DO_DB(elog(LOG, "FileWriteV: %d (%s) " INT64_FORMAT " %d", file,
             VfdCache[file].fileName, (int64)offset, iovcnt));

  vfdP = &VfdCache[file];

  /*
   * If enforcing temp_file_limit and it's a temp file, check to see if the
   * write would overrun temp_file_limit, and throw error if so.  Note: it's
   * really a modularity violation to throw error here; we should set errno
   * and return -1.  However, there's no way to report a suitable error
   * message if we do that.  All current callers would just throw error
   * immediately anyway, so this is safe at present.
   */
  if (temp_file_limit >= 0 && (vfdP->fdstate & FD_TEMP_FILE_LIMIT)) {
    off_t past_write = offset;

    for (int i = 0; i < iovcnt; ++i)
      past_write += iov[i].iov_len;

    if (past_write > vfdP->fileSize) {
      uint64 newTotal = temporary_files_size;

      newTotal += past_write - vfdP->fileSize;
      if (newTotal > (uint64)temp_file_limit * (uint64)1024)
        ereport(
            ERROR,
            (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
             errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)",
                    temp_file_limit)));
    }
  }

  /* sm0 reopens the file if it was closed, and retries if interrupted. */
  returnCode = sm_vfd_writev(vfd_cache(), vfdP->vfd, iov, iovcnt, offset);

  if (returnCode >= 0) {
    /*
     * Some callers expect short writes to set errno, and traditionally we
     * have assumed that they imply disk space shortage.  We don't want to
     * waste CPU cycles adding up the total size here, so we'll just set
     * it for all successful writes in case such a caller determines that
     * the write was short and ereports "%m".
     */
    errno = ENOSPC;

    /*
     * Maintain fileSize and temporary_files_size if it's a temp file.
     */
    if (vfdP->fdstate & FD_TEMP_FILE_LIMIT) {
      off_t past_write = offset + returnCode;

      if (past_write > vfdP->fileSize) {
        temporary_files_size += past_write - vfdP->fileSize;
        vfdP->fileSize = past_write;
      }
    }
  }

  return returnCode;
}

off_t FileSize(File file) {
  Assert(FileIsValid(file));

  DO_DB(elog(LOG, "FileSize %d (%s)", file, VfdCache[file].fileName));

  return sm_vfd_size(vfd_cache(), VfdCache[file].vfd);
}

/*
 * Return the pathname associated with an open file.
 *
 * The returned string points to an internal buffer, which is valid until
 * the file is closed.
 */
char *FilePathName(File file) {
  Assert(FileIsValid(file));

  return VfdCache[file].fileName;
}

/* Stop counting the fd a failed fopen(), popen() or opendir() did not take. */
static void unreserve_saving_errno(void) {
  int save_errno = errno;

  sm_vfd_unreserve(vfd_cache(), 1);
  errno = save_errno;
}

/*
 * Make room for another allocatedDescs[] array entry if needed and possible.
 * Returns true if an array element is available.
 */
static bool reserveAllocatedDesc(void) {
  AllocateDesc *newDescs;
  int newMax;

  /* Quick out if array already has a free slot. */
  if (numAllocatedDescs < maxAllocatedDescs)
    return true;

  /*
   * If the array hasn't yet been created in the current process, initialize
   * it with FD_MINFREE / 3 elements.  In many scenarios this is as many as
   * we will ever need, anyway.  We don't want to look at
   * max_safe_fds_per_thread
   * immediately because set_max_safe_fds() may not have run yet.
   */
  if (allocatedDescs == NULL) {
    newMax = FD_MINFREE / 3;
    newDescs = (AllocateDesc *)malloc(newMax * sizeof(AllocateDesc));
    /* Out of memory already?  Treat as fatal error. */
    if (newDescs == NULL)
      ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    allocatedDescs = newDescs;
    maxAllocatedDescs = newMax;
    return true;
  }

  /*
   * Consider enlarging the array beyond the initial allocation used above.
   * By the time this happens, max_safe_fds_per_thread should be known
   * accurately.
   *
   * We mustn't let allocated descriptors hog all the available FDs, and in
   * practice we'd better leave a reasonable number of FDs for VFD use.  So
   * set the maximum to max_safe_fds_per_thread / 3.  (This should certainly
   * be at
   * least as large as the initial size, FD_MINFREE / 3, so we aren't
   * tightening the restriction here.)  Recall that "external" FDs are
   * allowed to consume another third of max_safe_fds_per_thread.
   */
  newMax = pcb->max_safe_fds_per_thread / 3;
  if (newMax > maxAllocatedDescs) {
    newDescs =
        (AllocateDesc *)realloc(allocatedDescs, newMax * sizeof(AllocateDesc));
    /* Treat out-of-memory as a non-fatal error. */
    if (newDescs == NULL)
      return false;
    allocatedDescs = newDescs;
    maxAllocatedDescs = newMax;
    return true;
  }

  /* Can't enlarge allocatedDescs[] any more. */
  return false;
}

/*
 * Routines that want to use stdio (ie, FILE*) should use AllocateFile
 * rather than plain fopen().  This lets fd.c deal with freeing FDs if
 * necessary to open the file.  When done, call FreeFile rather than fclose.
 *
 * Note that files that will be open for any significant length of time
 * should NOT be handled this way, since they cannot share kernel file
 * descriptors with other files; there is grave risk of running out of FDs
 * if anyone locks down too many FDs.  Most callers of this routine are
 * simply reading a config file that they will read and close immediately.
 *
 * fd.c will automatically close all files opened with AllocateFile at
 * transaction commit or abort; this prevents FD leakage if a routine
 * that calls AllocateFile is terminated prematurely by ereport(ERROR).
 *
 * Ideally this should be the *only* direct call of fopen() in the backend.
 */
FILE *AllocateFile(const char *name, const char *mode) {
  FILE *file;

  DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)", numAllocatedDescs, name));

  /* Can we allocate another non-virtual FD? */
  if (!reserveAllocatedDesc())
    ereport(
        ERROR,
        (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
         errmsg(
             "exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
             maxAllocatedDescs, name)));

  /* Count the fd fopen() takes; sm0 closes VFDs to make room. */
  if (sm_vfd_reserve(vfd_cache(), 1) != 0)
    return NULL;

  if ((file = fopen(name, mode)) != NULL) {
    AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

    desc->kind = AllocateDescFile;
    desc->desc.file = file;
    desc->create_subid = GetCurrentSubTransactionId();
    numAllocatedDescs++;
    return desc->desc.file;
  }

  unreserve_saving_errno();
  return NULL;
}

/*
 * Open a file with OpenTransientFilePerm() and pass default file mode for
 * the fileMode parameter.
 */
int OpenTransientFile(const char *fileName, int fileFlags) {
  return OpenTransientFilePerm(fileName, fileFlags, pcb->pg_file_create_mode);
}

/*
 * Like AllocateFile, but returns an unbuffered fd like open(2)
 */
int OpenTransientFilePerm(const char *fileName, int fileFlags,
                          mode_t fileMode) {
  int fd;

  DO_DB(elog(LOG, "OpenTransientFile: Allocated %d (%s)", numAllocatedDescs,
             fileName));

  /* Can we allocate another non-virtual FD? */
  if (!reserveAllocatedDesc())
    ereport(
        ERROR,
        (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
         errmsg(
             "exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
             maxAllocatedDescs, fileName)));

  /* Counted, as a VFD is; sm0 closes VFDs to make room. */
  fd = sm_vfd_open_real(vfd_cache(), fileName, fileFlags, fileMode);

  if (fd >= 0) {
    AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

    desc->kind = AllocateDescRawFD;
    desc->desc.fd = fd;
    desc->create_subid = GetCurrentSubTransactionId();
    numAllocatedDescs++;

    return fd;
  }

  return -1; /* failure */
}

/*
 * Run "sh -c command" with a pipe from its stdout (mode "r") or to its stdin
 * ("w"): our end as a stream, and the program's pid in *pid; NULL, with
 * errno set, on failure.  popen() would hand the program this thread's
 * signal mask, which blocks every signal but the faults, and the process's
 * ignored signals, SIGPIPE among them, so a kill would not stop it and a
 * writer whose reader had gone would not end quietly.  It starts instead as
 * a PostgreSQL backend's program does: nothing blocked, and every signal at
 * its default but SIGTTIN, SIGTTOU and SIGXFSZ, which the postmaster ignores
 * for itself and its children.  Nothing of the process's changes, so no
 * other thread sees a signal handled otherwise meanwhile.
 */
static FILE *spawn_pipe(const char *command, const char *mode, pid_t *pid) {
  bool reading = mode[0] == 'r';
  char *argv[] = {(char *)"sh", (char *)"-c", (char *)command, NULL};
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  sigset_t mask;
  sigset_t defaults;
  int fds[2];
  int ours;
  int theirs;
  int rc;
  FILE *file;

  /* Both ends close on exec: the program has its end only as dup2() puts it. */
  if (pipe2(fds, O_CLOEXEC) != 0)
    return NULL;
  ours = reading ? fds[0] : fds[1];
  theirs = reading ? fds[1] : fds[0];

  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, theirs,
                                   reading ? STDOUT_FILENO : STDIN_FILENO);
  sigemptyset(&mask);
  sigfillset(&defaults);
  sigdelset(&defaults, SIGTTIN);
  sigdelset(&defaults, SIGTTOU);
  sigdelset(&defaults, SIGXFSZ);
  posix_spawnattr_init(&attr);
  posix_spawnattr_setsigmask(&attr, &mask);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setflags(&attr,
                           POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  rc = posix_spawn(pid, "/bin/sh", &actions, &attr, argv, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(theirs);
  if (rc != 0) {
    close(ours);
    errno = rc;
    return NULL;
  }

  file = fdopen(ours, reading ? "r" : "w");
  if (file == NULL) {
    int save_errno = errno;

    close(ours);
    while (waitpid(*pid, NULL, 0) < 0 && errno == EINTR)
      ;
    errno = save_errno;
  }
  return file;
}

/*
 * Close a stream spawn_pipe() made, then wait for its program: the program's
 * wait status, as pclose() gives it, or -1 with errno set.  Our end closes
 * first, so the program sees its input end, or its output's reader gone.
 */
static int close_pipe(FILE *file, pid_t pid) {
  int status;

  fclose(file);
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  return status;
}

/*
 * Routines that want to initiate a pipe stream should use OpenPipeStream
 * rather than plain popen().  This lets fd.c deal with freeing FDs if
 * necessary.  When done, call ClosePipeStream rather than pclose.
 *
 * The program is started with a signal state of its own (spawn_pipe()).
 */
FILE *OpenPipeStream(const char *command, const char *mode) {
  FILE *file;
  pid_t pid;

  DO_DB(elog(LOG, "OpenPipeStream: Allocated %d (%s)", numAllocatedDescs,
             command));

  /* Can we allocate another non-virtual FD? */
  if (!reserveAllocatedDesc())
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("exceeded maxAllocatedDescs (%d) while trying to "
                           "execute command \"%s\"",
                           maxAllocatedDescs, command)));

  /* Count the pipe's fd; sm0 closes VFDs to make room. */
  if (sm_vfd_reserve(vfd_cache(), 1) != 0)
    return NULL;

  file = spawn_pipe(command, mode, &pid);
  if (file != NULL) {
    AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

    desc->kind = AllocateDescPipe;
    desc->desc.file = file;
    desc->pid = pid;
    desc->create_subid = GetCurrentSubTransactionId();
    numAllocatedDescs++;
    return desc->desc.file;
  }

  unreserve_saving_errno();
  return NULL;
}

/*
 * Free an AllocateDesc of any type.
 *
 * The argument *must* point into the allocatedDescs[] array.
 */
static int FreeDesc(AllocateDesc *desc) {
  int result;

  /* Close the underlying object, and stop counting its fd */
  switch (desc->kind) {
  case AllocateDescFile:
    result = fclose(desc->desc.file);
    sm_vfd_unreserve(vfd_cache(), 1);
    break;
  case AllocateDescPipe:
    result = close_pipe(desc->desc.file, desc->pid);
    sm_vfd_unreserve(vfd_cache(), 1);
    break;
  case AllocateDescDir:
    result = closedir(desc->desc.dir);
    sm_vfd_unreserve(vfd_cache(), 1);
    break;
  case AllocateDescRawFD:
    result = sm_vfd_close_real(vfd_cache(), desc->desc.fd);
    break;
  default:
    elog(ERROR, "AllocateDesc kind not recognized");
    result = 0; /* keep compiler quiet */
    break;
  }

  /* Compact storage in the allocatedDescs array */
  numAllocatedDescs--;
  *desc = allocatedDescs[numAllocatedDescs];

  return result;
}

/*
 * Close a file returned by AllocateFile.
 *
 * Note we do not check fclose's return value --- it is up to the caller
 * to handle close errors.
 */
int FreeFile(FILE *file) {
  int i;

  DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

  /* Remove file from list of allocated files, if it's present */
  for (i = numAllocatedDescs; --i >= 0;) {
    AllocateDesc *desc = &allocatedDescs[i];

    if (desc->kind == AllocateDescFile && desc->desc.file == file)
      return FreeDesc(desc);
  }

  /* Only get here if someone passes us a file not in allocatedDescs */
  elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

  return fclose(file);
}

/*
 * Close a file returned by OpenTransientFile.
 *
 * Note we do not check close's return value --- it is up to the caller
 * to handle close errors.
 */
int CloseTransientFile(int fd) {
  int i;

  DO_DB(elog(LOG, "CloseTransientFile: Allocated %d", numAllocatedDescs));

  /* Remove fd from list of allocated files, if it's present */
  for (i = numAllocatedDescs; --i >= 0;) {
    AllocateDesc *desc = &allocatedDescs[i];

    if (desc->kind == AllocateDescRawFD && desc->desc.fd == fd)
      return FreeDesc(desc);
  }

  /* Only get here if someone passes us a file not in allocatedDescs */
  elog(WARNING, "fd passed to CloseTransientFile was not obtained from "
                "OpenTransientFile");

  return close(fd);
}

/*
 * Routines that want to use <dirent.h> (ie, DIR*) should use AllocateDir
 * rather than plain opendir().  This lets fd.c deal with freeing FDs if
 * necessary to open the directory, and with closing it after an elog.
 * When done, call FreeDir rather than closedir.
 *
 * Returns NULL, with errno set, on failure.  Note that failure detection
 * is commonly left to the following call of ReadDir or ReadDirExtended;
 * see the comments for ReadDir.
 *
 * Ideally this should be the *only* direct call of opendir() in the backend.
 */
DIR *AllocateDir(const char *dirname) {
  DIR *dir;

  DO_DB(
      elog(LOG, "AllocateDir: Allocated %d (%s)", numAllocatedDescs, dirname));

  /* Can we allocate another non-virtual FD? */
  if (!reserveAllocatedDesc())
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("exceeded maxAllocatedDescs (%d) while trying to "
                           "open directory \"%s\"",
                           maxAllocatedDescs, dirname)));

  /* Count the fd opendir() takes; sm0 closes VFDs to make room. */
  if (sm_vfd_reserve(vfd_cache(), 1) != 0)
    return NULL;

  if ((dir = opendir(dirname)) != NULL) {
    AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

    desc->kind = AllocateDescDir;
    desc->desc.dir = dir;
    desc->create_subid = GetCurrentSubTransactionId();
    numAllocatedDescs++;
    return desc->desc.dir;
  }

  unreserve_saving_errno();
  return NULL;
}

/*
 * Read a directory opened with AllocateDir, ereport'ing any error.
 *
 * This is easier to use than raw readdir() since it takes care of some
 * otherwise rather tedious and error-prone manipulation of errno.  Also,
 * if you are happy with a generic error message for AllocateDir failure,
 * you can just do
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			process dirent;
 *		FreeDir(dir);
 *
 * since a NULL dir parameter is taken as indicating AllocateDir failed.
 * (Make sure errno isn't changed between AllocateDir and ReadDir if you
 * use this shortcut.)
 *
 * The pathname passed to AllocateDir must be passed to this routine too,
 * but it is only used for error reporting.
 */
struct dirent *ReadDir(DIR *dir, const char *dirname) {
  return ReadDirExtended(dir, dirname, ERROR);
}

/*
 * Alternate version of ReadDir that allows caller to specify the elevel
 * for any error report (whether it's reporting an initial failure of
 * AllocateDir or a subsequent directory read failure).
 *
 * If elevel < ERROR, returns NULL after any error.  With the normal coding
 * pattern, this will result in falling out of the loop immediately as
 * though the directory contained no (more) entries.
 */
struct dirent *ReadDirExtended(DIR *dir, const char *dirname, int elevel) {
  struct dirent *dent;

  /* Give a generic message for AllocateDir failure, if caller didn't */
  if (dir == NULL) {
    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not open directory \"%s\": %m", dirname)));
    return NULL;
  }

  errno = 0;
  if ((dent = readdir(dir)) != NULL)
    return dent;

  if (errno)
    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not read directory \"%s\": %m", dirname)));
  return NULL;
}

/*
 * Close a directory opened with AllocateDir.
 *
 * Returns closedir's return value (with errno set if it's not 0).
 * Note we do not check the return value --- it is up to the caller
 * to handle close errors if wanted.
 *
 * Does nothing if dir == NULL; we assume that directory open failure was
 * already reported if desired.
 */
int FreeDir(DIR *dir) {
  int i;

  /* Nothing to do if AllocateDir failed */
  if (dir == NULL)
    return 0;

  DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

  /* Remove dir from list of allocated dirs, if it's present */
  for (i = numAllocatedDescs; --i >= 0;) {
    AllocateDesc *desc = &allocatedDescs[i];

    if (desc->kind == AllocateDescDir && desc->desc.dir == dir)
      return FreeDesc(desc);
  }

  /* Only get here if someone passes us a dir not in allocatedDescs */
  elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

  return closedir(dir);
}

/*
 * Close a pipe stream returned by OpenPipeStream.
 */
int ClosePipeStream(FILE *file) {
  int i;

  DO_DB(elog(LOG, "ClosePipeStream: Allocated %d", numAllocatedDescs));

  /* Remove file from list of allocated files, if it's present */
  for (i = numAllocatedDescs; --i >= 0;) {
    AllocateDesc *desc = &allocatedDescs[i];

    if (desc->kind == AllocateDescPipe && desc->desc.file == file)
      return FreeDesc(desc);
  }

  /* Only get here if someone passes us a file not in allocatedDescs */
  elog(WARNING,
       "file passed to ClosePipeStream was not obtained from OpenPipeStream");

  /* No program is known to wait for. */
  fclose(file);
  errno = ECHILD;
  return -1;
}

/*
 * Close every file and free the cache: a session thread, as it ends, whose
 * files no process exit closes.
 */
void ShutdownFileAccess(void) {
  for (Size i = 1; i < SizeVfdCache; i++)
    free(VfdCache[i].fileName);
  free(VfdCache);
  VfdCache = NULL;
  SizeVfdCache = 0;

  /* sm0's cache closes the real fds, and uncounts them from the ceiling. */
  tcb->vfd.reset();
}

/*
 * closeAllVfds
 *
 * Force all VFDs into the physically-closed state, so that the fewest
 * possible number of kernel file descriptors are in use.  There is no
 * change in the logical state of the VFDs.
 */
void closeAllVfds(void) {
  if (tcb->vfd != nullptr)
    sm_vfd_close_all(tcb->vfd.get());
}

/*
 * AtEOSubXact_Files
 *
 * Take care of subtransaction commit/abort.  At abort, we close temp files
 * that the subtransaction may have opened.  At commit, we reassign the
 * files that were opened to the parent subtransaction.
 */
void AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
                       SubTransactionId parentSubid) {
  Index i;

  for (i = 0; i < numAllocatedDescs; i++) {
    if (allocatedDescs[i].create_subid == mySubid) {
      if (isCommit)
        allocatedDescs[i].create_subid = parentSubid;
      else {
        /* have to recheck the item after FreeDesc (ugly) */
        FreeDesc(&allocatedDescs[i--]);
      }
    }
  }
}

/*
 * AtEOXact_Files
 *
 * This routine is called during transaction commit or abort.  All still-open
 * per-transaction temporary file VFDs are closed, which also causes the
 * underlying files to be deleted (although they should've been closed already
 * by the ResourceOwner cleanup). Furthermore, all "allocated" stdio files are
 * closed.
 *
 * The isCommit flag is used only to decide whether to emit warnings about
 * unclosed files.
 */
void AtEOXact_Files(bool isCommit) { CleanupTempFiles(isCommit, false); }

/*
 * BeforeShmemExit_Files
 *
 * before_shmem_exit hook to clean up temp files during backend shutdown.
 * Here, we want to clean up *all* temp files including interXact ones.
 */
static void BeforeShmemExit_Files(int code, Datum arg) {
  CleanupTempFiles(false, true);

  /* prevent further temp files from being created */
#ifdef USE_ASSERT_CHECKING
  temporary_files_allowed = false;
#endif
}

/*
 * Close temporary files and delete their underlying files.
 *
 * isCommit: if true, this is normal transaction commit, and we don't
 * expect any remaining files; warn if there are some.
 *
 * isProcExit: if true, this is being called as the backend process is
 * exiting. If that's the case, we should remove all temporary files; if
 * that's not the case, we are being called for transaction commit/abort
 * and should only remove transaction-local temp files.  In either case,
 * also clean up "allocated" stdio files, dirs and fds.
 */
static void CleanupTempFiles(bool isCommit, bool isProcExit) {
  Index i;

  /*
   * Careful here: at proc_exit we need extra cleanup, not just
   * xact_temporary files.
   */
  if (isProcExit || have_xact_temporary_files) {
    for (i = 1; i < SizeVfdCache; i++) {
      unsigned short fdstate = VfdCache[i].fdstate;

      if (((fdstate & FD_DELETE_AT_CLOSE) || (fdstate & FD_CLOSE_AT_EOXACT)) &&
          VfdCache[i].fileName != NULL) {
        /*
         * If we're in the process of exiting a backend process, close
         * all temporary files. Otherwise, only close temporary files
         * local to the current transaction. They should be closed by
         * the ResourceOwner mechanism already, so this is just a
         * debugging cross-check.
         */
        if (isProcExit)
          FileClose(i);
        else if (fdstate & FD_CLOSE_AT_EOXACT) {
          elog(WARNING, "temporary file %s not closed at end-of-transaction",
               VfdCache[i].fileName);
          FileClose(i);
        }
      }
    }

    have_xact_temporary_files = false;
  }

  /* Complain if any allocated files remain open at commit. */
  if (isCommit && numAllocatedDescs > 0)
    elog(WARNING,
         "%d temporary files and directories not closed at end-of-transaction",
         numAllocatedDescs);

  /* Clean up "allocated" stdio files, dirs and fds. */
  while (numAllocatedDescs > 0)
    FreeDesc(&allocatedDescs[0]);
}

/*
 * Remove the temporary files a prior postmaster session left, in
 * base/pgsql_tmp: OpenTemporaryFile()'s, as digres has no other tablespace
 * and no temporary relation files.
 *
 * This should be called during postmaster startup.  It reports a failed
 * syscall with ereport(LOG) and keeps going: removing temp files is not so
 * critical that we should fail to start the database when we can't do it.
 */
void RemovePgTempFiles(void) {
  char temp_path[MAXPGPATH];

  snprintf(temp_path, sizeof(temp_path), "base/%s", PG_TEMP_FILES_DIR);
  RemovePgTempFilesInDir(temp_path, true, false);
}

/*
 * Process one pgsql_tmp directory for RemovePgTempFiles.
 *
 * If missing_ok is true, it's all right for the named directory to not exist.
 * Any other problem results in a LOG message.  (missing_ok should be true at
 * the top level, since pgsql_tmp directories are not created until needed.)
 *
 * At the top level, this should be called with unlink_all = false, so that
 * only files matching the temporary name prefix will be unlinked.  When
 * recursing it will be called with unlink_all = true to unlink everything
 * under a top-level temporary directory.
 *
 * (These two flags could be replaced by one, but it seems clearer to keep
 * them separate.)
 */
void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
                            bool unlink_all) {
  DIR *temp_dir;
  struct dirent *temp_de;
  char rm_path[MAXPGPATH * 2];

  temp_dir = AllocateDir(tmpdirname);

  if (temp_dir == NULL && errno == ENOENT && missing_ok)
    return;

  while ((temp_de = ReadDirExtended(temp_dir, tmpdirname, LOG)) != NULL) {
    if (strcmp(temp_de->d_name, ".") == 0 || strcmp(temp_de->d_name, "..") == 0)
      continue;

    /*
     * sm keeps the rows of temporary tables here, in an instance it makes
     * anew at each attach (sm/TEMPTABLE.md section 2): it is sm's to remove.
     */
    if (!unlink_all && strcmp(temp_de->d_name, "sm") == 0)
      continue;

    snprintf(rm_path, sizeof(rm_path), "%s/%s", tmpdirname, temp_de->d_name);

    if (unlink_all || strncmp(temp_de->d_name, PG_TEMP_FILE_PREFIX,
                              strlen(PG_TEMP_FILE_PREFIX)) == 0) {
      PGFileType type = get_dirent_type(rm_path, temp_de, false, LOG);

      if (type == PGFILETYPE_ERROR)
        continue;
      else if (type == PGFILETYPE_DIR) {
        /* recursively remove contents, then directory itself */
        RemovePgTempFilesInDir(rm_path, false, true);

        if (rmdir(rm_path) < 0)
          ereport(LOG,
                  (errcode_for_file_access(),
                   errmsg("could not remove directory \"%s\": %m", rm_path)));
      } else {
        if (unlink(rm_path) < 0)
          ereport(LOG, (errcode_for_file_access(),
                        errmsg("could not remove file \"%s\": %m", rm_path)));
      }
    } else
      ereport(
          LOG,
          (errmsg("unexpected file found in temporary-files directory: \"%s\"",
                  rm_path)));
  }

  FreeDir(temp_dir);
}

#ifdef HAVE_SYNCFS
#endif

/*
 * Hint to the OS that it should get ready to fsync() this file.
 *
 * Ignores errors trying to open unreadable files, and logs other errors at a
 * caller-specified level.
 */
#ifdef PG_FLUSH_DATA_WORKS

#endif /* PG_FLUSH_DATA_WORKS */

/*
 * fsync_fname_ext -- Try to fsync a file or directory
 *
 * If ignore_perm is true, ignore errors upon trying to open unreadable
 * files. Logs other errors at a caller-specified level.
 *
 * Returns 0 if the operation succeeded, -1 otherwise.
 */
int fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm,
                    int elevel) {
  int fd;
  int flags;
  int returncode;

  /*
   * Some OSs require directories to be opened read-only whereas other
   * systems don't allow us to fsync files opened read-only; so we need both
   * cases here.  Using O_RDWR will cause us to fail to fsync files that are
   * not writable by our userid, but we assume that's OK.
   */
  flags = PG_BINARY;
  if (!isdir)
    flags |= O_RDWR;
  else
    flags |= O_RDONLY;

  fd = OpenTransientFile(fname, flags);

  /*
   * Some OSs don't allow us to open directories at all (Windows returns
   * EACCES), just ignore the error in that case.  If desired also silently
   * ignoring errors about unreadable files. Log others.
   */
  if (fd < 0 && isdir && (errno == EISDIR || errno == EACCES))
    return 0;
  else if (fd < 0 && ignore_perm && errno == EACCES)
    return 0;
  else if (fd < 0) {
    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not open file \"%s\": %m", fname)));
    return -1;
  }

  returncode = pg_fsync(fd);

  /*
   * Some OSes don't allow us to fsync directories at all, so we can ignore
   * those errors. Anything else needs to be logged.
   */
  if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL))) {
    int save_errno;

    /* close file upon error, might not be in transaction context */
    save_errno = errno;
    (void)CloseTransientFile(fd);
    errno = save_errno;

    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not fsync file \"%s\": %m", fname)));
    return -1;
  }

  if (CloseTransientFile(fd) != 0) {
    ereport(elevel, (errcode_for_file_access(),
                     errmsg("could not close file \"%s\": %m", fname)));
    return -1;
  }

  return 0;
}

/*
 * fsync_parent_path -- fsync the parent path of a file or directory
 *
 * This is aimed at making file operations persistent on disk in case of
 * an OS crash or power failure.
 */
static int fsync_parent_path(const char *fname, int elevel) {
  char parentpath[MAXPGPATH];

  strlcpy(parentpath, fname, MAXPGPATH);
  get_parent_directory(parentpath);

  /*
   * get_parent_directory() returns an empty string if the input argument is
   * just a file name (see comments in path.c), so handle that as being the
   * current directory.
   */
  if (strlen(parentpath) == 0)
    strlcpy(parentpath, ".", MAXPGPATH);

  if (fsync_fname_ext(parentpath, true, false, elevel) != 0)
    return -1;

  return 0;
}

/*
 * Create a PostgreSQL data sub-directory
 *
 * The data directory itself, and most of its sub-directories, are created at
 * initdb time, but we do have some occasions when we create directories in
 * the backend (CREATE TABLESPACE, for example).  In those cases, we want to
 * make sure that those directories are created consistently.  Today, that means
 * making sure that the created directory has the correct permissions, which is
 * what pg_dir_create_mode tracks for us.
 *
 * Note that we also set the umask() based on what we understand the correct
 * permissions to be (see file_perm.c).
 *
 * For permissions other than the default, mkdir() can be used directly, but
 * be sure to consider carefully such cases -- a sub-directory with incorrect
 * permissions in a PostgreSQL data directory could cause backups and other
 * processes to fail.
 */
int MakePGDirectory(const char *directoryName) {
  return mkdir(directoryName, pcb->pg_dir_create_mode);
}

/*
 * Return the passed-in error level, or PANIC if data_sync_retry is off.
 *
 * Failure to fsync any data file is cause for immediate panic, unless
 * data_sync_retry is enabled.  Data may have been written to the operating
 * system and removed from our buffer pool already, and if we are running on
 * an operating system that forgets dirty data on write-back failure, there
 * may be only one copy of the data remaining: in the WAL.  A later attempt to
 * fsync again might falsely report success.  Therefore we must not allow any
 * further checkpoints to be attempted.  data_sync_retry can in theory be
 * enabled on systems known not to drop dirty buffered data on write-back
 * failure (with the likely outcome that checkpoints will continue to fail
 * until the underlying problem is fixed).
 *
 * Any code that reports a failure from fsync() or related functions should
 * filter the error level with this function.
 */
int data_sync_elevel(int elevel) { return data_sync_retry ? elevel : PANIC; }

/* ResourceOwner callbacks */

static void ResOwnerReleaseFile(Datum res) {
  File file = (File)DatumGetInt32(res);
  Vfd *vfdP;

  Assert(FileIsValid(file));

  vfdP = &VfdCache[file];
  vfdP->resowner = NULL;

  FileClose(file);
}

static char *ResOwnerPrintFile(Datum res) {
  return psprintf("File %d", DatumGetInt32(res));
}
