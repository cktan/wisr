/*
 * interrupt.h -- interrupting a thread, interruptible waits, and
 * timers that interrupt (INTERRUPT.md). Not built yet: this is the
 * interface the design proposes.
 *
 * Each thread has a word of pending interrupt bits. Any thread may
 * raise bits in any other; the owner checks and claims them. A
 * thread blocked in sm1_wait() or polling its wake fd is woken. A
 * timer raises bits at a deadline. sm1 attaches no meaning to a
 * bit: the caller defines them, claims them, and decides what an
 * interrupt means. Nothing here uses signals.
 *
 * The interface is C in style: opaque handles with explicit
 * reference counts, times as int64_t microseconds on the monotonic
 * clock, and a function pointer with a void * for a predicate.
 * Only the mutex and condition variable a wait is made on are C++.
 *
 * Calls marked "any thread" may be made from any thread; the rest
 * act on the calling thread.
 */
#ifndef SM1_INTERRUPT_H
#define SM1_INTERRUPT_H

#include <stdint.h>

#include <condition_variable>
#include <mutex>

/* ---- threads ---- */

/* A thread's control block: its pending bits, its waiter, its fd. */
typedef struct sm1_thread_t sm1_thread_t;

/*
 * The calling thread's control block, made on its first call.
 * Never NULL. It is valid while the thread lives; a thread that
 * hands it to another for longer takes a reference for it.
 */
sm1_thread_t *sm1_thread_self(void);

/* Take a reference to t, which stays valid until released. Any thread. */
sm1_thread_t *sm1_thread_ref(sm1_thread_t *t);

/* Release a reference taken by sm1_thread_ref(). Any thread. */
void sm1_thread_unref(sm1_thread_t *t);

/* ---- raising and claiming ---- */

/*
 * Raise bits in t, then wake it if it waits on a mask that has any
 * of them, and write its wake fd if it has one. Bits already
 * pending stay as they are. Never fails. Any thread.
 */
void sm1_interrupt(sm1_thread_t *t, uint32_t bits);

/* The calling thread's pending bits: a relaxed load, cheap to poll. */
uint32_t sm1_pending(void);

/*
 * Claim the calling thread's bits in mask: clear them, and return
 * those that were set. Only the owner claims its bits.
 */
uint32_t sm1_take(uint32_t mask);

/* ---- waiting ---- */

/* How sm1_wait() ended. */
typedef enum sm1_wait_result_t {
  SM1_WAIT_DONE,        /* done(arg) was true */
  SM1_WAIT_INTERRUPTED, /* a bit in the mask was pending */
  SM1_WAIT_TIMED_OUT    /* the deadline passed */
} sm1_wait_result_t;

/* A wait's condition, read with the wait's mutex held. */
typedef bool (*sm1_wait_pred_t)(void *arg);

/* A deadline that never comes. */
#define SM1_FOREVER INT64_MAX

/*
 * Wait on cv until done(arg) is true, a bit in mask is pending, or
 * the monotonic clock reaches deadline_us (SM1_FOREVER for none).
 * The caller holds lock, which guards what done() reads; it is
 * held again on return. The wait takes no bit: one that ends it
 * is still pending afterwards. A bit raised before the call ends
 * it at once.
 */
sm1_wait_result_t sm1_wait(std::unique_lock<std::mutex> *lock,
                           std::condition_variable *cv, sm1_wait_pred_t done,
                           void *arg, uint32_t mask, int64_t deadline_us);

/*
 * The calling thread's wake fd, an eventfd made on the first call:
 * readable once an interrupt has been raised since it was last
 * drained, whatever the bits. For a thread that waits in poll() or
 * epoll_wait(). Returns -1 with the error set if it cannot be made.
 */
int sm1_wake_fd(void);

/* Drain the calling thread's wake fd; check sm1_pending() after. */
void sm1_clear_wake_fd(void);

/*
 * The bits that end the calling thread's waits inside sm1 -- for a
 * row lock, a row being written, a deadlock check -- which then
 * fail with SM_ERR_INTERRUPTED. Default: all bits.
 */
void sm1_set_wait_mask(uint32_t bits);

/* The calling thread's mask for waits inside sm1. */
uint32_t sm1_wait_mask(void);

/* ---- time ---- */

/* Now, in microseconds on the monotonic clock. Any thread. */
int64_t sm1_now_us(void);

/* ---- timers ---- */

/* A timer's id; never 0. */
typedef uint64_t sm1_timer_id_t;

/*
 * Raise bits in t when the monotonic clock reaches when_us, and
 * then every period_us after if period_us > 0. The timer holds a
 * reference to t until it is cancelled or, one-shot, fires. One
 * process-wide thread runs the timers, started by the first call.
 * Returns the timer's id. Any thread.
 */
sm1_timer_id_t sm1_timer_at(sm1_thread_t *t, int64_t when_us, uint32_t bits,
                            int64_t period_us);

/*
 * Cancel a timer. Cancel and expiry are decided under one lock, so
 * exactly one happens: true if the timer was removed before it
 * fired, false if it fired (one-shot) or was not there. Once this
 * returns, the timer raises nothing more. To wait on the same bit
 * again: cancel, then sm1_take() the bit, then arm the next timer.
 * Any thread.
 */
bool sm1_timer_cancel(sm1_timer_id_t id);

#endif /* SM1_INTERRUPT_H */
