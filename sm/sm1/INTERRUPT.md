# Interrupts and timers

A design for interrupting threads, and for timers that
interrupt them, in sm1. It is not built yet. sm1 knows nothing
of what an interrupt means to its caller: it is grounded in
`std::thread`, gives each thread a word of pending interrupt
bits, lets any thread raise bits in any other, makes its own
blocking waits end when one arrives, and runs timers that raise
bits at a deadline. A host -- digres, say -- builds its query
cancel, terminate and timeouts on top.

It is to replace sm1's present cancel protocol (`sm1::cancel`
and the session's `cancel` flag), which ties a cancel to a
session; that protocol is to be reworked, and nothing here
depends on it.

## 1. Goals

- **Any thread can interrupt any other**, by bits it chooses.
- **A blocked thread wakes**, whether it waits on a condition
  variable (sm1's lock waits) or in a poll loop (a host's
  socket waits).
- **No interrupt is lost**: one raised before a wait begins
  ends that wait.
- **No signals.** Interrupts and timers are atomics, mutexes,
  condition variables and an eventfd.
- **sm1 attaches no meaning to a bit.** The caller defines
  them, claims them and decides what to do.

## 2. Thread identity

```cpp
namespace sm1 {
struct thread_t;                           // opaque, one per thread
using thread_ref = std::shared_ptr<thread_t>;

thread_ref this_thread();   // the calling thread's; made on first call
}
```

Each thread that uses the facility has a control block, made
the first time it calls `sm1::this_thread()` and held in a
`thread_local std::shared_ptr`. Other threads hold a
`thread_ref`; the shared pointer keeps the block valid after its
thread ends, so an interrupt or a timer that arrives late is
harmless, not a use after free. A thread hands its own ref to
whoever must interrupt it; sm1 keeps no registry of threads.

The block holds:

- `pending`, a `std::atomic<uint32_t>` of raised bits;
- a mutex, guarding the two below;
- the waiter, if the thread is blocked in `sm1::wait()`: the
  condition variable, its mutex, and the wait's mask;
- the thread's eventfd, if it asked for one (§4.2).

## 3. Raising and claiming

```cpp
void     interrupt(const thread_ref &t, uint32_t bits);  // any thread
uint32_t pending();                  // the calling thread's bits
uint32_t take(uint32_t mask);        // claim: clear and return those bits
```

- **`interrupt()`** sets the bits with `fetch_or` (release),
  then, under the block's mutex, wakes the waiter if the
  thread is blocked in a wait whose mask includes any of them,
  and writes the eventfd if there is one. It never fails, and
  holds a lock only long enough to notify.
- **`pending()`** is a relaxed load: cheap enough for a check
  on every loop iteration, as digres's `CHECK_FOR_INTERRUPTS`
  is.
- **`take()`** clears the bits in `mask` with `fetch_and`
  (acquire) and returns those that were set. Only the owning
  thread claims its bits.
- **Bits are sticky** until taken. Raising a bit already set is
  one interrupt, not two.

Raising and acting are apart: a thread that is not waiting
sees the bits at its next check of `pending()`.

## 4. Interruptible waiting

### 4.1 On a condition variable

```cpp
enum class wait_result_t { done, interrupted, timed_out };

template <class Pred>
wait_result_t wait(std::unique_lock<std::mutex> &lk,
                   std::condition_variable &cv, Pred done,
                   uint32_t mask,
                   std::chrono::steady_clock::time_point deadline =
                       std::chrono::steady_clock::time_point::max());
```

The calling thread holds `lk`, the mutex that guards what
`done()` reads. The wait:

1. registers `{&cv, lk.mutex(), mask}` as the thread's waiter,
   under the block's mutex;
2. loops: returns `done` if `done()`, `interrupted` if
   `pending() & mask`, `timed_out` past the deadline, and
   otherwise sleeps on `cv` until the deadline;
3. unregisters the waiter, whichever way it returns.

`interrupt()` wakes the waiter by taking the waiter's mutex and
then calling `cv.notify_all()`. The waiter checks
`pending() & mask` under that same mutex before every sleep, so
a wakeup cannot be lost: if the bit was set before the check,
the check sees it; if after, the thread is asleep on `cv`, or
about to be with the mutex held, and the notify reaches it.

The wait never takes a bit. An interrupt that ends a wait is
still pending when the wait returns, for the owner to claim.

What it costs: an uninterrupted wait locks the block's mutex
twice, to register and to unregister. An interrupt to a thread
that is not waiting is one atomic and a short lock.

### 4.2 In a poll loop

```cpp
int  wake_fd();         // the calling thread's eventfd, made on first call
void clear_wake_fd();   // drain it
```

A thread that waits in `poll()` or `epoll_wait()` cannot wait on
a condition variable as well. It asks for an eventfd, adds it to
its set, and when it is readable drains it with
`clear_wake_fd()` and checks `pending()`. `interrupt()` writes it
whatever the bits, once the thread has asked for one: the mask
is the poll loop's business. Draining before checking keeps the
order safe: a bit raised after the drain writes the fd again.

## 5. sm1's own waits

The waits a caller can block in -- for a row lock, for a row
another transaction is writing, and the deadlock wait -- use
`sm1::wait()`. Each thread has a mask of bits that end them:

```cpp
void set_wait_mask(uint32_t bits);   // the calling thread's; default all
```

An interrupt in the mask ends the wait, and the call fails with
a new error, `SM_ERR_INTERRUPTED`. That is not an abort: the
transaction stays as it was, holding what it held, and the
caller decides what the interrupt meant and whether to abort.
sm1 never takes a bit, so every later wait the thread makes
fails the same way until the caller takes it.

sm1's waits share a condition variable per database
(`lock_cv`). Registering it means an interrupt wakes every
waiter on that database, not only the target. The others find
nothing pending and sleep again: correct, and noisy only under
many waiters. A condition variable per waiter is the later
optimisation.

sm1's own threads (GC, the timer thread) do not take part
unless they choose to.

## 6. Timers

```cpp
using timer_id = uint64_t;

timer_id timer_at(const thread_ref &t,
                  std::chrono::steady_clock::time_point when,
                  uint32_t bits,
                  std::chrono::milliseconds period = {});
bool     timer_cancel(timer_id id);
```

- **One timer thread for the process**, a `std::thread` sm1
  starts on the first `timer_at()`. It keeps a min-heap of
  `(when, id, thread, bits, period)` and sleeps on a condition
  variable of its own until the earliest deadline, or until a
  new timer is earlier.
- **Expiry is an interrupt**: `interrupt(t, bits)`. A timeout is
  one more bit; nothing runs on the timer thread but the raise.
- **A periodic timer** is armed again at `when + period`.
- **The thread is held weakly.** A timer whose thread has ended
  fires into nothing and is dropped.
- **Cancel or fire, exactly one.** `timer_cancel()` and expiry
  decide under the timer mutex. It returns true if it removed
  the timer before it fired, false if the timer had fired (a
  one-shot) or was not there.
- **Not tied to an environment.** Timers are wanted with no
  home directory attached -- a standalone host, tests -- so the
  timer thread does not start with `sm1::attach` or stop with
  `sm1::detach`. It is detached and lives as long as the
  process, so no static destructor has to stop it at exit.

## 7. Guarantees

- An interrupt raised before a wait begins ends that wait.
- An interrupt never ends a wait whose mask excludes its bits,
  and never sets an error by itself.
- Bits stay pending until the owner takes them.
- `interrupt()`, `timer_at()` and `timer_cancel()` may be called
  from any thread; `wait()`, `pending()`, `take()`,
  `set_wait_mask()`, `wake_fd()` and `clear_wake_fd()` act on
  the calling thread.
- Nothing here uses signals.

## 8. How a host uses it

digres, for one:

- defines its bits: `CANCEL`, `DIE`, `RELOAD`, invalidation
  catch-up, `NOTIFY` and the rest;
- keeps each session's `thread_ref` where another session or
  the postmaster can find it, and `SignalProc()` becomes
  `sm1::interrupt(ref, bit)`;
- has `CHECK_FOR_INTERRUPTS` read `sm1::pending()`, and its
  epoll loop add `sm1::wake_fd()`, in place of SIGURG and its
  signalfd;
- sets `statement_timeout` and the other timeouts with
  `sm1::timer_at()`, in place of a timer of its own per thread
  and SIGALRM;
- turns `SM_ERR_INTERRUPTED` from an sm call into its usual
  cancel or terminate processing.

## 9. Tests

sm1 unit tests, each with its synopsis and steps:

- an interrupt raised before a wait, during one, and to a
  thread that has ended;
- a wait whose mask excludes the bit stays asleep, and one
  whose deadline passes returns `timed_out`;
- the eventfd is readable after an interrupt, and not after it
  is drained;
- a timer fires once, a periodic one again and again, a
  cancelled one never; a cancel racing an expiry is decided
  once;
- a row lock waiter interrupted from another thread fails with
  `SM_ERR_INTERRUPTED`, its transaction intact: a new scenario
  in `mvcc/`, added beside the others, none changed.

## 10. Open questions

1. **The eventfd.** Keep `wake_fd()` in sm1, or keep sm1 to
   condition variables and let a host register a waker of its
   own?
2. **Where the code lives.** In sm1 as asked, in a file and
   header of its own (`src/thread.cc`, `include/sm1_thread.h`)
   that nothing about databases depends on.
3. **The wait mask.** A mask per thread lets a host's bit that
   should not abort a lock wait -- a reload -- pass. Without it,
   every sm1 wait ends on any bit, which is simpler.
