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

thread_local ProtocolVersion FrontendProtocol;

thread_local volatile sig_atomic_t InterruptPending = false;
thread_local volatile sig_atomic_t QueryCancelPending = false;
thread_local volatile sig_atomic_t ProcDiePending = false;
thread_local volatile sig_atomic_t CheckClientConnectionPending = false;
thread_local volatile sig_atomic_t ClientConnectionLost = false;
thread_local volatile sig_atomic_t IdleInTransactionSessionTimeoutPending =
    false;
thread_local volatile sig_atomic_t TransactionTimeoutPending = false;
thread_local volatile sig_atomic_t IdleSessionTimeoutPending = false;
thread_local volatile sig_atomic_t ProcSignalBarrierPending = false;
thread_local volatile sig_atomic_t LogMemoryContextPending = false;
thread_local volatile sig_atomic_t IdleStatsUpdateTimeoutPending = false;
thread_local volatile uint32 InterruptHoldoffCount = 0;
thread_local volatile uint32 QueryCancelHoldoffCount = 0;
thread_local volatile uint32 CritSectionCount = 0;

thread_local int MyProcPid;
thread_local pg_time_t MyStartTime;
thread_local TimestampTz MyStartTimestamp;
thread_local struct ClientSocket *MyClientSocket;
thread_local struct Port *MyProcPort;
thread_local uint8 MyCancelKey[MAX_CANCEL_KEY_LENGTH];
thread_local int MyCancelKeyLength = 0;
thread_local int MyPMChildSlot;

/*
 * MyLatch points to the latch that should be used for signal handling by the
 * current process. It will either point to a process local latch if the
 * current process does not have a PGPROC entry in that moment, or to
 * PGPROC->procLatch if it has. Thus it can always be used in signal handlers,
 * without checking for its existence.
 */
thread_local struct Latch *MyLatch;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
PG_SHARED char *DataDir = NULL;

/*
 * Mode of the data directory.  The default is 0700 but it may be changed in
 * checkDataDir() to 0750 if the data directory actually has that mode.
 */
PG_SHARED int data_directory_mode = PG_DIR_MODE_OWNER;

PG_SHARED char OutputFileName[MAXPGPATH]; /* debugging output file */

PG_SHARED char my_exec_path[MAXPGPATH]; /* full path to my executable */
PG_SHARED char pkglib_path[MAXPGPATH];  /* full path to lib directory */

#ifdef EXEC_BACKEND
char postgres_exec_path[MAXPGPATH]; /* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

thread_local ProcNumber MyProcNumber = INVALID_PROC_NUMBER;

thread_local ProcNumber ParallelLeaderProcNumber = INVALID_PROC_NUMBER;

thread_local Oid MyDatabaseId = InvalidOid;

thread_local Oid MyDatabaseTableSpace = InvalidOid;

thread_local bool MyDatabaseHasLoginEventTriggers = false;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
thread_local char *DatabasePath = NULL;

PG_SHARED pid_t PostmasterPid = 0;

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
PG_SHARED bool IsPostmasterEnvironment = false;
thread_local bool IsUnderPostmaster = false;
PG_SHARED bool IsBinaryUpgrade = false;

thread_local bool ExitOnAnyError = false;

thread_local int DateStyle = USE_ISO_DATES;
thread_local int DateOrder = DATEORDER_MDY;
thread_local int IntervalStyle = INTSTYLE_POSTGRES;

thread_local bool enableFsync = true;
thread_local bool allowSystemTableMods = false;
thread_local int work_mem = 4096;
thread_local double hash_mem_multiplier = 2.0;
thread_local int maintenance_work_mem = 65536;

/*
 * Primary determinants of sizes of shared-memory structures.
 *
 * MaxBackends is computed by PostmasterMain after modules have had a chance to
 * register background workers.
 */
thread_local int NBuffers = 16384;
thread_local int MaxConnections = 100;
thread_local int max_worker_processes = 8;
PG_SHARED int MaxBackends = 0;

/* GUC parameters for vacuum */
thread_local int VacuumBufferUsageLimit = 2048;

thread_local int VacuumCostPageHit = 1;
thread_local int VacuumCostPageMiss = 2;
thread_local int VacuumCostPageDirty = 20;
thread_local int VacuumCostLimit = 200;
thread_local double VacuumCostDelay = 0;

thread_local int VacuumCostBalance = 0; /* working state for vacuum */
thread_local bool VacuumCostActive = false;

/* configurable SLRU buffer sizes */
thread_local int commit_timestamp_buffers = 0;
thread_local int multixact_member_buffers = 32;
thread_local int multixact_offset_buffers = 16;
thread_local int notify_buffers = 16;
thread_local int serializable_buffers = 32;
thread_local int subtransaction_buffers = 0;
thread_local int transaction_buffers = 0;
