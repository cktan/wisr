# Interrupts and timers

A design for interrupting threads, and for timers that
interrupt them, built in `src/interrupt.cc`. sys knows nothing
of what
an interrupt means to its caller: it is grounded in
`std::thread`, gives each thread a word of pending interrupt
bits, lets any thread raise bits in any other, wakes a thread
blocked in an interruptible wait, and runs timers that raise
bits at a deadline. sm makes its blocking waits interruptible
with it; digres builds its query cancel, terminate and timeouts
on it.

The interface is `include/sys.h`, C in style: `sys_` names
outside any namespace, times as `int64_t` microseconds on the
monotonic clock, and a function pointer and a `void *` for a
predicate. What is C++ is a thread's handle, a
`std::shared_ptr`, and the mutex and condition variable a wait
is made on.

It replaced sm1's cancel protocol (`sm1::cancel` and the
session's `cancel` flag), which tied a cancel to a session
rather than to the thread that blocks.

## 1. Goals

- **Any thread can interrupt any other**, by bits it chooses.
- **A blocked thread wakes**, whether it waits on a condition
  variable (sm's lock waits) or in a poll loop (digres's socket
  waits).
- **No interrupt is lost**: one raised before a wait begins
  ends that wait.
- **No signals.** Interrupts and timers are atomics, mutexes,
  condition variables and an eventfd.
- **sys attaches no meaning to a bit.** The caller defines
  them, claims them and decides what to do.

## 2. Thread identity

```cpp
typedef struct sys_thread_t sys_thread_t;    // opaque, one per thread
typedef std::shared_ptr<sys_thread_t> sys_thread_ref_t;

sys_thread_ref_t sys_thread_self(void);      // made on first call
```

Each thread that uses the facility has a control block, made
the first time it calls `sys_thread_self()`, and handed out as
a `std::shared_ptr`. The thread holds a handle of its own until
it ends; another thread that must interrupt it keeps a copy. The
block lives as long as any handle, so an interrupt that arrives
after the thread ended is harmless, not a use after free: the
bits land where nobody reads them. A thread hands its handle to
whoever must interrupt it; sys keeps no registry of threads.

A handle kept in memory C++ did not construct -- a struct
allocated with `malloc()` -- must be made there with placement
new, and destroyed by hand, like any other C++ object.

The block holds:

- `pending`, a `std::atomic<uint32_t>` of raised bits;
- a mutex, guarding the two below;
- the waiter, if the thread is blocked in `sys_wait()`: the
  condition variable, its mutex, and the wait's mask;
- the thread's eventfd, if it asked for one (§4.2);
- the thread's wait mask (§5).

## 3. Raising and claiming

```cpp
void     sys_interrupt(const sys_thread_ref_t &t, uint32_t bits);
void     sys_interrupt_self(uint32_t bits);  // signal-safe, own thread
uint32_t sys_pending(void);           // the calling thread's bits
const std::atomic<uint32_t> *sys_pending_word(void);
uint32_t sys_take(uint32_t mask);     // claim: clear and return them
```

- **`sys_interrupt()`** sets the bits with `fetch_or`
  (release), then, under the block's mutex, wakes the waiter if
  the thread is blocked in a wait whose mask includes any of
  them, and writes the eventfd if there is one. It never fails,
  and holds a lock only long enough to notify.
- **`sys_interrupt_self()`** is for a signal handler that must
  wake its own thread's poll loop, as digres's postmaster's
  handlers do. It takes no lock and allocates nothing: a
  `fetch_or` and a `write()` to the eventfd, with `errno` kept.
  It wakes no `sys_wait()`; a handler that interrupted one
  could not take its mutex anyway. A thread with no block yet
  has nothing to raise into, and the call does nothing: make
  the block first, with `sys_wake_fd()`. The eventfd is
  unpublished before it is closed, as the thread ends, so a
  handler never writes a closed one.
- **`sys_pending()`** is a relaxed load: cheap enough for a
  check on every loop iteration, as digres's
  `CHECK_FOR_INTERRUPTS` is. `sys_pending_word()` gives the
  word itself, for a loop that would rather load it inline
  than call: valid until the thread ends.
- **`sys_take()`** clears the bits in `mask` with `fetch_and`
  (acquire) and returns those that were set. Only the owning
  thread claims its bits.
- **Bits are sticky** until taken. Raising a bit already set is
  one interrupt, not two.

Raising and acting are apart: a thread that is not waiting
sees the bits at its next check of `sys_pending()`.

## 4. Interruptible waiting

### 4.1 On a condition variable

```cpp
typedef enum sys_wait_result_t {
  SYS_WAIT_DONE, SYS_WAIT_INTERRUPTED, SYS_WAIT_TIMED_OUT
} sys_wait_result_t;
typedef bool (*sys_wait_pred_t)(void *arg);

sys_wait_result_t sys_wait(std::unique_lock<std::mutex> *lock,
                           std::condition_variable *cv,
                           sys_wait_pred_t done, void *arg,
                           uint32_t mask, int64_t deadline_us);
```

The calling thread holds `lock`, the mutex that guards what
`done(arg)` reads; `deadline_us` is on the monotonic clock
(`sys_now_us()`), or `SYS_FOREVER`. The wait:

1. registers `{cv, lock's mutex, mask}` as the thread's waiter,
   under the block's mutex;
2. loops: returns `SYS_WAIT_DONE` if `done(arg)`,
   `SYS_WAIT_INTERRUPTED` if `sys_pending() & mask`,
   `SYS_WAIT_TIMED_OUT` past the deadline, and otherwise sleeps
   on `cv` until the deadline;
3. unregisters the waiter, whichever way it returns.

`sys_interrupt()` wakes the waiter by taking the waiter's mutex
and then calling `notify_all()` on its condition variable. The
waiter checks `sys_pending() & mask` under that same mutex
before every sleep, so a wakeup cannot be lost: if the bit was
set before the check, the check sees it; if after, the thread
is asleep on its condition variable, or about to be with the
mutex held, and the notify reaches it.

The two locks are taken in opposite orders: the waiter holds
its mutex when it registers under the block's, and the
interrupter finds the waiter under the block's lock before it
needs the waiter's mutex. So the interrupter only tries the
waiter's mutex, never waits for it with the block's lock held:
if it is busy -- the waiter is awake, or another thread holds it
for a moment -- it lets go, yields, and tries again. The bit is
set before any of this, so a waiter that is awake sees it at
its next check.

It follows that a thread must not call `sys_interrupt()` while
it holds the mutex of a wait the call may wake: the try would
never succeed.

The wait never takes a bit. An interrupt that ends a wait is
still pending when the wait returns, for the owner to claim.

What it costs: an uninterrupted wait locks the block's mutex
twice, to register and to unregister. An interrupt to a thread
that is not waiting is one atomic and a short lock.

### 4.2 In a poll loop

```cpp
int  sys_wake_fd(void);        // the calling thread's eventfd
void sys_clear_wake_fd(void);  // drain it
```

A thread that waits in `poll()` or `epoll_wait()` cannot wait on
a condition variable as well. It asks for an eventfd, adds it to
its set, and when it is readable drains it with
`sys_clear_wake_fd()` and checks `sys_pending()`.
`sys_interrupt()` writes it whatever the bits, once the thread
has asked for one: the mask is the poll loop's business.
Draining before checking keeps the order safe: a bit raised
after the drain writes the fd again. `sys_wake_fd()` returns -1
with `errno` set if the eventfd cannot be made.

## 5. The wait mask

```cpp
void     sys_set_wait_mask(uint32_t bits);  // default: all bits
uint32_t sys_wait_mask(void);
```

A library that waits on a thread's behalf, inside a call the
thread made, does not know the thread's bits. The wait mask
says which of them should end such waits: the library passes
`sys_wait_mask()` as the mask to `sys_wait()`. `sys_wait()`
itself does not read it.

sm is the first such library. Its waits a caller can block in
-- for a row lock, for a row another transaction is writing,
and the deadlock wait -- are to use `sys_wait()` with the wait
mask. An interrupt in the mask ends the wait, and the call fails
with a new sm error, `SM_ERR_INTERRUPTED`. That is not an abort:
the transaction stays as it was, holding what it held, and the
caller decides what the interrupt meant and whether to abort.
sm never takes a bit, so every later wait the thread makes
fails the same way until the caller takes it.

sm1's waits share a condition variable per database
(`lock_cv`). Registering it means an interrupt wakes every
waiter on that database, not only the target. The others find
nothing pending and sleep again: correct, and noisy only under
many waiters. A condition variable per waiter is the later
optimisation.

## 6. Timers

```cpp
typedef uint64_t sys_timer_id_t;   // never 0

int64_t        sys_now_us(void);
sys_timer_id_t sys_timer_at(const sys_thread_ref_t &t,
                            int64_t when_us, uint32_t bits,
                            int64_t period_us);
bool           sys_timer_cancel(sys_timer_id_t id);
```

- **One timer thread for the process**, a `std::thread` sys
  starts on the first `sys_timer_at()`. It keeps a min-heap of
  `(when, id, thread, bits, period)` and sleeps on a condition
  variable of its own until the earliest deadline, or until a
  new timer is earlier.
- **Expiry is an interrupt**: `sys_interrupt(t, bits)`. A
  timeout is one more bit; nothing runs on the timer thread but
  the raise.
- **A periodic timer** (`period_us > 0`) is armed again at
  `when_us + period_us`.
- **A timer holds its thread weakly** (a `std::weak_ptr`): it
  does not keep the block alive. One that comes due after every
  handle on its thread is gone is dropped, not fired. While a
  handle remains, it fires into the block even if the thread
  has ended, and that is harmless.
- **Cancel or fire, exactly one.** `sys_timer_cancel()` and
  expiry decide under the timer mutex. It returns true if it
  removed the timer while armed -- before a one-shot fired, or
  between a periodic one's firings -- false if a one-shot had
  fired, the timer was dropped because its thread was gone, or
  the id was not there.
- **A fired answer means the bits landed.** The timer thread
  raises a timer's bits outside its lock, since raising may
  have to try a waiter's mutex again and again. So a cancel
  waits out a firing under way before it answers: once it
  returns, the timer raises nothing more. A thread must not
  cancel while holding the mutex of a wait the timer may wake:
  the firing would wait for that mutex, and the cancel for the
  firing.
- **The timer thread lives as long as the process.** It is
  detached, so no static destructor has to stop it at exit.

### 6.1 Waiting on a timeout again

A bit does not say which timer raised it, and it stays pending
until taken. A thread that was interrupted by a timeout, and
wants another on the same bit, clears the old one first:

```cpp
sys_timer_cancel(old_id);                    // can no longer fire
sys_take(T);                                 // discard its bit, if it did
id = sys_timer_at(self, when_us, T, 0);      // arm the next
```

Cancel comes before take: once the cancel returns, the old
timer has fired or never will, so the take clears any stale bit
for good. Taken first, the old timer could fire between the two
and end the next wait early. It follows that a bit has at most
one live timer at a time: a caller with several timeouts that
may overlap gives each its own bit.

A thread that only needs to bound one wait needs no timer at
all: `sys_wait()`'s deadline returns `SYS_WAIT_TIMED_OUT`, and
leaves no bit behind.

## 7. Guarantees

- An interrupt raised before a wait begins ends that wait.
- An interrupt never ends a wait whose mask excludes its bits.
- Neither `sys_interrupt()` nor `sys_timer_cancel()` may be
  called while holding the mutex of a wait it may wake.
- Bits stay pending until the owner takes them.
- `sys_interrupt()`, `sys_now_us()`, `sys_timer_at()` and
  `sys_timer_cancel()` may be called from any thread, as may a
  handle be copied or dropped; the rest act on the calling
  thread.
- Nothing here uses signals. `sys_interrupt_self()` may be
  called from a signal handler; nothing else may.

## 8. How sm uses it

- sm1's waits for another transaction, on a lock or an
  uncommitted writer, are a `sys_wait()` on the lock table's
  condition with `sys_wait_mask()`, and its deadline is the
  next deadlock check. An interrupt fails the call with
  `SM_ERR_INTERRUPTED`; sm takes no bit.
- sm3's wait for an index build (`builder_wait`) is a
  `sys_wait()` on the builder's condition, the same way.
- sm3's builder workers are sm's own threads, with a bit of
  sm's own: a detach raises it in each worker building one of
  the catalog's indexes, so one waiting on a transaction does
  not hold the detach up. The worker takes it as the build
  ends.

## 9. How digres uses it

- It defines its bits (`storage/procsignal.h`): `CANCEL`,
  `DIE`, `QUIT`, `RELOAD`, and `PROCSIG`, which carries
  invalidation catch-up, `NOTIFY` and the rest as reasons in
  the target's ProcSignal slot.
- A latch keeps its owner's handle, new'd when a thread owns
  it and deleted when it lets go; `SetLatch()` on another
  thread's latch is `sys_interrupt(owner, 0)`, a wake with no
  bit, and from a signal handler of the owner's own,
  `sys_interrupt_self(0)`. `InterruptProc(proc, bits)` raises
  bits in the owner of the PGPROC's latch.
- `CHECK_FOR_INTERRUPTS` loads `sys_pending_word()` inline, and
  the epoll loop waits on `sys_wake_fd()`, in place of SIGURG
  and its signalfd.
- `statement_timeout` and the other timeouts are
  `sys_timer_at()`, a bit for each, in place of a timer of its
  own per thread and SIGALRM.
- `SM_ERR_INTERRUPTED` from an sm call goes to its usual cancel
  or terminate processing.

## 10. Tests

sys unit tests (`test/test_interrupt.cc`), each with its
synopsis and steps, run by `make test`, and under the
sanitizers by `make tsan` and `make asan`:

- an interrupt raised before a wait, during one, and to a
  thread that has ended;
- a wait whose mask excludes the bit stays asleep, and one
  whose deadline passes returns `SYS_WAIT_TIMED_OUT`;
- the eventfd is readable after an interrupt, and not after it
  is drained;
- a signal handler's `sys_interrupt_self()` raises the bit and
  writes the eventfd, and does nothing in a thread with no
  block;
- a timer fires once, a periodic one again and again, a
  cancelled one never; a cancel racing an expiry is decided
  once, and a fired answer means the bit is there -- with the
  timer thread kept busy, so that the race is met: without the
  cancel's wait for a firing, the test fails most runs.

And in sm, when its waits use sys: a row lock waiter
interrupted from another thread fails with
`SM_ERR_INTERRUPTED`, its transaction intact -- a new scenario
in `sm1/mvcc/`, added beside the others, none changed.

## 11. Open questions

1. **The eventfd.** Keep `sys_wake_fd()` in sys, or keep sys to
   condition variables and let a caller register a waker of its
   own?
2. **The wait mask.** A mask per thread lets a bit that should
   not abort a lock wait -- digres's reload -- pass. Without
   it, every wait sm makes for a caller ends on any bit, which
   is simpler.
