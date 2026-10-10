/*-------------------------------------------------------------------------
 *
 * private_shmem.c
 *	  The block of memory that PostgreSQL calls shared memory.
 *
 * PostgreSQL put its shared structures in a segment its processes
 * attached.  digres is one process of threads, which share all its memory,
 * so the block is an ordinary private mapping of the process: no System V
 * segment, no key in postmaster.pid, no huge pages.  It is made once, by
 * the postmaster or a standalone backend, and lives as long as the process.
 *
 * IDENTIFICATION
 *	  src/backend/port/private_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/mman.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/pg_shmem.h"

/*
 * Make the block of size bytes and initialize its header; *shim is the
 * header too.  The mapping's pages are zero, and are committed only as they
 * are first touched, as a segment's were.
 */
PGShmemHeader *PGSharedMemoryCreate(Size size, PGShmemHeader **shim) {
  PGShmemHeader *hdr;
  void *addr;

  /* Room for a header? */
  Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

  /* The process's own memory, never unmapped: the process outlives no use. */
  addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
              -1, 0);
  if (addr == MAP_FAILED)
    ereport(FATAL, (errmsg("could not map shared memory: %m"),
                    errdetail("Failed system call was mmap(size=%zu).", size)));

  hdr = (PGShmemHeader *)addr;
  hdr->creatorPID = getpid();
  hdr->magic = PGShmemMagic;
  hdr->dsm_control = 0;
  hdr->device = 0;
  hdr->inode = 0;
  hdr->totalsize = size;
  hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
  *shim = hdr;
  return hdr;
}
