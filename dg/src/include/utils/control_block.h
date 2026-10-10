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

/* The process control block: one for the process. */
typedef struct pcb_t {
  std::mutex stderr_mutex; /* elog.c: one message at a time to stderr */
} pcb_t;

/* The thread control block: one for each thread that runs digres code. */
typedef struct tcb_t {
  std::unique_ptr<timeout_state_t, timeout_state_free_t> timeout;
} tcb_t;

extern pcb_t *pcb;
extern thread_local /*LOCAL*/ tcb_t *tcb;

extern void pcb_init(void);
extern void tcb_init(void);
extern void tcb_free(void);

#endif /* CONTROL_BLOCK_H */
