/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres transaction (commit) log interface routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/transam.c
 *
 * NOTES
 *	  This file contains the high level access-method interface to the
 *	  transaction system.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"

/* ----------------------------------------------------------------
 *						Interface functions
 *
 *		TransactionIdDidCommit
 *		TransactionIdDidAbort
 *		========
 *		   these functions test the transaction status of
 *		   a specified transaction id.
 *
 * There is no commit log: sm1's status record is to replace it.  Until then
 * every transaction that is no longer in progress is said to have
 * committed, so an aborted transaction's changes are visible.  Callers check
 * TransactionIdIsInProgress() first.
 * ----------------------------------------------------------------
 */

/*
 * TransactionIdDidCommit
 *		True iff transaction associated with the identifier did commit.
 *		A dummy: true for every valid transaction id.
 */
bool TransactionIdDidCommit(TransactionId transactionId) {
  return TransactionIdIsValid(transactionId);
}

/*
 * TransactionIdDidAbort
 *		True iff transaction associated with the identifier did abort.
 *		A dummy: true only for the invalid transaction id.
 */
bool TransactionIdDidAbort(TransactionId transactionId) {
  return !TransactionIdDidCommit(transactionId);
}

/*
 * TransactionIdPrecedes --- is id1 logically < id2?
 */
bool TransactionIdPrecedes(TransactionId id1, TransactionId id2) {
  /*
   * If either ID is a permanent XID then we can just do unsigned
   * comparison.  If both are normal, do a modulo-2^32 comparison.
   */
  int32 diff;

  if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
    return (id1 < id2);

  diff = (int32)(id1 - id2);
  return (diff < 0);
}

/*
 * TransactionIdPrecedesOrEquals --- is id1 logically <= id2?
 */
bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2) {
  int32 diff;

  if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
    return (id1 <= id2);

  diff = (int32)(id1 - id2);
  return (diff <= 0);
}

/*
 * TransactionIdFollows --- is id1 logically > id2?
 */
bool TransactionIdFollows(TransactionId id1, TransactionId id2) {
  int32 diff;

  if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
    return (id1 > id2);

  diff = (int32)(id1 - id2);
  return (diff > 0);
}

/*
 * TransactionIdFollowsOrEquals --- is id1 logically >= id2?
 */
bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2) {
  int32 diff;

  if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
    return (id1 >= id2);

  diff = (int32)(id1 - id2);
  return (diff >= 0);
}

/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 */
bool TransactionIdIsInProgress(TransactionId xid) {
  /* No session holds a PostgreSQL xid: digres assigns none. */
  return false;
}
