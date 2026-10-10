/*
 * test_interrupt.cc -- sys.h's interrupts, interruptible
 * waits, the wake fd, and timers (INTERRUPT.md).
 */
#include "doctest.h"

#include "sys.h"

#include <poll.h>
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
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
sys_wait_result_t wait_box(box_t *box, uint32_t mask, int64_t deadline_us) {
  std::unique_lock<std::mutex> lock(box->mu);
  return sys_wait(&lock, &box->cv, box_ready, box, mask, deadline_us);
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
std::thread start_publishing(std::promise<sys_thread_ref_t> *out, F body) {
  return std::thread([out, body] {
    out->set_value(sys_thread_self());
    body();
  });
}

} // namespace

TEST_CASE("the header keeps the values it promises") {
  // Verify the constants the interface documents.
  // 1. Confirm SYS_FOREVER is a deadline that never comes.
  // 2. Confirm the three wait results are distinct.
  CHECK(SYS_FOREVER == INT64_MAX);
  CHECK(SYS_WAIT_DONE != SYS_WAIT_INTERRUPTED);
  CHECK(SYS_WAIT_INTERRUPTED != SYS_WAIT_TIMED_OUT);
  CHECK(SYS_WAIT_DONE != SYS_WAIT_TIMED_OUT);
}

TEST_CASE("bits stay pending until taken, and take claims only its mask") {
  // Verify raising, checking and claiming on the calling thread.
  // 1. Raise B1 and B2 in this thread.
  // 2. Confirm both are pending, and raising B1 again changes nothing.
  // 3. Take B1: it returns B1 and leaves B2 pending.
  // 4. Take B2, leaving nothing.
  sys_thread_ref_t self = sys_thread_self();
  sys_interrupt(self, B1 | B2);
  CHECK(sys_pending() == (B1 | B2));
  sys_interrupt(self, B1);
  CHECK(sys_pending() == (B1 | B2));
  CHECK(sys_take(B1) == B1);
  CHECK(sys_pending() == B2);
  CHECK(sys_take(B1 | B2) == B2);
  CHECK(sys_pending() == 0);
}

TEST_CASE("an interrupt raised before a wait ends it at once") {
  // Verify no interrupt is lost when it comes first.
  // 1. Raise B1 in this thread.
  // 2. Wait for a box nobody fills, on B1, with no deadline.
  // 3. Confirm the wait ends interrupted, and leaves B1 pending.
  box_t box;
  sys_interrupt(sys_thread_self(), B1);
  CHECK(wait_box(&box, B1, SYS_FOREVER) == SYS_WAIT_INTERRUPTED);
  CHECK(sys_take(B1) == B1);
}

TEST_CASE("an interrupt from another thread wakes a wait under way") {
  // Verify a sleeping waiter is woken.
  // 1. Start a thread that waits on an unfilled box, on B1.
  // 2. Let it fall asleep, then raise B1 in it from here.
  // 3. Confirm its wait ended interrupted.
  box_t box;
  std::promise<sys_thread_ref_t> who;
  sys_wait_result_t result = SYS_WAIT_DONE;
  std::thread waiter =
      start_publishing(&who, [&] { result = wait_box(&box, B1, SYS_FOREVER); });
  sys_thread_ref_t t = who.get_future().get();
  sleep_ms(50);
  sys_interrupt(t, B1);
  waiter.join();
  CHECK(result == SYS_WAIT_INTERRUPTED);
}

TEST_CASE("the condition ends a wait, and is checked before the bits") {
  // Verify a wait ends on its own condition, first.
  // 1. Start a thread that waits on a box, on B1.
  // 2. Fill the box under its mutex and notify.
  // 3. Confirm the wait ended done.
  // 4. With the box full and B1 pending, confirm a wait is done.
  box_t box;
  sys_wait_result_t result = SYS_WAIT_INTERRUPTED;
  std::thread waiter(
      [&] { result = wait_box(&box, B1, sys_now_us() + 5000000); });
  sleep_ms(20);
  {
    std::lock_guard<std::mutex> guard(box.mu);
    box.ready = true;
  }
  box.cv.notify_all();
  waiter.join();
  CHECK(result == SYS_WAIT_DONE);

  sys_interrupt(sys_thread_self(), B1);
  CHECK(wait_box(&box, B1, SYS_FOREVER) == SYS_WAIT_DONE);
  CHECK(sys_take(B1) == B1);
}

TEST_CASE("a deadline ends a wait, and leaves no bit") {
  // Verify a bounded wait times out on its own.
  // 1. Wait 30 ms on an unfilled box, on B1.
  // 2. Confirm it timed out after at least 30 ms, with nothing pending.
  box_t box;
  int64_t start = sys_now_us();
  CHECK(wait_box(&box, B1, start + 30000) == SYS_WAIT_TIMED_OUT);
  CHECK(sys_now_us() - start >= 30000);
  CHECK(sys_pending() == 0);
}

TEST_CASE("a bit outside the mask does not end a wait") {
  // Verify the mask is honoured.
  // 1. Start a thread that waits 100 ms on an unfilled box, on B1.
  // 2. Raise B2 in it while it waits.
  // 3. Confirm it timed out rather than being interrupted.
  box_t box;
  std::promise<sys_thread_ref_t> who;
  sys_wait_result_t result = SYS_WAIT_DONE;
  uint32_t left = 0;
  std::thread waiter = start_publishing(&who, [&] {
    result = wait_box(&box, B1, sys_now_us() + 100000);
    left = sys_take(B1 | B2);
  });
  sys_thread_ref_t t = who.get_future().get();
  sleep_ms(20);
  sys_interrupt(t, B2);
  waiter.join();
  CHECK(result == SYS_WAIT_TIMED_OUT);
  CHECK(left == B2);
}

TEST_CASE("an interrupt to a thread that has ended is harmless") {
  // Verify a held block outlives its thread.
  // 1. Start a thread that publishes its block and ends at once.
  // 2. Join it, then raise B1 in its block and release the reference.
  std::promise<sys_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sys_thread_ref_t t = who.get_future().get();
  gone.join();
  sys_interrupt(t, B1);
}

TEST_CASE("the wake fd is readable after an interrupt, until drained") {
  // Verify the eventfd a poll loop waits on.
  // 1. Get this thread's wake fd: not readable yet.
  // 2. Raise B1: it becomes readable.
  // 3. Drain it: not readable, and B1 still pending until taken.
  int fd = sys_wake_fd();
  REQUIRE(fd >= 0);
  CHECK(sys_wake_fd() == fd);
  CHECK_FALSE(readable(fd));
  sys_interrupt(sys_thread_self(), B1);
  CHECK(readable(fd));
  sys_clear_wake_fd();
  CHECK_FALSE(readable(fd));
  CHECK(sys_take(B1) == B1);
}

TEST_CASE("the wait mask is the thread's own, all bits by default") {
  // Verify the per-thread wait mask.
  // 1. Confirm a new thread's mask is all bits.
  // 2. Set this thread's mask, and confirm it reads back.
  // 3. Confirm another thread still has all bits.
  uint32_t fresh = 0;
  std::thread([&] { fresh = sys_wait_mask(); }).join();
  CHECK(fresh == ~0u);
  sys_set_wait_mask(B1);
  CHECK(sys_wait_mask() == B1);
  std::thread([&] { fresh = sys_wait_mask(); }).join();
  CHECK(fresh == ~0u);
  sys_set_wait_mask(~0u);
}

TEST_CASE("a one-shot timer interrupts at its deadline") {
  // Verify a timer raises its bits when due, once.
  // 1. Arm a timer on this thread for B1, 20 ms from now.
  // 2. Wait on an unfilled box, on B1, with no deadline.
  // 3. Confirm the wait was interrupted no sooner than the deadline.
  // 4. Confirm cancelling it now says it fired.
  box_t box;
  int64_t when = sys_now_us() + 20000;
  sys_timer_id_t id = sys_timer_at(sys_thread_self(), when, B1, 0);
  CHECK(id != 0);
  CHECK(wait_box(&box, B1, SYS_FOREVER) == SYS_WAIT_INTERRUPTED);
  CHECK(sys_now_us() >= when);
  CHECK_FALSE(sys_timer_cancel(id));
  CHECK(sys_take(B1) == B1);
}

TEST_CASE("a periodic timer interrupts again and again, until cancelled") {
  // Verify a periodic timer is armed again after each firing.
  // 1. Arm a timer on this thread for B1, every 10 ms.
  // 2. Three times: wait for B1, then take it.
  // 3. Cancel it: it is removed; then nothing more arrives.
  box_t box;
  sys_timer_id_t id =
      sys_timer_at(sys_thread_self(), sys_now_us() + 10000, B1, 10000);
  for (int i = 0; i < 3; i++) {
    CHECK(wait_box(&box, B1, sys_now_us() + 2000000) == SYS_WAIT_INTERRUPTED);
    CHECK(sys_take(B1) == B1);
  }
  CHECK(sys_timer_cancel(id));
  sys_take(B1);
  sleep_ms(40);
  CHECK(sys_pending() == 0);
}

TEST_CASE("a cancelled timer never fires") {
  // Verify cancelling before the deadline.
  // 1. Arm a timer on this thread for B1, 30 ms from now.
  // 2. Cancel it at once: it was removed.
  // 3. Sleep past its deadline: nothing is pending.
  sys_timer_id_t id =
      sys_timer_at(sys_thread_self(), sys_now_us() + 30000, B1, 0);
  CHECK(sys_timer_cancel(id));
  sleep_ms(60);
  CHECK(sys_pending() == 0);
  CHECK_FALSE(sys_timer_cancel(id));
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
      sys_timer_at(sys_thread_self(), sys_now_us(), B2, 0);
      sys_take(B2);
    }
  });
  int removed = 0, fired = 0;
  for (int i = 0; i < 5000; i++) {
    sys_timer_id_t id = sys_timer_at(sys_thread_self(), sys_now_us(), B1, 0);
    bool was_removed = sys_timer_cancel(id);
    uint32_t bits = sys_take(B1);
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
  std::promise<sys_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sys_thread_ref_t t = who.get_future().get();
  gone.join();
  sys_timer_id_t id = sys_timer_at(t, sys_now_us(), B1, 0);
  sleep_ms(20);
  CHECK_FALSE(sys_timer_cancel(id));
}

TEST_CASE("the last handle frees a block, and its timers are dropped") {
  // Verify handles own the block, and timers do not.
  // 1. Start a thread that hands out its handle and ends at once.
  // 2. Arm a periodic timer on it, then drop our handle after the
  //    thread has ended: the block is freed though the timer holds it.
  // 3. Sleep past its deadline: the timer was dropped, not fired, so
  //    cancelling it finds nothing.
  std::promise<sys_thread_ref_t> who;
  std::thread gone = start_publishing(&who, [] {});
  sys_thread_ref_t t = who.get_future().get();
  gone.join();
  std::weak_ptr<sys_thread_t> watch = t;
  sys_timer_id_t id = sys_timer_at(t, sys_now_us() + 20000, B1, 10000);
  t.reset();
  CHECK(watch.expired());
  sleep_ms(50);
  CHECK_FALSE(sys_timer_cancel(id));
}
