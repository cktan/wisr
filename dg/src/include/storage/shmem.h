/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  The structures every thread of the server shares (shmem.c).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "storage/spin.h"
#include "utils/hsearch.h"

/* shmem.c */
extern thread_local /*SHARED*/ PGDLLIMPORT slock_t *ShmemLock;
extern void InitShmemIndex(void);
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);
extern HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
                           HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);

#endif /* SHMEM_H */
