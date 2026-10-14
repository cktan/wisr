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

/* shmem.c: the index of the structures the threads share, by name. */
typedef struct shmem_index_t shmem_index_t;
struct shmem_index_free_t {
  void operator()(shmem_index_t *index) const;
};

union LWLockPadded;     /* storage/lwlock.h */
struct PGSemaphoreData; /* port/posix_sema.c */

/*
 * The process control block: one for the process.  What the process keeps
 * for its whole life may be malloc'd, its pointer here, and never freed:
 * the block itself never is.
 */
typedef struct pcb_t {
  std::mutex stderr_mutex; /* elog.c: one message at a time to stderr */
  std::unique_ptr<shmem_index_t, shmem_index_free_t> shmem_index;

  /* lwlock.c: the main LWLock array (MainLWLockArray), malloc'd */
  union LWLockPadded *main_lwlock_array = nullptr;
  /* lwlock.c: the next tranche ID to give out, and its lock */
  std::mutex lwlock_counter_mutex;
  int lwlock_counter = 0;

  /* posix_sema.c: the semaphores, malloc'd */
  struct PGSemaphoreData *shared_semas = nullptr;
} pcb_t;

/* The thread control block: one for each thread that runs digres code. */
typedef struct tcb_t {
  std::unique_ptr<timeout_state_t, timeout_state_free_t> timeout;
  std::unique_ptr<sequence_state_t, sequence_state_free_t> sequence;
  std::unique_ptr<smstore_state_t, smstore_state_free_t> smstore;
} tcb_t;

extern pcb_t *pcb;
extern thread_local /*LOCAL*/ tcb_t *tcb;

extern void pcb_init(void);
extern void tcb_init(void);
extern void tcb_free(void);

#endif /* CONTROL_BLOCK_H */
