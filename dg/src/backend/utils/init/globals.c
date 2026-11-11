/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"

thread_local /*LOCAL*/ ProtocolVersion FrontendProtocol;

thread_local /*LOCAL*/ bool IsSessionThread = false;
thread_local /*LOCAL*/ bool InterruptPending = false;
const std::atomic<uint32_t> NoInterrupts{0};
thread_local /*LOCAL*/ const std::atomic<uint32_t> *MyInterrupts =
    &NoInterrupts;
thread_local /*LOCAL*/ bool QueryCancelPending = false;
thread_local /*LOCAL*/ bool ProcDiePending = false;
thread_local /*LOCAL*/ bool CheckClientConnectionPending = false;
thread_local /*LOCAL*/ bool ClientConnectionLost = false;
thread_local /*LOCAL*/ bool IdleInTransactionSessionTimeoutPending = false;
thread_local /*LOCAL*/ bool TransactionTimeoutPending = false;
thread_local /*LOCAL*/ bool IdleSessionTimeoutPending = false;
thread_local /*LOCAL*/ bool LogMemoryContextPending = false;
thread_local /*LOCAL*/ volatile uint32 InterruptHoldoffCount = 0;
thread_local /*LOCAL*/ volatile uint32 QueryCancelHoldoffCount = 0;
thread_local /*LOCAL*/ volatile uint32 CritSectionCount = 0;

thread_local /*LOCAL*/ int MyProcPid;
thread_local /*LOCAL*/ pg_time_t MyStartTime;
thread_local /*LOCAL*/ TimestampTz MyStartTimestamp;
thread_local /*LOCAL*/ struct ClientSocket *MyClientSocket;
thread_local /*LOCAL*/ struct Port *MyProcPort;
thread_local /*LOCAL*/ uint8 MyCancelKey[MAX_CANCEL_KEY_LENGTH];
thread_local /*LOCAL*/ int MyCancelKeyLength = 0;
thread_local /*LOCAL*/ int MyPMChildSlot;

/*
 * MyLatch points to the latch that should be used for signal handling by the
 * current thread: its local latch, which it always has.  Thus it can always
 * be used in signal handlers, without checking for its existence.
 */
thread_local /*LOCAL*/ struct Latch *MyLatch;

/* DataDir and the paths below are pcb's (utils/control_block.h). */

thread_local /*LOCAL*/ ProcNumber MyProcNumber = INVALID_PROC_NUMBER;

thread_local /*LOCAL*/ ProcNumber ParallelLeaderProcNumber =
    INVALID_PROC_NUMBER;

thread_local /*LOCAL*/ Oid MyDatabaseId = InvalidOid;

thread_local /*LOCAL*/ Oid MyDatabaseTableSpace = InvalidOid;

thread_local /*LOCAL*/ bool MyDatabaseHasLoginEventTriggers = false;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
thread_local /*LOCAL*/ char *DatabasePath = NULL;

/*
 * IsPostmasterEnvironment is true in a postmaster process and any postmaster
 * child process; it is false in a standalone process (bootstrap or
 * standalone backend).  IsUnderPostmaster is true in postmaster child
 * processes.  Note that "child process" includes all children, not only
 * regular backends.  These should be set correctly as early as possible
 * in the execution of a process, so that error handling will do the right
 * things if an error should occur during process initialization.
 *
 * These are initialized for the bootstrap/standalone case.
 * IsPostmasterEnvironment, PostmasterPid and IsBinaryUpgrade are pcb's.
 */
thread_local /*LOCAL*/ bool IsUnderPostmaster = false;

thread_local /*LOCAL*/ int DateStyle = USE_ISO_DATES;
thread_local /*LOCAL*/ int DateOrder = DATEORDER_MDY;
thread_local /*LOCAL*/ int IntervalStyle = INTSTYLE_POSTGRES;

/*
 * Primary determinants of sizes of shared-memory structures.
 *
 * MaxBackends, pcb's, is computed by PostmasterMain after modules have had a
 * chance to register background workers.  NBuffers sizes sm's block cache,
 * in blocks (datadir_load()).
 */

/* GUC parameters for vacuum */

thread_local /*LOCAL*/ int VacuumCostBalance = 0; /* working state for vacuum */
thread_local /*LOCAL*/ bool VacuumCostActive = false;

/* configurable SLRU buffer sizes */
