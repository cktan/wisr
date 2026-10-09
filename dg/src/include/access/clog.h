/*
 * clog.h
 *
 * PostgreSQL transaction-commit-log manager
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/clog.h
 */
#ifndef CLOG_H
#define CLOG_H

#include "storage/sync.h"
#include "lib/stringinfo.h"

/*
 * Possible transaction statuses --- note that all-zeroes is the initial
 * state.
 *
 * A "subcommitted" transaction is a committed subtransaction whose parent
 * hasn't committed or aborted yet.
 */
typedef int XidStatus;

#define TRANSACTION_STATUS_IN_PROGRESS		0x00
#define TRANSACTION_STATUS_COMMITTED		0x01
#define TRANSACTION_STATUS_ABORTED			0x02
#define TRANSACTION_STATUS_SUB_COMMITTED	0x03

extern void TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
                                       TransactionId *subxids,
                                       XidStatus status);
extern XidStatus TransactionIdGetStatus(TransactionId xid);

extern Size CLOGShmemSize(void);
extern void CLOGShmemInit(void);
extern void TrimCLOG(void);
extern void CheckPointCLOG(void);
extern void ExtendCLOG(TransactionId newestXact);

extern int clogsyncfiletag(const FileTag *ftag, char *path);

#endif /* CLOG_H */
