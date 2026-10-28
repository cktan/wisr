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

#include "common/file_perm.h"
#include "datatype/timestamp.h"

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
 * The shared block: what the threads share and write, one for the process.
 * What the process keeps for its whole life may be malloc'd, its pointer
 * here, and never freed: the block itself never is.
 */
typedef struct shmem_t {
  std::mutex stderr_mutex; /* elog.c: one message at a time to stderr */

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
} shmem_t;

/* The thread control block: one for each thread that runs digres code. */
typedef struct tcb_t {
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
   * library's own statics are not the session's (dg/ISSUE.md issue 1).
   */
  std::unique_ptr<hooks_t, hooks_free_t> hooks;
  /*
   * dfmgr.c, explain_state.c: the libraries loaded and the EXPLAIN
   * extensions registered; a session starts with its parent's, and loads
   * none of its own (dg/ISSUE.md issue 1)
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
