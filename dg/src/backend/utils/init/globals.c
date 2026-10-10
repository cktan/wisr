/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
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

#include "common/file_perm.h"
#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"

thread_local /*LOCAL*/ ProtocolVersion FrontendProtocol;

thread_local /*LOCAL*/ volatile sig_atomic_t InterruptPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t QueryCancelPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t ProcDiePending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t CheckClientConnectionPending =
    false;
thread_local /*LOCAL*/ volatile sig_atomic_t ClientConnectionLost = false;
thread_local /*LOCAL*/ volatile sig_atomic_t
    IdleInTransactionSessionTimeoutPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t TransactionTimeoutPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t IdleSessionTimeoutPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t ProcSignalBarrierPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t LogMemoryContextPending = false;
thread_local /*LOCAL*/ volatile sig_atomic_t IdleStatsUpdateTimeoutPending =
    false;
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
 * current process. It will either point to a process local latch if the
 * current process does not have a PGPROC entry in that moment, or to
 * PGPROC->procLatch if it has. Thus it can always be used in signal handlers,
 * without checking for its existence.
 */
thread_local /*LOCAL*/ struct Latch *MyLatch;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
thread_local /*SHARED*/ char *DataDir = NULL;

/*
 * Mode of the data directory.  The default is 0700 but it may be changed in
 * checkDataDir() to 0750 if the data directory actually has that mode.
 */
thread_local /*SHARED*/ int data_directory_mode = PG_DIR_MODE_OWNER;

thread_local /*SHARED*/ char
    OutputFileName[MAXPGPATH]; /* debugging output file */

thread_local /*SHARED*/ char
    my_exec_path[MAXPGPATH]; /* full path to my executable */
thread_local /*SHARED*/ char
    pkglib_path[MAXPGPATH]; /* full path to lib directory */

#ifdef EXEC_BACKEND
char postgres_exec_path[MAXPGPATH]; /* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

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

thread_local /*SHARED*/ pid_t PostmasterPid = 0;

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
 */
thread_local /*SHARED*/ bool IsPostmasterEnvironment = false;
thread_local /*LOCAL*/ bool IsUnderPostmaster = false;
thread_local /*SHARED*/ bool IsBinaryUpgrade = false;

thread_local /*LOCAL*/ bool ExitOnAnyError = false;

thread_local /*LOCAL*/ int DateStyle = USE_ISO_DATES;
thread_local /*LOCAL*/ int DateOrder = DATEORDER_MDY;
thread_local /*LOCAL*/ int IntervalStyle = INTSTYLE_POSTGRES;

thread_local /*LOCAL*/ bool enableFsync = true;
thread_local /*LOCAL*/ bool allowSystemTableMods = false;
thread_local /*LOCAL*/ int work_mem = 4096;
thread_local /*LOCAL*/ double hash_mem_multiplier = 2.0;
thread_local /*LOCAL*/ int maintenance_work_mem = 65536;

/*
 * Primary determinants of sizes of shared-memory structures.
 *
 * MaxBackends is computed by PostmasterMain after modules have had a chance to
 * register background workers.
 */
thread_local /*LOCAL*/ int NBuffers = 16384;
thread_local /*LOCAL*/ int MaxConnections = 100;
thread_local /*LOCAL*/ int max_worker_processes = 8;
thread_local /*SHARED*/ int MaxBackends = 0;

/* GUC parameters for vacuum */
thread_local /*LOCAL*/ int VacuumBufferUsageLimit = 2048;

thread_local /*LOCAL*/ int VacuumCostPageHit = 1;
thread_local /*LOCAL*/ int VacuumCostPageMiss = 2;
thread_local /*LOCAL*/ int VacuumCostPageDirty = 20;
thread_local /*LOCAL*/ int VacuumCostLimit = 200;
thread_local /*LOCAL*/ double VacuumCostDelay = 0;

thread_local /*LOCAL*/ int VacuumCostBalance = 0; /* working state for vacuum */
thread_local /*LOCAL*/ bool VacuumCostActive = false;

/* configurable SLRU buffer sizes */
thread_local /*LOCAL*/ int commit_timestamp_buffers = 0;
thread_local /*LOCAL*/ int multixact_member_buffers = 32;
thread_local /*LOCAL*/ int multixact_offset_buffers = 16;
thread_local /*LOCAL*/ int notify_buffers = 16;
thread_local /*LOCAL*/ int serializable_buffers = 32;
thread_local /*LOCAL*/ int subtransaction_buffers = 0;
thread_local /*LOCAL*/ int transaction_buffers = 0;
