/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * advisory_lock.h
 *	  PostgreSQL's advisory locks, in a table of digres's own.
 *
 * An advisory lock names a number the application picks, and guards
 * nothing digres stores, so sm does not hold it (dg/SHMEM.md section 3).
 * A session holds one at session scope, until it unlocks it, or at
 * transaction scope, until its transaction or the subtransaction that
 * took it ends; each hold is counted, a share and an exclusive hold of
 * one key by one session are two, and a session's holds never conflict
 * with each other.  A deadlock among advisory lock waits is detected; one
 * through sm's locks as well is not, and lock_timeout ends it.
 *
 * src/include/storage/advisory_lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ADVISORY_LOCK_H
#define ADVISORY_LOCK_H

#include <vector>

/* What an advisory lock names: PostgreSQL's advisory locktag's fields. */
typedef struct advisory_key_t {
  Oid database; /* the session's database: a lock is local to it */
  uint32 key1;  /* the first int4 key, or an int8 key's high half */
  uint32 key2;  /* the second int4 key, or an int8 key's low half */
  uint16 form;  /* 1 for an int8 key, 2 for two int4 keys */
} advisory_key_t;

/* A lock of the table, held or waited for, as pg_locks lists it. */
typedef struct advisory_status_t {
  advisory_key_t key;
  int pid;        /* the holder's, or the waiter's */
  bool exclusive; /* the mode held or asked: exclusive, else share */
  bool granted;   /* held; false if waited for */
} advisory_status_t;

extern void advisory_init(void);
extern bool advisory_lock(const advisory_key_t &key, bool exclusive,
                          bool session, bool dontwait);
extern bool advisory_unlock(const advisory_key_t &key, bool exclusive);
extern void advisory_unlock_all(void);
extern void advisory_release_all(void);
extern void advisory_xact_end(void);
extern void advisory_subxact_end(bool commit);
extern std::vector<int> advisory_blockers(int pid);
extern std::vector<advisory_status_t> advisory_status(void);

#endif /* ADVISORY_LOCK_H */
