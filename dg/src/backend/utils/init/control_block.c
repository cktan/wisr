/*-------------------------------------------------------------------------
 *
 * control_block.c
 *	  The process control block, the shared block and the thread control
 *	  block.
 *
 * See utils/control_block.h.
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/control_block.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/control_block.h"

/* The process's control block: one for the process, never freed. */
const pcb_t *pcb = NULL;

/* What the threads share and write: one for the process, never freed. */
shmem_t *shmem = NULL;

/* This thread's control block, or NULL if it runs no digres code. */
thread_local /*LOCAL*/ tcb_t *tcb = NULL;

/* The postmaster's thread control block: the main thread's, else NULL. */
thread_local /*LOCAL*/ postmaster_tcb_t *postmaster_tcb = NULL;

/*
 * Make the process control block and the shared block.  main() calls it
 * first, before any thread starts and before anything logs.
 */
void pcb_init(void) {
  pcb_t *block = new pcb_t();

  Assert(pcb == NULL && shmem == NULL);
  block->datadir = datadir_state_new();
  block->ps_status = ps_status_state_new();
  block->auth_scram = auth_scram_state_new();
  pcb = block;
  shmem = new shmem_t();
}

/* Make this thread's control block, as the thread starts. */
void tcb_init(void) {
  Assert(tcb == NULL);
  tcb = new tcb_t();
  tcb->timeout.reset(timeout_state_new());
  tcb->sequence.reset(sequence_state_new());
  tcb->smstore.reset(smstore_state_new());
}

/*
 * Free this thread's control block, and all it owns, as the thread ends.
 * tcb is cleared first: a signal handler that runs meanwhile finds no
 * block rather than one half freed.
 */
void tcb_free(void) {
  tcb_t *block = tcb;

  tcb = NULL;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  delete block;
}

/*
 * Make the postmaster's thread control block, the main thread's, never
 * freed.  main() calls it after tcb_init(), before any thread starts.
 */
void postmaster_tcb_init(void) {
  Assert(postmaster_tcb == NULL);
  postmaster_tcb = new postmaster_tcb_t();
}
