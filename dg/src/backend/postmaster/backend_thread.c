/*-------------------------------------------------------------------------
 *
 * backend_thread.c
 *	  A backend that is a thread of the postmaster's process.
 *
 * A connection's backend runs in a thread the postmaster starts, where it
 * forked a process.  The thread is given the postmaster's SHARED variables
 * and settings (session_thread_start()), sets itself up as a forked child
 * does (InitPostmasterChild()), and runs BackendMain().  Its thread id is
 * its pid.
 *
 * The postmaster reaches it as it reaches a child process.  A signal is an
 * interrupt bit (storage/procsignal.h), raised in the thread through the
 * handle in its launch record (sm_sys.h), from its first instruction on.
 * The postmaster learns of its end as of a child's: the thread queues
 * itself, with its exit code, as it ends, and sets the postmaster's latch;
 * process_pm_child_exit() takes it from the queue as it takes a child from
 * waitpid().
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/backend_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <atomic>
#include <mutex>
#include <signal.h>
#include <unistd.h>

#include "port/atomics.h"
#include "postmaster/backend_thread.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/waiteventset.h"
#include "tcop/backend_startup.h"
#include "utils/memutils.h"
#include "utils/session_thread.h"

/*
 * The postmaster's queue of backend threads that have ended, and the latch
 * that wakes it for them.  Made by the first launch, never freed.
 */
typedef struct exit_queue_t {
  std::mutex mu;
  backend_thread_t *head;
  Latch *latch;
} exit_queue_t;

/* A backend thread's launch record: the postmaster frees it. */
struct backend_thread_t {
  BackendType child_type;
  int child_slot;
  ClientSocket client_sock;
  void *startup_data; /* a malloc'd copy */
  size_t startup_data_len;
  exit_queue_t *queue;
  pg_atomic_uint32 tid;                 /* the thread's id, once it runs */
  std::atomic<sm_thread_ref_t *> self; /* the thread, to wake */
  int exit_code;                        /* set as it ends */
  backend_thread_t *next_exited;
};

static thread_local /*LOCAL*/ exit_queue_t *exit_queue = NULL;

/* The thread: set up as a forked child is, then be the backend. */
static void backend_thread_main(void *arg) {
  backend_thread_t *thread = (backend_thread_t *)arg;
  exit_queue_t *queue = thread->queue;

  /* Before the tid: the postmaster wakes the thread once it has that. */
  thread->self.store(new sm_thread_ref_t(sm_thread_self()),
                     std::memory_order_release);
  pg_atomic_write_u32(&thread->tid, MyProcPid);

  if (IsExternalConnectionBackend(thread->child_type)) {
    BackendStartupData *bsdata = (BackendStartupData *)thread->startup_data;

    conn_timing.socket_create = bsdata->socket_created;
    conn_timing.fork_start = bsdata->fork_started;
    conn_timing.fork_end = GetCurrentTimestamp();
  }

  InitPostmasterChild();
  MemoryContextSwitchTo(TopMemoryContext);
  MyPMChildSlot = thread->child_slot;
  MyClientSocket = (ClientSocket *)palloc(sizeof(ClientSocket));
  memcpy(MyClientSocket, &thread->client_sock, sizeof(ClientSocket));

  /*
   * The backend ends in proc_exit(), which throws to the thread's top: on
   * the way, queue the thread for the postmaster.  The record is the
   * postmaster's from then on.
   */
  try {
    BackendMain(thread->startup_data, thread->startup_data_len);
  } catch (const proc_exit_t &e) {
    thread->exit_code = e.code;
    {
      /* The postmaster takes from the queue as threads add to it. */
      std::lock_guard<std::mutex> guard(queue->mu);

      thread->next_exited = queue->head;
      queue->head = thread;
    }
    SetLatch(queue->latch);
    throw;
  }
}

/*
 * Start a backend of child_type as a thread, as the postmaster once forked
 * one, and return its pid, which is its thread id.  *thread is its
 * launch record, for backend_thread_signal() and the postmaster's
 * bookkeeping.
 */
pid_t backend_thread_launch(BackendType child_type, int child_slot,
                            void *startup_data, size_t startup_data_len,
                            ClientSocket *client_sock,
                            backend_thread_t **thread) {
  backend_thread_t *t;
  pid_t tid;

  if (exit_queue == NULL) {
    exit_queue = new exit_queue_t();
    exit_queue->head = NULL;
    exit_queue->latch = MyLatch;
  }

  if (IsExternalConnectionBackend(child_type))
    ((BackendStartupData *)startup_data)->fork_started = GetCurrentTimestamp();

  t = (backend_thread_t *)calloc(1, sizeof(backend_thread_t));
  if (t == NULL || (t->startup_data = malloc(startup_data_len)) == NULL)
    elog(ERROR, "out of memory");
  t->child_type = child_type;
  t->child_slot = child_slot;
  memcpy(&t->client_sock, client_sock, sizeof(ClientSocket));
  memcpy(t->startup_data, startup_data, startup_data_len);
  t->startup_data_len = startup_data_len;
  t->queue = exit_queue;
  pg_atomic_init_u32(&t->tid, 0);

  /* Nobody joins it: the postmaster learns of its end from the queue. */
  session_thread_start(backend_thread_main, t).detach();

  /* Its pid is its thread id: wait for the thread to say. */
  while ((tid = (pid_t)pg_atomic_read_u32(&t->tid)) == 0)
    sched_yield();

  *thread = t;
  return tid;
}

/*
 * Send sig to a backend thread, as the postmaster kill()s a child: as its
 * interrupt bits, raised in the thread itself, PGPROC or no.  A signal with
 * no bit, SIGKILL among them, cannot end one thread, and is dropped.
 */
void backend_thread_signal(backend_thread_t *thread, int sig) {
  uint32 bits = InterruptsOfSignal(sig);

  if (bits != 0)
    sm_interrupt(*thread->self.load(std::memory_order_acquire), bits);
}

/* Whether a backend thread has ended that backend_thread_exited() has not
 * given yet. */
bool backend_thread_ended(void) {
  bool ended;

  if (exit_queue == NULL)
    return false;
  {
    /* Threads add to the queue as the postmaster looks. */
    std::lock_guard<std::mutex> guard(exit_queue->mu);

    ended = exit_queue->head != NULL;
  }
  return ended;
}

/*
 * A backend thread that has ended, with its exit status as waitpid() gives
 * one, or NULL if none has since the last call.
 */
backend_thread_t *backend_thread_exited(int *exitstatus) {
  backend_thread_t *thread;

  if (exit_queue == NULL)
    return NULL;
  {
    /* Threads add to the queue as the postmaster takes from it. */
    std::lock_guard<std::mutex> guard(exit_queue->mu);

    thread = exit_queue->head;
    if (thread != NULL)
      exit_queue->head = thread->next_exited;
  }
  if (thread != NULL)
    *exitstatus = (thread->exit_code & 0xff) << 8;
  return thread;
}

/* Free a backend thread's launch record, once it has ended. */
void backend_thread_free(backend_thread_t *thread) {
  delete thread->self.load();
  free(thread->startup_data);
  free(thread);
}

/* A backend thread's pid: its thread id. */
pid_t backend_thread_pid(backend_thread_t *thread) {
  return (pid_t)pg_atomic_read_u32(&thread->tid);
}
