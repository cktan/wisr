/*
 * interrupt.cc -- interrupting a thread, interruptible waits, and
 * timers that interrupt (INTERRUPT.md, include/sys.h).
 */
#include "sys.h"

#include <errno.h>
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
struct sys_thread_t {
  std::atomic<uint32_t> pending{0}; /* raised, not yet taken */
  std::atomic<int> refs{1};         /* the thread's own, and others' */

  std::mutex mu;                                /* guards the four below */
  std::condition_variable *waiter_cv = nullptr; /* the wait under way */
  std::mutex *waiter_mu = nullptr;              /* its mutex */
  uint32_t waiter_mask = 0;                     /* the bits that end it */
  int wake_fd = -1;                             /* eventfd, once asked */

  uint32_t wait_mask = ~0u; /* the owner's alone: no lock */
};

namespace {

/*
 * The calling thread's block. The holder drops the thread's own
 * reference as the thread ends, and closes its wake fd: nobody
 * polls it any more.
 */
struct self_holder_t {
  sys_thread_t *t = nullptr;

  /* Close the wake fd and drop the thread's reference. */
  ~self_holder_t() {
    if (t == nullptr)
      return;
    {
      // The fd goes under the block's lock: interrupt() writes it.
      std::lock_guard<std::mutex> guard(t->mu);
      if (t->wake_fd >= 0) {
        close(t->wake_fd);
        t->wake_fd = -1;
      }
    }
    sys_thread_unref(t);
  }
};

thread_local self_holder_t self;

/* The calling thread's block, made on first use. */
sys_thread_t *self_block() {
  if (self.t == nullptr)
    self.t = new sys_thread_t();
  return self.t;
}

/* A time on the monotonic clock, from microseconds. */
std::chrono::steady_clock::time_point time_point_of(int64_t us) {
  return std::chrono::steady_clock::time_point(std::chrono::microseconds(us));
}

} // namespace

/* The calling thread's control block, made on its first call. */
sys_thread_t *sys_thread_self(void) { return self_block(); }

/* Take a reference to t. */
sys_thread_t *sys_thread_ref(sys_thread_t *t) {
  t->refs.fetch_add(1, std::memory_order_relaxed);
  return t;
}

/* Release a reference; the last one frees the block. */
void sys_thread_unref(sys_thread_t *t) {
  if (t->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
    delete t;
}

/*
 * Raise bits in t and wake it. The waiter's mutex is only tried,
 * never waited for, while the block's lock is held: the waiter
 * takes them the other way round, its mutex first. A busy mutex
 * means the waiter is not asleep, or another thread holds it for
 * a moment; let go and try again.
 */
void sys_interrupt(sys_thread_t *t, uint32_t bits) {
  t->pending.fetch_or(bits, std::memory_order_release);

  for (;;) {
    // The block's lock pins the waiter and the fd while we look.
    std::unique_lock<std::mutex> guard(t->mu);

    if (t->wake_fd >= 0) {
      uint64_t one = 1;
      ssize_t rc = write(t->wake_fd, &one, sizeof(one));
      (void)rc; /* full (EAGAIN) is still readable: fine */
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

/* The calling thread's pending bits. */
uint32_t sys_pending(void) {
  return self_block()->pending.load(std::memory_order_relaxed);
}

/* Claim the calling thread's bits in mask. */
uint32_t sys_take(uint32_t mask) {
  return self_block()->pending.fetch_and(~mask, std::memory_order_acquire) &
         mask;
}

/* Wait on cv until done, an interrupt in mask, or the deadline. */
sys_wait_result_t sys_wait(std::unique_lock<std::mutex> *lock,
                           std::condition_variable *cv, sys_wait_pred_t done,
                           void *arg, uint32_t mask, int64_t deadline_us) {
  sys_thread_t *t = self_block();
  sys_wait_result_t result;

  {
    // Register, so an interrupt knows what to wake.
    std::lock_guard<std::mutex> guard(t->mu);
    t->waiter_cv = cv;
    t->waiter_mu = lock->mutex();
    t->waiter_mask = mask;
  }

  for (;;) {
    if (done(arg)) {
      result = SYS_WAIT_DONE;
      break;
    }
    if (t->pending.load(std::memory_order_acquire) & mask) {
      result = SYS_WAIT_INTERRUPTED;
      break;
    }
    if (deadline_us == SYS_FOREVER) {
      cv->wait(*lock);
    } else if (sys_now_us() >= deadline_us) {
      result = SYS_WAIT_TIMED_OUT;
      break;
    } else {
      cv->wait_until(*lock, time_point_of(deadline_us));
    }
  }

  {
    // Unregister: nothing may wake this wait now.
    std::lock_guard<std::mutex> guard(t->mu);
    t->waiter_cv = nullptr;
    t->waiter_mu = nullptr;
    t->waiter_mask = 0;
  }
  return result;
}

/* The calling thread's eventfd, made on the first call. */
int sys_wake_fd(void) {
  sys_thread_t *t = self_block();

  // Made under the block's lock: interrupt() writes it.
  std::lock_guard<std::mutex> guard(t->mu);
  if (t->wake_fd < 0)
    t->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  return t->wake_fd;
}

/* Drain the calling thread's eventfd. */
void sys_clear_wake_fd(void) {
  sys_thread_t *t = self_block();
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
void sys_set_wait_mask(uint32_t bits) { self_block()->wait_mask = bits; }

/* The calling thread's wait mask. */
uint32_t sys_wait_mask(void) { return self_block()->wait_mask; }

/* Now, in microseconds on the monotonic clock. */
int64_t sys_now_us(void) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* ---- timers ---- */

namespace {

/* One timer. */
struct timer_entry_t {
  sys_thread_t *thread; /* a reference, held until it is dropped */
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
  std::map<std::pair<int64_t, sys_timer_id_t>, timer_entry_t> by_when;
  std::unordered_map<sys_timer_id_t, int64_t> when_of;
  sys_timer_id_t next_id = 1;
  sys_timer_id_t firing = 0; /* the timer being raised, 0 if none */
};

timer_service_t *service = nullptr;
std::once_flag service_once;

/*
 * The timer thread: sleep until the earliest timer is due, raise
 * its bits outside the lock, and arm it again if it is periodic.
 */
void timer_main(timer_service_t *s) {
  std::unique_lock<std::mutex> lock(s->mu);

  for (;;) {
    if (s->by_when.empty()) {
      s->cv.wait(lock);
      continue;
    }
    auto first = s->by_when.begin();
    int64_t when = first->first.first;
    if (sys_now_us() < when) {
      s->cv.wait_until(lock, time_point_of(when));
      continue;
    }

    sys_timer_id_t id = first->first.second;
    timer_entry_t timer = first->second;
    s->by_when.erase(first);
    s->when_of.erase(id);
    s->firing = id;

    // Raise outside the lock: it may yield on a busy waiter.
    lock.unlock();
    sys_interrupt(timer.thread, timer.bits);
    lock.lock();

    if (timer.period_us > 0) {
      int64_t next = when + timer.period_us;
      s->by_when.emplace(std::make_pair(next, id), timer);
      s->when_of.emplace(id, next);
    } else {
      sys_thread_unref(timer.thread);
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
sys_timer_id_t sys_timer_at(sys_thread_t *t, int64_t when_us, uint32_t bits,
                            int64_t period_us) {
  timer_service_t *s = timers();
  sys_timer_id_t id;

  {
    // Arm under the service's lock; the timer thread reads it.
    std::lock_guard<std::mutex> guard(s->mu);
    id = s->next_id++;
    s->by_when.emplace(std::make_pair(when_us, id),
                       timer_entry_t{sys_thread_ref(t), bits, period_us});
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
bool sys_timer_cancel(sys_timer_id_t id) {
  timer_service_t *s = timers();
  sys_thread_t *thread;

  {
    // Decide cancel or fire under the service's lock.
    std::unique_lock<std::mutex> lock(s->mu);
    s->fired_cv.wait(lock, [&] { return s->firing != id; });

    auto it = s->when_of.find(id);
    if (it == s->when_of.end())
      return false;
    auto entry = s->by_when.find(std::make_pair(it->second, id));
    thread = entry->second.thread;
    s->by_when.erase(entry);
    s->when_of.erase(it);
  }
  sys_thread_unref(thread);
  return true;
}
