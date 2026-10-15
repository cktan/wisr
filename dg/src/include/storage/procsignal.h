/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

#include "storage/procnumber.h"

/*
 * Reasons for signaling a Postgres child process (a backend or an auxiliary
 * process, like checkpointer).  We can cope with concurrent signals for
 * different reasons.  However, if the same reason is signaled multiple times in
 * quick succession, the process is likely to observe only one notification of
 * it. This is okay for the present uses.
 *
 * Also, because of race conditions, it's important that all the signals be
 * defined so that no harm is done if a process mistakenly receives one.
 */
typedef enum {
  PROCSIG_CATCHUP_INTERRUPT,  /* sinval catchup interrupt */
  PROCSIG_LOG_MEMORY_CONTEXT, /* ask backend to log the memory contexts */
} ProcSignalReason;

#define NUM_PROCSIGNALS (PROCSIG_LOG_MEMORY_CONTEXT + 1)

/*
 * Length of query cancel keys generated.
 *
 * Note that the protocol allows for longer keys, or shorter, but this is the
 * length we actually generate.  Client code, and the server code that handles
 * incoming cancellation packets from clients, mustn't use this hardcoded
 * length.
 */
#define MAX_CANCEL_KEY_LENGTH  32

/*
 * prototypes for functions in procsignal.c
 */
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(const uint8 *cancel_key, int cancel_key_len);
extern int SendProcSignal(pid_t pid, ProcSignalReason reason,
                          ProcNumber procNumber);
extern void SendCancelRequest(int backendPID, const uint8 *cancel_key,
                              int cancel_key_len);

extern void procsignal_sigusr1_handler(void);

/*
 * A thread's interrupts: bits in its word of pending interrupts (sm_sys.h),
 * each with a handler of the thread's own, which runs at its next
 * CHECK_FOR_INTERRUPTS() or wait.  They were signals.
 */
#define INTERRUPT_CANCEL (1u << 0)  /* cancel the query: was SIGINT */
#define INTERRUPT_DIE (1u << 1)     /* end the session: was SIGTERM */
#define INTERRUPT_QUIT (1u << 2)    /* end it at once: was SIGQUIT */
#define INTERRUPT_RELOAD (1u << 3)  /* reread the settings: was SIGHUP */
#define INTERRUPT_PROCSIG (1u << 4) /* see its ProcSignal: was SIGUSR1 */
#define INTERRUPT_ALARM (1u << 5)   /* a timeout is due: was SIGALRM */
#define INTERRUPT_ALL                                                        \
  (INTERRUPT_CANCEL | INTERRUPT_DIE | INTERRUPT_QUIT | INTERRUPT_RELOAD |   \
   INTERRUPT_PROCSIG | INTERRUPT_ALARM)

/*
 * The thread's latch (storage/latch.h): set by SetLatch(), taken by
 * ResetLatch(), and waited for by a WaitEventSet.  No handler runs for it,
 * so it is not among INTERRUPT_ALL, and no wait of sm's ends on it.
 */
#define INTERRUPT_LATCH (1u << 6)

typedef void (*interrupt_handler_t)(void);

struct PGPROC;
extern void SetInterruptHandler(uint32 bit, interrupt_handler_t handler);
extern void InterruptProc(struct PGPROC *proc, uint32 bits);
extern void InterruptSelf(uint32 bits);
extern void InterruptSelfLater(uint32 bits);
extern void DeliverInterrupts(void);
extern uint32 InterruptsOfSignal(int sig);

#endif /* PROCSIGNAL_H */
