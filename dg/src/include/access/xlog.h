/*
 * xlog.h
 *
 * What is left of PostgreSQL's write-ahead log manager.  digres has no WAL:
 * sm keeps what is durable (sm/PART4.md).  This is the in-memory control
 * record, server startup and shutdown, and the checkpoint request flags and
 * statistics the checkpointer still uses.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog.h
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/xlogdefs.h"
#include "datatype/timestamp.h"

/* these variables are GUC parameters related to XLOG */
extern thread_local PGDLLIMPORT bool log_checkpoints;

/*
 * OR-able request flag bits for checkpoints.  The "cause" bits are used only
 * for logging purposes.  Note: the flags must be defined so that it's
 * sensible to OR together request flags arising from different requestors.
 */

/* These directly affect the behavior of a checkpoint */
#define CHECKPOINT_IS_SHUTDOWN	0x0001 /* Checkpoint is for shutdown */
#define CHECKPOINT_IMMEDIATE	0x0004   /* Do it without delays */
#define CHECKPOINT_FORCE		0x0008       /* Force even if no activity */
#define CHECKPOINT_FLUSH_ALL	0x0010                              /* Flush all pages, including those      \
                                       * belonging to unlogged tables */
/* These are important to RequestCheckpoint */
#define CHECKPOINT_WAIT			0x0020      /* Wait for completion */
#define CHECKPOINT_REQUESTED	0x0040 /* Checkpoint request has been made */
/* These indicate the cause of a checkpoint request */
#define CHECKPOINT_CAUSE_TIME	0x0100 /* Elapsed time */

/* Checkpoint statistics */
typedef struct CheckpointStatsData {
  TimestampTz ckpt_start_t;    /* start of checkpoint */
  TimestampTz ckpt_write_t;    /* start of flushing buffers */
  TimestampTz ckpt_sync_t;     /* start of fsyncs */
  TimestampTz ckpt_sync_end_t; /* end of fsyncs */
  TimestampTz ckpt_end_t;      /* end of checkpoint */

  int ckpt_bufs_written; /* # of buffers written */
  int ckpt_slru_written; /* # of SLRU buffers written */

  int ckpt_sync_rels;        /* # of relations synced */
  uint64 ckpt_longest_sync;  /* Longest sync for one relation */
  uint64 ckpt_agg_sync_time; /* The sum of all the individual sync
                              * times, which is not necessarily the
                              * same as the total elapsed time for the
                              * entire sync phase. */
} CheckpointStatsData;

extern thread_local PGDLLIMPORT CheckpointStatsData CheckpointStats;

extern char *GetMockAuthenticationNonce(void);
extern bool DataChecksumsEnabled(void);
extern Size XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void LocalProcessControlFile(bool reset);
extern void StartupXLOG(void);
extern void ShutdownXLOG(int code, Datum arg);

#endif /* XLOG_H */
