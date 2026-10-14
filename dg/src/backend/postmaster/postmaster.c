/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.  Frontend programs connect to the Postmaster,
 *	  and postmaster forks a new backend process to handle the
 *	  connection.
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup and shutdown. The postmaster itself doesn't do those
 *	  operations, mind you --- it just forks off a subprocess to do them
 *	  at the right times.  It also takes care of resetting the system
 *	  if a backend crashes.
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.  Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up shared memory data structures
 *		for the backends.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the
 *Postmaster should never block on messages from frontend clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an
 *emergency exit and/or core dump.
 *
 * Error Reporting:
 *		Use write_stderr() only for reporting "interactive" errors
 *		(essentially, bogus arguments on the command line).  Once the
 *		postmaster is launched, use ereport().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef USE_BONJOUR
#include <dns_sd.h>
#endif

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "libpq/scram.h"
#include "pg_getopt.h"
#include "port/pg_bswap.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "postmaster/backend_thread.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/datadir.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/*
 * CountChildren and SignalChildren take a bitmask argument to represent
 * BackendTypes to count or signal.  Define a separate type and functions to
 * work with the bitmasks, to avoid accidentally passing a plain BackendType
 * in place of a bitmask or vice versa.
 */
typedef struct {
  uint32 mask;
} BackendTypeMask;

StaticAssertDecl(BACKEND_NUM_TYPES < 32, "too many backend types for uint32");

static const BackendTypeMask BTYPE_MASK_ALL = {(1 << BACKEND_NUM_TYPES) - 1};
static const BackendTypeMask BTYPE_MASK_NONE = {0};

static inline BackendTypeMask btmask(BackendType t) {
  BackendTypeMask mask = {.mask = 1 << t};

  return mask;
}

static inline BackendTypeMask
btmask_add_n(BackendTypeMask mask, std::initializer_list<BackendType> types) {
  for (BackendType t : types)
    mask.mask |= 1 << t;
  return mask;
}

#define btmask_add(mask, ...) btmask_add_n(mask, {__VA_ARGS__})

static inline BackendTypeMask btmask_del(BackendTypeMask mask, BackendType t) {
  mask.mask &= ~(1 << t);
  return mask;
}

static inline BackendTypeMask
btmask_all_except_n(std::initializer_list<BackendType> types) {
  BackendTypeMask mask = BTYPE_MASK_ALL;

  for (BackendType t : types)
    mask = btmask_del(mask, t);
  return mask;
}

#define btmask_all_except(...) btmask_all_except_n({__VA_ARGS__})

static inline bool btmask_contains(BackendTypeMask mask, BackendType t) {
  return (mask.mask & (1 << t)) != 0;
}

/* The socket number we are listening for connections on */
thread_local /*LOCAL*/ int PostPortNumber = DEF_PGPORT;

/* The directory names for Unix socket(s) */
thread_local /*LOCAL*/ char *Unix_socket_directories;

/* The TCP listen address(es) */
thread_local /*LOCAL*/ char *ListenAddresses;

/*
 * SuperuserReservedConnections is the number of backends reserved for
 * superuser use, and ReservedConnections is the number of backends reserved
 * for use by roles with privileges of the pg_use_reserved_connections
 * predefined role.  These are taken out of the pool of MaxConnections backend
 * slots, so the number of backend slots available for roles that are neither
 * superuser nor have privileges of pg_use_reserved_connections is
 * (MaxConnections - SuperuserReservedConnections - ReservedConnections).
 *
 * If the number of remaining slots is less than or equal to
 * SuperuserReservedConnections, only superusers can make new connections.  If
 * the number of remaining slots is greater than SuperuserReservedConnections
 * but less than or equal to
 * (SuperuserReservedConnections + ReservedConnections), only superusers and
 * roles with privileges of pg_use_reserved_connections can make new
 * connections.  Note that pre-existing superuser and
 * pg_use_reserved_connections connections don't count against the limits.
 */
thread_local /*LOCAL*/ int SuperuserReservedConnections;
thread_local /*LOCAL*/ int ReservedConnections;

/* The socket(s) we're listening to. */
#define MAXLISTEN	64
static thread_local /*SHARED*/ int NumListenSockets = 0;
static thread_local /*SHARED*/ pgsocket *ListenSockets = NULL;

/* still more option variables */
thread_local /*LOCAL*/ bool EnableSSL = false;

thread_local /*LOCAL*/ int PreAuthDelay = 0;
thread_local /*LOCAL*/ int AuthenticationTimeout = 60;

thread_local /*LOCAL*/ bool log_hostname; /* for ps display and logging */

thread_local /*LOCAL*/ bool enable_bonjour = false;
thread_local /*LOCAL*/ char *bonjour_name;

/*
 * When terminating child processes after fatal errors, like a crash of a
 * child process, we normally send SIGQUIT -- and most other comments in this
 * file are written on the assumption that we do -- but developers might
 * prefer to use SIGABRT to collect per-child core dumps.
 */
thread_local /*LOCAL*/ bool send_abort_for_crash = false;
thread_local /*LOCAL*/ bool send_abort_for_kill = false;

/* special child processes; NULL when not running */

/* Startup/shutdown state */
#define NoShutdown		0
#define SmartShutdown	1
#define FastShutdown	2
#define ImmediateShutdown	3

static thread_local /*SHARED*/ int Shutdown = NoShutdown;

static thread_local /*SHARED*/ bool FatalError =
    false; /* T if recovering from backend crash */

/*
 * We use a simple state machine to control startup, shutdown, and
 * crash recovery (which is rather like shutdown followed by startup).
 *
 * After doing all the postmaster initialization work, we enter PM_RUN
 * state (StartRunning()).  There is no WAL to recover from, so there is no
 * startup process and no archive recovery or hot standby.
 *
 * Normal child backends can only be launched when we are in PM_RUN state.
 * (connsAllowed can also restrict launching.)
 * In other states we handle connection requests by launching "dead-end"
 * child processes, which will simply send the client an error message and
 * quit.  (We track these in the ActiveChildList so that we can know when they
 * are all gone; this is important because they're still connected to shared
 * memory, and would interfere with an attempt to destroy the shmem segment,
 * possibly leading to SHMALL failure when we try to make a new one.)
 * In PM_WAIT_DEAD_END state we are waiting for all the dead-end children
 * to drain out of the system, and therefore stop accepting connection
 * requests at all until the last existing child has quit (which hopefully
 * will not be very long).
 *
 * Notice that this state variable does not distinguish *why* we entered
 * states later than PM_RUN --- Shutdown and FatalError must be consulted
 * to find that out.  FatalError is never true in PM_RUN state (because we
 * don't enter it when trying to recover from a crash).
 */
typedef enum {
  PM_INIT,          /* postmaster starting */
  PM_RUN,           /* normal "database is alive" state */
  PM_STOP_BACKENDS, /* need to stop remaining backends */
  PM_WAIT_BACKENDS, /* waiting for live backends to exit */
  PM_WAIT_DEAD_END, /* waiting for dead-end children to exit */
  PM_NO_CHILDREN,   /* all important children have exited */
} PMState;

static thread_local /*SHARED*/ PMState pmState = PM_INIT;

/*
 * While performing a "smart shutdown", we restrict new connections but stay
 * in PM_RUN state until all the client backends are gone.  connsAllowed is a
 * sub-state indicator showing the active restriction.  It is of no interest
 * unless pmState is PM_RUN.
 */
static thread_local /*SHARED*/ bool connsAllowed = true;

/* Start time of SIGKILL timeout during immediate shutdown or child crash */
/* Zero means timeout is not running */
static thread_local /*SHARED*/ time_t AbortStartTime = 0;

/* Length of said timeout */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static thread_local /*SHARED*/ bool ReachedNormalRunning =
    false; /* T if we've reached PM_RUN */

thread_local /*LOCAL*/ bool ClientAuthInProgress = false; /* T during new-client
                                                           * authentication */

/* set when there's a worker that needs to be started up */

/* set when signals arrive */
static thread_local /*SHARED*/ volatile sig_atomic_t pending_pm_reload_request;
static thread_local /*SHARED*/ volatile sig_atomic_t
    pending_pm_shutdown_request;
static thread_local /*SHARED*/ volatile sig_atomic_t
    pending_pm_fast_shutdown_request;
static thread_local /*SHARED*/ volatile sig_atomic_t
    pending_pm_immediate_shutdown_request;

/* event multiplexing object */
static thread_local /*SHARED*/ WaitEventSet *pm_wait_set;

#ifdef USE_SSL
/* Set when and if SSL has been initialized properly */
bool LoadedSSL = false;
#endif

#ifdef USE_BONJOUR
static DNSServiceRef bonjour_sdref = NULL;
#endif

/*
 * postmaster.c - function prototypes
 */
static void CloseServerPorts(int status, Datum arg);
static void unlink_external_pid_file(int status, Datum arg);
static void getInstallationPaths(const char *argv0);
static void make_legacy_dirs(void);
static void handle_pm_reload_request_signal(SIGNAL_ARGS);
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS);
static void process_pm_child_exit(void);
static void process_pm_reload_request(void);
static void process_pm_shutdown_request(void);
static void CleanupBackend(PMChild *bp, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname, int pid,
                         int exitstatus);
static void PostmasterStateMachine(void);
static void UpdatePMState(PMState newState);

pg_noreturn static void ExitPostmaster(int status);
static int ServerLoop(void);
static void StartRunning(void);
static int BackendStartup(ClientSocket *client_sock);
static void report_fork_failure_to_client(ClientSocket *client_sock,
                                          int errnum);
static CAC_state canAcceptConnections(BackendType backend_type);
static void signal_child(PMChild *pmchild, int signal);
static bool SignalChildren(int signal, BackendTypeMask targetMask);
static void TerminateChildren(int signal);
static int CountChildren(BackendTypeMask targetMask);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static void InitPostmasterDeathWatchHandle(void);

/* Macros to check exit status of a child process */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)

#ifndef WIN32
/*
 * File descriptors for pipe used to monitor if postmaster is alive.
 * First is POSTMASTER_FD_WATCH, second is POSTMASTER_FD_OWN.
 */
thread_local /*SHARED*/ int postmaster_alive_fds[2] = {-1, -1};
#else
/* Process handle of postmaster used for the same purpose on Windows */
HANDLE PostmasterHandle;
#endif

/*
 * Postmaster main entry point
 */
void PostmasterMain(int argc, char *argv[]) {
  int opt;
  int status;
  char *userDoption = NULL;
  bool listen_addr_saved = false;
  char *output_config_variable = NULL;

  InitProcessGlobals();

  PostmasterPid = MyProcPid;

  IsPostmasterEnvironment = true;

  /*
   * Start our win32 signal implementation
   */
#ifdef WIN32
  pgwin32_signal_initialize();
#endif

  /*
   * We should not be creating any files or directories before we check the
   * data directory (see checkDataDir()), but just in case set the umask to
   * the most restrictive (owner-only) permissions.
   *
   * checkDataDir() will reset the umask based on the data directory
   * permissions.
   */
  umask(PG_MODE_MASK_OWNER);

  /*
   * By default, palloc() requests in the postmaster will be allocated in
   * the PostmasterContext, which is space that can be recycled by backends.
   * Allocated data that needs to be available to backends should be
   * allocated in TopMemoryContext.
   */
  PostmasterContext = AllocSetContextCreate(TopMemoryContext, "Postmaster",
                                            ALLOCSET_DEFAULT_SIZES);
  MemoryContextSwitchTo(PostmasterContext);

  /* Initialize paths to installation files */
  getInstallationPaths(argv[0]);

  /*
   * Set up signal handlers for the postmaster process.
   *
   * CAUTION: when changing this list, check for side-effects on the signal
   * handling setup of child processes.  See tcop/postgres.c,
   * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/walwriter.c,
   * and postmaster/pgarch.c.
   */
  pqinitmask();
  sigprocmask(SIG_SETMASK, &BlockSig, NULL);

  pqsignal(SIGHUP, handle_pm_reload_request_signal);
  pqsignal(SIGINT, handle_pm_shutdown_request_signal);
  pqsignal(SIGQUIT, handle_pm_shutdown_request_signal);
  pqsignal(SIGTERM, handle_pm_shutdown_request_signal);
  pqsignal(SIGALRM, SIG_IGN); /* ignored */
  pqsignal(SIGPIPE, SIG_IGN); /* ignored */
  pqsignal(SIGUSR1, SIG_IGN); /* ignored: no log rotation, nor pmsignals */
  pqsignal(SIGUSR2, SIG_IGN); /* ignored */

  /* The wake fd, which the signal handlers' SetLatch() writes. */
  InitializeWaitEventSupport();
  InitProcessLocalLatch();

  /*
   * No other place in Postgres should touch SIGTTIN/SIGTTOU handling.  We
   * ignore those signals in a postmaster environment, so that there is no
   * risk of a child process freezing up due to writing to stderr.  But for
   * a standalone backend, their default handling is reasonable.  Hence, all
   * child processes should just allow the inherited settings to stand.
   */
#ifdef SIGTTIN
  pqsignal(SIGTTIN, SIG_IGN); /* ignored */
#endif
#ifdef SIGTTOU
  pqsignal(SIGTTOU, SIG_IGN); /* ignored */
#endif

  /* ignore SIGXFSZ, so that ulimit violations work like disk full */
#ifdef SIGXFSZ
  pqsignal(SIGXFSZ, SIG_IGN); /* ignored */
#endif

  /* Begin accepting signals. */
  sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

  /*
   * Options setup
   */
  InitializeGUCOptions();

  opterr = 1;

  /*
   * Parse command-line options.  CAUTION: keep this in sync with
   * tcop/postgres.c (the option sets should not conflict) and with the
   * common help() function in main/main.c.
   */
  while ((opt = getopt(argc, argv,
                       "B:bC:c:D:d:EeFf:h:ijk:lN:OPp:r:S:sTt:W:-:")) != -1) {
    switch (opt) {
    case 'B':
      SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'b':
      /* Undocumented flag used for binary upgrades */
      IsBinaryUpgrade = true;
      break;

    case 'C':
      output_config_variable = strdup(optarg);
      break;

    case '-':

      /*
       * Error if the user misplaced a special must-be-first option
       * for dispatching to a subprogram.  parse_dispatch_option()
       * returns DISPATCH_POSTMASTER if it doesn't find a match, so
       * error for anything else.
       */
      if (parse_dispatch_option(optarg) != DISPATCH_POSTMASTER)
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg("--%s must be first argument", optarg)));

      /* FALLTHROUGH */
    case 'c': {
      char *name, *value;

      ParseLongOption(optarg, &name, &value);
      if (!value) {
        if (opt == '-')
          ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                          errmsg("--%s requires a value", optarg)));
        else
          ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                          errmsg("-c %s requires a value", optarg)));
      }

      SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
      pfree(name);
      pfree(value);
      break;
    }

    case 'D':
      userDoption = strdup(optarg);
      break;

    case 'd':
      set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'E':
      SetConfigOption("log_statement", "all", PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'e':
      SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'F':
      SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'f':
      if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV)) {
        write_stderr("%s: invalid argument for option -f: \"%s\"\n", progname,
                     optarg);
        ExitPostmaster(1);
      }
      break;

    case 'h':
      SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'i':
      SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'j':
      /* only used by interactive backend */
      break;

    case 'k':
      SetConfigOption("unix_socket_directories", optarg, PGC_POSTMASTER,
                      PGC_S_ARGV);
      break;

    case 'l':
      SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'N':
      SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'O':
      SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER,
                      PGC_S_ARGV);
      break;

    case 'P':
      SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER,
                      PGC_S_ARGV);
      break;

    case 'p':
      SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 'r':
      /* only used by single-user backend */
      break;

    case 'S':
      SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    case 's':
      SetConfigOption("log_statement_stats", "true", PGC_POSTMASTER,
                      PGC_S_ARGV);
      break;

    case 'T':

      /*
       * This option used to be defined as sending SIGSTOP after a
       * backend crash, but sending SIGABRT seems more useful.
       */
      SetConfigOption("send_abort_for_crash", "true", PGC_POSTMASTER,
                      PGC_S_ARGV);
      break;

    case 't': {
      const char *tmp = get_stats_option_name(optarg);

      if (tmp) {
        SetConfigOption(tmp, "true", PGC_POSTMASTER, PGC_S_ARGV);
      } else {
        write_stderr("%s: invalid argument for option -t: \"%s\"\n", progname,
                     optarg);
        ExitPostmaster(1);
      }
      break;
    }

    case 'W':
      SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
      break;

    default:
      write_stderr("Try \"%s --help\" for more information.\n", progname);
      ExitPostmaster(1);
    }
  }

  /*
   * Postmaster accepts no non-option switch arguments.
   */
  if (optind < argc) {
    write_stderr("%s: invalid argument: \"%s\"\n", progname, argv[optind]);
    write_stderr("Try \"%s --help\" for more information.\n", progname);
    ExitPostmaster(1);
  }

  /*
   * Locate the proper configuration files and data directory, and read
   * postgresql.conf for the first time.
   */
  if (!SelectConfigFiles(userDoption, progname))
    ExitPostmaster(2);

  if (output_config_variable != NULL) {
    /*
     * If this is a runtime-computed GUC, it hasn't yet been initialized,
     * and the present value is not useful.  However, this is a convenient
     * place to print the value for most GUCs because it is safe to run
     * postmaster startup to this point even if the server is already
     * running.  For the handful of runtime-computed GUCs that we cannot
     * provide meaningful values for yet, we wait until later in
     * postmaster startup to print the value.  We won't be able to use -C
     * on running servers for those GUCs, but using this option now would
     * lead to incorrect results for them.
     */
    int flags = GetConfigOptionFlags(output_config_variable, true);

    if ((flags & GUC_RUNTIME_COMPUTED) == 0) {
      /*
       * "-C guc" was specified, so print GUC's value and exit.  No
       * extra permission check is needed because the user is reading
       * inside the data dir.
       */
      const char *config_val =
          GetConfigOption(output_config_variable, false, false);

      puts(config_val ? config_val : "");
      ExitPostmaster(0);
    }

    /*
     * A runtime-computed GUC will be printed later on.  As we initialize
     * a server startup sequence, silence any log messages that may show
     * up in the output generated.  FATAL and more severe messages are
     * useful to show, even if one would only expect at least PANIC.  LOG
     * entries are hidden.
     */
    SetConfigOption("log_min_messages", "FATAL", PGC_SUSET, PGC_S_OVERRIDE);
  }

  /* Verify that DataDir looks reasonable */
  checkDataDir();

  /* And switch working directory into it */
  ChangeToDataDir();

  /* Make the directories PostgreSQL's storage still looks for */
  make_legacy_dirs();

  /* Read the superuser and the databases, which every backend inherits */
  datadir_load();

  /*
   * Check for invalid combinations of GUC settings.
   */
  if (SuperuserReservedConnections + ReservedConnections >= MaxConnections) {
    write_stderr("%s: \"superuser_reserved_connections\" (%d) plus "
                 "\"reserved_connections\" (%d) must be less than "
                 "\"max_connections\" (%d)\n",
                 progname, SuperuserReservedConnections, ReservedConnections,
                 MaxConnections);
    ExitPostmaster(1);
  }

  /*
   * Other one-time internal sanity checks can go here, if they are fast.
   * (Put any slow processing further down, after postmaster.pid creation.)
   */
  if (!CheckDateTokenTables()) {
    write_stderr("%s: invalid datetoken tables, please fix\n", progname);
    ExitPostmaster(1);
  }

  /*
   * Now that we are done processing the postmaster arguments, reset
   * getopt(3) library so that it will work correctly in subprocesses.
   */
  optind = 1;
#ifdef HAVE_INT_OPTRESET
  optreset = 1; /* some systems need this too */
#endif

  /* For debugging: display postmaster environment */
  if (message_level_is_interesting(DEBUG3)) {
#if !defined(WIN32) || defined(_MSC_VER)
    extern char **environ;
#endif
    char **p;
    StringInfoData si;

    initStringInfo(&si);

    appendStringInfoString(&si, "initial environment dump:");
    for (p = environ; *p; ++p)
      appendStringInfo(&si, "\n%s", *p);

    ereport(DEBUG3, errmsg_internal("%s", si.data));
    pfree(si.data);
  }

  /*
   * Create lockfile for data directory.
   *
   * We want to do this before we try to grab the input sockets, because the
   * data directory interlock is more reliable than the socket-file
   * interlock (thanks to whoever decided to put socket files in /tmp :-().
   * For the same reason, it's best to grab the TCP socket(s) before the
   * Unix socket(s).
   *
   * Also note that this internally sets up the on_proc_exit function that
   * is responsible for removing both data directory and socket lockfiles;
   * so it must happen before opening sockets so that at exit, the socket
   * lockfiles go away after CloseServerPorts runs.
   */
  CreateDataDirLockFile(true);

  /* Make the nonce for mock authentication, which sessions copy */
  scram_init_mock_nonce();

  /*
   * process any libraries that should be preloaded at postmaster start
   */
  process_shared_preload_libraries();

  /*
   * Initialize SSL library, if specified.
   */
#ifdef USE_SSL
  if (EnableSSL) {
    (void)secure_initialize(true);
    LoadedSSL = true;
  }
#endif

  /*
   * Now that loadable modules have had their chance to alter any GUCs,
   * calculate MaxBackends and initialize the machinery to track child
   * processes.
   */
  InitializeMaxBackends();
  InitPostmasterChildSlots();

  /*
   * Calculate the size of the PGPROC fast-path lock arrays.
   */
  InitializeFastPathLocks();

  /*
   * Give preloaded libraries a chance to request additional shared memory.
   */
  process_shmem_requests();

  /*
   * Now that loadable modules have had their chance to request additional
   * shared memory, determine the value of any runtime-computed GUCs that
   * depend on the amount of shared memory required.
   */
  InitializeShmemGUCs();

  /*
   * If -C was specified with a runtime-computed GUC, we held off printing
   * the value earlier, as the GUC was not yet initialized.  We handle -C
   * for most GUCs before we lock the data directory so that the option may
   * be used on a running server.  However, a handful of GUCs are runtime-
   * computed and do not have meaningful values until after locking the data
   * directory, and we cannot safely calculate their values earlier on a
   * running server.  At this point, such GUCs should be properly
   * initialized, and we haven't yet set up shared memory, so this is a good
   * time to handle the -C option for these special GUCs.
   */
  if (output_config_variable != NULL) {
    const char *config_val =
        GetConfigOption(output_config_variable, false, false);

    puts(config_val ? config_val : "");
    ExitPostmaster(0);
  }

  /*
   * Set up shared memory and semaphores.
   *
   * Note: if using SysV shmem and/or semas, each postmaster startup will
   * normally choose the same IPC keys.  This helps ensure that we will
   * clean up dead IPC objects if the postmaster crashes and is restarted.
   */
  CreateSharedMemoryAndSemaphores();

  /*
   * Estimate number of openable files.  This must happen after setting up
   * semaphores, because on some platforms semaphores count as open files.
   */
  set_max_safe_fds();

  /*
   * Initialize pipe (or process handle on Windows) that allows children to
   * wake up from sleep on postmaster death.
   */
  InitPostmasterDeathWatchHandle();

  /*
   * Reset whereToSendOutput from DestDebug (its starting state) to
   * DestNone. This stops ereport from sending log messages to stderr unless
   * Log_destination permits.  We don't do this until the postmaster is
   * fully launched, since startup failures may as well be reported to
   * stderr.
   *
   * If we are in fact disabling logging to stderr, first emit a log message
   * saying so, to provide a breadcrumb trail for users who may not remember
   * that their logging is configured to go somewhere else.
   */
  if (!(Log_destination & LOG_DESTINATION_STDERR))
    ereport(LOG,
            (errmsg("ending log output to stderr"),
             errhint("Future log output will go to log destination \"%s\".",
                     Log_destination_string)));

  whereToSendOutput = DestNone;

  /*
   * Report server startup in log.  While we could emit this much earlier,
   * it seems best to do so after starting the log collector, if we intend
   * to use one.
   */
  ereport(LOG, (errmsg("starting %s", PG_VERSION_STR)));

  /*
   * Establish input sockets.
   *
   * First set up an on_proc_exit function that's charged with closing the
   * sockets again at postmaster shutdown.
   */
  ListenSockets = (pgsocket *)palloc(MAXLISTEN * sizeof(pgsocket));
  on_proc_exit(CloseServerPorts, 0);

  if (ListenAddresses) {
    char *rawstring;
    List *elemlist;
    ListCell *l;
    int success = 0;

    /* Need a modifiable copy of ListenAddresses */
    rawstring = pstrdup(ListenAddresses);

    /* Parse string into list of hostnames */
    if (!SplitGUCList(rawstring, ',', &elemlist)) {
      /* syntax error in list */
      ereport(FATAL, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid list syntax in parameter \"%s\"",
                             "listen_addresses")));
    }

    foreach (l, elemlist) {
      char *curhost = (char *)lfirst(l);

      if (strcmp(curhost, "*") == 0)
        status =
            ListenServerPort(AF_UNSPEC, NULL, (unsigned short)PostPortNumber,
                             NULL, ListenSockets, &NumListenSockets, MAXLISTEN);
      else
        status =
            ListenServerPort(AF_UNSPEC, curhost, (unsigned short)PostPortNumber,
                             NULL, ListenSockets, &NumListenSockets, MAXLISTEN);

      if (status == STATUS_OK) {
        success++;
        /* record the first successful host addr in lockfile */
        if (!listen_addr_saved) {
          AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
          listen_addr_saved = true;
        }
      } else
        ereport(WARNING,
                (errmsg("could not create listen socket for \"%s\"", curhost)));
    }

    if (!success && elemlist != NIL)
      ereport(FATAL, (errmsg("could not create any TCP/IP sockets")));

    list_free(elemlist);
    pfree(rawstring);
  }

#ifdef USE_BONJOUR
  /* Register for Bonjour only if we opened TCP socket(s) */
  if (enable_bonjour && NumListenSockets > 0) {
    DNSServiceErrorType err;

    /*
     * We pass 0 for interface_index, which will result in registering on
     * all "applicable" interfaces.  It's not entirely clear from the
     * DNS-SD docs whether this would be appropriate if we have bound to
     * just a subset of the available network interfaces.
     */
    err = DNSServiceRegister(&bonjour_sdref, 0, 0, bonjour_name,
                             "_postgresql._tcp.", NULL, NULL,
                             pg_hton16(PostPortNumber), 0, NULL, NULL, NULL);
    if (err != kDNSServiceErr_NoError)
      ereport(LOG, (errmsg("DNSServiceRegister() failed: error code %ld",
                           (long)err)));

    /*
     * We don't bother to read the mDNS daemon's reply, and we expect that
     * it will automatically terminate our registration when the socket is
     * closed at postmaster termination.  So there's nothing more to be
     * done here.  However, the bonjour_sdref is kept around so that
     * forked children can close their copies of the socket.
     */
  }
#endif

  if (Unix_socket_directories) {
    char *rawstring;
    List *elemlist;
    ListCell *l;
    int success = 0;

    /* Need a modifiable copy of Unix_socket_directories */
    rawstring = pstrdup(Unix_socket_directories);

    /* Parse string into list of directories */
    if (!SplitDirectoriesString(rawstring, ',', &elemlist)) {
      /* syntax error in list */
      ereport(FATAL, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid list syntax in parameter \"%s\"",
                             "unix_socket_directories")));
    }

    foreach (l, elemlist) {
      char *socketdir = (char *)lfirst(l);

      status = ListenServerPort(AF_UNIX, NULL, (unsigned short)PostPortNumber,
                                socketdir, ListenSockets, &NumListenSockets,
                                MAXLISTEN);

      if (status == STATUS_OK) {
        success++;
        /* record the first successful Unix socket in lockfile */
        if (success == 1)
          AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
      } else
        ereport(
            WARNING,
            (errmsg("could not create Unix-domain socket in directory \"%s\"",
                    socketdir)));
    }

    if (!success && elemlist != NIL)
      ereport(FATAL, (errmsg("could not create any Unix-domain sockets")));

    list_free_deep(elemlist);
    pfree(rawstring);
  }

  /*
   * check that we have some socket to listen on
   */
  if (NumListenSockets == 0)
    ereport(FATAL, (errmsg("no socket created for listening")));

  /*
   * If no valid TCP ports, write an empty line for listen address,
   * indicating the Unix socket must be used.  Note that this line is not
   * added to the lock file until there is a socket backing it.
   */
  if (!listen_addr_saved)
    AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

  /*
   * Record postmaster options.  We delay this till now to avoid recording
   * bogus options (eg, unusable port number).
   */
  if (!CreateOptsFile(argc, argv, my_exec_path))
    ExitPostmaster(1);

  /*
   * Write the external PID file if requested
   */
  if (external_pid_file) {
    FILE *fpidfile = fopen(external_pid_file, "w");

    if (fpidfile) {
      fprintf(fpidfile, "%d\n", MyProcPid);
      fclose(fpidfile);

      /* Make PID file world readable */
      if (chmod(external_pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
        write_stderr("%s: could not change permissions of external PID file "
                     "\"%s\": %m\n",
                     progname, external_pid_file);
    } else
      write_stderr("%s: could not write external PID file \"%s\": %m\n",
                   progname, external_pid_file);

    on_proc_exit(unlink_external_pid_file, 0);
  }

  /*
   * Remove old temporary files.  At this point there can be no other
   * Postgres processes running in this directory, so this should be safe.
   */
  RemovePgTempFiles();

  /*
   * There are no configuration files for client authentication to load:
   * how a client is authenticated is fixed (WORKNOTE.md step 8).
   */

#ifdef HAVE_PTHREAD_IS_THREADED_NP

  /*
   * On macOS, libintl replaces setlocale() with a version that calls
   * CFLocaleCopyCurrent() when its second argument is "" and every relevant
   * environment variable is unset or empty.  CFLocaleCopyCurrent() makes
   * the process multithreaded.  The postmaster calls sigprocmask() and
   * calls fork() without an immediate exec(), both of which have undefined
   * behavior in a multithreaded program.  A multithreaded postmaster is the
   * normal case on Windows, which offers neither fork() nor sigprocmask().
   * Currently, macOS is the only platform having pthread_is_threaded_np(),
   * so we need not worry whether this HINT is appropriate elsewhere.
   */
  if (pthread_is_threaded_np() != 0)
    ereport(
        FATAL,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("postmaster became multithreaded during startup"),
         errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

  /*
   * Remember postmaster startup time
   */
  PgStartTime = GetCurrentTimestamp();

  /*
   * Report postmaster status in the postmaster.pid file, to allow pg_ctl to
   * see what's happening.
   */
  AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

  /* We're ready to rock and roll... */
  StartRunning();

  status = ServerLoop();

  /*
   * ServerLoop probably shouldn't ever return, but if it does, close down.
   */
  ExitPostmaster(status != STATUS_OK);

  abort(); /* not reached */
}

/*
 * on_proc_exit callback to close server's listen sockets
 */
static void CloseServerPorts(int status, Datum arg) {
  int i;

  /*
   * First, explicitly close all the socket FDs.  We used to just let this
   * happen implicitly at postmaster exit, but it's better to close them
   * before we remove the postmaster.pid lockfile; otherwise there's a race
   * condition if a new postmaster wants to re-use the TCP port number.
   */
  for (i = 0; i < NumListenSockets; i++) {
    if (closesocket(ListenSockets[i]) != 0)
      elog(LOG, "could not close listen socket: %m");
  }
  NumListenSockets = 0;

  /*
   * Next, remove any filesystem entries for Unix sockets.  To avoid race
   * conditions against incoming postmasters, this must happen after closing
   * the sockets and before removing lock files.
   */
  RemoveSocketFiles();

  /*
   * We don't do anything about socket lock files here; those will be
   * removed in a later on_proc_exit callback.
   */
}

/*
 * on_proc_exit callback to delete external_pid_file
 */
static void unlink_external_pid_file(int status, Datum arg) {
  if (external_pid_file)
    unlink(external_pid_file);
}

/*
 * Compute and check the directory paths to files that are part of the
 * installation (as deduced from the postgres executable's own location)
 */
static void getInstallationPaths(const char *argv0) {
  /* Locate the postgres executable itself */
  if (find_my_exec(argv0, my_exec_path) < 0)
    ereport(FATAL,
            (errmsg("%s: could not locate my own executable path", argv0)));

  /*
   * Locate the pkglib directory --- this has to be set early in case we try
   * to load any modules from it in response to postgresql.conf entries.
   */
  get_pkglib_path(my_exec_path, pkglib_path);

  /*
   * digres does not look for the directory: it is one program, and runs
   * from where it was built.
   */
}

/*
 * Make, in the data directory, the directories that PostgreSQL's initdb
 * makes and digres's does not. The subsystems that open them at startup
 * are still here, though sm is to replace them (sm/PART4.md section 10);
 * a name leaves this list when its subsystem is cut.
 */
static void make_legacy_dirs(void) {
  static const char *const dirs[] = {
      "pg_dynshmem",          "pg_snapshots",
      "pg_replslot",          "pg_stat",
      "pg_stat_tmp",          "pg_logical",
      "pg_logical/snapshots", "pg_logical/mappings"};

  for (int i = 0; i < lengthof(dirs); i++)
    if (mkdir(dirs[i], pg_dir_create_mode) != 0 && errno != EEXIST)
      ereport(FATAL,
              (errcode_for_file_access(),
               errmsg("could not create directory \"%s\": %m", dirs[i])));
}

/*
 * Determine how long should we let ServerLoop sleep, in milliseconds.
 *
 * In normal conditions we wait at most one minute, to ensure that the other
 * background tasks handled by ServerLoop get done even when no requests are
 * arriving.  However, if there are background workers waiting to be started,
 * we don't actually sleep so that they are quickly serviced.  Other exception
 * cases are as shown in the code.
 */
static int DetermineSleepTime(void) {
  if (AbortStartTime != 0) {
    int seconds;

    /* time left to abort; clamp to 0 in case it already expired */
    seconds = SIGKILL_CHILDREN_AFTER_SECS - (time(NULL) - AbortStartTime);

    return Max(seconds * 1000, 0);
  }
  return 60 * 1000;
}

/*
 * Activate or deactivate notifications of server socket events.  Since we
 * don't currently have a way to remove events from an existing WaitEventSet,
 * we'll just destroy and recreate the whole thing.  This is called during
 * shutdown so we can wait for backends to exit without accepting new
 * connections, and during crash reinitialization when we need to start
 * listening for new connections again.  The WaitEventSet will be freed in fork
 * children by ClosePostmasterPorts().
 */
static void ConfigurePostmasterWaitSet(bool accept_connections) {
  if (pm_wait_set)
    FreeWaitEventSet(pm_wait_set);
  pm_wait_set = NULL;

  pm_wait_set =
      CreateWaitEventSet(NULL, accept_connections ? (1 + NumListenSockets) : 1);
  AddWaitEventToSet(pm_wait_set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);

  if (accept_connections) {
    for (int i = 0; i < NumListenSockets; i++)
      AddWaitEventToSet(pm_wait_set, WL_SOCKET_ACCEPT, ListenSockets[i], NULL,
                        NULL);
  }
}

/*
 * Open for connections.  Shared memory is made, and with it the transaction
 * counters (VarsupShmemInit()): there is no WAL to recover from, so this is
 * what the startup process's clean exit used to do.
 */
static void StartRunning(void) {
  ReachedNormalRunning = true;
  UpdatePMState(PM_RUN);
  connsAllowed = true;

  /* at this point we are really open for business */
  ereport(LOG, (errmsg("database system is ready to accept connections")));

  /* Report status */
  AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
  sd_notify(0, "READY=1");
#endif
}

/*
 * Main idle loop of postmaster
 */
static int ServerLoop(void) {
  time_t last_lockfile_recheck_time, last_touch_time;
  WaitEvent events[MAXLISTEN];
  int nevents;

  ConfigurePostmasterWaitSet(true);
  last_lockfile_recheck_time = last_touch_time = time(NULL);

  for (;;) {
    time_t now;

    nevents = WaitEventSetWait(pm_wait_set, DetermineSleepTime(), events,
                               lengthof(events),
                               0 /* postmaster posts no wait_events */);

    /*
     * Latch set by signal handler, or new connection pending on any of
     * our sockets? If the latter, fork a child process to deal with it.
     */
    for (int i = 0; i < nevents; i++) {
      if (events[i].events & WL_LATCH_SET)
        ResetLatch(MyLatch);

      /*
       * The following requests are handled unconditionally, even if we
       * didn't see WL_LATCH_SET.  This gives high priority to shutdown
       * and reload requests where the latch happens to appear later in
       * events[] or will be reported by a later call to
       * WaitEventSetWait().
       */
      if (pending_pm_shutdown_request)
        process_pm_shutdown_request();
      if (pending_pm_reload_request)
        process_pm_reload_request();
      if (backend_thread_ended())
        process_pm_child_exit();

      if (events[i].events & WL_SOCKET_ACCEPT) {
        ClientSocket s;

        if (AcceptConnection(events[i].fd, &s) == STATUS_OK)
          BackendStartup(&s);

        /* We no longer need the open socket in this process */
        if (s.sock != PGINVALID_SOCKET) {
          if (closesocket(s.sock) != 0)
            elog(LOG, "could not close client socket: %m");
        }
      }
    }

#ifdef HAVE_PTHREAD_IS_THREADED_NP

    /*
     * With assertions enabled, check regularly for appearance of
     * additional threads.  All builds check at start and exit.
     */
    Assert(pthread_is_threaded_np() == 0);
#endif

    /*
     * Lastly, check to see if it's time to do some things that we don't
     * want to do every single time through the loop, because they're a
     * bit expensive.  Note that there's up to a minute of slop in when
     * these tasks will be performed, since DetermineSleepTime() will let
     * us sleep at most that long; except for SIGKILL timeout which has
     * special-case logic there.
     */
    now = time(NULL);

    /*
     * If we already sent SIGQUIT to children and they are slow to shut
     * down, it's time to send them SIGKILL (or SIGABRT if requested).
     * This doesn't happen normally, but under certain conditions backends
     * can get stuck while shutting down.  This is a last measure to get
     * them unwedged.
     *
     * Note we also do this during recovery from a process crash.
     */
    if ((Shutdown >= ImmediateShutdown || FatalError) && AbortStartTime != 0 &&
        (now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS) {
      /* We were gentle with them before. Not anymore */
      ereport(LOG,
              /* translator: %s is SIGKILL or SIGABRT */
              (errmsg("issuing %s to recalcitrant children",
                      send_abort_for_kill ? "SIGABRT" : "SIGKILL")));
      TerminateChildren(send_abort_for_kill ? SIGABRT : SIGKILL);
      /* reset flag so we don't SIGKILL again */
      AbortStartTime = 0;
    }

    /*
     * Once a minute, verify that postmaster.pid hasn't been removed or
     * overwritten.  If it has, we force a shutdown.  This avoids having
     * postmasters and child processes hanging around after their database
     * is gone, and maybe causing problems if a new database cluster is
     * created in the same place.  It also provides some protection
     * against a DBA foolishly removing postmaster.pid and manually
     * starting a new postmaster.  Data corruption is likely to ensue from
     * that anyway, but we can minimize the damage by aborting ASAP.
     */
    if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE) {
      if (!RecheckDataDirLockFile()) {
        ereport(LOG, (errmsg("performing immediate shutdown because data "
                             "directory lock file is invalid")));
        kill(MyProcPid, SIGQUIT);
      }
      last_lockfile_recheck_time = now;
    }

    /*
     * Touch Unix socket and lock files every 58 minutes, to ensure that
     * they are not removed by overzealous /tmp-cleaning tasks.  We assume
     * no one runs cleaners with cutoff times of less than an hour ...
     */
    if (now - last_touch_time >= 58 * SECS_PER_MINUTE) {
      TouchSocketFiles();
      TouchSocketLockFiles();
      last_touch_time = now;
    }
  }
}

/*
 * canAcceptConnections --- check to see if database state allows connections
 * of the specified type.  backend_type can only be B_BACKEND.
 * (Note that we don't yet know whether a normal B_BACKEND connection might
 * turn into a walsender.)
 */
static CAC_state canAcceptConnections(BackendType backend_type) {
  CAC_state result = CAC_OK;

  Assert(backend_type == B_BACKEND);

  /* Can't start backends when shutting down or recovering from a crash. */
  if (pmState != PM_RUN) {
    if (Shutdown > NoShutdown)
      return CAC_SHUTDOWN; /* shutdown is pending */
    else
      return CAC_RECOVERY; /* else must be crash recovery */
  }

  /*
   * "Smart shutdown" restrictions are applied only to normal connections.
   */
  if (!connsAllowed && backend_type == B_BACKEND)
    return CAC_SHUTDOWN; /* shutdown is pending */

  return result;
}

/*
 * InitProcessGlobals -- set MyStartTime[stamp], random seeds
 *
 * Called early in the postmaster and every backend.
 */
void InitProcessGlobals(void) {
  MyStartTimestamp = GetCurrentTimestamp();
  MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

  /*
   * Set a different global seed in every process.  We want something
   * unpredictable, so if possible, use high-quality random bits for the
   * seed.  Otherwise, fall back to a seed based on timestamp and PID.
   */
  if (unlikely(!pg_prng_strong_seed(&pg_global_prng_state))) {
    uint64 rseed;

    /*
     * Since PIDs and timestamps tend to change more frequently in their
     * least significant bits, shift the timestamp left to allow a larger
     * total number of seeds in a given time period.  Since that would
     * leave only 20 bits of the timestamp that cycle every ~1 second,
     * also mix in some higher bits.
     */
    rseed = ((uint64)MyProcPid) ^ ((uint64)MyStartTimestamp << 12) ^
            ((uint64)MyStartTimestamp >> 20);

    pg_prng_seed(&pg_global_prng_state, rseed);
  }

  /*
   * Also make sure that we've set a good seed for random(3).  Use of that
   * is deprecated in core Postgres, but extensions might use it.
   */
#ifndef WIN32
  srandom(pg_prng_uint32(&pg_global_prng_state));
#endif
}

/*
 * pg_ctl uses SIGHUP to request a reload of the configuration files.
 */
static void handle_pm_reload_request_signal(SIGNAL_ARGS) {
  pending_pm_reload_request = true;
  SetLatch(MyLatch);
}

/*
 * Re-read config files, and tell children to do same.
 */
static void process_pm_reload_request(void) {
  pending_pm_reload_request = false;

  ereport(DEBUG2,
          (errmsg_internal("postmaster received reload request signal")));

  if (Shutdown <= SmartShutdown) {
    ereport(LOG, (errmsg("received SIGHUP, reloading configuration files")));
    ProcessConfigFile(PGC_SIGHUP);
    SignalChildren(SIGHUP, btmask_all_except(B_DEAD_END_BACKEND));

#ifdef USE_SSL
    /* Reload SSL configuration as well */
    if (EnableSSL) {
      if (secure_initialize(false) == 0)
        LoadedSSL = true;
      else
        ereport(LOG, (errmsg("SSL configuration was not reloaded")));
    } else {
      secure_destroy();
      LoadedSSL = false;
    }
#endif
  }
}

/*
 * pg_ctl uses SIGTERM, SIGINT and SIGQUIT to request different types of
 * shutdown.
 */
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS) {
  switch (postgres_signal_arg) {
  case SIGTERM:
    /* smart is implied if the other two flags aren't set */
    pending_pm_shutdown_request = true;
    break;
  case SIGINT:
    pending_pm_fast_shutdown_request = true;
    pending_pm_shutdown_request = true;
    break;
  case SIGQUIT:
    pending_pm_immediate_shutdown_request = true;
    pending_pm_shutdown_request = true;
    break;
  }
  SetLatch(MyLatch);
}

/*
 * Process shutdown request.
 */
static void process_pm_shutdown_request(void) {
  int mode;

  ereport(DEBUG2,
          (errmsg_internal("postmaster received shutdown request signal")));

  pending_pm_shutdown_request = false;

  /*
   * If more than one shutdown request signal arrived since the last server
   * loop, take the one that is the most immediate.  That matches the
   * priority that would apply if we processed them one by one in any order.
   */
  if (pending_pm_immediate_shutdown_request) {
    pending_pm_immediate_shutdown_request = false;
    pending_pm_fast_shutdown_request = false;
    mode = ImmediateShutdown;
  } else if (pending_pm_fast_shutdown_request) {
    pending_pm_fast_shutdown_request = false;
    mode = FastShutdown;
  } else
    mode = SmartShutdown;

  switch (mode) {
  case SmartShutdown:

    /*
     * Smart Shutdown:
     *
     * Wait for children to end their work, then shut down.
     */
    if (Shutdown >= SmartShutdown)
      break;
    Shutdown = SmartShutdown;
    ereport(LOG, (errmsg("received smart shutdown request")));

    /* Report status */
    AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
    sd_notify(0, "STOPPING=1");
#endif

    /*
     * If we reached normal running, we go straight to waiting for
     * client backends to exit.  If already in PM_STOP_BACKENDS or a
     * later state, do not change it.
     */
    if (pmState == PM_RUN)
      connsAllowed = false;

    /*
     * Now wait for online backup mode to end and backends to exit. If
     * that is already the case, PostmasterStateMachine will take the
     * next step.
     */
    PostmasterStateMachine();
    break;

  case FastShutdown:

    /*
     * Fast Shutdown:
     *
     * Abort all children with SIGTERM (rollback active transactions
     * and exit) and shut down when they are gone.
     */
    if (Shutdown >= FastShutdown)
      break;
    Shutdown = FastShutdown;
    ereport(LOG, (errmsg("received fast shutdown request")));

    /* Report status */
    AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
    sd_notify(0, "STOPPING=1");
#endif

    if (pmState == PM_RUN) {
      /* Report that we're about to zap live client sessions */
      ereport(LOG, (errmsg("aborting any active transactions")));
      UpdatePMState(PM_STOP_BACKENDS);
    }

    /*
     * PostmasterStateMachine will issue any necessary signals, or
     * take the next step if no child processes need to be killed.
     */
    PostmasterStateMachine();
    break;

  case ImmediateShutdown:

    /*
     * Immediate Shutdown:
     *
     * abort all children with SIGQUIT, wait for them to exit,
     * terminate remaining ones with SIGKILL, then exit without
     * attempt to properly shut down the data base system.
     */
    if (Shutdown >= ImmediateShutdown)
      break;
    Shutdown = ImmediateShutdown;
    ereport(LOG, (errmsg("received immediate shutdown request")));

    /* Report status */
    AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
    sd_notify(0, "STOPPING=1");
#endif

    /* tell children to shut down ASAP */
    /* (note we don't apply send_abort_for_crash here) */
    SetQuitSignalReason(PMQUIT_FOR_STOP);
    TerminateChildren(SIGQUIT);
    UpdatePMState(PM_WAIT_BACKENDS);

    /* set stopwatch for them to die */
    AbortStartTime = time(NULL);

    /*
     * Now wait for backends to exit.  If there are none,
     * PostmasterStateMachine will take the next step.
     */
    PostmasterStateMachine();
    break;
  }
}

/*
 * Clean up after the backend threads that have ended.  Their ends are
 * queued, not waited for: the postmaster has no child process, and must not
 * reap one, since a session's popen() child is its own to wait for.
 */
static void process_pm_child_exit(void) {
  backend_thread_t *thread;
  int exitstatus;

  ereport(DEBUG4, (errmsg_internal("reaping ended backend threads")));

  while ((thread = backend_thread_exited(&exitstatus)) != NULL) {
    pid_t pid = backend_thread_pid(thread);
    PMChild *pmchild;

    backend_thread_free(thread);
    pmchild = FindPostmasterChildByPid(pid);
    if (pmchild) {
      pmchild->thread = NULL;
      CleanupBackend(pmchild, exitstatus);
    } else
      LogChildExit(LOG, _("untracked backend thread"), pid, exitstatus);
  }

  /* See if we have any state changes or actions to make. */
  PostmasterStateMachine();
}

/*
 * CleanupBackend -- cleanup after terminated backend or background worker.
 *
 * Remove all local state associated with the child process and release its
 * PMChild slot.
 */
static void CleanupBackend(PMChild *bp,
                           int exitstatus) /* child's exit status. */
{
  const char *procname;
  bool crashed = false;
  bool logged = false;
  pid_t bp_pid;

  /* Construct a process name for the log message */
  procname = _(GetBackendTypeDesc(bp->bkend_type));

  /*
   * If a backend dies in an ugly way then we must signal all other backends
   * to quickdie.  If exit status is zero (normal) or one (FATAL exit), we
   * assume everything is all right and proceed to remove the backend from
   * the active child list.
   */
  if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
    crashed = true;

#ifdef WIN32

  /*
   * On win32, also treat ERROR_WAIT_NO_CHILDREN (128) as nonfatal case,
   * since that sometimes happens under load when the process fails to start
   * properly (long before it starts using shared memory). Microsoft reports
   * it is related to mutex failure:
   * http://archives.postgresql.org/pgsql-hackers/2010-09/msg00790.php
   */
  if (exitstatus == ERROR_WAIT_NO_CHILDREN) {
    LogChildExit(LOG, procname, bp->pid, exitstatus);
    logged = true;
    crashed = false;
  }
#endif

  /*
   * Release the PMChild entry.
   *
   * If the process attached to shared memory, this also checks that it
   * detached cleanly.
   */
  bp_pid = bp->pid;
  if (!ReleasePostmasterChildSlot(bp)) {
    /*
     * Uh-oh, the child failed to clean itself up.  Treat as a crash after
     * all.
     */
    crashed = true;
  }
  bp = NULL;

  /* In a crash case, exit immediately. */
  if (crashed) {
    HandleChildCrash(bp_pid, exitstatus, procname);
    return;
  }

  if (!logged)
    LogChildExit(DEBUG2, procname, bp_pid, exitstatus);
}

/*
 * Transition into FatalError state, in response to something bad having
 * happened. Commonly the caller will have logged the reason for entering
 * FatalError state.
 *
 * This should only be called when not already in FatalError or
 * ImmediateShutdown state.
 */
static void HandleFatalError(QuitSignalReason reason, bool consider_sigabrt) {
  int sigtosend;

  Assert(!FatalError);
  Assert(Shutdown != ImmediateShutdown);

  SetQuitSignalReason(reason);

  if (consider_sigabrt && send_abort_for_crash)
    sigtosend = SIGABRT;
  else
    sigtosend = SIGQUIT;

  /*
   * Signal all other child processes to exit.
   *
   * We could exclude dead-end children here, but at least when sending
   * SIGABRT it seems better to include them.
   */
  TerminateChildren(sigtosend);

  FatalError = true;

  /*
   * Choose the appropriate new state to react to the fatal error. Unless we
   * were already in the process of shutting down, we go through
   * PM_WAIT_BACKENDS. For errors during the shutdown sequence, we directly
   * switch to PM_WAIT_DEAD_END.
   */
  switch (pmState) {
  case PM_INIT:
    /* shouldn't have any children */
    Assert(false);
    break;

    /* wait for children to die */
  case PM_RUN:
  case PM_STOP_BACKENDS:
    UpdatePMState(PM_WAIT_BACKENDS);
    break;

  case PM_WAIT_BACKENDS:
    /* there might be more backends to wait for */
    break;

  case PM_WAIT_DEAD_END:
  case PM_NO_CHILDREN:
    break;
  }

  /*
   * .. and if this doesn't happen quickly enough, now the clock is ticking
   * for us to kill them without mercy.
   */
  if (AbortStartTime == 0)
    AbortStartTime = time(NULL);
}

/*
 * HandleChildCrash -- cleanup after failed backend, bgwriter, checkpointer,
 * slot sync worker, or background worker.
 *
 * The objectives here are to clean up our local state about the child
 * process, and to signal all other remaining children to quickdie.
 *
 * The caller has already released its PMChild slot.
 */
static void HandleChildCrash(int pid, int exitstatus, const char *procname) {
  /*
   * We only log messages and send signals if this is the first process
   * crash and we're not doing an immediate shutdown; otherwise, we're only
   * here to update postmaster's idea of live processes.  If we have already
   * signaled children, nonzero exit status is to be expected, so don't
   * clutter log.
   */
  if (FatalError || Shutdown == ImmediateShutdown)
    return;

  LogChildExit(LOG, procname, pid, exitstatus);
  ereport(LOG, (errmsg("terminating any other active server processes")));

  /*
   * Switch into error state. The crashed process has already been removed
   * from ActiveChildList.
   */
  HandleFatalError(PMQUIT_FOR_CRASH, true);
}

/*
 * Log the death of a child process.
 */
static void LogChildExit(int lev, const char *procname, int pid,
                         int exitstatus) {
  if (WIFEXITED(exitstatus))
    ereport(lev,

            /*------
              translator: %s is a noun phrase describing a child process, such
              as "server process" */
            (errmsg("%s (PID %d) exited with exit code %d", procname, pid,
                    WEXITSTATUS(exitstatus))));
  else if (WIFSIGNALED(exitstatus)) {
#if defined(WIN32)
    ereport(
        lev,

        /*------
          translator: %s is a noun phrase describing a child process, such as
          "server process" */
        (errmsg("%s (PID %d) was terminated by exception 0x%X", procname, pid,
                WTERMSIG(exitstatus)),
         errhint("See C include file \"ntstatus.h\" for a description of the "
                 "hexadecimal value.")));
#else
    ereport(
        lev,

        /*------
          translator: %s is a noun phrase describing a child process, such as
          "server process" */
        (errmsg("%s (PID %d) was terminated by signal %d: %s", procname, pid,
                WTERMSIG(exitstatus), pg_strsignal(WTERMSIG(exitstatus)))));
#endif
  } else
    ereport(lev,

            /*------
              translator: %s is a noun phrase describing a child process, such
              as "server process" */
            (errmsg("%s (PID %d) exited with unrecognized status %d", procname,
                    pid, exitstatus)));
}

/*
 * Advance the postmaster's state machine and take actions as appropriate
 *
 * This is common code for process_pm_shutdown_request() and
 * process_pm_child_exit(), which process the signals that might mean we need
 * to change state.
 */
static void PostmasterStateMachine(void) {
  /* If we're doing a smart shutdown, try to advance that state. */
  if (pmState == PM_RUN) {
    if (!connsAllowed) {
      /*
       * This state ends when we have no normal client backends running.
       * Then we're ready to stop other children.
       */
      if (CountChildren(btmask(B_BACKEND)) == 0)
        UpdatePMState(PM_STOP_BACKENDS);
    }
  }

  /*
   * In the PM_WAIT_BACKENDS state, wait for all the regular backends and
   * processes like autovacuum and background workers that are comparable to
   * backends to exit.
   *
   * PM_STOP_BACKENDS is a transient state that means the same as
   * PM_WAIT_BACKENDS, but we signal the processes first, before waiting for
   * them.  Treating it as a distinct pmState allows us to share this code
   * across multiple shutdown code paths.
   */
  if (pmState == PM_STOP_BACKENDS || pmState == PM_WAIT_BACKENDS) {
    BackendTypeMask targetMask = BTYPE_MASK_NONE;

    /*
     * PM_WAIT_BACKENDS state ends when we have no regular backends.
     */
    targetMask = btmask_add(targetMask, B_BACKEND);

    /* Nor walsenders. */
    targetMask = btmask_add(targetMask, B_WAL_SENDER);

    /*
     * We let dead-end children keep running for now.
     *
     * This assertion checks that we have covered all backend types,
     * either by including them in targetMask, or by noting here that they
     * are allowed to continue running.
     */
#ifdef USE_ASSERT_CHECKING
    {
      BackendTypeMask remainMask = BTYPE_MASK_NONE;

      remainMask = btmask_add(remainMask, B_DEAD_END_BACKEND);

      /* these are not real postmaster children */
      remainMask = btmask_add(remainMask, B_INVALID, B_STANDALONE_BACKEND);

      /* All types should be included in targetMask or remainMask */
      Assert((remainMask.mask | targetMask.mask) == BTYPE_MASK_ALL.mask);
    }
#endif

    /* If we had not yet signaled the processes to exit, do so now */
    if (pmState == PM_STOP_BACKENDS) {
      SignalChildren(SIGTERM, targetMask);

      UpdatePMState(PM_WAIT_BACKENDS);
    }

    /* Are any of the target processes still running? */
    if (CountChildren(targetMask) == 0) {
      if (Shutdown >= ImmediateShutdown || FatalError) {
        /*
         * Stop any dead-end children and stop creating new ones.
         *
         * NB: Similar code exists in HandleFatalError(), when the
         * error happens in pmState > PM_WAIT_BACKENDS.
         */
        UpdatePMState(PM_WAIT_DEAD_END);
        ConfigurePostmasterWaitSet(false);
        SignalChildren(SIGQUIT, btmask(B_DEAD_END_BACKEND));

        /*
         * We already SIGQUIT'd auxiliary processes (other than
         * logger), if any, when we started immediate shutdown or
         * entered FatalError state.
         */
      } else {
        /*
         * If we get here, we are proceeding with normal shutdown. All
         * the regular children are gone.  There is no shutdown checkpoint
         * to write: sm keeps what is durable.  Stop any dead-end children.
         */
        Assert(Shutdown > NoShutdown);
        ereport(LOG, (errmsg("shutting down")));
        UpdatePMState(PM_WAIT_DEAD_END);
        ConfigurePostmasterWaitSet(false);
        SignalChildren(SIGTERM, btmask(B_DEAD_END_BACKEND));
      }
    }
  }

  if (pmState == PM_WAIT_DEAD_END) {
    /*
     * PM_WAIT_DEAD_END state ends when all other children are gone except
     * for the logger.  During normal shutdown, all that remains are
     * dead-end backends, but in FatalError processing we jump straight
     * here with more processes remaining.  Note that they have already
     * been sent appropriate shutdown signals, either during a normal
     * state transition leading up to PM_WAIT_DEAD_END, or during
     * FatalError processing.
     *
     * The reason we wait is to protect against a new postmaster starting
     * conflicting subprocesses; this isn't an ironclad protection, but it
     * at least helps in the shutdown-and-immediately-restart scenario.
     */
    if (CountChildren(BTYPE_MASK_ALL) == 0) {
      UpdatePMState(PM_NO_CHILDREN);
    }
  }

  /*
   * If we've been told to shut down, we exit as soon as there are no
   * remaining children.  If there was a crash, cleanup will occur at the
   * next startup.  (Before PostgreSQL 8.3, we tried to recover from the
   * crash before exiting, but that seems unwise if we are quitting because
   * we got SIGTERM from init --- there may well not be time for recovery
   * before init decides to SIGKILL us.)
   */
  if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN) {
    if (FatalError) {
      ereport(LOG, (errmsg("abnormal database system shutdown")));
      ExitPostmaster(1);
    } else {
      /*
       * Normal exit from the postmaster is here.  We don't need to log
       * anything here, since the UnlinkLockFiles proc_exit callback
       * will do so, and that should be the last user-visible action.
       */
      ExitPostmaster(0);
    }
  }

  /*
   * After a crash the server stops: there is no restart.  A thread that
   * ended abnormally may have left the state all threads share half
   * changed, and that state cannot be made anew under the running process.
   */
  if (FatalError && pmState == PM_NO_CHILDREN) {
    ereport(LOG, (errmsg("abnormal database system shutdown")));
    ExitPostmaster(1);
  }
}

static const char *pmstate_name(PMState state) {
#define PM_TOSTR_CASE(sym) case sym: return #sym
  switch (state) {
    PM_TOSTR_CASE(PM_INIT);
    PM_TOSTR_CASE(PM_RUN);
    PM_TOSTR_CASE(PM_STOP_BACKENDS);
    PM_TOSTR_CASE(PM_WAIT_BACKENDS);
    PM_TOSTR_CASE(PM_WAIT_DEAD_END);
    PM_TOSTR_CASE(PM_NO_CHILDREN);
  }
#undef PM_TOSTR_CASE

  pg_unreachable();
  return ""; /* silence compiler */
}

/*
 * Simple wrapper for updating pmState. The main reason to have this wrapper
 * is that it makes it easy to log all state transitions.
 */
static void UpdatePMState(PMState newState) {
  elog(DEBUG1, "updating PMState from %s to %s", pmstate_name(pmState),
       pmstate_name(newState));
  pmState = newState;
}

/*
 * Return string representation of signal.
 *
 * Because this is only implemented for signals we already rely on in this
 * file we don't need to deal with unimplemented or same-numeric-value signals
 * (as we'd e.g. have to for EWOULDBLOCK / EAGAIN).
 */
static const char *pm_signame(int signal) {
#define PM_TOSTR_CASE(sym) case sym: return #sym
  switch (signal) {
    PM_TOSTR_CASE(SIGABRT);
    PM_TOSTR_CASE(SIGCHLD);
    PM_TOSTR_CASE(SIGHUP);
    PM_TOSTR_CASE(SIGINT);
    PM_TOSTR_CASE(SIGKILL);
    PM_TOSTR_CASE(SIGQUIT);
    PM_TOSTR_CASE(SIGTERM);
    PM_TOSTR_CASE(SIGUSR1);
    PM_TOSTR_CASE(SIGUSR2);
  default:
    /* all signals sent by postmaster should be listed here */
    Assert(false);
    return "(unknown)";
  }
#undef PM_TOSTR_CASE

  return ""; /* silence compiler */
}

/*
 * Send a signal to a postmaster child process
 *
 * On systems that have setsid(), each child process sets itself up as a
 * process group leader.  For signals that are generally interpreted in the
 * appropriate fashion, we signal the entire process group not just the
 * direct child process.  This allows us to, for example, SIGQUIT a blocked
 * archive_recovery script, or SIGINT a script being run by a backend via
 * system().
 *
 * There is a race condition for recently-forked children: they might not
 * have executed setsid() yet.  So we signal the child directly as well as
 * the group.  We assume such a child will handle the signal before trying
 * to spawn any grandchild processes.  We also assume that signaling the
 * child twice will not cause any problems.
 */
static void signal_child(PMChild *pmchild, int signal) {
  pid_t pid = pmchild->pid;

  ereport(DEBUG3,
          (errmsg_internal("sending signal %d/%s to %s process with pid %d",
                           signal, pm_signame(signal),
                           GetBackendTypeDesc(pmchild->bkend_type),
                           (int)pmchild->pid)));

  /* A backend that is a thread is sent a flag (backend_thread.c). */
  if (pmchild->thread != NULL) {
    backend_thread_signal(pmchild->thread, signal);
    return;
  }

  /*
   * A backend is sent a signal as interrupt bits, through its PGPROC
   * (InterruptProc()); one with no PGPROC yet, and a crash or kill, get a
   * real one.
   */
  if (pmchild->bkend_type == B_BACKEND && signal != SIGQUIT &&
      signal != SIGKILL) {
    PGPROC *proc = PostmasterPidGetProc(pid);

    if (proc != NULL) {
      InterruptProc(proc, InterruptsOfSignal(signal));
      return;
    }
  }

  if (kill(pid, signal) < 0)
    elog(DEBUG3, "kill(%ld,%d) failed: %m", (long)pid, signal);
#ifdef HAVE_SETSID
  switch (signal) {
  case SIGINT:
  case SIGTERM:
  case SIGQUIT:
  case SIGKILL:
  case SIGABRT:
    if (kill(-pid, signal) < 0)
      elog(DEBUG3, "kill(%ld,%d) failed: %m", (long)(-pid), signal);
    break;
  default:
    break;
  }
#endif
}

/*
 * Send a signal to the targeted children.
 */
static bool SignalChildren(int signal, BackendTypeMask targetMask) {
  dlist_iter iter;
  bool signaled = false;

  dlist_foreach(iter, &ActiveChildList) {
    PMChild *bp = dlist_container(PMChild, elem, iter.cur);

    if (!btmask_contains(targetMask, bp->bkend_type))
      continue;

    signal_child(bp, signal);
    signaled = true;
  }
  return signaled;
}

/*
 * Send a termination signal to all of our children.
 */
static void TerminateChildren(int signal) {
  SignalChildren(signal, BTYPE_MASK_ALL);
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 */
static int BackendStartup(ClientSocket *client_sock) {
  PMChild *bn = NULL;
  pid_t pid;
  BackendStartupData startup_data;
  CAC_state cac;

  /*
   * Capture time that Postmaster got a socket from accept (for logging
   * connection establishment and setup total duration).
   */
  startup_data.socket_created = GetCurrentTimestamp();

  /*
   * Allocate and assign the child slot.  Note we must do this before
   * forking, so that we can handle failures (out of memory or child-process
   * slots) cleanly.
   */
  cac = canAcceptConnections(B_BACKEND);
  if (cac == CAC_OK) {
    /* Can change later to B_WAL_SENDER */
    bn = AssignPostmasterChildSlot(B_BACKEND);
    if (!bn) {
      /*
       * Too many regular child processes; launch a dead-end child
       * process instead.
       */
      cac = CAC_TOOMANY;
    }
  }
  if (!bn) {
    bn = AllocDeadEndChild();
    if (!bn) {
      ereport(LOG, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
      return STATUS_ERROR;
    }
  }

  /* Pass down canAcceptConnections state */
  startup_data.canAcceptConnections = cac;

  /* A connection's backend is a thread of this process. */
  pid = backend_thread_launch(bn->bkend_type, bn->child_slot, &startup_data,
                              sizeof(startup_data), client_sock, &bn->thread);
  if (pid < 0) {
    /* in parent, fork failed */
    int save_errno = errno;

    (void)ReleasePostmasterChildSlot(bn);
    errno = save_errno;
    ereport(LOG, (errmsg("could not fork new process for connection: %m")));
    report_fork_failure_to_client(client_sock, save_errno);
    return STATUS_ERROR;
  }

  /* in parent, successful fork */
  ereport(DEBUG2, (errmsg_internal("forked new %s, pid=%d socket=%d",
                                   GetBackendTypeDesc(bn->bkend_type), (int)pid,
                                   (int)client_sock->sock)));

  /*
   * Everything's been successful, it's safe to add this backend to our list
   * of backends.  The thread has the client socket now: ServerLoop must not
   * close it.
   */
  bn->pid = pid;
  client_sock->sock = PGINVALID_SOCKET;
  return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.  Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 */
static void report_fork_failure_to_client(ClientSocket *client_sock,
                                          int errnum) {
  char buffer[1000];
  int rc;

  /* Format the error message packet (always V2 protocol) */
  snprintf(buffer, sizeof(buffer), "E%s%s\n",
           _("could not fork new process for connection: "), strerror(errnum));

  /* Set port to non-blocking.  Don't do send() if this fails */
  if (!pg_set_noblock(client_sock->sock))
    return;

  /* We'll retry after EINTR, but ignore all other failures */
  do {
    rc = send(client_sock->sock, buffer, strlen(buffer) + 1, 0);
  } while (rc < 0 && errno == EINTR);
}

/*
 * ExitPostmaster -- cleanup
 *
 * Do NOT call exit() directly --- always go through here!
 */
static void ExitPostmaster(int status) {
#ifdef HAVE_PTHREAD_IS_THREADED_NP

  /*
   * There is no known cause for a postmaster to become multithreaded after
   * startup.  However, we might reach here via an error exit before
   * reaching the test in PostmasterMain, so provide the same hint as there.
   * This message uses LOG level, because an unclean shutdown at this point
   * would usually not look much different from a clean shutdown.
   */
  if (pthread_is_threaded_np() != 0)
    ereport(
        LOG,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("postmaster became multithreaded"),
         errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

  /* should cleanup shared memory and kill all backends */

  /*
   * Not sure of the semantics here.  When the Postmaster dies, should the
   * backends all be killed? probably not.
   *
   * MUST		-- vadim 05-10-1999
   */

  proc_exit(status);
}

/*
 * Count up number of child processes of specified types.
 */
static int CountChildren(BackendTypeMask targetMask) {
  dlist_iter iter;
  int cnt = 0;

  dlist_foreach(iter, &ActiveChildList) {
    PMChild *bp = dlist_container(PMChild, elem, iter.cur);

    if (!btmask_contains(targetMask, bp->bkend_type))
      continue;

    ereport(DEBUG4, (errmsg_internal("%s process %d is still running",
                                     GetBackendTypeDesc(bp->bkend_type),
                                     (int)bp->pid)));

    cnt++;
  }
  return cnt;
}

/*
 * Create the opts file
 */
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname) {
  FILE *fp;
  int i;

#define OPTS_FILE	"postmaster.opts"

  if ((fp = fopen(OPTS_FILE, "w")) == NULL) {
    ereport(LOG, (errcode_for_file_access(),
                  errmsg("could not create file \"%s\": %m", OPTS_FILE)));
    return false;
  }

  fprintf(fp, "%s", fullprogname);
  for (i = 1; i < argc; i++)
    fprintf(fp, " \"%s\"", argv[i]);
  fputs("\n", fp);

  if (fclose(fp)) {
    ereport(LOG, (errcode_for_file_access(),
                  errmsg("could not write file \"%s\": %m", OPTS_FILE)));
    return false;
  }

  return true;
}

/*
 * Initialize one and only handle for monitoring postmaster death.
 *
 * Called once in the postmaster, so that child processes can subsequently
 * monitor if their parent is dead.
 */
static void InitPostmasterDeathWatchHandle(void) {
#ifndef WIN32

  /*
   * Create a pipe. Postmaster holds the write end of the pipe open
   * (POSTMASTER_FD_OWN), and children hold the read end. Children can pass
   * the read file descriptor to select() to wake up in case postmaster
   * dies, or check for postmaster death with a (read() == 0). Children must
   * close the write end as soon as possible after forking, because EOF
   * won't be signaled in the read end until all processes have closed the
   * write fd. That is taken care of in ClosePostmasterPorts().
   */
  Assert(MyProcPid == PostmasterPid);
  if (pipe(postmaster_alive_fds) < 0)
    ereport(FATAL,
            (errcode_for_file_access(),
             errmsg_internal(
                 "could not create pipe to monitor postmaster death: %m")));

  /* Notify fd.c that we've eaten two FDs for the pipe. */
  ReserveExternalFD();
  ReserveExternalFD();

  /*
   * Set O_NONBLOCK to allow testing for the fd's presence with a read()
   * call.
   */
  if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) ==
      -1)
    ereport(FATAL, (errcode_for_socket_access(),
                    errmsg_internal("could not set postmaster death monitoring "
                                    "pipe to nonblocking mode: %m")));
#else

  /*
   * On Windows, we use a process handle for the same purpose.
   */
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                      GetCurrentProcess(), &PostmasterHandle, 0, TRUE,
                      DUPLICATE_SAME_ACCESS) == 0)
    ereport(FATAL, (errmsg_internal(
                       "could not duplicate postmaster handle: error code %lu",
                       GetLastError())));
#endif /* WIN32 */
}
