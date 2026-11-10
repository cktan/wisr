/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * control_block.h
 *	  The process control block, the shared block and the thread control
 *	  block.
 *
 * digres is one process of threads, and each block holds state by who owns
 * it and who reads it (CODE.md section 3):
 *
 * - pcb_t: what the main thread owns and other threads read, through a
 *   const pointer.  The main thread alone writes it; a field it writes
 *   after the first thread starts is an atomic, or guarded by a mutex.
 * - shmem_t: what the threads share and write.  Each field is an atomic, a
 *   mutex, whose comment names what it guards, or an object that guards
 *   itself.
 * - tcb_t: what one thread has, alone, its own: one for each thread, made
 *   as it starts and freed as it ends.  What a thread inherits from the one
 *   that started it is copied in at the start, and is its own after.  What
 *   the main thread owns and no other thread reads is in the main thread's
 *   postmaster_tcb_t.  It may point into shmem_t; nothing of it is reached
 *   from another thread.
 * - postmaster_tcb_t: the main thread's alone, the postmaster's: what the
 *   main thread owns and no other thread reads.  It is NULL in every other
 *   thread.
 *
 * They replace the SHARED and LOCAL thread_local variables (c.h), a module
 * at a time.  pcb and shmem are made once by main(), before any thread
 * starts, and never freed.
 *
 * A field is a value, or a std::unique_ptr to what the block owns: freeing
 * the block frees it.  A module keeps its state's layout to itself: the
 * block holds a pointer to a struct only declared here, and the module
 * defines the struct, the function that makes it and the deleter.
 *
 * src/include/utils/control_block.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONTROL_BLOCK_H
#define CONTROL_BLOCK_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "common/file_perm.h"
#include "datatype/timestamp.h"
#include "pgtime.h"

/* datadir.c: sm's home directory, the process's. */
struct datadir_state_t;
extern datadir_state_t *datadir_state_new(void);

/* ps_status.c: where the original argv is, the process's. */
struct ps_status_state_t;
extern ps_status_state_t *ps_status_state_new(void);

/* auth-scram.c: the mock authentication nonce, the process's. */
struct auth_scram_state_t;
extern auth_scram_state_t *auth_scram_state_new(void);

/* session_registry.c: the sessions registered, and a session's record. */
struct session_registry_t;
struct session_registry_free_t {
  void operator()(session_registry_t *registry) const;
};
struct session_record_t;

/* sequence.c: this session's last value of each sequence, for currval. */
typedef struct sequence_state_t sequence_state_t;
struct sequence_state_free_t {
  void operator()(sequence_state_t *state) const;
};
extern sequence_state_t *sequence_state_new(void);

/* be-secure-openssl.c: the TLS context connections are made from. */
struct tls_state_t;
struct tls_state_free_t {
  void operator()(tls_state_t *state) const;
};
extern tls_state_t *tls_state_new(void);
/* logfile.c: the log file written, the process's. */
struct logfile_state_t;
struct logfile_free_t {
  void operator()(logfile_state_t *state) const;
};
extern logfile_state_t *logfile_state_new(void);

/* pg_global.c: aa's state of pg_global, as last committed, that every
 * session reads, and a session's changes not yet committed (aa.h). */
struct aa_env_t;
struct pg_global_free_t {
  void operator()(aa_env_t *global) const;
};
struct aa_session_t;
struct pg_global_session_free_t {
  void operator()(aa_session_t *state) const;
};

/* advisory_lock.c: the advisory locks held, and a session's keys of them. */
struct advisory_table_t;
struct advisory_table_free_t {
  void operator()(advisory_table_t *table) const;
};
struct advisory_state_t;
struct advisory_state_free_t {
  void operator()(advisory_state_t *state) const;
};

/* smstore.c: the tables this session's transaction opened in sm. */
typedef struct smstore_state_t smstore_state_t;
struct smstore_state_free_t {
  void operator()(smstore_state_t *state) const;
};
extern smstore_state_t *smstore_state_new(void);

/* timeout.c: this thread's timeouts and its timer. */
typedef struct timeout_state_t timeout_state_t;
struct timeout_state_free_t {
  void operator()(timeout_state_t *state) const;
};
extern timeout_state_t *timeout_state_new(void);

/* fd.c: this thread's cache of virtual fds (sm0's sm_vfd.h). */
struct sm_vfd_cache_t;
struct sm_vfd_cache_free_t {
  void operator()(sm_vfd_cache_t *cache) const;
};

/* hooks.h: the hooks a loaded library sets, this thread's. */
struct hooks_t;
struct hooks_free_t {
  void operator()(hooks_t *hooks) const;
};

/* dfmgr.c: the libraries this session has loaded. */
typedef struct dfmgr_state_t dfmgr_state_t;
struct dfmgr_state_free_t {
  void operator()(dfmgr_state_t *state) const;
};
extern dfmgr_state_t *dfmgr_state_new(void);
extern dfmgr_state_t *dfmgr_state_copy(const dfmgr_state_t *state);

/* explain_state.c: the EXPLAIN extensions and options registered. */
typedef struct explain_registry_t explain_registry_t;
struct explain_registry_free_t {
  void operator()(explain_registry_t *registry) const;
};
extern explain_registry_t *explain_registry_new(void);
extern explain_registry_t *
explain_registry_copy(const explain_registry_t *registry);

/* postmaster.c: the postmaster's own state, the main thread's alone. */
struct postmaster_state_t;
struct postmaster_state_free_t {
  void operator()(postmaster_state_t *state) const;
};

/* pmchild.c: the postmaster's child slots, the main thread's alone. */
struct pmchild_state_t;
struct pmchild_state_free_t {
  void operator()(pmchild_state_t *state) const;
};

/*
 * The process control block: one for the process, what the main thread
 * owns and the other threads read.  The main thread alone writes it,
 * through pcb_main(): each field before the first thread starts, and after
 * only an atomic one.
 */
typedef struct pcb_t {
  /* -- the program, and where it runs -- */
  const char *progname = NULL; /* main.c: the program's name, from argv[0] */
  /*
   * globals.c: the absolute path to the top level of the data directory.
   * Except during early startup, this is also the server's working
   * directory; most code can use relative paths and not reference it.
   */
  char *DataDir = NULL;
  char my_exec_path[MAXPGPATH] = "";   /* full path to my executable */
  char pkglib_path[MAXPGPATH] = "";    /* full path to lib directory */
  char OutputFileName[MAXPGPATH] = ""; /* debugging output file */
  pid_t PostmasterPid = 0;
  TimestampTz PgStartTime = 0; /* timestamp.c: when the server started */
  pg_tz *log_timezone = nullptr; /* pgtz.c: timezone for log timestamps */

  /*
   * -- settings that change only with a restart (PGC_POSTMASTER) --
   * guc_tables.c: copied from the GUC variables by
   * publish_postmaster_settings() once the main thread has read its
   * settings, before any session thread starts; ConfigFileName by
   * SelectConfigFiles().  Read these, never the GUC variables, which each
   * thread has a copy of.  A string is malloc'd, never freed, or NULL.
   */
  bool enable_bonjour = false;
  bool Trace_connection_negotiation = false;
  bool Logging_collector = false;
  int MaxConnections = 0;
  int SuperuserReservedConnections = 0;
  int ReservedConnections = 0;
  int NBuffers = 0;
  int PostPortNumber = 0;
  int Unix_socket_permissions = 0;
  int max_files_per_process = 0;
  /* elog.c: the bitmask of LOG_DESTINATION_*, parsed from the string below */
  int Log_destination = 1; /* LOG_DESTINATION_STDERR */
  char *bonjour_name = nullptr;
  char *shared_preload_libraries_string = nullptr;
  char *event_source = nullptr;
  char *Unix_socket_group = nullptr;
  char *Unix_socket_directories = nullptr;
  char *ListenAddresses = nullptr;
  char *external_pid_file = nullptr;
  char *cluster_name = nullptr;
  char *Log_destination_string = nullptr;
  char *ConfigFileName = nullptr;

  /*
   * -- preset settings (PGC_INTERNAL) --
   * They never change: the build fixes them. guc_tables.c copies them from
   * the GUC variables with the settings above; the file in each comment is
   * where the variable was defined.
   */
  bool assert_enabled = false;    /* guc_tables.c */
  bool integer_datetimes = false; /* guc_tables.c */
  int max_function_args = 0;      /* guc_tables.c */
  int max_index_keys = 0;         /* guc_tables.c */
  int max_identifier_length = 0;  /* guc_tables.c */
  int block_size = 0;             /* guc_tables.c */
  char *ssl_library = nullptr;    /* be-secure.c */

  /*
   * -- the file logger's settings (PGC_SIGHUP) --
   * They change on a reload, after threads have started, so they are
   * guarded: the postmaster writes them with shmem->log.mutex held
   * (publish_log_file_settings(), guc_tables.c), and logfile.c reads them
   * with it held. The file in each comment is where the variable was defined.
   */
  std::string Log_directory;             /* logfile.c */
  std::string Log_filename;              /* logfile.c, a strftime() pattern */
  int Log_file_mode = 0600;              /* logfile.c */
  int Log_RotationAge = 0;               /* logfile.c, minutes */
  int Log_RotationSize = 0;              /* logfile.c, kB */
  bool Log_truncate_on_rotation = false; /* logfile.c */

  /*
   * -- the postmaster's own reloadable settings (PGC_SIGHUP) --
   * The main thread alone reads these, so they need no guard even though a
   * reload writes them: publish_postmaster_reload_settings() (guc_tables.c)
   * copies them from the GUC variables at start and on each reload. The file
   * in each comment is where the variable was defined.
   */
  bool EnableSSL = false;            /* postmaster.c, GUC "ssl" */
  bool send_abort_for_crash = false; /* postmaster.c */
  bool send_abort_for_kill = false;  /* postmaster.c */
  std::string ssl_cert_file;         /* be-secure.c */
  std::string ssl_key_file;          /* be-secure.c */
  std::string oauth_validator_libraries_string;        /* auth-oauth.c */
  bool SSLPreferServerCiphers = false;                 /* be-secure.c */
  std::string SSLCipherSuites;                         /* be-secure.c */
  std::string SSLCipherList;                           /* be-secure.c */
  bool ssl_passphrase_command_supports_reload = false; /* be-secure.c */
  std::string ssl_passphrase_command;                  /* be-secure.c */
  std::string SSLECDHCurve;          /* be-secure.c */
  std::string ssl_dh_params_file;    /* be-secure.c */
  std::string ssl_crl_dir;           /* be-secure.c */
  std::string ssl_crl_file;          /* be-secure.c */
  std::string ssl_ca_file;           /* be-secure.c */

  /* -- how the process runs -- */
  /*
   * globals.c: true in the postmaster and its sessions; false in a
   * standalone process (bootstrap or standalone backend).
   */
  bool IsPostmasterEnvironment = false;
  bool IsBinaryUpgrade = false;

  /* -- limits -- */
  /* globals.c: the session registry's limit, from MaxConnections */
  int MaxBackends = 0;
  /*
   * fd.c: how many fds a session thread may hold, its share of the
   * process's ceiling, from set_max_safe_fds(); conservative before that
   */
  int max_safe_fds_per_thread = 48; /* fd.c's FD_MINFREE, which it checks */

  /* -- file permissions -- */
  /* file_perm.c: modes for creating directories and files in the data
   * directory, and the mask to pass to umask() */
  int pg_dir_create_mode = PG_DIR_MODE_OWNER;
  int pg_file_create_mode = PG_FILE_MODE_OWNER;
  int pg_mode_mask = PG_MODE_MASK_OWNER;

  /* -- configuration and libraries -- */
  /*
   * guc.c: when the main thread last applied the configuration file; a
   * session's reload of it leaves this be
   */
  std::atomic<TimestampTz> PgReloadTime{0};
  /* miscinit.c: shared_preload_libraries being loaded, and loaded */
  bool process_shared_preload_libraries_in_progress = false;
  bool process_shared_preload_libraries_done = false;

  /* -- module state -- */
  /*
   * A module's own, made by pcb_init(), never freed: each module reads its
   * state through a const reference, as const does not reach through these
   */
  datadir_state_t *datadir = nullptr;       /* datadir.c */
  ps_status_state_t *ps_status = nullptr;   /* ps_status.c */
  auth_scram_state_t *auth_scram = nullptr; /* auth-scram.c */
} pcb_t;

/*
 * elog.c: the syslog settings (all PGC_SIGHUP), as the main thread last
 * published them. Never changed once made: a reload makes a new one.
 */
typedef struct syslog_conf_t {
  std::string ident;     /* syslog_ident */
  int facility;          /* syslog_facility */
  bool sequence_numbers; /* syslog_sequence_numbers */
  bool split_messages;   /* syslog_split_messages */
} syslog_conf_t;

/*
 * The shared block: what the threads share and write, one for the process.
 * What the process keeps for its whole life may be malloc'd, its pointer
 * here, and never freed: the block itself never is.
 */
typedef struct shmem_t {
  struct {
    /* logfile.c: one message at a time to the log, and its state, below */
    std::mutex mutex;
    /* logfile.c: the log file now written; guarded by mutex */
    std::unique_ptr<logfile_state_t, logfile_free_t> file;
  } log;

  /*
   * be-secure-openssl.c: the TLS context every connection is made from,
   * which the main thread makes and replaces on a reload; guarded by
   * tls_mutex
   */
  std::mutex tls_mutex;
  std::unique_ptr<tls_state_t, tls_state_free_t> tls;

  /* injection_point.c: one attach or detach at a time */
  std::mutex injection_point_mutex;
  /* injection_point.c: its control struct, malloc'd, with the build option */
  struct InjectionPointsCtl *injection_points = nullptr;

  /*
   * xact.c: the next local transaction id, a virtual xid's low part, given
   * every session's transactions from one counter, so no two are the same
   */
  std::atomic<uint32_t> next_lxid{1};

  /* guc.c: postgresql.auto.conf, rewritten by one ALTER SYSTEM at a time */
  std::mutex auto_file_mutex;

  /* session_registry.c, procsignal.c: guards each record's cancel key */
  std::mutex cancel_key_mutex;

  /* pmsignal.c: why the postmaster sent SIGQUIT, a QuitSignalReason */
  std::atomic<int> quit_reason{0};

  /* session_registry.c: the sessions registered, by pid */
  std::unique_ptr<session_registry_t, session_registry_free_t> session_registry;

  /* advisory_lock.c: the advisory locks held, and their waiters */
  std::unique_ptr<advisory_table_t, advisory_table_free_t> advisory;

  /*
   * pg_global.c: the roles, made by the postmaster as it loads the data
   * directory, before the first session; guarded by its own mutex
   */
  std::unique_ptr<aa_env_t, pg_global_free_t> pg_global;

  /*
   * -- values the postmaster may change while other threads read --
   * Guard each member with a mutex, or make it atomic.
   */
  /*
   * guc_tables.c: PGC_SIGHUP settings that a session reads, one atomic each.
   * The main thread stores them at start and on each config load
   * (publish_shared_settings()); a session reads them where it needs them.
   */
  /* backend_startup.c */
  std::atomic<bool> log_hostname{false};
  /* backend_startup.c, postinit.c */
  std::atomic<int> authentication_timeout{60};
  /* guc.c: ALTER SYSTEM */
  std::atomic<bool> allow_alter_system{true};
  /* backend_startup.c: seconds a new session sleeps before authentication */
  std::atomic<int> pre_auth_delay{0};
  /* be-secure-openssl.c: the protocol versions, in a hint about a failed
   * handshake */
  std::atomic<int> ssl_min_protocol_version{0};
  std::atomic<int> ssl_max_protocol_version{0};
  struct {
    /*
     * guc_tables.c: log_line_prefix (PGC_SIGHUP), which elog.c reads in
     * every thread. The mutex is a leaf: hold it only to copy or swap the
     * pointer, never across a call that may log, which would take it again.
     */
    std::mutex mutex;
    /* The string is never changed: a reload swaps in a new one. */
    std::shared_ptr<const std::string> value;
  } log_line_prefix;
  struct {
    /*
     * guc_tables.c: the syslog settings, which elog.c reads in every thread
     * that writes to syslog. Also a leaf mutex.
     */
    std::mutex mutex;
    std::shared_ptr<const syslog_conf_t> conf;
    /*
     * elog.c: the settings the process's one syslog connection was opened
     * with. openlog() keeps a pointer to the ident, so this keeps the string
     * alive until the connection is opened again; guarded by mutex.
     */
    std::shared_ptr<const syslog_conf_t> opened;
  } syslog;
} shmem_t;

/*
 * guc_tables.c: the thread's own GUC settings (group 3 of GUCPLAN.md): the
 * USERSET, SUSET, BACKEND and SU_BACKEND ones that a session can SET. The
 * thread's GUC table points at these fields (build_guc_variables()), and the
 * code reads them as tcb->guc.name. The file in each comment is where the
 * variable was defined.
 */
typedef struct tcb_guc_t {
  bool Array_nulls = true;            /* arrayfuncs.c */
  bool Transform_null_equals = false; /* parse_expr.c */
  bool quote_all_identifiers = false; /* ruleutils.c */
  bool check_function_bodies = true;  /* pg_proc.c */
  bool Debug_pretty_print = true;     /* guc_tables.c */
  bool enable_seqscan = true;         /* costsize.c */
  bool enable_indexscan = true;       /* costsize.c */
  bool enable_indexonlyscan = true;   /* costsize.c */
  bool enable_bitmapscan = true;      /* costsize.c */
  bool enable_tidscan = true;         /* costsize.c */
  bool enable_sort = true;            /* costsize.c */
  bool enable_incremental_sort = true; /* costsize.c */
  bool enable_hashagg = true;          /* costsize.c */
  bool enable_material = true;         /* costsize.c */
  bool enable_memoize = true;          /* costsize.c */
  bool enable_nestloop = true;         /* costsize.c */
  bool enable_mergejoin = true;        /* costsize.c */
  bool enable_hashjoin = true;         /* costsize.c */
  bool enable_partitionwise_join = false;      /* costsize.c */
  bool enable_partitionwise_aggregate = false; /* costsize.c */
  bool enable_partition_pruning = true;        /* costsize.c */
  bool enable_presorted_aggregate = true;      /* costsize.c */
  bool enable_async_append = true;             /* costsize.c */
  bool enable_self_join_elimination;           /* analyzejoins.c */
  bool enable_group_by_reordering = true;      /* pathkeys.c */
  bool enable_distinct_reordering = true;      /* planner.c */
  bool Log_disconnections = false;             /* postgres.c */
  bool ExitOnAnyError = false;                 /* globals.c */
  bool log_duration = false;                   /* guc_tables.c */
  bool Debug_copy_parse_plan_trees;            /* guc_tables.c */
  bool Debug_print_parse = false;              /* guc_tables.c */
  bool Debug_print_rewritten = false;          /* guc_tables.c */
  bool Debug_print_plan = false;               /* guc_tables.c */
  bool log_parser_stats = false;               /* guc_tables.c */
  bool log_planner_stats = false;              /* guc_tables.c */
  bool log_executor_stats = false;             /* guc_tables.c */
  bool track_io_timing = false;                /* buffile.c */
  bool update_process_title = true;            /* ps_status.c */
  bool DefaultXactReadOnly = false;            /* xact.c */
  bool XactReadOnly;                           /* xact.c */
  bool DefaultXactDeferrable = false;          /* xact.c */
  bool XactDeferrable;                         /* xact.c */
  bool row_security;                           /* guc_tables.c */
  bool default_with_oids = false;              /* guc_tables.c */
  bool allowSystemTableMods = false;           /* globals.c */
  bool IgnoreSystemIndexes = false;            /* miscinit.c */
  int PostAuthDelay = 0;                       /* postgres.c */
  int from_collapse_limit;                     /* initsplan.c */
  int join_collapse_limit;                     /* initsplan.c */
  int geqo_threshold;                          /* allpaths.c */
  double Geqo_selection_bias;                  /* geqo_main.c */
  int work_mem = 4096;                         /* globals.c */
  int maintenance_work_mem = 65536;            /* globals.c */
  int max_stack_depth = 100;                   /* stack_depth.c */
  int temp_file_limit = -1;                    /* guc_tables.c */
  int StatementTimeout = 0;                    /* proc.c */
  int DeadlockTimeout = 50;                    /* proc.c */
  int LockTimeout = 0;                         /* proc.c */
  int IdleInTransactionSessionTimeout = 0;     /* proc.c */
  int TransactionTimeout = 0;                  /* proc.c */
  int IdleSessionTimeout = 0;                  /* proc.c */
  int extra_float_digits = 1;                  /* float.c */
  int log_min_duration_sample = -1;            /* guc_tables.c */
  int log_min_duration_statement = -1;         /* guc_tables.c */
  int log_parameter_max_length = -1;           /* guc_tables.c */
  int log_parameter_max_length_on_error = 0;   /* guc_tables.c */
  int tcp_keepalives_idle;                     /* guc_tables.c */
  int tcp_keepalives_interval;                 /* guc_tables.c */
  int tcp_keepalives_count;                    /* guc_tables.c */
  int ssl_renegotiation_limit;                 /* guc_tables.c */
  int effective_cache_size = 524288;           /* costsize.c */
  int log_temp_files = -1;                     /* guc_tables.c */
  int debug_discard_caches = 0;                /* inval.c */
  int client_connection_check_interval = 0;    /* postgres.c */
  int scram_sha_256_iterations = 4096;         /* auth-scram.c */
  double seq_page_cost = 1.0;                  /* costsize.c */
  double random_page_cost = 4.0;               /* costsize.c */
  double cpu_tuple_cost = 0.01;                /* costsize.c */
  double cpu_index_tuple_cost = 0.005;         /* costsize.c */
  double cpu_operator_cost = 0.0025;           /* costsize.c */
  double cursor_tuple_fraction = 0.1;          /* planner.c */
  double recursive_worktable_factor = 10.0;    /* costsize.c */
  double hash_mem_multiplier = 2.0;            /* globals.c */
  double VacuumCostDelay = 0;                  /* globals.c */
  double log_statement_sample_rate = 1.0;      /* guc_tables.c */
  double log_xact_sample_rate = 0;             /* guc_tables.c */
  double phony_random_seed;                    /* guc_tables.c */
  char *client_encoding_string;                /* guc_tables.c */
  char *datestyle_string;                      /* guc_tables.c */
  char *default_table_access_method;           /* tableam.c */
  char *createrole_self_grant = "";            /* user.c */
  char *Dynamic_library_path;                  /* dfmgr.c */
  char *Extension_control_path;                /* extension.c */
  char *locale_messages;                       /* pg_locale.c */
  char *locale_monetary;                       /* pg_locale.c */
  char *locale_numeric;                        /* pg_locale.c */
  char *locale_time;                           /* pg_locale.c */
  char *local_preload_libraries_string = NULL; /* miscinit.c */
  char *namespace_search_path = NULL;          /* namespace.c */
  char *role_string;                           /* guc_tables.c */
  char *session_authorization_string;          /* guc_tables.c */
  char *timezone_string;                       /* guc_tables.c */
  char *timezone_abbreviations_string;         /* guc_tables.c */
  char *backtrace_functions;                   /* guc_tables.c */
  char *log_connections_string = NULL;         /* backend_startup.c */
  int compute_query_id;                        /* queryjumblefuncs.c */
  /* -- new GUC fields above -- */
} tcb_guc_t;

/* The thread control block: one for each thread that runs digres code. */
typedef struct tcb_t {
  /* -- the thread's own GUC settings -- */
  tcb_guc_t guc;

  /* -- the session -- */
  /* session_registry.c: this session's record, while it is registered */
  std::shared_ptr<session_record_t> session;
  std::unique_ptr<sequence_state_t, sequence_state_free_t> sequence;
  /* advisory_lock.c: the keys this session holds, made at its first lock */
  std::unique_ptr<advisory_state_t, advisory_state_free_t> advisory;

  /* -- its transaction -- */
  /*
   * xact.c: this session's transaction's local id, a virtual xid's low
   * part, invalid between transactions; its PGPROC's once
   * (dg/WORKNOTE.md step 159)
   */
  uint32 lxid = 0;
  std::unique_ptr<smstore_state_t, smstore_state_free_t> smstore;
  /*
   * pg_global.c: this session's role DDL not yet committed, its caches'
   * epoch of the roles, and its sm session on pg_global
   */
  std::unique_ptr<aa_session_t, pg_global_session_free_t> pg_global;

  /* -- the thread's resources -- */
  std::unique_ptr<timeout_state_t, timeout_state_free_t> timeout;
  /* made at its first use, by fd.c */
  std::unique_ptr<sm_vfd_cache_t, sm_vfd_cache_free_t> vfd;

  /* -- loaded libraries -- */
  /*
   * utils/hooks.h: the hooks loaded libraries set, all NULL at first; a
   * session starts with its parent's (session_thread_start()).  A
   * library's own statics are not the session's (ISSUE.md dg issue 1).
   */
  std::unique_ptr<hooks_t, hooks_free_t> hooks;
  /*
   * dfmgr.c, explain_state.c: the libraries loaded and the EXPLAIN
   * extensions registered; a session starts with its parent's, and loads
   * none of its own (ISSUE.md dg issue 1)
   */
  std::unique_ptr<dfmgr_state_t, dfmgr_state_free_t> dfmgr;
  std::unique_ptr<explain_registry_t, explain_registry_free_t> explain;
} tcb_t;

/*
 * The postmaster's thread control block: what the main thread owns and no
 * other thread reads, the postmaster's own state, or a standalone
 * backend's.  main() makes it for the main thread, and it is never freed;
 * it is NULL in every other thread.
 */
typedef struct postmaster_tcb_t {
  /* -- the postmaster's state -- */
  /* postmaster.c: the postmaster's state, made as PostmasterMain() starts */
  std::unique_ptr<postmaster_state_t, postmaster_state_free_t> postmaster;
  /* pmchild.c: the child slots, made by InitPostmasterChildSlots() */
  std::unique_ptr<pmchild_state_t, pmchild_state_free_t> pmchild;

  /* -- what to remove at exit -- */
  /* miscinit.c: the lock files to remove at exit, in TopMemoryContext */
  struct List *lock_files = nullptr;
  /* pqcomm.c: the socket files to remove at exit, in TopMemoryContext */
  struct List *sock_paths = nullptr;
} postmaster_tcb_t;

extern const pcb_t *pcb;
extern shmem_t *shmem;
extern thread_local /*LOCAL*/ tcb_t *tcb;
extern thread_local /*LOCAL*/ postmaster_tcb_t *postmaster_tcb;

extern void pcb_init(void);
extern void tcb_init(void);
extern void tcb_free(void);
extern void postmaster_tcb_init(void);

/*
 * The process control block, to write: the main thread's alone, before the
 * first thread starts, or an atomic field after.  Every other use reads it,
 * through pcb.
 */
static inline pcb_t *pcb_main(void) {
  Assert(postmaster_tcb != NULL);
  return const_cast<pcb_t *>(pcb);
}

#endif /* CONTROL_BLOCK_H */
