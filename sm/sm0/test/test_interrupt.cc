/*
 * test_interrupt.cc -- sm_sys.h's interrupts, interruptible
 * waits, the wake fd, and timers (INTERRUPT.md).
 */
#include "doctest.h"

#include "sm0.h"
#include "sm_sys.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace {

const uint32_t B1 = 0x1;
const uint32_t B2 = 0x2;

/* State a wait is made on: a flag, its mutex and its cv. */
struct box_t {
  std::mutex mu;
  std::condition_variable cv;
  bool ready = false;
};

/* True once the box is ready: read with its mutex held. */
bool box_ready(void *arg) { return static_cast<box_t *>(arg)->ready; }

/* Wait on box for its flag or a bit in mask, until deadline_us. */
sm_wait_result_t wait_box(box_t *box, uint32_t mask, int64_t deadline_us) {
  std::unique_lock<std::mutex> lock(box->mu);
  return sm_wait(&lock, &box->cv, box_ready, box, mask, deadline_us);
}

/* Whether fd is readable now. */
bool readable(int fd) {
  struct pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

/* Sleep ms milliseconds. */
void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/*
 * Start a thread that hands its handle to *out, then runs body;
 * returns the thread.
 */
template <class F>
std::thread start_publishing(std::promise<sm_thread_ref_t> *out, F body) {
  return std::thread([out, body] {
    out->set_value(sm_thread_self());
    body();
  });
}

} // namespace

TEST_CASE("the header keeps the values it promises") {
  // Verify the constants the interface documents.
  // 1. Confirm SM_FOREVER is a deadline that never comes.
  // 2. Confirm the three wait results are distinct.
  CHECK(SM_FOREVER == INT64_MAX);
  CHECK(SM_WAIT_DONE != SM_WAIT_INTERRUPTED);
  CHECK(SM_WAIT_INTERRUPTED != SM_WAIT_TIMED_OUT);
  CHECK(SM_WAIT_DONE != SM_WAIT_TIMED_OUT);
}

TEST_CASE("bits stay pending until taken, and take claims only its mask") {
  // Verify raising, checking and claiming on the calling thread.
  // 1. Raise B1 and B2 in this thread.
  // 2. Confirm both are pending, and raising B1 again changes nothing.
  // 3. Take B1: it returns B1 and leaves B2 pending.
  // 4. Take B2, leaving nothing.
  sm_thread_ref_t self = sm_thread_self();
  sm_interrupt(self, B1 | B2);
  CHECK(sm_pending() == (B1 | B2));
  sm_interrupt(self, B1);
  CHECK(sm_pending() == (B1 | B2));
  CHECK(sm_take(B1) == B1);
  CHECK(sm_pending() == B2);
  CHECK(sm_take(B1 | B2) == B2);
  CHECK(sm_pending() == 0);
}

TEST_CASE("an interrupt raised before a wait ends it at once") {
  // Verify no interrupt is lost when it comes first.
  // 1. Raise B1 in this thread.
  // 2. Wait for a box nobody fills, on B1, with no deadline.
  // 3. Confirm the wait ends interrupted, and leaves B1 pending.
  box_t box;
  sm_interrupt(sm_thread_self(), B1);
  CHECK(wait_box(&box, B1, SM_FOREVER) == SM_WAIT_INTERRUPTED);
  CHECK(sm_take(B1) == B1);
}

TEST_CASE("an interrupt from another thread wakes a wait under way") {
  // Verify a sleeping waiter is woken.
  // 1. Start a thread that waits on an unfilled box, on B1.
  // 2. Let it fall asleep, then raise B1 in it from here.
  // 3. Confirm its wait ended interrupted.
  box_t box;
  std::promise<sm_thread_ref_t> who;
  sm_wait_result_t result = SM_WAIT_DONE;
  std::thread waiter =
      start_publishing(&who, [&] { result = wait_box(&box, B1, SM_FOREVER); });
  sm_thread_ref_t t = who.get_future().get();
  sleep_ms(50);
  sm_interrupt(t, B1);
  waiter.join();
  CHECK(result == SM_WAIT_INTERRUPTED);
}

TEST_CASE("the condition ends a wait, and is checked before the bits") {
  // Verify a wait ends on its own condition, first.
  // 1. Start a thread that waits on a box, on B1.
  // 2. Fill the box under its mutex and notify.
  // 3. Confirm the wait ended done.
  // 4. With the box full and B1 pending, confirm a wait is done.
  box_t box;
  sm_wait_result_t result = SM_WAIT_INTERRUPTED;
  std::thread waiter(
      [&] { result = wait_box(&box, B1, sm_now_us() + 5000000); });
  sleep_ms(20);
  {
    std::lock_guard<std::mutex> guard(box.mu);
    box.ready = true;
  }
  box.cv.notify_all();
  waiter.join();
  CHECK(result == SM_WAIT_DONE);

  sm_interrupt(sm_thread_self(), B1);
  CHECK(wait_box(&box, B1, SM_FOREVER) == SM_WAIT_DONE);
  CHECK(sm_take(B1) == B1);
}

TEST_CASE("a deadline ends a wait, and leaves no bit") {
  // Verify a bounded wait times out on its own.
  // 1. Wait 30 ms on an unfilled box, on B1.
  // 2. Confirm it timed out after at least 30 ms, with nothing pending.
  box_t box;
  int64_t start = sm_now_us();
  CHECK(wait_box(&box, B1, start + 30000) == SM_WAIT_TIMED_OUT);
  CHECK(sm_now_us() - start >= 30000);
  CHECK(sm_pending() == 0);
}

TEST_CASE("a bit outside the mask does not end a wait") {
  // Verify the mask is honoured.
  // 1. Start a thread that waits 100 ms on an unfilled box, on B1.
  // 2. Raise B2 in it while it waits.
  // 3. Confirm it timed out rather than being interrupted.
  box_t box;
  std::promise<sm_thread_ref_t> who;
  sm_wait_result_t result = SM_WAIT_DONE;
  uint32_t left = 0;
  std::thread waiter = start_publishing(&who, [&] {
    result = wait_box(&box, B1, sm_now_us() + 100000);
    left = sm_take(B1 | B2);
  });
  sm_thread_ref_t t = who.get_future().get();
  sleep_ms(20);
  sm_interrupt(t, B2);
  waiter.join();
  CHECK(result == SM_WAIT_TIMED_OUT);
  CHECK(left == B2);
}

TEST_CASE("an interrupt to a thread that has ended is harmless") {
  // Verify a held block outlives its thread.
  // 1. Start a thread that publishes its block and ends at once.
  // 2. Join it, then raise B1 in its block and release the reference.
  std::promise<sm_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sm_thread_ref_t t = who.get_future().get();
  gone.join();
  sm_interrupt(t, B1);
}

TEST_CASE("the wake fd is readable after an interrupt, until drained") {
  // Verify the eventfd a poll loop waits on.
  // 1. Get this thread's wake fd: not readable yet.
  // 2. Raise B1: it becomes readable.
  // 3. Drain it: not readable, and B1 still pending until taken.
  int fd = sm_wake_fd();
  REQUIRE(fd >= 0);
  CHECK(sm_wake_fd() == fd);
  CHECK_FALSE(readable(fd));
  sm_interrupt(sm_thread_self(), B1);
  CHECK(readable(fd));
  sm_clear_wake_fd();
  CHECK_FALSE(readable(fd));
  CHECK(sm_take(B1) == B1);
}

TEST_CASE("a wake fd that cannot be made fails as an sm call does") {
  // Verify sm_wake_fd() reports through the thread's sm error.
  // 1. Lower the process's open-file limit to 0, so eventfd fails.
  // 2. In a new thread, with no wake fd yet, sm_wake_fd() returns
  //    -1 with SM_ERR_IO and a message naming eventfd.
  // 3. Restore the limit: the same call in a new thread succeeds.
  struct rlimit saved;
  REQUIRE(getrlimit(RLIMIT_NOFILE, &saved) == 0);
  struct rlimit none = saved;
  none.rlim_cur = 0;
  REQUIRE(setrlimit(RLIMIT_NOFILE, &none) == 0);
  int fd = 0;
  sm0::errnum_t err = sm0::SM_OK;
  std::string msg;
  std::thread([&] {
    fd = sm_wake_fd();
    err = sm0::errnum();
    msg = sm0::errmsg();
  }).join();
  REQUIRE(setrlimit(RLIMIT_NOFILE, &saved) == 0);
  CHECK(fd == -1);
  CHECK(err == sm0::SM_ERR_IO);
  CHECK(msg.starts_with("eventfd: "));

  std::thread([&] { fd = sm_wake_fd(); }).join();
  CHECK(fd >= 0);
}

TEST_CASE("a signal handler raises bits in its own thread and wakes it") {
  // Verify sm_interrupt_self() from a handler lands and writes the fd.
  // 1. In a new thread, make the wake fd and install a SIGUSR1 handler
  //    that raises B2 with sm_interrupt_self().
  // 2. Signal the thread itself: B2 is pending, readable through the
  //    inline word too, and the wake fd is readable.
  // 3. In a thread with no block, sm_interrupt_self() raises nothing.
  bool pending = false, word = false, woke = false;
  std::thread([&] {
    REQUIRE(sm_wake_fd() >= 0);
    struct sigaction act = {};
    act.sa_handler = [](int) { sm_interrupt_self(B2); };
    sigemptyset(&act.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &act, nullptr) == 0);
    raise(SIGUSR1);
    pending = sm_pending() == B2;
    word = sm_pending_word()->load() == B2;
    woke = readable(sm_wake_fd());
    sm_clear_wake_fd();
    sm_take(B2);
  }).join();
  signal(SIGUSR1, SIG_DFL);
  CHECK(pending);
  CHECK(word);
  CHECK(woke);

  uint32_t fresh = ~0u;
  std::thread([&] {
    sm_interrupt_self(B1);
    fresh = sm_pending();
  }).join();
  CHECK(fresh == 0);
}

TEST_CASE("the wait mask is the thread's own, all bits by default") {
  // Verify the per-thread wait mask.
  // 1. Confirm a new thread's mask is all bits.
  // 2. Set this thread's mask, and confirm it reads back.
  // 3. Confirm another thread still has all bits.
  uint32_t fresh = 0;
  std::thread([&] { fresh = sm_wait_mask(); }).join();
  CHECK(fresh == ~0u);
  sm_set_wait_mask(B1);
  CHECK(sm_wait_mask() == B1);
  std::thread([&] { fresh = sm_wait_mask(); }).join();
  CHECK(fresh == ~0u);
  sm_set_wait_mask(~0u);
}

TEST_CASE("a one-shot timer interrupts at its deadline") {
  // Verify a timer raises its bits when due, once.
  // 1. Arm a timer on this thread for B1, 20 ms from now.
  // 2. Wait on an unfilled box, on B1, with no deadline.
  // 3. Confirm the wait was interrupted no sooner than the deadline.
  // 4. Confirm cancelling it now says it fired.
  box_t box;
  int64_t when = sm_now_us() + 20000;
  sm_timer_id_t id = sm_timer_at(sm_thread_self(), when, B1, 0);
  CHECK(id != 0);
  CHECK(wait_box(&box, B1, SM_FOREVER) == SM_WAIT_INTERRUPTED);
  CHECK(sm_now_us() >= when);
  CHECK_FALSE(sm_timer_cancel(id));
  CHECK(sm_take(B1) == B1);
}

TEST_CASE("a periodic timer interrupts again and again, until cancelled") {
  // Verify a periodic timer is armed again after each firing.
  // 1. Arm a timer on this thread for B1, every 10 ms.
  // 2. Three times: wait for B1, then take it.
  // 3. Cancel it: it is removed; then nothing more arrives.
  box_t box;
  sm_timer_id_t id =
      sm_timer_at(sm_thread_self(), sm_now_us() + 10000, B1, 10000);
  for (int i = 0; i < 3; i++) {
    CHECK(wait_box(&box, B1, sm_now_us() + 2000000) == SM_WAIT_INTERRUPTED);
    CHECK(sm_take(B1) == B1);
  }
  CHECK(sm_timer_cancel(id));
  sm_take(B1);
  sleep_ms(40);
  CHECK(sm_pending() == 0);
}

/* A wait's predicate that throws, as a caller's callback may. */
bool throwing_done(void *) { throw 1; }

TEST_CASE("a wait left by an exception is no longer a waiter") {
  // Verify sm_wait() unregisters its wait when its predicate throws, so
  // a later interrupt finds no waiter to wake.
  // 1. Hold a mutex, and wait on it with a predicate that throws.
  // 2. Keep the mutex held, and interrupt this thread from another.
  // 3. Confirm the interrupt returns at once: it found no waiter, where a
  //    stale one would make it retry the held mutex forever.
  // 4. Let the mutex go, and take the bit.
  std::mutex mu;
  std::condition_variable cv;
  std::unique_lock<std::mutex> lock(mu);
  bool thrown = false;
  try {
    sm_wait(&lock, &cv, throwing_done, nullptr, B1, SM_FOREVER);
  } catch (int) {
    thrown = true;
  }
  REQUIRE(thrown);
  REQUIRE(lock.owns_lock());
  sm_thread_ref_t self = sm_thread_self();
  std::future<void> raised =
      std::async(std::launch::async, [&] { sm_interrupt(self, B1); });
  CHECK(raised.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  lock.unlock();
  raised.wait();
  CHECK(sm_take(B1) == B1);
}

TEST_CASE("a timer due never leaves the timer thread asleep") {
  // Verify a timer at SM_FOREVER costs no CPU while it is armed.
  // 1. Arm a timer on this thread for B1, due at SM_FOREVER.
  // 2. Sleep 200 ms, and read the CPU the process spent meanwhile.
  // 3. Confirm it is far below the sleep: the timer thread slept.
  // 4. Cancel the timer: it was removed, and nothing is pending.
  sm_timer_id_t id = sm_timer_at(sm_thread_self(), SM_FOREVER, B1, 0);
  // The CPU of every thread of the process, in microseconds.
  auto cpu_us = [] {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000LL +
           ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
  };
  int64_t before = cpu_us();
  sleep_ms(200);
  int64_t spent = cpu_us() - before;
  CAPTURE(spent);
  CHECK(spent < 50000);
  CHECK(sm_timer_cancel(id));
  CHECK(sm_pending() == 0);
}

TEST_CASE("the timer thread blocks every signal") {
  // Verify a signal to the process cannot land in sm_sys.h's timer thread.
  // 1. Arm and cancel a timer, so the timer thread runs.
  // 2. Confirm every thread of the process but this one blocks SIGTERM,
  //    though this one does not.
  sigset_t none, old;
  sigemptyset(&none);
  REQUIRE(pthread_sigmask(SIG_SETMASK, &none, &old) == 0);
  sm_timer_cancel(sm_timer_at(sm_thread_self(), SM_FOREVER, B1, 0));
  std::string self = std::to_string(gettid());
  int others = 0, unblocked = 0;
  DIR *dir = opendir("/proc/self/task");
  REQUIRE(dir != nullptr);
  while (struct dirent *e = readdir(dir)) {
    std::string tid = e->d_name;
    if (tid == "." || tid == ".." || tid == self)
      continue;
    std::ifstream status("/proc/self/task/" + tid + "/status");
    std::string line;
    while (std::getline(status, line)) {
      if (line.rfind("SigBlk:", 0) == 0) {
        others++;
        if ((std::stoull(line.substr(7), nullptr, 16) &
             (1ull << (SIGTERM - 1))) == 0)
          unblocked++;
      }
    }
  }
  closedir(dir);
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  CHECK(others >= 1);
  CHECK(unblocked == 0);
}

TEST_CASE("a cancelled timer never fires") {
  // Verify cancelling before the deadline.
  // 1. Arm a timer on this thread for B1, 30 ms from now.
  // 2. Cancel it at once: it was removed.
  // 3. Sleep past its deadline: nothing is pending.
  sm_timer_id_t id = sm_timer_at(sm_thread_self(), sm_now_us() + 30000, B1, 0);
  CHECK(sm_timer_cancel(id));
  sleep_ms(60);
  CHECK(sm_pending() == 0);
  CHECK_FALSE(sm_timer_cancel(id));
}

TEST_CASE("cancel against expiry is decided once") {
  // Verify exactly one of cancel and fire, and that a fired answer
  // means the bit has landed.
  // 1. Keep the timer thread busy: another thread arms timers due
  //    now on itself, over and over.
  // 2. Many times: arm a timer due now here, then cancel it at once.
  // 3. Confirm: removed means B1 is not pending; fired means it is.
  std::atomic<bool> stop{false};
  std::thread noise([&] {
    while (!stop.load()) {
      sm_timer_at(sm_thread_self(), sm_now_us(), B2, 0);
      sm_take(B2);
    }
  });
  int removed = 0, fired = 0;
  for (int i = 0; i < 5000; i++) {
    sm_timer_id_t id = sm_timer_at(sm_thread_self(), sm_now_us(), B1, 0);
    bool was_removed = sm_timer_cancel(id);
    uint32_t bits = sm_take(B1);
    if (was_removed) {
      CHECK(bits == 0);
      removed++;
    } else {
      CHECK(bits == B1);
      fired++;
    }
  }
  stop.store(true);
  noise.join();
  CHECK(removed + fired == 5000);
}

TEST_CASE("a timer on a thread that is still held fires into its block") {
  // Verify a timer reaches a thread that ended while a handle remains.
  // 1. Start a thread that hands out its handle and ends at once.
  // 2. Arm a timer on it, due now, keeping our handle.
  // 3. Wait for it to fire: cancel then says it fired.
  std::promise<sm_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sm_thread_ref_t t = who.get_future().get();
  gone.join();
  sm_timer_id_t id = sm_timer_at(t, sm_now_us(), B1, 0);
  sleep_ms(20);
  CHECK_FALSE(sm_timer_cancel(id));
}

TEST_CASE("the last handle frees a block, and its timers are dropped") {
  // Verify handles own the block, and timers do not.
  // 1. Start a thread that hands out its handle and ends at once.
  // 2. Arm a periodic timer on it, then drop our handle after the
  //    thread has ended: the block is freed though the timer holds it.
  // 3. Sleep past its deadline: the timer was dropped, not fired, so
  //    cancelling it finds nothing.
  std::promise<sm_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sm_thread_ref_t t = who.get_future().get();
  gone.join();
  std::weak_ptr<sm_thread_t> watch = t;
  sm_timer_id_t id = sm_timer_at(t, sm_now_us() + 20000, B1, 10000);
  t.reset();
  CHECK(watch.expired());
  sleep_ms(50);
  CHECK_FALSE(sm_timer_cancel(id));
}
