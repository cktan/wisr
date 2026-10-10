/*-------------------------------------------------------------------------
 *
 * twophase.h
 *	  Two-phase-commit related declarations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/twophase.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "access/xact.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"

/*
 * GlobalTransactionData is defined in twophase.c; other places have no
 * business knowing the internal definition.
 */
typedef struct GlobalTransactionData *GlobalTransaction;

/* GUC variable */
extern thread_local /*LOCAL*/ PGDLLIMPORT int max_prepared_xacts;

extern Size TwoPhaseShmemSize(void);
extern void TwoPhaseShmemInit(void);

extern void AtAbort_Twophase(void);

extern TransactionId TwoPhaseGetXidByVirtualXID(VirtualTransactionId vxid,
                                                bool *have_more);

extern void FinishPreparedTransaction(const char *gid, bool isCommit);

extern void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid_res,
                                   int szgid);
extern bool LookupGXactBySubid(Oid subid);

#endif /* TWOPHASE_H */
