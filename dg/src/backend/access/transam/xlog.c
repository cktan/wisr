/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		What is left of PostgreSQL's write-ahead log manager
 *
 * digres has no WAL and no pg_control: sm keeps what is durable and recovers
 * its own storage when its home directory is attached (sm/PART4.md).  What
 * is left here is the control record, made in memory for one run of the
 * server, and the startup and shutdown of the transaction counters.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/* GUC parameter */
thread_local /*LOCAL*/ bool log_checkpoints = true;

/*
 * Statistics for current checkpoint are collected in this global struct.
 * Because only the checkpointer or a stand-alone backend can perform
 * checkpoints, this will be unused in normal backends.
 */
thread_local /*LOCAL*/ CheckpointStatsData CheckpointStats;

/*
 * The control record.  The postmaster makes it before shared memory exists,
 * and XLOGShmemInit() moves it there.
 */
static thread_local /*SHARED*/ ControlFileData *ControlFile = NULL;

/*
 * Make the control record as PostgreSQL's bootstrap would have, with a
 * random nonce.  The nonce is used for authentication requests that will
 * fail because the user does not exist: it makes a genuine-looking password
 * challenge for the non-existent user.
 */
static void InitControlFile(void) {
  memset(ControlFile, 0, sizeof(ControlFileData));
  if (!pg_strong_random(ControlFile->mock_authentication_nonce,
                        MOCK_AUTH_NONCE_LEN))
    ereport(PANIC, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("could not generate secret authorization token")));
}

/*
 * Returns the random nonce from the control record.
 */
char *GetMockAuthenticationNonce(void) {
  Assert(ControlFile != NULL);
  return ControlFile->mock_authentication_nonce;
}

/*
 * Are checksums enabled for data pages?  Never: initdb does not ask for them.
 */
bool DataChecksumsEnabled(void) {
  Assert(ControlFile != NULL);
  return (ControlFile->data_checksum_version > 0);
}

/*
 * Make the control record in local memory, before shared memory exists.
 * reset says whether a previous record is expected (in the reset case,
 * there's a dangling pointer into old shared memory), or not.
 */
void LocalProcessControlFile(bool reset) {
  Assert(reset || ControlFile == NULL);
  ControlFile = (ControlFileData *)palloc(sizeof(ControlFileData));
  InitControlFile();
}

/*
 * Size of the shared memory for the control record.
 */
Size XLOGShmemSize(void) { return sizeof(ControlFileData); }

/*
 * Move the control record into shared memory, or attach to it there.
 */
void XLOGShmemInit(void) {
  bool found;
  ControlFileData *localControlFile = ControlFile;

  ControlFile = (ControlFileData *)ShmemInitStruct(
      "Control File", sizeof(ControlFileData), &found);
  if (found) {
    if (localControlFile)
      pfree(localControlFile);
    return;
  }

  /* Have made the record locally, unless in bootstrap mode */
  if (localControlFile) {
    memcpy(ControlFile, localControlFile, sizeof(ControlFileData));
    pfree(localControlFile);
  }
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup.
 *
 * There is no WAL to recover from.  What is left of startup is to give the
 * transaction counters their first values, as a bootstrap would.
 * PostgreSQL's transactions still run a statement here; they begin afresh
 * each time the server starts, and write nothing: sm's are to replace them.
 */
void StartupXLOG(void) {
  TransamVariables->nextXid = FirstNormalFullTransactionId;
  TransamVariables->nextOid = FirstNormalObjectId;
  SetTransactionIdLimit(FirstNormalTransactionId, InvalidOid);

  /* Hold the lock so that no snapshot sees the counter half-set */
  LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
  TransamVariables->latestCompletedXid = TransamVariables->nextXid;
  FullTransactionIdRetreat(&TransamVariables->latestCompletedXid);
  LWLockRelease(ProcArrayLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown.
 * There is no shutdown checkpoint: there is no WAL to checkpoint.
 */
void ShutdownXLOG(int code, Datum arg) {
  /* Don't be chatty in standalone mode */
  ereport(IsPostmasterEnvironment ? LOG : NOTICE, (errmsg("shutting down")));
}
