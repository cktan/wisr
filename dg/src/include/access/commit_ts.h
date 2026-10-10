/*
 * commit_ts.h
 *
 * PostgreSQL commit timestamp manager
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/commit_ts.h
 */
#ifndef COMMIT_TS_H
#define COMMIT_TS_H

#include "access/xlogdefs.h"
#include "datatype/timestamp.h"

extern thread_local /*LOCAL*/ PGDLLIMPORT bool track_commit_timestamp;

extern void TransactionTreeSetCommitTsData(TransactionId xid, int nsubxids,
                                           TransactionId *subxids,
                                           TimestampTz timestamp,
                                           RepOriginId nodeid);
extern bool TransactionIdGetCommitTsData(TransactionId xid, TimestampTz *ts,
                                         RepOriginId *nodeid);
extern TransactionId GetLatestCommitTsData(TimestampTz *ts,
                                           RepOriginId *nodeid);

extern void CommitTsShmemInit(void);
extern void StartupCommitTs(void);
extern void CompleteCommitTsInitialization(void);
extern void ExtendCommitTs(TransactionId newestXact);

#endif /* COMMIT_TS_H */
