/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  The structures every thread of the server shares.
 *
 * PostgreSQL laid these out in one shared memory block its processes
 * attached, sized ahead of time.  digres is one process, whose threads share
 * all its memory: each structure is an ordinary allocation, made when its
 * module first asks for it, and never freed.
 *
 * Each structure has a string name.  ShmemInitStruct() finds it by name, or
 * makes it, so that a module's initialization reads the same whether it
 * creates the structure (in the postmaster or a standalone backend) or a
 * session thread attaches to what the postmaster made.  The index of names
 * is in the process control block (utils/control_block.h).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <string>
#include <unordered_map>

#include "fmgr.h"
#include "miscadmin.h"
#include "port/pg_numa.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/control_block.h"

/* One structure: where it is, and the size it was made with. */
typedef struct shmem_entry_t {
  void *location;
  Size size;
} shmem_entry_t;

/* The index of the shared structures, by name: pcb->shmem_index. */
struct shmem_index_t {
  std::mutex mu; /* guards entries */
  std::unordered_map<std::string, shmem_entry_t> entries;
};

/* Free the index: never, as the process ends first; for the deleter. */
void shmem_index_free_t::operator()(shmem_index_t *index) const {
  delete index;
}

/* spinlock for LWLock allocation (lwlock.c) */
thread_local /*SHARED*/ slock_t *ShmemLock;

/*
 * Make the index of shared structures, and ShmemLock.  The postmaster or a
 * standalone backend calls it once, before any thread starts.
 */
void InitShmemIndex(void) {
  Assert(!IsUnderPostmaster && !pcb->shmem_index);
  pcb->shmem_index.reset(new shmem_index_t());
  ShmemLock = (slock_t *)ShmemAlloc(sizeof(slock_t));
  SpinLockInit(ShmemLock);
}

/*
 * ShmemAllocNoError -- allocate a zeroed, cache-line aligned chunk, or
 * return NULL.  It is never freed.  Large chunks are zero and committed as
 * first touched, as the allocator maps them afresh.
 */
void *ShmemAllocNoError(Size size) {
  char *raw;

  /*
   * Many parts of the system are sensitive to critical data structures
   * getting split across cache line boundaries, so each chunk starts on
   * one.  It is never freed, so the padding before it need not be found
   * again.
   */
  raw = (char *)calloc(1, add_size(size, PG_CACHE_LINE_SIZE));
  if (raw == NULL)
    return NULL;
  return (void *)CACHELINEALIGN(raw);
}

/* ShmemAlloc -- as ShmemAllocNoError, but throws an error if out of memory */
void *ShmemAlloc(Size size) {
  void *newSpace = ShmemAllocNoError(size);

  if (!newSpace)
    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                    errmsg("out of memory (%zu bytes requested)", size)));
  return newSpace;
}

/*
 * ShmemInitHash -- Create and initialize, or attach to, a hash table the
 *		threads share.
 *
 * max_size is the estimated maximum number of hashtable entries.  This is
 * not a hard limit, but the access efficiency will degrade if it is
 * exceeded substantially (since it's used to compute directory size and
 * the hash table buckets will get overfull).
 *
 * init_size is the number of hashtable entries to preallocate.
 *
 * *infoP and hash_flags must specify at least the entry sizes and key
 * comparison semantics (see hash_create()).  Flag bits and values specific
 * to shared hash tables are added here, except that callers may choose to
 * specify HASH_PARTITION and/or HASH_FIXED_SIZE.
 */
HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
                    HASHCTL *infoP, int hash_flags) {
  bool found;
  void *location;

  /*
   * A shared hash table has a fixed directory: it can't grow, or other
   * threads' handles wouldn't find it.  So make it big enough to start with.
   */
  infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
  infoP->alloc = ShmemAllocNoError;
  hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

  /* look it up in the index */
  location =
      ShmemInitStruct(name, hash_get_shared_size(infoP, hash_flags), &found);

  /* if it already exists, attach to it rather than initialize it anew */
  if (found)
    hash_flags |= HASH_ATTACH;

  /* Pass location of hashtable header to hash_create */
  infoP->hctl = (HASHHDR *)location;

  return hash_create(name, init_size, infoP, hash_flags);
}

/*
 * ShmemInitStruct -- Create, or find, a structure the threads share.
 *
 *	Returns: pointer to the object, zeroed when made.  *foundPtr is set true
 *		if the object was already in the index (hence, already
 *		initialized).  Throws an error rather than return NULL.
 */
void *ShmemInitStruct(const char *name, Size size, bool *foundPtr) {
  shmem_index_t *index = pcb->shmem_index.get();
  void *structPtr;

  /* The index is the threads': one looks or adds at a time. */
  std::lock_guard<std::mutex> guard(index->mu);
  auto it = index->entries.find(name);

  *foundPtr = it != index->entries.end();
  if (*foundPtr) {
    /*
     * Someone else has made it already.  The size better be the same as
     * the size we are trying to initialize to, or there is a name conflict
     * (or worse).
     */
    if (it->second.size != size)
      ereport(ERROR, (errmsg("shared structure \"%s\" has the wrong size: "
                             "expected %zu, actual %zu",
                             name, size, it->second.size)));
    return it->second.location;
  }

  structPtr = ShmemAlloc(size);
  index->entries.emplace(name, shmem_entry_t{structPtr, size});
  return structPtr;
}

/* Is NUMA support available? */
Datum pg_numa_available(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(pg_numa_init() != -1);
}
