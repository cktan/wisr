/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  the shared struct of transaction state GetSnapshotData() reads
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/varsup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/shmem.h"

/* pointer to variables struct in shared memory */
thread_local /*SHARED*/ TransamVariablesData *TransamVariables = NULL;

/*
 * Make the shared struct, before any thread starts.  PostgreSQL's xid and
 * OID counters are gone: digres assigns no xid, and sm gives the oids.  What
 * is left is what GetSnapshotData() reads, constant until the snapshot is.
 */
void VarsupShmemInit(void) {
  bool found;

  /* Initialize our shared state struct */
  TransamVariables = (TransamVariablesData *)ShmemInitStruct(
      "TransamVariables", sizeof(TransamVariablesData), &found);
  if (!IsUnderPostmaster) {
    Assert(!found);
    memset(TransamVariables, 0, sizeof(TransamVariablesData));
    TransamVariables->oldestXid = FirstNormalTransactionId;
    TransamVariables->latestCompletedXid = FirstNormalFullTransactionId;
    FullTransactionIdRetreat(&TransamVariables->latestCompletedXid);
  } else
    Assert(found);
}
