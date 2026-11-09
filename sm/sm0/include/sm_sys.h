/*
 * sm_sys.h -- interrupting a thread, interruptible waits,
 * and timers that interrupt (INTERRUPT.md).
 *
 * Each thread has a word of pending interrupt bits. Any thread may
 * raise bits in any other; the owner checks and claims them. A
 * thread blocked in sm_wait() or polling its wake fd is woken. A
 * timer raises bits at a deadline. sm_sys.h attaches no meaning to a
 * bit: the caller defines them, claims them, and decides what an
 * interrupt means. Nothing here uses signals.
 *
 * The interface is C in style: sm_ functions, times as int64_t
 * microseconds on the monotonic clock, and a function pointer with
 * a void * for a predicate. What is C++ is a thread's handle, a
 * std::shared_ptr to its control block, and the mutex and
 * condition variable a wait is made on.
 *
 * Calls marked "any thread" may be made from any thread; the rest
 * act on the calling thread.
 */
#ifndef SM_SYS_H
#define SM_SYS_H

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

/* ---- threads ---- */

/* A thread's control block: its pending bits, its waiter, its fd. */
typedef struct sm_thread_t sm_thread_t;

/*
 * A handle on a thread's control block, shared by whoever may
 * interrupt the thread. The block lives as long as any handle,
 * the thread's own included, so a handle kept after its thread
 * ended is still safe to interrupt: the bits land where nobody
 * reads them.
 */
typedef std::shared_ptr<sm_thread_t> sm_thread_ref_t;

/*
 * A handle on the calling thread's control block, made on its
 * first call. Never empty. The thread holds one of its own until
 * it ends.
 */
sm_thread_ref_t sm_thread_self(void);

/* ---- raising and claiming ---- */

/*
 * Raise bits in t, then wake it if it waits on a mask that has any
 * of them, and write its wake fd if it has one. Bits already
 * pending stay as they are. Never fails. Any thread -- but not one
 * holding the mutex of a wait this may wake: waking takes that
 * mutex, and would wait for it forever.
 */
void sm_interrupt(const sm_thread_ref_t &t, uint32_t bits);

/*
 * Raise bits in the calling thread, and write its wake fd if it
 * has one. Lock-free, so safe in a signal handler: for a handler
 * that must wake its own thread's poll loop. It wakes no
 * sm_wait(), which a handler could not have interrupted safely
 * anyway. The thread must have a block already -- any earlier sm_sys.h
 * call on it, such as sm_wake_fd() -- or nothing is raised: a
 * handler cannot make one.
 */
void sm_interrupt_self(uint32_t bits);

/* The calling thread's pending bits: a relaxed load, cheap to poll. */
uint32_t sm_pending(void);

/*
 * The calling thread's word of pending bits, for a hot loop that
 * polls it inline rather than calling sm_pending(). Valid until
 * the thread ends; only the owner reads it.
 */
const std::atomic<uint32_t> *sm_pending_word(void);

/*
 * Claim the calling thread's bits in mask: clear them, and return
 * those that were set. Only the owner claims its bits.
 */
uint32_t sm_take(uint32_t mask);

/* ---- waiting ---- */

/* How sm_wait() ended. */
typedef enum sm_wait_result_t {
  SM_WAIT_DONE,        /* done(arg) was true */
  SM_WAIT_INTERRUPTED, /* a bit in the mask was pending */
  SM_WAIT_TIMED_OUT    /* the deadline passed */
} sm_wait_result_t;

/* A wait's condition, read with the wait's mutex held. */
typedef bool (*sm_wait_pred_t)(void *arg);

/* A deadline that never comes. */
#define SM_FOREVER INT64_MAX

/*
 * Wait on cv until done(arg) is true, a bit in mask is pending, or
 * the monotonic clock reaches deadline_us (SM_FOREVER for none).
 * The caller holds lock, which guards what done() reads; it is
 * held again on return. The wait takes no bit: one that ends it
 * is still pending afterwards. A bit raised before the call ends
 * it at once.
 */
sm_wait_result_t sm_wait(std::unique_lock<std::mutex> *lock,
                         std::condition_variable *cv, sm_wait_pred_t done,
                         void *arg, uint32_t mask, int64_t deadline_us);

/*
 * The calling thread's wake fd, an eventfd made on the first call:
 * readable once an interrupt has been raised since it was last
 * drained, whatever the bits. For a thread that waits in poll() or
 * epoll_wait(). Returns -1 if it cannot be made, with the
 * thread's error set as every sm call sets it (sm0.h: errnum()
 * and errmsg(), sm_errnum() and sm_errmsg() in C).
 */
int sm_wake_fd(void);

/* Drain the calling thread's wake fd; check sm_pending() after. */
void sm_clear_wake_fd(void);

/*
 * The calling thread's wait mask: the bits that end the waits a
 * library makes on its behalf, inside calls the thread makes --
 * sm's row lock waits, say, which then fail with an error of
 * their own. Default: all bits. sm_wait() does not read it: its
 * caller passes sm_wait_mask() as the mask when it should.
 */
void sm_set_wait_mask(uint32_t bits);

/* The calling thread's wait mask. */
uint32_t sm_wait_mask(void);

/*
 * Whether the calling thread is to stop the work a library is doing on
 * its behalf that cannot be retried, such as a sort, for an interrupt
 * pending in its wait mask (INTERRUPT.md). Set by the host, once, for
 * every thread: it may act on what is pending, as running a timer's
 * handler, and says whether what is pending now stops the work. It must
 * not unwind: it runs inside the library's call. No function, the
 * default: any bit in the mask stops it.
 */
typedef bool (*sm_stop_fn)(void);
void sm_set_stop_fn(sm_stop_fn fn);

/*
 * Whether the calling thread is to stop such work now: false with no
 * bit of its wait mask pending, a relaxed load; else what sm_set_stop_fn's
 * function says, or true with none.
 */
bool sm_should_stop(void);

/* ---- time ---- */

/* Now, in microseconds on the monotonic clock. Any thread. */
int64_t sm_now_us(void);

/* ---- timers ---- */

/* A timer's id; never 0. */
typedef uint64_t sm_timer_id_t;

/*
 * Raise bits in t when the monotonic clock reaches when_us, and
 * then every period_us after if period_us > 0. The timer holds t
 * weakly: one due after every handle on t is gone is dropped, not
 * fired. One process-wide thread runs the timers, started by the
 * first call. Returns the timer's id. Any thread.
 */
sm_timer_id_t sm_timer_at(const sm_thread_ref_t &t, int64_t when_us,
                          uint32_t bits, int64_t period_us);

/*
 * Cancel a timer. Cancel and expiry are decided under one lock, so
 * exactly one happens: true if the timer was removed while armed
 * -- before a one-shot fired, or between a periodic one's firings
 * -- false if a one-shot had fired, a timer was dropped because
 * its thread was gone, or the id was not there. A
 * firing under way is waited out first, so once this returns the
 * timer raises nothing more, and any bits it raised have landed.
 * Any thread -- but not one holding the mutex of a wait the timer
 * may wake: a firing under way waits for that mutex, and the cancel
 * waits for the firing.
 *
 * To wait on the same bit again:
 * 1. Cancel the timer.
 * 2. Claim the bit with sm_take().
 * 3. Arm the next timer.
 */
bool sm_timer_cancel(sm_timer_id_t id);

#endif /* SM_SYS_H */
