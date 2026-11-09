/*
 * interrupt.cc -- interrupting a thread, interruptible waits, and
 * timers that interrupt (INTERRUPT.md, include/sm_sys.h).
 */
#include "sm0_defer.h"
#include "sm_sys.h"

#include "sm0_error.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

/* A thread's control block. */
struct sm_thread_t {
  std::atomic<uint32_t> pending{0}; /* raised, not yet taken */

  std::mutex mu;                                /* guards the four below */
  std::condition_variable *waiter_cv = nullptr; /* the wait under way */
  std::mutex *waiter_mu = nullptr;              /* its mutex */
  uint32_t waiter_mask = 0;                     /* the bits that end it */
  std::atomic<int> wake_fd{-1}; /* eventfd, once asked; set under mu */

  uint32_t wait_mask = ~0u; /* the owner's alone: no lock */
};

namespace {

/*
 * The calling thread's handle on its block. The holder closes the
 * wake fd as the thread ends -- nobody polls it any more -- and
 * drops the thread's own handle; the block lives on while others
 * hold one.
 */
struct self_holder_t {
  sm_thread_ref_t t;

  /* Close the wake fd; the handle goes with the holder. */
  ~self_holder_t() {
    if (!t)
      return;
    // The fd goes under the block's lock: interrupt() writes it.
    std::lock_guard<std::mutex> guard(t->mu);
    // Unpublished before it is closed, for a signal handler that
    // runs in between and writes it (sm_interrupt_self).
    int fd = t->wake_fd.exchange(-1);
    if (fd >= 0) {
      close(fd);
    }
  }
};

thread_local self_holder_t self;

/* The calling thread's handle, made on first use. */
const sm_thread_ref_t &self_ref() {
  if (!self.t)
    self.t = std::make_shared<sm_thread_t>();
  return self.t;
}

/* The calling thread's block. */
sm_thread_t *self_block() { return self_ref().get(); }

/*
 * A time on the monotonic clock, from microseconds; the clock's last for
 * one past what its nanoseconds hold, which would overflow into the past.
 */
std::chrono::steady_clock::time_point time_point_of(int64_t us) {
  using clock = std::chrono::steady_clock;
  constexpr int64_t most_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          clock::duration::max())
          .count();
  if (us >= most_us) {
    return clock::time_point::max();
  }
  return clock::time_point(std::chrono::microseconds(us));
}

} // namespace

/* A handle on the calling thread's control block. */
sm_thread_ref_t sm_thread_self(void) { return self_ref(); }

/*
 * Raise bits in t and wake it. The waiter takes its mutex first and
 * the block's lock second, so this only tries the waiter's mutex.
 * 1. Raise the bits in t's pending word.
 * 2. Take the block's lock.
 * 3. Write the wake fd, if t has one and this call has not written
 *    it yet: once is enough to make it readable.
 * 4. Return if t is in no wait, or its wait's mask has none of the
 *    bits.
 * 5. Try the waiter's mutex. If it is free, notify the waiter, let
 *    the mutex go, and return.
 * 6. Else let the block's lock go, yield, and go back to step 2. A
 *    busy mutex means the waiter is not asleep, or another thread
 *    holds it for a moment.
 */
void sm_interrupt(const sm_thread_ref_t &t, uint32_t bits) {
  t->pending.fetch_or(bits, std::memory_order_release);

  bool written = false;
  for (;;) {
    // The block's lock pins the waiter and the fd while we look.
    std::unique_lock<std::mutex> guard(t->mu);

    int fd = t->wake_fd.load();
    if (fd >= 0 && !written) {
      uint64_t one = 1;
      ssize_t rc = write(fd, &one, sizeof(one));
      (void)rc; /* full (EAGAIN) is still readable: fine */
      written = true;
    }
    if (t->waiter_cv == nullptr || (t->waiter_mask & bits) == 0)
      return;

    // Holding the waiter's mutex, the waiter is asleep or has yet
    // to check the bits: either way it cannot miss them.
    if (t->waiter_mu->try_lock()) {
      t->waiter_cv->notify_all();
      t->waiter_mu->unlock();
      return;
    }
    guard.unlock();
    std::this_thread::yield();
  }
}

/*
 * Raise bits in the calling thread, from a signal handler perhaps:
 * an atomic or, and a write(), and no lock, no allocation.
 */
void sm_interrupt_self(uint32_t bits) {
  sm_thread_t *t = self.t.get();

  if (t == nullptr)
    return;
  t->pending.fetch_or(bits, std::memory_order_release);
  int fd = t->wake_fd.load();
  if (fd >= 0) {
    // A handler leaves errno as it found it.
    int saved = errno;
    uint64_t one = 1;
    ssize_t rc = write(fd, &one, sizeof(one));
    (void)rc; /* full (EAGAIN) is still readable: fine */
    errno = saved;
  }
}

/* The calling thread's word of pending bits. */
const std::atomic<uint32_t> *sm_pending_word(void) {
  return &self_block()->pending;
}

/* The calling thread's pending bits. */
uint32_t sm_pending(void) {
  return self_block()->pending.load(std::memory_order_relaxed);
}

/* Claim the calling thread's bits in mask. */
uint32_t sm_take(uint32_t mask) {
  return self_block()->pending.fetch_and(~mask, std::memory_order_acquire) &
         mask;
}

/* Wait on cv until done, an interrupt in mask, or the deadline. */
sm_wait_result_t sm_wait(std::unique_lock<std::mutex> *lock,
                         std::condition_variable *cv, sm_wait_pred_t done,
                         void *arg, uint32_t mask, int64_t deadline_us) {
  sm_thread_t *t = self_block();
  sm_wait_result_t result;

  {
    // Register, so an interrupt knows what to wake.
    std::lock_guard<std::mutex> guard(t->mu);
    t->waiter_cv = cv;
    t->waiter_mu = lock->mutex();
    t->waiter_mask = mask;
  }
  // Unregister however the wait ends, an exception from done among the
  // ways: nothing may wake it, nor reach its cv and mutex, after.
  defer {
    std::lock_guard<std::mutex> guard(t->mu);
    t->waiter_cv = nullptr;
    t->waiter_mu = nullptr;
    t->waiter_mask = 0;
  };

  for (;;) {
    if (done(arg)) {
      result = SM_WAIT_DONE;
      break;
    }
    if (t->pending.load(std::memory_order_acquire) & mask) {
      result = SM_WAIT_INTERRUPTED;
      break;
    }
    if (deadline_us == SM_FOREVER) {
      cv->wait(*lock);
    } else if (sm_now_us() >= deadline_us) {
      result = SM_WAIT_TIMED_OUT;
      break;
    } else {
      cv->wait_until(*lock, time_point_of(deadline_us));
    }
  }
  return result;
}

/* The calling thread's eventfd, made on the first call. */
int sm_wake_fd(void) {
  sm_thread_t *t = self_block();

  // Made under the block's lock: interrupt() writes it.
  std::lock_guard<std::mutex> guard(t->mu);
  if (t->wake_fd.load() < 0) {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    // Report a failure as every sm call does: the thread's error.
    if (fd < 0)
      return sm0::set_errno_error(errno, "eventfd");
    t->wake_fd = fd;
  }
  return t->wake_fd.load();
}

/* Drain the calling thread's eventfd. */
void sm_clear_wake_fd(void) {
  sm_thread_t *t = self_block();
  uint64_t count;
  int fd;

  {
    // Read the fd under the lock; only this thread closes it.
    std::lock_guard<std::mutex> guard(t->mu);
    fd = t->wake_fd;
  }
  if (fd < 0)
    return;
  // An eventfd counts: one read takes all, then EAGAIN.
  while (read(fd, &count, sizeof(count)) == sizeof(count)) {
  }
}

/* Set the calling thread's wait mask. */
void sm_set_wait_mask(uint32_t bits) { self_block()->wait_mask = bits; }

/* The calling thread's wait mask. */
uint32_t sm_wait_mask(void) { return self_block()->wait_mask; }

namespace {

/* The host's stop function, for every thread; none by default. */
std::atomic<sm_stop_fn> stop_fn{nullptr};

} // namespace

/* Set the host's stop function; see sm_sys.h. */
void sm_set_stop_fn(sm_stop_fn fn) { stop_fn.store(fn); }

/* Whether the calling thread is to stop work that cannot be retried. */
bool sm_should_stop(void) {
  sm_thread_t *t = self_block();
  if (0 == (t->pending.load(std::memory_order_relaxed) & t->wait_mask)) {
    return false;
  }
  sm_stop_fn fn = stop_fn.load();
  return nullptr == fn || fn();
}

/* Now, in microseconds on the monotonic clock. */
int64_t sm_now_us(void) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* ---- timers ---- */

namespace {

/* One timer. */
struct timer_entry_t {
  std::weak_ptr<sm_thread_t> thread; /* dropped once nobody holds it */
  uint32_t bits;
  int64_t period_us; /* 0: one-shot */
};

/*
 * The timers of the process, and the thread that fires them. Made
 * once and never freed: the thread runs as long as the process.
 */
struct timer_service_t {
  std::mutex mu;                    /* guards everything below */
  std::condition_variable cv;       /* the timer thread sleeps on it */
  std::condition_variable fired_cv; /* cancel waits out a firing on it */
  std::map<std::pair<int64_t, sm_timer_id_t>, timer_entry_t> by_when;
  std::unordered_map<sm_timer_id_t, int64_t> when_of;
  sm_timer_id_t next_id = 1;
  sm_timer_id_t firing = 0; /* the timer being raised, 0 if none */
};

timer_service_t *service = nullptr;
std::once_flag service_once;

/*
 * The timer thread. It takes no signal.
 * 1. Sleep until the earliest timer is due.
 * 2. Take the timer out of the service.
 * 3. If the timer's thread is gone, drop the timer.
 * 4. Else raise its bits outside the lock, and arm it again if it is
 *    periodic: for its next period, or a period from now if that is
 *    past.
 * 5. Go back to step 1.
 */
void timer_main(timer_service_t *s) {
  // A signal to the process is never this thread's to take, whichever
  // thread started it: block them all.
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);

  std::unique_lock<std::mutex> lock(s->mu);

  for (;;) {
    if (s->by_when.empty()) {
      s->cv.wait(lock);
      continue;
    }
    auto first = s->by_when.begin();
    int64_t when = first->first.first;
    // Due never: sleep until a timer is armed or cancelled. Its time
    // point would overflow steady_clock's nanoseconds into the past,
    // and the wait would return at once, again and again.
    if (SM_FOREVER == when) {
      s->cv.wait(lock);
      continue;
    }
    if (sm_now_us() < when) {
      s->cv.wait_until(lock, time_point_of(when));
      continue;
    }

    sm_timer_id_t id = first->first.second;
    timer_entry_t timer = first->second;
    s->by_when.erase(first);
    s->when_of.erase(id);
    s->firing = id;

    sm_thread_ref_t target = timer.thread.lock();
    if (target) {
      // Raise outside the lock: it may yield on a busy waiter.
      lock.unlock();
      sm_interrupt(target, timer.bits);
      target.reset();
      lock.lock();

      if (timer.period_us > 0) {
        // The next period's, or one from now if that is past: a timer
        // behind skips what it missed, not firing them all in a row,
        // which holds the lock a cancel waits for.
        int64_t next = when + timer.period_us;
        int64_t now = sm_now_us();
        if (next <= now) {
          next = now + timer.period_us;
        }
        s->by_when.emplace(std::make_pair(next, id), timer);
        s->when_of.emplace(id, next);
      }
    }
    s->firing = 0;
    s->fired_cv.notify_all();
  }
}

/* The timer service, started on first use. */
timer_service_t *timers() {
  std::call_once(service_once, [] {
    service = new timer_service_t();
    // The timer thread lives as long as the process: detached.
    std::thread(timer_main, service).detach();
  });
  return service;
}

} // namespace

/* Raise bits in t at when_us, and every period_us after if > 0. */
sm_timer_id_t sm_timer_at(const sm_thread_ref_t &t, int64_t when_us,
                          uint32_t bits, int64_t period_us) {
  timer_service_t *s = timers();
  sm_timer_id_t id;

  {
    // Arm under the service's lock; the timer thread reads it.
    std::lock_guard<std::mutex> guard(s->mu);
    id = s->next_id++;
    s->by_when.emplace(std::make_pair(when_us, id),
                       timer_entry_t{t, bits, period_us});
    s->when_of.emplace(id, when_us);
  }
  s->cv.notify_one();
  return id;
}

/*
 * Cancel a timer. A firing under way is waited out first, so that
 * once this returns the timer raises nothing more, and a "fired"
 * answer means its bits have landed.
 */
bool sm_timer_cancel(sm_timer_id_t id) {
  timer_service_t *s = timers();

  // Decide cancel or fire under the service's lock.
  std::unique_lock<std::mutex> lock(s->mu);
  s->fired_cv.wait(lock, [&] { return s->firing != id; });

  auto it = s->when_of.find(id);
  if (it == s->when_of.end())
    return false;
  s->by_when.erase(std::make_pair(it->second, id));
  s->when_of.erase(it);
  return true;
}
