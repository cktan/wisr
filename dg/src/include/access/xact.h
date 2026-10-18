/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"

/*
 * Maximum size of Global Transaction ID (including '\0').
 *
 * Note that the max value of GIDSIZE must fit in the uint16 gidlen,
 * specified in TwoPhaseFileHeader.
 */
#define GIDSIZE 200

/*
 * Xact isolation levels
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern thread_local /*LOCAL*/ PGDLLIMPORT int DefaultXactIsoLevel;
extern thread_local /*LOCAL*/ PGDLLIMPORT int XactIsoLevel;

/*
 * We implement three isolation levels internally.
 * The two stronger ones use one snapshot per database transaction;
 * the others use one snapshot per statement.
 * Serializable uses predicate locks in addition to snapshots.
 * These macros should be used to check which isolation level is selected.
 */
#define IsolationUsesXactSnapshot() (XactIsoLevel >= XACT_REPEATABLE_READ)
#define IsolationIsSerializable() (XactIsoLevel == XACT_SERIALIZABLE)

/* Xact read-only state */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool DefaultXactReadOnly;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool XactReadOnly;

/* flag for logging statements in this transaction */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool xact_is_sampled;

/*
 * Xact is deferrable -- only meaningful (currently) for read only
 * SERIALIZABLE transactions
 */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool DefaultXactDeferrable;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool XactDeferrable;

/*
 * Miscellaneous flag bits to record events which occur on the top level
 * transaction. These flags are only persisted in MyXactFlags and are intended
 * so we remember to do certain things later in the transaction. This is
 * globally accessible, so can be set from anywhere in the code which requires
 * recording flags.
 */
extern thread_local /*LOCAL*/ PGDLLIMPORT int MyXactFlags;

/*
 * XACT_FLAGS_ACCESSEDTEMPNAMESPACE - set when a temporary object is accessed.
 * We don't allow PREPARE TRANSACTION in that case.
 */
#define XACT_FLAGS_ACCESSEDTEMPNAMESPACE		(1U << 0)

/*
 * XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK - records whether the top level xact
 * logged any Access Exclusive Locks.
 */
#define XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK	(1U << 1)

/*
 * XACT_FLAGS_NEEDIMMEDIATECOMMIT - records whether the top level statement
 * is one that requires immediate commit, such as CREATE DATABASE.
 */
#define XACT_FLAGS_NEEDIMMEDIATECOMMIT			(1U << 2)

/*
 * XACT_FLAGS_PIPELINING - set when we complete an extended-query-protocol
 * Execute message.  This is useful for detecting that an implicit transaction
 * block has been created via pipelining.
 */
#define XACT_FLAGS_PIPELINING					(1U << 3)

/* Data structure for Save/RestoreTransactionCharacteristics */
typedef struct SavedTransactionCharacteristics {
  int save_XactIsoLevel;
  bool save_XactReadOnly;
  bool save_XactDeferrable;
} SavedTransactionCharacteristics;

/*
 * A transactionally dropped statistics entry.
 */
typedef struct xl_xact_stats_item {
  int kind;
  Oid dboid;

  /* The value of PgStat_HashKey.objid, as two uint32 */
  uint32 objid_lo;
  uint32 objid_hi;
} xl_xact_stats_item;

/* ----------------
 *		extern definitions
 * ----------------
 */
extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern SubTransactionId GetCurrentSubTransactionId(void);
extern CommandId GetCurrentCommandId(bool used);
extern TimestampTz GetCurrentTransactionStartTimestamp(void);
extern TimestampTz GetCurrentStatementStartTimestamp(void);
extern void SetCurrentStatementStartTimestamp(void);
extern int GetCurrentTransactionNestLevel(void);
extern void CommandCounterIncrement(void);
extern void StartTransactionCommand(void);
extern void SaveTransactionCharacteristics(SavedTransactionCharacteristics *s);
extern void
RestoreTransactionCharacteristics(const SavedTransactionCharacteristics *s);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern bool EndTransactionBlock(bool chain);
extern void UserAbortTransactionBlock(bool chain);
extern void BeginImplicitTransactionBlock(void);
extern void EndImplicitTransactionBlock(void);
extern void ReleaseSavepoint(const char *name);
extern void DefineSavepoint(const char *name);
extern void RollbackToSavepoint(const char *name);
extern bool IsSubTransaction(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern char TransactionBlockStatusCode(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventInTransactionBlock(bool isTopLevel, const char *stmtType);
extern void RequireTransactionBlock(bool isTopLevel, const char *stmtType);
extern void WarnNoTransactionBlock(bool isTopLevel, const char *stmtType);

#endif /* XACT_H */
