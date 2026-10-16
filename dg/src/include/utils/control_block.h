/*-------------------------------------------------------------------------
 *
 * control_block.h
 *	  The process control block and the thread control block.
 *
 * digres is one process of threads.  What there is one of for the process
 * is in the pcb_t, made once by main() before any thread starts; what each
 * thread has its own of is in its tcb_t, made as the thread starts and
 * freed as it ends.  They replace the SHARED and LOCAL thread_local
 * variables (c.h), a module at a time.
 *
 * A field is a value, or a std::unique_ptr to what the block owns: freeing
 * the block frees it.  A module keeps its state's layout to itself: the
 * block holds a pointer to a struct only declared here, and the module
 * defines the struct, the function that makes it and the deleter.
 *
 * A pcb_t field is either set before the first thread starts and only
 * read after, or guards itself (a mutex, an atomic).
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

/* timeout.c: this thread's timeouts and its timer. */
typedef struct timeout_state_t timeout_state_t;
struct timeout_state_free_t {
  void operator()(timeout_state_t *state) const;
};
extern timeout_state_t *timeout_state_new(void);

/* sequence.c: this session's last value of each sequence, for currval. */
typedef struct sequence_state_t sequence_state_t;
struct sequence_state_free_t {
  void operator()(sequence_state_t *state) const;
};
extern sequence_state_t *sequence_state_new(void);

/* smstore.c: the tables this session's transaction opened in sm. */
typedef struct smstore_state_t smstore_state_t;
struct smstore_state_free_t {
  void operator()(smstore_state_t *state) const;
};
extern smstore_state_t *smstore_state_new(void);

/* fd.c: this thread's cache of virtual fds (sm0's sm_vfd.h). */
struct sm_vfd_cache_t;
struct sm_vfd_cache_free_t {
  void operator()(sm_vfd_cache_t *cache) const;
};

/* shmem.c: the index of the structures the threads share, by name. */
typedef struct shmem_index_t shmem_index_t;
struct shmem_index_free_t {
  void operator()(shmem_index_t *index) const;
};

/* session_registry.c: the sessions registered, and a session's record. */
struct session_registry_t;
struct session_registry_free_t {
  void operator()(session_registry_t *registry) const;
};
struct session_record_t;

/* advisory_lock.c: the advisory locks held, and a session's keys of them. */
struct advisory_table_t;
struct advisory_table_free_t {
  void operator()(advisory_table_t *table) const;
};
struct advisory_state_t;
struct advisory_state_free_t {
  void operator()(advisory_state_t *state) const;
};

/*
 * The process control block: one for the process.  What the process keeps
 * for its whole life may be malloc'd, its pointer here, and never freed:
 * the block itself never is.
 */
typedef struct pcb_t {
  std::mutex stderr_mutex; /* elog.c: one message at a time to stderr */
  std::unique_ptr<shmem_index_t, shmem_index_free_t> shmem_index;

  /* injection_point.c: one attach or detach at a time */
  std::mutex injection_point_mutex;

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
} pcb_t;

/* The thread control block: one for each thread that runs digres code. */
typedef struct tcb_t {
  std::unique_ptr<timeout_state_t, timeout_state_free_t> timeout;
  std::unique_ptr<sequence_state_t, sequence_state_free_t> sequence;
  std::unique_ptr<smstore_state_t, smstore_state_free_t> smstore;
  /* made at its first use, by fd.c */
  std::unique_ptr<sm_vfd_cache_t, sm_vfd_cache_free_t> vfd;
  /* session_registry.c: this session's record, while it is registered */
  std::shared_ptr<session_record_t> session;
  /* advisory_lock.c: the keys this session holds, made at its first lock */
  std::unique_ptr<advisory_state_t, advisory_state_free_t> advisory;
  /*
   * xact.c, procarray.c, snapmgr.c: this session's transaction's local id,
   * a virtual xid's low part, and its xmin, both invalid between
   * transactions; its PGPROC's once (dg/WORKNOTE.md step 159)
   */
  uint32 lxid = 0;
  uint32 xmin = 0;
} tcb_t;

extern pcb_t *pcb;
extern thread_local /*LOCAL*/ tcb_t *tcb;

extern void pcb_init(void);
extern void tcb_init(void);
extern void tcb_free(void);

#endif /* CONTROL_BLOCK_H */
